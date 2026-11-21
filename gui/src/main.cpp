// dwo-Forge control panel: Dear ImGui (docking) + Win32 + DirectX 11.
//
// Runs the same commands as __dwo-forge.bat and reads the same run.json / out/*.json files, so the GUI and the
// command line always show identical results. Groups the three provided tools: DwoLens (analysis lines),
// DwoStateMonitor (live link on 127.0.0.1:47731) and DwoCanvas (design file -> UMG importer).
//
//   dwo-forge-gui.exe [--project <folder with .uproject>] [--home <dwo-Forge repository>]
//                     [--view overview|architecture|problems|tests|logs|performance|live|canvas|history|tools]
//                     [--select <run id>] [--run <command>] [--open-recording <file.jsonl>] [--canvas-file <design.json>]
//   dwo-forge-gui.exe --project <folder> --export-report <file.html>   (no window: every line's latest report in one HTML)
//   dwo-forge-gui.exe ... --screenshot <file.bmp>                      (renders a few frames, saves the window, exits)

#include "views_canvas.h"
#include "views_launcher.h"

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <d3d11.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================ activities

struct ActivityDef
{
	Activity id;
	const char* icon;
	const char* name;
	const char* key;    // --view value
	const char* shortcut;
	ImGuiKey hotkey;    // with Ctrl
	const char* group;  // tool the screen belongs to: DwoLens, DwoStateMonitor, DwoCanvas, CI (Forge = the launcher)
};

// Shortcuts keep their dwo-Lens 1.x numbers (Ctrl+1..8); Ctrl+9 (formerly the tools screen) opens the launcher,
// DwoCanvas takes Ctrl+0.
static const ActivityDef kActivities[] = {
	{ Activity::Launcher, ICON_HOME, "런처", "launcher", "Ctrl+9", ImGuiKey_9, "Forge" },
	{ Activity::Overview, ICON_DASHBOARD, "대시보드", "overview", "Ctrl+1", ImGuiKey_1, "DwoLens" },
	{ Activity::Architecture, ICON_LAYERS, "아키텍처", "architecture", "Ctrl+2", ImGuiKey_2, "DwoLens" },
	{ Activity::Problems, ICON_WARNING, "문제", "problems", "Ctrl+3", ImGuiKey_3, "DwoLens" },
	{ Activity::Tests, ICON_CHECKLIST, "검증", "tests", "Ctrl+4", ImGuiKey_4, "DwoLens" },
	{ Activity::Logs, ICON_LIST, "로그", "logs", "Ctrl+5", ImGuiKey_5, "DwoLens" },
	{ Activity::Performance, ICON_SPEED, "성능·용량", "performance", "Ctrl+6", ImGuiKey_6, "DwoLens" },
	{ Activity::History, ICON_HISTORY, "실행 기록", "history", "Ctrl+8", ImGuiKey_8, "DwoLens" },
	{ Activity::Live, ICON_EYE, "DwoStateMonitor", "live", "Ctrl+7", ImGuiKey_7, "DwoStateMonitor" },
	{ Activity::Canvas, ICON_CANVAS, "DwoCanvas", "canvas", "Ctrl+0", ImGuiKey_0, "DwoCanvas" },
	{ Activity::Ci, ICON_SYNC, "DwoBuilder", "ci", "", ImGuiKey_None, "CI" },
};

// Short caption drawn above each group in the activity bar.
static const char* GroupCaption(const char* group)
{
	std::string g = group;
	if (g == "DwoLens") return "LENS";
	if (g == "DwoStateMonitor") return "STATE";
	if (g == "DwoCanvas") return "CANVAS";
	return "FORGE";
}

static ImU32 GroupColor(const char* group)
{
	std::string g = group;
	if (g == "DwoLens") return col::Accent;
	if (g == "DwoStateMonitor") return col::Pass;
	if (g == "DwoCanvas") return col::Purple;
	if (g == "CI") return col::Teal;
	return col::Dim;
}

static const ActivityDef& DefOf(Activity a)
{
	for (auto& d : kActivities)
		if (d.id == a) return d;
	return kActivities[0];
}

// ============================================================================ command palette

struct Command
{
	std::string label;
	std::string hint;
	std::function<void()> run;
	bool enabled = true;
};

static bool g_paletteOpen = false;
static char g_paletteQuery[256] = {};
static int g_paletteIndex = 0;

static std::vector<Command> BuildCommands(App& app)
{
	std::vector<Command> list;
	for (auto& a : kActivities)
	{
		Activity id = a.id;
		list.push_back({ std::string("이동: ") + (std::string(a.group) == "DwoLens" ? "DwoLens · " : "") + a.name, a.shortcut, [&app, id] { app.requestActivity = id; } });
	}
	for (int i = 0; i < kLineCount; ++i)
	{
		std::string cmd = kLines[i].command;
		std::string owner = IsBuildLine(i) ? "DwoBuilder · " : "DwoLens · ";
		list.push_back({ "실행: " + owner + kLines[i].name, cmd, [&app, cmd] { app.Run(cmd); }, app.attached });
	}
	list.push_back({ "실행: 기본 검사 모두", "analyze, logs, sizes", [&app] { app.RunAll(); }, app.attached && app.lensInstalled });
	list.push_back({ "게임 실행 (DwoStateMonitor 연결)", "play", [&app] { app.Run("play"); app.requestActivity = Activity::Live; }, app.attached });
	for (auto& v : ToolViews(app))
	{
		const ToolMeta* m = ToolMetaOf(v.id);
		std::string id = v.id;
		bool managed = v.state == "installed" || v.state == "update" || v.state == "legacy";
		list.push_back({ std::string(m->name) + " 설치", "tool install " + id, [&app, id] { app.Run("tool install " + id); }, app.attached && v.state == "none" });
		list.push_back({ std::string(m->name) + " 업데이트", "tool update " + id, [&app, id] { app.Run("tool update " + id); }, app.attached && managed });
		list.push_back({ std::string(m->name) + " 제거", "tool remove " + id, [&app, id] { app.Run("tool remove " + id); }, app.attached && managed });
		list.push_back({ std::string(m->name) + " 열기", m->mono, [&app, m] { app.requestActivity = m->open; } });
	}
	list.push_back({ "DwoStateMonitor 설치 (패키지 게임 포함)", "tool install statemonitor --runtime", [&app] { app.Run("tool install statemonitor --runtime"); }, app.attached && app.stateMonitor == 0 });
	list.push_back({ "모든 도구 업데이트", "tool update --all", [&app] { app.Run("tool update --all"); }, app.attached });
	list.push_back({ "DwoCanvas: 디자인 파일 열기...", "*.dwocanvas.json", [&app] { PickDesignFile(app); app.requestActivity = Activity::Canvas; } });
	list.push_back({ "DwoCanvas: 검사", "canvas-validate", [&app] { RunCanvas(app, "canvas-validate"); }, !app.canvasFile.empty() && app.hasUProject });
	list.push_back({ "DwoCanvas: 가져오기", "canvas-import", [&app] { RunCanvas(app, "canvas-import"); }, !app.canvasFile.empty() && app.canvas != 0 });
	list.push_back({ "DwoCanvas: 초기화하고 가져오기", "canvas-import --reset", [&app] { RunCanvas(app, "canvas-import", true); }, !app.canvasFile.empty() && app.canvas != 0 });
	list.push_back({ "종합 리포트 저장...", "HTML", [&app] { SaveBundleReport(app); }, !app.runs.empty() });
	list.push_back({ "프로젝트 열기...", "Ctrl+O", [&app] { fs::path p; if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p); } });
	list.push_back({ "새로고침", "F5", [&app] { app.outputs.clear(); app.metrics.clear(); app.structureFor.clear(); app.Refresh(); } });
	list.push_back({ "프로젝트 연결", "connect", [&app] { app.Run("connect"); }, app.hasUProject && !app.attached });
	list.push_back({ "연결 새로 고침 (도구 업데이트 포함)", "update", [&app] { app.Run("update"); }, app.attached });
	list.push_back({ "환경 점검", "doctor", [&app] { app.Run("doctor"); }, app.attached });
	list.push_back({ "프로젝트 연결 해제...", "uninstall", [&app] { app.confirmDetach = true; }, app.attached });
	list.push_back({ "실행 로그 보기", "Ctrl+`", [&app] { app.showConsole = true; } });
	list.push_back({ "실행 취소", "Esc", [&app] { app.runner.Cancel(); }, (bool)app.runner.running });
	list.push_back({ "DwoStateMonitor 녹화 저장...", "JSONL", [&app] {
		fs::path p;
		if (PickSaveFile(g_hwnd, ToWide(app.projectName + "_state.jsonl"), L"JSON Lines", L"*.jsonl", L"jsonl", p)) app.store.Export(p);
	}, !app.store.recording.empty() });
	list.push_back({ "DwoStateMonitor 녹화 열기...", "JSONL", [&app] {
		fs::path p;
		if (PickOpen(g_hwnd, false, L"DwoStateMonitor 녹화 열기", p, L"JSON Lines", L"*.jsonl")) { app.store.Import(p); app.requestActivity = Activity::Live; }
	} });
	for (auto& r : app.recent)
	{
		fs::path p = fs::u8path(r);
		list.push_back({ "프로젝트 열기: " + PathUtf8(p.filename()), r, [&app, p] { app.SetProject(p); } });
	}
	return list;
}

