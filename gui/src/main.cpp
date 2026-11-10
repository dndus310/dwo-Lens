// dwo-Forge control panel: Dear ImGui (docking) + Win32 + DirectX 11.
//
// Runs the same commands as __dwo-forge.bat and reads the same run.json / out/*.json files, so the GUI and the
// command line always show identical results. Groups the three provided tools: Lens (analysis lines),
// DwoStateMonitor (live link on 127.0.0.1:47731) and DwoCanvas (design file -> UMG importer).
//
//   dwo-forge-gui.exe [--project <folder with .uproject>] [--home <dwo-Forge repository>]
//                     [--view overview|architecture|problems|tests|logs|performance|live|canvas|history|tools]
//                     [--select <run id>] [--run <command>] [--open-recording <file.jsonl>] [--canvas-file <design.json>]
//   dwo-forge-gui.exe --project <folder> --export-report <file.html>   (no window: every line's latest report in one HTML)
//   dwo-forge-gui.exe ... --screenshot <file.bmp>                      (renders a few frames, saves the window, exits)

#include "views_canvas.h"

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
	const char* group;  // tool group shown in the activity bar: Lens, DwoStateMonitor, DwoCanvas, Forge
};

// Shortcuts keep their dwo-Lens 1.x numbers (Ctrl+1..9); the new DwoCanvas view takes Ctrl+0.
static const ActivityDef kActivities[] = {
	{ Activity::Overview, ICON_DASHBOARD, "대시보드", "overview", "Ctrl+1", ImGuiKey_1, "Lens" },
	{ Activity::Architecture, ICON_LAYERS, "아키텍처", "architecture", "Ctrl+2", ImGuiKey_2, "Lens" },
	{ Activity::Problems, ICON_WARNING, "문제", "problems", "Ctrl+3", ImGuiKey_3, "Lens" },
	{ Activity::Tests, ICON_CHECKLIST, "검증", "tests", "Ctrl+4", ImGuiKey_4, "Lens" },
	{ Activity::Logs, ICON_LIST, "로그", "logs", "Ctrl+5", ImGuiKey_5, "Lens" },
	{ Activity::Performance, ICON_SPEED, "성능·용량", "performance", "Ctrl+6", ImGuiKey_6, "Lens" },
	{ Activity::Live, ICON_EYE, "DwoStateMonitor", "live", "Ctrl+7", ImGuiKey_7, "DwoStateMonitor" },
	{ Activity::Canvas, ICON_CANVAS, "DwoCanvas", "canvas", "Ctrl+0", ImGuiKey_0, "DwoCanvas" },
	{ Activity::History, ICON_HISTORY, "실행 기록", "history", "Ctrl+8", ImGuiKey_8, "Forge" },
	{ Activity::Tools, ICON_PUZZLE, "도구", "tools", "Ctrl+9", ImGuiKey_9, "Forge" },
};

// Short caption drawn above each group in the activity bar.
static const char* GroupCaption(const char* group)
{
	std::string g = group;
	if (g == "Lens") return "LENS";
	if (g == "DwoStateMonitor") return "STATE";
	if (g == "DwoCanvas") return "CANVAS";
	return "FORGE";
}

