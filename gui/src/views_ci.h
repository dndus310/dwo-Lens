#pragma once
// Local CI view: the private Jenkins on this PC (installed from an offline bundle) and this project's job.
// Status and builds come from Jenkins' REST API on 127.0.0.1 (polled on a worker thread, HTTP/1.0 so no
// chunked bodies); every action runs through forge.ps1 -> ci.ps1 in the runner like the other tools.

#include "views.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace ci
{
struct Build
{
	int number = 0;
	std::string result;   // SUCCESS, UNSTABLE, FAILURE, ABORTED or "" while building
	bool building = false;
	double timestamp = 0; // ms since epoch
	double duration = 0;  // ms
	std::string description;
};

struct Snapshot
{
	bool installed = false;
	bool running = false;
	int port = 8098;
	std::string version, java, user, password;
	std::string job;          // this project's job (empty when not registered)
	bool jobExists = false;
	bool inQueue = false;
	std::vector<Build> builds;
	std::vector<std::string> hooks;
	int keepBuilds = 0;
	std::string console;      // tail of the selected build's log
	int consoleFor = 0;
	double polledAt = 0;
};

static std::string Base64(const std::string& in)
{
	static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	int val = 0, bits = -6;
	for (unsigned char c : in)
	{
		val = (val << 8) + c;
		bits += 8;
		while (bits >= 0) { out.push_back(t[(val >> bits) & 0x3F]); bits -= 6; }
	}
	if (bits > -6) out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
	while (out.size() % 4) out.push_back('=');
	return out;
}

// GET on 127.0.0.1. Returns the HTTP status (0 = not reachable) and the body.
static int HttpGet(int port, const std::string& path, const std::string& auth, std::string& body, size_t maxBytes = 4u << 20)
{
	body.clear();
	static bool wsa = false;
	if (!wsa) { WSADATA d; if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return 0; wsa = true; }
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) return 0;
	DWORD timeout = 4000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) { closesocket(s); return 0; }
	std::string req = "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\nAuthorization: Basic " + auth + "\r\nConnection: close\r\n\r\n";
	send(s, req.data(), (int)req.size(), 0);
	std::string resp;
	char buf[16384];
	for (;;)
	{
		int n = recv(s, buf, sizeof(buf), 0);
		if (n <= 0) break;
		resp.append(buf, n);
		if (resp.size() > maxBytes + 65536) resp.erase(4096, resp.size() - maxBytes);   // keep headers and the tail
	}
	closesocket(s);
	size_t hdr = resp.find("\r\n\r\n");
	if (resp.compare(0, 5, "HTTP/") != 0 || hdr == std::string::npos) return 0;
	int status = atoi(resp.c_str() + resp.find(' ') + 1);
	body = resp.substr(hdr + 4);
	return status;
}

static std::string UrlEncode(const std::string& s)
{
	std::string out;
	for (unsigned char c : s)
	{
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back((char)c);
		else out += Fmt("%%%02X", c);
	}
	return out;
}

class Client
{
public:
	~Client() { Stop(); }

	void Ensure(const fs::path& project)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (project != projectDir) { projectDir = project; snap = Snapshot(); }
		}
		if (!worker.joinable())
		{
			stop = false;
			worker = std::thread([this] { Loop(); });
		}
		wake = true;
	}

	void Stop()
	{
		stop = true;
		if (worker.joinable()) worker.join();
	}

	Snapshot Get()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return snap;
	}

	void Select(int number) { selected = number; wake = true; }
	void Poke() { wake = true; }

