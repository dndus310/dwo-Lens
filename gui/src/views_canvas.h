// DwoCanvas: design file (*.dwocanvas.json) -> UMG Widget Blueprint importer.
// Install state, design file picker (open dialog or drag and drop), validate / import / import with reset
// (output in the bottom run log), and the latest report from <Project>/Saved/DwoCanvas/Reports.
#pragma once

#include "views_live.h"

// "Shop_20261111_101500.txt" -> "2026-11-11 10:15:00" (the importer stamps every report name).
static std::string CanvasStamp(const std::string& name)
{
	static const std::regex stamp(R"(_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})\.txt$)");
	std::smatch m;
	if (!std::regex_search(name, m, stamp)) return {};
	return m[1].str() + "-" + m[2].str() + "-" + m[3].str() + " " + m[4].str() + ":" + m[5].str() + ":" + m[6].str();
}

// Document name part of a report file ("Shop.dwocanvas_20261111_101500.txt" -> "Shop.dwocanvas").
static std::string CanvasReportTitle(const std::string& name)
{
	static const std::regex stamp(R"(_\d{8}_\d{6}\.txt$)");
	return std::regex_replace(name, stamp, "");
}

static bool IsDesignFile(const fs::path& p)
{
	std::string n = Lower(PathUtf8(p.filename()));
	return n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0;
}

static std::string Quoted(const fs::path& p) { return "\"" + PathUtf8(p) + "\""; }

static void PickDesignFile(App& app)
{
	fs::path p;
	if (PickOpen(g_hwnd, false, L"디자인 파일 열기 (*.dwocanvas.json)", p, L"DwoCanvas 디자인 (*.dwocanvas.json, *.json)", L"*.dwocanvas.json;*.json"))
		app.SetCanvasFile(p);
}

static void RunCanvas(App& app, const std::string& verb, bool reset = false)
{
	if (app.canvasFile.empty()) return;
	std::string cmd = verb + " " + Quoted(app.canvasFile);
	if (reset) cmd += " --reset";
	if (verb == "canvas-import" && ui.canvasDoc[0]) cmd += std::string(" --doc \"") + ui.canvasDoc + "\"";
	if (verb == "canvas-import") ui.canvasReport = 0;   // show the new report when it arrives
	app.showConsole = true;
	app.Run(cmd);
}

static const char* CanvasStateText(const App& app)
{
	return app.canvas == 1 ? "설치됨 (dwo-Forge)" : app.canvas == 2 ? "프로젝트에 포함" : "설치 안 됨";
}

// ---------------------------------------------------------------------------- nav