static void DrawCommandPalette(App& app)
{
	if (!g_paletteOpen) return;
	ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + S(70)), ImGuiCond_Always, ImVec2(0.5f, 0));
	ImGui::SetNextWindowSize(ImVec2(S(560), 0));
	ImGui::OpenPopup("##palette");
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(10)));
	if (ImGui::BeginPopupModal("##palette", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
	{
		bool appearing = ImGui::IsWindowAppearing();
		if (appearing) { g_paletteQuery[0] = 0; g_paletteIndex = 0; ImGui::SetKeyboardFocusHere(); }
		// A click outside closes the palette and does nothing else (the modal keeps it from reaching the window behind).
		bool outside = !appearing && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) &&
		               !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::PushFont(g_fonts.title);
		// Follow the selection only when the keyboard moved it, so the mouse wheel can scroll the list freely.
		bool follow = false;
		if (ImGui::InputTextWithHint("##q", ICON_SEARCH "  명령, 화면, 검사 검색", g_paletteQuery, sizeof(g_paletteQuery))) { g_paletteIndex = 0; follow = true; }
		ImGui::PopFont();
		auto commands = BuildCommands(app);
		std::vector<std::pair<int, int>> scored;
		for (int i = 0; i < (int)commands.size(); ++i)
		{
			int sc = ImMax(FuzzyScore(commands[i].label, g_paletteQuery), FuzzyScore(commands[i].hint, g_paletteQuery));
			if (sc > 0) scored.push_back({ -sc, i });
		}
		std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first < b.first; });
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { g_paletteIndex = ImMin(g_paletteIndex + 1, (int)scored.size() - 1); follow = true; }
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { g_paletteIndex = ImMax(g_paletteIndex - 1, 0); follow = true; }
		bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
		ImGui::BeginChild("list", ImVec2(0, ImMin(S(380), (scored.size() + 1) * ImGui::GetTextLineHeightWithSpacing() * 1.25f)));
		std::function<void()> chosen;
		for (int k = 0; k < (int)scored.size(); ++k)
		{
			Command& c = commands[scored[k].second];
			ImGui::PushID(k);
			ImGui::BeginDisabled(!c.enabled);
			bool selected = k == g_paletteIndex;
			if (ImGui::Selectable(c.label.c_str(), selected, ImGuiSelectableFlags_None, ImVec2(0, ImGui::GetTextLineHeight() * 1.25f)) && c.enabled) chosen = c.run;
			ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize(c.hint.c_str()).x - S(6));
			TextDim(c.hint);
			ImGui::EndDisabled();
			if (selected && follow) ImGui::SetScrollHereY();
			if (selected && enter && c.enabled) chosen = c.run;
			ImGui::PopID();
		}
		if (scored.empty()) TextDim("일치하는 명령이 없습니다.");
		ImGui::EndChild();
		TextDim("↑↓ 이동  ·  Enter 실행  ·  Esc 닫기");
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) || chosen || outside)
		{
			g_paletteOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
		if (chosen) chosen();
	}
	ImGui::PopStyleVar();
}

// ============================================================================ shell parts

static float g_activityWidth = 0;
static float g_statusHeight = 0;

static void DrawMenuBar(App& app, bool& quit)
{
	if (!ImGui::BeginMainMenuBar()) return;
	ImGui::PushFont(g_fonts.bold);
	TextColored(col::Accent, ICON_FORGE);
	ImGui::SameLine(0, S(4));
	ImGui::TextUnformatted("dwo-Forge");
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(S(8), 0));
	if (ImGui::BeginMenu("파일"))
	{
		if (ImGui::MenuItem("프로젝트 열기...", "Ctrl+O")) { fs::path p; if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p); }
		if (ImGui::BeginMenu("최근 프로젝트", !app.recent.empty()))
		{
			for (auto& r : app.recent)
				if (ImGui::MenuItem(r.c_str())) { app.SetProject(fs::u8path(r)); break; }
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("종합 리포트 저장...", nullptr, false, !app.runs.empty())) SaveBundleReport(app);
		if (ImGui::MenuItem("프로젝트 폴더 열기", nullptr, false, app.hasUProject)) OpenWithShell(app.project);
		ImGui::Separator();
		if (ImGui::MenuItem("종료", "Alt+F4")) quit = true;
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("실행"))
	{
		ImGui::TextDisabled("DwoLens");
		for (int i = 0; i < kLineCount; ++i)
		{
			if (i == kLensLineCount) { ImGui::Separator(); ImGui::TextDisabled("DwoBuilder"); }
			if (ImGui::MenuItem((std::string(kLines[i].icon) + "  " + kLines[i].name).c_str(), kLines[i].command, false, app.attached)) app.Run(kLines[i].command);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("기본 검사 모두 실행", nullptr, false, app.attached)) app.RunAll();
		if (ImGui::MenuItem("게임 실행", nullptr, false, app.attached)) app.Run("play");
		ImGui::Separator();
		if (ImGui::MenuItem("실행 취소", "Esc", false, app.runner.running)) app.runner.Cancel();
		if (ImGui::MenuItem("대기열 비우기", nullptr, false, !app.queue.empty())) app.queue.clear();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("보기"))
	{
		const char* group = nullptr;
		for (auto& a : kActivities)
		{
			if (!group || std::string(group) != a.group)
			{
				if (group) ImGui::Separator();
				group = a.group;
				ImGui::TextDisabled("%s", std::string(group) == "Forge" ? "dwo-Forge" : std::string(group) == "CI" ? "DwoBuilder" : group);
			}
			if (ImGui::MenuItem((std::string(a.icon) + "  " + a.name).c_str(), a.shortcut, app.activity == a.id)) app.requestActivity = a.id;
		}
		ImGui::Separator();
		if (ImGui::MenuItem("명령 팔레트", "Ctrl+K")) g_paletteOpen = true;
		if (ImGui::MenuItem("레이아웃 초기화")) app.message = "reset-layout";
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("도구"))
	{
		std::vector<ToolView> views = ToolViews(app);
		bool idle = !app.runner.running;
		for (int i = 0; i < kToolCount; ++i)
		{
			const ToolMeta& m = kToolMeta[i];
			const ToolView& v = views[i];
			std::string id = m.id;
			bool managed = v.state == "installed" || v.state == "update" || v.state == "legacy";
			std::string label = std::string(m.name) + "##menu" + id;
			if (ImGui::BeginMenu(label.c_str()))
			{
				if (ImGui::MenuItem("열기", nullptr, false, v.state != "none")) app.requestActivity = m.open;
				ImGui::Separator();
				if (ImGui::MenuItem("설치", nullptr, false, idle && app.attached && v.state == "none")) app.Run("tool install " + id);
				if (id == "statemonitor" && ImGui::MenuItem("설치 (패키지 게임 포함)", nullptr, false, idle && app.attached && v.state == "none"))
					app.Run("tool install statemonitor --runtime");
				if (ImGui::MenuItem("업데이트", nullptr, false, idle && app.attached && managed)) app.Run("tool update " + id);
				if (ImGui::MenuItem("제거", nullptr, false, idle && app.attached && managed)) app.Run("tool remove " + id);
				ImGui::EndMenu();
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("모든 도구 업데이트", nullptr, false, app.attached && UpdateCount(views) > 0)) app.Run("tool update --all");
		if (ImGui::MenuItem("런처 열기", "Ctrl+9")) app.requestActivity = Activity::Launcher;
		ImGui::Separator();
		ImGui::TextDisabled("프로젝트");
		if (ImGui::MenuItem("연결", nullptr, false, app.hasUProject && !app.attached)) app.requestActivity = Activity::Launcher;
		if (ImGui::MenuItem("연결 새로 고침", nullptr, false, app.attached)) app.Run("update");
		if (ImGui::MenuItem("환경 점검", nullptr, false, app.attached)) app.Run("doctor");
		if (ImGui::MenuItem("연결 해제...", nullptr, false, app.attached)) app.confirmDetach = true;
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("도움말"))
	{
		if (ImGui::MenuItem("가이드 문서")) OpenWithShell(app.home / "docs" / "guide.html");
		if (ImGui::MenuItem("README")) OpenWithShell(app.home / "README.md");
		if (ImGui::MenuItem("DwoStateMonitor 사용법")) OpenWithShell(app.home / "plugin" / "DwoStateMonitor" / "README.md");
		if (ImGui::MenuItem("DwoCanvas 사용법")) OpenWithShell(app.home / "plugin" / "DwoCanvas" / "README.md");
		if (ImGui::MenuItem("DwoCanvas 스키마 설계")) OpenWithShell(app.home / "canvas" / "docs" / "SchemaFirstArchitecture.md");
		ImGui::EndMenu();
	}
	// right side: project chip and palette button
	float right = ImGui::GetWindowWidth();
	float menusEnd = ImGui::GetCursorPosX();
	std::string chip = app.hasUProject ? std::string(ICON_FOLDER) + "  " + app.projectName : std::string(ICON_FOLDER) + "  프로젝트 없음";
	float chipW = ImMin(ImGui::CalcTextSize(chip.c_str()).x + S(24), S(220));
	// narrow window: the command search button becomes an icon so it never covers the menus
	bool compact = right - menusEnd < chipW + S(240) + S(40);
	float paletteW = compact ? ImGui::GetFrameHeight() + S(8) : S(240);
	ImGui::SetCursorPosX(ImMax(menusEnd + S(8), right - chipW - paletteW - S(24)));
	ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 255, 255, 12));
	if (ImGui::Button(compact ? ICON_COMMAND "##palette" : (std::string(ICON_COMMAND) + "  명령 검색        Ctrl+K").c_str(), ImVec2(paletteW, 0))) g_paletteOpen = true;
	if (compact) Tooltip("명령 검색 (Ctrl+K)");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	if (ImGui::Button(chip.c_str(), ImVec2(chipW, 0))) ImGui::OpenPopup("projects");
	Tooltip(PathUtf8(app.project).c_str());
	if (ImGui::BeginPopup("projects"))
	{
		for (auto& r : app.recent)
			if (ImGui::Selectable(r.c_str(), fs::u8path(r) == app.project)) { app.SetProject(fs::u8path(r)); break; }
		ImGui::Separator();
		if (ImGui::Selectable("다른 프로젝트 열기...")) { fs::path p; if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p); }
		ImGui::EndPopup();
	}
	ImGui::EndMainMenuBar();
}

