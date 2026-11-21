#pragma once
// DwoBuilder view: the private Jenkins on this PC (installed from an offline bundle) and this project's job.
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
	int tab = -1;              // 0 automatic builds (Jenkins), 1 local builds; -1 = pick from the project's state
	std::string localRun;      // selected compile/package run (empty = the newest)
	bool stageCompile = true, stageLens = true, stagePackage = false;
	char reason[128] = "dwo-Forge 창에서 실행";
};
static Ui& State()
{
	static Ui u;
	return u;
}
} // namespace ci

// Set to a launcher page (1 = Jenkins) to open it from a tool screen; the launcher consumes it.
static int g_launcherPageRequest = -1;

static void DrawCiNav(App& app)
{
	ci::Client& c = ci::Instance();
	c.Ensure(app.hasUProject ? app.project : fs::path());
	ci::Snapshot s = c.Get();
	float w = ImGui::GetContentRegionAvail().x;
	bool idle = !app.runner.running;
	SectionHeader("Jenkins (PC)");
	if (!s.installed) Pill("설치 안 됨", col::Muted);
	else if (s.running) Pill(Fmt("실행 중 · :%d", s.port), col::Pass, true);
	else Pill("중지됨", col::Warn);
	if (!s.version.empty()) { ImGui::SameLine(); TextDim("Jenkins " + s.version); }
	ImGui::Spacing();
	ImGui::BeginDisabled(!idle);
	if (s.installed && !s.running && PrimaryButton((std::string(ICON_PLAY) + "  시작").c_str(), ImVec2(w, 0))) app.Run("ci-start");
	ImGui::EndDisabled();
	if (s.installed && s.running && ImGui::Button((std::string(ICON_OPEN) + "  Jenkins 화면 열기").c_str(), ImVec2(w, 0)))
		ShellExecuteW(nullptr, L"open", ToWide(Fmt("http://127.0.0.1:%d/", s.port)).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	if (ImGui::Button((std::string(ICON_SETTINGS) + "  Jenkins 관리 (런처)").c_str(), ImVec2(w, 0)))
	{
		g_launcherPageRequest = 1;
		app.requestActivity = Activity::Launcher;
	}
	Tooltip("설치·업데이트·제거, 계정, 이 Jenkins를 쓰는 프로젝트 목록은 런처의 PC 구성요소에 있습니다.");

	SectionHeader("로컬 빌드");
	for (int line = kLensLineCount; line < kLineCount; ++line)
	{
		ImGui::BeginDisabled(!app.attached);
		if (ImGui::Button((std::string(kLines[line].icon) + "  " + kLines[line].name).c_str(), ImVec2(w, 0)))
		{
			app.Run(kLines[line].command);
			ci::State().tab = 1;
		}
		ImGui::EndDisabled();
		Tooltip(kLines[line].desc);
	}

	SectionHeader("자동 빌드");
	if (s.job.empty())
	{
		TextDim("DwoBuilder가 설치되지 않았습니다.");
		ImGui::BeginDisabled(!app.attached || !idle);
		if (PrimaryButton((std::string(ICON_ADD) + "  DwoBuilder 설치").c_str(), ImVec2(w, 0))) app.Run("tool install ci");
		ImGui::EndDisabled();
		Tooltip("Jenkins에 이 프로젝트의 파이프라인(컴파일 → DwoLens 검사 → 패키지)을 만들고, git 훅으로 push·merge·브랜치 생성·커밋 때 자동 빌드합니다. PC에 Jenkins가 없으면 오프라인 번들로 먼저 설치합니다.");
	}
	else
	{
		KeyValue("작업", s.job, 60);
		KeyValue("보관", s.keepBuilds ? Fmt("최근 %d개 (넘으면 오래된 것부터 삭제)", s.keepBuilds) : std::string("-"), 60);
		KeyValue("git 훅", s.hooks.empty() ? std::string("없음 (git 저장소 아님)") : std::to_string(s.hooks.size()) + "개", 60);
		ImGui::BeginDisabled(!idle);
		if (ImGui::Button((std::string(ICON_SYNC) + "  설정 다시 적용").c_str(), ImVec2(w, 0))) app.Run("tool update ci");
		Tooltip("__dwo-forge/config.json의 \"ci\" 설정(트리거, 보관 개수, DwoLens 검사 목록)과 이 dwo-Forge의 파이프라인을 작업과 훅에 다시 적용합니다.");
		ImGui::EndDisabled();
		if (!s.user.empty() && ImGui::Button((std::string(ICON_COPY) + "  계정 복사").c_str(), ImVec2(w, 0)))
		{
			ImGui::SetClipboardText((s.user + " / " + s.password).c_str());
			PushToast("복사함", "Jenkins 계정과 비밀번호를 클립보드에 복사했습니다.", col::Info);
		}
	}
}

// Local builds: compile and package run straight on this PC (no Jenkins), the same steps the automatic build runs.
static const RunInfo* FindRun(App& app, const std::string& id)
{
	for (auto& r : app.runs)
		if (r.id == id) return &r;
	return nullptr;
}

static void DrawLocalBuilds(App& app)
{
	ci::Ui& u = ci::State();
	ImGui::PushTextWrapPos(0);
	TextDim("Jenkins 없이 이 PC에서 바로 실행합니다. 자동 빌드의 컴파일·패키지 단계와 같은 명령(__dwo-forge.bat compile / package)입니다. 에디터를 닫고 실행하세요.");
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	float avail = ImGui::GetContentRegionAvail().x;
	float w = (avail - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	for (int line = kLensLineCount; line < kLineCount; ++line)
	{
		if (line > kLensLineCount) ImGui::SameLine();
		ImGui::PushID(line);
		ImGui::BeginChild("card", ImVec2(w, S(112)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
		TextBold(std::string(kLines[line].icon) + "  " + kLines[line].name);
		ImGui::SameLine();
		TextDim(kLines[line].command);
		ImGui::PushTextWrapPos(0);
		TextDim(kLines[line].desc);
		ImGui::PopTextWrapPos();
		ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - S(10));
		RunLineButton(app, line, true, (std::string(ICON_PLAY) + "  실행").c_str());
		if (const RunInfo* r = app.Latest(line))
		{
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			VerdictPill(r->verdict);
			ImGui::SameLine();
			TextDim(StampOf(r->id) + " · " + Duration(r->seconds));
		}
		ImGui::EndChild();
		ImGui::PopID();
	}

	SectionHeader("최근 실행");
	std::vector<const RunInfo*> runs;
	for (auto& r : app.runs)
		if (IsBuildLine(r.lineIndex)) runs.push_back(&r);
	if (runs.empty())
	{
		TextDim("아직 실행한 적이 없습니다. 위의 실행 버튼을 누르거나 자동 빌드를 기다리세요.");
		return;
	}
	const RunInfo* sel = u.localRun.empty() ? nullptr : FindRun(app, u.localRun);
	if (!sel) sel = runs.front();
	float listH = ImMin(S(200), (runs.size() + 1.5f) * ImGui::GetFrameHeightWithSpacing());
	if (ImGui::BeginTable("local_runs", 5, kTable, ImVec2(0, listH)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("결과", ImGuiTableColumnFlags_WidthFixed, S(64));
		ImGui::TableSetupColumn("빌드", ImGuiTableColumnFlags_WidthFixed, S(110));
		ImGui::TableSetupColumn("실행 ID", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("소요", ImGuiTableColumnFlags_WidthFixed, S(80));
		ImGui::TableSetupColumn("오류", ImGuiTableColumnFlags_WidthFixed, S(60));
		ImGui::TableHeadersRow();
		for (const RunInfo* r : runs)
		{
			const json& res = Obj(app.Outputs(*r), kLines[r->lineIndex].line);
			int errors = res.contains("errorCount") ? (int)Num(res, "errorCount") : (int)Arr(res, "errors").size();
			ImGui::PushID(r->id.c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			VerdictPill(r->verdict);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(kLines[r->lineIndex].name);
			ImGui::TableNextColumn();
			if (ImGui::Selectable(r->id.c_str(), r == sel, ImGuiSelectableFlags_SpanAllColumns)) u.localRun = r->id;
			ImGui::TableNextColumn();
			TextDim(Duration(r->seconds));
			ImGui::TableNextColumn();
			if (errors) TextColored(col::Fail, std::to_string(errors).c_str()); else TextDim("0");
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	// the selected run: errors, then what is left of the output when the build stopped without parsable errors
	const json& res = Obj(app.Outputs(*sel), kLines[sel->lineIndex].line);
	SectionHeader((std::string(kLines[sel->lineIndex].name) + " · " + sel->id).c_str());
	if (ImGui::SmallButton("리포트 열기")) OpenWithShell(sel->dir / "out" / "report.html");
	ImGui::SameLine();
	if (ImGui::SmallButton("전체 로그")) OpenWithShell(sel->dir / "raw" / (kLines[sel->lineIndex].command + std::string(".log")));
	std::string archive = Str(res, "archive");
	if (!archive.empty())
	{
		ImGui::SameLine();
		if (ImGui::SmallButton("결과물 폴더")) OpenWithShell(fs::u8path(archive));
	}
	if (!Str(res, "skipped").empty()) { TextDim(Str(res, "skipped")); return; }
	if (!Str(res, "hint").empty()) TextColored(col::Warn, Str(res, "hint").c_str());
	const json& errors = Arr(res, "errors");
	if (!errors.empty() && ImGui::BeginTable("local_errors", 3, kTable, ImVec2(0, 0)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("코드", ImGuiTableColumnFlags_WidthFixed, S(70));
		ImGui::TableSetupColumn("위치", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("내용", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableHeadersRow();
		for (auto& e : errors)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			TextDim(Str(e, "code"));
			ImGui::TableNextColumn();
			std::string file = Str(e, "file");
			long long ln = (long long)Num(e, "line");
			std::string where = file.empty() ? "-" : PathUtf8(fs::u8path(file).filename()) + (ln ? ":" + std::to_string(ln) : "");
			if (!file.empty() && ImGui::Selectable((where + "##" + std::to_string((size_t)&e)).c_str())) OpenWithShell(fs::u8path(file));
			else if (file.empty()) TextDim(where);
			if (!file.empty()) Tooltip(file.c_str());
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", Str(e, "message").c_str());
		}
		ImGui::EndTable();
	}
	else if (errors.empty())
	{
		const json& tail = Arr(res, "tail");
		if (sel->verdict == "fail" && !tail.empty())
		{
			std::string text;
			for (auto& t : tail) if (t.is_string()) text += t.get<std::string>() + "\n";
			ImGui::BeginChild("tail", ImVec2(0, 0), ImGuiChildFlags_Borders);
			ImGui::TextUnformatted(text.c_str());
			ImGui::EndChild();
		}
		else TextDim("오류 없음");
	}
}

static void DrawCiMain(App& app)
{
	ci::Client& c = ci::Instance();
	c.Ensure(app.hasUProject ? app.project : fs::path());
	ci::Snapshot s = c.Get();
	ci::Ui& ui = ci::State();
	ViewHeader(ICON_SYNC, "DwoBuilder", "컴파일 · DwoLens 검사 · 패키지 빌드. 자동 빌드는 내 PC의 Jenkins에서, 로컬 빌드는 바로 실행");
	if (ui.tab < 0) ui.tab = s.job.empty() ? 1 : 0;
	static const char* const tabs[] = { "자동 빌드 (Jenkins)", "로컬 빌드" };
	Segmented("##buildertab", tabs, 2, ui.tab);
	ImGui::Spacing();
	if (ui.tab == 1) { DrawLocalBuilds(app); return; }
	if (!s.installed)
	{
		EmptyState(ICON_DOWNLOAD, "자동 빌드를 쓰려면 Jenkins가 필요합니다",
		           "런처에서 이 프로젝트에 DwoBuilder를 설치하면 PC에 Jenkins가 함께 설치됩니다(오프라인 번들, 네트워크 불필요). "
		           "Jenkins 없이 컴파일·패키지만 하려면 위의 '로컬 빌드'를 쓰세요.");
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
		if (EmptyState(ICON_ADD, "이 프로젝트에 DwoBuilder가 설치되지 않았습니다", "설치하면 Jenkins에 이 프로젝트의 파이프라인 작업이 생기고 git 훅이 설치됩니다.", "DwoBuilder 설치"))
			app.Run("tool install builder");
		return;
	}

	// trigger bar
	ImGui::Checkbox("C++ 컴파일", &ui.stageCompile);
	ImGui::SameLine();
	ImGui::Checkbox("DwoLens 검사", &ui.stageLens);
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
	ImGui::TextWrapped("보관: Jenkins는 최근 빌드만 남기고 오래된 것부터 지웁니다. 로컬 빌드와 자동 빌드의 패키지 결과물은 프로젝트 실행 기록에 최근 3개만 남깁니다.");
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