static void DrawCanvasNav(App& app)
{
	SectionHeader("플러그인");
	ImGui::BeginChild("canvasstate", ImVec2(0, S(118)), ImGuiChildFlags_Borders);
	TextBold(std::string(ICON_CANVAS) + "  DwoCanvas");
	ImGui::SameLine();
	Pill(app.canvas == 0 ? "설치 안 됨" : app.canvas == 1 ? "설치됨" : "프로젝트 소유", app.canvas == 0 ? col::Muted : app.canvas == 1 ? col::Pass : col::Info);
	TextDim(app.canvas ? "Plugins/DwoCanvas" + (app.canvasVersion.empty() ? std::string() : "  ·  " + app.canvasVersion) : "Plugins/DwoCanvas에 설치합니다");
	ImGui::Spacing();
	bool idle = !app.runner.running;
	if (app.canvas == 0)
	{
		ImGui::BeginDisabled(!idle || !app.attached);
		if (PrimaryButton((std::string(ICON_DOWNLOAD) + "  설치").c_str(), ImVec2(-FLT_MIN, 0))) app.Run("canvas-install");
		ImGui::EndDisabled();
		Tooltip(app.attached ? "미리 빌드한 에디터 바이너리와 소스를 Plugins/DwoCanvas에 복사합니다. 숨기지 않으며 git에 커밋하는 플러그인입니다."
		                     : "먼저 프로젝트에 부착하세요.");
	}
	else
	{
		ImGui::BeginDisabled(!idle || app.canvas != 1);
		if (ImGui::Button((std::string(ICON_DELETE) + "  제거").c_str(), ImVec2(-FLT_MIN, 0))) app.Run("canvas-remove");
		ImGui::EndDisabled();
		Tooltip(app.canvas == 1 ? "생성된 위젯이 DwoCanvasRuntime을 쓰고 있으면 제거를 거부합니다 (명령줄 --force로만 강제)."
		                        : "프로젝트가 직접 가진 DwoCanvas는 dwo-Forge가 제거하지 않습니다.");
	}
	ImGui::EndChild();

	SectionHeader("디자인 파일");
	if (ImGui::Button((std::string(ICON_OPEN) + "  열기...").c_str(), ImVec2(-FLT_MIN, 0))) PickDesignFile(app);
	std::error_code ec;
	for (size_t i = 0; i < app.canvasRecent.size(); ++i)
	{
		fs::path p = fs::u8path(app.canvasRecent[i]);
		bool exists = fs::exists(p, ec);
		ImGui::PushID((int)i);
		ImGui::PushStyleColor(ImGuiCol_Text, exists ? col::Text : col::Muted);
		if (ImGui::Selectable((std::string(ICON_PAGE) + "  " + PathUtf8(p.filename())).c_str(), app.canvasFile == p) && exists) app.SetCanvasFile(p);
		ImGui::PopStyleColor();
		Tooltip(app.canvasRecent[i].c_str());
		ImGui::PopID();
	}
	fs::path samples = app.home / "plugin" / "DwoCanvas" / "Samples";
	if (fs::is_directory(samples, ec))
	{
		SectionHeader("예제");
		for (auto& e : fs::directory_iterator(samples, ec))
		{
			if (!IsDesignFile(e.path())) continue;
			ImGui::PushID(PathUtf8(e.path()).c_str());
			if (ImGui::Selectable((std::string(ICON_LIBRARY) + "  " + PathUtf8(e.path().filename())).c_str(), app.canvasFile == e.path())) app.SetCanvasFile(e.path());
			ImGui::PopID();
		}
	}

	SectionHeader("리포트", "Saved/DwoCanvas/Reports");
	if (app.canvasReports.empty()) TextDim("아직 가져오기 리포트가 없습니다.");
	for (size_t i = 0; i < app.canvasReports.size() && i < 40; ++i)
	{
		CanvasReport* r = app.CanvasReportAt(i);
		ImGui::PushID((int)i);
		StatusDot(r->errors ? col::Fail : r->warnings ? col::Warn : col::Pass);
		ImGui::SameLine();
		if (ImGui::Selectable((CanvasReportTitle(r->name) + "##rep").c_str(), ui.canvasReport == (int)i)) ui.canvasReport = (int)i;
		ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(96));
		std::string stamp = CanvasStamp(r->name);
		TextDim(stamp.size() > 5 ? stamp.substr(5, 11) : stamp);
		ImGui::PopID();
	}
}

// ---------------------------------------------------------------------------- main

