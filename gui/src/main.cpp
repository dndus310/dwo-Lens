// dwo-Assay control panel: Dear ImGui + Win32 + DirectX 11.
//
// Runs the same commands as __dwo-assay.bat and reads the same run.json / out/*.json files,
// so the GUI and the command line always show identical results.
//
//   dwo-assay-gui.exe [--project <folder with .uproject>] [--home <dwo-Assay repository>]
//                     [--tab log|history|result] [--run <command>]   (--run starts a command on launch)

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "nlohmann/json.hpp"

#include <d3d11.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tchar.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================ text helpers

static std::string ToUtf8(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

static std::wstring ToWide(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

// Child output mixes UTF-8 (Python) and the console code page (cmd, PowerShell).
static std::string ConsoleToUtf8(const std::string& raw)
{
	if (raw.empty()) return raw;
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0) > 0)
		return raw;
	UINT cp = GetOEMCP();
	int n = MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), w.data(), n);
	return ToUtf8(w);
}

static std::string PathUtf8(const fs::path& p) { return p.u8string(); }

static std::optional<json> ReadJson(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) return std::nullopt;
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
		text.erase(0, 3);
	try { return json::parse(text); }
	catch (...) { return std::nullopt; }
}

static std::string Str(const json& j, const char* key, const std::string& fallback = "")
{
	if (!j.is_object()) return fallback;
	auto it = j.find(key);
	if (it == j.end() || it->is_null()) return fallback;
	if (it->is_string()) return it->get<std::string>();
	return it->dump();
}

static long long Num(const json& j, const char* key)
{
	if (!j.is_object()) return 0;
	auto it = j.find(key);
	return (it != j.end() && it->is_number()) ? it->get<long long>() : 0;
}

static bool ContainsNoCase(const std::string& hay, const char* needle)
{
	if (!needle || !*needle) return true;
	std::string h = hay, n = needle;
	std::transform(h.begin(), h.end(), h.begin(), ::tolower);
	std::transform(n.begin(), n.end(), n.begin(), ::tolower);
	return h.find(n) != std::string::npos;
}

static void OpenWithShell(const fs::path& path)
{
	ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ============================================================================ process runner

// Runs one command at a time inside a job object so cancelling also stops UnrealEditor-Cmd children.
class Runner
{
public:
	~Runner() { Cancel(); Join(); }

	bool Start(const std::wstring& commandLine, const fs::path& workDir, const std::string& label)
	{
		if (running) return false;
		Join();
		{
			std::lock_guard<std::mutex> lock(mutex);
			AppendLocked("");
			AppendLocked("> " + label);
		}
		SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
		HANDLE readPipe = nullptr, writePipe = nullptr;
		if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
		SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW si{ sizeof(si) };
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdOutput = writePipe;
		si.hStdError = writePipe;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

		PROCESS_INFORMATION pi{};
		std::wstring cmd = commandLine;
		BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
		                         CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, workDir.wstring().c_str(), &si, &pi);
		CloseHandle(writePipe);
		if (!ok)
		{
			CloseHandle(readPipe);
			std::lock_guard<std::mutex> lock(mutex);
			AppendLocked("[gui] 실행 실패: CreateProcess error " + std::to_string(GetLastError()));
			return false;
		}
		job = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
		AssignProcessToJobObject(job, pi.hProcess);
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);

		process = pi.hProcess;
		running = true;
		finished = false;
		startedAt = std::chrono::steady_clock::now();
		reader = std::thread([this, readPipe] { ReadLoop(readPipe); });
		return true;
	}

	void Cancel()
	{
		if (running && job)
		{
			TerminateJobObject(job, 1);
			std::lock_guard<std::mutex> lock(mutex);
			AppendLocked("[gui] 취소됨");
		}
	}

	// True once per completed command; the UI uses it to refresh run history.
	bool ConsumeFinished()
	{
		bool expected = true;
		return finished.compare_exchange_strong(expected, false);
	}

	double ElapsedSeconds() const
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
	}

	void Clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		lines.clear();
	}

	template <typename F> void WithLines(F&& fn)
	{
		std::lock_guard<std::mutex> lock(mutex);
		fn(lines);
	}

	std::atomic<bool> running{ false };
	int exitCode = 0;
	size_t version = 0;

private:
	void AppendLocked(const std::string& line)
	{
		lines.push_back(line);
		if (lines.size() > 20000) lines.pop_front();
		++version;
	}

	void ReadLoop(HANDLE pipe)
	{
		std::string pending;
		char buffer[4096];
		DWORD read = 0;
		while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
		{
			pending.append(buffer, read);
			size_t pos;
			while ((pos = pending.find('\n')) != std::string::npos)
			{
				std::string line = pending.substr(0, pos);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				pending.erase(0, pos + 1);
				std::lock_guard<std::mutex> lock(mutex);
				AppendLocked(ConsoleToUtf8(line));
			}
		}
		CloseHandle(pipe);
		WaitForSingleObject(process, INFINITE);
		DWORD code = 0;
		GetExitCodeProcess(process, &code);
		CloseHandle(process);
		CloseHandle(job);
		process = nullptr;
		job = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!pending.empty()) AppendLocked(ConsoleToUtf8(pending));
			AppendLocked("[gui] 종료 코드 " + std::to_string(code) + " (" + std::to_string((int)ElapsedSeconds()) + "초)");
		}
		exitCode = (int)code;
		running = false;
		finished = true;
	}

	void Join()
	{
		if (reader.joinable()) reader.join();
	}

	std::mutex mutex;
	std::deque<std::string> lines;
	std::thread reader;
	HANDLE process = nullptr;
	HANDLE job = nullptr;
	std::atomic<bool> finished{ false };
	std::chrono::steady_clock::time_point startedAt;
};

