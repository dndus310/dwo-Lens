// Theme, fonts, icons and the small widgets every view shares.
#pragma once

#include "util.h"

// Segoe Fluent Icons / Segoe MDL2 Assets code points (UTF-8).
#define ICON_HOME      u8"\uE80F"
#define ICON_DASHBOARD u8"\uECA5"
#define ICON_LAYERS    u8"\uE81E"
#define ICON_WARNING   u8"\uE7BA"
#define ICON_CHECKLIST u8"\uE9D5"
#define ICON_CHECK     u8"\uE73E"
#define ICON_LIST      u8"\uE8FD"
#define ICON_PLAY      u8"\uE768"
#define ICON_SPEED     u8"\uEC4A"
#define ICON_PACKAGE   u8"\uE7B8"
#define ICON_DIAG      u8"\uE9D9"
#define ICON_EYE       u8"\uE7B3"
#define ICON_HISTORY   u8"\uE81C"
#define ICON_PUZZLE    u8"\uEA86"
#define ICON_SETTINGS  u8"\uE713"
#define ICON_OK        u8"\uE930"
#define ICON_ERROR     u8"\uE783"
#define ICON_INFO      u8"\uE946"
#define ICON_PAUSE     u8"\uE769"
#define ICON_STOP      u8"\uE71A"
#define ICON_REFRESH   u8"\uE72C"
#define ICON_SEARCH    u8"\uE721"
#define ICON_FOLDER    u8"\uE8B7"
#define ICON_SAVE      u8"\uE74E"
#define ICON_DOWNLOAD  u8"\uE896"
#define ICON_OPEN      u8"\uE8A7"
#define ICON_COPY      u8"\uE8C8"
#define ICON_FILTER    u8"\uE71C"
#define ICON_PAGE      u8"\uE7C3"
#define ICON_CODE      u8"\uE943"
#define ICON_BUG       u8"\uEBE8"
#define ICON_SYNC      u8"\uE895"
#define ICON_NETWORK   u8"\uE968"
#define ICON_TIMER     u8"\uE916"
#define ICON_FLAG      u8"\uE7C1"
#define ICON_LINK      u8"\uE71B"
#define ICON_ADD       u8"\uE710"
#define ICON_CLOSE     u8"\uE711"
#define ICON_DELETE    u8"\uE74D"
#define ICON_RIGHT     u8"\uE76C"
#define ICON_DOWN      u8"\uE70D"
#define ICON_GAME      u8"\uE7FC"
#define ICON_BOLT      u8"\uE945"
#define ICON_GRAPH     u8"\uE9D2"
#define ICON_LIBRARY   u8"\uE8F1"
#define ICON_TAG       u8"\uE8EC"
#define ICON_STAR      u8"\uE734"
#define ICON_RECORD    u8"\uE7C8"
#define ICON_POWER     u8"\uE7E8"
#define ICON_SORT      u8"\uE8CB"
#define ICON_PIN       u8"\uE718"
#define ICON_CANVAS    u8"\uE790"
#define ICON_FORGE     u8"\uE90F"

// ============================================================================ palette

namespace col
{
	static ImU32 Bg = IM_COL32(0x1E, 0x1F, 0x22, 255);
	static ImU32 Panel = IM_COL32(0x2B, 0x2D, 0x30, 255);
	static ImU32 Panel2 = IM_COL32(0x31, 0x33, 0x37, 255);
	static ImU32 Border = IM_COL32(0x3C, 0x3F, 0x41, 255);
	static ImU32 Text = IM_COL32(0xDF, 0xE1, 0xE5, 255);
	static ImU32 Dim = IM_COL32(0x8C, 0x91, 0x9A, 255);
	static ImU32 Accent = IM_COL32(0x35, 0x74, 0xF0, 255);
	static ImU32 Pass = IM_COL32(0x4C, 0xB0, 0x6E, 255);
	static ImU32 Warn = IM_COL32(0xE0, 0x9B, 0x3A, 255);
	static ImU32 Fail = IM_COL32(0xE5, 0x53, 0x4B, 255);
	static ImU32 Minor = IM_COL32(0xD6, 0xC3, 0x4A, 255);
	static ImU32 Info = IM_COL32(0x4A, 0x9C, 0xE8, 255);
	static ImU32 Muted = IM_COL32(0x6F, 0x73, 0x7A, 255);
	static ImU32 Purple = IM_COL32(0xA5, 0x7B, 0xE8, 255);
	static ImU32 Teal = IM_COL32(0x3F, 0xB8, 0xAF, 255);
}

static ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

static ImU32 WithAlpha(ImU32 c, float a)
{
	return (c & 0x00FFFFFF) | ((ImU32)(ImClamp(a, 0.f, 1.f) * 255) << 24);
}

static ImU32 Mix(ImU32 a, ImU32 b, float t)
{
	ImVec4 x = V4(a), y = V4(b);
	return ImGui::ColorConvertFloat4ToU32(ImLerp(x, y, ImClamp(t, 0.f, 1.f)));
}

// Verdicts, severities and log levels map to one semantic color each.
static ImU32 SemanticColor(const std::string& v)
{
	std::string s = Lower(v);
	if (s == "pass" || s == "success" || s == "ok" || s == "passed" || s == "live") return col::Pass;
	if (s == "warn" || s == "warning" || s == "major" || s == "succeededwithwarnings") return col::Warn;
	if (s == "fail" || s == "error" || s == "fatal" || s == "crash" || s == "blocker" || s == "critical" || s == "failed") return col::Fail;
	if (s == "minor") return col::Minor;
	if (s == "info" || s == "running") return col::Info;
	return col::Muted;
}

static const char* VerdictLabel(const std::string& v)
{
	if (v == "pass") return "통과";
	if (v == "warn") return "경고";
	if (v == "fail") return "실패";
	if (v == "info") return "정보";
	if (v == "incomplete") return "미완료";
	if (v == "running") return "실행 중";
	if (v.empty() || v == "?") return "-";
	return nullptr;
}

static int SeverityRank(const std::string& v)
{
	std::string s = Lower(v);
	if (s == "fatal" || s == "blocker" || s == "crash") return 5;
	if (s == "fail" || s == "error" || s == "critical") return 4;
	if (s == "warn" || s == "warning" || s == "major") return 3;
	if (s == "minor") return 2;
	if (s == "info") return 1;
	return 0;
}

// ============================================================================ fonts

struct Fonts
{
	ImFont* body = nullptr;
	ImFont* bold = nullptr;
	ImFont* title = nullptr;
	ImFont* big = nullptr;
	ImFont* mono = nullptr;
	ImFont* icon = nullptr;     // icons only, larger, for the activity bar
};
static Fonts g_fonts;
static float g_scale = 1.0f;

static float S(float v) { return v * g_scale; }

static void LoadFonts(float scale)
{
	ImGuiIO& io = ImGui::GetIO();
	auto exists = [](const char* p) { std::error_code ec; return fs::exists(p, ec); };
	const char* latin = "C:\\Windows\\Fonts\\segoeui.ttf";
	const char* latinBold = "C:\\Windows\\Fonts\\seguisb.ttf";
	const char* hangul = "C:\\Windows\\Fonts\\malgun.ttf";
	const char* hangulBold = "C:\\Windows\\Fonts\\malgunbd.ttf";
	const char* mono = "C:\\Windows\\Fonts\\consola.ttf";
	const char* icons = exists("C:\\Windows\\Fonts\\SegoeIcons.ttf") ? "C:\\Windows\\Fonts\\SegoeIcons.ttf" : "C:\\Windows\\Fonts\\segmdl2.ttf";

	static const ImWchar iconRanges[] = { 0xE700, 0xF8FF, 0 };
	static ImVector<ImWchar> textRanges;
	if (textRanges.empty())
	{
		ImFontGlyphRangesBuilder b;
		b.AddRanges(io.Fonts->GetGlyphRangesKorean());
		b.AddText(u8"·…→←↑↓●○◆▲▼✓✕—–“”‘’×≥≤±°");
		b.BuildRanges(&textRanges);
	}

	auto add = [&](const char* primary, const char* korean, float size, bool withIcons) -> ImFont* {
		ImFont* f = nullptr;
		ImFontConfig cfg;
		cfg.OversampleH = 2;
		if (exists(primary)) f = io.Fonts->AddFontFromFileTTF(primary, size, &cfg, io.Fonts->GetGlyphRangesDefault());
		else f = io.Fonts->AddFontDefault();
		ImFontConfig merge;
		merge.MergeMode = true;
		merge.OversampleH = 2;
		if (exists(korean)) io.Fonts->AddFontFromFileTTF(korean, size, &merge, textRanges.Data);
		if (withIcons && exists(icons))
		{
			ImFontConfig ic;
			ic.MergeMode = true;
			ic.GlyphOffset = ImVec2(0, size * 0.18f);
			ic.PixelSnapH = true;
			io.Fonts->AddFontFromFileTTF(icons, size * 0.92f, &ic, iconRanges);
		}
		return f;
	};
	g_fonts.body = add(latin, hangul, 15.0f * scale, true);
	g_fonts.bold = add(latinBold, hangulBold, 15.0f * scale, true);
	g_fonts.title = add(latinBold, hangulBold, 19.0f * scale, true);
	g_fonts.big = add(latinBold, hangulBold, 28.0f * scale, false);
	g_fonts.mono = add(mono, hangul, 14.0f * scale, false);
	{
		ImFontConfig cfg;
		cfg.PixelSnapH = true;
		g_fonts.icon = exists(icons) ? io.Fonts->AddFontFromFileTTF(icons, 20.0f * scale, &cfg, iconRanges) : g_fonts.body;
	}
	io.FontDefault = g_fonts.body;
}

