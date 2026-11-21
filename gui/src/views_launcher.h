// The launcher: dwo-Forge's home, drawn as one card next to the sidebar. Project page: tabs (all / installed /
// updates), project picker and search, then the tool library (thumbnail, text, action + "..." menu, version).
// PC page: Jenkins and engines. Tool versions come from 'forge.ps1 tools --json'.
#pragma once

#include "views_ci.h"

// ============================================================================ tool identity

struct ToolMeta
{
	const char* id;        // forge.ps1 tool id
	const char* name;
	const char* mono;      // two-letter tile shown in the sidebar
	const char* icon;      // glyph on the library thumbnail
	ImU32 color;
	const char* summary;
	Activity open;         // first screen of the tool
	const char* folder;    // what it puts into the project
};

static const ToolMeta kToolMeta[] = {
	{ "lens", "DwoLens", "Ln", ICON_SEARCH, col::Accent, "아키텍처·문제·검증·로그·성능·용량 검사와 실행 기록", Activity::Overview, "Plugins/__DwoLensInspector (편집기 전용, 기본 꺼짐)" },
	{ "statemonitor", "DwoStateMonitor", "SM", ICON_EYE, col::Pass, "실행 중인 게임의 상태를 실시간으로 보는 플러그인", Activity::Live, "Plugins/__DwoStateMonitor" },
	{ "canvas", "DwoCanvas", "Cv", ICON_CANVAS, col::Purple, "디자인 파일(*.dwocanvas.json)을 UMG 위젯 블루프린트로 가져오기", Activity::Canvas, "Plugins/__DwoCanvas" },
	{ "ci", "DwoBuilder", "Bd", ICON_PACKAGE, col::Teal, "C++ 컴파일·패키지 빌드, push·merge·커밋 때 자동 빌드(Jenkins)", Activity::Ci, "Jenkins 작업, .git/hooks (Jenkins는 PC에 하나)" },
};
static const int kToolCount = 4;

static const ToolMeta* ToolMetaOf(const std::string& id)
{
	for (auto& t : kToolMeta)
		if (id == t.id) return &t;
	return nullptr;
}

// Which tool a screen belongs to ("" = the launcher itself).
static const char* ToolOfActivity(Activity a)
{
	switch (a)
	{
	case Activity::Overview: case Activity::Architecture: case Activity::Problems: case Activity::Tests:
	case Activity::Logs: case Activity::Performance: case Activity::History: return "lens";
	case Activity::Live: return "statemonitor";
	case Activity::Canvas: return "canvas";
	case Activity::Ci: return "ci";
	default: return "";
	}
}

// Rounded tile with the tool's two letters.
static void DrawToolTile(ImDrawList* dl, ImVec2 pos, float size, const ToolMeta& t, float alpha = 1.0f)
{
	dl->AddRectFilled(pos, pos + ImVec2(size, size), WithAlpha(t.color, alpha), size * 0.22f);
	ImFont* f = g_fonts.bold;
	float fs = size * 0.42f;
	ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, t.mono);
	dl->AddText(f, fs, pos + (ImVec2(size, size) - ts) * 0.5f, IM_COL32(255, 255, 255, (int)(255 * alpha)), t.mono);
}

// ============================================================================ tool status (forge.ps1 tools --json)

struct ToolView
{
	std::string id, state, reason, detail, installedVersion, availableVersion, installedEngine, installedAt;
	bool available = true;
	bool known = false;   // from forge.ps1 (versions); false = file-system guess while it loads
};

struct ToolsProbe
{
	std::mutex mutex;
	json doc;
	fs::path docProject;
	std::string error;
	std::atomic<bool> busy{ false };
	std::thread worker;
	~ToolsProbe() { if (worker.joinable()) worker.join(); }

	void Start(const fs::path& home, const fs::path& project)
	{
		if (busy || project.empty()) return;
		if (worker.joinable()) worker.join();
		busy = true;
		std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + (home / "tools" / "forge.ps1").wstring() +
		                   L"\" -Command tools -Project \"" + project.wstring() + L"\" --json";
		worker = std::thread([this, cmd, project] {
			std::string out;
			DWORD code = 1;
			json parsed;
			std::string err;
			if (!CaptureOutput(cmd, project, out, code)) err = "forge.ps1을 실행할 수 없습니다.";
			else
			{
				size_t at = out.find('{');
				try { parsed = json::parse(out.substr(at == std::string::npos ? 0 : at)); }
				catch (...) { err = ConsoleToUtf8(out); }
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (err.empty()) { doc = parsed; docProject = project; }
				error = err;
			}
			busy = false;
		});
	}

	json Get(const fs::path& project)
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::error_code ec;
		if (docProject.empty() || project.empty() || !fs::equivalent(docProject, project, ec)) return json();
		return doc;
	}
};

static ToolsProbe g_tools;

// Called every frame: refresh tool versions after a command finished or the project changed.
static void PollTools(App& app)
{
	if (app.toolsDirty && !g_tools.busy && app.HomeValid() && app.hasUProject)
	{
		app.toolsDirty = false;
		g_tools.Start(app.home, app.project);
	}
}