private:
	void Loop()
	{
		while (!stop)
		{
			Poll();
			for (int i = 0; i < 30 && !stop && !wake; ++i) Sleep(100);
			wake = false;
		}
	}

	void Poll()
	{
		fs::path project;
		{
			std::lock_guard<std::mutex> lock(mutex);
			project = projectDir;
		}
		Snapshot n;
		const wchar_t* ciHome = _wgetenv(L"DWO_FORGE_CI_HOME");
		fs::path ciDir = (ciHome && *ciHome) ? fs::path(ciHome)
			: fs::path(_wgetenv(L"LOCALAPPDATA") ? _wgetenv(L"LOCALAPPDATA") : L".") / "dwo-Forge" / "ci";
		std::error_code ec;
		n.installed = fs::exists(ciDir / "runtime" / "jenkins.war", ec);
		std::string auth;
		if (auto s = ReadJson(ciDir / "secrets.json"))
		{
			n.user = Str(*s, "user");
			n.password = Str(*s, "password");
			n.port = (int)Real(*s, "port", 8098);
			auth = Base64(n.user + ":" + Str(*s, "token"));
		}
		if (auto c = ReadJson(ciDir / "ci.json"))
		{
			n.version = Str(*c, "jenkinsVersion");
			n.java = Str(*c, "java");
		}
		if (!project.empty())
			if (auto p = ReadJson(project / "__dwo-forge" / "ci.json"))
			{
				n.job = Str(*p, "job");
				for (auto& h : Arr(*p, "hooks")) if (h.is_string()) n.hooks.push_back(h.get<std::string>());
			}
		std::string body;
		if (n.installed && !auth.empty() && HttpGet(n.port, "/api/json?tree=mode", auth, body) == 200)
		{
			n.running = true;
			if (!n.job.empty())
			{
				std::string path = "/job/" + UrlEncode(n.job) + "/api/json?tree=inQueue,property[strategy[numToKeep]],"
				                   "builds[number,result,building,timestamp,duration,description]{0,50}";
				if (HttpGet(n.port, path, auth, body) == 200)
				{
					n.jobExists = true;
					json j = json::parse(body, nullptr, false);
					if (j.is_object())
					{
						n.inQueue = j.value("inQueue", false);
						for (auto& p : Arr(j, "property"))
							if (p.contains("strategy") && p["strategy"].is_object()) n.keepBuilds = (int)Num(p["strategy"], "numToKeep");
						for (auto& b : Arr(j, "builds"))
						{
							Build x;
							x.number = (int)Num(b, "number");
							x.result = b.contains("result") && b["result"].is_string() ? b["result"].get<std::string>() : "";
							x.building = b.value("building", false);
							x.timestamp = Real(b, "timestamp");
							x.duration = Real(b, "duration");
							x.description = b.contains("description") && b["description"].is_string() ? b["description"].get<std::string>() : "";
							n.builds.push_back(std::move(x));
						}
					}
				}
				int sel = selected;
				if (!sel && !n.builds.empty()) sel = n.builds.front().number;
				if (sel)
				{
					if (HttpGet(n.port, "/job/" + UrlEncode(n.job) + "/" + std::to_string(sel) + "/consoleText", auth, body, 512u << 10) == 200)
					{
						if (body.size() > 200000) body = "...\n" + body.substr(body.size() - 200000);
						n.console = std::move(body);
					}
					n.consoleFor = sel;
				}
			}
		}
		std::lock_guard<std::mutex> lock(mutex);
		if (project == projectDir) snap = std::move(n);
	}

	std::mutex mutex;
	fs::path projectDir;
	Snapshot snap;
	std::thread worker;
	std::atomic<bool> stop{ false };
	std::atomic<bool> wake{ false };
	std::atomic<int> selected{ 0 };
};

static Client& Instance()
{
	static Client c;
	return c;
}

static std::string Verdict(const Build& b)
{
	if (b.building) return "running";
	if (b.result == "SUCCESS") return "pass";
	if (b.result == "UNSTABLE") return "warn";
	if (b.result == "FAILURE") return "fail";
	return "aborted";
}

static std::string When(double ms)
{
	if (ms <= 0) return "-";
	time_t t = (time_t)(ms / 1000);
	tm local{};
	localtime_s(&local, &t);
	char buf[32];
	strftime(buf, sizeof(buf), "%m-%d %H:%M", &local);
	return buf;
}

struct Ui
{
	int selected = 0;
	bool stageCompile = true, stageLens = true, stagePackage = false;
	char reason[128] = "dwo-Forge 창에서 실행";
};
static Ui& State()
{
	static Ui u;
	return u;
}
} // namespace ci