static std::map<std::string, Activity> g_lastToolScreen;   // tool id -> last screen used, reopened from its entry
static std::map<std::string, bool> g_toolFolded;            // tool id -> its screens are folded in the sidebar

// Sidebar: home, PC components, the installed tools (the open one lists its screens), and at the bottom the command
// palette, about and collapse. Expanded it shows names; collapsed, icons only with tooltips.
static void DrawActivityBar(App& app)
{
	ImGuiViewport* vp = ImGui::GetMainViewport();
	bool narrow = vp->WorkSize.x < S(1180);
	bool collapsed = app.sidebarCollapsed || narrow;
	g_activityWidth = collapsed ? S(56) : S(220);
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(ImVec2(g_activityWidth, vp->WorkSize.y - g_statusHeight));
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, S(8)));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(2)));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0x22, 0x23, 0x26, 255));
	ImGui::Begin("##activity", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
	                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 wp = ImGui::GetWindowPos();
	float W = g_activityWidth;
	dl->AddLine(wp + ImVec2(W - 1, 0), wp + ImVec2(W - 1, ImGui::GetWindowHeight()), col::Border, 1);
	app.RebuildProblems();
	int newProblems = 0;
	for (auto& p : app.problems) newProblems += p.isNew && !p.muted && p.severity != "info";
	int connected = app.live.ConnectedCount();
	std::vector<ToolView> views = ToolViews(app);
	std::string activeTool = ToolOfActivity(app.activity);
	if (!activeTool.empty()) g_lastToolScreen[activeTool] = app.activity;
	ConsumeLauncherRequest();
	launcher::Ui& lu = launcher::State();
	bool inLauncher = app.activity == Activity::Launcher;

	// One row. icon: glyph, or a tool tile when 'tool' is set. badge: text pill (expanded) or a dot (collapsed / no text).
	// fold: nullptr = no fold arrow; otherwise the tool's folded state, flipped by a click on the arrow.
	auto row = [&](const char* id, const char* icon, const ToolMeta* tool, const std::string& label, bool active, int depth,
	               ImU32 badgeColor, const std::string& badge, const std::string& tip, bool* fold = nullptr) -> bool {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float h = depth ? S(34) : S(40);
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::PushID(id);
		bool showFold = fold && !collapsed;
		if (showFold) ImGui::SetNextItemAllowOverlap();
		bool clicked = ImGui::InvisibleButton("row", ImVec2(W - 1, h));
		bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
		ImVec2 a = p + ImVec2(S(8), 0), b = p + ImVec2(W - S(9), h);
		if (active) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 26), S(6));
		else if (hov) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 12), S(6));
		float indent = collapsed ? 0 : depth * S(16);
		{
			// tools use their own glyph (the one on the launcher thumbnail) in their colour; the rest are grey
			const char* glyph = tool ? tool->icon : icon;
			float fs = g_fonts.icon->FontSize * (depth ? 0.8f : 0.92f);
			ImVec2 is = g_fonts.icon->CalcTextSizeA(fs, FLT_MAX, 0, glyph);
			float glyphX = collapsed ? (W - is.x) * 0.5f : S(18) + indent + (S(22) - is.x) * 0.5f;
			ImU32 c = tool ? (active || hov ? Mix(tool->color, IM_COL32(255, 255, 255, 255), 0.25f) : tool->color)
			               : (active ? col::Text : hov ? IM_COL32(0xD0, 0xD3, 0xD8, 255) : col::Dim);
			dl->AddText(g_fonts.icon, fs, ImVec2(p.x + glyphX, p.y + (h - is.y) * 0.5f), c, glyph);
		}
		if (!collapsed)
		{
			ImFont* f = active && !depth ? g_fonts.bold : g_fonts.body;
			float fs = f->FontSize * (depth ? 0.95f : 1.0f);
			ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
			float lx = p.x + S(52) + indent;
			dl->PushClipRect(ImVec2(lx, p.y), ImVec2(p.x + W - S(14), p.y + h), true);
			dl->AddText(f, fs, ImVec2(lx, p.y + (h - ts.y) * 0.5f), active ? col::Text : hov ? IM_COL32(0xE6, 0xE8, 0xEB, 255) : IM_COL32(0xB8, 0xBC, 0xC2, 255), label.c_str());
			dl->PopClipRect();
		}
		float badgeRight = p.x + W - S(20);
		bool foldHovered = false;
		if (showFold)
		{
			// fold arrow at the right edge
			float aw = S(24);
			ImVec2 ap(p.x + W - S(12) - aw, p.y + (h - aw) * 0.5f);
			ImGui::SetCursorScreenPos(ap);
			if (ImGui::InvisibleButton("fold", ImVec2(aw, aw))) { *fold = !*fold; clicked = false; }
			bool ah = ImGui::IsItemHovered();
			foldHovered = ah;
			if (ah) dl->AddRectFilled(ap, ap + ImVec2(aw, aw), IM_COL32(255, 255, 255, 20), S(4));
			const char* glyph = *fold ? ICON_RIGHT : ICON_DOWN;
			float fs = g_fonts.icon->FontSize * 0.55f;
			ImVec2 gs = g_fonts.icon->CalcTextSizeA(fs, FLT_MAX, 0, glyph);
			dl->AddText(g_fonts.icon, fs, ap + (ImVec2(aw, aw) - gs) * 0.5f, ah ? col::Text : col::Dim, glyph);
			if (ah) { BeginTip(); ImGui::TextUnformatted(*fold ? "화면 펼치기" : "화면 접기"); ImGui::EndTooltip(); }
			badgeRight = ap.x - S(4);
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
			ImGui::Dummy(ImVec2(0, 0));
		}
		if (badgeColor)
		{
			if (!collapsed && !badge.empty())
			{
				ImFont* f = g_fonts.bold;
				float fs = f->FontSize * 0.78f;
				ImVec2 bs = f->CalcTextSizeA(fs, FLT_MAX, 0, badge.c_str());
				ImVec2 bb = ImVec2(badgeRight, p.y + h * 0.5f + bs.y * 0.5f + S(2));
				ImVec2 ba = ImVec2(bb.x - bs.x - S(12), bb.y - bs.y - S(4));
				dl->AddRectFilled(ba, bb, badgeColor, S(9));
				dl->AddText(f, fs, ba + ImVec2(S(6), S(2)), IM_COL32(255, 255, 255, 255), badge.c_str());
			}
			else dl->AddCircleFilled(ImVec2(collapsed ? p.x + W * 0.5f + S(12) : badgeRight - S(6), p.y + (collapsed ? S(9) : h * 0.5f)), S(4), badgeColor);
		}
		if (hov && !foldHovered && (collapsed || !tip.empty()))
		{
			ImGui::SetNextWindowPos(ImVec2(p.x + W + S(4), p.y + S(6)));
			BeginTip();
			if (collapsed) { ImGui::TextUnformatted(label.c_str()); if (!tip.empty()) ImGui::SameLine(); }
			if (!tip.empty()) TextDim(tip);
			ImGui::EndTooltip();
		}
		ImGui::PopID();
		return clicked;
	};
	auto rule = [&](const char* caption) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImGui::Dummy(ImVec2(W, S(4)));
		ImVec2 c = ImGui::GetCursorScreenPos();
		dl->AddLine(c + ImVec2(S(16), 0), c + ImVec2(W - S(16), 0), col::Border, 1);
		ImGui::Dummy(ImVec2(W, S(6)));
		if (caption && !collapsed)
		{
			ImVec2 q = ImGui::GetCursorScreenPos();
			ImFont* f = g_fonts.bold;
			float fs = f->FontSize * 0.8f;
			dl->AddText(f, fs, q + ImVec2(S(20), 0), col::Muted, caption);
			ImGui::Dummy(ImVec2(W, fs + S(6)));
		}
	};

	int updates = app.attached ? UpdateCount(views) : 0;
	if (row("home", ICON_HOME, nullptr, "홈", inLauncher && lu.page == launcher::Project, 0, updates ? col::Warn : 0,
	        updates ? std::to_string(updates) : std::string(), updates ? Fmt("업데이트 %d", updates) : std::string("Ctrl+9")))
	{
		app.requestActivity = Activity::Launcher;
		lu.page = launcher::Project;
	}
	if (row("pc", ICON_LIBRARY, nullptr, "PC 구성요소", inLauncher && lu.page == launcher::Pc, 0, 0, "", "Jenkins, 언리얼 엔진"))
	{
		app.requestActivity = Activity::Launcher;
		lu.page = launcher::Pc;
	}
	rule("도구");
	// The tools and their screens scroll between the fixed top rows and the bottom rows (short windows).
	float bottomH = (S(40) + S(2)) * 3 + S(14) + S(12);   // three rows, the rule above them, window padding
	ImGui::BeginChild("tools", ImVec2(W - 1, ImMax(S(40), ImGui::GetContentRegionAvail().y - bottomH)), ImGuiChildFlags_None,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
	bool anyTool = false;
	for (int i = 0; i < kToolCount; ++i)
	{
		const ToolMeta& m = kToolMeta[i];
		const ToolView& v = views[i];
		std::string id = m.id;
		bool active = activeTool == id;
		if (!ToolPresent(v) && !active) continue;
		anyTool = true;
		ImU32 bc = 0;
		std::string badge, tip;
		if (v.state == "update" || v.state == "legacy") { bc = col::Warn; badge = "업데이트"; tip = "업데이트 있음"; }
		else if (id == "statemonitor" && connected) { bc = col::Pass; badge = Fmt("연결 %d", connected); }
		else if (id == "lens" && !active && newProblems) { bc = col::Fail; badge = std::to_string(newProblems); tip = Fmt("새 문제 %d", newProblems); }
		else if (id == "canvas" && !app.canvasReports.empty() && app.CanvasReportAt(0)->errors > 0) { bc = col::Fail; tip = "최근 가져오기에 오류"; }
		int screens = 0;
		for (auto& a : kActivities) screens += ToolOfActivity(a.id) == id;
		// Tools with several screens fold them (arrow on the row). Unfolded by default for the tool on screen.
		bool* fold = nullptr;
		if (screens > 1)
		{
			if (!g_toolFolded.count(id)) g_toolFolded[id] = !active;
			fold = &g_toolFolded[id];
		}
		bool showScreens = fold && !*fold && (!collapsed || active);
		// the tool row is the active one when its screens are not listed under it
		if (row(m.id, nullptr, &m, m.name, active && !showScreens, 0, bc, badge, tip, fold))
		{
			auto last = g_lastToolScreen.find(id);
			app.requestActivity = last != g_lastToolScreen.end() ? last->second : m.open;
			if (fold) *fold = false;
		}
		showScreens = fold && !*fold && (!collapsed || active);
		if (showScreens)
		{
			for (auto& a : kActivities)
			{
				if (ToolOfActivity(a.id) != id) continue;
				ImU32 sc = 0;
				std::string sb;
				if (a.id == Activity::Problems && newProblems) { sc = col::Fail; sb = newProblems > 99 ? "99+" : std::to_string(newProblems); }
				if (a.id == Activity::Overview && app.runner.running) sc = col::Info;
				if (row(a.key, a.icon, nullptr, a.name, app.activity == a.id, 1, sc, sb, collapsed ? a.shortcut : "")) app.requestActivity = a.id;
			}
		}
	}
	if (!anyTool && !collapsed)
	{
		ImGui::SetCursorPosX(S(20));
		ImGui::PushTextWrapPos(W - S(16));
		TextDim("설치한 도구가 여기에 나타납니다.");
		ImGui::PopTextWrapPos();
	}

	ImGui::EndChild();
	// bottom
	ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bottomH);
	rule(nullptr);
	if (row("palette", ICON_COMMAND, nullptr, "명령 검색", false, 0, 0, "", "Ctrl+K")) g_paletteOpen = true;
	if (row("about", ICON_INFO, nullptr, "정보", inLauncher && lu.page == launcher::About, 0, 0, "", collapsed ? "버전, 문서, 도구 카탈로그" : ""))
	{
		app.requestActivity = Activity::Launcher;
		lu.page = launcher::About;
	}
	if (row("collapse", collapsed ? ICON_RIGHT : ICON_LEFT, nullptr, "접기", false, 0, 0, "",
	        narrow ? "창이 좁아 자동으로 접혀 있습니다" : collapsed ? "펼치기" : "") && !narrow)
	{
		app.sidebarCollapsed = !app.sidebarCollapsed;
		app.SaveSettings();
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

static void DrawStatusBar(App& app)
{
	ImGuiViewport* vp = ImGui::GetMainViewport();
	g_statusHeight = ImGui::GetFrameHeight() + S(2);
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - g_statusHeight));
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, g_statusHeight));
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(1)));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	bool running = app.runner.running;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, running ? IM_COL32(0x23, 0x4A, 0x8A, 255) : IM_COL32(0x2B, 0x2D, 0x30, 255));
	ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
	                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
	// right side first: what fits decides how much room the left text gets
	app.RebuildProblems();
	int errors = 0, warnings = 0;
	for (auto& p : app.problems)
	{
		if (p.muted) continue;
		int r = SeverityRank(p.severity);
		errors += r >= 4;
		warnings += r == 3;
	}
	int connected = app.live.ConnectedCount();
	std::string liveText = app.live.listening ? (connected ? Fmt("DwoStateMonitor %d 연결", connected) : Fmt("DwoStateMonitor :%d", app.live.port)) : "DwoStateMonitor 꺼짐";
	std::string probText = Fmt("%s %d   %s %d", ICON_ERROR, errors, ICON_WARNING, warnings);
	float winW = ImGui::GetWindowWidth();
	float liveW = ImGui::CalcTextSize(liveText.c_str()).x + S(40);
	float probW = ImGui::CalcTextSize(probText.c_str()).x + S(20);
	bool showLive = winW > S(760), showProb = winW > S(560);
	float rightW = (showLive ? liveW : 0) + (showProb ? probW : 0) + S(20);
	ImVec2 wp = ImGui::GetWindowPos();
	ImGui::PushClipRect(wp, ImVec2(wp.x + winW - rightW - S(8), wp.y + ImGui::GetWindowHeight()), true);
	ImGui::AlignTextToFramePadding();
	if (running)
	{
		Spinner(5, IM_COL32(255, 255, 255, 230));
		ImGui::SameLine();
		std::string last = app.runner.LastLine();
		if (last.size() > 90) last = last.substr(0, 87) + "…";
		ImGui::Text("%s  ·  %s", app.runner.current.c_str(), Duration(app.runner.ElapsedSeconds()).c_str());
		ImGui::SameLine();
		TextColored(IM_COL32(0xC8, 0xD8, 0xF0, 255), last.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("취소")) app.runner.Cancel();
		if (!app.queue.empty()) { ImGui::SameLine(); TextDim(Fmt("대기 %d", (int)app.queue.size())); }
	}
	else
	{
		StatusDot(app.attached ? col::Pass : col::Muted);
		ImGui::SameLine();
		ImGui::TextUnformatted(app.hasUProject ? (app.projectName + (app.attached ? "  ·  UE " + app.Engine() : app.legacyAttached ? "  ·  dwo-Lens 1.x (이전 중)" : "  ·  연결 안 됨")).c_str() : "프로젝트 없음");
	}
	ImGui::PopClipRect();
	if (showProb || showLive)
	{
		ImGui::SameLine(winW - rightW + S(10));
		if (showProb && ImGui::Selectable(probText.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(probText.c_str()).x, 0))) app.requestActivity = Activity::Problems;
		if (showProb && showLive) ImGui::SameLine(0, S(20));
		if (showLive)
		{
			StatusDot(connected ? col::Pass : app.live.listening ? col::Info : col::Fail, 3.5f, connected > 0);
			ImGui::SameLine();
			if (ImGui::Selectable(liveText.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(liveText.c_str()).x, 0))) app.requestActivity = Activity::Live;
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

