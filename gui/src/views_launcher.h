// The launcher: dwo-Forge's home. Left: projects and PC components; middle: the selected project's tool library
// (install, update, remove, open); right: details. Tool versions come from 'forge.ps1 tools --json'.
#pragma once

#include "views_ci.h"

// ============================================================================ tool identity

struct ToolMeta
{
	const char* id;        // forge.ps1 tool id
	const char* name;
	const char* mono;      // two-letter tile shown in the activity bar and the library
	ImU32 color;
	const char* summary;
	Activity open;         // first screen of the tool
	const char* folder;    // what it puts into the project
};

static const ToolMeta kToolMeta[] = {
	{ "lens", "DwoLens", "Ln", col::Accent, "아키텍처·문제·검증·로그·성능·용량 검사와 실행 기록", Activity::Overview, "Plugins/__DwoLensInspector (편집기 전용, 기본 꺼짐)" },
	{ "statemonitor", "DwoStateMonitor", "SM", col::Pass, "실행 중인 게임의 상태를 실시간으로 보는 플러그인", Activity::Live, "Plugins/__DwoStateMonitor" },
	{ "canvas", "DwoCanvas", "Cv", col::Purple, "디자인 파일(*.dwocanvas.json)을 UMG 위젯 블루프린트로 가져오기", Activity::Canvas, "Plugins/__DwoCanvas" },
	{ "ci", "로컬 CI", "CI", col::Teal, "push·merge·커밋 때 컴파일 → DwoLens 검사 → 패키지 빌드를 자동 실행", Activity::Ci, "Jenkins 작업, .git/hooks (Jenkins는 PC에 하나)" },
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
enum Page { Project, Jenkins, Engines, About };
struct Ui
{
	Page page = Project;
	std::string tool;              // selected row (inspector)
	bool connectWith[kToolCount] = { true, false, false, false };
	std::string confirmRemove;     // tool id waiting for confirmation
	json projects;                 // CI projects.json (PC page), re-read every few seconds
	double projectsAt = -100;
};
static Ui& State()
{
	static Ui u;
	return u;
}
} // namespace launcher

static fs::path CiHomeDir()
{
	const wchar_t* ciHome = _wgetenv(L"DWO_FORGE_CI_HOME");
	if (ciHome && *ciHome) return fs::path(ciHome);
	return fs::path(_wgetenv(L"LOCALAPPDATA") ? _wgetenv(L"LOCALAPPDATA") : L".") / "dwo-Forge" / "ci";
}

// ============================================================================ navigator

static void DrawLauncherNav(App& app)
{
	launcher::Ui& u = launcher::State();
	if (g_launcherPageRequest >= 0) { u.page = (launcher::Page)g_launcherPageRequest; g_launcherPageRequest = -1; }
	std::vector<ToolView> views = ToolViews(app);
	SectionHeader("프로젝트");
	bool listed = false;
	for (auto& r : app.recent)
	{
		fs::path p = fs::u8path(r);
		std::error_code ec;
		if (!fs::exists(p, ec)) continue;
		listed = true;
		bool current = p == app.project;
		ImGui::PushID(r.c_str());
		std::string label = PathUtf8(p.filename());
		if (ImGui::Selectable(label.c_str(), current && u.page == launcher::Project, ImGuiSelectableFlags_AllowOverlap))
		{
			if (!current) app.SetProject(p);
			u.page = launcher::Project;
			u.tool.clear();
		}
		Tooltip(r.c_str());
		ImGui::SameLine();
		if (current && app.attached && UpdateCount(views) > 0)
		{
			ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - S(64));
			Pill(Fmt("업데이트 %d", UpdateCount(views)), col::Warn);
		}
		else TextDim(PathUtf8(p.parent_path().filename()));
		ImGui::PopID();
	}
	if (!listed) TextDim("열어 본 프로젝트가 없습니다.");
	if (ImGui::Selectable((std::string(ICON_ADD) + "  프로젝트 열기...").c_str()))
	{
		fs::path p;
		if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) { app.SetProject(p); u.page = launcher::Project; }
	}
	Tooltip("언리얼 프로젝트 폴더(.uproject가 있는 폴더)를 고릅니다. 창으로 끌어다 놓아도 됩니다.");

	SectionHeader("PC 구성요소");
	ci::Client& c = ci::Instance();
	c.Ensure(app.hasUProject ? app.project : fs::path());
	ci::Snapshot s = c.Get();
	if (ImGui::Selectable((std::string(ICON_SYNC) + "  Jenkins (로컬 CI)").c_str(), u.page == launcher::Jenkins, ImGuiSelectableFlags_AllowOverlap)) u.page = launcher::Jenkins;
	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - S(56));
	TextDim(!s.installed ? "없음" : s.running ? "실행 중" : "중지됨");
	json doc = g_tools.Get(app.project);
	int engines = (int)Arr(Obj(doc, "pc"), "engines").size();
	if (ImGui::Selectable((std::string(ICON_GAME) + "  언리얼 엔진").c_str(), u.page == launcher::Engines, ImGuiSelectableFlags_AllowOverlap)) u.page = launcher::Engines;
	if (engines) { ImGui::SameLine(); ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - S(56)); TextDim(Fmt("%d개", engines)); }

	SectionHeader("dwo-Forge");
	if (ImGui::Selectable((std::string(ICON_INFO) + "  정보와 도구 카탈로그").c_str(), u.page == launcher::About)) u.page = launcher::About;
}

