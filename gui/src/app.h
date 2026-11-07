// Application model: project, run history, per-run results and metrics, problems, settings, live link.
#pragma once

#include "graph.h"
#include "live.h"

// ============================================================================ lines

struct LineDef
{
	const char* line;       // run.json "line"
	const char* prefix;     // run id prefix
	const char* command;    // __dwo-lens.bat command
	const char* name;
	const char* icon;
	const char* desc;
	bool engine;            // runs the engine (close the editor for verify)
};

static const LineDef kLines[] = {
	{ "architecture-report", "run", "analyze", "아키텍처 분석", ICON_LAYERS, "리플렉션·BP 그래프·에셋 참조 수집, 통합 그래프, 품질 점검", true },
	{ "verify", "verify", "verify", "검증", ICON_CHECKLIST, "BP 전체 컴파일, 데이터 검증, 자동화 테스트. 에디터를 닫고 실행하세요.", true },
	{ "logs", "logs", "logs", "로그 분석", ICON_LIST, "Saved/Logs·크래시 분석, 이전 실행 대비 새 오류", false },
	{ "smoke", "smoke", "smoke", "맵 스모크", ICON_PLAY, "맵마다 게임을 띄워 오류·ensure·크래시 수집", true },
	{ "perf", "perf", "perf", "성능 캡처", ICON_SPEED, "CSV 프로파일러로 프레임·스레드 시간과 히치 측정", true },
	{ "sizes", "sizes", "sizes", "콘텐츠 용량", ICON_PACKAGE, "에셋 용량, 폴더·유형별 분포, 하드 참조로 함께 로드되는 무게", false },
	{ "trace", "trace", "trace", "Insights 트레이스", ICON_DIAG, "Unreal Insights 트레이스 캡처", true },
};
static const int kLineCount = (int)(sizeof(kLines) / sizeof(kLines[0]));

static int LineIndexOf(const std::string& line, const std::string& id)
{
	for (int i = 0; i < kLineCount; ++i)
		if (line == kLines[i].line || line == kLines[i].command) return i;
	std::string prefix = id.substr(0, id.find('-'));
	for (int i = 0; i < kLineCount; ++i)
		if (prefix == kLines[i].prefix) return i;
	return -1;
}

// ============================================================================ structure model (graph.json and flows/*.mmd)

struct ClassNode
{
	std::string name;
	std::string kind;
	bool external = false;
	std::vector<std::string> props;
	std::vector<std::string> funcs;
	std::vector<std::string> children;
	std::string parent;
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
	std::vector<std::string> order;
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
	int symbols = 0, relations = 0;
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
		f.order.push_back(id);
	}
	for (auto& id : f.order)
		if (!hasIncoming.count(id)) f.starts.push_back(id);
	return f;
}