static ImU32 GroupColor(const char* group)
{
	std::string g = group;
	if (g == "Lens") return col::Accent;
	if (g == "DwoStateMonitor") return col::Pass;
	if (g == "DwoCanvas") return col::Purple;
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
		list.push_back({ std::string("이동: ") + (std::string(a.group) == "Lens" ? "Lens · " : "") + a.name, a.shortcut, [&app, id] { app.requestActivity = id; } });
	}
	for (int i = 0; i < kLineCount; ++i)
	{
		std::string cmd = kLines[i].command;
		list.push_back({ std::string("실행: ") + kLines[i].name, cmd, [&app, cmd] { app.Run(cmd); }, app.attached });
	}
	list.push_back({ "실행: 기본 라인 모두", "analyze, logs, sizes", [&app] { app.RunAll(); }, app.attached });
	list.push_back({ "게임 실행 (DwoStateMonitor 연결)", "play", [&app] { app.Run("play"); app.requestActivity = Activity::Live; }, app.attached });
	list.push_back({ "DwoStateMonitor 설치 (에디터 전용)", "sm-install", [&app] { app.Run("sm-install"); }, app.attached && app.stateMonitor == 0 });
	list.push_back({ "DwoStateMonitor 설치 (패키지 게임 포함)", "sm-install --runtime", [&app] { app.Run("sm-install --runtime"); }, app.attached && app.stateMonitor == 0 });
	list.push_back({ "DwoStateMonitor 제거", "sm-remove", [&app] { app.Run("sm-remove"); }, app.stateMonitor == 1 });
	list.push_back({ "DwoCanvas 설치", "canvas-install", [&app] { app.Run("canvas-install"); }, app.attached && app.canvas == 0 });
	list.push_back({ "DwoCanvas 제거", "canvas-remove", [&app] { app.Run("canvas-remove"); }, app.canvas == 1 });
	list.push_back({ "DwoCanvas: 디자인 파일 열기...", "*.dwocanvas.json", [&app] { PickDesignFile(app); app.requestActivity = Activity::Canvas; } });
	list.push_back({ "DwoCanvas: 검사", "canvas-validate", [&app] { RunCanvas(app, "canvas-validate"); }, !app.canvasFile.empty() && app.hasUProject });
	list.push_back({ "DwoCanvas: 가져오기", "canvas-import", [&app] { RunCanvas(app, "canvas-import"); }, !app.canvasFile.empty() && app.canvas != 0 });
	list.push_back({ "DwoCanvas: 초기화하고 가져오기", "canvas-import --reset", [&app] { RunCanvas(app, "canvas-import", true); }, !app.canvasFile.empty() && app.canvas != 0 });
	list.push_back({ "종합 리포트 저장...", "HTML", [&app] { SaveBundleReport(app); }, !app.runs.empty() });
	list.push_back({ "프로젝트 열기...", "Ctrl+O", [&app] { fs::path p; if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p); } });
	list.push_back({ "새로고침", "F5", [&app] { app.outputs.clear(); app.metrics.clear(); app.structureFor.clear(); app.Refresh(); } });
	list.push_back({ "부착", "attach", [&app] { app.Run("attach"); }, app.hasUProject && !app.attached });
	list.push_back({ "시스템 업데이트", "update", [&app] { app.Run("update"); }, app.attached });
	list.push_back({ "환경 점검", "doctor", [&app] { app.Run("doctor"); }, app.attached });
	list.push_back({ "제거 (uninstall)...", "uninstall", [&app] { app.confirmDetach = true; }, app.attached });
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
		if (ImGui::IsWindowAppearing()) { g_paletteQuery[0] = 0; g_paletteIndex = 0; ImGui::SetKeyboardFocusHere(); }
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::PushFont(g_fonts.title);
		if (ImGui::InputTextWithHint("##q", ICON_SEARCH "  명령, 화면, 라인 검색", g_paletteQuery, sizeof(g_paletteQuery))) g_paletteIndex = 0;
		ImGui::PopFont();
		auto commands = BuildCommands(app);
		std::vector<std::pair<int, int>> scored;
		for (int i = 0; i < (int)commands.size(); ++i)
		{
			int sc = ImMax(FuzzyScore(commands[i].label, g_paletteQuery), FuzzyScore(commands[i].hint, g_paletteQuery));
			if (sc > 0) scored.push_back({ -sc, i });
		}
		std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first < b.first; });
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) g_paletteIndex = ImMin(g_paletteIndex + 1, (int)scored.size() - 1);
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) g_paletteIndex = ImMax(g_paletteIndex - 1, 0);
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
			if (selected) ImGui::SetScrollHereY();
			if (selected && enter && c.enabled) chosen = c.run;
			ImGui::PopID();
		}
		if (scored.empty()) TextDim("일치하는 명령이 없습니다.");
		ImGui::EndChild();
		TextDim("↑↓ 이동  ·  Enter 실행  ·  Esc 닫기");
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) || chosen)
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
		for (int i = 0; i < kLineCount; ++i)
			if (ImGui::MenuItem((std::string(kLines[i].icon) + "  " + kLines[i].name).c_str(), kLines[i].command, false, app.attached)) app.Run(kLines[i].command);
		ImGui::Separator();
		if (ImGui::MenuItem("기본 라인 모두 실행", nullptr, false, app.attached)) app.RunAll();
		if (ImGui::MenuItem("게임 실행 (play)", nullptr, false, app.attached)) app.Run("play");
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
				ImGui::TextDisabled("%s", std::string(group) == "Forge" ? "dwo-Forge" : group);
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
		ImGui::TextDisabled("DwoStateMonitor");
		if (ImGui::MenuItem("설치 (에디터 전용)", nullptr, false, app.attached && app.stateMonitor == 0)) app.Run("sm-install");
		if (ImGui::MenuItem("설치 (패키지 게임 포함)", nullptr, false, app.attached && app.stateMonitor == 0)) app.Run("sm-install --runtime");
		if (ImGui::MenuItem("dwo-Lens 1.x 설치 옮기기", nullptr, false, app.attached && app.stateMonitorLegacy)) app.Run("update");
		if (ImGui::MenuItem("제거##sm", nullptr, false, app.stateMonitor == 1)) app.Run("sm-remove");
		ImGui::Separator();
		ImGui::TextDisabled("DwoCanvas");
		if (ImGui::MenuItem("설치##canvas", nullptr, false, app.attached && app.canvas == 0)) app.Run("canvas-install");
		if (ImGui::MenuItem("디자인 파일 열기...", nullptr, false, app.hasUProject)) { PickDesignFile(app); app.requestActivity = Activity::Canvas; }
		if (ImGui::MenuItem("가져오기", nullptr, false, !app.canvasFile.empty() && app.canvas != 0)) RunCanvas(app, "canvas-import");
		if (ImGui::MenuItem("제거##canvas", nullptr, false, app.canvas == 1)) app.Run("canvas-remove");
		ImGui::Separator();
		ImGui::TextDisabled("dwo-Forge");
		if (ImGui::MenuItem("부착", nullptr, false, app.hasUProject && !app.attached)) app.Run("attach");
		if (ImGui::MenuItem("시스템 업데이트", nullptr, false, app.attached)) app.Run("update");
		if (ImGui::MenuItem("환경 점검", nullptr, false, app.attached)) app.Run("doctor");
		if (ImGui::MenuItem("제거 (uninstall)...", nullptr, false, app.attached)) app.confirmDetach = true;
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
	std::string chip = app.hasUProject ? std::string(ICON_FOLDER) + "  " + app.projectName : std::string(ICON_FOLDER) + "  프로젝트 없음";
	float chipW = ImGui::CalcTextSize(chip.c_str()).x + S(24);
	float paletteW = S(240);
	ImGui::SetCursorPosX(right - chipW - paletteW - S(24));
	ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 255, 255, 12));
	if (ImGui::Button((std::string(ICON_SEARCH) + "  명령 검색        Ctrl+K").c_str(), ImVec2(paletteW, 0))) g_paletteOpen = true;
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

