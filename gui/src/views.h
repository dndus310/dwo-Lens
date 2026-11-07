// Views for the run-based activities: overview, architecture, problems, tests, logs, performance, history, tools.
// Each activity draws into three docked windows: navigator (left), main (center) and inspector (right).
#pragma once

#include "app.h"

struct UiState
{
	char filter[256] = {};
	// architecture
	int archMode = 0;              // 0 classes, 1 flows, 2 calls, 3 assets, 4 modules, 5 quality
	std::string archSel;           // class key / flow title / asset name / module
	std::string archRun;
	std::string archNode;          // selected graph node id
	GraphView archGraph;
	std::string archGraphKey;
	// problems
	std::string problemSel;
	int groupBy = 0;               // 0 none, 1 category, 2 severity, 3 line
	bool showMuted = false;
	bool onlyNew = false;
	std::set<std::string> hiddenSeverities;
	std::string categoryFilter;
	// tests
	int testsTab = 0;
	int testFilter = 0;            // 0 all, 1 failed, 2 passed, 3 flaky
	std::string testSel, testPrefix, smokeSel;
	// logs
	std::string logSel;
	std::string logSeverity;
	int logSort = 0;
	// performance
	int perfTab = 0;
	std::string perfRun, sizesRun, traceRun;
	std::string sizesSel;
	int hitchSel = -1;
	// history
	int historyLine = -1;
	std::string historySel, compareA, compareB;
	// tools
	std::string toolSel;
	std::string toolCategory;
	// live
	int liveSession = 0;
	std::string liveSource;
	std::string liveField;
	int liveMode = 0;              // 0 objects, 1 state machine
	double playhead = -1;          // <0 follows live
	double timelineSpan = 30;
	double timelineEnd = -1;
	bool liveFollowGraph = true;
	char watchInput[128] = {};
	char liveFilter[128] = {};
	GraphView liveGraph;
	GraphView machineGraph;
	long long liveGraphRevision = -1;
	std::string liveGraphKey;
	std::string inspectorPath;     // property path selected in the live inspector
	// console
	char consoleFilter[128] = {};
	bool consoleFollow = true;
	size_t consoleSeen = 0;
};
static UiState ui;

static const RunInfo* PickRunCombo(App& app, const char* id, int line, std::string& selected)
{
	const RunInfo* current = selected.empty() ? app.Latest(line) : app.FindRun(selected);
	if (!current) current = app.Latest(line);
	std::string preview = current ? StampOf(current->id) + "  " + (VerdictLabel(current->verdict) ? VerdictLabel(current->verdict) : current->verdict) : "실행 없음";
	ImGui::SetNextItemWidth(S(190));
	if (ImGui::BeginCombo(id, preview.c_str()))
	{
		for (auto& r : app.runs)
		{
			if (r.lineIndex != line) continue;
			std::string label = StampOf(r.id) + "  " + (VerdictLabel(r.verdict) ? VerdictLabel(r.verdict) : r.verdict) + "##" + r.id;
			if (ImGui::Selectable(label.c_str(), current && current->id == r.id)) selected = r.id;
		}
		ImGui::EndCombo();
	}
	return current;
}

// Header strip at the top of the main window: title, subtitle and right-aligned content via callback.
static void ViewHeader(const char* icon, const std::string& title, const std::string& subtitle)
{
	ImGui::PushFont(g_fonts.title);
	ImGui::TextUnformatted((std::string(icon) + "  " + title).c_str());
	ImGui::PopFont();
	if (!subtitle.empty())
	{
		ImGui::SameLine();
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
		TextDim(subtitle);
	}
}

static void RunLineButton(App& app, int line, bool primary = true, const char* label = nullptr)
{
	std::string text = label ? std::string(label) : std::string(ICON_PLAY) + "  " + kLines[line].name + " 실행";
	text += "##run" + std::to_string(line);
	ImGui::BeginDisabled(!app.attached);
	bool clicked = primary ? PrimaryButton(text.c_str()) : ImGui::Button(text.c_str());
	ImGui::EndDisabled();
	Tooltip(app.attached ? kLines[line].desc : "먼저 프로젝트에 부착하세요.");
	if (clicked) app.Run(kLines[line].command);
}

static std::string VerdictStripOf(App& app, int line, int count)
{
	std::string s;
	for (auto* r : app.RunsOf(line, count)) s += r->verdict == "pass" ? 'p' : r->verdict == "fail" ? 'f' : r->verdict == "warn" ? 'w' : '-';
	return s;
}

static std::vector<float> TrendOf(App& app, int line, int count, const std::function<float(const RunMetrics&)>& pick)
{
	std::vector<float> v;
	for (auto* r : app.RunsOf(line, count))
	{
		float x = pick(app.Metrics(*r));
		if (x >= 0) v.push_back(x);
	}
	return v;
}

static void DeltaText(double now, double before, bool lowerIsBetter, std::string& text, ImU32& color, const char* unit = "")
{
	double d = now - before;
	if (std::fabs(d) < 1e-9) { text = "이전과 같음"; color = col::Dim; return; }
	bool good = lowerIsBetter ? d < 0 : d > 0;
	color = good ? col::Pass : col::Fail;
	text = Fmt("%s%s%s 이전 대비", d > 0 ? "▲ " : "▼ ", (std::fabs(d - std::round(d)) < 1e-9 ? Fmt("%.0f", std::fabs(d)) : Fmt("%.1f", std::fabs(d))).c_str(), unit);
}

// ============================================================================ overview

static void DrawOverviewNav(App& app)
{
	SectionHeader("프로젝트");
	if (!app.hasUProject)
	{
		TextDim("프로젝트가 선택되지 않았습니다.");
		return;
	}
	TextBold(app.projectName);
	TextDim(app.isCodeProject ? "C++ 프로젝트" : "Blueprint 전용 프로젝트");
	ImGui::Spacing();
	KeyValue("상태", app.attached ? "부착됨" : "부착 안 됨", 70);
	if (app.attached)
	{
		KeyValue("엔진", "UE " + app.Engine(), 70);
		KeyValue("시스템", app.SystemCommit(), 70);
		KeyValue("부착", Str(app.state, "attachedAt").substr(0, 16), 70);
	}
	KeyValue("경로", PathUtf8(app.project), 70);
	ImGui::Spacing();
	if (!app.attached)
	{
		if (PrimaryButton((std::string(ICON_LINK) + "  이 프로젝트에 부착").c_str(), ImVec2(-FLT_MIN, 0))) app.Run("attach");
		Tooltip("검사 플러그인과 __dwo-lens 폴더를 넣습니다. .uproject와 기존 파일은 건드리지 않고, 도구 > 제거로 모두 지울 수 있습니다.");
	}
	SectionHeader("최근 프로젝트");
	for (auto& r : app.recent)
	{
		fs::path p = fs::u8path(r);
		std::error_code ec;
		if (!fs::exists(p, ec)) continue;   // moved or deleted projects
		bool current = p == app.project;
		std::string label = PathUtf8(p.filename()) + "##" + r;
		if (ImGui::Selectable(label.c_str(), current)) { app.SetProject(p); break; }
		Tooltip(r.c_str());
		// same-named projects are told apart by their parent folder
		ImGui::SameLine();
		TextDim(PathUtf8(p.parent_path().filename()));
	}
}

static void DrawOverviewMain(App& app)
{
	if (!app.hasUProject)
	{
		if (EmptyState(ICON_FOLDER, "프로젝트를 여세요", "언리얼 프로젝트 폴더(.uproject가 있는 폴더)를 고르거나 이 창으로 끌어다 놓으세요. 부착은 버튼 하나로 끝납니다.", "프로젝트 열기..."))
		{
			fs::path p;
			if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p);
		}
		return;
	}
	// ---- header
	ImGui::PushFont(g_fonts.big);
	ImGui::TextUnformatted(app.projectName.c_str());
	ImGui::PopFont();
	std::string sub = (app.attached ? "UE " + app.Engine() + "  ·  " : std::string("부착되지 않음  ·  ")) +
	                  (app.isCodeProject ? "C++" : "Blueprint 전용") + (app.attached ? "  ·  시스템 " + app.SystemCommit() : "");
	TextDim(sub);
	float right = ImGui::GetWindowContentRegionMax().x;
	ImGui::SameLine();
	ImGui::SetCursorPosX(right - S(330));
	ImGui::BeginDisabled(!app.attached);
	if (PrimaryButton((std::string(ICON_PLAY) + "  기본 라인 실행").c_str())) app.RunAll();
	Tooltip("분석 → 로그 → 용량을 차례로 실행합니다 (엔진을 띄우지 않거나 짧은 라인).");
	ImGui::SameLine();
	if (ImGui::Button((std::string(ICON_SAVE) + "  종합 리포트").c_str())) ui.problemSel = "", app.requestActivity = Activity::History;
	Tooltip("기록 화면에서 종합 HTML 리포트를 저장합니다.");
	ImGui::EndDisabled();
	ImGui::Spacing();

	if (!app.attached)
	{
		if (EmptyState(ICON_LINK, "아직 부착되지 않았습니다", "부착하면 프로젝트 폴더에 __dwo-lens 폴더, __dwo-lens.bat, 미리 빌드된 검사 플러그인이 생깁니다. git 프로젝트라면 git에서 숨겨지고, 도구 > 제거로 흔적 없이 지울 수 있습니다.", "부착"))
			app.Run("attach");
		return;
	}

	// ---- health banner
	app.RebuildProblems();
	int worst = 0, withRuns = 0, failing = 0;
	for (int i = 0; i < kLineCount; ++i)
		if (const RunInfo* r = app.Latest(i))
		{
			++withRuns;
			int rank = SeverityRank(r->verdict);
			worst = ImMax(worst, rank);
			if (rank >= 3) ++failing;
		}
	int newCount = 0, openCount = 0;
	for (auto& p : app.problems)
		if (!p.muted && p.severity != "info") { ++openCount; newCount += p.isNew ? 1 : 0; }
	{
		ImU32 c = withRuns == 0 ? col::Muted : worst >= 4 ? col::Fail : worst >= 3 ? col::Warn : col::Pass;
		const char* state = withRuns == 0 ? "아직 실행 기록이 없습니다" : worst >= 4 ? "실패한 라인이 있습니다" : worst >= 3 ? "경고가 있습니다" : "모든 라인 통과";
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImVec2 size(ImGui::GetContentRegionAvail().x, S(54));
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p, p + size, WithAlpha(c, 0.12f), S(6));
		dl->AddRect(p, p + size, WithAlpha(c, 0.45f), S(6));
		dl->AddText(g_fonts.icon, g_fonts.icon->FontSize, p + ImVec2(S(16), S(16)), c, worst >= 3 ? ICON_WARNING : ICON_OK);
		dl->AddText(g_fonts.title, g_fonts.title->FontSize, p + ImVec2(S(48), S(8)), col::Text, state);
		std::string detail = Fmt("실행한 라인 %d/%d  ·  문제 %d  ·  새 문제 %d  ·  경고 이상 라인 %d", withRuns, kLineCount, openCount, newCount, failing);
		dl->AddText(p + ImVec2(S(48), S(32)), col::Dim, detail.c_str());
		ImGui::InvisibleButton("health", size);
		if (ImGui::IsItemClicked()) app.requestActivity = Activity::Problems;
		Tooltip("문제 화면으로 이동");
	}
	ImGui::Spacing();

	// ---- KPI cards
	float avail = ImGui::GetContentRegionAvail().x;
	int columns = ImMax(1, (int)(avail / S(250)));
	float w = (avail - (columns - 1) * ImGui::GetStyle().ItemSpacing.x) / columns;
	int index = 0;
	auto next = [&]() { if (index++ % columns != 0) ImGui::SameLine(); };
	auto card = [&](const char* id, const char* label, std::string value, int line, const std::function<float(const RunMetrics&)>& pick,
	                bool lowerBetter, ImU32 accent, Activity target, const char* unit, const char* tip) {
		next();
		std::string delta;
		ImU32 dc = col::Dim;
		std::vector<float> trend = line >= 0 ? TrendOf(app, line, 12, pick) : std::vector<float>();
		if (trend.size() >= 2) DeltaText(trend.back(), trend[trend.size() - 2], lowerBetter, delta, dc, unit);
		else if (line >= 0 && !app.Latest(line)) { value = "-"; delta = "실행 기록 없음"; }
		bool clicked = false;
		KpiCard(id, label, value, delta, dc, trend, accent, w, tip, &clicked);
		if (clicked) app.requestActivity = target;
	};
	const RunInfo* an = app.Latest(0);
	const RunInfo* ve = app.Latest(1);
	const RunInfo* lo = app.Latest(2);
	const RunInfo* sm = app.Latest(3);
	const RunInfo* pe = app.Latest(4);
	const RunInfo* sz = app.Latest(5);
	{
		next();
		bool clicked = false;
		KpiCard("k_new", "새 문제", std::to_string(newCount), Fmt("열린 문제 %d", openCount), col::Dim, {}, newCount ? col::Fail : col::Pass, w,
		        "최신 실행에서 처음 나타난 문제 (이전 실행 대비)", &clicked);
		if (clicked) { ui.onlyNew = true; app.requestActivity = Activity::Problems; }
	}
	auto metric = [&](const RunInfo* r) -> const RunMetrics* { return r ? &app.Metrics(*r) : nullptr; };
	const RunMetrics* mA = metric(an);
	const RunMetrics* mV = metric(ve);
	const RunMetrics* mL = metric(lo);
	const RunMetrics* mS = metric(sm);
	const RunMetrics* mP = metric(pe);
	const RunMetrics* mZ = metric(sz);
	card("k_find", "아키텍처 발견", mA ? std::to_string(mA->findingsFail + mA->findingsWarn) : "-", 0,
	     [](const RunMetrics& m) { return (float)(m.findingsFail + m.findingsWarn); }, true, col::Purple, Activity::Architecture, "", "실패·경고 수준 발견 (순환 참조, 하드 참조 체인 등)");
	card("k_bp", "BP 복잡도 초과", mA ? std::to_string(mA->bpOver) : "-", 0, [](const RunMetrics& m) { return (float)m.bpOver; }, true,
	     col::Warn, Activity::Problems, "", "노드·분기·실행 체인 기준을 넘는 BP 그래프");
	card("k_test", "테스트 통과율", mV && mV->testsTotal ? Fmt("%.0f%%", 100.0 * mV->testsPass / mV->testsTotal) : "-", 1,
	     [](const RunMetrics& m) { return m.testsTotal ? 100.f * m.testsPass / m.testsTotal : -1.f; }, false, col::Pass, Activity::Tests, "%p", "자동화 테스트 통과 비율");
	card("k_log", "로그 오류", mL ? std::to_string(mL->logError + mL->logFatal) : "-", 2, [](const RunMetrics& m) { return (float)(m.logError + m.logFatal); },
	     true, col::Fail, Activity::Logs, "", "Error·Fatal 줄 수");
	card("k_smoke", "맵 스모크", mS && mS->mapsTotal ? Fmt("%d/%d", mS->mapsPass, mS->mapsTotal) : "-", 3,
	     [](const RunMetrics& m) { return (float)m.mapsFail; }, true, col::Teal, Activity::Tests, "", "실행에 성공한 맵 / 전체 맵");
	card("k_perf", "프레임 p95", mP && mP->perfP95 >= 0 ? Fmt("%.1fms", mP->perfP95) : "-", 4, [](const RunMetrics& m) { return (float)m.perfP95; },
	     true, col::Info, Activity::Performance, "ms", "프레임 시간 95번째 백분위수");
	card("k_size", "콘텐츠 용량", mZ && mZ->contentBytes >= 0 ? Bytes(mZ->contentBytes) : "-", 5,
	     [](const RunMetrics& m) { return (float)(m.contentBytes / (1024.0 * 1024.0)); }, true, col::Accent, Activity::Performance, "MB", "Content 폴더의 에셋 용량");
	{
		next();
		int connected = 0, sources = 0;
		for (auto& s : app.store.sessions)
			if (s.connected) { ++connected; sources += (int)s.sources.size(); }
		bool clicked = false;
		KpiCard("k_live", "State Monitor", connected ? Fmt("%d 연결", connected) : "대기", connected ? Fmt("상태 소스 %d", sources) : Fmt("포트 %d에서 대기", app.live.port),
		        connected ? col::Pass : col::Dim, {}, connected ? col::Pass : col::Muted, w, "실행 중인 게임에서 실시간으로 받는 상태", &clicked);
		if (clicked) app.requestActivity = Activity::Live;
	}
	ImGui::Spacing();

	// ---- lines
	SectionHeader("생산 라인", "최근 10회 판정");
	if (ImGui::BeginTable("lines", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("라인", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("판정", ImGuiTableColumnFlags_WidthFixed, S(70));
		ImGui::TableSetupColumn("마지막 실행", ImGuiTableColumnFlags_WidthFixed, S(110));
		ImGui::TableSetupColumn("소요", ImGuiTableColumnFlags_WidthFixed, S(80));
		ImGui::TableSetupColumn("추세", ImGuiTableColumnFlags_WidthFixed, S(110));
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(150));
		ImGui::TableHeadersRow();
		for (int i = 0; i < kLineCount; ++i)
		{
			const RunInfo* r = app.Latest(i);
			ImGui::PushID(i);
			ImGui::TableNextRow(0, S(30));
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted((std::string(kLines[i].icon) + "  " + kLines[i].name).c_str());
			Tooltip(kLines[i].desc);
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			if (r) VerdictPill(r->verdict); else TextDim("-");
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			TextDim(r ? StampOf(r->id) : "기록 없음");
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			TextDim(r ? Duration(r->seconds) : "");
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ResultStrip(VerdictStripOf(app, i, 10));
			ImGui::TableNextColumn();
			ImGui::BeginDisabled(!app.attached);
			if (ImGui::SmallButton((std::string(ICON_PLAY) + " 실행").c_str())) app.Run(kLines[i].command);
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!r);
			if (ImGui::SmallButton("결과")) app.requestActivity = App::ActivityOfLine(i);
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_PAGE) && r) OpenWithShell(r->dir / "out" / "report.html");
			Tooltip("HTML 리포트 열기");
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	// ---- top problems
	SectionHeader("주요 문제", "새 문제와 심각도 순");
	int shown = 0;
	for (auto& p : app.problems)
	{
		if (p.muted || p.severity == "info") continue;
		if (shown++ >= 6) break;
		ImGui::PushID(shown);
		SeverityPill(p.severity);
		ImGui::SameLine();
		if (p.isNew) { Pill("NEW", col::Info, true); ImGui::SameLine(); }
		if (ImGui::Selectable((p.title + "##p").c_str(), false, ImGuiSelectableFlags_None))
		{
			ui.problemSel = p.key;
			app.requestActivity = Activity::Problems;
		}
		ImGui::SameLine();
		TextDim(p.object);
		ImGui::PopID();
	}
	if (shown == 0) TextDim("열린 문제가 없습니다.");
}