static void LoadStructure(const fs::path& outDir, Structure& s)
{
	s = Structure();
	auto graph = ReadJson(outDir / "graph.json");
	if (!graph) return;
	std::map<std::string, std::string> names;
	s.symbols = (int)Arr(*graph, "symbols").size();
	s.relations = (int)Arr(*graph, "relations").size();
	for (auto& sym : Arr(*graph, "symbols"))
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
		for (auto& p : Arr(sym, "properties")) n.props.push_back(Str(p, "type") + " " + Str(p, "name"));
		for (auto& fn : Arr(sym, "functions")) n.funcs.push_back(fn.is_string() ? fn.get<std::string>() : fn.dump());
	}
	auto nameOf = [&names](const std::string& key) {
		auto it = names.find(key);
		return it != names.end() ? it->second : key.substr(key.find(':') + 1);
	};
	std::set<std::string> hasParent;
	for (auto& r : Arr(*graph, "relations"))
	{
		std::string type = Str(r, "type"), src = Str(r, "source"), dst = Str(r, "target");
		if (type == "Inherits")
		{
			if (!s.classes.count(dst)) { s.classes[dst].name = nameOf(dst); s.classes[dst].external = true; }
			s.classes[dst].children.push_back(src);
			s.classes[src].parent = dst;
			hasParent.insert(src);
		}
		else if (type == "Calls")
			s.calls.push_back({ nameOf(src), nameOf(dst), std::to_string(Num(r, "count")) });
		else if (type == "AssetHardRef" || type == "AssetSoftRef")
			s.assetRefs.push_back({ nameOf(src), nameOf(dst), type == "AssetHardRef" ? "하드" : "소프트" });
		else if (type == "DependsOnModule" || type == "DependsOnPlugin")
			s.moduleDeps.push_back({ nameOf(src), nameOf(dst), type == "DependsOnModule" ? "모듈" : "플러그인" });
	}
	std::map<std::string, int> memo;
	std::function<bool(const std::string&, int)> hasProject = [&](const std::string& key, int depth) {
		auto m = memo.find(key);
		if (m != memo.end()) return m->second == 1;
		const ClassNode& n = s.classes[key];
		bool r = !n.external;
		if (!r && depth < 40)
			for (auto& c : n.children)
				if (hasProject(c, depth + 1)) { r = true; break; }
		memo[key] = r ? 1 : 0;
		return r;
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

// ============================================================================ runs

struct RunInfo
{
	std::string id;
	std::string line;
	int lineIndex = -1;
	std::string verdict;
	std::string startedAt;
	double seconds = 0;
	fs::path dir;
	json run;
};

// Numbers extracted once per run for KPIs, trends and comparisons.
struct RunMetrics
{
	bool loaded = false;
	int findingsFail = 0, findingsWarn = 0, findingsInfo = 0;
	int bpOver = 0, naming = 0, unused = 0, redirectors = 0;
	int testsPass = 0, testsFail = 0, testsTotal = 0;
	int logFatal = 0, logError = 0, logWarning = 0, logGroups = 0, logNew = 0;
	int mapsPass = 0, mapsFail = 0, mapsTotal = 0;
	double perfP95 = -1, perfAvg = -1;
	double contentBytes = -1;
	std::map<std::string, char> tests;   // test name -> 'p' / 'f'
};

struct Problem
{
	std::string key;        // stable identity for mute / new detection
	std::string severity;
	std::string category;
	int line = -1;
	std::string title;
	std::string object;
	std::string runId;
	const json* detail = nullptr;
	bool isNew = false;
	bool muted = false;
};

enum class Activity
{
	Overview,
	Architecture,
	Problems,
	Tests,
	Logs,
	Performance,
	Live,
	History,
	Tools,
	Count
};

struct LiveSettings
{
	int port = 47731;
	bool autoObserve = true;
	bool engineProps = false;
	float interval = 0.25f;
	std::vector<std::string> watch;
};

struct App
{
	fs::path settingsDir;
	fs::path home;
	fs::path project;
	std::string projectName;
	bool hasUProject = false;
	bool attached = false;
	bool isCodeProject = false;
	int stateMonitor = 0;        // 0 none, 1 installed by dwo-Lens, 2 part of the project
	std::string stateMonitorMode;
	json state;
	json catalog;
	std::vector<RunInfo> runs;
	std::vector<std::string> recent;
	std::set<std::string> mutes;
	Runner runner;
	std::deque<std::string> queue;   // commands waiting for the runner
	std::string lastCommand;
	std::string message;
	fs::path saved;
	bool confirmDetach = false;
	Activity activity = Activity::Overview;
	Activity requestActivity = Activity::Count;
	bool showConsole = false;        // focus the run log
	bool forgetAfterUninstall = false;
	LiveServer live;
	LiveStore store;
	LiveSettings liveSettings;

	// ---- caches
	std::map<std::string, json> outputs;       // run id -> {"findings": ..., "quality": ..., ...}
	std::map<std::string, RunMetrics> metrics;
	Structure structure;
	std::string structureFor;
	std::vector<Problem> problems;
	long long problemsRevision = -1;
	long long runsRevision = 0;

	// ---------------------------------------------------------------- settings

	void LoadSettings()
	{
		if (auto s = ReadJson(settingsDir / "gui.json"))
		{
			for (auto& r : Arr(*s, "recent")) if (r.is_string()) recent.push_back(r.get<std::string>());
			const json& l = Obj(*s, "live");
			liveSettings.port = (int)l.value("port", 47731);
			liveSettings.autoObserve = l.value("auto", true);
			liveSettings.engineProps = l.value("engine", false);
			liveSettings.interval = l.value("interval", 0.25f);
			for (auto& w : Arr(l, "watch")) if (w.is_string()) liveSettings.watch.push_back(w.get<std::string>());
		}
	}

	void SaveSettings()
	{
		json s;
		s["recent"] = recent;
		s["live"] = { { "port", liveSettings.port }, { "auto", liveSettings.autoObserve }, { "engine", liveSettings.engineProps },
		              { "interval", liveSettings.interval }, { "watch", liveSettings.watch } };
		std::error_code ec;
		fs::create_directories(settingsDir, ec);
		std::ofstream(settingsDir / "gui.json", std::ios::binary) << s.dump(1);
	}

	// Per-project GUI settings live inside the project's __dwo-lens folder, so uninstall removes them too.
	fs::path ProjectSettingsFile() const { return project / "__dwo-lens" / "gui.json"; }

	// The dwo-Lens install folder (where tools\lens.ps1 is).
	bool HomeValid() const
	{
		std::error_code ec;
		return !home.empty() && fs::exists(home / "tools" / "lens.ps1", ec);
	}

	void LoadProjectSettings()
	{
		mutes.clear();
		if (auto s = ReadJson(ProjectSettingsFile()))
			for (auto& m : Arr(*s, "mutes")) if (m.is_string()) mutes.insert(m.get<std::string>());
	}

	void SaveProjectSettings()
	{
		json s;
		s["project"] = PathUtf8(project);
		s["mutes"] = std::vector<std::string>(mutes.begin(), mutes.end());
		std::error_code ec;
		problemsRevision = -1;
		if (!fs::is_directory(project / "__dwo-lens", ec)) return;   // not attached: nothing to keep
		std::ofstream(ProjectSettingsFile(), std::ios::binary) << s.dump(1);
	}

	// ---------------------------------------------------------------- project

	void SetProject(const fs::path& path)
	{
		project = path;
		std::string u = PathUtf8(path);
		recent.erase(std::remove(recent.begin(), recent.end(), u), recent.end());
		recent.insert(recent.begin(), u);
		if (recent.size() > 10) recent.resize(10);
		SaveSettings();
		outputs.clear();
		metrics.clear();
		structureFor.clear();
		LoadProjectSettings();
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
		stateMonitorMode.clear();
		fs::path own = project / "Plugins" / "__StateMonitor" / "StateMonitor.uplugin";
		if (fs::exists(own, ec))
		{
			stateMonitor = 1;
			std::string text = ReadText(own);
			stateMonitorMode = text.find("TargetAllowList") != std::string::npos ? "editor" : "runtime";
		}
		else if (fs::is_directory(project / "Plugins", ec))
			for (auto it = fs::recursive_directory_iterator(project / "Plugins", ec); it != fs::recursive_directory_iterator(); it.increment(ec))
			{
				if (ec) break;
				if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
				if (it->path().filename() == "StateMonitor.uplugin" && PathUtf8(it->path()).find("__") == std::string::npos) { stateMonitor = 2; break; }
			}
		auto st = ReadJson(project / "__dwo-lens" / "state.json");
		attached = st.has_value();
		state = st ? *st : json::object();
		if (auto c = ReadJson(home / "tools" / "catalog.json")) catalog = *c;
		LoadRuns();
	}

	void LoadRuns()
	{
		runs.clear();
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
					r.startedAt = Str(*j, "startedAt");
					for (auto& s : Arr(*j, "stations")) r.seconds += Num(s, "ms") / 1000.0;
				}
				else
				{
					r.line = "-";
					r.verdict = "incomplete";
				}
				r.lineIndex = LineIndexOf(r.line, r.id);
				runs.push_back(std::move(r));
			}
		std::sort(runs.begin(), runs.end(), [](const RunInfo& a, const RunInfo& b) { return SortKeyOf(a.id) > SortKeyOf(b.id); });
		++runsRevision;
		problemsRevision = -1;
	}

	const RunInfo* FindRun(const std::string& id) const
	{
		for (auto& r : runs)
			if (r.id == id) return &r;
		return nullptr;
	}

	// Newest completed run of a line, skipping `skip` newer ones.
	const RunInfo* Latest(int line, int skip = 0) const
	{
		for (auto& r : runs)
			if (r.lineIndex == line && r.verdict != "incomplete" && skip-- <= 0) return &r;
		return nullptr;
	}

	std::vector<const RunInfo*> RunsOf(int line, int max) const
	{
		std::vector<const RunInfo*> out;
		for (auto& r : runs)
			if (r.lineIndex == line && r.verdict != "incomplete")
			{
				out.push_back(&r);
				if ((int)out.size() >= max) break;
			}
		std::reverse(out.begin(), out.end());   // oldest first
		return out;
	}

	const json& Outputs(const RunInfo& r)
	{
		auto it = outputs.find(r.id);
		if (it != outputs.end()) return it->second;
		json d = json::object();
		std::error_code ec;
		fs::path out = r.dir / "out";
		if (fs::is_directory(out, ec))
			for (auto& e : fs::directory_iterator(out, ec))
			{
				if (e.path().extension() != ".json") continue;
				std::string name = PathUtf8(e.path().stem());
				if (name == "graph") continue;   // large; the structure view loads it separately
				std::error_code sec;
				if (fs::file_size(e.path(), sec) > 64ull * 1024 * 1024) continue;
				if (auto j = ReadJson(e.path())) d[name] = std::move(*j);
			}
		if (outputs.size() > 48) outputs.erase(outputs.begin());
		return outputs[r.id] = std::move(d);
	}

	const RunMetrics& Metrics(const RunInfo& r)
	{
		auto it = metrics.find(r.id);
		if (it != metrics.end()) return it->second;
		RunMetrics m;
		const json& d = Outputs(r);
		const json& fin = Obj(d, "findings");
		for (auto& f : Arr(fin, "findings"))
		{
			std::string s = Str(f, "severity");
			if (s == "fail") ++m.findingsFail;
			else if (s == "warn") ++m.findingsWarn;
			else ++m.findingsInfo;
		}
		const json& q = Obj(d, "quality");
		for (auto& g : Arr(Obj(q, "complexity"), "graphs")) if (!Arr(g, "over").empty()) ++m.bpOver;
		m.naming = (int)Arr(Obj(q, "naming"), "violations").size();
		m.unused = (int)Arr(Obj(q, "unused"), "unused").size();
		m.redirectors = (int)Arr(Obj(q, "unused"), "redirectors").size();
		for (auto& step : Arr(Obj(d, "verify"), "steps"))
			for (auto& t : Arr(Obj(step, "automation"), "tests"))
			{
				bool ok = Str(t, "state") == "Success";
				m.tests[Str(t, "name")] = ok ? 'p' : 'f';
				++m.testsTotal;
				(ok ? m.testsPass : m.testsFail)++;
			}
		const json& logs = Obj(d, "logs");
		const json& c = Obj(logs, "counts");
		m.logFatal = (int)Num(c, "Fatal");
		m.logError = (int)Num(c, "Error");
		m.logWarning = (int)Num(c, "Warning");
		m.logGroups = (int)Arr(logs, "groups").size();
		m.logNew = (int)Num(logs, "newGroups");
		for (auto& map : Arr(Obj(d, "smoke"), "maps"))
		{
			++m.mapsTotal;
			(Str(map, "verdict") == "fail" ? m.mapsFail : m.mapsPass)++;
		}
		const json& ft = Obj(Obj(Obj(d, "perf"), "stats"), "FrameTime");
		m.perfP95 = Real(ft, "p95", -1);
		m.perfAvg = Real(ft, "avg", -1);
		m.contentBytes = Real(Obj(d, "sizes"), "totalBytes", -1);
		m.loaded = true;
		return metrics[r.id] = std::move(m);
	}

	const Structure& StructureOf(const RunInfo& r)
	{
		if (structureFor != r.id)
		{
			LoadStructure(r.dir / "out", structure);
			structureFor = r.id;
		}
		return structure;
	}

	// ---------------------------------------------------------------- problems

	static void CollectProblems(App& app, const RunInfo& r, std::vector<Problem>& out)
	{
		const json& d = app.Outputs(r);
		auto add = [&](std::string key, std::string severity, std::string category, std::string title, std::string object, const json* detail) {
			Problem p;
			p.key = std::move(key);
			severity = Lower(severity);
			if (severity == "warning") severity = "warn";
			if (severity == "fail") severity = "error";
			p.severity = std::move(severity);
			p.category = std::move(category);
			p.line = r.lineIndex;
			p.title = std::move(title);
			p.object = std::move(object);
			p.runId = r.id;
			p.detail = detail;
			out.push_back(std::move(p));
		};
		for (auto& f : Arr(Obj(d, "findings"), "findings"))
		{
			if (Str(f, "severity") == "info") continue;
			add("finding:" + Str(f, "rule"), Str(f, "severity"), "아키텍처", Str(f, "message"), Str(f, "rule"), &f);
		}
		const json& q = Obj(d, "quality");
		for (auto& g : Arr(Obj(q, "complexity"), "graphs"))
			if (!Arr(g, "over").empty())
				add("complexity:" + Str(g, "blueprint") + "/" + Str(g, "graph"), "warn", "BP 복잡도",
				    "기준 초과: " + JoinStrings(Arr(g, "over")), Str(g, "blueprint") + " · " + Str(g, "graph"), &g);
		for (auto& v : Arr(Obj(q, "naming"), "violations"))
			add("naming:" + Str(v, "asset"), "minor", "이름 규칙", JoinStrings(Arr(v, "problems"), " · "), Str(v, "asset"), &v);
		for (auto& a : Arr(Obj(q, "unused"), "redirectors"))
			add("redirector:" + (a.is_string() ? a.get<std::string>() : a.dump()), "minor", "리다이렉터", "정리되지 않은 리다이렉터",
			    a.is_string() ? a.get<std::string>() : a.dump(), &a);
		for (auto& a : Arr(Obj(q, "unused"), "unused"))
			add("unused:" + (a.is_string() ? a.get<std::string>() : a.dump()), "info", "미사용 후보", "어디에서도 참조되지 않는 에셋",
			    a.is_string() ? a.get<std::string>() : a.dump(), &a);
		for (auto& step : Arr(Obj(d, "verify"), "steps"))
		{
			for (auto& t : Arr(Obj(step, "automation"), "tests"))
				if (Str(t, "state") != "Success")
					add("test:" + Str(t, "name"), "error", "테스트 실패", Arr(t, "errors").empty() ? "실패" : JoinStrings(Arr(t, "errors"), " / "), Str(t, "name"), &t);
			for (auto& g : Arr(Obj(step, "summary"), "groups"))
			{
				std::string sev = Str(g, "severity");
				if (sev != "Error" && sev != "Fatal") continue;
				add("verifylog:" + Str(step, "id") + ":" + Str(g, "fingerprint"), sev, Str(step, "name"), Str(g, "sample"), Str(g, "category"), &g);
			}
		}
		for (auto& g : Arr(Obj(d, "logs"), "groups"))
		{
			std::string sev = Str(g, "severity");
			if (sev != "Error" && sev != "Fatal" && sev != "Warning") continue;
			add("log:" + Str(g, "fingerprint"), sev, "로그", Str(g, "sample"), Str(g, "category"), &g);
		}
		for (auto& c : Arr(Obj(d, "logs"), "crashes"))
			add("crash:" + Str(c, "id", Str(c, "dir")), "fatal", "크래시", Str(c, "error", Str(c, "message", "크래시")), Str(c, "dir"), &c);
		for (auto& map : Arr(Obj(d, "smoke"), "maps"))
			if (Str(map, "verdict") != "pass")
				add("smoke:" + Str(map, "map"), Str(map, "verdict"), "맵 스모크",
				    Arr(map, "reasons").empty() ? Fmt("오류 %lld · 경고 %lld · ensure %lld", Num(map, "errors"), Num(map, "warnings"), Num(map, "ensures"))
				                                : JoinStrings(Arr(map, "reasons"), " · "),
				    Str(map, "map"), &map);
		const json& perf = Obj(d, "perf");
		if (!perf.empty() && Str(perf, "verdict") != "pass")
			add("perf:" + Str(perf, "map"), Str(perf, "verdict"), "성능",
			    Arr(perf, "reasons").empty() ? Fmt("p95 %.1fms (예산 %.1fms)", Real(Obj(Obj(perf, "stats"), "FrameTime"), "p95"), Real(perf, "budgetMs"))
			                                 : JoinStrings(Arr(perf, "reasons"), " · "),
			    Str(perf, "map"), &perf);
		const json& sizes = Obj(d, "sizes");
		for (auto& w : Arr(sizes, "overLimit"))
			add("size:" + Str(w, "asset", w.dump()), "warn", "콘텐츠 용량", Fmt("%s · 기준 %lld MB 초과", Bytes(Real(w, "bytes")).c_str(), Num(sizes, "warnAssetMB")),
			    Str(w, "asset"), &w);
		const json& tr = Obj(d, "trace");
		if (!tr.empty() && Str(tr, "verdict") != "pass")
			add("trace:" + r.id, Str(tr, "verdict"), "트레이스", JoinStrings(Arr(tr, "reasons"), " · "), Str(tr, "file"), &tr);
	}

	void RebuildProblems()
	{
		if (problemsRevision == runsRevision) return;
		problems.clear();
		for (int line = 0; line < kLineCount; ++line)
		{
			const RunInfo* latest = Latest(line);
			if (!latest) continue;
			std::vector<Problem> now, before;
			CollectProblems(*this, *latest, now);
			std::set<std::string> previous;
			if (const RunInfo* prev = Latest(line, 1))
			{
				CollectProblems(*this, *prev, before);
				for (auto& p : before) previous.insert(p.key);
			}
			for (auto& p : now)
			{
				p.isNew = Latest(line, 1) && !previous.count(p.key);
				p.muted = mutes.count(p.key) > 0;
				problems.push_back(std::move(p));
			}
		}
		std::stable_sort(problems.begin(), problems.end(), [](const Problem& a, const Problem& b) {
			if (a.isNew != b.isNew) return a.isNew;
			return SeverityRank(a.severity) > SeverityRank(b.severity);
		});
		problemsRevision = runsRevision;
	}

	// ---------------------------------------------------------------- commands

	// Runs a dwo-Lens command (same as the command line) straight from the install folder.
	bool Run(const std::string& command)
	{
		if (runner.running)
		{
			queue.push_back(command);
			PushToast("대기열에 추가", command + " (앞의 작업이 끝나면 실행)", col::Info);
			return true;
		}
		if (!HomeValid())
		{
			message = "dwo-Lens 폴더를 찾을 수 없습니다: " + PathUtf8(home) + ". dwo-Lens 폴더의 dwo-Lens.bat으로 다시 여세요.";
			PushToast("실행할 수 없음", message, col::Fail);
			return false;
		}
		SetEnvironmentVariableW(L"DWO_LENS_HOME", home.wstring().c_str());
		SetEnvironmentVariableW(L"DWO_LENS_FROM_BAT", nullptr);
		std::string verb = command.substr(0, command.find(' '));
		std::string rest = command.size() > verb.size() ? command.substr(verb.size()) : "";
		std::wstring ps = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + (home / "tools" / "lens.ps1").wstring() +
		                  L"\" -Command " + ToWide(verb) + L" -Project \"" + project.wstring() + L"\"" + ToWide(rest);
		runner.logDir = settingsDir / "logs";
		if (runner.Start(ps, project, "dwo-lens " + command))
		{
			message.clear();
			lastCommand = command;
			return true;
		}
		return false;
	}

	void RunAll()
	{
		for (const char* c : { "analyze", "logs", "sizes" }) Run(c);
	}

	// Called every frame: finished commands, queue, live messages.
	void Tick()
	{
		if (runner.ConsumeFinished())
		{
			std::string done = lastCommand;
			int code = runner.exitCode;
			Refresh();
			int line = -1;
			for (int i = 0; i < kLineCount; ++i)
				if (done == kLines[i].command) line = i;
			if (line >= 0 && !runs.empty() && runs.front().lineIndex == line)
			{
				const RunInfo& r = runs.front();
				ImU32 c = SemanticColor(r.verdict);
				const char* label = VerdictLabel(r.verdict);
				Activity target = ActivityOfLine(line);
				PushToast(std::string(kLines[line].name) + " 완료 · " + (label ? label : r.verdict), Duration(r.seconds) + " · " + r.id, c,
				          [this, target] { requestActivity = target; }, "결과 보기");
			}
			else
			{
				fs::path logFile = runner.logFile;
				std::string verb = done.substr(0, done.find(' '));
				std::string what = verb == "attach" ? "부착" : verb == "uninstall" ? "제거" : verb == "update" ? "업데이트" : verb;
				std::string body = code == 0 ? "완료" : "실패 (종료 코드 " + std::to_string(code) + ") · " + runner.LastLine();
				if (code == 0 && verb == "uninstall" && forgetAfterUninstall)
				{
					recent.erase(std::remove(recent.begin(), recent.end(), PathUtf8(project)), recent.end());
					SaveSettings();
				}
				forgetAfterUninstall = false;
				PushToast(what + (code == 0 ? " 완료" : " 실패"), body, code == 0 ? col::Pass : col::Fail,
				          [logFile] { if (!logFile.empty()) ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + logFile.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL); },
				          "로그 파일 열기");
				if (code != 0) showConsole = true;
			}
			if (!queue.empty())
			{
				std::string next = queue.front();
				queue.pop_front();
				Run(next);
			}
		}
		std::vector<LiveMessage> messages;
		live.Drain(messages);
		for (auto& m : messages)
		{
			bool hello = store.Apply(m);
			if (hello || m.connected) SendLiveSettings(m.client);
		}
	}

	static Activity ActivityOfLine(int line)
	{
		switch (line)
		{
		case 0: return Activity::Architecture;
		case 1: return Activity::Tests;
		case 2: return Activity::Logs;
		case 3: return Activity::Tests;
		default: return Activity::Performance;
		}
	}

	void SendLiveSettings(int client)
	{
		json autoMsg = { { "t", "auto" }, { "on", liveSettings.autoObserve }, { "interval", liveSettings.interval }, { "engine", liveSettings.engineProps } };
		live.Send(client, autoMsg);
		for (auto& w : liveSettings.watch) live.Send(client, { { "t", "watch" }, { "class", w }, { "max", 16 } });
	}

	void BroadcastLiveSettings()
	{
		live.Broadcast({ { "t", "auto" }, { "on", liveSettings.autoObserve }, { "interval", liveSettings.interval }, { "engine", liveSettings.engineProps } });
		SaveSettings();
	}

	std::string SystemCommit() const { return Str(state, "systemCommit"); }
	std::string Engine() const { return Str(state, "engineVersion"); }
};