// ============================================================================ app model

struct RunInfo
{
	std::string id;
	std::string line;
	std::string verdict;
	std::string finishedAt;
	long long totalMs = 0;
	fs::path dir;
	json run;
};

struct App
{
	fs::path settingsDir;
	fs::path home;               // dwo-Assay repository (DWO_ASSAY_HOME), used to attach new projects
	fs::path project;
	std::string projectName;
	bool hasUProject = false;
	bool attached = false;
	bool isCodeProject = false;
	json state;
	std::vector<RunInfo> runs;
	int selectedRun = -1;
	json detail;                 // cached out/*.json of the selected run
	std::string detailFor;
	std::vector<std::string> recent;
	char projectInput[1024] = {};
	char filter[256] = {};
	std::string message;         // one-line status for the user
	int tab = 0;                 // 0 log, 1 history, 2 result
	int requestTab = -1;
	bool confirmDetach = false;
	Runner runner;

	void LoadSettings()
	{
		if (auto s = ReadJson(settingsDir / "gui.json"))
			if (s->contains("recent") && (*s)["recent"].is_array())
				for (auto& r : (*s)["recent"]) recent.push_back(r.get<std::string>());
	}

	void SaveSettings()
	{
		json s;
		s["recent"] = recent;
		std::error_code ec;
		fs::create_directories(settingsDir, ec);
		std::ofstream(settingsDir / "gui.json", std::ios::binary) << s.dump(1);
	}

	void SetProject(const fs::path& path)
	{
		project = path;
		std::string u = PathUtf8(path);
		strncpy_s(projectInput, u.c_str(), _TRUNCATE);
		recent.erase(std::remove(recent.begin(), recent.end(), u), recent.end());
		recent.insert(recent.begin(), u);
		if (recent.size() > 8) recent.resize(8);
		SaveSettings();
		Refresh();
	}

	void Refresh()
	{
		hasUProject = false;
		projectName.clear();
		std::error_code ec;
		if (!project.empty() && fs::is_directory(project, ec))
			for (auto& e : fs::directory_iterator(project, ec))
				if (e.path().extension() == ".uproject")
				{
					hasUProject = true;
					projectName = PathUtf8(e.path().stem());
					break;
				}
		isCodeProject = fs::exists(project / "Source", ec);
		auto st = ReadJson(project / "__dwo-assay" / "state.json");
		attached = st.has_value();
		state = st ? *st : json::object();
		LoadRuns();
	}

	void LoadRuns()
	{
		std::string keep = (selectedRun >= 0 && selectedRun < (int)runs.size()) ? runs[selectedRun].id : "";
		runs.clear();
		selectedRun = -1;
		std::error_code ec;
		fs::path root = project / "__dwo-assay" / "data" / "runs";
		if (fs::is_directory(root, ec))
			for (auto& e : fs::directory_iterator(root, ec))
			{
				if (!e.is_directory()) continue;
				RunInfo r;
				r.dir = e.path();
				r.id = PathUtf8(e.path().filename());
				if (auto j = ReadJson(e.path() / "run.json"))
				{
					r.run = *j;
					r.line = Str(*j, "line");
					r.verdict = Str(*j, "final", "?");
					r.finishedAt = Str(*j, "finishedAt", Str(*j, "startedAt"));
					if (j->contains("stations") && (*j)["stations"].is_array())
						for (auto& s : (*j)["stations"]) r.totalMs += Num(s, "ms");
				}
				else
				{
					r.line = "-";
					r.verdict = "incomplete";
				}
				runs.push_back(std::move(r));
			}
		std::sort(runs.begin(), runs.end(), [](const RunInfo& a, const RunInfo& b) {
			// ids carry a timestamp after the line prefix; compare that part.
			auto stamp = [](const std::string& id) { auto p = id.find('-'); return p == std::string::npos ? id : id.substr(p + 1); };
			return stamp(a.id) > stamp(b.id);
		});
		for (int i = 0; i < (int)runs.size(); ++i)
			if (runs[i].id == keep) selectedRun = i;
		detailFor.clear();
	}

	fs::path Bat() const { return project / "__dwo-assay.bat"; }