static void DrawConsole(App& app)
{
	if (ToolButton(ICON_DELETE, "지우기")) app.runner.Clear();
	ImGui::SameLine();
	if (ToolButton(ICON_COPY, "모두 복사"))
	{
		std::string all;
		app.runner.WithLines([&](const std::deque<std::string>& lines) { for (auto& l : lines) all += l + "\n"; });
		CopyToClipboard(all);
	}
	ImGui::SameLine();
	if (ToolButton(ICON_DOWN, "자동 스크롤", ui.consoleFollow)) ui.consoleFollow = !ui.consoleFollow;
	ImGui::SameLine();
	SearchBox("##cfilter", ui.consoleFilter, sizeof(ui.consoleFilter), ICON_FILTER "  필터", S(220));
	if (app.runner.running)
	{
		ImGui::SameLine();
		if (DangerButton((std::string(ICON_STOP) + " 취소").c_str())) app.runner.Cancel();
	}
	ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::PushFont(g_fonts.mono);
	size_t version = 0;
	app.runner.WithLines([&](const std::deque<std::string>& lines) {
		version = app.runner.version;
		std::vector<int> visible;
		for (int i = 0; i < (int)lines.size(); ++i)
			if (ContainsNoCase(lines[i], ui.consoleFilter)) visible.push_back(i);
		ImGuiListClipper clip;
		clip.Begin((int)visible.size());
		while (clip.Step())
			for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k)
			{
				const std::string& l = lines[visible[k]];
				ImU32 c = col::Text;
				std::string low = Lower(l);
				if (l.rfind("> ", 0) == 0) c = col::Accent;
				else if (low.find("error") != std::string::npos || low.find("fail") != std::string::npos || low.find("오류") != std::string::npos) c = col::Fail;
				else if (low.find("warn") != std::string::npos || low.find("경고") != std::string::npos) c = col::Warn;
				else if (l.rfind("[dwo-forge]", 0) == 0 || l.rfind("[dwo-lens]", 0) == 0) c = IM_COL32(0xA8, 0xC8, 0xF0, 255);
				else if (l.rfind("[gui]", 0) == 0) c = col::Dim;
				TextColored(c, l.c_str());
			}
		if (lines.empty())
		{
			ImGui::PopFont();
			TextDim("명령을 실행하면 출력이 여기에 표시됩니다.");
			ImGui::PushFont(g_fonts.mono);
		}
	});
	ImGui::PopFont();
	if (ui.consoleFollow && version != ui.consoleSeen) ImGui::SetScrollHereY(1.0f);
	ui.consoleSeen = version;
	ImGui::EndChild();
}