static void ApplyTheme(float scale)
{
	ImGuiStyle& s = ImGui::GetStyle();
	s = ImGuiStyle();
	ImGui::StyleColorsDark(&s);
	s.WindowRounding = 0;
	s.ChildRounding = 4;
	s.FrameRounding = 4;
	s.PopupRounding = 6;
	s.GrabRounding = 4;
	s.TabRounding = 4;
	s.ScrollbarRounding = 6;
	s.ScrollbarSize = 11;
	s.WindowBorderSize = 1;
	s.ChildBorderSize = 1;
	s.FrameBorderSize = 0;
	s.PopupBorderSize = 1;
	s.TabBorderSize = 0;
	s.TabBarBorderSize = 1;
	s.DockingSeparatorSize = 1;
	s.WindowPadding = ImVec2(12, 10);
	s.FramePadding = ImVec2(8, 5);
	s.ItemSpacing = ImVec2(8, 6);
	s.ItemInnerSpacing = ImVec2(6, 4);
	s.CellPadding = ImVec2(8, 5);
	s.IndentSpacing = 16;
	s.WindowMenuButtonPosition = ImGuiDir_None;
	s.SeparatorTextBorderSize = 1;

	ImVec4* c = s.Colors;
	auto set = [&](ImGuiCol i, ImU32 v) { c[i] = V4(v); };
	set(ImGuiCol_Text, col::Text);
	set(ImGuiCol_TextDisabled, col::Dim);
	set(ImGuiCol_WindowBg, col::Bg);
	set(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
	set(ImGuiCol_PopupBg, IM_COL32(0x2B, 0x2D, 0x30, 250));
	set(ImGuiCol_Border, col::Border);
	set(ImGuiCol_FrameBg, IM_COL32(0x2B, 0x2D, 0x30, 255));
	set(ImGuiCol_FrameBgHovered, IM_COL32(0x35, 0x38, 0x3C, 255));
	set(ImGuiCol_FrameBgActive, IM_COL32(0x3A, 0x3D, 0x42, 255));
	set(ImGuiCol_TitleBg, col::Bg);
	set(ImGuiCol_TitleBgActive, col::Bg);
	set(ImGuiCol_MenuBarBg, IM_COL32(0x2B, 0x2D, 0x30, 255));
	set(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
	set(ImGuiCol_ScrollbarGrab, IM_COL32(0x4A, 0x4D, 0x52, 255));
	set(ImGuiCol_ScrollbarGrabHovered, IM_COL32(0x5A, 0x5E, 0x64, 255));
	set(ImGuiCol_CheckMark, col::Accent);
	set(ImGuiCol_SliderGrab, col::Accent);
	set(ImGuiCol_Button, IM_COL32(0x3A, 0x3D, 0x41, 255));
	set(ImGuiCol_ButtonHovered, IM_COL32(0x46, 0x4A, 0x4F, 255));
	set(ImGuiCol_ButtonActive, IM_COL32(0x50, 0x54, 0x5A, 255));
	set(ImGuiCol_Header, IM_COL32(0x2E, 0x43, 0x6E, 255));
	set(ImGuiCol_HeaderHovered, IM_COL32(0x35, 0x38, 0x3C, 255));
	set(ImGuiCol_HeaderActive, IM_COL32(0x2E, 0x43, 0x6E, 255));
	set(ImGuiCol_Separator, col::Border);
	set(ImGuiCol_SeparatorHovered, col::Accent);
	set(ImGuiCol_SeparatorActive, col::Accent);
	set(ImGuiCol_ResizeGrip, IM_COL32(0, 0, 0, 0));
	set(ImGuiCol_Tab, col::Bg);
	set(ImGuiCol_TabHovered, IM_COL32(0x35, 0x38, 0x3C, 255));
	set(ImGuiCol_TabSelected, IM_COL32(0x2B, 0x2D, 0x30, 255));
	set(ImGuiCol_TabSelectedOverline, col::Accent);
	set(ImGuiCol_TabDimmed, col::Bg);
	set(ImGuiCol_TabDimmedSelected, IM_COL32(0x2B, 0x2D, 0x30, 255));
	set(ImGuiCol_TabDimmedSelectedOverline, IM_COL32(0, 0, 0, 0));
	set(ImGuiCol_DockingPreview, WithAlpha(col::Accent, 0.5f));
	set(ImGuiCol_DockingEmptyBg, col::Bg);
	set(ImGuiCol_TableHeaderBg, IM_COL32(0x26, 0x28, 0x2B, 255));
	set(ImGuiCol_TableBorderStrong, col::Border);
	set(ImGuiCol_TableBorderLight, IM_COL32(0x33, 0x35, 0x39, 255));
	set(ImGuiCol_TableRowBg, IM_COL32(0, 0, 0, 0));
	set(ImGuiCol_TableRowBgAlt, IM_COL32(255, 255, 255, 6));
	set(ImGuiCol_TextSelectedBg, WithAlpha(col::Accent, 0.35f));
	set(ImGuiCol_NavHighlight, col::Accent);
	set(ImGuiCol_PlotLines, col::Accent);
	set(ImGuiCol_PlotHistogram, col::Accent);
	set(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 120));
	s.ScaleAllSizes(scale);
}

// ============================================================================ widgets

static const ImGuiTableFlags kTable = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchProp;

static void Pill(const std::string& text, ImU32 color, bool filled = false)
{
	ImFont* f = g_fonts.bold;
	float size = f->FontSize * 0.82f;
	ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
	ImVec2 pad(S(6), S(1.5f));
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float lineH = ImGui::GetTextLineHeight();
	pos.y += (lineH - ts.y - pad.y * 2) * 0.5f;
	ImVec2 end = pos + ts + pad * 2;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(pos, end, filled ? color : WithAlpha(color, 0.18f), S(4));
	if (!filled) dl->AddRect(pos, end, WithAlpha(color, 0.55f), S(4));
	dl->AddText(f, size, pos + pad, filled ? IM_COL32(255, 255, 255, 255) : color, text.c_str());
	ImGui::Dummy(ImVec2(end.x - ImGui::GetCursorScreenPos().x, lineH));
}

static void VerdictPill(const std::string& verdict)
{
	const char* label = VerdictLabel(verdict);
	Pill(label ? label : verdict, SemanticColor(verdict));
}

static void SeverityPill(const std::string& severity)
{
	std::string s = Lower(severity);
	const char* label = s == "fatal" ? "치명" : s == "fail" || s == "error" ? "오류" : s == "warn" || s == "warning" ? "경고"
	                  : s == "minor" ? "경미" : s == "info" ? "정보" : severity.c_str();
	Pill(label, SemanticColor(severity));
}

static void StatusDot(ImU32 color, float radius = 4.0f, bool pulse = false)
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = ImGui::GetTextLineHeight();
	float r = S(radius);
	ImVec2 c(p.x + r + S(1), p.y + h * 0.5f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (pulse)
	{
		float t = (float)fmod(ImGui::GetTime(), 1.4) / 1.4f;
		dl->AddCircleFilled(c, r + t * S(5), WithAlpha(color, 0.45f * (1 - t)));
	}
	dl->AddCircleFilled(c, r, color);
	ImGui::Dummy(ImVec2(r * 2 + S(4), h));
}

static void TextColored(ImU32 color, const char* text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
}

static void TextDim(const std::string& text) { TextColored(col::Dim, text.c_str()); }

static void TextBold(const std::string& text)
{
	ImGui::PushFont(g_fonts.bold);
	ImGui::TextUnformatted(text.c_str());
	ImGui::PopFont();
}

static void Title(const std::string& text)
{
	ImGui::PushFont(g_fonts.title);
	ImGui::TextUnformatted(text.c_str());
	ImGui::PopFont();
}

static void SectionHeader(const char* text, const char* right = nullptr)
{
	ImGui::Spacing();
	ImGui::PushFont(g_fonts.bold);
	ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
	ImGui::PopFont();
	if (right)
	{
		ImGui::SameLine();
		TextDim(right);
	}
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), col::Border);
	ImGui::Dummy(ImVec2(0, S(4)));
}