	void Run(const char* command)
	{
		if (runner.running) return;
		std::error_code ec;
		if (!fs::exists(Bat(), ec))
		{
			if (home.empty() || !fs::exists(home / "__dwo-assay.bat", ec))
			{
				message = "DWO_ASSAY_HOME이 설정되지 않았습니다. dwo-Assay 폴더의 setup.bat을 먼저 실행하세요.";
				return;
			}
			fs::copy_file(home / "__dwo-assay.bat", Bat(), fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				message = "__dwo-assay.bat 복사 실패: " + ec.message();
				return;
			}
		}
		if (!home.empty()) SetEnvironmentVariableW(L"DWO_ASSAY_HOME", home.wstring().c_str());
		std::wstring cmd = L"cmd.exe /d /s /c \"\"" + Bat().wstring() + L"\" " + ToWide(command) + L"\"";
		if (runner.Start(cmd, project, std::string("__dwo-assay.bat ") + command))
		{
			message.clear();
			requestTab = 0;
		}
	}

	const json& Detail()
	{
		if (selectedRun < 0 || selectedRun >= (int)runs.size()) { static json empty; return empty; }
		RunInfo& r = runs[selectedRun];
		if (detailFor != r.id)
		{
			detail = json::object();
			fs::path out = r.dir / "out";
			for (const char* name : { "findings", "quality", "verify", "logs" })
				if (auto j = ReadJson(out / (std::string(name) + ".json"))) detail[name] = *j;
			detailFor = r.id;
		}
		return detail;
	}
};

// ============================================================================ widgets

static ImVec4 VerdictColor(const std::string& v)
{
	if (v == "pass") return ImVec4(0.12f, 0.55f, 0.42f, 1);
	if (v == "warn" || v == "Warning") return ImVec4(0.80f, 0.50f, 0.10f, 1);
	if (v == "fail" || v == "Error" || v == "Fatal" || v == "crash") return ImVec4(0.78f, 0.22f, 0.20f, 1);
	if (v == "info") return ImVec4(0.20f, 0.45f, 0.75f, 1);
	return ImVec4(0.40f, 0.44f, 0.48f, 1);
}

static void Badge(const std::string& text)
{
	ImVec2 pad(6, 2);
	ImVec2 size = ImGui::CalcTextSize(text.c_str()) + pad * 2;
	ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddRectFilled(pos, pos + size, ImGui::GetColorU32(VerdictColor(text)), 3.0f);
	ImGui::GetWindowDrawList()->AddText(pos + pad, IM_COL32(255, 255, 255, 255), text.c_str());
	ImGui::Dummy(size);
}

static void SectionTitle(const char* text)
{
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
	ImGui::Separator();
}

static void KeyValue(const char* key, const std::string& value)
{
	ImGui::TextDisabled("%s", key);
	ImGui::SameLine(110);
	ImGui::TextWrapped("%s", value.empty() ? "-" : value.c_str());
}

static bool BigButton(const char* label, const char* hint, bool enabled)
{
	ImGui::BeginDisabled(!enabled);
	bool clicked = ImGui::Button(label, ImVec2(-FLT_MIN, 40));
	ImGui::EndDisabled();
	if (hint && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hint);
	return clicked;
}

static const ImGuiTableFlags kTable = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_SizingStretchProp;

static void StringList(const json& items, const char* filter, int limit = 300)
{
	if (!items.is_array()) return;
	int shown = 0;
	for (auto& it : items)
	{
		std::string s = it.is_string() ? it.get<std::string>() : it.dump();
		if (!ContainsNoCase(s, filter)) continue;
		ImGui::BulletText("%s", s.c_str());
		if (++shown >= limit) { ImGui::TextDisabled("... (%d개 이상)", limit); break; }
	}
	if (shown == 0) ImGui::TextDisabled("없음");
}

static void LogGroupsTable(const json& summary, const char* filter, const char* id)
{
	const json& groups = summary.contains("groups") ? summary["groups"] : json::array();
	if (summary.contains("crashes") && summary["crashes"].is_array())
		for (auto& c : summary["crashes"])
		{
			Badge("crash");
			ImGui::SameLine();
			ImGui::TextWrapped("%s  %s", Str(c, "folder").c_str(), Str(c, "error").c_str());
			if (ImGui::TreeNode(&c, "콜스택"))
			{
				StringList(c.value("callstack", json::array()), nullptr);
				ImGui::TreePop();
			}
		}
	if (!ImGui::BeginTable(id, 4, kTable)) return;
	ImGui::TableSetupColumn("심각도", ImGuiTableColumnFlags_WidthFixed, 90);
	ImGui::TableSetupColumn("횟수", ImGuiTableColumnFlags_WidthFixed, 50);
	ImGui::TableSetupColumn("카테고리", ImGuiTableColumnFlags_WidthFixed, 200);
	ImGui::TableSetupColumn("내용");
	ImGui::TableHeadersRow();
	int rows = 0;
	for (auto& g : groups)
	{
		std::string sample = Str(g, "sample"), cat = Str(g, "category");
		if (!ContainsNoCase(sample + " " + cat, filter)) continue;
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		Badge(Str(g, "severity"));
		if (g.value("new", false)) { ImGui::SameLine(); Badge("new"); }
		ImGui::TableNextColumn();
		ImGui::Text("%lld", Num(g, "count"));
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(cat.c_str());
		ImGui::TableNextColumn();
		ImGui::TextWrapped("%s", sample.c_str());
		std::string where;
		for (auto& w : g.value("where", json::array())) where += (where.empty() ? "" : ", ") + w.get<std::string>();
		ImGui::TextDisabled("%s", where.c_str());
		++rows;
	}
	if (rows == 0)
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextDisabled("없음");
	}
	ImGui::EndTable();
}