// What uninstall will remove, from 'forge.ps1 uninstall --plan-json' (the same scan the command uses).
struct UninstallPlan
{
	std::atomic<int> state{ 0 };   // 0 idle, 1 scanning, 2 ready, 3 failed
	json scan;
	std::string error;
	std::thread worker;
	~UninstallPlan() { if (worker.joinable()) worker.join(); }
};
static UninstallPlan g_plan;

static void StartUninstallScan(App& app)
{
	if (g_plan.worker.joinable()) g_plan.worker.join();
	g_plan.state = 1;
	g_plan.scan = json();
	g_plan.error.clear();
	std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + (app.home / "tools" / "forge.ps1").wstring() +
	                   L"\" -Command uninstall -Project \"" + app.project.wstring() + L"\" --plan-json";
	fs::path dir = app.project;
	g_plan.worker = std::thread([cmd, dir] {
		std::string out;
		DWORD code = 1;
		if (!CaptureOutput(cmd, dir, out, code)) { g_plan.error = "검사를 시작할 수 없습니다."; g_plan.state = 3; return; }
		size_t at = out.find('{');
		try { g_plan.scan = json::parse(out.substr(at == std::string::npos ? 0 : at)); g_plan.state = 2; }
		catch (...) { g_plan.error = ConsoleToUtf8(out); g_plan.state = 3; }
	});
}

static std::vector<std::string> PlanList(const char* key)
{
	std::vector<std::string> list;
	for (auto& v : Arr(g_plan.scan, key))
	{
		if (v.is_string()) list.push_back(v.get<std::string>());
		else if (v.is_object() && v.contains("Module")) list.push_back(Str(v, "Module") + " · " + Str(v, "File"));
		else if (v.is_object() && v.contains("Lines")) list.push_back(Str(v, "File") + Fmt(" (%lld줄)", Num(v, "Lines")));
	}
	return list;
}