static void DesignFilePanel(App& app)
{
	float width = ImGui::GetContentRegionAvail().x;
	ImGui::BeginChild("designfile", ImVec2(width, S(112)), ImGuiChildFlags_Borders);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	std::error_code ec;
	if (app.canvasFile.empty() || !fs::exists(app.canvasFile, ec))
	{
		// drop zone
		ImVec2 a = ImGui::GetWindowPos() + ImVec2(S(8), S(8));
		ImVec2 b = ImGui::GetWindowPos() + ImGui::GetWindowSize() - ImVec2(S(8), S(8));
		for (float x = a.x; x < b.x; x += S(10)) { dl->AddLine(ImVec2(x, a.y), ImVec2(ImMin(x + S(5), b.x), a.y), col::Muted); dl->AddLine(ImVec2(x, b.y), ImVec2(ImMin(x + S(5), b.x), b.y), col::Muted); }
		for (float y = a.y; y < b.y; y += S(10)) { dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, ImMin(y + S(5), b.y)), col::Muted); dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, ImMin(y + S(5), b.y)), col::Muted); }
		ImGui::SetCursorPos(ImVec2(S(26), S(26)));
		ImGui::PushFont(g_fonts.title);
		TextColored(col::Dim, (std::string(ICON_DOWNLOAD) + "  *.dwocanvas.json 파일을 이 창으로 끌어 놓으세요").c_str());
		ImGui::PopFont();
		ImGui::SetCursorPosX(S(26));
		TextDim("dwocanvas/2 형식(이전 designbridge/1 JSON도 가능). Figma 플러그인, HTML 변환기, 직접 작성한 파일 모두 같은 형식입니다.");
		ImGui::SetCursorPosX(S(26));
		if (ImGui::SmallButton("파일 열기...")) PickDesignFile(app);
		ImGui::EndChild();
		return;
	}
	std::string schema = DesignSchemaOf(app.canvasFile);
	ImGui::PushFont(g_fonts.title);
	ImGui::TextUnformatted((std::string(ICON_PAGE) + "  " + PathUtf8(app.canvasFile.filename())).c_str());
	ImGui::PopFont();
	ImGui::SameLine();
	if (schema.rfind("dwocanvas/", 0) == 0) Pill(schema, col::Pass);
	else if (schema.rfind("designbridge/", 0) == 0) Pill(schema + " · 이전 형식", col::Info);
	else Pill(schema.empty() ? "schema 없음" : schema, col::Warn);
	TextDim(PathUtf8(app.canvasFile.parent_path()));
	TextDim(Bytes((double)fs::file_size(app.canvasFile, ec)) + "  ·  드래그 앤 드롭이나 왼쪽 목록으로 다른 파일을 고를 수 있습니다");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(170));
	if (ImGui::SmallButton("다른 파일...")) PickDesignFile(app);
	ImGui::SameLine();
	if (ImGui::SmallButton("폴더 열기")) OpenWithShell(app.canvasFile.parent_path());
	ImGui::EndChild();
}

static void CanvasActions(App& app)
{
	std::error_code ec;
	bool haveFile = !app.canvasFile.empty() && fs::exists(app.canvasFile, ec);
	bool idle = !app.runner.running;
	bool canImport = haveFile && app.canvas != 0 && app.hasUProject;
	ImGui::BeginDisabled(!haveFile || !app.hasUProject);
	if (ImGui::Button((std::string(ICON_CHECKLIST) + "  검사 (Validate)").c_str())) RunCanvas(app, "canvas-validate");
	ImGui::EndDisabled();
	Tooltip("python -m dwocanvas validate: 스키마와 규칙을 확인합니다. 엔진을 띄우지 않아 몇 초면 끝납니다.");
	ImGui::SameLine();
	ImGui::BeginDisabled(!canImport);
	if (PrimaryButton((std::string(ICON_DOWNLOAD) + "  가져오기 (Import)").c_str())) RunCanvas(app, "canvas-import");
	Tooltip("UnrealEditor-Cmd -run=DwoCanvasImport. UE에서 고친 위젯은 유지하고 디자인이 바뀐 곳만 다시 만듭니다. 에디터를 닫고 실행하세요.");
	ImGui::SameLine();
	if (ImGui::Button((std::string(ICON_REFRESH) + "  초기화하고 가져오기").c_str())) RunCanvas(app, "canvas-import", true);
	Tooltip("-ResetOverrides: UE에서 고쳐 잠긴 위젯과 DwoCanvas가 만들지 않은 기존 WBP도 디자인대로 덮어씁니다.");
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(S(160));
	ImGui::InputTextWithHint("##doc", "문서 이름 (선택, --doc)", ui.canvasDoc, sizeof(ui.canvasDoc));
	Tooltip("파일에 화면 문서가 여러 개일 때 하나만 가져옵니다.");
	if (!idle && app.runner.current.find("canvas-") != std::string::npos)
	{
		ImGui::SameLine();
		Spinner(5, col::Info);
		ImGui::SameLine();
		TextDim(app.runner.current + "  ·  " + Duration(app.runner.ElapsedSeconds()));
	}
	if (app.canvas == 0 && app.hasUProject)
		TextColored(col::Warn, (std::string(ICON_WARNING) + "  가져오려면 먼저 DwoCanvas를 설치하세요 (왼쪽 플러그인 카드). 검사는 설치 없이 됩니다.").c_str());
}