// ============================================================================ panels

static void DrawSidebar(App& app)
{
	SectionTitle("프로젝트");
	if (!app.hasUProject)
	{
		ImGui::TextWrapped("위쪽에서 .uproject가 있는 폴더를 선택하세요.");
		return;
	}
	ImGui::Text("%s", app.projectName.c_str());
	ImGui::TextDisabled("%s", app.isCodeProject ? "C++ 프로젝트" : "Blueprint 전용 프로젝트");
	ImGui::Spacing();
	if (app.attached)
	{
		KeyValue("엔진", "UE " + Str(app.state, "engineVersion"));
		KeyValue("시스템", Str(app.state, "systemCommit"));
		KeyValue("부착", Str(app.state, "attachedAt").substr(0, 16));
	}
	else
	{
		ImGui::PushStyleColor(ImGuiCol_Text, VerdictColor("warn"));
		ImGui::TextWrapped("부착되지 않은 프로젝트입니다.");
		ImGui::PopStyleColor();
	}

	bool idle = !app.runner.running;
	SectionTitle("생산 라인");
	if (BigButton("분석  analyze", "아키텍처 리포트와 품질 점검 (수 초~수십 초)", idle && app.attached)) app.Run("analyze");
	if (BigButton("검증  verify", "BP 전체 컴파일, 데이터 검증, 자동화 테스트. 에디터를 닫고 실행하세요.", idle && app.attached)) app.Run("verify");
	if (BigButton("로그  logs", "Saved/Logs와 크래시 분석", idle && app.attached)) app.Run("logs");

	SectionTitle("관리");
	float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	ImGui::BeginDisabled(!idle);
	if (!app.attached)
	{
		if (ImGui::Button("부착  attach", ImVec2(-FLT_MIN, 34))) app.Run("attach");
	}
	else
	{
		if (ImGui::Button("점검 doctor", ImVec2(half, 0))) app.Run("doctor");
		ImGui::SameLine();
		if (ImGui::Button("상태 status", ImVec2(half, 0))) app.Run("status");
		if (ImGui::Button("업데이트 update", ImVec2(half, 0))) app.Run("update");
		ImGui::SameLine();
		if (ImGui::Button("도구 tools", ImVec2(half, 0))) app.Run("tools");
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1));
		if (ImGui::Button("제거  detach", ImVec2(-FLT_MIN, 0))) app.confirmDetach = true;
		ImGui::PopStyleColor();
	}
	ImGui::EndDisabled();

	if (app.runner.running)
	{
		SectionTitle("실행 중");
		ImGui::Text("%.0f초 경과", app.runner.ElapsedSeconds());
		if (ImGui::Button("취소", ImVec2(-FLT_MIN, 0))) app.runner.Cancel();
	}
	if (!app.message.empty())
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, VerdictColor("fail"));
		ImGui::TextWrapped("%s", app.message.c_str());
		ImGui::PopStyleColor();
	}
}

static void DrawLog(App& app)
{
	if (ImGui::Button("지우기")) app.runner.Clear();
	ImGui::SameLine();
	if (ImGui::Button("복사"))
	{
		std::string all;
		app.runner.WithLines([&](std::deque<std::string>& lines) { for (auto& l : lines) all += l + "\n"; });
		ImGui::SetClipboardText(all.c_str());
	}
	static bool follow = true;
	ImGui::SameLine();
	ImGui::Checkbox("자동 스크롤", &follow);
	ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	static size_t lastVersion = 0;
	app.runner.WithLines([&](std::deque<std::string>& lines) {
		ImGuiListClipper clipper;
		clipper.Begin((int)lines.size());
		while (clipper.Step())
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
			{
				const std::string& l = lines[i];
				bool warn = l.find("ERROR") != std::string::npos || l.find("[fail]") != std::string::npos;
				bool head = l.rfind("> ", 0) == 0;
				if (warn) ImGui::PushStyleColor(ImGuiCol_Text, VerdictColor("fail"));
				else if (head) ImGui::PushStyleColor(ImGuiCol_Text, VerdictColor("info"));
				ImGui::TextUnformatted(l.c_str());
				if (warn || head) ImGui::PopStyleColor();
			}
	});
	if (follow && app.runner.version != lastVersion) ImGui::SetScrollHereY(1.0f);
	lastVersion = app.runner.version;
	ImGui::EndChild();
}

static const char* LineName(const std::string& line)
{
	if (line == "architecture-report") return "분석";
	if (line == "verify") return "검증";
	if (line == "logs") return "로그";
	return line.c_str();
}