static std::vector<ToolView> ToolViews(App& app)
{
	std::vector<ToolView> list;
	json doc = g_tools.Get(app.project);
	for (auto& m : kToolMeta)
	{
		ToolView v;
		v.id = m.id;
		bool found = false;
		for (auto& t : Arr(doc, "tools"))
			if (Str(t, "id") == m.id)
			{
				v.state = Str(t, "state", "none");
				v.reason = Str(t, "reason");
				v.detail = Str(t, "detail");
				v.installedVersion = Str(t, "installedVersion");
				v.availableVersion = Str(t, "availableVersion");
				v.installedEngine = Str(t, "installedEngine");
				v.installedAt = Str(t, "installedAt");
				v.available = t.value("available", true);
				v.known = true;
				found = true;
			}
		if (!found)
		{
			std::string id = m.id;
			bool installed = id == "lens" ? app.lensInstalled : id == "statemonitor" ? app.stateMonitor == 1 : id == "canvas" ? app.canvas == 1 : app.ciInstalled;
			bool own = id == "statemonitor" ? app.stateMonitor == 2 : id == "canvas" ? app.canvas == 2 : false;
			v.state = own ? "own" : id == "statemonitor" && app.stateMonitorLegacy ? "legacy" : installed ? "installed" : "none";
		}
		list.push_back(v);
	}
	return list;
}

static bool ToolPresent(const ToolView& v) { return v.state != "none"; }

static int UpdateCount(const std::vector<ToolView>& views)
{
	int n = 0;
	for (auto& v : views) n += v.state == "update" || v.state == "legacy";
	return n;
}

static std::string ShortDate(const std::string& iso)
{
	std::string s = iso.substr(0, 16);
	if (s.size() > 10 && s[10] == 'T') s[10] = ' ';
	return s;
}

// ============================================================================ launcher state

namespace launcher
{
enum Page { Project, Pc, About };
struct Ui
{
	Page page = Project;
	int tab = 0;                   // 0 all tools, 1 installed, 2 updates
	int pcTab = 0;                 // 0 Jenkins, 1 engines
	char search[96] = {};
	std::string confirmRemove;     // tool id waiting for confirmation
	std::string details;           // tool id whose details popup is open
	bool logOpen = false;          // run log drawer under the launcher
	json projects;                 // Jenkins projects.json (PC page), re-read every few seconds
	double projectsAt = -100;
};
static Ui& State()
{
	static Ui u;
	return u;
}
} // namespace launcher

// Page requests from elsewhere (tool screens, --view): 1 Jenkins, 2 engines, 3 about.
static void ConsumeLauncherRequest()
{
	launcher::Ui& u = launcher::State();
	if (g_launcherPageRequest < 0) return;
	if (g_launcherPageRequest == 1 || g_launcherPageRequest == 2) { u.page = launcher::Pc; u.pcTab = g_launcherPageRequest - 1; }
	else if (g_launcherPageRequest == 3) u.page = launcher::About;
	else u.page = launcher::Project;
	g_launcherPageRequest = -1;
}

static fs::path CiHomeDir()
{
	const wchar_t* ciHome = _wgetenv(L"DWO_FORGE_CI_HOME");
	if (ciHome && *ciHome) return fs::path(ciHome);
	return fs::path(_wgetenv(L"LOCALAPPDATA") ? _wgetenv(L"LOCALAPPDATA") : L".") / "dwo-Forge" / "ci";
}

// ============================================================================ building blocks (card, tabs, thumbnails)

// Large rounded tile with the tool's colour as a diagonal gradient and its icon, like a store thumbnail.
static void DrawToolThumb(ImDrawList* dl, ImVec2 a, ImVec2 b, const ToolMeta& m, bool dim)
{
	int v0 = dl->VtxBuffer.Size;
	dl->AddRectFilled(a, b, IM_COL32_WHITE, S(8));
	int v1 = dl->VtxBuffer.Size;
	ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, v1, a, b, Mix(m.color, col::Bg, dim ? 0.72f : 0.28f), Mix(m.color, col::Bg, dim ? 0.88f : 0.74f));
	dl->AddRect(a, b, IM_COL32(255, 255, 255, 18), S(8));
	ImFont* f = g_fonts.iconLarge;
	float fs = f->FontSize;
	ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, m.icon);
	dl->AddText(f, fs, (a + b - ts) * 0.5f, IM_COL32(255, 255, 255, dim ? 120 : 235), m.icon);
}

// Tab with an accent underline (the card's top row).
static bool UnderlineTab(const char* label, bool active)
{
	ImGui::PushFont(g_fonts.title);
	ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 size(ts.x + S(24), ts.y + S(18));
	bool clicked = ImGui::InvisibleButton(label, size);
	bool hovered = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImU32 c = active ? col::Accent : hovered ? col::Text : col::Dim;
	dl->AddText(g_fonts.title, g_fonts.title->FontSize, p + ImVec2(S(12), S(8)), c, label, ImGui::FindRenderedTextEnd(label));
	if (active) dl->AddRectFilled(p + ImVec2(0, size.y - S(3)), p + ImVec2(size.x, size.y), col::Accent, S(1.5f));
	ImGui::PopFont();
	return clicked;
}

static void CardRule()
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = ImGui::GetContentRegionAvail().x;
	ImGui::GetWindowDrawList()->AddLine(p, p + ImVec2(w, 0), col::Border, S(1));
	ImGui::Dummy(ImVec2(w, S(1)));
}

// A rounded panel lighter than the window, filling the rest of the space (or 'height').
static void BeginCard(const char* id, float height = 0)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Panel);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0x3A, 0x3C, 0x41, 255));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0x43, 0x45, 0x4A, 255));
	ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0x3A, 0x3C, 0x41, 255));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0x46, 0x48, 0x4D, 255));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(6)));
	ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
}
static void EndCard()
{
	ImGui::EndChild();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(5);
}