static void DrawOverviewInspector(App& app)
{
	SectionHeader("시작하기");
	struct Step { const char* text; bool done; const char* action; std::function<void()> fn; };
	bool analyzed = app.Latest(0) != nullptr;
	bool verified = app.Latest(1) != nullptr;
	bool connected = false;
	for (auto& s : app.store.sessions) connected |= !s.sources.empty();
	Step steps[] = {
		{ "프로젝트에 부착", app.attached, "부착", [&] { app.Run("attach"); } },
		{ "아키텍처 분석 실행", analyzed, "실행", [&] { app.Run("analyze"); } },
		{ "검증 실행 (에디터 닫고)", verified, "실행", [&] { app.Run("verify"); } },
		{ "StateMonitor 설치", app.stateMonitor != 0, "설치", [&] { app.Run("sm-install"); } },
		{ "게임을 실행해 상태 받기", connected, "열기", [&] { app.requestActivity = Activity::Live; } },
	};
	for (int i = 0; i < 5; ++i)
	{
		ImGui::PushID(i);
		TextColored(steps[i].done ? col::Pass : col::Muted, steps[i].done ? ICON_OK : ICON_RIGHT);
		ImGui::SameLine();
		if (steps[i].done) TextDim(steps[i].text);
		else
		{
			ImGui::TextUnformatted(steps[i].text);
			ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(54));
			ImGui::BeginDisabled(!app.attached && i != 0);
			if (ImGui::SmallButton(steps[i].action)) steps[i].fn();
			ImGui::EndDisabled();
		}
		ImGui::PopID();
	}
	SectionHeader("최근 실행");
	int n = 0;
	for (auto& r : app.runs)
	{
		if (n++ >= 10) break;
		ImGui::PushID(r.id.c_str());
		StatusDot(SemanticColor(r.verdict));
		ImGui::SameLine();
		std::string name = r.lineIndex >= 0 ? kLines[r.lineIndex].name : r.line;
		if (ImGui::Selectable((name + "##r").c_str(), false))
		{
			ui.historySel = r.id;
			app.requestActivity = Activity::History;
		}
		ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(80));
		TextDim(StampOf(r.id));
		ImGui::PopID();
	}
	if (app.runs.empty()) TextDim("실행 기록이 없습니다.");
}

// ============================================================================ architecture

static GraphNode ClassGraphNode(const Structure& s, const std::string& key)
{
	GraphNode n;
	const ClassNode& c = s.classes.at(key);
	n.id = key;
	n.title = c.name;
	n.subtitle = c.external ? "엔진 클래스" : c.kind == "blueprint" ? "Blueprint" : "C++ 클래스";
	n.accent = c.external ? col::Muted : c.kind == "blueprint" ? IM_COL32(0x35, 0x74, 0xF0, 255) : col::Purple;
	n.dim = c.external;
	for (size_t i = 0; i < c.props.size(); ++i)
	{
		auto sp = c.props[i].rfind(' ');
		n.rows.push_back({ sp == std::string::npos ? c.props[i] : c.props[i].substr(sp + 1), sp == std::string::npos ? "" : c.props[i].substr(0, sp) });
	}
	if (!c.funcs.empty()) n.footer = Fmt("함수 %d개", (int)c.funcs.size());
	return n;
}

static void BuildArchGraph(App& app, const Structure& s)
{
	std::string key = ui.archRun + "|" + std::to_string(ui.archMode) + "|" + ui.archSel;
	if (key == ui.archGraphKey) return;
	ui.archGraphKey = key;
	std::vector<GraphNode> nodes;
	std::vector<GraphLink> links;
	std::map<std::string, int> index;
	auto addNode = [&](GraphNode n) -> int {
		auto it = index.find(n.id);
		if (it != index.end()) return it->second;
		index[n.id] = (int)nodes.size();
		nodes.push_back(std::move(n));
		return (int)nodes.size() - 1;
	};
	auto simple = [](const std::string& id, const std::string& subtitle, ImU32 accent) {
		GraphNode n;
		n.id = id;
		n.title = id.size() > 48 ? "…" + id.substr(id.size() - 47) : id;
		n.subtitle = subtitle;
		n.accent = accent;
		return n;
	};
	ui.archGraph.horizontal = true;
	ui.archGraph.rowLimit = 8;
	if (ui.archMode == 0 && s.classes.count(ui.archSel))
	{
		// ancestors, the class, and descendants (limited)
		std::vector<std::string> chain;
		for (std::string k = ui.archSel; !k.empty() && s.classes.count(k) && chain.size() < 12; k = s.classes.at(k).parent) chain.push_back(k);
		int prev = -1;
		for (auto it = chain.rbegin(); it != chain.rend(); ++it)
		{
			int i = addNode(ClassGraphNode(s, *it));
			if (prev >= 0) links.push_back({ prev, i, "", 1, false, 0, 0 });
			prev = i;
		}
		std::deque<std::pair<std::string, int>> q{ { ui.archSel, 0 } };
		while (!q.empty() && nodes.size() < 120)
		{
			auto [k, depth] = q.front();
			q.pop_front();
			if (depth >= 3) continue;
			int from = index[k];
			for (auto& child : s.classes.at(k).children)
			{
				if (!s.classes.count(child)) continue;
				int to = addNode(ClassGraphNode(s, child));
				links.push_back({ from, to, "", 1, false, 0, 0 });
				q.push_back({ child, depth + 1 });
			}
		}
		ui.archGraph.horizontal = false;
		ui.archGraph.rowLimit = 6;
	}
	else if (ui.archMode == 1)
	{
		for (auto& f : s.flows)
		{
			if (f.title != ui.archSel) continue;
			for (auto& id : f.order)
			{
				GraphNode n;
				n.id = id;
				std::string label = f.labels.count(id) ? f.labels.at(id) : id;
				auto nl = label.find('\n');
				n.title = nl == std::string::npos ? label : label.substr(0, nl);
				if (nl != std::string::npos) n.subtitle = label.substr(nl + 1);
				bool event = std::find(f.starts.begin(), f.starts.end(), id) != f.starts.end();
				n.accent = event ? IM_COL32(0xC0, 0x3A, 0x2B, 255) : IM_COL32(0x35, 0x74, 0xF0, 255);
				n.icon = event ? ICON_BOLT : "";
				addNode(std::move(n));
			}
			for (auto& [from, edges] : f.out)
				for (auto& e : edges)
				{
					if (!index.count(from) || !index.count(e.to)) continue;
					links.push_back({ index[from], index[e.to], e.pin, 1, false, 0, IM_COL32(0xE6, 0xE6, 0xE6, 220) });
				}
		}
	}
	else if (ui.archMode == 2 || ui.archMode == 3 || ui.archMode == 4)
	{
		const auto& refs = ui.archMode == 2 ? s.calls : ui.archMode == 3 ? s.assetRefs : s.moduleDeps;
		ImU32 accent = ui.archMode == 2 ? col::Teal : ui.archMode == 3 ? col::Warn : col::Purple;
		const char* sub = ui.archMode == 2 ? "Blueprint" : ui.archMode == 3 ? "에셋" : "모듈";
		for (auto& r : refs)
		{
			if (!ui.archSel.empty() && r.src != ui.archSel && r.dst != ui.archSel) continue;
			if (nodes.size() > 160) break;
			int a = addNode(simple(r.src, sub, accent));
			int b = addNode(simple(r.dst, sub, accent));
			std::string label = ui.archMode == 2 ? r.type + "회" : r.type;
			links.push_back({ a, b, ui.archMode == 3 && r.type == "하드" ? "" : label, ui.archMode == 2 ? (float)ImMax(1, atoi(r.type.c_str())) : 1.f,
			                  ui.archMode == 3 && r.type != "하드", 0, 0 });
		}
		if (!ui.archSel.empty() && index.count(ui.archSel)) nodes[index[ui.archSel]].active = true;
	}
	ui.archGraph.Clear();
	ui.archGraph.Set(std::move(nodes), std::move(links));
	ui.archGraph.RequestLayout();
}