static void KeyValue(const char* key, const std::string& value, float keyWidth = 110)
{
	TextDim(key);
	ImGui::SameLine(S(keyWidth));
	ImGui::TextWrapped("%s", value.empty() ? "-" : value.c_str());
}

static void HelpMarker(const char* text)
{
	ImGui::SameLine();
	TextDim(ICON_INFO);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(S(380));
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

static void Tooltip(const char* text)
{
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(S(380));
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

// Primary (accent) button.
static bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0))
{
	ImGui::PushStyleColor(ImGuiCol_Button, col::Accent);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Mix(col::Accent, IM_COL32(255, 255, 255, 255), 0.15f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, Mix(col::Accent, IM_COL32(0, 0, 0, 255), 0.15f));
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
	bool r = ImGui::Button(label, size);
	ImGui::PopStyleColor(4);
	return r;
}

static bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0))
{
	ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0x6B, 0x2A, 0x28, 255));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0x85, 0x33, 0x30, 255));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0x5A, 0x22, 0x20, 255));
	bool r = ImGui::Button(label, size);
	ImGui::PopStyleColor(3);
	return r;
}

// Flat icon button for toolbars. `active` draws it as toggled on.
static bool ToolButton(const char* icon, const char* tip, bool active = false, bool enabled = true)
{
	ImGui::PushStyleColor(ImGuiCol_Button, active ? WithAlpha(col::Accent, 0.35f) : IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 20));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 35));
	ImGui::BeginDisabled(!enabled);
	bool r = ImGui::Button(icon);
	ImGui::EndDisabled();
	ImGui::PopStyleColor(3);
	if (tip) Tooltip(tip);
	return r;
}