// Banner row inside the card (connect, updates, engine change).
static bool Banner(const char* id, ImU32 color, const std::string& text, const char* action)
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = ImGui::GetContentRegionAvail().x;
	float wrap = w - S(160) - S(14);
	float textH = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).y;
	float h = ImMax(ImGui::GetFrameHeight(), textH) + S(16);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + ImVec2(w, h), WithAlpha(color, 0.10f), S(6));
	dl->AddRect(p, p + ImVec2(w, h), WithAlpha(color, 0.45f), S(6));
	ImGui::SetCursorScreenPos(p + ImVec2(S(14), S(8) + (h - S(16) - textH) * 0.5f));
	ImGui::PushTextWrapPos(p.x + S(14) + wrap);
	ImGui::TextUnformatted(text.c_str());
	ImGui::PopTextWrapPos();
	bool clicked = false;
	if (action)
	{
		ImGui::PushID(id);
		float bw = ImGui::CalcTextSize(action).x + ImGui::GetStyle().FramePadding.x * 2 + S(8);
		ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - S(12), p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
		ImGui::BeginDisabled(false);
		clicked = PrimaryButton(action, ImVec2(bw, 0));
		ImGui::EndDisabled();
		ImGui::PopID();
	}
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + S(10)));
	ImGui::Dummy(ImVec2(0, 0));
	return clicked;
}

// ============================================================================ tool list

static const char* StateLabel(const ToolView& v)
{
	if (v.state == "installed") return "설치됨";
	if (v.state == "update") return "업데이트 있음";
	if (v.state == "own") return "프로젝트에 포함";
	if (v.state == "legacy") return "이전 형식";
	return "설치 안 됨";
}

static ImU32 StateColor(const ToolView& v)
{
	if (v.state == "installed") return col::Pass;
	if (v.state == "update" || v.state == "legacy") return col::Warn;
	if (v.state == "own") return col::Info;
	return col::Muted;
}

static std::string VersionText(const ToolView& v)
{
	if (!v.known) return v.state == "none" ? std::string() : "확인 중...";
	if (v.state == "update")
	{
		if (v.reason == "engine-changed") return "UE " + v.installedEngine + "용 → 다시 빌드";
		if (v.reason == "unknown-version") return "이전 버전 → " + v.availableVersion;
		return v.installedVersion + " → " + v.availableVersion;
	}
	if (v.state == "own") return "프로젝트 자체";
	if (v.state == "legacy") return "dwo-Lens 1.x";
	return "버전 " + v.availableVersion;
}

static std::string TagLine(App& app, const ToolMeta& m, const ToolView& v)
{
	std::string id = m.id;
	std::string t = id == "ci" ? "프로젝트 + PC" : "프로젝트";
	if (id == "lens" && ToolPresent(v))
	{
		int done = 0, failed = 0;
		for (int i = 0; i < kLensLineCount; ++i)
			if (const RunInfo* r = app.Latest(i)) { ++done; failed += r->verdict == "fail"; }
		t += Fmt("  ·  검사 %d종 중 %d종 실행, 실패 %d", kLensLineCount, done, failed);
	}
	else if (id == "statemonitor" && v.detail == "runtime") t += "  ·  패키지 게임에도 포함";
	else if (id == "statemonitor" && v.detail == "editor") t += "  ·  에디터·PIE 전용";
	else if (id == "canvas" && !app.canvasVersion.empty()) t += "  ·  플러그인 " + app.canvasVersion;
	else if (id == "ci")
	{
		if (!v.detail.empty())
		{
			ci::Snapshot s = ci::Instance().Get();
			t += "  ·  작업 " + v.detail;
			if (!s.builds.empty())
			{
				std::string verdict = ci::Verdict(s.builds.front());
				const char* label = verdict == "running" ? "진행 중" : VerdictLabel(verdict);
				t += Fmt("  ·  최근 빌드 #%d ", s.builds.front().number) + (label ? label : verdict);
			}
		}
		else t += "  ·  로컬 빌드는 설치 없이 사용";
	}
	if (v.state == "none" || v.state == "own") return t;
	return t + "  ·  " + m.folder;
}

// Runs a tool action; on a project that is not connected yet, connects first.
static void RunToolCommand(App& app, const std::string& command)
{
	if (!app.attached) app.Run("connect");
	app.Run(command);
}

static void DrawToolDetails(App& app, const ToolMeta& m, const ToolView& v)
{
	KeyValue("상태", StateLabel(v), 90);
	if (!v.installedVersion.empty()) KeyValue("설치 버전", v.installedVersion, 90);
	if (!v.availableVersion.empty()) KeyValue("최신 버전", v.availableVersion, 90);
	if (!v.installedEngine.empty()) KeyValue("빌드 엔진", "UE " + v.installedEngine, 90);
	if (!v.installedAt.empty()) KeyValue("설치 일시", ShortDate(v.installedAt), 90);
	KeyValue("범위", m.id == std::string("ci") ? "프로젝트 + PC" : "프로젝트", 90);
	KeyValue("설치 위치", m.folder, 90);
	ImGui::Spacing();
	TextDim("명령줄 (누르면 복사)");
	std::string id = m.id == std::string("ci") ? "builder" : m.id;
	for (const char* verb : { "install", "update", "remove" })
	{
		std::string line = "__dwo-forge.bat tool " + std::string(verb) + " " + id;
		if (ImGui::Selectable((std::string(ICON_COPY) + "  " + line).c_str())) { ImGui::SetClipboardText(line.c_str()); PushToast("복사함", line, col::Info); }
	}
	(void)app;
}