static void DrawActivityBar(App& app)
{
	ImGuiViewport* vp = ImGui::GetMainViewport();
	g_activityWidth = S(52);
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(ImVec2(g_activityWidth, vp->WorkSize.y - g_statusHeight));
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, S(6)));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0x2B, 0x2D, 0x30, 255));
	ImGui::Begin("##activity", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
	                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	app.RebuildProblems();
	int newProblems = 0;
	for (auto& p : app.problems) newProblems += p.isNew && !p.muted && p.severity != "info";
	int connected = app.live.ConnectedCount();
	const char* group = nullptr;
	for (auto& a : kActivities)
	{
		if (!group || std::string(group) != a.group)
		{
			// group caption: a thin rule and a small label (LENS / STATE / CANVAS / FORGE)
			ImVec2 c = ImGui::GetCursorScreenPos();
			float h = S(16);
			if (group) dl->AddLine(c + ImVec2(S(10), S(2)), c + ImVec2(g_activityWidth - S(10), S(2)), col::Border, S(1));
			const char* cap = GroupCaption(a.group);
			float fs = g_fonts.bold->FontSize * 0.62f;
			ImVec2 ts = g_fonts.bold->CalcTextSizeA(fs, FLT_MAX, 0, cap);
			dl->AddText(g_fonts.bold, fs, c + ImVec2((g_activityWidth - ts.x) * 0.5f, S(5)), WithAlpha(GroupColor(a.group), 0.85f), cap);
			ImGui::Dummy(ImVec2(g_activityWidth, h));
			group = a.group;
		}
		ImGui::PushID((int)a.id);
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImVec2 size(g_activityWidth, S(42));
		bool clicked = ImGui::InvisibleButton("act", size);
		bool hovered = ImGui::IsItemHovered();
		bool active = app.activity == a.id;
		if (active) dl->AddRectFilled(p, p + ImVec2(S(3), size.y), col::Accent);
		ImU32 color = active ? col::Text : hovered ? IM_COL32(0xC8, 0xCB, 0xD0, 255) : col::Dim;
		ImVec2 is = g_fonts.icon->CalcTextSizeA(g_fonts.icon->FontSize, FLT_MAX, 0, a.icon);
		dl->AddText(g_fonts.icon, g_fonts.icon->FontSize, p + (size - is) * 0.5f, color, a.icon);
		int badge = a.id == Activity::Problems ? newProblems : 0;
		if (badge)
		{
			std::string b = badge > 99 ? "99+" : std::to_string(badge);
			ImVec2 bs = ImGui::CalcTextSize(b.c_str()) * 0.8f;
			ImVec2 c = p + ImVec2(size.x - S(14), S(12));
			dl->AddCircleFilled(c, ImMax(bs.x, bs.y) * 0.5f + S(3), col::Fail);
			dl->AddText(g_fonts.bold, g_fonts.bold->FontSize * 0.8f, c - bs * 0.5f, IM_COL32(255, 255, 255, 255), b.c_str());
		}
		if (a.id == Activity::Live && connected)
			dl->AddCircleFilled(p + ImVec2(size.x - S(14), S(12)), S(4), col::Pass);
		if (a.id == Activity::Canvas && app.runner.running && app.runner.current.find("canvas-") != std::string::npos)
		{
			float t = (float)ImGui::GetTime() * 6;
			dl->PathArcTo(p + ImVec2(size.x - S(14), S(12)), S(4), t, t + 4.5f, 10);
			dl->PathStroke(col::Purple, 0, S(1.5f));
		}
		else if (a.id == Activity::Canvas && !app.canvasReports.empty() && app.CanvasReportAt(0)->errors > 0)
			dl->AddCircleFilled(p + ImVec2(size.x - S(14), S(12)), S(4), col::Fail);
		if (a.id == Activity::Overview && app.runner.running)
		{
			float t = (float)ImGui::GetTime() * 6;
			dl->PathArcTo(p + ImVec2(size.x - S(14), S(12)), S(4), t, t + 4.5f, 10);
			dl->PathStroke(col::Info, 0, S(1.5f));
		}
		if (hovered)
		{
			ImGui::SetNextWindowPos(ImVec2(p.x + size.x + S(4), p.y + S(10)));
			ImGui::BeginTooltip();
			if (std::string(a.group) == "Lens") { TextDim("Lens"); ImGui::SameLine(); }
			ImGui::Text("%s", a.name);
			ImGui::SameLine();
			TextDim(a.shortcut);
			ImGui::EndTooltip();
		}
		if (clicked) app.requestActivity = a.id;
		ImGui::PopID();
	}
	// settings at the bottom
	ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(52));
	ImVec2 p = ImGui::GetCursorScreenPos();
	if (ImGui::InvisibleButton("palette", ImVec2(g_activityWidth, S(46)))) g_paletteOpen = true;
	ImVec2 is = g_fonts.icon->CalcTextSizeA(g_fonts.icon->FontSize, FLT_MAX, 0, ICON_SEARCH);
	dl->AddText(g_fonts.icon, g_fonts.icon->FontSize, p + (ImVec2(g_activityWidth, S(46)) - is) * 0.5f, ImGui::IsItemHovered() ? col::Text : col::Dim, ICON_SEARCH);
	Tooltip("명령 팔레트 (Ctrl+K)");
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
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
		ImGui::TextUnformatted(app.hasUProject ? (app.projectName + (app.attached ? "  ·  UE " + app.Engine() : app.legacyAttached ? "  ·  dwo-Lens 1.x 부착 (이전 중)" : "  ·  부착 안 됨")).c_str() : "프로젝트 없음");
	}
	// right side
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
	float w = ImGui::CalcTextSize(liveText.c_str()).x + ImGui::CalcTextSize(probText.c_str()).x + S(80);
	ImGui::SameLine(ImGui::GetWindowWidth() - w);
	if (ImGui::Selectable(probText.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(probText.c_str()).x, 0))) app.requestActivity = Activity::Problems;
	ImGui::SameLine(0, S(20));
	StatusDot(connected ? col::Pass : app.live.listening ? col::Info : col::Fail, 3.5f, connected > 0);
	ImGui::SameLine();
	if (ImGui::Selectable(liveText.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(liveText.c_str()).x, 0))) app.requestActivity = Activity::Live;
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
	if (ToolButton(ICON_DOWN, "끝까지 따라가기", ui.consoleFollow)) ui.consoleFollow = !ui.consoleFollow;
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