static void DrawArchitectureNav(App& app)
{
	const RunInfo* r = PickRunCombo(app, "##archrun", 0, ui.archRun);
	if (!r) { TextDim("분석 기록이 없습니다."); return; }
	const Structure& s = app.StructureOf(*r);
	const char* modes[] = { "클래스 계층", "BP 실행 흐름", "BP 호출 관계", "에셋 참조", "모듈 의존성", "품질 점검" };
	int counts[] = { (int)s.classes.size(), (int)s.flows.size(), (int)s.calls.size(), (int)s.assetRefs.size(), (int)s.moduleDeps.size(), -1 };
	const char* icons[] = { ICON_LAYERS, ICON_SORT, ICON_LINK, ICON_PACKAGE, ICON_LIBRARY, ICON_CHECKLIST };
	SectionHeader("보기");
	for (int i = 0; i < 6; ++i)
	{
		std::string label = std::string(icons[i]) + "  " + modes[i] + (counts[i] >= 0 ? Fmt("  (%d)", counts[i]) : "");
		if (ImGui::Selectable(label.c_str(), ui.archMode == i)) { ui.archMode = i; ui.archSel.clear(); ui.archNode.clear(); }
	}
	SectionHeader(modes[ui.archMode]);
	SearchBox("##archfilter", ui.filter, sizeof(ui.filter), "필터", -FLT_MIN);
	ImGui::BeginChild("archlist");
	if (ui.archMode == 0)
	{
		std::function<void(const std::string&, int)> tree = [&](const std::string& key, int depth) {
			const ClassNode& c = s.classes.at(key);
			bool match = ContainsNoCase(c.name, ui.filter);
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
			if (c.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
			if (ui.archSel == key) flags |= ImGuiTreeNodeFlags_Selected;
			if (ui.filter[0] || depth < 1) flags |= ImGuiTreeNodeFlags_DefaultOpen;
			if (ui.filter[0] && !match)
			{
				// only walk into children when filtering
				for (auto& ch : c.children) if (s.classes.count(ch)) tree(ch, depth + 1);
				return;
			}
			ImGui::PushStyleColor(ImGuiCol_Text, c.external ? col::Dim : col::Text);
			bool open = ImGui::TreeNodeEx(key.c_str(), flags, "%s", c.name.c_str());
			ImGui::PopStyleColor();
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { ui.archSel = key; ui.archNode = key; }
			if (open)
			{
				for (auto& ch : c.children) if (s.classes.count(ch)) tree(ch, depth + 1);
				ImGui::TreePop();
			}
		};
		for (auto& root : s.classRoots) tree(root, 0);
	}
	else if (ui.archMode == 1)
	{
		for (auto& f : s.flows)
		{
			if (!ContainsNoCase(f.title, ui.filter)) continue;
			if (ImGui::Selectable((f.title + Fmt("  (%d)", (int)f.labels.size())).c_str(), ui.archSel == f.title)) ui.archSel = f.title;
		}
	}
	else if (ui.archMode >= 2 && ui.archMode <= 4)
	{
		const auto& refs = ui.archMode == 2 ? s.calls : ui.archMode == 3 ? s.assetRefs : s.moduleDeps;
		std::map<std::string, int> degree;
		for (auto& rf : refs) { ++degree[rf.src]; ++degree[rf.dst]; }
		std::vector<std::pair<int, std::string>> sorted;
		for (auto& [k, v] : degree) if (ContainsNoCase(k, ui.filter)) sorted.push_back({ -v, k });
		std::sort(sorted.begin(), sorted.end());
		if (ImGui::Selectable("전체 보기", ui.archSel.empty())) ui.archSel.clear();
		ImGuiListClipper clip;
		clip.Begin((int)sorted.size());
		while (clip.Step())
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
			{
				auto& [neg, k] = sorted[i];
				if (ImGui::Selectable((k + "##" + std::to_string(i)).c_str(), ui.archSel == k)) ui.archSel = k;
				ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(30));
				TextDim(std::to_string(-neg));
			}
	}
	else TextDim("가운데 화면에서 표로 확인합니다.");
	ImGui::EndChild();
}