static void DrawCiNav(App& app)
{
	ci::Client& c = ci::Instance();
	c.Ensure(app.hasUProject ? app.project : fs::path());
	ci::Snapshot s = c.Get();
	SectionHeader("로컬 CI (Jenkins)");
	if (!s.installed) Pill("설치 안 됨", col::Muted);
	else if (s.running) Pill(Fmt("실행 중 · :%d", s.port), col::Pass, true);
	else Pill("중지됨", col::Warn);
	if (!s.version.empty()) { ImGui::SameLine(); TextDim("Jenkins " + s.version); }
	ImGui::Spacing();
	bool idle = !app.runner.running;
	ImGui::BeginDisabled(!app.hasUProject);
	float w = ImGui::GetContentRegionAvail().x;
	if (!s.installed)
	{
		if (PrimaryButton((std::string(ICON_DOWNLOAD) + "  설치 (오프라인 번들)").c_str(), ImVec2(w, 0)))
		{
			fs::path p;
			if (PickOpen(g_hwnd, false, L"dwo-Forge CI 번들 선택", p, L"dwo-Forge CI 번들 (dwo-forge-ci-bundle-*.zip)", L"dwo-forge-ci-bundle-*.zip;*.zip"))
				app.Run("ci-install --bundle \"" + PathUtf8(p) + "\"");
		}
		Tooltip("인터넷이 되는 PC에서 dwo-Forge.bat ci-bundle로 만든 zip을 고릅니다. 설치할 때 네트워크에 접속하지 않습니다.");
	}
	else
	{
		if (s.running)
		{
			if (ImGui::Button((std::string(ICON_STOP) + "  중지").c_str(), ImVec2(w * 0.5f - S(4), 0))) app.Run("ci-stop");
			ImGui::SameLine();
			if (ImGui::Button((std::string(ICON_OPEN) + "  Jenkins 열기").c_str(), ImVec2(-FLT_MIN, 0)))
				ShellExecuteW(nullptr, L"open", ToWide(Fmt("http://127.0.0.1:%d/", s.port)).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			Tooltip("Jenkins 화면은 선택 사항입니다. 계정은 아래 '계정 복사'로 복사합니다.");
		}
		else if (PrimaryButton((std::string(ICON_PLAY) + "  시작").c_str(), ImVec2(w, 0))) app.Run("ci-start");
	}
	ImGui::EndDisabled();
	if (!app.hasUProject) TextDim("프로젝트를 열면 로컬 CI를 설치하고 쓸 수 있습니다.");

	SectionHeader("이 프로젝트");
	if (s.job.empty())
	{
		TextDim("CI 작업이 없습니다.");
		ImGui::BeginDisabled(!s.installed || !app.attached || !idle);
		if (PrimaryButton((std::string(ICON_ADD) + "  CI 작업 등록").c_str(), ImVec2(w, 0))) app.Run("ci-job");
		ImGui::EndDisabled();
		Tooltip("Jenkins에 이 프로젝트의 파이프라인(컴파일 → Lens 검사 → 패키지)을 만들고, git 훅으로 push·merge·브랜치 생성·커밋 때 자동 빌드합니다.");
	}
	else
	{
		KeyValue("작업", s.job, 60);
		KeyValue("보관", s.keepBuilds ? Fmt("최근 %d개 (넘으면 오래된 것부터 삭제)", s.keepBuilds) : std::string("-"), 60);
		KeyValue("git 훅", s.hooks.empty() ? std::string("없음 (git 저장소 아님)") : std::to_string(s.hooks.size()) + "개", 60);
		ImGui::BeginDisabled(!idle);
		if (ImGui::Button((std::string(ICON_SYNC) + "  설정 다시 적용").c_str(), ImVec2(w, 0))) app.Run("ci-job");
		Tooltip("__dwo-forge/config.json의 \"ci\" 설정(트리거, 보관 개수, Lens 검사 목록)을 작업과 훅에 다시 적용합니다.");
		if (ImGui::Button((std::string(ICON_DELETE) + "  CI 작업 해제").c_str(), ImVec2(w, 0))) app.Run("ci-job --remove");
		Tooltip("Jenkins 작업과 git 훅을 지웁니다. Jenkins 자체는 그대로 둡니다.");
		ImGui::EndDisabled();
	}

	if (s.installed)
	{
		SectionHeader("관리");
		if (!s.user.empty() && ImGui::Button((std::string(ICON_COPY) + "  계정 복사").c_str(), ImVec2(w, 0)))
		{
			ImGui::SetClipboardText((s.user + " / " + s.password).c_str());
			PushToast("복사함", "Jenkins 계정과 비밀번호를 클립보드에 복사했습니다.", col::Info);
		}
		ImGui::BeginDisabled(!idle || !app.hasUProject);
		if (ImGui::Button((std::string(ICON_DOWNLOAD) + "  번들로 업데이트").c_str(), ImVec2(w, 0)))
		{
			fs::path p;
			if (PickOpen(g_hwnd, false, L"dwo-Forge CI 번들 선택", p, L"dwo-Forge CI 번들 (dwo-forge-ci-bundle-*.zip)", L"dwo-forge-ci-bundle-*.zip;*.zip"))
				app.Run("ci-install --bundle \"" + PathUtf8(p) + "\"");
		}
		if (DangerButton((std::string(ICON_DELETE) + "  로컬 CI 제거").c_str(), ImVec2(w, 0))) app.Run("ci-remove --keep-data");
		Tooltip("Jenkins, 모든 프로젝트의 CI 작업과 git 훅을 지웁니다. 빌드 기록은 %LOCALAPPDATA%\\dwo-Forge\\uninstalled로 옮겨 둡니다.");
		ImGui::EndDisabled();
	}
}

static void DrawCiMain(App& app)
{
	ci::Client& c = ci::Instance();
	c.Ensure(app.hasUProject ? app.project : fs::path());
	ci::Snapshot s = c.Get();
	ci::Ui& ui = ci::State();
	ViewHeader(ICON_SYNC, "로컬 CI", "내 PC의 Jenkins에서 컴파일 · Lens 검사 · 패키지 빌드를 자동으로 실행");
	if (!s.installed)
	{
		EmptyState(ICON_DOWNLOAD, "로컬 CI가 설치되지 않았습니다",
		           "인터넷이 되는 PC에서 'dwo-Forge.bat ci-bundle'로 오프라인 번들(zip)을 만든 뒤, 왼쪽 '설치 (오프라인 번들)'로 설치합니다. "
		           "설치와 실행 모두 네트워크 없이 동작하므로 폐쇄망에서도 쓸 수 있습니다. Jenkins는 127.0.0.1에서만 열립니다.");
		return;
	}
	if (!s.running)
	{
		if (EmptyState(ICON_POWER, "Jenkins가 중지되어 있습니다", "시작하면 git 훅과 외부 요청으로 들어온 빌드를 처리합니다. 훅이 빌드를 요청하면 자동으로 시작됩니다.", "시작"))
			app.Run("ci-start");
		return;
	}
	if (s.job.empty())
	{
		if (EmptyState(ICON_ADD, "이 프로젝트의 CI 작업이 없습니다", "등록하면 Jenkins에 파이프라인 작업이 생기고 git 훅이 설치됩니다.", "CI 작업 등록"))
			app.Run("ci-job");
		return;
	}

	// trigger bar
	ImGui::Checkbox("C++ 컴파일", &ui.stageCompile);
	ImGui::SameLine();
	ImGui::Checkbox("Lens 검사", &ui.stageLens);
	ImGui::SameLine();
	ImGui::Checkbox("패키지 빌드", &ui.stagePackage);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(S(220));
	ImGui::InputText("##reason", ui.reason, sizeof(ui.reason));
	ImGui::SameLine();
	bool any = ui.stageCompile || ui.stageLens || ui.stagePackage;
	ImGui::BeginDisabled(!any || s.inQueue);
	if (PrimaryButton((std::string(ICON_PLAY) + "  지금 빌드").c_str()))
	{
		std::string stages;
		for (auto [on, name] : { std::pair{ ui.stageCompile, "compile" }, std::pair{ ui.stageLens, "lens" }, std::pair{ ui.stagePackage, "package" } })
			if (on) stages += (stages.empty() ? "" : ",") + std::string(name);
		std::string reason = ui.reason;
		for (char& ch : reason) if (ch == '"') ch = '\'';
		app.Run("ci-trigger --event manual --stages " + stages + " --reason \"" + reason + "\"");
		c.Poke();
	}
	ImGui::EndDisabled();
	if (s.inQueue) { ImGui::SameLine(); Pill("대기열에 있음", col::Info); }

	ImGui::Spacing();
	float listH = ImGui::GetContentRegionAvail().y * 0.45f;
	if (ImGui::BeginTable("ci_builds", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, listH)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("결과", ImGuiTableColumnFlags_WidthFixed, S(70));
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, S(50));
		ImGui::TableSetupColumn("이유", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("시작", ImGuiTableColumnFlags_WidthFixed, S(90));
		ImGui::TableSetupColumn("소요", ImGuiTableColumnFlags_WidthFixed, S(80));
		ImGui::TableHeadersRow();
		for (auto& b : s.builds)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			std::string v = ci::Verdict(b);
			const char* label = VerdictLabel(v);
			Pill(label ? label : "중단", SemanticColor(v));
			ImGui::TableNextColumn();
			bool sel = (ui.selected ? ui.selected : (s.builds.empty() ? 0 : s.builds.front().number)) == b.number;
			if (ImGui::Selectable(Fmt("#%d##b%d", b.number, b.number).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
			{
				ui.selected = b.number;
				c.Select(b.number);
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(b.description.empty() ? "-" : b.description.c_str());
			ImGui::TableNextColumn();
			TextDim(ci::When(b.timestamp));
			ImGui::TableNextColumn();
			TextDim(b.building ? std::string("진행 중") : Duration(b.duration / 1000.0));
		}
		if (s.builds.empty())
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			TextDim("아직 빌드가 없습니다. '지금 빌드'를 누르거나 develop에 push하면 시작됩니다.");
		}
		ImGui::EndTable();
	}
	SectionHeader(s.consoleFor ? Fmt("빌드 #%d 로그", s.consoleFor).c_str() : "빌드 로그");
	ImGui::BeginChild("ci_console", ImVec2(0, 0), ImGuiChildFlags_Borders);
	ImGui::TextUnformatted(s.console.empty() ? "빌드를 고르면 로그가 여기에 표시됩니다." : s.console.c_str());
	if (!s.builds.empty() && s.builds.front().building && s.consoleFor == s.builds.front().number) ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
}

static void DrawCiInspector(App& app)
{
	ci::Snapshot s = ci::Instance().Get();
	SectionHeader("동작 방식");
	ImGui::TextWrapped("내 PC 전용 Jenkins입니다. 127.0.0.1에서만 열리고 계정으로 보호됩니다.");
	ImGui::Spacing();
	ImGui::TextWrapped("자동 빌드: develop으로 push, develop에 merge, feature/* 브랜치 생성, develop·feature/* 커밋. 브랜치와 단계는 __dwo-forge/config.json의 \"ci\"에서 바꿉니다.");
	ImGui::Spacing();
	ImGui::TextWrapped("외부 요청: __dwo-forge.bat ci-trigger --reason <내용>, 또는 HTTP POST /job/<작업>/buildWithParameters (계정 토큰).");
	ImGui::Spacing();
	ImGui::TextWrapped("보관: Jenkins는 최근 빌드만 남기고 오래된 것부터 지웁니다. 패키지 결과물은 Lens 실행 기록에 최근 3개만 남깁니다.");
	if (s.installed)
	{
		SectionHeader("설치");
		KeyValue("Jenkins", s.version.empty() ? "-" : s.version, 70);
		KeyValue("Java", s.java.empty() ? "-" : s.java, 70);
		KeyValue("주소", Fmt("http://127.0.0.1:%d/", s.port), 70);
		KeyValue("계정", s.user, 70);
	}
	(void)app;
}