static void DrawToolItem(App& app, const ToolMeta& m, const ToolView& v, const json& doc, bool last)
{
	launcher::Ui& u = launcher::State();
	bool idle = !app.runner.running;
	std::string id = m.id;
	bool present = ToolPresent(v);
	bool managed = v.state == "installed" || v.state == "update" || v.state == "legacy";
	ImGui::PushID(m.id);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = ImGui::GetContentRegionAvail().x;
	float h = S(100);
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("row", ImVec2(w, h));
	bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
	if (hovered && ImGui::IsMouseDoubleClicked(0) && (present || id == "ci")) app.requestActivity = m.open;
	if (hovered) dl->AddRectFilled(p + ImVec2(-S(8), S(4)), p + ImVec2(w + S(8), h - S(4)), IM_COL32(255, 255, 255, 8), S(8));

	// thumbnail: full size, smaller in a narrow card, none when there is no room
	ImVec2 thumb = w >= S(640) ? ImVec2(S(116), S(66)) : w >= S(480) ? ImVec2(S(92), S(54)) : ImVec2(0, 0);
	ImVec2 ta = p + ImVec2(0, S(14));
	ImVec2 tb = ta + thumb;
	if (thumb.x > 0)
	{
		ta.y = p.y + (h - thumb.y) * 0.5f;
		tb = ta + thumb;
		DrawToolThumb(dl, ta, tb, m, v.state == "none");
	}
	ta.y = p.y + S(13);
	tb.y = ta.y + S(74);   // the text block: state, name, description, tags

	// text block
	float rightW = S(170);
	float tx = thumb.x > 0 ? p.x + thumb.x + S(18) : p.x;
	float textW = ImMax(S(80), p.x + w - rightW - tx - S(16));
	ImGui::PushClipRect(ImVec2(tx, p.y), ImVec2(tx + textW, p.y + h), true);
	ImGui::SetCursorScreenPos(ImVec2(tx, ta.y - S(1)));
	ImGui::PushFont(g_fonts.bold);
	ImGui::PushStyleColor(ImGuiCol_Text, StateColor(v));
	ImGui::SetWindowFontScale(0.82f);
	ImGui::TextUnformatted(StateLabel(v));
	ImGui::SetWindowFontScale(1.0f);
	ImGui::PopStyleColor();
	ImGui::PopFont();
	ImGui::SetCursorScreenPos(ImVec2(tx, ImGui::GetCursorScreenPos().y - S(2)));
	ImGui::PushFont(g_fonts.title);
	ImGui::TextUnformatted(m.name);
	ImGui::PopFont();
	ImGui::SetCursorScreenPos(ImVec2(tx, ImGui::GetCursorScreenPos().y + S(1)));
	{
		// description: at most the space above the tag line
		float top = ImGui::GetCursorScreenPos().y;
		float bottom = tb.y - ImGui::GetTextLineHeight() - S(2);
		ImGui::PushClipRect(ImVec2(tx, top), ImVec2(tx + textW, bottom), true);
		ImGui::PushTextWrapPos(tx + textW);
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0xC4, 0xC7, 0xCC, 255));
		ImGui::TextWrapped("%s", m.summary);
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
		ImGui::PopClipRect();
	}
	ImGui::SetCursorScreenPos(ImVec2(tx, tb.y - ImGui::GetTextLineHeight() + S(1)));
	ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
	ImGui::SetWindowFontScale(0.9f);
	std::string tags = TagLine(app, m, v);
	ImGui::TextUnformatted(tags.c_str());
	ImGui::SetWindowFontScale(1.0f);
	ImGui::PopStyleColor();
	ImGui::PopClipRect();

	// actions: primary button + "..." menu, version under them
	std::string primary, icon;
	if (v.state == "none") { primary = "설치"; icon = ICON_DOWNLOAD; }
	else if (v.state == "update") { primary = "업데이트"; icon = ICON_SYNC; }
	else if (v.state == "legacy") { primary = "옮기기"; icon = ICON_SYNC; }
	else { primary = "열기"; icon = ICON_OPEN; }
	json pc = Obj(doc, "pc");
	bool ciBlocked = id == "ci" && v.state == "none" && v.known && !Obj(pc, "jenkins").value("installed", false) && Str(Obj(pc, "bundle"), "file").empty();
	std::string label = icon + "  " + primary;
	float bw = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + S(18);
	float mw = ImGui::GetFrameHeight() + S(6);
	float bx = p.x + w - bw - mw - ImGui::GetStyle().ItemSpacing.x;
	ImGui::SetCursorScreenPos(ImVec2(bx, ta.y + S(4)));
	bool mutates = primary != "열기";
	ImGui::BeginDisabled(mutates && (!idle || !app.hasUProject || (primary == "설치" && (ciBlocked || !v.available))));
	bool clicked = primary == "열기" ? PrimaryButton(label.c_str(), ImVec2(bw, 0)) : ImGui::Button(label.c_str(), ImVec2(bw, 0));
	ImGui::EndDisabled();
	if (primary == "설치" && ciBlocked) Tooltip("자동 빌드에는 PC의 Jenkins가 필요합니다. 인터넷 PC에서 dwo-Forge.bat ci-bundle로 만든 번들을 dwo-Forge의 bundles 폴더에 두세요. 로컬 빌드는 ··· > 열기로 설치 없이 쓸 수 있습니다.");
	else if (primary == "설치" && !app.attached) Tooltip("프로젝트를 dwo-Forge에 연결한 뒤 설치합니다.");
	if (clicked)
	{
		if (primary == "설치") RunToolCommand(app, "tool install " + id);
		else if (primary == "업데이트" || primary == "옮기기") app.Run("tool update " + id);
		else app.requestActivity = m.open;
	}
	ImGui::SameLine();
	if (ImGui::Button("···", ImVec2(mw, 0))) ImGui::OpenPopup("more");
	if (ImGui::BeginPopup("more"))
	{
		if (ImGui::MenuItem("열기", nullptr, false, present || id == "ci")) app.requestActivity = m.open;
		if (id == "statemonitor" && ImGui::MenuItem("설치 (패키지 게임 포함)", nullptr, false, idle && v.state == "none")) RunToolCommand(app, "tool install statemonitor --runtime");
		if (ImGui::MenuItem("업데이트", nullptr, false, idle && managed)) app.Run("tool update " + id);
		if (ImGui::MenuItem("제거...", nullptr, false, idle && managed)) u.confirmRemove = id;
		ImGui::Separator();
		if (ImGui::MenuItem("자세히...")) u.details = id;
		ImGui::EndPopup();
	}
	std::string ver = VersionText(v);
	ImVec2 vs = ImGui::CalcTextSize(ver.c_str());
	ImGui::SetCursorScreenPos(ImVec2(p.x + w - vs.x, ta.y + S(4) + ImGui::GetFrameHeight() + S(8)));
	if (v.state == "update" || v.state == "legacy") TextColored(col::Warn, ver.c_str());
	else { ImGui::PushFont(g_fonts.bold); TextDim(ver); ImGui::PopFont(); }

	// confirm removal / details
	if (u.confirmRemove == id) { ImGui::OpenPopup("confirm-remove"); u.confirmRemove.clear(); }
	if (ImGui::BeginPopup("confirm-remove"))
	{
		TextBold(std::string(m.name) + " 제거");
		ImGui::PushTextWrapPos(S(360));
		if (id == "ci") TextDim("이 프로젝트의 Jenkins 작업(빌드 기록 포함)과 git 훅을 지웁니다. PC의 Jenkins와 다른 프로젝트는 그대로입니다.");
		else if (id == "lens") TextDim("Plugins/__DwoLensInspector를 지웁니다. 실행 기록은 남고, 다시 설치하면 이어서 씁니다.");
		else TextDim(std::string(m.folder) + "를 지웁니다. 프로젝트 코드나 에셋이 아직 쓰고 있으면 제거를 멈추고 목록을 보여 줍니다.");
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		if (DangerButton("제거")) { app.Run("tool remove " + id); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("취소")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (u.details == id) { ImGui::OpenPopup("details"); u.details.clear(); }
	ImGui::SetNextWindowSize(ImVec2(S(460), 0));
	if (ImGui::BeginPopup("details"))
	{
		Title(m.name);
		ImGui::PushTextWrapPos(0);
		TextDim(m.summary);
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		DrawToolDetails(app, m, v);
		ImGui::EndPopup();
	}

	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
	if (!last) CardRule();
	ImGui::PopID();
}

// ============================================================================ project page

static void ProjectPicker(App& app)
{
	ImGui::AlignTextToFramePadding();
	TextDim("프로젝트");
	ImGui::SameLine();
	std::string name = app.hasUProject ? app.projectName : std::string("선택 안 됨");
	float bw = ImMax(S(220), ImGui::CalcTextSize(name.c_str()).x + S(48));
	ImVec2 bp = ImGui::GetCursorScreenPos();
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), ImGui::GetStyle().FramePadding.y));
	if (ImGui::Button(("  " + name + "##projectpick").c_str(), ImVec2(bw, 0))) ImGui::OpenPopup("projects");
	ImGui::PopStyleVar(2);
	{
		// caret at the right edge, like a dropdown
		float fs = g_fonts.icon->FontSize * 0.6f;
		ImVec2 cs = g_fonts.icon->CalcTextSizeA(fs, FLT_MAX, 0, ICON_DOWN);
		ImGui::GetWindowDrawList()->AddText(g_fonts.icon, fs, ImVec2(bp.x + bw - cs.x - S(12), bp.y + (ImGui::GetFrameHeight() - cs.y) * 0.5f), col::Dim, ICON_DOWN);
	}
	if (app.hasUProject) Tooltip(PathUtf8(app.project).c_str());
	if (ImGui::BeginPopup("projects"))
	{
		for (auto& r : app.recent)
		{
			fs::path p = fs::u8path(r);
			std::error_code ec;
			if (!fs::exists(p, ec)) continue;
			ImGui::PushID(r.c_str());
			if (ImGui::Selectable(PathUtf8(p.filename()).c_str(), p == app.project, 0, ImVec2(S(320), 0))) app.SetProject(p);
			ImGui::SameLine(S(200));
			TextDim(PathUtf8(p.parent_path().filename()));
			ImGui::PopID();
		}
		ImGui::Separator();
		if (ImGui::Selectable((std::string(ICON_FOLDER) + "  프로젝트 열기...").c_str()))
		{
			fs::path p;
			if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p);
		}
		ImGui::EndPopup();
	}
}