// ============================================================================ project page

static const char* StateLabel(const ToolView& v)
{
	if (v.state == "installed") return "최신";
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

static std::string VersionLine(App& app, const ToolView& v)
{
	if (!v.known) return v.state == "none" ? std::string() : "버전 확인 중...";
	if (v.state == "installed") return "dwo-Forge " + (v.installedVersion.empty() ? v.availableVersion : v.installedVersion) + (v.installedEngine.empty() ? "" : " · UE " + v.installedEngine);
	if (v.state == "update")
	{
		if (v.reason == "engine-changed") return "엔진이 바뀜: UE " + v.installedEngine + " 빌드 → 다시 설치 필요";
		if (v.reason == "unknown-version") return "설치 버전 기록 없음 (이전 dwo-Forge) → " + v.availableVersion;
		return v.installedVersion + "  →  " + v.availableVersion;
	}
	if (v.state == "legacy") return "dwo-Lens 1.x의 Plugins/__StateMonitor → 업데이트하면 DwoStateMonitor로 옮김";
	if (v.state == "own") return "프로젝트의 자체 플러그인 · dwo-Forge가 관리하지 않음";
	return "사용 가능 " + v.availableVersion;
}

static std::string ExtraLine(App& app, const ToolView& v)
{
	if (v.id == "lens" && ToolPresent(v))
	{
		int done = 0, failed = 0;
		for (int i = 0; i < kLineCount; ++i)
			if (const RunInfo* r = app.Latest(i)) { ++done; failed += r->verdict == "fail"; }
		return Fmt("검사 %d종 중 %d종 실행 · 실패 %d", kLineCount, done, failed);
	}
	if (v.id == "statemonitor" && v.detail == "runtime") return "패키지 게임에도 포함";
	if (v.id == "statemonitor" && v.detail == "editor") return "에디터·PIE 전용";
	if (v.id == "canvas" && !app.canvasVersion.empty()) return "플러그인 " + app.canvasVersion;
	if (v.id == "ci" && !v.detail.empty())
	{
		ci::Snapshot s = ci::Instance().Get();
		std::string last = "빌드 없음";
		if (!s.builds.empty())
		{
			std::string verdict = ci::Verdict(s.builds.front());
			const char* label = verdict == "running" ? "진행 중" : VerdictLabel(verdict);
			last = Fmt("최근 빌드 #%d ", s.builds.front().number) + (label ? label : verdict);
		}
		return "작업 " + v.detail + " · " + last;
	}
	return "";
}

static void DrawToolRow(App& app, const ToolMeta& m, const ToolView& v, const json& doc)
{
	launcher::Ui& u = launcher::State();
	bool idle = !app.runner.running;
	bool selected = u.tool == m.id;
	float h = S(86);
	ImGui::PushID(m.id);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? col::Panel2 : col::Panel);
	ImGui::PushStyleColor(ImGuiCol_Border, v.state == "update" || v.state == "legacy" ? WithAlpha(col::Warn, 0.6f) : col::Border);
	ImGui::BeginChild("row", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 origin = ImGui::GetCursorScreenPos();
	float tile = S(40);
	DrawToolTile(dl, origin + ImVec2(0, S(4)), tile, m, v.state == "none" ? 0.45f : 1.0f);
	float textX = ImGui::GetCursorPosX() + tile + S(14);
	ImGui::SetCursorPosX(textX);
	ImGui::PushFont(g_fonts.title);
	ImGui::TextUnformatted(m.name);
	ImGui::PopFont();
	ImGui::SameLine();
	Pill(StateLabel(v), StateColor(v));
	std::string ver = VersionLine(app, v);
	ImGui::SetCursorPosX(textX);
	TextDim(m.summary);
	ImGui::SetCursorPosX(textX);
	std::string extra = ExtraLine(app, v);
	if (v.state == "update" || v.state == "legacy") TextColored(col::Warn, ver.c_str());
	else TextDim(ver + (extra.empty() ? "" : (ver.empty() ? "" : "   ·   ") + extra));

	// buttons, right-aligned on the first line
	std::vector<std::pair<std::string, int>> buttons;   // label, kind (0 primary, 1 normal, 2 remove)
	bool present = ToolPresent(v);
	bool managed = v.state == "installed" || v.state == "update" || v.state == "legacy";
	if (v.state == "none") buttons.push_back({ "설치", 0 });
	if (v.state == "update" || v.state == "legacy") buttons.push_back({ v.state == "legacy" ? "옮기기" : "업데이트", 0 });
	if (present) buttons.push_back({ "열기", v.state == "installed" || v.state == "own" ? 0 : 1 });
	if (m.id == std::string("statemonitor") && v.state == "none") buttons.push_back({ "패키지 포함 설치", 1 });
	if (managed) buttons.push_back({ "제거", 2 });
	float x = ImGui::GetWindowContentRegionMax().x;
	for (auto it = buttons.rbegin(); it != buttons.rend(); ++it)
		x -= ImGui::CalcTextSize(it->first.c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
	ImGui::SetCursorPos(ImVec2(x, S(8)));
	json pc = Obj(doc, "pc");
	bool ciBlocked = m.id == std::string("ci") && v.state == "none" && v.known &&
	                 !Obj(pc, "jenkins").value("installed", false) && Str(Obj(pc, "bundle"), "file").empty();
	for (size_t i = 0; i < buttons.size(); ++i)
	{
		auto& [label, kind] = buttons[i];
		if (i) ImGui::SameLine();
		bool mutates = label != "열기";
		ImGui::BeginDisabled(mutates && (!idle || !app.attached || (label == "설치" && (ciBlocked || !v.available))));
		bool clicked = kind == 0 ? PrimaryButton(label.c_str()) : ImGui::Button(label.c_str());
		ImGui::EndDisabled();
		if (label == "설치" && ciBlocked) Tooltip("로컬 CI는 PC에 Jenkins가 필요합니다. 인터넷 PC에서 dwo-Forge.bat ci-bundle로 만든 번들을 dwo-Forge의 bundles 폴더에 두세요.");
		else if (label == "패키지 포함 설치") Tooltip("패키지 게임에서도 동작합니다. .uproject(BP 전용) 또는 Build.cs(C++)에 DwoStateMonitor를 등록해야 합니다.");
		else if (label == "설치" && m.id == std::string("ci")) Tooltip("Jenkins 작업과 git 훅을 만듭니다. PC에 Jenkins가 없으면 bundles 폴더의 오프라인 번들로 먼저 설치합니다.");
		if (!clicked) continue;
		std::string id = m.id;
		if (label == "설치") app.Run("tool install " + id);
		else if (label == "패키지 포함 설치") app.Run("tool install statemonitor --runtime");
		else if (label == "업데이트" || label == "옮기기") app.Run("tool update " + id);
		else if (label == "열기") app.requestActivity = m.open;
		else if (label == "제거") { u.confirmRemove = id; ImGui::OpenPopup("confirm-remove"); }
	}
	if (ImGui::BeginPopup("confirm-remove"))
	{
		TextBold(std::string(m.name) + " 제거");
		ImGui::PushTextWrapPos(S(360));
		if (m.id == std::string("ci")) TextDim("이 프로젝트의 Jenkins 작업(빌드 기록 포함)과 git 훅을 지웁니다. PC의 Jenkins와 다른 프로젝트는 그대로입니다.");
		else if (m.id == std::string("lens")) TextDim("Plugins/__DwoLensInspector를 지웁니다. 실행 기록은 남고, 다시 설치하면 이어서 씁니다.");
		else TextDim(std::string(m.folder) + "를 지웁니다. 프로젝트 코드나 에셋이 아직 쓰고 있으면 제거를 멈추고 목록을 보여 줍니다.");
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		if (DangerButton("제거")) { app.Run("tool remove " + u.confirmRemove); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("취소")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	// clicking the row (outside the buttons) selects it for the inspector
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) u.tool = m.id;
	if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(0) && present && !ImGui::IsAnyItemHovered()) app.requestActivity = m.open;
	ImGui::EndChild();
	ImGui::PopStyleColor(2);
	ImGui::PopID();
}

static void DrawConnectPanel(App& app)
{
	launcher::Ui& u = launcher::State();
	ImGui::BeginChild("connect", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
	ImGui::PushFont(g_fonts.title);
	ImGui::TextUnformatted("이 프로젝트는 아직 dwo-Forge에 연결되지 않았습니다");
	ImGui::PopFont();
	ImGui::PushTextWrapPos(0);
	TextDim("연결하면 프로젝트에 __dwo-forge 폴더와 실행 파일 __dwo-forge.bat이 생기고, git 프로젝트에서는 변경 목록에 나타나지 않게 숨깁니다. "
	        ".uproject와 기존 파일은 건드리지 않습니다. 연결 해제하면 흔적 없이 지워집니다.");
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	TextBold("함께 설치할 도구");
	for (int i = 0; i < kToolCount; ++i)
	{
		ImGui::PushID(i);
		ImGui::Checkbox("##with", &u.connectWith[i]);
		ImGui::SameLine();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float t = ImGui::GetFrameHeight() * 0.8f;
		DrawToolTile(ImGui::GetWindowDrawList(), p + ImVec2(0, (ImGui::GetFrameHeight() - t) * 0.5f), t, kToolMeta[i]);
		ImGui::Dummy(ImVec2(t, ImGui::GetFrameHeight()));
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(kToolMeta[i].name);
		ImGui::SameLine();
		TextDim(kToolMeta[i].summary);
		ImGui::PopID();
	}
	ImGui::Spacing();
	ImGui::BeginDisabled(app.runner.running || !app.HomeValid());
	if (PrimaryButton((std::string(ICON_LINK) + "  연결").c_str()))
	{
		app.Run("connect");
		for (int i = 0; i < kToolCount; ++i)
			if (u.connectWith[i]) app.Run(std::string("tool install ") + kToolMeta[i].id);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	TextDim("도구는 나중에 이 화면에서 언제든 설치·제거할 수 있습니다.");
	ImGui::EndChild();
}

static void DrawProjectPage(App& app)
{
	launcher::Ui& u = launcher::State();
	if (!app.hasUProject)
	{
		if (EmptyState(ICON_FOLDER, "프로젝트를 여세요", "언리얼 프로젝트 폴더(.uproject가 있는 폴더)를 고르거나 이 창으로 끌어다 놓으세요. 연결하고 쓸 도구를 고르면 됩니다.", "프로젝트 열기..."))
		{
			fs::path p;
			if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p);
		}
		return;
	}
	json doc = g_tools.Get(app.project);
	const json& pj = Obj(doc, "project");
	std::vector<ToolView> views = ToolViews(app);

	// ---- header
	ImGui::PushFont(g_fonts.big);
	ImGui::TextUnformatted(app.projectName.c_str());
	ImGui::PopFont();
	std::string engine = Str(pj, "engine", app.attached ? app.Engine() : "");
	std::string sub = std::string(app.isCodeProject ? "C++" : "Blueprint 전용") + (engine.empty() ? "" : "  ·  UE " + engine) +
	                  (app.attached ? "  ·  " + std::string(Str(app.state, "vcs") == "none" ? "버전 관리 없음" : "git") : "") + "  ·  " + PathUtf8(app.project);
	TextDim(sub);
	{
		float right = ImGui::GetWindowContentRegionMax().x;
		float bw = S(110);
		ImGui::SameLine();
		ImGui::SetCursorPosX(right - (app.attached ? bw * 2 + S(8) : bw));
		if (ImGui::Button((std::string(ICON_FOLDER) + "  폴더 열기").c_str(), ImVec2(bw, 0))) OpenWithShell(app.project);
		if (app.attached)
		{
			ImGui::SameLine();
			ImGui::BeginDisabled(app.runner.running);
			if (ImGui::Button((std::string(ICON_CLOSE) + "  연결 해제...").c_str(), ImVec2(bw, 0))) app.confirmDetach = true;
			ImGui::EndDisabled();
			Tooltip("모든 도구를 지우고 프로젝트를 dwo-Forge에서 분리합니다. 무엇을 지우는지 먼저 보여 줍니다.");
		}
	}
	ImGui::Spacing();

	if (!app.attached && app.legacyAttached)
	{
		EmptyState(ICON_SYNC, "dwo-Lens 1.x로 연동된 프로젝트입니다",
		           "__dwo-lens 폴더와 실행 파일, git exclude 블록을 dwo-Forge 이름으로 옮기고 있습니다. 실행 기록과 설정은 그대로 유지됩니다.");
		return;
	}
	if (!app.attached) { DrawConnectPanel(app); return; }

	// ---- update / engine banners
	int updates = UpdateCount(views);
	std::string connectedEngine = Str(pj, "connectedEngine");
	if (!engine.empty() && !connectedEngine.empty() && engine != connectedEngine)
	{
		Pill("엔진 변경", col::Warn);
		ImGui::SameLine();
		ImGui::TextUnformatted(("연결할 때 UE " + connectedEngine + ", 지금 UE " + engine + ". 연결을 새로 고치면 도구도 새 엔진으로 다시 빌드합니다.").c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(app.runner.running);
		if (PrimaryButton("연결 새로 고침")) app.Run("update");
		ImGui::EndDisabled();
	}
	else if (updates > 0)
	{
		Pill(Fmt("업데이트 %d", updates), col::Warn);
		ImGui::SameLine();
		ImGui::TextUnformatted(Fmt("이 dwo-Forge(%s)에 더 새 버전이 있는 도구가 %d개 있습니다.", Str(Obj(doc, "forge"), "version").c_str(), updates).c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(app.runner.running);
		if (PrimaryButton((std::string(ICON_SYNC) + "  모두 업데이트").c_str())) app.Run("tool update --all");
		ImGui::EndDisabled();
	}

	// ---- library
	std::string forgeVersion = Str(Obj(doc, "forge"), "version");
	SectionHeader("도구", forgeVersion.empty() ? (g_tools.busy ? "확인 중..." : nullptr) : ("dwo-Forge " + forgeVersion).c_str());
	for (int i = 0; i < kToolCount; ++i)
	{
		DrawToolRow(app, kToolMeta[i], views[i], doc);
		ImGui::Dummy(ImVec2(0, S(2)));
	}

	SectionHeader("프로젝트 관리");
	ImGui::BeginDisabled(app.runner.running);
	if (ImGui::Button((std::string(ICON_SETTINGS) + "  환경 점검").c_str())) app.Run("doctor");
	ImGui::SameLine();
	if (ImGui::Button((std::string(ICON_INFO) + "  상태 확인").c_str())) app.Run("status");
	ImGui::SameLine();
	if (ImGui::Button((std::string(ICON_SYNC) + "  연결 새로 고침").c_str())) app.Run("update");
	Tooltip("실행 파일과 git 숨김 설정을 이 dwo-Forge 버전으로 맞추고, 업데이트가 있는 도구를 모두 업데이트합니다.");
	ImGui::EndDisabled();
	if (!g_tools.error.empty()) { ImGui::Spacing(); TextColored(col::Fail, ("도구 상태를 읽지 못했습니다: " + g_tools.error.substr(0, 200)).c_str()); }
}

// ============================================================================ PC pages

static void DrawJenkinsPage(App& app)
{
	launcher::Ui& u = launcher::State();
	ci::Client& c = ci::Instance();
	ci::Snapshot s = c.Get();
	json doc = g_tools.Get(app.project);
	const json& pc = Obj(doc, "pc");
	ViewHeader(ICON_SYNC, "Jenkins (로컬 CI)", "PC에 하나만 설치되고, 프로젝트마다 로컬 CI 도구가 작업을 하나씩 만듭니다.");
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
		if (!bundle.empty() && PrimaryButton((std::string(ICON_DOWNLOAD) + "  설치").c_str())) app.Run("ci-install");
		if (!bundle.empty()) { Tooltip(("bundles 폴더의 " + bundle + "로 설치합니다. 네트워크에 접속하지 않습니다.").c_str()); ImGui::SameLine(); }
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
		TextDim("Jenkins와 모든 프로젝트의 로컬 CI 작업·git 훅을 지웁니다. 빌드 기록은 %LOCALAPPDATA%\\dwo-Forge\\uninstalled로 옮겨 둡니다.");
		ImGui::PopTextWrapPos();
		if (DangerButton("제거")) { app.Run("ci-remove --keep-data"); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("취소")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// projects that use it
	if (NowSeconds() - u.projectsAt > 3)
	{
		u.projectsAt = NowSeconds();
		auto pj = ReadJson(CiHomeDir() / "projects.json");
		u.projects = pj ? *pj : json::array();
		if (u.projects.is_object()) u.projects = json::array({ u.projects });
	}
	SectionHeader("이 Jenkins를 쓰는 프로젝트");
	if (!u.projects.is_array() || u.projects.empty()) TextDim("없음. 프로젝트의 도구 목록에서 로컬 CI를 설치하면 여기에 나타납니다.");
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

static void DrawEnginesPage(App& app)
{
	json doc = g_tools.Get(app.project);
	const json& pj = Obj(doc, "project");
	ViewHeader(ICON_GAME, "언리얼 엔진", "이 PC에 설치된 엔진. 도구 플러그인은 프로젝트가 쓰는 엔진에 맞춰 빌드됩니다.");
	auto engines = Arr(Obj(doc, "pc"), "engines");
	if (engines.empty()) { TextDim(g_tools.busy ? "확인 중..." : "프로젝트를 열면 엔진 목록을 읽습니다."); return; }
	if (ImGui::BeginTable("engines", 3, kTable))
	{
		ImGui::TableSetupColumn("버전", ImGuiTableColumnFlags_WidthFixed, S(110));
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

static void DrawAboutPage(App& app)
{
	json doc = g_tools.Get(app.project);
	ViewHeader(ICON_FORGE, "dwo-Forge", "언리얼 프로젝트에 도구를 설치·업데이트하고 실행하는 런처");
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
}

static void DrawLauncherMain(App& app)
{
	switch (launcher::State().page)
	{
	case launcher::Project: DrawProjectPage(app); break;
	case launcher::Jenkins: DrawJenkinsPage(app); break;
	case launcher::Engines: DrawEnginesPage(app); break;
	case launcher::About: DrawAboutPage(app); break;
	}
}

// ============================================================================ inspector

static void DrawLauncherInspector(App& app)
{
	launcher::Ui& u = launcher::State();
	if (u.page == launcher::Jenkins)
	{
		SectionHeader("로컬 CI 구조");
		ImGui::PushTextWrapPos(0);
		TextDim("Jenkins(Java 포함)는 PC에 하나만 설치되며 127.0.0.1에서만 열립니다. 프로젝트에 로컬 CI 도구를 설치하면 그 프로젝트의 Jenkins 작업과 git 훅이 생기고, "
		        "push·merge·브랜치 생성·커밋 때 빌드가 자동으로 시작됩니다.");
		ImGui::Spacing();
		TextDim("폐쇄망 PC: 인터넷 PC에서 dwo-Forge.bat ci-bundle로 만든 zip을 dwo-Forge의 bundles 폴더에 두면 네트워크 없이 설치됩니다.");
		ImGui::PopTextWrapPos();
		return;
	}
	if (u.page != launcher::Project || !app.hasUProject) return;
	std::vector<ToolView> views = ToolViews(app);
	for (int i = 0; i < kToolCount; ++i)
	{
		if (u.tool != kToolMeta[i].id) continue;
		const ToolMeta& m = kToolMeta[i];
		const ToolView& v = views[i];
		ImVec2 p = ImGui::GetCursorScreenPos();
		DrawToolTile(ImGui::GetWindowDrawList(), p, S(28), m);
		ImGui::Dummy(ImVec2(S(28), S(28)));
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		Title(m.name);
		ImGui::PushTextWrapPos(0);
		TextDim(m.summary);
		ImGui::PopTextWrapPos();
		SectionHeader("설치");
		KeyValue("상태", StateLabel(v), 80);
		if (!v.installedVersion.empty()) KeyValue("설치 버전", v.installedVersion, 80);
		if (!v.availableVersion.empty()) KeyValue("최신 버전", v.availableVersion, 80);
		if (!v.installedEngine.empty()) KeyValue("빌드 엔진", "UE " + v.installedEngine, 80);
		if (!v.installedAt.empty()) KeyValue("설치 일시", ShortDate(v.installedAt), 80);
		KeyValue("범위", m.id == std::string("ci") ? "프로젝트 + PC" : "프로젝트", 80);
		KeyValue("설치 위치", m.folder, 80);
		SectionHeader("명령줄");
		std::string id = m.id;
		for (const char* verb : { "install", "update", "remove" })
		{
			std::string line = "__dwo-forge.bat tool " + std::string(verb) + " " + id;
			if (ImGui::Selectable((std::string(ICON_COPY) + "  " + line).c_str())) { ImGui::SetClipboardText(line.c_str()); PushToast("복사함", line, col::Info); }
		}
		if (ImGui::Selectable((std::string(ICON_COPY) + "  __dwo-forge.bat tools").c_str())) ImGui::SetClipboardText("__dwo-forge.bat tools");
		return;
	}
	SectionHeader("프로젝트");
	KeyValue("이름", app.projectName, 80);
	KeyValue("종류", app.isCodeProject ? "C++" : "Blueprint 전용", 80);
	if (app.attached)
	{
		KeyValue("엔진", "UE " + app.Engine(), 80);
		KeyValue("연결 일시", ShortDate(Str(app.state, "attachedAt")), 80);
		KeyValue("연결 버전", app.SystemCommit(), 80);
		KeyValue("버전 관리", Str(app.state, "vcs") == "none" ? "없음" : Str(app.state, "vcs") == "parent" ? "상위 git 저장소" : "git", 80);
	}
	KeyValue("경로", PathUtf8(app.project), 80);
	ImGui::Spacing();
	ImGui::PushTextWrapPos(0);
	TextDim("도구 줄을 누르면 버전과 설치 위치를, 두 번 누르면 그 도구를 엽니다.");
	ImGui::PopTextWrapPos();
}
