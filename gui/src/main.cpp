// dwo-Lens control panel: Dear ImGui + Win32 + DirectX 11.
//
// Runs the same commands as __dwo-lens.bat and reads the same run.json / out/*.json files,
// so the GUI and the command line always show identical results.
//
//   dwo-lens-gui.exe [--project <folder with .uproject>] [--home <dwo-Lens repository>]
//                     [--tab log|history|result] [--select <run id>] [--run <command>]   (--run starts a command on launch)
//   dwo-lens-gui.exe --project <folder> --export-report <file.html>   (no window: latest analyze+verify+logs in one HTML)

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
#include <functional>
#include <map>
#include <regex>
#include <set>
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

static HWND g_hwnd = nullptr;

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

// ============================================================================ structure model (from graph.json and flows/*.mmd)

struct ClassNode
{
	std::string name;
	std::string kind;
	bool external = false;
	std::vector<std::string> props;
	std::vector<std::string> funcs;
	std::vector<std::string> children;
};

struct FlowEdge
{
	std::string to;
	std::string pin;
};

struct Flow
{
	std::string title;
	std::map<std::string, std::string> labels;
	std::map<std::string, std::vector<FlowEdge>> out;
	std::vector<std::string> starts;
};

struct Structure
{
	struct Ref
	{
		std::string src, dst, type;
	};
	bool loaded = false;
	std::map<std::string, ClassNode> classes;
	std::vector<std::string> classRoots;
	std::vector<Ref> calls, assetRefs, moduleDeps;
	std::vector<Flow> flows;
};

static std::string MermaidUnescape(std::string s)
{
	auto replaceAll = [&s](const std::string& a, const std::string& b) {
		for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
	};
	replaceAll("#quot;", "\"");
	replaceAll("#lt;", "<");
	replaceAll("#gt;", ">");
	return s;
}

static Flow ParseFlow(const fs::path& file)
{
	Flow f;
	f.title = PathUtf8(file.stem());
	std::ifstream in(file, std::ios::binary);
	std::string line;
	std::set<std::string> hasIncoming;
	std::vector<std::string> order;
	static const std::regex edge(R"(^(\S+) -->(?:\|([^|]*)\|)? (\S+)$)");
	std::smatch m;
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.rfind("flowchart", 0) == 0 || line.empty()) continue;
		if (std::regex_match(line, m, edge))
		{
			f.out[m[1]].push_back({ m[3], MermaidUnescape(m[2]) });
			hasIncoming.insert(m[3]);
			continue;
		}
		size_t cut = line.find_first_of("[({");
		size_t q1 = line.find('"'), q2 = line.rfind('"');
		if (cut == std::string::npos || q1 == std::string::npos || q2 <= q1) continue;
		std::string id = line.substr(0, cut);
		f.labels[id] = MermaidUnescape(line.substr(q1 + 1, q2 - q1 - 1));
		order.push_back(id);
	}
	for (auto& id : order)
		if (!hasIncoming.count(id)) f.starts.push_back(id);
	return f;
}