static void ProjectMenu(App& app)
{
	if (ImGui::Button("···##project", ImVec2(ImGui::GetFrameHeight() + S(6), 0))) ImGui::OpenPopup("projectmenu");
	Tooltip("프로젝트 관리");
	if (ImGui::BeginPopup("projectmenu"))
	{
		bool idle = !app.runner.running;
		if (ImGui::MenuItem("폴더 열기", nullptr, false, app.hasUProject)) OpenWithShell(app.project);
		ImGui::Separator();
		if (ImGui::MenuItem("연결", nullptr, false, idle && app.hasUProject && !app.attached)) app.Run("connect");
		if (ImGui::MenuItem("연결 새로 고침", nullptr, false, idle && app.attached)) app.Run("update");
		if (ImGui::MenuItem("환경 점검", nullptr, false, idle && app.attached)) app.Run("doctor");
		if (ImGui::MenuItem("상태 확인", nullptr, false, idle && app.attached)) app.Run("status");
		ImGui::Separator();
		if (ImGui::MenuItem("연결 해제...", nullptr, false, idle && app.attached)) app.confirmDetach = true;
		ImGui::EndPopup();
	}
}

static void DrawProjectPage(App& app)
{
	launcher::Ui& u = launcher::State();
	json doc = g_tools.Get(app.project);
	const json& pj = Obj(doc, "project");
	std::vector<ToolView> views = ToolViews(app);
	int installed = 0;
	for (auto& v : views) installed += ToolPresent(v);
	int updates = app.attached ? UpdateCount(views) : 0;

	// tabs
	ImGui::Dummy(ImVec2(0, S(2)));
	if (UnderlineTab("모든 도구", u.tab == 0)) u.tab = 0;
	ImGui::SameLine(0, S(4));
	if (UnderlineTab(Fmt("설치됨 (%d)##installed", installed).c_str(), u.tab == 1)) u.tab = 1;
	ImGui::SameLine(0, S(4));
	if (UnderlineTab(Fmt("업데이트 (%d)##updates", updates).c_str(), u.tab == 2)) u.tab = 2;
	CardRule();
	ImGui::Dummy(ImVec2(0, S(6)));

	// filter row: project, facts, search, project menu. In a narrow card the facts drop out, then search narrows.
	float rowW = ImGui::GetContentRegionAvail().x;
	float pickerW = S(300), menuW = ImGui::GetFrameHeight() + S(14);
	float searchW = ImClamp(rowW - pickerW - menuW - S(24), S(120), S(260));
	ProjectPicker(app);
	std::string factsText;
	if (app.hasUProject)
	{
		std::string eng = Str(pj, "engine", app.attached ? app.Engine() : "");
		factsText = std::string(app.isCodeProject ? "C++" : "Blueprint 전용") + (eng.empty() ? "" : "  ·  UE " + eng) +
		            (app.attached ? (Str(app.state, "vcs") == "none" ? "  ·  버전 관리 없음" : "  ·  git") : "  ·  연결 안 됨");
	}
	bool showFacts = !factsText.empty() && rowW - pickerW - searchW - menuW - S(40) > ImGui::CalcTextSize(factsText.c_str()).x;
	if (showFacts)
	{
		ImGui::SameLine(0, S(14));
		ImGui::AlignTextToFramePadding();
		TextDim(factsText);
	}
	float right = ImGui::GetWindowContentRegionMax().x;
	ImGui::SameLine(right - searchW - ImGui::GetFrameHeight() - S(14));
	SearchBox("##toolsearch", u.search, sizeof(u.search), ICON_SEARCH "  도구 검색", searchW);
	ImGui::SameLine();
	ProjectMenu(app);
	ImGui::Dummy(ImVec2(0, S(6)));

	if (!app.hasUProject)
	{
		if (EmptyState(ICON_FOLDER, "프로젝트를 여세요", "언리얼 프로젝트 폴더(.uproject가 있는 폴더)를 고르거나 이 창으로 끌어다 놓으세요. 그다음 쓸 도구를 설치하면 됩니다.", "프로젝트 열기..."))
		{
			fs::path p;
			if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p);
		}
		return;
	}
	if (!app.attached && app.legacyAttached)
	{
		EmptyState(ICON_SYNC, "dwo-Lens 1.x로 연결된 프로젝트입니다",
		           "__dwo-lens 폴더와 실행 파일, git exclude 블록을 dwo-Forge 이름으로 옮기고 있습니다. 실행 기록과 설정은 그대로 유지됩니다.");
		return;
	}

	// banners
	std::string engine = Str(pj, "engine");
	std::string connectedEngine = Str(pj, "connectedEngine");
	ImGui::BeginDisabled(app.runner.running);
	if (!app.attached)
	{
		if (Banner("connect", col::Info, "이 프로젝트는 dwo-Forge에 연결되지 않았습니다. 도구의 설치를 누르면 연결과 설치를 함께 합니다. "
		                                 "연결은 __dwo-forge 폴더와 실행 파일만 만들고 git에서 숨기며, 연결 해제로 흔적 없이 지울 수 있습니다.", "연결만 하기"))
			app.Run("connect");
	}
	else if (!engine.empty() && !connectedEngine.empty() && engine != connectedEngine)
	{
		if (Banner("engine", col::Warn, "연결할 때 UE " + connectedEngine + ", 지금 UE " + engine + ". 연결을 새로 고치면 도구도 새 엔진으로 다시 빌드합니다.", "연결 새로 고침"))
			app.Run("update");
	}
	else if (updates > 0)
	{
		if (Banner("updates", col::Warn, Fmt("이 dwo-Forge(%s)에 더 새 버전이 있는 도구가 %d개 있습니다.", Str(Obj(doc, "forge"), "version").c_str(), updates), "모두 업데이트"))
			app.Run("tool update --all");
	}
	ImGui::EndDisabled();
	if (!g_tools.error.empty()) TextColored(col::Fail, ("도구 상태를 읽지 못했습니다: " + g_tools.error.substr(0, 160)).c_str());

	// list
	std::vector<int> shown;
	for (int i = 0; i < kToolCount; ++i)
	{
		const ToolView& v = views[i];
		if (u.tab == 1 && !ToolPresent(v)) continue;
		if (u.tab == 2 && !(v.state == "update" || v.state == "legacy")) continue;
		if (u.search[0] && !ContainsNoCase(std::string(kToolMeta[i].name) + " " + kToolMeta[i].summary, u.search)) continue;
		shown.push_back(i);
	}
	TextDim(Fmt("도구 (%d)", (int)shown.size()) + (g_tools.busy ? "   ·   버전 확인 중..." : ""));
	ImGui::BeginChild("tools", ImVec2(0, 0), ImGuiChildFlags_None);
	for (size_t k = 0; k < shown.size(); ++k) DrawToolItem(app, kToolMeta[shown[k]], views[shown[k]], doc, k + 1 == shown.size());
	if (shown.empty()) TextDim(u.tab == 2 ? "업데이트할 도구가 없습니다." : u.tab == 1 ? "설치한 도구가 없습니다. '모든 도구'에서 설치하세요." : "검색과 일치하는 도구가 없습니다.");
	ImGui::EndChild();
}