static void DrawHistory(App& app)
{
	if (ImGui::Button("새로고침")) app.LoadRuns();
	ImGui::SameLine();
	ImGui::TextDisabled("실행 %d개 · 행을 두 번 클릭하면 결과 탭으로 이동", (int)app.runs.size());
	if (!ImGui::BeginTable("runs", 5, kTable | ImGuiTableFlags_ScrollY)) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("실행 ID", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("라인", ImGuiTableColumnFlags_WidthFixed, 70);
	ImGui::TableSetupColumn("판정", ImGuiTableColumnFlags_WidthFixed, 90);
	ImGui::TableSetupColumn("소요", ImGuiTableColumnFlags_WidthFixed, 80);
	ImGui::TableSetupColumn("공정", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableHeadersRow();
	for (int i = 0; i < (int)app.runs.size(); ++i)
	{
		RunInfo& r = app.runs[i];
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::PushID(i);
		if (ImGui::Selectable(r.id.c_str(), app.selectedRun == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
		{
			app.selectedRun = i;
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.requestTab = 2;
		}
		ImGui::PopID();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(LineName(r.line));
		ImGui::TableNextColumn();
		Badge(r.verdict);
		ImGui::TableNextColumn();
		ImGui::Text("%.1f초", r.totalMs / 1000.0);
		ImGui::TableNextColumn();
		std::string st;
		if (r.run.contains("stations"))
			for (auto& s : r.run["stations"]) st += Str(s, "id") + "=" + Str(s, "verdict", "?") + "  ";
		ImGui::TextUnformatted(st.c_str());
	}
	ImGui::EndTable();
}

static void DrawAnalyzeResult(App& app, const json& d)
{
	const char* f = app.filter;
	if (d.contains("findings"))
	{
		const json& fin = d["findings"];
		SectionTitle("검증 게이트");
		ImGui::Text("관계 상태: %s", fin.value("relationStatus", json::object()).dump().c_str());
		for (auto& item : fin.value("findings", json::array()))
		{
			ImGui::PushID(&item);
			Badge(Str(item, "severity"));
			ImGui::SameLine();
			bool open = ImGui::TreeNodeEx("##f", ImGuiTreeNodeFlags_SpanAvailWidth, "%s  ·  %s", Str(item, "rule").c_str(), Str(item, "message").c_str());
			if (open)
			{
				StringList(item.value("items", json::array()), f);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}
	if (!d.contains("quality")) return;
	const json& q = d["quality"];

	if (ImGui::CollapsingHeader("BP 복잡도", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("cx", 6, kTable))
	{
		for (const char* h : { "Blueprint", "그래프", "노드", "분기", "실행 체인", "기준 초과" }) ImGui::TableSetupColumn(h);
		ImGui::TableHeadersRow();
		for (auto& r : q.value("complexity", json::object()).value("graphs", json::array()))
		{
			if (!ContainsNoCase(Str(r, "blueprint") + Str(r, "graph"), f)) continue;
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(r, "blueprint").c_str());
			ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(r, "graph").c_str());
			ImGui::TableNextColumn(); ImGui::Text("%lld", Num(r, "nodes"));
			ImGui::TableNextColumn(); ImGui::Text("%lld", Num(r, "branches"));
			ImGui::TableNextColumn(); ImGui::Text("%lld", Num(r, "execChain"));
			ImGui::TableNextColumn();
			std::string over;
			for (auto& o : r.value("over", json::array())) over += o.get<std::string>() + " ";
			if (over.empty()) ImGui::TextDisabled("-"); else Badge(over);
		}
		ImGui::EndTable();
	}
	if (ImGui::CollapsingHeader("이름 · 폴더 규칙") && ImGui::BeginTable("nm", 3, kTable))
	{
		for (const char* h : { "에셋", "클래스", "문제" }) ImGui::TableSetupColumn(h);
		ImGui::TableHeadersRow();
		for (auto& v : q.value("naming", json::object()).value("violations", json::array()))
		{
			std::string problems;
			for (auto& p : v.value("problems", json::array())) problems += p.get<std::string>() + "  ";
			if (!ContainsNoCase(Str(v, "asset") + problems, f)) continue;
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(v, "asset").c_str());
			ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(v, "class").c_str());
			ImGui::TableNextColumn(); ImGui::TextWrapped("%s", problems.c_str());
		}
		ImGui::EndTable();
	}
	if (ImGui::CollapsingHeader("미사용 후보 · 리다이렉터"))
	{
		ImGui::TextDisabled("도달할 수 없는 에셋");
		StringList(q.value("unused", json::object()).value("unused", json::array()), f);
		ImGui::TextDisabled("리다이렉터");
		StringList(q.value("unused", json::object()).value("redirectors", json::array()), f);
	}
	const json& sm = q.value("stateMonitor", json::object());
	if (sm.value("present", false) && ImGui::CollapsingHeader("StateMonitor 연계", ImGuiTreeNodeFlags_DefaultOpen))
	{
		KeyValue("플러그인", Str(sm, "plugin"));
		std::string ids;
		for (auto& s : sm.value("sourceIds", json::array())) ids += s.get<std::string>() + "  ";
		KeyValue("Source ID", ids);
		KeyValue("테스트 필터", Str(sm, "testFilter"));
		if (ImGui::BeginTable("sm", 3, kTable))
		{
			for (const char* h : { "위치", "종류", "코드" }) ImGui::TableSetupColumn(h);
			ImGui::TableHeadersRow();
			for (auto& r : sm.value("cppRefs", json::array()))
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::Text("%s:%lld", Str(r, "file").c_str(), Num(r, "line"));
				ImGui::TableNextColumn(); ImGui::TextUnformatted(r.value("submit", false) ? "제출" : "참조");
				ImGui::TableNextColumn(); ImGui::TextWrapped("%s", Str(r, "code").c_str());
			}
			for (auto& b : sm.value("bpCalls", json::array()))
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::Text("BP %s.%s", Str(b, "blueprint").c_str(), Str(b, "graph").c_str());
				ImGui::TableNextColumn(); ImGui::TextUnformatted("호출");
				ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(b, "function").c_str());
			}
			ImGui::EndTable();
		}
	}
}