// What uninstall will remove, shown before the user confirms.
struct UninstallInventory
{
	int runs = 0;
	double dataBytes = 0;
	std::vector<std::string> created;   // files dwo-Forge caused outside __dwo-forge (perf CSVs)
	bool inspector = false, stateMonitor = false, stateMonitorLegacy = false, launcher = false, canvas = false;
	std::string vcs, exclude;
};

static UninstallInventory TakeInventory(const App& app)
{
	UninstallInventory inv;
	std::error_code ec;
	fs::path data = app.ForgeDir() / "data";
	if (fs::is_directory(data / "runs", ec))
		for (auto& e : fs::directory_iterator(data / "runs", ec)) inv.runs += e.is_directory() ? 1 : 0;
	if (fs::is_directory(data, ec))
		for (auto it = fs::recursive_directory_iterator(data, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
			if (!ec && it->is_regular_file(ec)) inv.dataBytes += (double)it->file_size(ec);
	std::ifstream ledger(app.ForgeDir() / "created-files.txt");
	std::set<std::string> seen;
	for (std::string line; std::getline(ledger, line);)
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty() && seen.insert(line).second && fs::exists(app.project / fs::u8path(line), ec)) inv.created.push_back(line);
	}
	inv.inspector = fs::exists(app.project / "Plugins" / "__DwoLensInspector", ec);
	inv.stateMonitor = app.stateMonitor == 1;
	inv.stateMonitorLegacy = app.stateMonitorLegacy;
	inv.launcher = fs::exists(app.project / "__dwo-forge.bat", ec);
	inv.canvas = app.canvas == 1;
	inv.vcs = Str(app.state, "vcs", "own");
	inv.exclude = Str(app.state, "excludeFile");
	return inv;
}