static void CanvasReportSummary(App& app)
{
	CanvasReport* r = app.CanvasReportAt((size_t)ImMax(0, ui.canvasReport));
	if (!r)
	{
		TextDim("가져오기를 실행하면 Saved/DwoCanvas/Reports의 최신 리포트가 여기에 요약됩니다.");
		return;
	}
	// status line
	ImU32 c = r->errors ? col::Fail : r->warnings ? col::Warn : col::Pass;
	VerdictPill(r->errors ? "fail" : r->warnings ? "warn" : "pass");
	ImGui::SameLine();
	TextBold(r->status.empty() ? r->name : r->status);
	ImGui::SameLine();
	TextDim(CanvasReportTitle(r->name) + "  ·  " + CanvasStamp(r->name) + (ui.canvasReport == 0 ? "  ·  최신" : ""));
	ImGui::Spacing();

	// counts: four compact stat chips, then the widget counts of the "Widgets:" line
	float avail = ImGui::GetContentRegionAvail().x;
	float w = (avail - 3 * ImGui::GetStyle().ItemSpacing.x) / 4;
	int k = 0;
	auto chip = [&](const char* label, int value, ImU32 color, const char* tip) {
		if (k++) ImGui::SameLine();
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImVec2 size(w, S(52));
		ImGui::PushID(k);
		ImGui::InvisibleButton("chip", size);
		ImGui::PopID();
		if (tip) Tooltip(tip);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p, p + size, ImGui::IsItemHovered() ? col::Panel2 : col::Panel, S(6));
		dl->AddRect(p, p + size, col::Border, S(6));
		dl->AddRectFilled(p, p + ImVec2(S(3), size.y), color, S(6), ImDrawFlags_RoundCornersLeft);
		dl->AddText(p + ImVec2(S(14), S(8)), col::Dim, label);
		std::string v = std::to_string(value);
		dl->AddText(g_fonts.title, g_fonts.title->FontSize, p + ImVec2(S(14), S(24)), value ? col::Text : col::Dim, v.c_str());
	};
	chip("오류", r->errors, r->errors ? col::Fail : col::Muted, "ERROR 줄. 하나라도 있으면 커맨드릿 종료 코드가 0이 아닙니다.");
	chip("경고", r->warnings, r->warnings ? col::Warn : col::Muted, "WARN 줄");
	chip("정보", r->infos, col::Info, "info 줄");
	chip("쓴 에셋", (int)r->written.size(), col::Accent, "이번 가져오기에서 저장한 에셋");
	if (!r->counts.empty())
	{
		TextDim("위젯");
		for (auto& [label, n] : r->counts)
		{
			ImGui::SameLine();
			Pill(Fmt("%s %d", label.c_str(), n), n ? col::Teal : col::Muted);
		}
	}
	(void)c;
	ImGui::Spacing();

	static const char* tabs[] = { "오류·경고", "쓴 에셋", "원문" };
	Segmented("canvastab", tabs, 3, ui.canvasTab);
	if (ui.canvasTab == 0)
	{
		ImGui::SameLine(0, S(16));
		static const char* filters[] = { "전체", "오류", "경고", "정보" };
		Segmented("canvasfilter", filters, 4, ui.canvasEntryFilter);
		ImGui::SameLine(0, S(12));
		SearchBox("##canvasq", ui.filter, sizeof(ui.filter), ICON_FILTER "  필터", S(220));
		if (ImGui::BeginTable("entries", 2, kTable | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("심각도", ImGuiTableColumnFlags_WidthFixed, S(70));
			ImGui::TableSetupColumn("내용", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			int shown = 0;
			for (auto& e : r->entries)
			{
				int want = ui.canvasEntryFilter;
				if (want == 1 && e.severity != "error") continue;
				if (want == 2 && e.severity != "warn") continue;
				if (want == 3 && e.severity != "info") continue;
				if (!ContainsNoCase(e.text, ui.filter)) continue;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				SeverityPill(e.severity == "error" ? "error" : e.severity == "warn" ? "warn" : "info");
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", e.text.c_str());
				++shown;
			}
			if (shown == 0) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TableNextColumn(); TextDim(r->entries.empty() ? "메시지가 없습니다." : "필터에 맞는 메시지가 없습니다."); }
			ImGui::EndTable();
		}
	}
	else if (ui.canvasTab == 1)
	{
		if (r->written.empty()) TextDim("이 가져오기에서 저장한 에셋이 없습니다.");
		ImGui::BeginChild("written", ImVec2(0, 0));
		for (auto& a : r->written)
		{
			ImGui::PushID(a.c_str());
			if (ImGui::Selectable((std::string(ICON_PACKAGE) + "  " + a).c_str())) CopyToClipboard(a);
			Tooltip("클릭하면 경로를 복사합니다.");
			ImGui::PopID();
		}
		ImGui::EndChild();
	}
	else
	{
		static std::string rawFor, raw;
		if (rawFor != PathUtf8(r->path) + std::to_string(r->mtime)) { raw = ReadText(r->path); rawFor = PathUtf8(r->path) + std::to_string(r->mtime); }
		ImGui::PushFont(g_fonts.mono);
		ImGui::InputTextMultiline("##raw", (char*)raw.c_str(), raw.size() + 1, ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y), ImGuiInputTextFlags_ReadOnly);
		ImGui::PopFont();
	}
}