static bool SearchBox(const char* id, char* buf, size_t size, const char* hint, float width)
{
	ImGui::SetNextItemWidth(width);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(5));
	bool changed = ImGui::InputTextWithHint(id, hint, buf, size);
	ImGui::PopStyleVar();
	if (buf[0])
	{
		ImGui::SameLine(0, S(2));
		ImGui::PushID(id);
		if (ToolButton(ICON_CLOSE, "지우기")) { buf[0] = 0; changed = true; }
		ImGui::PopID();
	}
	return changed;
}

// Segmented control (two to four exclusive options).
static bool Segmented(const char* id, const char* const* labels, int count, int& value)
{
	bool changed = false;
	ImGui::PushID(id);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	for (int i = 0; i < count; ++i)
	{
		if (i) ImGui::SameLine();
		bool on = value == i;
		ImGui::PushStyleColor(ImGuiCol_Button, on ? col::Accent : IM_COL32(0x2B, 0x2D, 0x30, 255));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? col::Accent : IM_COL32(0x3A, 0x3D, 0x41, 255));
		ImGui::PushStyleColor(ImGuiCol_Text, on ? IM_COL32(255, 255, 255, 255) : col::Dim);
		ImDrawFlags corners = count == 1 ? ImDrawFlags_RoundCornersAll : i == 0 ? ImDrawFlags_RoundCornersLeft : i == count - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
		(void)corners;
		ImGui::PushID(i);
		if (ImGui::Button(labels[i])) { value = i; changed = true; }
		ImGui::PopID();
		ImGui::PopStyleColor(3);
	}
	ImGui::PopStyleVar();
	ImGui::PopID();
	return changed;
}