static void DrawVerifyResult(App& app, const json& v)
{
	for (auto& step : v.value("steps", json::array()))
	{
		ImGui::PushID(&step);
		Badge(Str(step, "verdict"));
		ImGui::SameLine();
		std::string title = Str(step, "id") + "  " + Str(step, "name");
		if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (step.contains("summary") && step["summary"].is_object())
			{
				const json& c = step["summary"]["counts"];
				ImGui::Text("Error %lld · Warning %lld · Fatal %lld", Num(c, "Error"), Num(c, "Warning"), Num(c, "Fatal"));
				LogGroupsTable(step["summary"], app.filter, "groups");
			}
			else if (step.contains("automation") && step["automation"].is_object())
			{
				const json& a = step["automation"];
				const json& s = a["summary"];
				ImGui::Text("성공 %lld · 경고 포함 %lld · 실패 %lld · 미실행 %lld  %s", Num(s, "succeeded"), Num(s, "succeededWithWarnings"),
				            Num(s, "failed"), Num(s, "notRun"), Str(step, "note").c_str());
				if (ImGui::BeginTable("tests", 3, kTable))
				{
					for (const char* h : { "테스트", "결과", "오류" }) ImGui::TableSetupColumn(h);
					ImGui::TableHeadersRow();
					for (auto& t : a.value("tests", json::array()))
					{
						if (!ContainsNoCase(Str(t, "name"), app.filter)) continue;
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(t, "name").c_str());
						ImGui::TableNextColumn(); Badge(Str(t, "state") == "Success" ? "pass" : "fail");
						ImGui::TableNextColumn();
						for (auto& e : t.value("errors", json::array())) ImGui::TextWrapped("%s", e.get<std::string>().c_str());
					}
					ImGui::EndTable();
				}
			}
			else
			{
				ImGui::TextDisabled("%s", Str(step, "note", "실행 안 함").c_str());
			}
		}
		ImGui::PopID();
	}
}

static void DrawResult(App& app)
{
	if (app.selectedRun < 0 || app.selectedRun >= (int)app.runs.size())
	{
		ImGui::TextDisabled("실행 이력 탭에서 실행을 선택하세요.");
		return;
	}
	RunInfo& r = app.runs[app.selectedRun];
	const json& d = app.Detail();
	ImGui::Text("%s", r.id.c_str());
	ImGui::SameLine();
	Badge(r.verdict);
	ImGui::SameLine();
	if (ImGui::Button("리포트 열기")) OpenWithShell(r.dir / "out" / "report.html");
	ImGui::SameLine();
	if (ImGui::Button("폴더 열기")) OpenWithShell(r.dir);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(260);
	ImGui::InputTextWithHint("##filter", "필터 (에셋, 규칙, 메시지...)", app.filter, sizeof(app.filter));
	ImGui::TextDisabled("UML·실행 흐름 다이어그램은 '리포트 열기'로 브라우저에서 볼 수 있습니다.");

	if (r.run.contains("stations") && ImGui::BeginTable("stations", 4, kTable))
	{
		for (const char* h : { "공정", "이름", "판정", "소요" }) ImGui::TableSetupColumn(h);
		ImGui::TableHeadersRow();
		for (auto& s : r.run["stations"])
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(s, "id").c_str());
			ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(s, "name").c_str());
			ImGui::TableNextColumn(); Badge(Str(s, "verdict", "?"));
			ImGui::TableNextColumn(); ImGui::Text("%.1f초", Num(s, "ms") / 1000.0);
		}
		ImGui::EndTable();
	}

	ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_None);
	if (r.line == "verify" && d.contains("verify")) DrawVerifyResult(app, d["verify"]);
	else if (r.line == "logs" && d.contains("logs"))
	{
		const json& c = d["logs"]["counts"];
		ImGui::Text("Fatal %lld · Error %lld · Warning %lld · 새 그룹 %lld", Num(c, "Fatal"), Num(c, "Error"), Num(c, "Warning"), Num(d["logs"], "newGroups"));
		LogGroupsTable(d["logs"], app.filter, "loggroups");
	}
	else DrawAnalyzeResult(app, d);
	ImGui::EndChild();
}