static void DrawCanvasMain(App& app)
{
	ViewHeader(ICON_CANVAS, "DwoCanvas", "디자인 파일 → UMG 위젯 블루프린트");
	if (!app.hasUProject)
	{
		if (EmptyState(ICON_FOLDER, "프로젝트가 없습니다", ".uproject가 있는 폴더를 열면 DwoCanvas를 설치하고 디자인 파일을 가져올 수 있습니다.", "프로젝트 열기..."))
		{
			fs::path p;
			if (PickOpen(g_hwnd, true, L".uproject가 있는 프로젝트 폴더 선택", p)) app.SetProject(p);
		}
		return;
	}
	SectionHeader("디자인 파일");
	DesignFilePanel(app);
	ImGui::Spacing();
	CanvasActions(app);
	SectionHeader("최근 가져오기 리포트", "Saved/DwoCanvas/Reports");
	CanvasReportSummary(app);
}

// ---------------------------------------------------------------------------- inspector

static void DrawCanvasInspector(App& app)
{
	SectionHeader("DwoCanvas");
	KeyValue("상태", CanvasStateText(app), 80);
	if (!app.canvasVersion.empty()) KeyValue("버전", app.canvasVersion, 80);
	KeyValue("런타임 모듈", "DwoCanvasRuntime", 80);
	KeyValue("에디터 모듈", "DwoCanvasEditor", 80);
	ImGui::PushTextWrapPos(0);
	TextDim("생성된 위젯 블루프린트가 DwoCanvasRuntime을 쓰므로 Plugins/DwoCanvas는 git에서 숨기지 않고 uninstall도 지우지 않습니다.");
	ImGui::PopTextWrapPos();

	if (CanvasReport* r = app.CanvasReportAt((size_t)ImMax(0, ui.canvasReport)))
	{
		SectionHeader("선택한 리포트");
		KeyValue("파일", r->name, 80);
		KeyValue("시각", CanvasStamp(r->name), 80);
		if (!r->widgets.empty())
		{
			ImGui::PushTextWrapPos(0);
			TextDim(r->widgets);
			ImGui::PopTextWrapPos();
		}
		if (ImGui::Button("리포트 열기")) OpenWithShell(r->path);
		ImGui::SameLine();
		if (ImGui::Button("폴더 열기")) OpenWithShell(r->path.parent_path());
	}

	SectionHeader("명령줄");
	std::string file = app.canvasFile.empty() ? "<file.dwocanvas.json>" : Quoted(app.canvasFile.filename());
	ImGui::PushFont(g_fonts.mono);
	ImGui::PushTextWrapPos(0);
	TextDim("__dwo-forge.bat canvas-validate " + file);
	TextDim("__dwo-forge.bat canvas-import " + file + " [--reset] [--doc Name]");
	ImGui::PopTextWrapPos();
	ImGui::PopFont();
	if (!app.canvasLastCommand.empty()) KeyValue("마지막 실행", app.canvasLastCommand, 80);

	SectionHeader("문서");
	if (ImGui::Button("DwoCanvas README")) OpenWithShell(app.home / "plugin" / "DwoCanvas" / "README.md");
	if (ImGui::Button("Schema-first 설계 (SchemaFirstArchitecture.md)")) OpenWithShell(app.home / "canvas" / "docs" / "SchemaFirstArchitecture.md");
}