// Empty-state block: icon, message, and an optional call to action.
static bool EmptyState(const char* icon, const char* title, const char* body, const char* action = nullptr)
{
	ImVec2 avail = ImGui::GetContentRegionAvail();
	float width = ImMin(avail.x, S(460));
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImMax(S(20), avail.y * 0.22f));
	float x = ImGui::GetCursorPosX() + (avail.x - width) * 0.5f;
	ImGui::PushFont(g_fonts.icon);
	ImVec2 is = ImGui::CalcTextSize(icon);
	ImGui::SetCursorPosX(x + (width - is.x * 2) * 0.5f);
	ImGui::PushStyleColor(ImGuiCol_Text, col::Muted);
	ImGui::SetWindowFontScale(2.0f);
	ImGui::TextUnformatted(icon);
	ImGui::SetWindowFontScale(1.0f);
	ImGui::PopStyleColor();
	ImGui::PopFont();
	ImGui::PushFont(g_fonts.title);
	ImVec2 ts = ImGui::CalcTextSize(title);
	ImGui::SetCursorPosX(x + ImMax(0.f, (width - ts.x) * 0.5f));
	ImGui::TextUnformatted(title);
	ImGui::PopFont();
	ImGui::SetCursorPosX(x);
	ImGui::PushTextWrapPos(x + width);
	ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
	ImGui::TextWrapped("%s", body);
	ImGui::PopStyleColor();
	ImGui::PopTextWrapPos();
	bool clicked = false;
	if (action)
	{
		ImGui::Spacing();
		ImVec2 bs = ImGui::CalcTextSize(action) + ImGui::GetStyle().FramePadding * 2 + ImVec2(S(20), S(4));
		ImGui::SetCursorPosX(x + (width - bs.x) * 0.5f);
		clicked = PrimaryButton(action, bs);
	}
	return clicked;
}

// Sparkline inside the given rect. Values left to right, oldest first.
static void Sparkline(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::vector<float>& values, ImU32 color, bool fill = true)
{
	if (values.size() < 2) return;
	float lo = *std::min_element(values.begin(), values.end());
	float hi = *std::max_element(values.begin(), values.end());
	if (hi - lo < 1e-6f) { hi += 1; lo -= 1; }
	std::vector<ImVec2> pts;
	for (size_t i = 0; i < values.size(); ++i)
	{
		float x = a.x + (b.x - a.x) * (float)i / (float)(values.size() - 1);
		float y = b.y - (b.y - a.y) * (values[i] - lo) / (hi - lo);
		pts.push_back(ImVec2(x, y));
	}
	if (fill)
	{
		for (size_t i = 0; i + 1 < pts.size(); ++i)
		{
			dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, b.y), ImVec2(pts[i].x, b.y), WithAlpha(color, 0.12f));
		}
	}
	dl->AddPolyline(pts.data(), (int)pts.size(), color, 0, S(1.5f));
	dl->AddCircleFilled(pts.back(), S(2.5f), color);
}

// KPI card: label, big value, delta text and an optional trend line.
static void KpiCard(const char* id, const char* label, const std::string& value, const std::string& delta, ImU32 deltaColor,
                    const std::vector<float>& trend, ImU32 accent, float width, const char* tip = nullptr, bool* clicked = nullptr)
{
	ImVec2 size(width, S(104));
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, size);
	bool hovered = ImGui::IsItemHovered();
	if (clicked) *clicked = ImGui::IsItemClicked();
	if (tip) Tooltip(tip);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + size, hovered ? col::Panel2 : col::Panel, S(6));
	dl->AddRect(p, p + size, col::Border, S(6));
	dl->AddRectFilled(p + ImVec2(0, S(10)), p + ImVec2(S(3), size.y - S(10)), accent, S(2));
	ImVec2 pad(S(14), S(10));
	dl->AddText(g_fonts.body, g_fonts.body->FontSize, p + pad, col::Dim, label);
	dl->AddText(g_fonts.big, g_fonts.big->FontSize, p + pad + ImVec2(0, S(20)), col::Text, value.c_str());
	if (!delta.empty())
		dl->AddText(g_fonts.body, g_fonts.body->FontSize * 0.92f, p + ImVec2(pad.x, size.y - S(24)), deltaColor, delta.c_str());
	if (trend.size() >= 2)
		Sparkline(dl, p + ImVec2(size.x * 0.55f, S(42)), p + ImVec2(size.x - S(12), size.y - S(16)), trend, accent);
}