static bool PickFolder(HWND owner, fs::path& out)
{
	IFileOpenDialog* dialog = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return false;
	DWORD options = 0;
	dialog->GetOptions(&options);
	dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	dialog->SetTitle(L".uproject가 있는 프로젝트 폴더 선택");
	bool ok = false;
	if (SUCCEEDED(dialog->Show(owner)))
	{
		IShellItem* item = nullptr;
		if (SUCCEEDED(dialog->GetResult(&item)))
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				out = path;
				CoTaskMemFree(path);
				ok = true;
			}
			item->Release();
		}
	}
	dialog->Release();
	return ok;
}

static void DrawApp(App& app, HWND hwnd)
{
	if (app.runner.ConsumeFinished()) app.Refresh();

	ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::Begin("dwo-Assay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
	                                   ImGuiWindowFlags_NoBringToFrontOnFocus);

	// ---- project bar
	ImGui::TextUnformatted("dwo-Assay");
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 260);
	if (ImGui::InputTextWithHint("##project", "프로젝트 폴더 경로", app.projectInput, sizeof(app.projectInput), ImGuiInputTextFlags_EnterReturnsTrue))
		app.SetProject(fs::u8path(app.projectInput));
	ImGui::SameLine();
	ImGui::BeginDisabled(app.runner.running);
	if (ImGui::Button("찾아보기...")) { fs::path p; if (PickFolder(hwnd, p)) app.SetProject(p); }
	ImGui::SameLine();
	if (ImGui::Button("최근")) ImGui::OpenPopup("recent");
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("새로고침")) app.Refresh();
	if (ImGui::BeginPopup("recent"))
	{
		if (app.recent.empty()) ImGui::TextDisabled("최근 프로젝트 없음");
		for (auto& r : app.recent)
			if (ImGui::Selectable(r.c_str())) { app.SetProject(fs::u8path(r)); break; }
		ImGui::EndPopup();
	}
	ImGui::Separator();

	// ---- body
	ImGui::BeginChild("side", ImVec2(270, 0), ImGuiChildFlags_Borders);
	DrawSidebar(app);
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("main", ImVec2(0, 0));
	if (ImGui::BeginTabBar("tabs"))
	{
		const char* names[] = { "실행 로그", "실행 이력", "결과" };
		for (int i = 0; i < 3; ++i)
		{
			ImGuiTabItemFlags flags = app.requestTab == i ? ImGuiTabItemFlags_SetSelected : 0;
			if (ImGui::BeginTabItem(names[i], nullptr, flags))
			{
				app.tab = i;
				if (i == 0) DrawLog(app);
				else if (i == 1) DrawHistory(app);
				else DrawResult(app);
				ImGui::EndTabItem();
			}
		}
		app.requestTab = -1;
		ImGui::EndTabBar();
	}
	ImGui::EndChild();

	// ---- detach confirmation
	if (app.confirmDetach) { ImGui::OpenPopup("detach?"); app.confirmDetach = false; }
	if (ImGui::BeginPopupModal("detach?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("dwo-Assay를 이 프로젝트에서 제거합니다.");
		ImGui::TextDisabled("플러그인, exclude 블록, __dwo-assay 폴더(분석 기록 포함)가 삭제됩니다.");
		static bool keep = true;
		ImGui::Checkbox("분석 기록 보존 (--keep-data)", &keep);
		if (ImGui::Button("제거", ImVec2(120, 0))) { app.Run(keep ? "detach --keep-data" : "detach"); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("취소", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::End();
}

// ============================================================================ D3D11 / Win32 boilerplate

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeWidth = 0, g_resizeHeight = 0;

static void CreateRenderTarget()
{
	ID3D11Texture2D* back = nullptr;
	g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
	g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
	back->Release();
}

static void CleanupRenderTarget()
{
	if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDeviceD3D(HWND hwnd)
{
	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL level;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
	                                           &g_swapChain, &g_device, &level, &g_context);
	if (hr == DXGI_ERROR_UNSUPPORTED)
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
		                                   &g_swapChain, &g_device, &level, &g_context);
	if (FAILED(hr)) return false;
	CreateRenderTarget();
	return true;
}

static void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
	if (g_context) { g_context->Release(); g_context = nullptr; }
	if (g_device) { g_device->Release(); g_device = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
	switch (msg)
	{
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED) return 0;
		g_resizeWidth = (UINT)LOWORD(lParam);
		g_resizeHeight = (UINT)HIWORD(lParam);
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ApplyStyle(float scale)
{
	ImGuiStyle& s = ImGui::GetStyle();
	ImGui::StyleColorsDark();
	s.WindowRounding = 0;
	s.ChildRounding = 4;
	s.FrameRounding = 4;
	s.GrabRounding = 4;
	s.TabRounding = 4;
	s.FramePadding = ImVec2(8, 5);
	s.ItemSpacing = ImVec2(8, 6);
	s.CellPadding = ImVec2(6, 4);
	ImVec4* c = s.Colors;
	c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.11f, 0.13f, 1);
	c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.13f, 0.16f, 1);
	c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.14f, 0.17f, 1);
	c[ImGuiCol_Button] = ImVec4(0.06f, 0.36f, 0.34f, 1);
	c[ImGuiCol_ButtonHovered] = ImVec4(0.09f, 0.47f, 0.44f, 1);
	c[ImGuiCol_ButtonActive] = ImVec4(0.05f, 0.30f, 0.28f, 1);
	c[ImGuiCol_Header] = ImVec4(0.12f, 0.28f, 0.30f, 1);
	c[ImGuiCol_HeaderHovered] = ImVec4(0.15f, 0.36f, 0.38f, 1);
	c[ImGuiCol_HeaderActive] = ImVec4(0.12f, 0.32f, 0.34f, 1);
	c[ImGuiCol_Tab] = ImVec4(0.12f, 0.17f, 0.20f, 1);
	c[ImGuiCol_TabHovered] = ImVec4(0.15f, 0.36f, 0.38f, 1);
	c[ImGuiCol_TabSelected] = ImVec4(0.09f, 0.40f, 0.38f, 1);
	c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.17f, 0.20f, 1);
	c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.18f, 0.21f, 1);
	s.ScaleAllSizes(scale);
}