// ============================================================================ PC components

static void DrawJenkinsPanel(App& app)
{
	launcher::Ui& u = launcher::State();
	ci::Client& c = ci::Instance();
	c.Ensure(app.hasUProject ? app.project : fs::path());
	ci::Snapshot s = c.Get();
	json doc = g_tools.Get(app.project);
	const json& pc = Obj(doc, "pc");
	ImGui::PushTextWrapPos(0);
	TextDim("DwoBuilder의 자동 빌드에 쓰는 Jenkins입니다. PC에 하나만 설치되고 127.0.0.1에서만 열리며, 프로젝트마다 DwoBuilder가 작업을 하나씩 만듭니다.");
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	if (!s.installed) Pill("설치 안 됨", col::Muted);
	else if (s.running) Pill(Fmt("실행 중 · 127.0.0.1:%d", s.port), col::Pass);
	else Pill("중지됨", col::Warn);
	if (!s.version.empty()) { ImGui::SameLine(); TextDim("Jenkins " + s.version + (s.java.empty() ? "" : " · " + s.java)); }
	ImGui::Spacing();
	KeyValue("위치", Str(Obj(pc, "jenkins"), "home", PathUtf8(CiHomeDir())), 110);
	std::string bundle = Str(Obj(pc, "bundle"), "file");
	KeyValue("오프라인 번들", bundle.empty() ? "없음 (인터넷 PC에서 dwo-Forge.bat ci-bundle)" : bundle, 110);
	if (!s.user.empty()) KeyValue("계정", s.user, 110);
	ImGui::Spacing();
	bool idle = !app.runner.running;
	ImGui::BeginDisabled(!idle);
	auto pickBundle = [&](const char* verb) {
		fs::path p;
		if (PickOpen(g_hwnd, false, L"dwo-Forge CI 번들 선택", p, L"dwo-Forge CI 번들 (dwo-forge-ci-bundle-*.zip)", L"dwo-forge-ci-bundle-*.zip;*.zip"))
			app.Run(std::string(verb) + " --bundle \"" + PathUtf8(p) + "\"");
	};
	if (!s.installed)
	{
		if (!bundle.empty())
		{
			if (PrimaryButton((std::string(ICON_DOWNLOAD) + "  설치").c_str())) app.Run("ci-install");
			Tooltip(("bundles 폴더의 " + bundle + "로 설치합니다. 네트워크에 접속하지 않습니다.").c_str());
			ImGui::SameLine();
		}
		if (ImGui::Button((std::string(ICON_FOLDER) + "  번들 파일로 설치...").c_str())) pickBundle("ci-install");
	}
	else
	{
		if (s.running) { if (ImGui::Button((std::string(ICON_STOP) + "  중지").c_str())) app.Run("ci-stop"); }
		else if (PrimaryButton((std::string(ICON_PLAY) + "  시작").c_str())) app.Run("ci-start");
		ImGui::SameLine();
		ImGui::BeginDisabled(!s.running);
		if (ImGui::Button((std::string(ICON_OPEN) + "  Jenkins 화면 열기").c_str()))
			ShellExecuteW(nullptr, L"open", ToWide(Fmt("http://127.0.0.1:%d/", s.port)).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (!s.user.empty() && ImGui::Button((std::string(ICON_COPY) + "  계정 복사").c_str()))
		{
			ImGui::SetClipboardText((s.user + " / " + s.password).c_str());
			PushToast("복사함", "Jenkins 계정과 비밀번호를 클립보드에 복사했습니다.", col::Info);
		}
		ImGui::SameLine();
		if (ImGui::Button((std::string(ICON_DOWNLOAD) + "  번들로 업데이트...").c_str())) pickBundle("ci-install");
		Tooltip("새 번들로 Jenkins·Java·플러그인을 바꿉니다. 작업과 빌드 기록은 유지됩니다.");
		ImGui::SameLine();
		if (DangerButton((std::string(ICON_DELETE) + "  제거").c_str())) ImGui::OpenPopup("remove-jenkins");
	}
	ImGui::EndDisabled();
	if (ImGui::BeginPopup("remove-jenkins"))
	{
		TextBold("Jenkins 제거");
		ImGui::PushTextWrapPos(S(380));
		TextDim("Jenkins와 모든 프로젝트의 DwoBuilder 작업·git 훅을 지웁니다. 빌드 기록은 %LOCALAPPDATA%\\dwo-Forge\\uninstalled로 옮겨 둡니다.");
		ImGui::PopTextWrapPos();
		if (DangerButton("제거")) { app.Run("ci-remove --keep-data"); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("취소")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (NowSeconds() - u.projectsAt > 3)
	{
		u.projectsAt = NowSeconds();
		auto pj = ReadJson(CiHomeDir() / "projects.json");
		u.projects = pj ? *pj : json::array();
		if (u.projects.is_object()) u.projects = json::array({ u.projects });
	}
	SectionHeader("이 Jenkins를 쓰는 프로젝트");
	if (!u.projects.is_array() || u.projects.empty()) TextDim("없음. 프로젝트의 도구 목록에서 DwoBuilder를 설치하면 여기에 나타납니다.");
	else if (ImGui::BeginTable("cijobs", 2, kTable))
	{
		ImGui::TableSetupColumn("작업", ImGuiTableColumnFlags_WidthFixed, S(260));
		ImGui::TableSetupColumn("프로젝트", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		for (auto& p : u.projects)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Str(p, "job").c_str());
			ImGui::TableNextColumn();
			std::string path = Str(p, "project");
			if (ImGui::Selectable((path + "##" + Str(p, "job")).c_str())) { app.SetProject(fs::u8path(path)); u.page = launcher::Project; }
		}
		ImGui::EndTable();
	}
}

static void DrawEnginesPanel(App& app)
{
	json doc = g_tools.Get(app.project);
	const json& pj = Obj(doc, "project");
	ImGui::PushTextWrapPos(0);
	TextDim("이 PC에 설치된 언리얼 엔진입니다. 플러그인 도구는 프로젝트가 쓰는 엔진에 맞춰 빌드됩니다.");
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	auto engines = Arr(Obj(doc, "pc"), "engines");
	if (engines.empty()) { TextDim(g_tools.busy ? "확인 중..." : "프로젝트를 열면 엔진 목록을 읽습니다."); return; }
	if (ImGui::BeginTable("engines", 3, kTable))
	{
		ImGui::TableSetupColumn("버전", ImGuiTableColumnFlags_WidthFixed, S(140));
		ImGui::TableSetupColumn("연결 이름", ImGuiTableColumnFlags_WidthFixed, S(200));
		ImGui::TableSetupColumn("위치", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		for (auto& e : engines)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(("UE " + Str(e, "version")).c_str());
			if (Str(e, "version") == Str(pj, "engine")) { ImGui::SameLine(); Pill("이 프로젝트", col::Accent); }
			ImGui::TableNextColumn(); TextDim(Str(e, "association"));
			ImGui::TableNextColumn(); TextDim(Str(e, "root"));
		}
		ImGui::EndTable();
	}
}

static void DrawPcPage(App& app)
{
	launcher::Ui& u = launcher::State();
	ImGui::Dummy(ImVec2(0, S(2)));
	if (UnderlineTab("Jenkins (DwoBuilder)", u.pcTab == 0)) u.pcTab = 0;
	ImGui::SameLine(0, S(4));
	if (UnderlineTab("언리얼 엔진", u.pcTab == 1)) u.pcTab = 1;
	CardRule();
	ImGui::Dummy(ImVec2(0, S(8)));
	ImGui::BeginChild("pc", ImVec2(0, 0), ImGuiChildFlags_None);
	if (u.pcTab == 0) DrawJenkinsPanel(app); else DrawEnginesPanel(app);
	ImGui::EndChild();
}

static void DrawAboutPage(App& app)
{
	json doc = g_tools.Get(app.project);
	ImGui::Dummy(ImVec2(0, S(2)));
	UnderlineTab("dwo-Forge", true);
	CardRule();
	ImGui::Dummy(ImVec2(0, S(8)));
	ImGui::BeginChild("about", ImVec2(0, 0), ImGuiChildFlags_None);
	ImGui::PushTextWrapPos(0);
	TextDim("언리얼 프로젝트에 도구를 설치·업데이트하고 실행하는 런처입니다.");
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	std::string version = Str(Obj(doc, "forge"), "commit", app.SystemCommit());
	KeyValue("버전", version.empty() ? "-" : version, 70);
	KeyValue("위치", PathUtf8(app.home), 70);
	ImGui::Spacing();
	if (ImGui::Button("가이드 문서")) OpenWithShell(app.home / "docs" / "guide.html");
	ImGui::SameLine();
	if (ImGui::Button("README")) OpenWithShell(app.home / "README.md");
	ImGui::SameLine();
	if (ImGui::Button("설치 폴더 열기")) OpenWithShell(app.home);
	SectionHeader("도구 카탈로그", "tools/catalog.json");
	DrawCatalogTable(app);
	ImGui::EndChild();
}

// The whole launcher: one card in the space next to the sidebar (the run log drawer is drawn by the caller).
static void DrawLauncherContent(App& app, float height)
{
	ConsumeLauncherRequest();
	ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(S(16), S(14)));
	ImGui::BeginGroup();
	float w = ImGui::GetContentRegionAvail().x - S(16);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(6)));
	ImGui::BeginChild("launcher-frame", ImVec2(w, height - S(14)), ImGuiChildFlags_None);
	BeginCard("card");
	switch (launcher::State().page)
	{
	case launcher::Project: DrawProjectPage(app); break;
	case launcher::Pc: DrawPcPage(app); break;
	case launcher::About: DrawAboutPage(app); break;
	}
	EndCard();
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::EndGroup();
}