static void LoadStructure(const fs::path& outDir, Structure& s)
{
	s = Structure();
	auto graph = ReadJson(outDir / "graph.json");
	if (!graph) return;
	std::map<std::string, std::string> names;
	for (auto& sym : (*graph)["symbols"])
	{
		std::string key = Str(sym, "key"), kind = Str(sym, "kind");
		std::string name = Str(sym, "name");
		if (kind == "asset") name = Str(sym, "package", name);
		names[key] = name;
		if (kind != "class" && kind != "blueprint") continue;
		ClassNode& n = s.classes[key];
		n.name = name;
		n.kind = kind;
		n.external = sym.value("external", false);
		if (sym.contains("properties"))
			for (auto& p : sym["properties"]) n.props.push_back(Str(p, "type") + " " + Str(p, "name"));
		if (sym.contains("functions"))
			for (auto& fn : sym["functions"]) n.funcs.push_back(fn.get<std::string>());
	}
	auto nameOf = [&names](const std::string& key) {
		auto it = names.find(key);
		return it != names.end() ? it->second : key.substr(key.find(':') + 1);
	};
	std::set<std::string> hasParent;
	for (auto& r : (*graph)["relations"])
	{
		std::string type = Str(r, "type"), src = Str(r, "source"), dst = Str(r, "target");
		if (type == "Inherits")
		{
			if (!s.classes.count(dst)) { s.classes[dst].name = nameOf(dst); s.classes[dst].external = true; }
			s.classes[dst].children.push_back(src);
			hasParent.insert(src);
		}
		else if (type == "Calls")
			s.calls.push_back({ nameOf(src), nameOf(dst), "호출 " + std::to_string(Num(r, "count")) + "회" });
		else if (type == "AssetHardRef" || type == "AssetSoftRef")
			s.assetRefs.push_back({ nameOf(src), nameOf(dst), type == "AssetHardRef" ? "하드 참조" : "소프트 참조" });
		else if (type == "DependsOnModule" || type == "DependsOnPlugin")
			s.moduleDeps.push_back({ nameOf(src), nameOf(dst), type == "DependsOnModule" ? "모듈" : "플러그인" });
	}
	// Roots: tops of inheritance chains that lead to at least one project class.
	std::function<bool(const std::string&, int)> hasProject = [&](const std::string& key, int depth) {
		const ClassNode& n = s.classes[key];
		if (!n.external) return true;
		if (depth > 30) return false;
		for (auto& c : n.children)
			if (hasProject(c, depth + 1)) return true;
		return false;
	};
	for (auto& [key, n] : s.classes)
		if (!hasParent.count(key) && hasProject(key, 0)) s.classRoots.push_back(key);

	std::error_code ec;
	if (fs::is_directory(outDir / "flows", ec))
		for (auto& e : fs::directory_iterator(outDir / "flows", ec))
			if (e.path().extension() == ".mmd") s.flows.push_back(ParseFlow(e.path()));
	std::sort(s.flows.begin(), s.flows.end(), [](const Flow& a, const Flow& b) { return a.title < b.title; });
	s.loaded = true;
}

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
	fs::path home;               // dwo-Lens repository (DWO_LENS_HOME), used to attach new projects
	fs::path project;
	std::string projectName;
	bool hasUProject = false;
	bool attached = false;
	bool isCodeProject = false;
	int stateMonitor = 0;        // 0 none, 1 installed by dwo-Lens, 2 part of the project
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
	std::string lastCommand;
	bool latestReady = false;   // a line just finished; offer its result
	Structure structure;        // class tree, flows and references of the selected analyze run
	fs::path saved;             // last exported HTML report
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
		stateMonitor = 0;
		if (fs::exists(project / "Plugins" / "__StateMonitor" / "StateMonitor.uplugin", ec)) stateMonitor = 1;
		else if (fs::is_directory(project / "Plugins", ec))
			for (auto it = fs::recursive_directory_iterator(project / "Plugins", ec); it != fs::recursive_directory_iterator(); it.increment(ec))
			{
				if (ec || it.depth() > 3) { if (it.depth() > 3) it.disable_recursion_pending(); continue; }
				if (it->path().filename() == "StateMonitor.uplugin" && PathUtf8(it->path()).find("__") == std::string::npos) { stateMonitor = 2; break; }
			}
		auto st = ReadJson(project / "__dwo-lens" / "state.json");
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
		fs::path root = project / "__dwo-lens" / "data" / "runs";
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

	fs::path Bat() const { return project / "__dwo-lens.bat"; }

	void Run(const char* command)
	{
		if (runner.running) return;
		std::error_code ec;
		if (!fs::exists(Bat(), ec))
		{
			if (home.empty() || !fs::exists(home / "__dwo-lens.bat", ec))
			{
				message = "DWO_LENS_HOME이 설정되지 않았습니다. dwo-Lens 폴더의 setup.bat을 먼저 실행하세요.";
				return;
			}
			fs::copy_file(home / "__dwo-lens.bat", Bat(), fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				message = "__dwo-lens.bat 복사 실패: " + ec.message();
				return;
			}
		}
		if (!home.empty()) SetEnvironmentVariableW(L"DWO_LENS_HOME", home.wstring().c_str());
		std::wstring cmd = L"cmd.exe /d /s /c \"\"" + Bat().wstring() + L"\" " + ToWide(command) + L"\"";
		if (runner.Start(cmd, project, std::string("__dwo-lens.bat ") + command))
		{
			message.clear();
			lastCommand = command;
			latestReady = false;
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
			if (r.line == "architecture-report") LoadStructure(out, structure);
			else structure = Structure();
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

	if (app.attached)
	{
		SectionTitle("제공 도구");
		ImGui::TextUnformatted("StateMonitor");
		ImGui::SameLine();
		const char* smState = app.stateMonitor == 1 ? "설치됨" : app.stateMonitor == 2 ? "프로젝트 포함" : "미설치";
		Badge(app.stateMonitor == 0 ? "-" : "pass");
		ImGui::SameLine();
		ImGui::TextDisabled("%s", smState);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("USTRUCT 상태 스냅샷·변경 이력 뷰어 (런타임).\n설치하면 analyze가 제출 지점을 찾고 verify가 StateMonitor 테스트를 함께 실행합니다.");
		ImGui::BeginDisabled(!idle || app.stateMonitor == 2);
		if (app.stateMonitor == 1)
		{
			if (ImGui::Button("StateMonitor 제거", ImVec2(-FLT_MIN, 0))) app.Run("sm-remove");
		}
		else if (ImGui::Button("StateMonitor 설치", ImVec2(-FLT_MIN, 0))) app.Run("sm-install");
		ImGui::EndDisabled();
	}

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
	if (app.latestReady && !app.runs.empty())
	{
		ImGui::SameLine();
		std::string label = "결과 보기  " + app.runs[0].id + "  " + app.runs[0].verdict;
		ImGui::PushStyleColor(ImGuiCol_Button, VerdictColor(app.runs[0].verdict));
		if (ImGui::Button(label.c_str())) { app.selectedRun = 0; app.requestTab = 2; }
		ImGui::PopStyleColor();
	}
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

static void SaveBundleReport(App& app);
static void DrawSavedNotice(App& app);

static void DrawHistory(App& app)
{
	if (ImGui::Button("새로고침")) app.LoadRuns();
	ImGui::SameLine();
	ImGui::BeginDisabled(app.runs.empty());
	if (ImGui::Button("종합 리포트 저장...")) SaveBundleReport(app);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("최신 분석·검증·로그 결과를 HTML 파일 하나로 저장");
	ImGui::SameLine();
	ImGui::TextDisabled("실행 %d개 · 행을 두 번 클릭하면 결과 탭으로 이동", (int)app.runs.size());
	DrawSavedNotice(app);
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

// Arrays are always taken by reference from the cached detail document. Never iterate a copy:
// ImGui widget ids must stay stable between frames or tree/header open state is lost.
static const json& Arr(const json& j, const char* key)
{
	static const json empty = json::array();
	if (!j.is_object()) return empty;
	auto it = j.find(key);
	return (it != j.end() && it->is_array()) ? *it : empty;
}

static const json& Obj(const json& j, const char* key)
{
	static const json empty = json::object();
	if (!j.is_object()) return empty;
	auto it = j.find(key);
	return (it != j.end() && it->is_object()) ? *it : empty;
}

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
	const json& crashes = Arr(summary, "crashes");
	for (int i = 0; i < (int)crashes.size(); ++i)
	{
		const json& c = crashes[i];
		ImGui::PushID(i);
		Badge("crash");
		ImGui::SameLine();
		ImGui::TextWrapped("%s  %s", Str(c, "folder").c_str(), Str(c, "error").c_str());
		if (ImGui::TreeNode("callstack", "콜스택"))
		{
			StringList(Arr(c, "callstack"), nullptr);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	if (!ImGui::BeginTable(id, 4, kTable)) return;
	ImGui::TableSetupColumn("심각도", ImGuiTableColumnFlags_WidthFixed, 110);
	ImGui::TableSetupColumn("횟수", ImGuiTableColumnFlags_WidthFixed, 50);
	ImGui::TableSetupColumn("카테고리", ImGuiTableColumnFlags_WidthFixed, 200);
	ImGui::TableSetupColumn("내용");
	ImGui::TableHeadersRow();
	int rows = 0;
	for (auto& g : Arr(summary, "groups"))
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
		for (auto& w : Arr(g, "where")) where += (where.empty() ? "" : ", ") + w.get<std::string>();
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

// ============================================================================ structure views (GUI form of the HTML diagrams)

static void DrawClassNode(const Structure& s, const std::string& key, int depth)
{
	auto it = s.classes.find(key);
	if (it == s.classes.end() || depth > 30) return;
	const ClassNode& n = it->second;
	bool hasBody = !n.children.empty() || !n.props.empty() || !n.funcs.empty();
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
	if (!hasBody) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (n.external) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	bool open = ImGui::TreeNodeEx(key.c_str(), flags, "%s%s", n.name.c_str(),
	                              n.external ? "  (엔진)" : (n.kind == "blueprint" ? "  [BP]" : "  [C++]"));
	if (n.external) ImGui::PopStyleColor();
	if (!hasBody || !open) return;
	if (!n.props.empty() || !n.funcs.empty())
	{
		if (ImGui::TreeNodeEx("members", ImGuiTreeNodeFlags_SpanAvailWidth, "멤버 (프로퍼티 %d · 함수 %d)", (int)n.props.size(), (int)n.funcs.size()))
		{
			for (auto& p : n.props) ImGui::BulletText("%s", p.c_str());
			for (auto& f : n.funcs) ImGui::BulletText("%s()", f.c_str());
			ImGui::TreePop();
		}
	}
	for (auto& c : n.children) DrawClassNode(s, c, depth + 1);
	ImGui::TreePop();
}

static void DrawFlowNode(const Flow& f, const std::string& id, const std::string& pin, std::vector<std::string>& path)
{
	auto label = f.labels.find(id);
	std::string text = (pin.empty() ? "" : "[" + pin + "]  ") + (label != f.labels.end() ? label->second : id);
	bool repeat = std::find(path.begin(), path.end(), id) != path.end();
	auto out = f.out.find(id);
	bool leaf = repeat || path.size() > 40 || out == f.out.end() || out->second.empty();
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
	if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	bool open = ImGui::TreeNodeEx(id.c_str(), flags, "%s%s", text.c_str(), repeat ? "  (위로 돌아감)" : "");
	if (leaf || !open) return;
	path.push_back(id);
	for (int i = 0; i < (int)out->second.size(); ++i)
	{
		ImGui::PushID(i);
		DrawFlowNode(f, out->second[i].to, out->second[i].pin, path);
		ImGui::PopID();
	}
	path.pop_back();
	ImGui::TreePop();
}

static void RefTable(const char* id, const std::vector<Structure::Ref>& refs, const char* a, const char* b, const char* filter)
{
	if (!ImGui::BeginTable(id, 3, kTable)) return;
	ImGui::TableSetupColumn(a);
	ImGui::TableSetupColumn(b);
	ImGui::TableSetupColumn("관계", ImGuiTableColumnFlags_WidthFixed, 150);
	ImGui::TableHeadersRow();
	int rows = 0;
	for (auto& r : refs)
	{
		if (!ContainsNoCase(r.src + " " + r.dst, filter)) continue;
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::TextUnformatted(r.src.c_str());
		ImGui::TableNextColumn(); ImGui::TextUnformatted(r.dst.c_str());
		ImGui::TableNextColumn(); ImGui::TextUnformatted(r.type.c_str());
		++rows;
	}
	if (rows == 0) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("없음"); }
	ImGui::EndTable();
}

static void DrawStructure(App& app)
{
	const Structure& s = app.structure;
	const char* f = app.filter;
	SectionTitle("구조 (HTML 리포트의 다이어그램과 같은 데이터)");
	if (!s.loaded) { ImGui::TextDisabled("graph.json이 없습니다."); return; }
	if (ImGui::CollapsingHeader("클래스 계층"))
	{
		if (s.classRoots.empty()) ImGui::TextDisabled("프로젝트 클래스 없음");
		for (auto& root : s.classRoots) DrawClassNode(s, root, 0);
	}
	if (ImGui::CollapsingHeader("BP 실행 흐름"))
	{
		if (s.flows.empty()) ImGui::TextDisabled("실행 흐름 없음");
		for (int i = 0; i < (int)s.flows.size(); ++i)
		{
			const Flow& fl = s.flows[i];
			if (!ContainsNoCase(fl.title, f)) continue;
			ImGui::PushID(i);
			if (ImGui::TreeNodeEx("flow", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", fl.title.c_str()))
			{
				std::vector<std::string> path;
				for (int k = 0; k < (int)fl.starts.size(); ++k)
				{
					ImGui::PushID(k);
					DrawFlowNode(fl, fl.starts[k], "", path);
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}
	if (ImGui::CollapsingHeader("BP 호출 관계")) RefTable("calls", s.calls, "Blueprint", "호출 대상 클래스", f);
	if (ImGui::CollapsingHeader("에셋 참조")) RefTable("assets", s.assetRefs, "에셋", "참조 대상", f);
	if (ImGui::CollapsingHeader("모듈 · 플러그인 의존성")) RefTable("modules", s.moduleDeps, "모듈", "의존 대상", f);
}

// ============================================================================ result views

static void DrawAnalyzeResult(App& app, const json& d)
{
	const char* f = app.filter;
	const json& fin = Obj(d, "findings");
	if (!fin.empty())
	{
		SectionTitle("검증 게이트");
		std::string rel;
		for (auto& [k, v] : Obj(fin, "relationStatus").items()) rel += k + " " + v.dump() + " · ";
		if (rel.size() > 4) rel.resize(rel.size() - 4);
		ImGui::TextDisabled("관계 상태");
		ImGui::SameLine();
		ImGui::TextUnformatted(rel.empty() ? "-" : rel.c_str());
		const json& items = Arr(fin, "findings");
		for (int i = 0; i < (int)items.size(); ++i)
		{
			const json& item = items[i];
			ImGui::PushID(i);
			Badge(Str(item, "severity"));
			ImGui::SameLine();
			const json& list = Arr(item, "items");
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
			if (list.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			bool open = ImGui::TreeNodeEx("finding", flags, "%s  ·  %s%s", Str(item, "rule").c_str(), Str(item, "message").c_str(),
			                              list.empty() ? "" : "  (펼치기)");
			if (open && !list.empty())
			{
				StringList(list, f);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}

	const json& q = Obj(d, "quality");
	if (!q.empty())
	{
		SectionTitle("품질 점검");
		if (ImGui::CollapsingHeader("BP 복잡도", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("cx", 6, kTable))
		{
			for (const char* h : { "Blueprint", "그래프", "노드", "분기", "실행 체인", "기준 초과" }) ImGui::TableSetupColumn(h);
			ImGui::TableHeadersRow();
			for (auto& r : Arr(Obj(q, "complexity"), "graphs"))
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
				for (auto& o : Arr(r, "over")) over += o.get<std::string>() + " ";
				if (over.empty()) ImGui::TextDisabled("-"); else Badge(over);
			}
			ImGui::EndTable();
		}
		const json& violations = Arr(Obj(q, "naming"), "violations");
		std::string namingTitle = "이름 · 폴더 규칙 (" + std::to_string(violations.size()) + ")###naming";
		if (ImGui::CollapsingHeader(namingTitle.c_str()) && ImGui::BeginTable("nm", 3, kTable))
		{
			for (const char* h : { "에셋", "클래스", "문제" }) ImGui::TableSetupColumn(h);
			ImGui::TableHeadersRow();
			for (auto& v : violations)
			{
				std::string problems;
				for (auto& p : Arr(v, "problems")) problems += p.get<std::string>() + "  ";
				if (!ContainsNoCase(Str(v, "asset") + problems, f)) continue;
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(v, "asset").c_str());
				ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(v, "class").c_str());
				ImGui::TableNextColumn(); ImGui::TextWrapped("%s", problems.c_str());
			}
			ImGui::EndTable();
		}
		const json& unused = Obj(q, "unused");
		std::string unusedTitle = "미사용 후보 " + std::to_string(Arr(unused, "unused").size()) + " · 리다이렉터 " +
		                          std::to_string(Arr(unused, "redirectors").size()) + "###unused";
		if (ImGui::CollapsingHeader(unusedTitle.c_str()))
		{
			ImGui::TextDisabled("도달할 수 없는 에셋");
			StringList(Arr(unused, "unused"), f);
			ImGui::TextDisabled("리다이렉터");
			StringList(Arr(unused, "redirectors"), f);
		}
		const json& sm = Obj(q, "stateMonitor");
		if (sm.value("present", false) && ImGui::CollapsingHeader("StateMonitor 연계", ImGuiTreeNodeFlags_DefaultOpen))
		{
			KeyValue("플러그인", Str(sm, "plugin"));
			std::string ids;
			for (auto& s : Arr(sm, "sourceIds")) ids += s.get<std::string>() + "  ";
			KeyValue("Source ID", ids);
			KeyValue("테스트 필터", Str(sm, "testFilter"));
			if (ImGui::BeginTable("sm", 3, kTable))
			{
				for (const char* h : { "위치", "종류", "코드" }) ImGui::TableSetupColumn(h);
				ImGui::TableHeadersRow();
				for (auto& r : Arr(sm, "cppRefs"))
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::Text("%s:%lld", Str(r, "file").c_str(), Num(r, "line"));
					ImGui::TableNextColumn(); ImGui::TextUnformatted(r.value("submit", false) ? "제출" : "참조");
					ImGui::TableNextColumn(); ImGui::TextWrapped("%s", Str(r, "code").c_str());
				}
				for (auto& b : Arr(sm, "bpCalls"))
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
	DrawStructure(app);
}

static void DrawVerifyResult(App& app, const json& v)
{
	const json& steps = Arr(v, "steps");
	for (int i = 0; i < (int)steps.size(); ++i)
	{
		const json& step = steps[i];
		ImGui::PushID(i);
		Badge(Str(step, "verdict"));
		ImGui::SameLine();
		std::string title = Str(step, "id") + "  " + Str(step, "name") + "###step";
		if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			const json& summary = Obj(step, "summary");
			const json& automation = Obj(step, "automation");
			if (!summary.empty())
			{
				const json& c = Obj(summary, "counts");
				ImGui::Text("Error %lld · Warning %lld · Fatal %lld", Num(c, "Error"), Num(c, "Warning"), Num(c, "Fatal"));
				LogGroupsTable(summary, app.filter, "groups");
			}
			else if (!automation.empty())
			{
				const json& s = Obj(automation, "summary");
				ImGui::Text("성공 %lld · 경고 포함 %lld · 실패 %lld · 미실행 %lld  %s", Num(s, "succeeded"), Num(s, "succeededWithWarnings"),
				            Num(s, "failed"), Num(s, "notRun"), Str(step, "note").c_str());
				if (ImGui::BeginTable("tests", 3, kTable))
				{
					for (const char* h : { "테스트", "결과", "오류" }) ImGui::TableSetupColumn(h);
					ImGui::TableHeadersRow();
					for (auto& t : Arr(automation, "tests"))
					{
						if (!ContainsNoCase(Str(t, "name"), app.filter)) continue;
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(t, "name").c_str());
						ImGui::TableNextColumn(); Badge(Str(t, "state") == "Success" ? "pass" : "fail");
						ImGui::TableNextColumn();
						for (auto& e : Arr(t, "errors")) ImGui::TextWrapped("%s", e.get<std::string>().c_str());
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

// ============================================================================ HTML export

static bool PickSaveFile(HWND owner, const std::wstring& defaultName, fs::path& out)
{
	IFileSaveDialog* dialog = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return false;
	COMDLG_FILTERSPEC types[] = { { L"HTML 리포트", L"*.html" } };
	dialog->SetFileTypes(1, types);
	dialog->SetDefaultExtension(L"html");
	dialog->SetFileName(defaultName.c_str());
	dialog->SetTitle(L"HTML 리포트 저장");
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

static std::string ReadText(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string Between(const std::string& s, const std::string& open, const std::string& close)
{
	size_t a = s.find(open);
	size_t b = s.rfind(close);
	if (a == std::string::npos || b == std::string::npos || b < a) return {};
	return s.substr(a + open.size(), b - a - open.size());
}

static std::string HtmlEscape(const std::string& s)
{
	std::string o;
	for (char c : s)
	{
		if (c == '<') o += "&lt;";
		else if (c == '>') o += "&gt;";
		else if (c == '&') o += "&amp;";
		else if (c == '"') o += "&quot;";
		else o += c;
	}
	return o;
}

static void SaveRunReport(App& app, const RunInfo& r)
{
	fs::path src = r.dir / "out" / "report.html";
	std::error_code ec;
	if (!fs::exists(src, ec)) { app.message = "이 실행에는 report.html이 없습니다."; return; }
	fs::path dest;
	std::wstring name = ToWide(app.projectName + "_" + r.id + ".html");
	if (!PickSaveFile(g_hwnd, name, dest)) return;
	if (!CopyFileW(src.wstring().c_str(), dest.wstring().c_str(), FALSE))
	{
		app.message = "저장 실패: error " + std::to_string(GetLastError());
		return;
	}
	app.saved = dest;
}

// One self-contained page with the latest analyze, verify and logs reports. Returns an error message or "".
static std::string WriteBundleReport(App& app, const fs::path& dest)
{
	struct Part { const char* line; const char* title; const RunInfo* run; };
	Part parts[] = { { "architecture-report", "아키텍처 분석", nullptr }, { "verify", "검증", nullptr }, { "logs", "로그", nullptr } };
	for (auto& r : app.runs)
		for (auto& p : parts)
			if (!p.run && r.line == p.line && !r.verdict.empty() && r.verdict != "incomplete") p.run = &r;
	if (!parts[0].run && !parts[1].run && !parts[2].run) return "묶을 실행 기록이 없습니다.";

	std::string style, body, mermaid;
	std::string cover = "<section><div class='meta'>dwo-Lens 종합 리포트</div><h1>" + HtmlEscape(app.projectName) +
	                    "</h1><div class='scroll'><table><thead><tr><th>라인</th><th>실행 ID</th><th>판정</th></tr></thead><tbody>";
	for (auto& p : parts)
	{
		if (!p.run)
		{
			cover += std::string("<tr><td>") + p.title + "</td><td colspan='2'>실행 기록 없음</td></tr>";
			continue;
		}
		cover += std::string("<tr><td>") + p.title + "</td><td><code>" + HtmlEscape(p.run->id) + "</code></td><td><span class='badge " +
		         HtmlEscape(p.run->verdict) + "'>" + HtmlEscape(p.run->verdict) + "</span></td></tr>";
		std::string html = ReadText(p.run->dir / "out" / "report.html");
		if (style.empty()) style = Between(html, "<style>", "</style>");
		std::string main = Between(html, "<main>", "</main>");
		body += std::string("<section><h1>") + p.title + "</h1><div class='meta'>" + HtmlEscape(p.run->id) + "</div></section>" + main;
		if (mermaid.empty() && html.find("mermaid.min.js") != std::string::npos)
		{
			size_t at = html.find("<script src=");
			size_t end = html.rfind("</script>");
			if (at != std::string::npos && end != std::string::npos) mermaid = html.substr(at, end + 9 - at);
		}
	}
	cover += "</tbody></table></div></section>";
	std::string page = "<!doctype html><html lang='ko'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
	                   "<title>" + HtmlEscape(app.projectName) + " · dwo-Lens 종합 리포트</title><style>" + style +
	                   "</style></head><body><main>" + cover + body + "</main>" + mermaid + "</body></html>";
	std::ofstream out(dest, std::ios::binary);
	out << page;
	if (!out) return "저장 실패: " + PathUtf8(dest);
	return "";
}

static void SaveBundleReport(App& app)
{
	fs::path dest;
	if (!PickSaveFile(g_hwnd, ToWide(app.projectName + "_dwo-lens_report.html"), dest)) return;
	std::string err = WriteBundleReport(app, dest);
	if (err.empty()) app.saved = dest;
	else app.message = err;
}

static void DrawSavedNotice(App& app)
{
	if (app.saved.empty()) return;
	ImGui::PushStyleColor(ImGuiCol_Text, VerdictColor("pass"));
	ImGui::TextWrapped("저장됨: %s", PathUtf8(app.saved).c_str());
	ImGui::PopStyleColor();
	ImGui::SameLine();
	if (ImGui::SmallButton("열기")) OpenWithShell(app.saved);
	ImGui::SameLine();
	if (ImGui::SmallButton("폴더")) OpenWithShell(app.saved.parent_path());
	ImGui::SameLine();
	if (ImGui::SmallButton("닫기")) app.saved.clear();
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
	if (ImGui::Button("HTML 저장...")) SaveRunReport(app, r);
	ImGui::SameLine();
	if (ImGui::Button("종합 리포트 저장...")) SaveBundleReport(app);
	ImGui::SameLine();
	if (ImGui::Button("폴더 열기")) OpenWithShell(r.dir);
	ImGui::SetNextItemWidth(320);
	ImGui::InputTextWithHint("##filter", "필터 (에셋, 규칙, 메시지, 클래스...)", app.filter, sizeof(app.filter));
	ImGui::SameLine();
	ImGui::TextDisabled("다이어그램 원본은 '리포트 열기'로 브라우저에서 볼 수 있습니다.");
	DrawSavedNotice(app);

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
	if (r.line == "verify") DrawVerifyResult(app, Obj(d, "verify"));
	else if (r.line == "logs")
	{
		const json& logs = Obj(d, "logs");
		const json& c = Obj(logs, "counts");
		ImGui::Text("Fatal %lld · Error %lld · Warning %lld · 새 그룹 %lld", Num(c, "Fatal"), Num(c, "Error"), Num(c, "Warning"), Num(logs, "newGroups"));
		LogGroupsTable(logs, app.filter, "loggroups");
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
	if (app.runner.ConsumeFinished())
	{
		app.Refresh();
		bool isLine = app.lastCommand == "analyze" || app.lastCommand == "verify" || app.lastCommand == "logs";
		if (isLine && !app.runs.empty())
		{
			app.selectedRun = 0;
			app.latestReady = true;
		}
	}

	ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::Begin("dwo-Lens", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
	                                   ImGuiWindowFlags_NoBringToFrontOnFocus);

	// ---- project bar
	ImGui::TextUnformatted("dwo-Lens");
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
		ImGui::TextUnformatted("dwo-Lens를 이 프로젝트에서 제거합니다.");
		ImGui::TextDisabled("플러그인, exclude 블록, __dwo-lens 폴더(분석 기록 포함)가 삭제됩니다.");
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
	app.settingsDir = fs::path(local) / L"dwo-Lens";
	app.LoadSettings();

	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::wstring projectArg = ArgValue(argc, argv, L"--project");
	std::wstring homeArg = ArgValue(argc, argv, L"--home");
	std::wstring tabArg = ArgValue(argc, argv, L"--tab");
	std::wstring runArg = ArgValue(argc, argv, L"--run");
	std::wstring selectArg = ArgValue(argc, argv, L"--select");
	std::wstring exportArg = ArgValue(argc, argv, L"--export-report");
	LocalFree(argv);
	if (homeArg.empty())
	{
		wchar_t buf[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"DWO_LENS_HOME", buf, MAX_PATH)) homeArg = buf;
	}
	app.home = homeArg;

	// Headless export: write the combined HTML report and exit without opening a window.
	if (!exportArg.empty())
	{
		app.project = projectArg.empty() ? fs::current_path() : fs::path(projectArg);
		app.Refresh();
		std::string err = app.hasUProject ? WriteBundleReport(app, exportArg) : "no .uproject in " + PathUtf8(app.project);
		CoUninitialize();
		return err.empty() ? 0 : 1;
	}

	WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"DwoLensGui", nullptr };
	RegisterClassExW(&wc);
	HWND hwnd = g_hwnd = CreateWindowW(wc.lpszClassName, L"dwo-Lens", WS_OVERLAPPEDWINDOW, 100, 80, 1360, 860, nullptr, nullptr, wc.hInstance, nullptr);
	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		MessageBoxW(nullptr, L"DirectX 11 장치를 만들 수 없습니다.", L"dwo-Lens", MB_ICONERROR);
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
	if (!selectArg.empty())
		for (int i = 0; i < (int)app.runs.size(); ++i)
			if (app.runs[i].id == ToUtf8(selectArg)) { app.selectedRun = i; app.requestTab = 2; }
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