static std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* name)
{
	for (int i = 1; i + 1 < argc; ++i)
		if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
	return {};
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	ImGui_ImplWin32_EnableDpiAwareness();
	SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");

	App app;
	wchar_t local[MAX_PATH] = {};
	GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
	app.settingsDir = fs::path(local) / L"dwo-Assay";
	app.LoadSettings();

	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::wstring projectArg = ArgValue(argc, argv, L"--project");
	std::wstring homeArg = ArgValue(argc, argv, L"--home");
	std::wstring tabArg = ArgValue(argc, argv, L"--tab");
	std::wstring runArg = ArgValue(argc, argv, L"--run");
	LocalFree(argv);
	if (homeArg.empty())
	{
		wchar_t buf[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"DWO_ASSAY_HOME", buf, MAX_PATH)) homeArg = buf;
	}
	app.home = homeArg;

	WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"DwoAssayGui", nullptr };
	RegisterClassExW(&wc);
	HWND hwnd = CreateWindowW(wc.lpszClassName, L"dwo-Assay", WS_OVERLAPPEDWINDOW, 100, 80, 1360, 860, nullptr, nullptr, wc.hInstance, nullptr);
	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		MessageBoxW(nullptr, L"DirectX 11 장치를 만들 수 없습니다.", L"dwo-Assay", MB_ICONERROR);
		return 1;
	}
	float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
	SetWindowPos(hwnd, nullptr, 0, 0, (int)(1360 * scale), (int)(860 * scale), SWP_NOMOVE | SWP_NOZORDER);
	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	static std::string iniPath = PathUtf8(app.settingsDir / "gui-layout.ini");
	std::error_code ec;
	fs::create_directories(app.settingsDir, ec);
	io.IniFilename = iniPath.c_str();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ApplyStyle(scale);
	const char* font = "C:\\Windows\\Fonts\\malgun.ttf";
	if (fs::exists(font)) io.Fonts->AddFontFromFileTTF(font, 16.0f * scale, nullptr, io.Fonts->GetGlyphRangesKorean());
	else io.Fonts->AddFontDefault();

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_device, g_context);

	// Initial project: argument, then current folder, then the most recent one.
	if (!projectArg.empty()) app.SetProject(projectArg);
	else
	{
		wchar_t cwd[MAX_PATH] = {};
		GetCurrentDirectoryW(MAX_PATH, cwd);
		app.project = cwd;
		app.Refresh();
		if (app.hasUProject) app.SetProject(cwd);
		else if (!app.recent.empty()) app.SetProject(fs::u8path(app.recent.front()));
		else app.project.clear();
	}
	if (tabArg == L"history") app.requestTab = 1;
	else if (tabArg == L"result") { app.requestTab = 2; if (!app.runs.empty()) app.selectedRun = 0; }
	if (!runArg.empty() && app.hasUProject) app.Run(ToUtf8(runArg).c_str());

	bool done = false;
	while (!done)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT) done = true;
		}
		if (done) break;
		if (IsIconic(hwnd)) { Sleep(50); continue; }
		if (g_resizeWidth != 0 && g_resizeHeight != 0)
		{
			CleanupRenderTarget();
			g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
			g_resizeWidth = g_resizeHeight = 0;
			CreateRenderTarget();
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		DrawApp(app, hwnd);
		ImGui::Render();
		const float clear[4] = { 0.09f, 0.11f, 0.13f, 1.0f };
		g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_context->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swapChain->Present(1, 0);
	}

	app.runner.Cancel();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDeviceD3D();
	DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	CoUninitialize();
	return 0;
}