static void QualityTables(App& app, const RunInfo& r)
{
	const json& q = Obj(app.Outputs(r), "quality");
	if (ImGui::BeginTabBar("qtabs"))
	{
		if (ImGui::BeginTabItem("BP 복잡도"))
		{
			if (ImGui::BeginTable("cx", 7, kTable | ImGuiTableFlags_Sortable))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				for (const char* h : { "Blueprint", "그래프", "종류", "노드", "분기", "실행 체인", "기준 초과" }) ImGui::TableSetupColumn(h);
				ImGui::TableHeadersRow();
				std::vector<const json*> rows;
				for (auto& g : Arr(Obj(q, "complexity"), "graphs")) if (ContainsNoCase(Str(g, "blueprint") + Str(g, "graph"), ui.filter)) rows.push_back(&g);
				if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs())
					if (sp->SpecsCount > 0)
					{
						int c = sp->Specs[0].ColumnIndex;
						bool asc = sp->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
						const char* keys[] = { "blueprint", "graph", "kind", "nodes", "branches", "execChain", "over" };
						std::stable_sort(rows.begin(), rows.end(), [&](const json* a, const json* b) {
							bool less = c >= 3 && c <= 5 ? Num(*a, keys[c]) < Num(*b, keys[c]) : Str(*a, keys[c]) < Str(*b, keys[c]);
							bool greater = c >= 3 && c <= 5 ? Num(*a, keys[c]) > Num(*b, keys[c]) : Str(*a, keys[c]) > Str(*b, keys[c]);
							return asc ? less : greater;
						});
					}
				for (auto* g : rows)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(*g, "blueprint").c_str());
					ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(*g, "graph").c_str());
					ImGui::TableNextColumn(); TextDim(Str(*g, "kind"));
					ImGui::TableNextColumn(); ImGui::Text("%lld", Num(*g, "nodes"));
					ImGui::TableNextColumn(); ImGui::Text("%lld", Num(*g, "branches"));
					ImGui::TableNextColumn(); ImGui::Text("%lld", Num(*g, "execChain"));
					ImGui::TableNextColumn();
					if (Arr(*g, "over").empty()) TextDim("-"); else Pill(JoinStrings(Arr(*g, "over")), col::Warn);
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}
		const json& violations = Arr(Obj(q, "naming"), "violations");
		if (ImGui::BeginTabItem(Fmt("이름 규칙 (%d)###naming", (int)violations.size()).c_str()))
		{
			if (ImGui::BeginTable("nm", 3, kTable))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				for (const char* h : { "에셋", "클래스", "문제" }) ImGui::TableSetupColumn(h);
				ImGui::TableHeadersRow();
				for (auto& v : violations)
				{
					std::string problems = JoinStrings(Arr(v, "problems"), " · ");
					if (!ContainsNoCase(Str(v, "asset") + problems, ui.filter)) continue;
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::TextUnformatted(Str(v, "asset").c_str());
					ImGui::TableNextColumn(); TextDim(Str(v, "class"));
					ImGui::TableNextColumn(); ImGui::TextWrapped("%s", problems.c_str());
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}
		const json& unused = Obj(q, "unused");
		if (ImGui::BeginTabItem(Fmt("미사용 %d · 리다이렉터 %d###unused", (int)Arr(unused, "unused").size(), (int)Arr(unused, "redirectors").size()).c_str()))
		{
			TextDim("시작점: " + JoinStrings(Arr(unused, "roots")) + " " + JoinStrings(Arr(unused, "iniRoots")));
			for (const char* key : { "unused", "redirectors" })
			{
				SectionHeader(std::string(key) == "unused" ? "도달할 수 없는 에셋" : "리다이렉터");
				for (auto& a : Arr(unused, key))
					if (ContainsNoCase(a.dump(), ui.filter)) ImGui::BulletText("%s", a.is_string() ? a.get<std::string>().c_str() : a.dump().c_str());
			}
			ImGui::EndTabItem();
		}
		const json& sm = Obj(q, "stateMonitor");
		if (sm.value("present", false) && ImGui::BeginTabItem("StateMonitor 연계"))
		{
			KeyValue("플러그인", Str(sm, "plugin"));
			KeyValue("Source ID", JoinStrings(Arr(sm, "sourceIds")));
			KeyValue("테스트 필터", Str(sm, "testFilter"));
			for (auto& c : Arr(sm, "cppRefs")) ImGui::BulletText("%s:%lld  %s", Str(c, "file").c_str(), Num(c, "line"), Str(c, "code").c_str());
			for (auto& b : Arr(sm, "bpCalls")) ImGui::BulletText("BP %s.%s  %s", Str(b, "blueprint").c_str(), Str(b, "graph").c_str(), Str(b, "function").c_str());
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

static void DrawArchitectureMain(App& app)
{
	const RunInfo* r = ui.archRun.empty() ? app.Latest(0) : app.FindRun(ui.archRun);
	if (!r) r = app.Latest(0);
	if (!r)
	{
		if (EmptyState(ICON_LAYERS, "아키텍처 분석 기록이 없습니다", "분석을 실행하면 클래스 계층, Blueprint 실행 흐름, 호출·참조 관계를 그래프로 볼 수 있습니다.", app.attached ? "분석 실행" : nullptr))
			app.Run("analyze");
		return;
	}
	const Structure& s = app.StructureOf(*r);
	// open on something useful: the first project class or the largest flow
	if (ui.archSel.empty() && ui.archMode == 0)
		for (auto& [key, c] : s.classes)
			if (!c.external) { ui.archSel = key; break; }
	if (ui.archSel.empty() && ui.archMode == 1 && !s.flows.empty())
	{
		const Flow* best = &s.flows.front();
		for (auto& f : s.flows) if (f.labels.size() > best->labels.size()) best = &f;
		ui.archSel = best->title;
	}
	const char* modes[] = { "클래스 계층", "BP 실행 흐름", "BP 호출 관계", "에셋 참조", "모듈 의존성", "품질 점검" };
	ViewHeader(ICON_LAYERS, "아키텍처", StampOf(r->id) + "  ·  심볼 " + std::to_string(s.symbols) + " · 관계 " + std::to_string(s.relations));
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(250));
	VerdictPill(r->verdict);
	ImGui::SameLine();
	if (ImGui::Button((std::string(ICON_PAGE) + " 리포트").c_str())) OpenWithShell(r->dir / "out" / "report.html");
	ImGui::SameLine();
	RunLineButton(app, 0, false, ICON_PLAY " 다시 분석");
	// breadcrumb
	TextDim("아키텍처");
	ImGui::SameLine(); TextDim(ICON_RIGHT); ImGui::SameLine();
	TextDim(modes[ui.archMode]);
	if (!ui.archSel.empty())
	{
		ImGui::SameLine(); TextDim(ICON_RIGHT); ImGui::SameLine();
		std::string name = ui.archMode == 0 && s.classes.count(ui.archSel) ? s.classes.at(ui.archSel).name : ui.archSel;
		ImGui::TextUnformatted(name.c_str());
	}
	ImGui::Separator();

	if (ui.archMode == 5)
	{
		QualityTables(app, *r);
		return;
	}
	if ((ui.archMode == 0 || ui.archMode == 1) && ui.archSel.empty())
	{
		EmptyState(ui.archMode == 0 ? ICON_LAYERS : ICON_SORT, ui.archMode == 0 ? "클래스를 고르세요" : "Blueprint 그래프를 고르세요",
		           ui.archMode == 0 ? "왼쪽 계층에서 클래스를 고르면 부모 체인과 자식 클래스를 노드 그래프로 보여 줍니다."
		                            : "왼쪽 목록에서 그래프를 고르면 이벤트부터 실행 순서를 Blueprint처럼 노드로 보여 줍니다.");
		return;
	}
	BuildArchGraph(app, s);
	// toolbar
	if (ToolButton(ICON_REFRESH, "자동 배치 (노드 위치 초기화)")) ui.archGraph.RequestLayout();
	ImGui::SameLine();
	if (ToolButton(ICON_SEARCH, "전체 보기 (F)")) ui.archGraph.RequestFrame();
	ImGui::SameLine();
	if (ToolButton(ICON_GRAPH, "미니맵", ui.archGraph.showMinimap)) ui.archGraph.showMinimap = !ui.archGraph.showMinimap;
	ImGui::SameLine();
	TextDim(Fmt("노드 %d · 연결 %d  ·  드래그: 이동 · 휠: 확대 · F: 전체 보기", (int)ui.archGraph.nodes.size(), (int)ui.archGraph.links.size()));
	if (ui.archGraph.nodes.empty())
	{
		EmptyState(ICON_GRAPH, "표시할 노드가 없습니다", "필터나 선택을 바꿔 보세요.");
		return;
	}
	int clicked = ui.archGraph.Draw("##archcanvas", ImVec2(0, 0));
	if (clicked >= 0) ui.archNode = ui.archGraph.nodes[clicked].id;
	if (ui.archGraph.hovered >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ui.archMode != 1)
	{
		ui.archSel = ui.archGraph.nodes[ui.archGraph.hovered].id;
	}
}

static void DrawArchitectureInspector(App& app)
{
	const RunInfo* r = ui.archRun.empty() ? app.Latest(0) : app.FindRun(ui.archRun);
	if (!r) r = app.Latest(0);
	if (!r) { TextDim("선택 항목 없음"); return; }
	const Structure& s = app.StructureOf(*r);
	std::string key = ui.archNode.empty() ? ui.archSel : ui.archNode;
	if (ui.archMode == 0 && s.classes.count(key))
	{
		const ClassNode& c = s.classes.at(key);
		Title(c.name);
		TextDim(c.external ? "엔진 클래스" : c.kind == "blueprint" ? "Blueprint" : "C++ 클래스");
		if (!c.parent.empty() && s.classes.count(c.parent))
		{
			TextDim("부모");
			ImGui::SameLine();
			if (ImGui::SmallButton(s.classes.at(c.parent).name.c_str())) { ui.archSel = c.parent; ui.archNode = c.parent; }
		}
		SectionHeader("프로퍼티", std::to_string(c.props.size()).c_str());
		for (auto& p : c.props) ImGui::BulletText("%s", p.c_str());
		SectionHeader("함수", std::to_string(c.funcs.size()).c_str());
		for (auto& f : c.funcs) ImGui::BulletText("%s", f.c_str());
		SectionHeader("자식 클래스", std::to_string(c.children.size()).c_str());
		for (auto& ch : c.children)
			if (s.classes.count(ch) && ImGui::Selectable(s.classes.at(ch).name.c_str())) { ui.archSel = ch; ui.archNode = ch; }
		return;
	}
	if (ui.archMode >= 2 && ui.archMode <= 4 && !key.empty())
	{
		const auto& refs = ui.archMode == 2 ? s.calls : ui.archMode == 3 ? s.assetRefs : s.moduleDeps;
		Title(key);
		SectionHeader("나가는 관계");
		for (auto& rf : refs) if (rf.src == key) { ImGui::BulletText("%s", rf.dst.c_str()); ImGui::SameLine(); TextDim(rf.type); }
		SectionHeader("들어오는 관계");
		for (auto& rf : refs) if (rf.dst == key) { ImGui::BulletText("%s", rf.src.c_str()); ImGui::SameLine(); TextDim(rf.type); }
		return;
	}
	if (ui.archMode == 1 && !ui.archNode.empty())
	{
		for (auto& n : ui.archGraph.nodes)
			if (n.id == ui.archNode) { Title(n.title); TextDim(n.subtitle); }
		return;
	}
	SectionHeader("분석 요약");
	const RunMetrics& m = app.Metrics(*r);
	KeyValue("클래스", std::to_string(s.classes.size()));
	KeyValue("BP 그래프", std::to_string(s.flows.size()));
	KeyValue("호출 관계", std::to_string(s.calls.size()));
	KeyValue("에셋 참조", std::to_string(s.assetRefs.size()));
	KeyValue("발견", Fmt("실패 %d · 경고 %d · 정보 %d", m.findingsFail, m.findingsWarn, m.findingsInfo));
	KeyValue("BP 초과", std::to_string(m.bpOver));
	KeyValue("이름 규칙", std::to_string(m.naming));
	SectionHeader("발견");
	for (auto& f : Arr(Obj(app.Outputs(*r), "findings"), "findings"))
	{
		SeverityPill(Str(f, "severity"));
		ImGui::SameLine();
		ImGui::TextWrapped("%s", Str(f, "message").c_str());
	}
}

// ============================================================================ problems

static const char* kProblemGroups[] = { "묶지 않음", "범주", "심각도", "라인" };

static std::string ProblemGroupOf(const Problem& p)
{
	switch (ui.groupBy)
	{
	case 1: return p.category;
	case 2: return std::to_string(9 - SeverityRank(p.severity)) + p.severity;
	case 3: return p.line >= 0 ? kLines[p.line].name : "-";
	default: return "";
	}
}

static bool ProblemVisible(const Problem& p)
{
	if (p.muted && !ui.showMuted) return false;
	if (ui.onlyNew && !p.isNew) return false;
	if (ui.hiddenSeverities.count(Lower(p.severity))) return false;
	if (!ui.categoryFilter.empty() && p.category != ui.categoryFilter) return false;
	return ContainsNoCase(p.title + " " + p.object + " " + p.category, ui.filter);
}

static void DrawProblemsNav(App& app)
{
	app.RebuildProblems();
	std::map<std::string, int> bySeverity, byCategory;
	int newCount = 0, muted = 0;
	for (auto& p : app.problems)
	{
		if (p.muted) { ++muted; continue; }
		++bySeverity[Lower(p.severity)];
		++byCategory[p.category];
		newCount += p.isNew;
	}
	SectionHeader("상태");
	ImGui::Checkbox(Fmt("새 문제만  (%d)", newCount).c_str(), &ui.onlyNew);
	ImGui::Checkbox(Fmt("음소거 표시  (%d)", muted).c_str(), &ui.showMuted);
	SectionHeader("심각도");
	for (const char* sev : { "fatal", "error", "warn", "minor", "info" })
	{
		auto it = bySeverity.find(sev);
		if (it == bySeverity.end()) continue;
		bool on = !ui.hiddenSeverities.count(sev);
		ImGui::PushID(sev);
		if (ImGui::Checkbox("##sev", &on)) { if (on) ui.hiddenSeverities.erase(sev); else ui.hiddenSeverities.insert(sev); }
		ImGui::SameLine();
		SeverityPill(sev);
		ImGui::SameLine();
		TextDim(std::to_string(it->second));
		ImGui::PopID();
	}
	SectionHeader("범주");
	if (ImGui::Selectable(Fmt("전체  (%d)", (int)app.problems.size() - muted).c_str(), ui.categoryFilter.empty())) ui.categoryFilter.clear();
	for (auto& [c, n] : byCategory)
		if (ImGui::Selectable(Fmt("%s  (%d)", c.c_str(), n).c_str(), ui.categoryFilter == c)) ui.categoryFilter = c;
}

static void DrawProblemsMain(App& app)
{
	app.RebuildProblems();
	ViewHeader(ICON_WARNING, "문제", "모든 라인의 최신 실행에서 모은 문제");
	if (app.problems.empty())
	{
		EmptyState(ICON_OK, "문제가 없습니다", app.runs.empty() ? "라인을 실행하면 발견·테스트 실패·로그 오류·스모크 실패를 이곳에 모읍니다." : "최신 실행에서 열린 문제가 없습니다.");
		return;
	}
	SearchBox("##pfilter", ui.filter, sizeof(ui.filter), ICON_SEARCH "  제목, 대상, 범주 검색", S(340));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(S(130));
	ImGui::Combo("##group", &ui.groupBy, kProblemGroups, 4);
	ImGui::SameLine();
	int visible = 0;
	for (auto& p : app.problems) visible += ProblemVisible(p);
	TextDim(Fmt("%d개 표시", visible));
	if (ImGui::BeginTable("problems", 6, kTable | ImGuiTableFlags_Sortable, ImVec2(0, 0)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("심각도", ImGuiTableColumnFlags_WidthFixed, S(70));
		ImGui::TableSetupColumn("문제", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("대상", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("범주", ImGuiTableColumnFlags_WidthFixed, S(100));
		ImGui::TableSetupColumn("라인", ImGuiTableColumnFlags_WidthFixed, S(100));
		ImGui::TableSetupColumn("실행", ImGuiTableColumnFlags_WidthFixed, S(90));
		ImGui::TableHeadersRow();
		std::vector<const Problem*> rows;
		for (auto& p : app.problems) if (ProblemVisible(p)) rows.push_back(&p);
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs())
			if (sp->SpecsCount > 0 && sp->SpecsDirty == false)
			{
				int c = sp->Specs[0].ColumnIndex;
				bool asc = sp->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
				std::stable_sort(rows.begin(), rows.end(), [&](const Problem* a, const Problem* b) {
					int cmp = 0;
					if (c == 0) cmp = SeverityRank(a->severity) - SeverityRank(b->severity);
					else if (c == 1) cmp = a->title.compare(b->title);
					else if (c == 2) cmp = a->object.compare(b->object);
					else if (c == 3) cmp = a->category.compare(b->category);
					else if (c == 4) cmp = a->line - b->line;
					else cmp = a->runId.compare(b->runId);
					return asc ? cmp < 0 : cmp > 0;
				});
			}
		if (ui.groupBy) std::stable_sort(rows.begin(), rows.end(), [](const Problem* a, const Problem* b) { return ProblemGroupOf(*a) < ProblemGroupOf(*b); });
		std::string group;
		bool groupOpen = true;
		for (size_t i = 0; i < rows.size(); ++i)
		{
			const Problem& p = *rows[i];
			if (ui.groupBy)
			{
				std::string g = ProblemGroupOf(p);
				if (g != group)
				{
					group = g;
					int count = 0;
					for (size_t k = i; k < rows.size() && ProblemGroupOf(*rows[k]) == g; ++k) ++count;
					ImGui::TableNextRow(ImGuiTableRowFlags_None, S(26));
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 255, 255, 10));
					ImGui::TableNextColumn();
					std::string shown = ui.groupBy == 2 ? g.substr(1) : g;
					ImGui::PushID(g.c_str());
					groupOpen = ImGui::TreeNodeEx("##g", ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen,
					                              "%s  (%d)", shown.c_str(), count);
					ImGui::PopID();
				}
				if (!groupOpen) continue;
			}
			ImGui::PushID(p.key.c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			SeverityPill(p.severity);
			ImGui::TableNextColumn();
			if (p.isNew) { Pill("NEW", col::Info, true); ImGui::SameLine(); }
			if (p.muted) { TextDim(ICON_CLOSE); ImGui::SameLine(); }
			if (ImGui::Selectable(p.title.c_str(), ui.problemSel == p.key, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
				ui.problemSel = p.key;
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem(p.muted ? "음소거 해제" : "음소거 (기준선에 추가)"))
				{
					if (p.muted) app.mutes.erase(p.key); else app.mutes.insert(p.key);
					app.SaveProjectSettings();
				}
				if (ImGui::MenuItem("제목 복사")) CopyToClipboard(p.title);
				if (ImGui::MenuItem("대상 복사")) CopyToClipboard(p.object);
				ImGui::EndPopup();
			}
			ImGui::TableNextColumn(); TextDim(p.object);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(p.category.c_str());
			ImGui::TableNextColumn(); TextDim(p.line >= 0 ? kLines[p.line].name : "-");
			ImGui::TableNextColumn(); TextDim(StampOf(p.runId));
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

static void DetailJson(const json& j, int depth = 0)
{
	if (j.is_object())
	{
		for (auto it = j.begin(); it != j.end(); ++it)
		{
			const json& v = it.value();
			if (v.is_object() || (v.is_array() && !v.empty() && (v[0].is_object() || v.size() > 3)))
			{
				ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAvailWidth | (depth < 1 && v.size() <= 30 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
				if (ImGui::TreeNodeEx(it.key().c_str(), f, "%s  (%d)", it.key().c_str(), (int)v.size()))
				{
					DetailJson(v, depth + 1);
					ImGui::TreePop();
				}
			}
			else KeyValue(it.key().c_str(), v.is_string() ? v.get<std::string>() : v.is_array() ? JoinStrings(v) : v.dump(), 100);
		}
	}
	else if (j.is_array())
	{
		int i = 0;
		for (auto& v : j)
		{
			if (i++ > 300) { TextDim("…"); break; }
			ImGui::PushID(i);
			if (v.is_object())
			{
				if (ImGui::TreeNodeEx("##i", ImGuiTreeNodeFlags_SpanAvailWidth, "[%d]", i - 1)) { DetailJson(v, depth + 1); ImGui::TreePop(); }
			}
			else ImGui::TextWrapped("%s", v.is_string() ? v.get<std::string>().c_str() : v.dump().c_str());
			ImGui::PopID();
		}
	}
	else ImGui::TextWrapped("%s", j.is_string() ? j.get<std::string>().c_str() : j.dump().c_str());
}

static void DrawProblemsInspector(App& app)
{
	app.RebuildProblems();
	const Problem* sel = nullptr;
	for (auto& p : app.problems) if (p.key == ui.problemSel) sel = &p;
	if (!sel) { TextDim("문제를 고르면 자세한 내용이 여기에 표시됩니다."); return; }
	SeverityPill(sel->severity);
	if (sel->isNew) { ImGui::SameLine(); Pill("NEW", col::Info, true); }
	ImGui::PushTextWrapPos(0);
	Title(sel->title);
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	KeyValue("대상", sel->object, 70);
	KeyValue("범주", sel->category, 70);
	KeyValue("라인", sel->line >= 0 ? kLines[sel->line].name : "-", 70);
	KeyValue("실행", sel->runId, 70);
	ImGui::Spacing();
	if (ImGui::Button(sel->muted ? "음소거 해제" : (std::string(ICON_CLOSE) + " 음소거").c_str()))
	{
		if (sel->muted) app.mutes.erase(sel->key); else app.mutes.insert(sel->key);
		app.SaveProjectSettings();
		return;
	}
	Tooltip("기준선에 추가해 목록과 새 문제 집계에서 뺍니다.");
	ImGui::SameLine();
	if (ImGui::Button((std::string(ICON_COPY) + " 복사").c_str())) CopyToClipboard(sel->title + "\n" + sel->object);
	ImGui::SameLine();
	if (const RunInfo* r = app.FindRun(sel->runId))
		if (ImGui::Button((std::string(ICON_PAGE) + " 리포트").c_str())) OpenWithShell(r->dir / "out" / "report.html");
	if (sel->detail)
	{
		SectionHeader("세부 정보");
		std::string log = Str(*sel->detail, "log");
		if (!log.empty() && ImGui::SmallButton("로그 파일 열기")) OpenWithShell(fs::u8path(log));
		DetailJson(*sel->detail);
	}
}

// ============================================================================ tests (verify + smoke)

struct TestRow
{
	std::string name;
	char state = '-';
	std::string history;   // oldest first
	bool flaky = false;
	bool newFailure = false;
	const json* detail = nullptr;
};

static std::vector<TestRow> CollectTests(App& app, const RunInfo* latest)
{
	std::vector<TestRow> rows;
	if (!latest) return rows;
	auto history = app.RunsOf(1, 10);
	const json& d = app.Outputs(*latest);
	for (auto& step : Arr(Obj(d, "verify"), "steps"))
		for (auto& t : Arr(Obj(step, "automation"), "tests"))
		{
			TestRow row;
			row.name = Str(t, "name");
			row.state = Str(t, "state") == "Success" ? 'p' : 'f';
			row.detail = &t;
			bool sawPass = false, sawFail = false;
			for (auto* r : history)
			{
				const RunMetrics& m = app.Metrics(*r);
				auto it = m.tests.find(row.name);
				char c = it == m.tests.end() ? '-' : it->second;
				row.history += c;
				sawPass |= c == 'p';
				sawFail |= c == 'f';
			}
			row.flaky = sawPass && sawFail && row.history.size() >= 3;
			if (row.state == 'f' && row.history.size() >= 2) row.newFailure = row.history[row.history.size() - 2] != 'f';
			rows.push_back(std::move(row));
		}
	return rows;
}

static bool TestVisible(const TestRow& t)
{
	if (ui.testFilter == 1 && t.state != 'f') return false;
	if (ui.testFilter == 2 && t.state != 'p') return false;
	if (ui.testFilter == 3 && !t.flaky) return false;
	if (!ui.testPrefix.empty() && t.name.rfind(ui.testPrefix, 0) != 0) return false;
	return ContainsNoCase(t.name, ui.filter);
}

static void DrawTestsNav(App& app)
{
	const RunInfo* latest = app.Latest(1);
	auto tests = CollectTests(app, latest);
	int fail = 0, pass = 0, flaky = 0;
	for (auto& t : tests) { fail += t.state == 'f'; pass += t.state == 'p'; flaky += t.flaky; }
	SectionHeader("자동화 테스트");
	const char* filters[] = { "전체", "실패", "통과", "불안정" };
	int counts[] = { (int)tests.size(), fail, pass, flaky };
	for (int i = 0; i < 4; ++i)
		if (ImGui::Selectable(Fmt("%s  (%d)", filters[i], counts[i]).c_str(), ui.testFilter == i)) { ui.testFilter = i; ui.testsTab = 0; }
	SectionHeader("테스트 트리");
	// group by the first two dot segments
	std::map<std::string, std::pair<int, int>> groups;
	for (auto& t : tests)
	{
		std::string g = t.name;
		size_t dot = g.find('.');
		if (dot != std::string::npos) dot = g.find('.', dot + 1);
		g = dot == std::string::npos ? g : g.substr(0, dot + 1);
		auto& c = groups[g];
		(t.state == 'f' ? c.second : c.first)++;
	}
	if (ImGui::Selectable("모든 테스트", ui.testPrefix.empty())) ui.testPrefix.clear();
	for (auto& [g, c] : groups)
	{
		ImGui::PushID(g.c_str());
		TextColored(c.second ? col::Fail : col::Pass, c.second ? ICON_ERROR : ICON_OK);
		ImGui::SameLine();
		if (ImGui::Selectable(Fmt("%s  (%d)", g.c_str(), c.first + c.second).c_str(), ui.testPrefix == g)) { ui.testPrefix = g; ui.testsTab = 0; }
		ImGui::PopID();
	}
	const RunInfo* smoke = app.Latest(3);
	SectionHeader("맵 스모크");
	if (!smoke) TextDim("실행 기록 없음");
	else
		for (auto& m : Arr(Obj(app.Outputs(*smoke), "smoke"), "maps"))
		{
			std::string name = Str(m, "map");
			ImGui::PushID(name.c_str());
			TextColored(SemanticColor(Str(m, "verdict")), Str(m, "verdict") == "pass" ? ICON_OK : ICON_ERROR);
			ImGui::SameLine();
			if (ImGui::Selectable(name.c_str(), ui.smokeSel == name)) { ui.smokeSel = name; ui.testSel.clear(); ui.testsTab = 1; }
			ImGui::PopID();
		}
}

static void DrawTestsMain(App& app)
{
	const RunInfo* latest = app.Latest(1);
	const RunInfo* smoke = app.Latest(3);
	ViewHeader(ICON_CHECKLIST, "검증", latest ? "검증 " + StampOf(latest->id) : "검증 기록 없음");
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(290));
	RunLineButton(app, 1, true, ICON_PLAY " 검증");
	ImGui::SameLine();
	RunLineButton(app, 3, false, ICON_PLAY " 맵 스모크");
	if (!latest && !smoke)
	{
		if (EmptyState(ICON_CHECKLIST, "검증 기록이 없습니다", "검증은 Blueprint 전체 컴파일, 데이터 검증, 자동화 테스트를 실행합니다. 맵 스모크는 맵마다 게임을 띄워 오류와 크래시를 수집합니다.", app.attached ? "검증 실행" : nullptr))
			app.Run("verify");
		return;
	}
	// step cards
	if (latest)
	{
		const json& steps = Arr(Obj(app.Outputs(*latest), "verify"), "steps");
		float avail = ImGui::GetContentRegionAvail().x;
		int n = ImMax(1, (int)steps.size());
		float w = (avail - (n - 1) * ImGui::GetStyle().ItemSpacing.x) / n;
		for (int i = 0; i < (int)steps.size(); ++i)
		{
			const json& st = steps[i];
			if (i) ImGui::SameLine();
			const json& auto_ = Obj(st, "automation");
			const json& sum = Obj(st, "summary");
			std::string value, sub;
			if (!auto_.empty())
			{
				const json& s = Obj(auto_, "summary");
				value = Fmt("%lld/%lld", Num(s, "succeeded") + Num(s, "succeededWithWarnings"), Num(s, "succeeded") + Num(s, "succeededWithWarnings") + Num(s, "failed"));
				sub = Fmt("실패 %lld · 미실행 %lld", Num(s, "failed"), Num(s, "notRun"));
			}
			else if (!sum.empty())
			{
				const json& c = Obj(sum, "counts");
				value = Fmt("%lld", Num(c, "Error") + Num(c, "Fatal"));
				sub = Fmt("오류 · 경고 %lld", Num(c, "Warning"));
			}
			else { value = "-"; sub = Str(st, "note", "실행 안 함"); }
			KpiCard(("step" + std::to_string(i)).c_str(), (Str(st, "id") + "  " + Str(st, "name")).c_str(), value, sub, SemanticColor(Str(st, "verdict")),
			        {}, SemanticColor(Str(st, "verdict")), w);
		}
	}
	if (ImGui::BeginTabBar("testtabs"))
	{
		ImGuiTabItemFlags f0 = ui.testsTab == 0 ? ImGuiTabItemFlags_SetSelected : 0;
		auto tests = CollectTests(app, latest);
		if (ImGui::BeginTabItem(Fmt("자동화 테스트 (%d)###t0", (int)tests.size()).c_str(), nullptr, f0))
		{
			SearchBox("##tfilter", ui.filter, sizeof(ui.filter), ICON_SEARCH "  테스트 이름", S(300));
			if (ImGui::BeginTable("tests", 4, kTable | ImGuiTableFlags_Sortable))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("결과", ImGuiTableColumnFlags_WidthFixed, S(70));
				ImGui::TableSetupColumn("테스트", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("최근 10회", ImGuiTableColumnFlags_WidthFixed, S(110));
				ImGui::TableSetupColumn("상태", ImGuiTableColumnFlags_WidthFixed, S(110));
				ImGui::TableHeadersRow();
				std::stable_sort(tests.begin(), tests.end(), [](const TestRow& a, const TestRow& b) { return a.state == 'f' && b.state != 'f'; });
				for (auto& t : tests)
				{
					if (!TestVisible(t)) continue;
					ImGui::PushID(t.name.c_str());
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					VerdictPill(t.state == 'p' ? "pass" : "fail");
					ImGui::TableNextColumn();
					if (ImGui::Selectable(t.name.c_str(), ui.testSel == t.name, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
						{ ui.testSel = t.name; ui.smokeSel.clear(); }
					ImGui::TableNextColumn();
					ResultStrip(t.history, "왼쪽이 오래된 실행");
					ImGui::TableNextColumn();
					if (t.newFailure) { Pill("새 실패", col::Fail, true); ImGui::SameLine(); }
					if (t.flaky) Pill("불안정", col::Warn);
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}
		ImGuiTabItemFlags f1 = ui.testsTab == 1 ? ImGuiTabItemFlags_SetSelected : 0;
		if (ImGui::BeginTabItem("맵 스모크###t1", nullptr, f1))
		{
			if (!smoke) EmptyState(ICON_PLAY, "맵 스모크 기록이 없습니다", "맵마다 게임을 에디터 없이 띄워 오류·ensure·크래시를 수집합니다.");
			else if (ImGui::BeginTable("maps", 7, kTable))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				for (const char* h : { "판정", "맵", "오류", "경고", "ensure", "시간", "종료 코드" }) ImGui::TableSetupColumn(h);
				ImGui::TableHeadersRow();
				for (auto& m : Arr(Obj(app.Outputs(*smoke), "smoke"), "maps"))
				{
					std::string name = Str(m, "map");
					ImGui::PushID(name.c_str());
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); VerdictPill(Str(m, "verdict"));
					ImGui::TableNextColumn();
					if (ImGui::Selectable(name.c_str(), ui.smokeSel == name, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) { ui.smokeSel = name; ui.testSel.clear(); }
					ImGui::TableNextColumn(); ImGui::Text("%lld", Num(m, "errors"));
					ImGui::TableNextColumn(); ImGui::Text("%lld", Num(m, "warnings"));
					ImGui::TableNextColumn(); ImGui::Text("%lld", Num(m, "ensures"));
					ImGui::TableNextColumn(); TextDim(Fmt("%.1f초", Real(m, "seconds")));
					ImGui::TableNextColumn(); TextDim(Str(m, "exitCode", "-"));
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}
		ui.testsTab = -1;
		ImGui::EndTabBar();
	}
}

static void LogGroupList(const json& groups, const char* id);

static void DrawTestsInspector(App& app)
{
	if (!ui.smokeSel.empty())
		if (const RunInfo* smoke = app.Latest(3))
			for (auto& m : Arr(Obj(app.Outputs(*smoke), "smoke"), "maps"))
				if (Str(m, "map") == ui.smokeSel)
				{
					VerdictPill(Str(m, "verdict"));
					Title(ui.smokeSel);
					KeyValue("오류", std::to_string(Num(m, "errors")), 70);
					KeyValue("경고", std::to_string(Num(m, "warnings")), 70);
					KeyValue("ensure", std::to_string(Num(m, "ensures")), 70);
					KeyValue("크래시", m.value("fatal", false) ? "예" : "아니오", 70);
					if (!Str(m, "log").empty() && ImGui::Button("로그 파일 열기")) OpenWithShell(fs::u8path(Str(m, "log")));
					SectionHeader("로그 그룹");
					LogGroupList(Arr(m, "groups"), "smokegroups");
					return;
				}
	const RunInfo* latest = app.Latest(1);
	auto tests = CollectTests(app, latest);
	for (auto& t : tests)
		if (t.name == ui.testSel)
		{
			VerdictPill(t.state == 'p' ? "pass" : "fail");
			if (t.flaky) { ImGui::SameLine(); Pill("불안정", col::Warn); }
			ImGui::PushTextWrapPos(0);
			Title(t.name);
			ImGui::PopTextWrapPos();
			SectionHeader("최근 실행");
			ResultStrip(t.history);
			if (t.detail)
			{
				SectionHeader("오류");
				const json& errors = Arr(*t.detail, "errors");
				if (errors.empty()) TextDim("없음");
				for (auto& e : errors) { ImGui::PushTextWrapPos(0); TextColored(col::Fail, e.is_string() ? e.get<std::string>().c_str() : e.dump().c_str()); ImGui::PopTextWrapPos(); }
				SectionHeader("세부 정보");
				DetailJson(*t.detail);
			}
			return;
		}
	TextDim("테스트나 맵을 고르면 자세한 내용이 여기에 표시됩니다.");
}

// ============================================================================ logs

static void LogGroupList(const json& groups, const char* id)
{
	ImGui::PushID(id);
	int i = 0;
	for (auto& g : groups)
	{
		ImGui::PushID(i++);
		SeverityPill(Str(g, "severity"));
		ImGui::SameLine();
		ImGui::PushTextWrapPos(0);
		ImGui::TextUnformatted((Str(g, "sample") + Fmt("  ×%lld", Num(g, "count"))).c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopID();
	}
	if (groups.empty()) TextDim("없음");
	ImGui::PopID();
}

static void DrawLogsNav(App& app)
{
	const RunInfo* r = app.Latest(2);
	SectionHeader("심각도");
	if (!r) { TextDim("로그 분석 기록 없음"); return; }
	const json& logs = Obj(app.Outputs(*r), "logs");
	const json& c = Obj(logs, "counts");
	const char* sev[] = { "", "Fatal", "Error", "Warning" };
	const char* names[] = { "전체", "치명", "오류", "경고" };
	for (int i = 0; i < 4; ++i)
	{
		long long n = i == 0 ? (long long)Arr(logs, "groups").size() : 0;
		if (i) for (auto& g : Arr(logs, "groups")) n += Str(g, "severity") == sev[i];
		if (ImGui::Selectable(Fmt("%s  (%lld 그룹)", names[i], n).c_str(), ui.logSeverity == sev[i])) ui.logSeverity = sev[i];
	}
	SectionHeader("줄 수");
	KeyValue("Fatal", std::to_string(Num(c, "Fatal")), 70);
	KeyValue("Error", std::to_string(Num(c, "Error")), 70);
	KeyValue("Warning", std::to_string(Num(c, "Warning")), 70);
	SectionHeader("크래시");
	const json& crashes = Arr(logs, "crashes");
	if (crashes.empty()) TextDim("없음");
	for (auto& cr : crashes) { TextColored(col::Fail, ICON_BUG); ImGui::SameLine(); ImGui::TextWrapped("%s", Str(cr, "error", Str(cr, "dir")).c_str()); }
	SectionHeader("파일");
	for (auto& f : Arr(logs, "files"))
	{
		std::string p = f.is_string() ? f.get<std::string>() : f.dump();
		if (ImGui::Selectable(PathUtf8(fs::u8path(p).filename()).c_str())) OpenWithShell(fs::u8path(p));
		Tooltip(p.c_str());
	}
}

static void DrawLogsMain(App& app)
{
	const RunInfo* r = app.Latest(2);
	ViewHeader(ICON_LIST, "로그 · 크래시", r ? StampOf(r->id) : "기록 없음");
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(150));
	RunLineButton(app, 2, true, ICON_PLAY " 로그 분석");
	if (!r)
	{
		if (EmptyState(ICON_LIST, "로그 분석 기록이 없습니다", "Saved/Logs와 Saved/Crashes를 읽어 같은 메시지를 지문으로 묶고, 이전 실행 대비 새 오류를 표시합니다.", app.attached ? "로그 분석" : nullptr))
			app.Run("logs");
		return;
	}
	auto history = app.RunsOf(2, 12);
	SearchBox("##lfilter", ui.filter, sizeof(ui.filter), ICON_SEARCH "  메시지, 카테고리", S(320));
	ImGui::SameLine();
	const char* sorts[] = { "심각도순", "횟수순", "새 그룹 먼저" };
	ImGui::SetNextItemWidth(S(130));
	ImGui::Combo("##lsort", &ui.logSort, sorts, 3);
	const json& logs = Obj(app.Outputs(*r), "logs");
	std::vector<const json*> groups;
	for (auto& g : Arr(logs, "groups"))
	{
		if (!ui.logSeverity.empty() && Str(g, "severity") != ui.logSeverity) continue;
		if (!ContainsNoCase(Str(g, "sample") + Str(g, "category"), ui.filter)) continue;
		groups.push_back(&g);
	}
	std::stable_sort(groups.begin(), groups.end(), [](const json* a, const json* b) {
		if (ui.logSort == 1) return Num(*a, "count") > Num(*b, "count");
		if (ui.logSort == 2 && a->value("new", false) != b->value("new", false)) return a->value("new", false);
		return SeverityRank(Str(*a, "severity")) > SeverityRank(Str(*b, "severity"));
	});
	if (ImGui::BeginTable("logs", 5, kTable))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("수준", ImGuiTableColumnFlags_WidthFixed, S(64));
		ImGui::TableSetupColumn("메시지", ImGuiTableColumnFlags_WidthStretch, 4);
		ImGui::TableSetupColumn("카테고리", ImGuiTableColumnFlags_WidthFixed, S(150));
		ImGui::TableSetupColumn("횟수", ImGuiTableColumnFlags_WidthFixed, S(60));
		ImGui::TableSetupColumn("추세", ImGuiTableColumnFlags_WidthFixed, S(90));
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin((int)groups.size());
		while (clip.Step())
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
			{
				const json& g = *groups[i];
				std::string fp = Str(g, "fingerprint");
				ImGui::PushID(fp.c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); SeverityPill(Str(g, "severity"));
				ImGui::TableNextColumn();
				if (g.value("new", false)) { Pill("NEW", col::Info, true); ImGui::SameLine(); }
				if (ImGui::Selectable(Str(g, "sample").c_str(), ui.logSel == fp, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) ui.logSel = fp;
				ImGui::TableNextColumn(); TextDim(Str(g, "category"));
				ImGui::TableNextColumn(); ImGui::Text("%lld", Num(g, "count"));
				ImGui::TableNextColumn();
				{
					std::vector<float> trend;
					for (auto* h : history)
					{
						float v = 0;
						for (auto& hg : Arr(Obj(app.Outputs(*h), "logs"), "groups"))
							if (Str(hg, "fingerprint") == fp) { v = (float)Num(hg, "count"); break; }
						trend.push_back(v);
					}
					ImVec2 p = ImGui::GetCursorScreenPos();
					float hgt = ImGui::GetTextLineHeight();
					Sparkline(ImGui::GetWindowDrawList(), p, p + ImVec2(S(80), hgt), trend, SemanticColor(Str(g, "severity")), false);
					ImGui::Dummy(ImVec2(S(80), hgt));
				}
				ImGui::PopID();
			}
		ImGui::EndTable();
	}
}

static void DrawLogsInspector(App& app)
{
	const RunInfo* r = app.Latest(2);
	if (!r || ui.logSel.empty()) { TextDim("로그 그룹을 고르면 자세한 내용이 여기에 표시됩니다."); return; }
	for (auto& g : Arr(Obj(app.Outputs(*r), "logs"), "groups"))
	{
		if (Str(g, "fingerprint") != ui.logSel) continue;
		SeverityPill(Str(g, "severity"));
		ImGui::SameLine();
		TextDim(Str(g, "category"));
		ImGui::PushTextWrapPos(0);
		Title(Str(g, "sample"));
		ImGui::PopTextWrapPos();
		KeyValue("횟수", std::to_string(Num(g, "count")), 70);
		KeyValue("지문", ui.logSel, 70);
		// first and last seen across log runs
		std::string first, last;
		for (auto* h : app.RunsOf(2, 50))
			for (auto& hg : Arr(Obj(app.Outputs(*h), "logs"), "groups"))
				if (Str(hg, "fingerprint") == ui.logSel) { if (first.empty()) first = h->id; last = h->id; }
		KeyValue("처음", StampOf(first), 70);
		KeyValue("마지막", StampOf(last), 70);
		if (ImGui::Button((std::string(ICON_COPY) + " 메시지 복사").c_str())) CopyToClipboard(Str(g, "sample"));
		SectionHeader("위치");
		for (auto& w : Arr(g, "where")) ImGui::TextWrapped("%s", w.is_string() ? w.get<std::string>().c_str() : w.dump().c_str());
		SectionHeader("세부 (콜스택 등)");
		ImGui::PushFont(g_fonts.mono);
		for (auto& d : Arr(g, "detail")) ImGui::TextWrapped("%s", d.is_string() ? d.get<std::string>().c_str() : d.dump().c_str());
		ImGui::PopFont();
		return;
	}
	TextDim("선택한 그룹이 최신 실행에 없습니다.");
}

// ============================================================================ performance (perf, sizes, trace)

static void FrameChart(const json& perf, float height)
{
	const json& series = Obj(perf, "series");
	const json& frame = Arr(series, "FrameTime");
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 size(ImGui::GetContentRegionAvail().x, height);
	ImGui::InvisibleButton("chart", size);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + size, col::Panel, S(6));
	if (frame.empty())
	{
		dl->AddText(p + ImVec2(S(12), S(10)), col::Dim, "프레임 시계열이 없습니다.");
		return;
	}
	double budget = Real(perf, "budgetMs", 33.3);
	double hi = budget * 1.5;
	for (auto& v : frame) hi = ImMax(hi, v.get<double>());
	for (const char* k : { "GameThreadTime", "RenderThreadTime" })
		for (auto& v : Arr(series, k)) hi = ImMax(hi, v.get<double>());
	ImVec2 a = p + ImVec2(S(40), S(10)), b = p + size - ImVec2(S(10), S(20));
	auto y = [&](double ms) { return b.y - (float)(ms / hi) * (b.y - a.y); };
	for (double ms : { 8.33, 16.67, 33.33, 50.0, 100.0 })
	{
		if (ms > hi) break;
		dl->AddLine(ImVec2(a.x, y(ms)), ImVec2(b.x, y(ms)), IM_COL32(255, 255, 255, 14));
		std::string t = Fmt("%.0f", ms);
		dl->AddText(ImVec2(p.x + S(8), y(ms) - S(7)), col::Muted, t.c_str());
	}
	struct S2 { const char* key; ImU32 color; };
	for (S2 s : { S2{ "GameThreadTime", WithAlpha(col::Teal, 0.8f) }, S2{ "RenderThreadTime", WithAlpha(col::Purple, 0.8f) }, S2{ "FrameTime", col::Info } })
	{
		const json& vals = Arr(series, s.key);
		if (vals.size() < 2) continue;
		std::vector<ImVec2> pts;
		for (size_t i = 0; i < vals.size(); ++i)
			pts.push_back(ImVec2(a.x + (b.x - a.x) * i / (float)(vals.size() - 1), y(vals[i].get<double>())));
		dl->AddPolyline(pts.data(), (int)pts.size(), s.color, 0, S(1.3f));
	}
	dl->AddLine(ImVec2(a.x, y(budget)), ImVec2(b.x, y(budget)), col::Fail, S(1));
	std::string bt = Fmt("예산 %.1fms", budget);
	dl->AddText(ImVec2(b.x - S(90), y(budget) - S(16)), col::Fail, bt.c_str());
	// legend
	float lx = a.x;
	for (S2 s : { S2{ "프레임", col::Info }, S2{ "게임 스레드", col::Teal }, S2{ "렌더 스레드", col::Purple } })
	{
		dl->AddRectFilled(ImVec2(lx, b.y + S(7)), ImVec2(lx + S(10), b.y + S(15)), s.color, 2);
		dl->AddText(ImVec2(lx + S(14), b.y + S(3)), col::Dim, s.key);
		lx += S(100);
	}
	if (ImGui::IsItemHovered())
	{
		float mx = ImGui::GetIO().MousePos.x;
		float t = ImClamp((mx - a.x) / (b.x - a.x), 0.f, 1.f);
		size_t i = (size_t)std::round(t * (frame.size() - 1));
		dl->AddLine(ImVec2(mx, a.y), ImVec2(mx, b.y), IM_COL32(255, 255, 255, 60));
		ImGui::BeginTooltip();
		ImGui::Text("샘플 %d", (int)i);
		for (const char* k : { "FrameTime", "GameThreadTime", "RenderThreadTime" })
			if (i < Arr(series, k).size()) ImGui::Text("%s  %.2fms", k, Arr(series, k)[i].get<double>());
		ImGui::EndTooltip();
	}
}

// Squarified treemap of (label, bytes).
static void Treemap(const std::vector<std::pair<std::string, double>>& items, ImVec2 size, std::string& selected)
{
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("treemap", size);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	double total = 0;
	for (auto& it : items) total += it.second;
	if (total <= 0) return;
	struct Rect { float x, y, w, h; };
	std::vector<Rect> rects(items.size());
	double scale = (double)size.x * size.y / total;
	size_t i = 0;
	Rect area{ origin.x, origin.y, size.x, size.y };
	while (i < items.size())
	{
		float side = ImMin(area.w, area.h);
		size_t j = i;
		double sum = 0;
		double worst = DBL_MAX;
		while (j < items.size())
		{
			double s2 = sum + items[j].second * scale;
			double mx = items[i].second * scale, mn = items[j].second * scale;
			double w2 = ImMax(side * side * mx / (s2 * s2), (s2 * s2) / (side * side * mn));
			if (w2 > worst && j > i) break;
			worst = w2;
			sum = s2;
			++j;
		}
		float thick = (float)(sum / side);
		float off = 0;
		for (size_t k = i; k < j; ++k)
		{
			float len = (float)(items[k].second * scale / thick);
			rects[k] = area.w >= area.h ? Rect{ area.x, area.y + off, thick, len } : Rect{ area.x + off, area.y, len, thick };
			off += len;
		}
		if (area.w >= area.h) { area.x += thick; area.w -= thick; }
		else { area.y += thick; area.h -= thick; }
		i = j;
	}
	ImU32 palette[] = { col::Accent, col::Teal, col::Purple, col::Warn, col::Info, col::Pass, col::Minor };
	ImVec2 mouse = ImGui::GetIO().MousePos;
	for (size_t k = 0; k < items.size(); ++k)
	{
		const Rect& r = rects[k];
		ImVec2 a(r.x + 1, r.y + 1), b(r.x + r.w - 1, r.y + r.h - 1);
		if (b.x <= a.x || b.y <= a.y) continue;
		bool hover = ImGui::IsItemHovered() && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
		ImU32 c = palette[k % 7];
		dl->AddRectFilled(a, b, WithAlpha(c, items[k].first == selected ? 0.75f : hover ? 0.55f : 0.35f), S(3));
		dl->AddRect(a, b, WithAlpha(c, 0.8f), S(3));
		if (r.w > S(60) && r.h > S(34))
		{
			dl->PushClipRect(a, b, true);
			std::string name = PathUtf8(fs::u8path(items[k].first).filename());
			if (name.empty()) name = items[k].first;
			dl->AddText(a + ImVec2(S(5), S(3)), col::Text, name.c_str());
			std::string bytes = Bytes(items[k].second);
			dl->AddText(a + ImVec2(S(5), S(19)), col::Dim, bytes.c_str());
			dl->PopClipRect();
		}
		if (hover)
		{
			ImGui::SetTooltip("%s\n%s (%.1f%%)", items[k].first.c_str(), Bytes(items[k].second).c_str(), 100.0 * items[k].second / total);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selected = items[k].first;
		}
	}
}

static void DrawPerformanceNav(App& app)
{
	const char* tabs[] = { "프레임 성능", "콘텐츠 용량", "Insights 트레이스" };
	const char* icons[] = { ICON_SPEED, ICON_PACKAGE, ICON_DIAG };
	SectionHeader("보기");
	for (int i = 0; i < 3; ++i)
		if (ImGui::Selectable((std::string(icons[i]) + "  " + tabs[i]).c_str(), ui.perfTab == i)) ui.perfTab = i;
	int line = 4 + ui.perfTab;
	std::string& sel = ui.perfTab == 0 ? ui.perfRun : ui.perfTab == 1 ? ui.sizesRun : ui.traceRun;
	SectionHeader("실행 기록");
	int n = 0;
	for (auto& r : app.runs)
	{
		if (r.lineIndex != line) continue;
		ImGui::PushID(r.id.c_str());
		StatusDot(SemanticColor(r.verdict));
		ImGui::SameLine();
		bool isSel = sel == r.id || (sel.empty() && n == 0);
		if (ImGui::Selectable(StampOf(r.id).c_str(), isSel)) sel = r.id;
		ImGui::PopID();
		++n;
	}
	if (!n) TextDim("기록 없음");
}

static void DrawPerformanceMain(App& app)
{
	int line = 4 + ui.perfTab;
	std::string& sel = ui.perfTab == 0 ? ui.perfRun : ui.perfTab == 1 ? ui.sizesRun : ui.traceRun;
	const RunInfo* r = sel.empty() ? app.Latest(line) : app.FindRun(sel);
	if (!r) r = app.Latest(line);
	ViewHeader(kLines[line].icon, kLines[line].name, r ? StampOf(r->id) : "기록 없음");
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(150));
	RunLineButton(app, line, true, (std::string(ICON_PLAY) + " " + kLines[line].command).c_str());
	if (!r)
	{
		if (EmptyState(kLines[line].icon, (std::string(kLines[line].name) + " 기록이 없습니다").c_str(), kLines[line].desc, app.attached ? "실행" : nullptr))
			app.Run(kLines[line].command);
		return;
	}
	const json& d = app.Outputs(*r);
	if (ui.perfTab == 0)
	{
		const json& perf = Obj(d, "perf");
		const json& stats = Obj(perf, "stats");
		TextDim(Fmt("맵 %s  ·  프레임 %lld  ·  예산 %.1fms", Str(perf, "map").c_str(), Num(perf, "frames"), Real(perf, "budgetMs", 33.3)));
		float avail = ImGui::GetContentRegionAvail().x;
		float w = (avail - 3 * ImGui::GetStyle().ItemSpacing.x) / 4;
		const json& ft = Obj(stats, "FrameTime");
		std::vector<float> trend = TrendOf(app, 4, 12, [](const RunMetrics& m) { return (float)m.perfP95; });
		KpiCard("pavg", "프레임 평균", Fmt("%.1fms", Real(ft, "avg")), Fmt("%.0f FPS", Real(ft, "avg") > 0 ? 1000.0 / Real(ft, "avg") : 0.0), col::Dim, {}, col::Info, w);
		ImGui::SameLine();
		KpiCard("p95", "p95", Fmt("%.1fms", Real(ft, "p95")), "", col::Dim, trend, Real(ft, "p95") > Real(perf, "budgetMs", 33.3) ? col::Fail : col::Pass, w, "실행 기록별 p95 추세");
		ImGui::SameLine();
		KpiCard("p99", "p99", Fmt("%.1fms", Real(ft, "p99")), Fmt("최대 %.1fms", Real(ft, "max")), col::Dim, {}, col::Warn, w);
		ImGui::SameLine();
		KpiCard("hitch", "히치", std::to_string(Arr(perf, "hitches").size()), "기준 초과 프레임", col::Dim, {}, col::Fail, w);
		SectionHeader("프레임 시간");
		FrameChart(perf, S(230));
		SectionHeader("스레드별 통계");
		if (ImGui::BeginTable("pstats", 6, kTable, ImVec2(0, S(150))))
		{
			for (const char* h : { "지표", "평균", "p50", "p95", "p99", "최대" }) ImGui::TableSetupColumn(h);
			ImGui::TableHeadersRow();
			for (auto it = stats.begin(); it != stats.end(); ++it)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextUnformatted(it.key().c_str());
				for (const char* k : { "avg", "p50", "p95", "p99", "max" }) { ImGui::TableNextColumn(); ImGui::Text("%.2f", Real(it.value(), k)); }
			}
			ImGui::EndTable();
		}
		if (!Str(perf, "csv").empty() && ImGui::Button("CSV 열기")) OpenWithShell(fs::u8path(Str(perf, "csv")));
	}
	else if (ui.perfTab == 1)
	{
		const json& sizes = Obj(d, "sizes");
		TextDim(Fmt("전체 %s  ·  에셋 %lld개", Bytes(Real(sizes, "totalBytes")).c_str(), Num(sizes, "assetCount")));
		std::vector<std::pair<std::string, double>> folders;
		for (auto& f : Arr(sizes, "byFolder")) folders.push_back({ Str(f, "folder"), Real(f, "bytes") });
		std::sort(folders.begin(), folders.end(), [](auto& a, auto& b) { return a.second > b.second; });
		if (folders.size() > 40) folders.resize(40);
		SectionHeader("폴더별 용량", "클릭해서 선택");
		if (!folders.empty()) Treemap(folders, ImVec2(ImGui::GetContentRegionAvail().x, S(230)), ui.sizesSel);
		if (ImGui::BeginTabBar("sizetabs"))
		{
			auto table = [&](const char* id, const json& rows, const char* nameKey, const char* bytesKey, const char* extraKey, const char* extraName) {
				if (ImGui::BeginTable(id, extraKey ? 3 : 2, kTable))
				{
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("용량", ImGuiTableColumnFlags_WidthFixed, S(90));
					if (extraKey) ImGui::TableSetupColumn(extraName, ImGuiTableColumnFlags_WidthFixed, S(90));
					ImGui::TableHeadersRow();
					double top = rows.empty() ? 1 : ImMax(1.0, Real(rows[0], bytesKey));
					for (auto& row : rows)
					{
						if (!ContainsNoCase(Str(row, nameKey), ui.filter)) continue;
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						if (ImGui::Selectable(Str(row, nameKey).c_str(), ui.sizesSel == Str(row, nameKey), ImGuiSelectableFlags_SpanAllColumns)) ui.sizesSel = Str(row, nameKey);
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(Bytes(Real(row, bytesKey)).c_str());
						Meter((float)(Real(row, bytesKey) / top), col::Accent, nullptr, S(80), S(3));
						if (extraKey) { ImGui::TableNextColumn(); ImGui::Text("%lld", Num(row, extraKey)); }
					}
					ImGui::EndTable();
				}
			};
			if (ImGui::BeginTabItem("큰 에셋")) { table("largest", Arr(sizes, "largest"), "asset", "bytes", nullptr, nullptr); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("함께 로드되는 무게"))
			{
				TextDim("에셋을 로드할 때 하드 참조로 함께 로드되는 에셋 용량의 합 (최신 분석 결과 기준)");
				table("closures", Arr(sizes, "heaviestClosures"), "asset", "closureBytes", "closureCount", "에셋 수");
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("유형별")) { table("bytype", Arr(sizes, "byType"), "ext", "bytes", "count", "개수"); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("폴더별")) { table("byfolder", Arr(sizes, "byFolder"), "folder", "bytes", "count", "개수"); ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}
	}
	else
	{
		const json& tr = Obj(d, "trace");
		fs::path file = fs::u8path(Str(tr, "file"));
		KeyValue("파일", PathUtf8(file));
		KeyValue("크기", Bytes(Real(tr, "bytes")));
		KeyValue("캡처 시간", Fmt("%.0f초", Real(tr, "seconds")));
		ImGui::Spacing();
		std::string insights = Str(tr, "insights");
		std::error_code ec;
		ImGui::BeginDisabled(insights.empty() || !fs::exists(file, ec));
		if (PrimaryButton((std::string(ICON_OPEN) + "  Unreal Insights에서 열기").c_str()))
			ShellExecuteW(nullptr, L"open", fs::u8path(insights).wstring().c_str(), (L"\"" + file.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button((std::string(ICON_FOLDER) + "  폴더에서 보기").c_str())) RevealInExplorer(file);
		ImGui::Spacing();
		TextDim("Unreal Insights에서 타이밍, 프레임, 메모리를 분석합니다. 트레이스 채널과 캡처 시간은 config의 trace 항목에서 바꿉니다.");
	}
}

static void DrawPerformanceInspector(App& app)
{
	if (ui.perfTab == 1 && !ui.sizesSel.empty())
	{
		Title(PathUtf8(fs::u8path(ui.sizesSel).filename()));
		ImGui::TextWrapped("%s", ui.sizesSel.c_str());
		const RunInfo* r = ui.sizesRun.empty() ? app.Latest(5) : app.FindRun(ui.sizesRun);
		if (!r) return;
		const json& sizes = Obj(app.Outputs(*r), "sizes");
		for (const char* key : { "largest", "heaviestClosures", "byFolder" })
			for (auto& row : Arr(sizes, key))
				if (Str(row, "asset", Str(row, "folder")) == ui.sizesSel)
				{
					SectionHeader(key);
					DetailJson(row);
				}
		return;
	}
	if (ui.perfTab == 0)
	{
		const RunInfo* r = ui.perfRun.empty() ? app.Latest(4) : app.FindRun(ui.perfRun);
		if (!r) { TextDim("선택 항목 없음"); return; }
		const json& perf = Obj(app.Outputs(*r), "perf");
		SectionHeader("히치", std::to_string(Arr(perf, "hitches").size()).c_str());
		for (auto& h : Arr(perf, "hitches")) ImGui::BulletText("프레임 %lld  ·  %.1fms", Num(h, "frame"), Real(h, "ms"));
		SectionHeader("p95 추세");
		for (auto* pr : app.RunsOf(4, 10))
		{
			const RunMetrics& m = app.Metrics(*pr);
			ImGui::Text("%s  %.1fms", StampOf(pr->id).c_str(), m.perfP95);
		}
		return;
	}
	TextDim("선택 항목 없음");
}

// ============================================================================ history and comparison

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

static std::string Between(const std::string& s, const std::string& open, const std::string& close)
{
	size_t a = s.find(open);
	size_t b = s.rfind(close);
	if (a == std::string::npos || b == std::string::npos || b < a) return {};
	return s.substr(a + open.size(), b - a - open.size());
}

// One self-contained page with the latest report of every line. Returns an error message or "".
static std::string WriteBundleReport(App& app, const fs::path& dest)
{
	std::string style, body, mermaid;
	std::string cover = "<section><div class='meta'>dwo-Lens 종합 리포트</div><h1>" + HtmlEscape(app.projectName) +
	                    "</h1><div class='scroll'><table><thead><tr><th>라인</th><th>실행 ID</th><th>판정</th></tr></thead><tbody>";
	int parts = 0;
	for (int i = 0; i < kLineCount; ++i)
	{
		const RunInfo* r = app.Latest(i);
		if (!r)
		{
			cover += std::string("<tr><td>") + kLines[i].name + "</td><td colspan='2'>실행 기록 없음</td></tr>";
			continue;
		}
		std::string html = ReadText(r->dir / "out" / "report.html");
		cover += std::string("<tr><td>") + kLines[i].name + "</td><td><code>" + HtmlEscape(r->id) + "</code></td><td><span class='badge " +
		         HtmlEscape(r->verdict) + "'>" + HtmlEscape(r->verdict) + "</span></td></tr>";
		if (html.empty()) continue;
		++parts;
		if (style.empty()) style = Between(html, "<style>", "</style>");
		body += std::string("<section><h1>") + kLines[i].name + "</h1><div class='meta'>" + HtmlEscape(r->id) + "</div></section>" + Between(html, "<main>", "</main>");
		if (mermaid.empty() && html.find("mermaid.min.js") != std::string::npos)
		{
			size_t at = html.find("<script src=");
			size_t end = html.rfind("</script>");
			if (at != std::string::npos && end != std::string::npos) mermaid = html.substr(at, end + 9 - at);
		}
	}
	if (!parts) return "묶을 실행 기록이 없습니다.";
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
	if (!PickSaveFile(g_hwnd, ToWide(app.projectName + "_dwo-lens_report.html"), L"HTML 리포트", L"*.html", L"html", dest)) return;
	std::string err = WriteBundleReport(app, dest);
	if (err.empty()) PushToast("종합 리포트 저장됨", PathUtf8(dest), col::Pass, [dest] { OpenWithShell(dest); }, "열기");
	else PushToast("저장 실패", err, col::Fail);
}

static void SaveRunReport(App& app, const RunInfo& r)
{
	fs::path src = r.dir / "out" / "report.html";
	std::error_code ec;
	if (!fs::exists(src, ec)) { PushToast("저장할 수 없음", "이 실행에는 report.html이 없습니다.", col::Warn); return; }
	fs::path dest;
	if (!PickSaveFile(g_hwnd, ToWide(app.projectName + "_" + r.id + ".html"), L"HTML 리포트", L"*.html", L"html", dest)) return;
	if (!CopyFileW(src.wstring().c_str(), dest.wstring().c_str(), FALSE)) { PushToast("저장 실패", "error " + std::to_string(GetLastError()), col::Fail); return; }
	PushToast("리포트 저장됨", PathUtf8(dest), col::Pass, [dest] { OpenWithShell(dest); }, "열기");
}

static void StationStrip(const RunInfo& r)
{
	float s = S(14), gap = S(3);
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = ImGui::GetTextLineHeight();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const json& stations = Arr(r.run, "stations");
	int hoveredIndex = -1;
	for (size_t i = 0; i < stations.size(); ++i)
	{
		ImVec2 a(p.x + i * (s + gap), p.y + (h - s) * 0.5f);
		dl->AddRectFilled(a, a + ImVec2(s, s), SemanticColor(Str(stations[i], "verdict")), S(3));
		if (ImGui::IsMouseHoveringRect(a, a + ImVec2(s, s))) hoveredIndex = (int)i;
	}
	ImGui::Dummy(ImVec2(ImMax(1.f, stations.size() * (s + gap)), h));
	if (hoveredIndex >= 0)
	{
		const json& st = stations[hoveredIndex];
		ImGui::SetTooltip("%s %s\n%s · %.1f초", Str(st, "id").c_str(), Str(st, "name").c_str(), Str(st, "verdict").c_str(), Num(st, "ms") / 1000.0);
	}
}

static void DrawCompare(App& app, const RunInfo& a, const RunInfo& b)
{
	SectionHeader(("비교  " + StampOf(a.id) + "  →  " + StampOf(b.id)).c_str());
	if (a.lineIndex != b.lineIndex) { TextColored(col::Warn, "같은 라인의 실행끼리 비교하세요."); return; }
	std::vector<Problem> pa, pb;
	App::CollectProblems(app, a, pa);
	App::CollectProblems(app, b, pb);
	std::map<std::string, const Problem*> ma, mb;
	for (auto& p : pa) ma[p.key] = &p;
	for (auto& p : pb) mb[p.key] = &p;
	int added = 0, removed = 0;
	for (auto& [k, p] : mb) added += !ma.count(k);
	for (auto& [k, p] : ma) removed += !mb.count(k);
	const RunMetrics& x = app.Metrics(a);
	const RunMetrics& y = app.Metrics(b);
	auto row = [](const char* name, double before, double after, bool lowerBetter, const char* fmt = "%.0f") {
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
		ImGui::TableNextColumn(); ImGui::Text(fmt, before);
		ImGui::TableNextColumn(); ImGui::Text(fmt, after);
		ImGui::TableNextColumn();
		double d = after - before;
		if (std::fabs(d) < 1e-9) TextDim("-");
		else TextColored((d < 0) == lowerBetter ? col::Pass : col::Fail, Fmt(d > 0 ? "+%.1f" : "%.1f", d).c_str());
	};
	if (ImGui::BeginTable("cmp", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
	{
		for (const char* h : { "지표", "이전", "이후", "변화" }) ImGui::TableSetupColumn(h);
		ImGui::TableHeadersRow();
		row("문제", (double)pa.size(), (double)pb.size(), true);
		row("발견 (실패+경고)", x.findingsFail + x.findingsWarn, y.findingsFail + y.findingsWarn, true);
		row("BP 복잡도 초과", x.bpOver, y.bpOver, true);
		row("이름 규칙", x.naming, y.naming, true);
		row("테스트 실패", x.testsFail, y.testsFail, true);
		row("로그 오류", x.logError + x.logFatal, y.logError + y.logFatal, true);
		row("로그 경고", x.logWarning, y.logWarning, true);
		row("스모크 실패 맵", x.mapsFail, y.mapsFail, true);
		if (x.perfP95 >= 0 || y.perfP95 >= 0) row("프레임 p95 (ms)", x.perfP95, y.perfP95, true, "%.1f");
		if (x.contentBytes >= 0 || y.contentBytes >= 0) row("콘텐츠 (MB)", x.contentBytes / 1048576.0, y.contentBytes / 1048576.0, true, "%.1f");
		row("소요 (초)", a.seconds, b.seconds, true, "%.1f");
		ImGui::EndTable();
	}
	SectionHeader(Fmt("새로 생긴 문제 %d", added).c_str());
	for (auto& [k, p] : mb) if (!ma.count(k)) { SeverityPill(p->severity); ImGui::SameLine(); ImGui::TextWrapped("%s  ·  %s", p->title.c_str(), p->object.c_str()); }
	SectionHeader(Fmt("해결된 문제 %d", removed).c_str());
	for (auto& [k, p] : ma) if (!mb.count(k)) { TextColored(col::Pass, ICON_CHECK); ImGui::SameLine(); ImGui::TextWrapped("%s  ·  %s", p->title.c_str(), p->object.c_str()); }
	if (!x.tests.empty() || !y.tests.empty())
	{
		SectionHeader("결과가 바뀐 테스트");
		for (auto& [name, st] : y.tests)
		{
			auto it = x.tests.find(name);
			if (it != x.tests.end() && it->second == st) continue;
			TextColored(st == 'p' ? col::Pass : col::Fail, st == 'p' ? ICON_OK : ICON_ERROR);
			ImGui::SameLine();
			ImGui::TextUnformatted((name + (it == x.tests.end() ? "  (새 테스트)" : "")).c_str());
		}
	}
}

static void DrawHistoryNav(App& app)
{
	SectionHeader("라인");
	if (ImGui::Selectable(Fmt("전체  (%d)", (int)app.runs.size()).c_str(), ui.historyLine < 0)) ui.historyLine = -1;
	for (int i = 0; i < kLineCount; ++i)
	{
		int n = 0;
		for (auto& r : app.runs) n += r.lineIndex == i;
		if (ImGui::Selectable(Fmt("%s  %s  (%d)", kLines[i].icon, kLines[i].name, n).c_str(), ui.historyLine == i)) ui.historyLine = i;
	}
	SectionHeader("리포트");
	if (ImGui::Button((std::string(ICON_SAVE) + "  종합 리포트 저장...").c_str(), ImVec2(-FLT_MIN, 0))) SaveBundleReport(app);
	Tooltip("라인마다 최신 실행의 리포트를 표지와 함께 HTML 파일 하나로 저장합니다.");
}

static void DrawHistoryMain(App& app)
{
	ViewHeader(ICON_HISTORY, "실행 기록", Fmt("%d회", (int)app.runs.size()));
	if (app.runs.empty())
	{
		EmptyState(ICON_HISTORY, "실행 기록이 없습니다", "라인을 실행하면 이곳에 판정·공정·소요 시간이 쌓이고, 두 실행을 골라 비교할 수 있습니다.");
		return;
	}
	TextDim("체크 두 개를 고르면 비교합니다. 두 번 클릭하면 결과 화면으로 이동합니다.");
	bool comparing = !ui.compareA.empty() && !ui.compareB.empty();
	float tableHeight = comparing ? ImGui::GetContentRegionAvail().y * 0.45f : 0;
	if (ImGui::BeginTable("history", 7, kTable, ImVec2(0, tableHeight)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(26));
		ImGui::TableSetupColumn("판정", ImGuiTableColumnFlags_WidthFixed, S(64));
		ImGui::TableSetupColumn("라인", ImGuiTableColumnFlags_WidthFixed, S(130));
		ImGui::TableSetupColumn("실행 ID", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("공정", ImGuiTableColumnFlags_WidthFixed, S(110));
		ImGui::TableSetupColumn("시작", ImGuiTableColumnFlags_WidthFixed, S(100));
		ImGui::TableSetupColumn("소요", ImGuiTableColumnFlags_WidthFixed, S(80));
		ImGui::TableHeadersRow();
		for (auto& r : app.runs)
		{
			if (ui.historyLine >= 0 && r.lineIndex != ui.historyLine) continue;
			ImGui::PushID(r.id.c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			bool checked = ui.compareA == r.id || ui.compareB == r.id;
			if (ImGui::Checkbox("##cmp", &checked))
			{
				if (checked)
				{
					if (ui.compareA.empty()) ui.compareA = r.id;
					else { if (!ui.compareB.empty()) ui.compareA = ui.compareB; ui.compareB = r.id; }
				}
				else
				{
					if (ui.compareA == r.id) ui.compareA = ui.compareB;
					ui.compareB.clear();
				}
			}
			ImGui::TableNextColumn(); VerdictPill(r.verdict);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(r.lineIndex >= 0 ? kLines[r.lineIndex].name : r.line.c_str());
			ImGui::TableNextColumn();
			if (ImGui::Selectable(r.id.c_str(), ui.historySel == r.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick))
			{
				ui.historySel = r.id;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && r.lineIndex >= 0)
				{
					if (r.lineIndex == 0) ui.archRun = r.id;
					if (r.lineIndex == 4) ui.perfRun = r.id;
					if (r.lineIndex == 5) ui.sizesRun = r.id, ui.perfTab = 1;
					if (r.lineIndex == 6) ui.traceRun = r.id, ui.perfTab = 2;
					app.requestActivity = App::ActivityOfLine(r.lineIndex);
				}
			}
			ImGui::TableNextColumn(); StationStrip(r);
			ImGui::TableNextColumn(); TextDim(StampOf(r.id));
			ImGui::TableNextColumn(); TextDim(Duration(r.seconds));
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (comparing)
	{
		const RunInfo* a = app.FindRun(ui.compareA);
		const RunInfo* b = app.FindRun(ui.compareB);
		if (a && b)
		{
			if (SortKeyOf(a->id) > SortKeyOf(b->id)) std::swap(a, b);
			ImGui::BeginChild("compare");
			DrawCompare(app, *a, *b);
			ImGui::EndChild();
		}
	}
}

static void DrawHistoryInspector(App& app)
{
	const RunInfo* r = app.FindRun(ui.historySel);
	if (!r) { TextDim("실행을 고르면 공정과 산출물이 여기에 표시됩니다."); return; }
	VerdictPill(r->verdict);
	Title(r->lineIndex >= 0 ? kLines[r->lineIndex].name : r->line);
	TextDim(r->id);
	KeyValue("시작", r->startedAt.substr(0, 19), 70);
	KeyValue("소요", Duration(r->seconds), 70);
	KeyValue("시스템", Str(r->run, "systemCommit"), 70);
	ImGui::Spacing();
	if (ImGui::Button((std::string(ICON_PAGE) + " 리포트").c_str())) OpenWithShell(r->dir / "out" / "report.html");
	ImGui::SameLine();
	if (ImGui::Button((std::string(ICON_SAVE) + " HTML 저장").c_str())) SaveRunReport(app, *r);
	ImGui::SameLine();
	if (ImGui::Button(ICON_FOLDER)) OpenWithShell(r->dir);
	Tooltip("실행 폴더 열기");
	SectionHeader("공정");
	for (auto& st : Arr(r->run, "stations"))
	{
		ImGui::PushID(Str(st, "id").c_str());
		StatusDot(SemanticColor(Str(st, "verdict")));
		ImGui::SameLine();
		ImGui::TextUnformatted((Str(st, "id") + "  " + Str(st, "name")).c_str());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(60));
		TextDim(Fmt("%.1f초", Num(st, "ms") / 1000.0));
		std::string log = Str(st, "log");
		if (!log.empty())
		{
			fs::path lp = r->dir / fs::u8path(log);
			if (ImGui::SmallButton(("로그 " + log).c_str())) OpenWithShell(lp);
		}
		ImGui::PopID();
	}
	SectionHeader("산출물");
	std::error_code ec;
	if (fs::is_directory(r->dir / "out", ec))
		for (auto& e : fs::directory_iterator(r->dir / "out", ec))
		{
			std::string name = PathUtf8(e.path().filename());
			if (ImGui::Selectable(name.c_str())) OpenWithShell(e.path());
			if (e.is_regular_file())
			{
				ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(70));
				std::error_code sec;
				TextDim(Bytes((double)e.file_size(sec)));
			}
		}
}

// ============================================================================ tools

static void DrawToolsNav(App& app)
{
	SectionHeader("범주");
	std::map<std::string, int> cats;
	for (auto& t : Arr(app.catalog, "tools")) ++cats[Str(t, "category", Str(t, "station", "기타"))];
	if (ImGui::Selectable(Fmt("전체  (%d)", (int)Arr(app.catalog, "tools").size()).c_str(), ui.toolCategory.empty())) ui.toolCategory.clear();
	for (auto& [c, n] : cats)
		if (ImGui::Selectable(Fmt("%s  (%d)", c.c_str(), n).c_str(), ui.toolCategory == c)) ui.toolCategory = c;
	SectionHeader("관리");
	bool idle = !app.runner.running;
	ImGui::BeginDisabled(!idle || !app.hasUProject);
	float w = -FLT_MIN;
	if (!app.attached)
	{
		if (PrimaryButton((std::string(ICON_LINK) + "  부착").c_str(), ImVec2(w, 0))) app.Run("attach");
	}
	else
	{
		if (ImGui::Button((std::string(ICON_SETTINGS) + "  환경 점검 (doctor)").c_str(), ImVec2(w, 0))) app.Run("doctor");
		if (ImGui::Button((std::string(ICON_INFO) + "  상태 (status)").c_str(), ImVec2(w, 0))) app.Run("status");
		if (ImGui::Button((std::string(ICON_SYNC) + "  시스템 업데이트").c_str(), ImVec2(w, 0))) app.Run("update");
		Tooltip("부착된 시스템을 dwo-Lens 저장소의 최신 커밋으로 맞춥니다.");
		if (DangerButton((std::string(ICON_DELETE) + "  제거 (uninstall)").c_str(), ImVec2(w, 0))) app.confirmDetach = true;
	}
	ImGui::EndDisabled();
}

static void ProvidedToolCard(App& app)
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	float width = ImGui::GetContentRegionAvail().x;
	ImGui::BeginChild("smcard", ImVec2(width, S(170)), ImGuiChildFlags_Borders);
	ImGui::PushFont(g_fonts.title);
	ImGui::TextUnformatted(ICON_EYE "  StateMonitor");
	ImGui::PopFont();
	ImGui::SameLine();
	if (app.stateMonitor == 1) Pill(app.stateMonitorMode == "runtime" ? "설치됨 · 런타임" : "설치됨 · 에디터 전용", col::Pass);
	else if (app.stateMonitor == 2) Pill("프로젝트에 포함", col::Info);
	else Pill("설치 안 됨", col::Muted);
	ImGui::TextWrapped("실행 중인 게임의 상태(GameMode, GameState, 플레이어, Pawn, 컴포넌트, 직접 제출한 USTRUCT)를 dwo-Lens로 실시간 전송합니다. "
	                   "State Monitor 화면에서 노드 그래프·상태 전이·타임라인으로 봅니다. 코드 없이 동작하며 프로젝트 파일은 수정하지 않습니다.");
	ImGui::Spacing();
	bool idle = !app.runner.running;
	ImGui::BeginDisabled(!idle || !app.attached || app.stateMonitor == 2);
	if (app.stateMonitor == 1)
	{
		if (ImGui::Button("제거")) app.Run("sm-remove");
		ImGui::SameLine();
		if (app.stateMonitorMode != "runtime" && ImGui::Button("런타임 모드로 다시 설치")) { app.Run("sm-remove"); app.Run("sm-install --runtime"); }
	}
	else
	{
		if (PrimaryButton("설치 (에디터·PIE 전용)")) app.Run("sm-install");
		Tooltip("에디터와 PIE, 에디터에서 띄운 독립 실행 게임에서 동작합니다. 패키지 빌드에는 영향이 없습니다.");
		ImGui::SameLine();
		if (ImGui::Button("설치 (패키지 게임 포함)")) app.Run("sm-install --runtime");
		Tooltip("패키지 게임에서도 동작합니다. .uproject(BP 전용) 또는 Build.cs(C++)에 StateMonitor를 등록해야 합니다.");
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("State Monitor 열기")) app.requestActivity = Activity::Live;
	ImGui::EndChild();
	(void)p;
}

static void DrawToolsMain(App& app)
{
	ViewHeader(ICON_PUZZLE, "도구", "생산 라인, 제공 도구, 연계 도구");
	SectionHeader("제공 도구");
	ProvidedToolCard(app);
	SectionHeader("생산 라인");
	float avail = ImGui::GetContentRegionAvail().x;
	int columns = ImMax(1, (int)(avail / S(300)));
	float w = (avail - (columns - 1) * ImGui::GetStyle().ItemSpacing.x) / columns;
	for (int i = 0; i < kLineCount; ++i)
	{
		if (i % columns) ImGui::SameLine();
		ImGui::PushID(i);
		ImGui::BeginChild("line", ImVec2(w, S(118)), ImGuiChildFlags_Borders);
		TextBold(std::string(kLines[i].icon) + "  " + kLines[i].name);
		ImGui::SameLine();
		TextDim(kLines[i].command);
		ImGui::PushTextWrapPos(0);
		TextDim(kLines[i].desc);
		ImGui::PopTextWrapPos();
		ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - S(10));
		ImGui::BeginDisabled(!app.attached);
		if (ImGui::SmallButton((std::string(ICON_PLAY) + " 실행").c_str())) app.Run(kLines[i].command);
		ImGui::EndDisabled();
		if (const RunInfo* r = app.Latest(i)) { ImGui::SameLine(); VerdictPill(r->verdict); ImGui::SameLine(); TextDim(StampOf(r->id)); }
		ImGui::EndChild();
		ImGui::PopID();
	}
	SectionHeader("도구 카탈로그", "tools/catalog.json");
	SearchBox("##toolfilter", ui.filter, sizeof(ui.filter), ICON_SEARCH "  도구 검색", S(300));
	if (ImGui::BeginTable("catalog", 4, kTable))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("상태", ImGuiTableColumnFlags_WidthFixed, S(70));
		ImGui::TableSetupColumn("도구", ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn("범주", ImGuiTableColumnFlags_WidthFixed, S(110));
		ImGui::TableSetupColumn("설명", ImGuiTableColumnFlags_WidthStretch, 2.5f);
		ImGui::TableHeadersRow();
		for (auto& t : Arr(app.catalog, "tools"))
		{
			std::string id = Str(t, "id");
			if (!ui.toolCategory.empty() && Str(t, "category", Str(t, "station", "기타")) != ui.toolCategory) continue;
			if (!ContainsNoCase(id + Str(t, "description") + Str(t, "invocation") + Str(t, "command"), ui.filter)) continue;
			ImGui::PushID(id.c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			std::string status = Str(t, "status");
			Pill(status == "ready" ? "사용 가능" : status == "planned" ? "계획" : status, status == "ready" ? col::Pass : col::Muted);
			ImGui::TableNextColumn();
			if (ImGui::Selectable(Str(t, "name", id).c_str(), ui.toolSel == id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) ui.toolSel = id;
			ImGui::TableNextColumn(); TextDim(Str(t, "category", Str(t, "station")));
			ImGui::TableNextColumn(); ImGui::TextWrapped("%s", Str(t, "description", Str(t, "invocation")).c_str());
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

static void DrawToolsInspector(App& app)
{
	for (auto& t : Arr(app.catalog, "tools"))
		if (Str(t, "id") == ui.toolSel)
		{
			Title(Str(t, "name", ui.toolSel));
			TextDim(ui.toolSel);
			ImGui::Spacing();
			DetailJson(t);
			return;
		}
	SectionHeader("dwo-Lens");
	KeyValue("저장소", PathUtf8(app.home), 70);
	KeyValue("시스템", app.SystemCommit(), 70);
	KeyValue("카탈로그", std::to_string(Arr(app.catalog, "tools").size()) + "개 도구", 70);
	ImGui::Spacing();
	if (ImGui::Button("가이드 문서 열기")) OpenWithShell(app.home / "docs" / "guide.html");
	if (ImGui::Button("README 열기")) OpenWithShell(app.home / "README.md");
}