static void DrawDetachModal(App& app)
{
	static int dataChoice = 0;          // 0 keep, 1 delete
	static int generatedChoice = 1;     // 0 keep, 1 delete
	static bool cleanRefs = true;
	static bool acceptCode = false;
	static bool forget = true;
	if (app.confirmDetach)
	{
		StartUninstallScan(app);
		dataChoice = 0;
		generatedChoice = 1;
		cleanRefs = true;
		acceptCode = false;
		ImGui::OpenPopup("dwo-Forge 제거");
		app.confirmDetach = false;
	}
	ImGui::SetNextWindowSize(ImVec2(S(640), 0));
	// Re-centre every frame: the dialog grows when the scan finishes.
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	if (!ImGui::BeginPopupModal("dwo-Forge 제거", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	Title("이 프로젝트에서 dwo-Forge를 완전히 제거합니다");
	ImGui::PushTextWrapPos(S(630));
	TextDim("플러그인을 따로 지울 필요 없이 한 번에 지웁니다. 연동할 때 추가한 파일, 도구가 만든 파일, 프로젝트 파일에 등록된 항목까지 정리합니다.");
	ImGui::PopTextWrapPos();
	if (g_plan.state == 1)
	{
		ImGui::Spacing();
		Spinner(6, col::Info);
		ImGui::SameLine();
		TextDim("프로젝트를 검사하는 중 (생성된 위젯·코드·설정 파일)...");
		ImGui::Spacing();
		if (ImGui::Button("취소", ImVec2(S(120), 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	if (g_plan.state == 3)
	{
		TextColored(col::Fail, "검사에 실패했습니다.");
		ImGui::PushTextWrapPos(S(620));
		TextDim(g_plan.error.substr(0, 1500));
		ImGui::PopTextWrapPos();
		if (ImGui::Button("다시 검사")) StartUninstallScan(app);
		ImGui::SameLine();
		if (ImGui::Button("닫기", ImVec2(S(120), 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	auto bullet = [](ImU32 color, const char* icon, const char* what, const std::string& detail, const std::vector<std::string>& items = {}) {
		TextColored(color, icon);
		ImGui::SameLine();
		ImGui::TextUnformatted(what);
		ImGui::SameLine();
		TextDim(detail);
		if (!items.empty()) Tooltip(JoinStrings(json(items), "\n").c_str());
	};
	auto plugins = PlanList("Plugins");
	auto launchers = PlanList("Launchers");
	auto artifacts = PlanList("Artifacts");
	auto records = PlanList("Records");
	auto generated = PlanList("Generated");
	auto uproject = PlanList("Uproject");
	auto buildcs = PlanList("BuildCs");
	auto configs = PlanList("Configs");
	auto editorIni = PlanList("EditorIni");
	auto code = PlanList("CodeUses");
	auto created = PlanList("Created");
	std::string exclude = Str(g_plan.scan, "Exclude");

	// The list can be long; it scrolls so the buttons below always stay on screen.
	float maxList = ImGui::GetMainViewport()->WorkSize.y * 0.55f;
	ImGui::SetNextWindowSizeConstraints(ImVec2(S(630), 0), ImVec2(S(630), maxList));
	ImGui::BeginChild("plan", ImVec2(S(630), 0), ImGuiChildFlags_AutoResizeY);
	SectionHeader("항상 지우는 것");
	bullet(plugins.empty() ? col::Muted : col::Text, ICON_DELETE, "플러그인", plugins.empty() ? "없음" : JoinStrings(json(plugins)));
	bullet(col::Text, ICON_DELETE, "실행 파일·연동 정보", JoinStrings(json(launchers)) + (launchers.empty() ? "" : ", ") + "__dwo-forge\\");
	bullet(exclude.empty() ? col::Muted : col::Text, ICON_DELETE, "git exclude", exclude.empty() ? "추가한 항목 없음" : "dwo-Forge 블록만 · " + exclude);
	std::vector<std::string> tests;
	for (auto& a : artifacts) if (a.find("Tests") != std::string::npos) tests.push_back(a);
	if (!tests.empty()) bullet(col::Text, ICON_DELETE, "테스트 잔여물", JoinStrings(json(tests)));

	size_t refCount = uproject.size() + buildcs.size() + configs.size() + editorIni.size();
	if (refCount)
	{
		SectionHeader("프로젝트 파일에 등록된 항목");
		if (!uproject.empty()) bullet(col::Warn, ICON_PAGE, ".uproject Plugins", JoinStrings(json(uproject)), uproject);
		if (!buildcs.empty()) bullet(col::Warn, ICON_CODE, "Build.cs 의존성", Fmt("%d곳", (int)buildcs.size()), buildcs);
		if (!configs.empty()) bullet(col::Warn, ICON_SETTINGS, "플러그인 설정 파일", JoinStrings(json(configs)), configs);
		if (!editorIni.empty()) bullet(col::Muted, ICON_SETTINGS, "에디터 개인 설정", "모듈 기록 줄", editorIni);
		ImGui::Checkbox("이 항목들도 정리 (원본은 백업)", &cleanRefs);
		Tooltip("백업 위치: %LOCALAPPDATA%\\dwo-Forge\\uninstalled\\<프로젝트>-<시각>\\backup");
	}
	if (!code.empty())
	{
		SectionHeader("프로젝트 코드가 플러그인을 사용 중");
		ImGui::PushTextWrapPos(S(620));
		TextColored(col::Warn, Fmt("%d곳이 플러그인 헤더나 클래스를 씁니다. 코드는 자동으로 고치지 않으므로, 제거 후 이 줄들을 지워야 빌드됩니다.", (int)code.size()).c_str());
		ImGui::PopTextWrapPos();
		ImGui::BeginChild("codeuses", ImVec2(0, ImMin(S(110), (float)code.size() * ImGui::GetTextLineHeightWithSpacing() + S(8))), ImGuiChildFlags_Borders);
		ImGui::PushFont(g_fonts.mono);
		for (auto& c : code) ImGui::TextUnformatted(c.c_str());
		ImGui::PopFont();
		ImGui::EndChild();
		if (ImGui::SmallButton("목록 복사")) CopyToClipboard(JoinStrings(json(code), "\n"));
		ImGui::Checkbox("알고 있음: 제거 후 코드를 직접 고치겠습니다", &acceptCode);
	}
	if (!generated.empty() || !records.empty())
	{
		SectionHeader("DwoCanvas로 만든 위젯");
		ImGui::Text("위젯 %d개%s", (int)generated.size(), records.empty() ? "" : " · 가져오기 기록");
		Tooltip(JoinStrings(json(generated), "\n").c_str());
		TextDim("DwoCanvas 없이 열면 깨지는 에셋입니다.");
		ImGui::RadioButton("위젯과 기록 삭제##gen", &generatedChoice, 1);
		ImGui::SameLine();
		ImGui::RadioButton("프로젝트에 남김##gen", &generatedChoice, 0);
	}
	SectionHeader("실행 기록·로그·리포트");
	ImGui::Text("실행 기록 %lld개 · %s%s", Num(g_plan.scan, "Runs"), Bytes(Real(g_plan.scan, "DataBytes")).c_str(),
	            created.empty() ? "" : Fmt(" · 프로젝트에 남은 파일 %d개", (int)created.size()).c_str());
	TextDim("__dwo-forge\\data, Saved\\DwoCanvas 리포트, 성능 캡처 CSV");
	ImGui::RadioButton("보존: 프로젝트 밖(%LOCALAPPDATA%\\dwo-Forge\\uninstalled)으로 옮김", &dataChoice, 0);
	ImGui::RadioButton("삭제", &dataChoice, 1);
	ImGui::EndChild();
	ImGui::Separator();
	ImGui::Checkbox("dwo-Forge의 최근 프로젝트 목록에서도 빼기", &forget);
	ImGui::Spacing();
	bool blocked = !code.empty() && !acceptCode;
	ImGui::BeginDisabled(blocked);
	if (DangerButton("제거", ImVec2(S(120), 0)))
	{
		std::string args = "uninstall";
		args += dataChoice == 0 ? " --keep-data" : " --delete-data";
		args += generatedChoice == 0 ? " --keep-generated" : " --delete-generated";
		if (!cleanRefs) args += " --keep-references";
		if (!code.empty()) args += " --force";
		app.forgetAfterUninstall = forget;
		app.Run(args);
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	if (blocked) Tooltip("코드 사용 목록을 확인하고 체크하세요.");
	ImGui::SameLine();
	if (ImGui::Button("취소", ImVec2(S(120), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// Fixed dock layout: navigator | main | inspector, with a bottom panel under main.
static void BuildDefaultLayout(ImGuiID dockspace, ImVec2 size)
{
	ImGui::DockBuilderRemoveNode(dockspace);
	ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspace, size);
	ImGuiID mainId = dockspace;
	ImGuiID left = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Left, 0.20f, nullptr, &mainId);
	ImGuiID right = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.27f, nullptr, &mainId);
	ImGuiID bottom = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Down, 0.24f, nullptr, &mainId);
	ImGui::DockBuilderDockWindow("탐색###nav", left);
	ImGui::DockBuilderDockWindow("속성###inspector", right);
	ImGui::DockBuilderDockWindow("###main", mainId);
	ImGui::DockBuilderDockWindow("실행 로그###console", bottom);
	ImGui::DockBuilderDockWindow("타임라인###timeline", bottom);
	ImGui::DockBuilderDockWindow("이벤트###events", bottom);
	if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(mainId)) n->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
	ImGui::DockBuilderFinish(dockspace);
}

static void DrawApp(App& app, bool& quit)
{
	app.Tick();
	PollTools(app);
	ImGuiIO& io = ImGui::GetIO();
	// shortcuts
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) g_paletteOpen = true;
	if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P, false)) g_paletteOpen = true;
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) { fs::path p; if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p); }
	if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { app.outputs.clear(); app.metrics.clear(); app.structureFor.clear(); app.Refresh(); }
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) app.showConsole = true;
	if (io.KeyCtrl && !io.WantTextInput)
		for (auto& a : kActivities)
			if (a.hotkey != ImGuiKey_None && ImGui::IsKeyPressed(a.hotkey, false)) app.requestActivity = a.id;
	if (app.requestActivity != Activity::Count)
	{
		if (app.activity != app.requestActivity) ui.filter[0] = 0;
		app.activity = app.requestActivity;
		app.requestActivity = Activity::Count;
	}

	bool quitMenu = false;
	DrawMenuBar(app, quitMenu);
	quit |= quitMenu;
	DrawStatusBar(app);
	DrawActivityBar(app);

	// dock space beside the activity bar, above the status bar
	ImGuiViewport* vp = ImGui::GetMainViewport();
	ImVec2 pos = vp->WorkPos + ImVec2(g_activityWidth, 0);
	ImVec2 size = vp->WorkSize - ImVec2(g_activityWidth, g_statusHeight);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::Begin("##host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
	                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
	ImGui::PopStyleVar(2);
	ImGuiID dockspace = ImGui::GetID("dwo-lens-dock");   // id kept: saved layouts of dwo-Lens 1.x still apply
	static bool layoutChecked = false;
	if (!layoutChecked || app.message == "reset-layout")
	{
		if (!ImGui::DockBuilderGetNode(dockspace) || app.message == "reset-layout") BuildDefaultLayout(dockspace, size);
		layoutChecked = true;
		if (app.message == "reset-layout") app.message.clear();
	}
	bool launcherMode = app.activity == Activity::Launcher;
	if (launcherMode)
	{
		// The launcher is one card next to the sidebar, with the run log as a drawer under it. The dock layout of the
		// tool screens stays alive (KeepAliveOnly) and comes back unchanged when a tool is opened.
		ImGui::DockSpace(dockspace, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
		static bool logOpen = false;
		if (app.showConsole || (app.runner.running && app.runner.ElapsedSeconds() < 0.2)) logOpen = true;
		app.showConsole = false;
		float drawerBar = ImGui::GetFrameHeight() + S(8);
		float drawer = logOpen ? ImMin(S(240), size.y * 0.4f) : 0;
		DrawLauncherContent(app, size.y - drawerBar - drawer);
		// run log drawer
		ImGui::SetCursorPos(ImVec2(S(16), size.y - drawerBar - drawer));
		ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
		std::string bar = std::string(logOpen ? ICON_DOWN : ICON_RIGHT) + "  실행 로그" + (app.runner.running ? "  ●  " + app.runner.current : std::string());
		if (ImGui::Button(bar.c_str())) logOpen = !logOpen;
		ImGui::PopStyleColor();
		if (logOpen)
		{
			ImGui::SetCursorPos(ImVec2(S(16), size.y - drawer));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(6)));
			ImGui::BeginChild("launcher-log", ImVec2(size.x - S(32), drawer - S(10)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
			DrawConsole(app);
			ImGui::EndChild();
			ImGui::PopStyleVar();
		}
		ImGui::End();
		DrawDetachModal(app);
		DrawCommandPalette(app);
		DrawToasts(g_statusHeight);
		return;
	}
	ImGui::DockSpace(dockspace, ImVec2(0, 0), ImGuiDockNodeFlags_None);
	ImGui::End();

	const ActivityDef& def = DefOf(app.activity);
	const ToolMeta* tool = ToolMetaOf(ToolOfActivity(app.activity));
	ImGuiWindowFlags panel = ImGuiWindowFlags_NoCollapse;
	// navigator
	if (ImGui::Begin("탐색###nav", nullptr, panel))
	{
		switch (app.activity)
		{
		case Activity::Overview: DrawOverviewNav(app); break;
		case Activity::Architecture: DrawArchitectureNav(app); break;
		case Activity::Problems: DrawProblemsNav(app); break;
		case Activity::Tests: DrawTestsNav(app); break;
		case Activity::Logs: DrawLogsNav(app); break;
		case Activity::Performance: DrawPerformanceNav(app); break;
		case Activity::Live: DrawLiveNav(app); break;
		case Activity::Canvas: DrawCanvasNav(app); break;
		case Activity::History: DrawHistoryNav(app); break;
		case Activity::Ci: DrawCiNav(app); break;
		default: break;
		}
	}
	ImGui::End();
	// main
	std::string mainTitle = (tool && std::string(tool->name) != def.name ? std::string(tool->name) + "  ›  " : std::string()) + def.name + "###main";
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
	if (ImGui::Begin(mainTitle.c_str(), nullptr, panel))
	{
		switch (app.activity)
		{
		case Activity::Overview: DrawOverviewMain(app); break;
		case Activity::Architecture: DrawArchitectureMain(app); break;
		case Activity::Problems: DrawProblemsMain(app); break;
		case Activity::Tests: DrawTestsMain(app); break;
		case Activity::Logs: DrawLogsMain(app); break;
		case Activity::Performance: DrawPerformanceMain(app); break;
		case Activity::Live: DrawLiveMain(app); break;
		case Activity::Canvas: DrawCanvasMain(app); break;
		case Activity::History: DrawHistoryMain(app); break;
		case Activity::Ci: DrawCiMain(app); break;
		default: break;
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
	// inspector
	if (ImGui::Begin("속성###inspector", nullptr, panel))
	{
		switch (app.activity)
		{
		case Activity::Overview: DrawOverviewInspector(app); break;
		case Activity::Architecture: DrawArchitectureInspector(app); break;
		case Activity::Problems: DrawProblemsInspector(app); break;
		case Activity::Tests: DrawTestsInspector(app); break;
		case Activity::Logs: DrawLogsInspector(app); break;
		case Activity::Performance: DrawPerformanceInspector(app); break;
		case Activity::Live: DrawLiveInspector(app); break;
		case Activity::Canvas: DrawCanvasInspector(app); break;
		case Activity::History: DrawHistoryInspector(app); break;
		case Activity::Ci: DrawCiInspector(app); break;
		default: break;
		}
	}
	ImGui::End();
	// bottom panel tabs
	static Activity lastActivity = Activity::Count;
	bool firstFrame = lastActivity == Activity::Count;
	bool enteredLive = app.activity == Activity::Live && lastActivity != Activity::Live;
	bool leftLive = app.activity != Activity::Live && lastActivity == Activity::Live;
	lastActivity = app.activity;
	if (app.showConsole || app.runner.running && app.runner.ElapsedSeconds() < 0.2) ImGui::SetNextWindowFocus();
	std::string consoleTitle = std::string(app.runner.running ? "실행 로그 ●" : "실행 로그") + "###console";
	if (ImGui::Begin(consoleTitle.c_str(), nullptr, panel)) DrawConsole(app);
	ImGui::End();
	if (app.showConsole) app.showConsole = false;
	if (ImGui::Begin("타임라인###timeline", nullptr, panel)) DrawLiveTimeline(app);
	ImGui::End();
	if (ImGui::Begin("이벤트###events", nullptr, panel)) DrawLiveEvents(app);
	ImGui::End();
	// pick the bottom tab after every window exists (new docked windows grab focus on their first frame)
	static int bottomFocusFrames = 0;
	static const char* bottomFocus = nullptr;
	if (enteredLive) { bottomFocus = "타임라인###timeline"; bottomFocusFrames = 3; }
	else if (firstFrame || leftLive) { bottomFocus = "실행 로그###console"; bottomFocusFrames = 3; }
	if (bottomFocusFrames > 0 && bottomFocus && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
	{
		std::string name = bottomFocus;
		if (name == "실행 로그###console") name = consoleTitle;
		ImGui::SetWindowFocus(name.c_str());
		--bottomFocusFrames;
	}

	DrawDetachModal(app);
	DrawCommandPalette(app);
	DrawToasts(g_statusHeight);
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

// Saves the current back buffer as a 32-bit BMP (used by --screenshot for docs and QA).
static bool SaveBackBuffer(const fs::path& path)
{
	ID3D11Texture2D* back = nullptr;
	if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
	D3D11_TEXTURE2D_DESC desc;
	back->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	ID3D11Texture2D* staging = nullptr;
	if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &staging))) { back->Release(); return false; }
	g_context->CopyResource(staging, back);
	D3D11_MAPPED_SUBRESOURCE map;
	bool ok = false;
	if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
	{
		BITMAPFILEHEADER fh{};
		BITMAPINFOHEADER ih{};
		ih.biSize = sizeof(ih);
		ih.biWidth = (LONG)desc.Width;
		ih.biHeight = -(LONG)desc.Height;
		ih.biPlanes = 1;
		ih.biBitCount = 32;
		ih.biCompression = BI_RGB;
		DWORD imageSize = desc.Width * desc.Height * 4;
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof(fh) + sizeof(ih);
		fh.bfSize = fh.bfOffBits + imageSize;
		std::ofstream out(path, std::ios::binary);
		out.write((const char*)&fh, sizeof(fh));
		out.write((const char*)&ih, sizeof(ih));
		std::vector<unsigned char> row(desc.Width * 4);
		for (UINT y = 0; y < desc.Height; ++y)
		{
			const unsigned char* src = (const unsigned char*)map.pData + y * map.RowPitch;
			for (UINT x = 0; x < desc.Width; ++x)
			{
				row[x * 4 + 0] = src[x * 4 + 2];
				row[x * 4 + 1] = src[x * 4 + 1];
				row[x * 4 + 2] = src[x * 4 + 0];
				row[x * 4 + 3] = 255;
			}
			out.write((const char*)row.data(), row.size());
		}
		ok = (bool)out;
		g_context->Unmap(staging, 0);
	}
	staging->Release();
	back->Release();
	return ok;
}

static fs::path g_dropped;   // a folder, .uproject or design file (*.json) dropped onto the window

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
	switch (msg)
	{
	case WM_DROPFILES:
	{
		HDROP drop = (HDROP)wParam;
		wchar_t path[MAX_PATH * 2] = {};
		if (DragQueryFileW(drop, 0, path, (UINT)(sizeof(path) / sizeof(path[0])))) g_dropped = path;
		DragFinish(drop);
		return 0;
	}
	case WM_GETMINMAXINFO:
	{
		// Below this the sidebar, the launcher card and the docked tool panels no longer fit side by side.
		MINMAXINFO* mm = (MINMAXINFO*)lParam;
		mm->ptMinTrackSize.x = (LONG)(960 * g_scale);
		mm->ptMinTrackSize.y = (LONG)(620 * g_scale);
		return 0;
	}
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

	static App app;
	wchar_t local[MAX_PATH] = {};
	GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
	app.settingsDir = fs::path(local) / L"dwo-Forge";
	{
		// Settings of dwo-Lens 1.x (recent projects, layout): copied once when dwo-Forge has none yet.
		std::error_code ec;
		fs::path legacy = fs::path(local) / L"dwo-Lens";
		fs::create_directories(app.settingsDir, ec);
		for (const wchar_t* name : { L"gui.json", L"gui-layout-v2.ini" })
			if (fs::exists(legacy / name, ec) && !fs::exists(app.settingsDir / name, ec)) fs::copy_file(legacy / name, app.settingsDir / name, ec);
	}
	app.LoadSettings();

	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::wstring projectArg = ArgValue(argc, argv, L"--project");
	std::wstring homeArg = ArgValue(argc, argv, L"--home");
	std::wstring viewArg = ArgValue(argc, argv, L"--view");
	if (viewArg.empty()) viewArg = ArgValue(argc, argv, L"--tab");
	std::wstring runArg = ArgValue(argc, argv, L"--run");
	std::wstring selectArg = ArgValue(argc, argv, L"--select");
	std::wstring exportArg = ArgValue(argc, argv, L"--export-report");
	std::wstring shotArg = ArgValue(argc, argv, L"--screenshot");
	std::wstring recordingArg = ArgValue(argc, argv, L"--open-recording");
	std::wstring portArg = ArgValue(argc, argv, L"--port");
	std::wstring modeArg = ArgValue(argc, argv, L"--mode");
	std::wstring delayArg = ArgValue(argc, argv, L"--screenshot-delay");
	std::wstring canvasFileArg = ArgValue(argc, argv, L"--canvas-file");
	// --hover x,y (window pixels): park the mouse there, so --screenshot can capture hover states and tooltips.
	std::wstring hoverArg = ArgValue(argc, argv, L"--hover");
	std::wstring sizeArg = ArgValue(argc, argv, L"--size");   // WxH in logical pixels (screenshots of other window sizes)
	int sizeW = 1500, sizeH = 920;
	if (!sizeArg.empty()) swscanf_s(sizeArg.c_str(), L"%dx%d", &sizeW, &sizeH);
	float hoverX = -1, hoverY = -1;
	if (!hoverArg.empty()) swscanf_s(hoverArg.c_str(), L"%f,%f", &hoverX, &hoverY);
	double shotDelay = delayArg.empty() ? 0.8 : _wtof(delayArg.c_str());
	if (modeArg == L"machine") ui.liveMode = 1;
	bool openUninstall = ArgValue(argc, argv, L"--dialog") == L"uninstall";
	if (ArgValue(argc, argv, L"--dialog") == L"palette") g_paletteOpen = true;   // screenshots of the command palette
	LocalFree(argv);
	if (homeArg.empty())
	{
		wchar_t buf[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"DWO_FORGE_HOME", buf, MAX_PATH)) homeArg = buf;
		else if (GetEnvironmentVariableW(L"DWO_LENS_HOME", buf, MAX_PATH)) homeArg = buf;   // dwo-Lens 1.x registration
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

	WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"DwoForgeGui", nullptr };
	wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	RegisterClassExW(&wc);
	HWND hwnd = g_hwnd = CreateWindowW(wc.lpszClassName, L"dwo-Forge", WS_OVERLAPPEDWINDOW, 80, 60, 1500, 920, nullptr, nullptr, wc.hInstance, nullptr);
	BOOL dark = TRUE;
	DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		MessageBoxW(nullptr, L"DirectX 11 장치를 만들 수 없습니다.", L"dwo-Forge", MB_ICONERROR);
		return 1;
	}
	float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
	g_scale = scale;
	SetWindowPos(hwnd, nullptr, 0, 0, (int)(sizeW * scale), (int)(sizeH * scale), SWP_NOMOVE | SWP_NOZORDER);
	DragAcceptFiles(hwnd, TRUE);
	ShowWindow(hwnd, shotArg.empty() ? SW_SHOWDEFAULT : SW_SHOWNOACTIVATE);
	UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	static std::string iniPath = PathUtf8(app.settingsDir / "gui-layout-v2.ini");
	std::error_code ec;
	fs::create_directories(app.settingsDir, ec);
	io.IniFilename = shotArg.empty() ? iniPath.c_str() : nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
	io.ConfigWindowsMoveFromTitleBarOnly = true;

	ApplyTheme(scale);
	LoadFonts(scale);

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_device, g_context);

	// --port overrides for this window only (tests, a second window); it is not saved.
	app.live.Start(portArg.empty() ? app.liveSettings.port : _wtoi(portArg.c_str()));

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
	for (auto& a : kActivities)
		if (viewArg == ToWide(a.key)) app.activity = a.id;
	if (viewArg == L"result" || viewArg == L"history") app.activity = Activity::History;
	if (viewArg == L"tools") app.activity = Activity::Launcher;   // the tools screen became the launcher (3.0)
	// launcher pages (docs, screenshots): --view jenkins | engines | about
	if (viewArg == L"jenkins" || viewArg == L"engines" || viewArg == L"about")
	{
		app.activity = Activity::Launcher;
		g_launcherPageRequest = viewArg == L"jenkins" ? 1 : viewArg == L"engines" ? 2 : 3;
	}
	if (!selectArg.empty() && app.activity == Activity::Live) ui.liveSource = ToUtf8(selectArg);
	else if (!selectArg.empty())
	{
		ui.historySel = ToUtf8(selectArg);
		if (viewArg.empty()) app.activity = Activity::History;
	}
	if (!recordingArg.empty()) app.store.Import(recordingArg);
	if (!canvasFileArg.empty()) app.canvasFile = canvasFileArg;   // not added to the recent list (tests, screenshots)
	if (!runArg.empty() && app.hasUProject) app.Run(ToUtf8(runArg));
	if (openUninstall && app.attached) app.confirmDetach = true;

	bool done = false;
	int frame = 0;
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
		if (IsIconic(hwnd)) { Sleep(50); app.Tick(); continue; }
		if (g_resizeWidth != 0 && g_resizeHeight != 0)
		{
			CleanupRenderTarget();
			g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
			g_resizeWidth = g_resizeHeight = 0;
			CreateRenderTarget();
		}

		if (!g_dropped.empty())
		{
			fs::path p = g_dropped;
			g_dropped.clear();
			std::error_code ec;
			if (IsDesignFile(p) && fs::is_regular_file(p, ec)) { app.SetCanvasFile(p); app.activity = Activity::Canvas; }
			else
			{
				if (p.extension() == ".uproject") p = p.parent_path();
				if (fs::is_directory(p, ec)) { app.SetProject(p); app.activity = Activity::Launcher; }
			}
		}
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		if (hoverX >= 0) ImGui::GetIO().AddMousePosEvent(hoverX, hoverY);
		ImGui::NewFrame();
		bool quit = false;
		DrawApp(app, quit);
		ImGui::Render();
		const float clear[4] = { 0.118f, 0.122f, 0.133f, 1.0f };
		g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_context->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		if (!shotArg.empty() && ++frame > 10 && ImGui::GetTime() > shotDelay)
		{
			SaveBackBuffer(shotArg);
			done = true;
		}
		g_swapChain->Present(1, 0);
		if (quit) done = true;
	}

	app.runner.Cancel();
	app.live.Stop();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDeviceD3D();
	DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	CoUninitialize();
	return 0;
}