static void DrawDetachModal(App& app)
{
	static UninstallInventory inv;
	static int dataChoice = 1;          // 0 keep, 1 delete
	static bool forget = true;
	if (app.confirmDetach)
	{
		inv = TakeInventory(app);
		dataChoice = inv.runs > 0 ? 0 : 1;
		ImGui::OpenPopup("dwo-Forge 제거");
		app.confirmDetach = false;
	}
	ImGui::SetNextWindowSize(ImVec2(S(560), 0));
	if (ImGui::BeginPopupModal("dwo-Forge 제거", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		Title("이 프로젝트에서 dwo-Forge를 제거합니다");
		TextDim("부착할 때 넣은 것과 dwo-Forge가 만든 것을 모두 지웁니다. .uproject와 프로젝트 파일은 건드리지 않습니다.");
		SectionHeader("지울 항목");
		auto row = [](bool on, const char* what, const std::string& detail) {
			TextColored(on ? col::Text : col::Muted, on ? ICON_DELETE : ICON_CHECK);
			ImGui::SameLine();
			ImGui::TextUnformatted(what);
			ImGui::SameLine();
			TextDim(detail);
		};
		row(inv.inspector, "Lens 검사 플러그인", "Plugins\\__DwoLensInspector");
		row(inv.stateMonitor, "DwoStateMonitor", !inv.stateMonitor ? "dwo-Forge가 설치하지 않음" : inv.stateMonitorLegacy ? "Plugins\\__StateMonitor (dwo-Lens 1.x)" : "Plugins\\__DwoStateMonitor");
		row(inv.launcher, "실행 파일", "__dwo-forge.bat");
		row(true, "설정", "__dwo-forge 폴더 (부착 정보, 화면 설정, 문제 음소거)");
		if (inv.vcs == "none") row(false, "git exclude", "형상관리 없음: 추가한 항목 없음");
		else row(true, "git exclude", inv.exclude.empty() ? "dwo-Forge 블록만 제거" : "dwo-Forge 블록만 제거 · " + inv.exclude);
		if (inv.canvas)
		{
			TextColored(col::Info, ICON_PIN);
			ImGui::SameLine();
			ImGui::TextUnformatted("DwoCanvas 유지");
			ImGui::SameLine();
			TextDim("생성된 위젯이 DwoCanvasRuntime을 씁니다. 필요 없으면 DwoCanvas 화면에서 제거하세요.");
		}

		SectionHeader("실행 기록과 로그");
		ImGui::Text("실행 기록 %d개 · %s", inv.runs, Bytes(inv.dataBytes).c_str());
		TextDim("분석·검증·로그·스모크·성능 결과, 리포트, 엔진 로그 (__dwo-forge\\data)");
		if (!inv.created.empty())
		{
			ImGui::Text("프로젝트에 남은 실행 파일 %d개", (int)inv.created.size());
			Tooltip(JoinStrings(json(inv.created), "\n").c_str());
			TextDim("성능 캡처가 Saved\\Profiling\\CSV에 만든 CSV");
		}
		ImGui::RadioButton("보존: 프로젝트 밖(%LOCALAPPDATA%\\dwo-Forge\\uninstalled)으로 옮김", &dataChoice, 0);
		ImGui::RadioButton("삭제: 기록과 파일을 모두 지움", &dataChoice, 1);
		ImGui::Checkbox("dwo-Forge의 최근 프로젝트 목록에서도 빼기", &forget);
		ImGui::Spacing();
		if (DangerButton("제거", ImVec2(S(120), 0)))
		{
			app.forgetAfterUninstall = forget;
			app.Run(dataChoice == 0 ? "uninstall --keep-data" : "uninstall --delete-data");
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("취소", ImVec2(S(120), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
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
	ImGuiIO& io = ImGui::GetIO();
	// shortcuts
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) g_paletteOpen = true;
	if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P, false)) g_paletteOpen = true;
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) { fs::path p; if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p); }
	if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { app.outputs.clear(); app.metrics.clear(); app.structureFor.clear(); app.Refresh(); }
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) app.showConsole = true;
	if (io.KeyCtrl && !io.WantTextInput)
		for (auto& a : kActivities)
			if (ImGui::IsKeyPressed(a.hotkey, false)) app.requestActivity = a.id;
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
	ImGui::DockSpace(dockspace, ImVec2(0, 0), ImGuiDockNodeFlags_None);
	ImGui::End();

	const ActivityDef& def = DefOf(app.activity);
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
		case Activity::Tools: DrawToolsNav(app); break;
		default: break;
		}
	}
	ImGui::End();
	// main
	std::string mainTitle = std::string(def.name) + "###main";
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
		case Activity::Tools: DrawToolsMain(app); break;
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
		case Activity::Tools: DrawToolsInspector(app); break;
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
	double shotDelay = delayArg.empty() ? 0.8 : _wtof(delayArg.c_str());
	if (modeArg == L"machine") ui.liveMode = 1;
	bool openUninstall = ArgValue(argc, argv, L"--dialog") == L"uninstall";
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
	SetWindowPos(hwnd, nullptr, 0, 0, (int)(1500 * scale), (int)(920 * scale), SWP_NOMOVE | SWP_NOZORDER);
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
				if (fs::is_directory(p, ec)) { app.SetProject(p); app.activity = Activity::Overview; }
			}
		}
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
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