// Horizontal progress bar with a label inside.
static void Meter(float fraction, ImU32 color, const char* overlay, float width = -1, float height = 0)
{
	if (width < 0) width = ImGui::GetContentRegionAvail().x;
	if (height <= 0) height = S(6);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + ImVec2(width, height), IM_COL32(255, 255, 255, 18), height * 0.5f);
	dl->AddRectFilled(p, p + ImVec2(width * ImClamp(fraction, 0.f, 1.f), height), color, height * 0.5f);
	ImGui::Dummy(ImVec2(width, height));
	if (overlay) Tooltip(overlay);
}

// Small colored squares, one per past result (oldest left). 'p' pass, 'f' fail, 'w' warn, '-' none.
static void ResultStrip(const std::string& results, const char* tip = nullptr)
{
	float s = S(8), gap = S(2);
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = ImGui::GetTextLineHeight();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	for (size_t i = 0; i < results.size(); ++i)
	{
		char c = results[i];
		ImU32 color = c == 'p' ? col::Pass : c == 'f' ? col::Fail : c == 'w' ? col::Warn : IM_COL32(255, 255, 255, 25);
		ImVec2 a(p.x + i * (s + gap), p.y + (h - s) * 0.5f);
		dl->AddRectFilled(a, a + ImVec2(s, s), color, S(2));
	}
	ImGui::Dummy(ImVec2(results.size() * (s + gap), h));
	if (tip) Tooltip(tip);
}

static void Spinner(float radius, ImU32 color)
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = ImGui::GetTextLineHeight();
	float r = S(radius);
	ImVec2 c(p.x + r, p.y + h * 0.5f);
	float t = (float)ImGui::GetTime() * 6.0f;
	ImGui::GetWindowDrawList()->PathArcTo(c, r, t, t + IM_PI * 1.4f, 16);
	ImGui::GetWindowDrawList()->PathStroke(color, 0, S(2));
	ImGui::Dummy(ImVec2(r * 2 + S(2), h));
}

// ============================================================================ toasts

struct Toast
{
	std::string title, body;
	ImU32 color;
	double at;
	std::function<void()> action;
	std::string actionLabel;
};
static std::vector<Toast> g_toasts;

static void PushToast(const std::string& title, const std::string& body, ImU32 color, std::function<void()> action = nullptr,
                      const std::string& actionLabel = "")
{
	g_toasts.push_back({ title, body, color, ImGui::GetTime(), std::move(action), actionLabel });
	if (g_toasts.size() > 4) g_toasts.erase(g_toasts.begin());
}

static void DrawToasts(float bottomInset)
{
	ImGuiViewport* vp = ImGui::GetMainViewport();
	float y = vp->WorkPos.y + vp->WorkSize.y - bottomInset - S(12);
	double now = ImGui::GetTime();
	for (int i = (int)g_toasts.size() - 1; i >= 0; --i)
	{
		Toast& t = g_toasts[i];
		double age = now - t.at;
		if (age > 7.0) { g_toasts.erase(g_toasts.begin() + i); continue; }
		float alpha = (float)ImMin(1.0, ImMin(age * 6.0, (7.0 - age) * 2.0));
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - S(16), y), ImGuiCond_Always, ImVec2(1, 1));
		ImGui::SetNextWindowSize(ImVec2(S(340), 0));
		ImGui::SetNextWindowBgAlpha(0.97f * alpha);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8));
		std::string name = "##toast" + std::to_string(i);
		ImGui::Begin(name.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize);
		ImVec2 wp = ImGui::GetWindowPos();
		ImGui::GetWindowDrawList()->AddRectFilled(wp, wp + ImVec2(S(4), ImGui::GetWindowHeight()), t.color, S(8), ImDrawFlags_RoundCornersLeft);
		TextBold(t.title);
		if (!t.body.empty())
		{
			ImGui::PushTextWrapPos(S(320));
			TextDim(t.body);
			ImGui::PopTextWrapPos();
		}
		if (t.action)
		{
			if (ImGui::SmallButton(t.actionLabel.c_str())) { auto a = t.action; g_toasts.erase(g_toasts.begin() + i); ImGui::End(); ImGui::PopStyleVar(2); a(); return; }
			ImGui::SameLine();
		}
		if (ImGui::SmallButton("닫기")) t.at = now - 7.0;
		y -= ImGui::GetWindowHeight() + S(8);
		ImGui::End();
		ImGui::PopStyleVar(2);
	}
}
