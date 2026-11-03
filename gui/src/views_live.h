// State Monitor: live state from running games as an object graph, per-field state machines,
// a property inspector, a scrubbable timeline and an event list.
#pragma once

#include "views.h"

static LiveSession* LiveCurrentSession(App& app)
{
	LiveSession* s = app.store.Find(ui.liveSession);
	if (s) return s;
	// default: newest connected session, else newest
	for (auto it = app.store.sessions.rbegin(); it != app.store.sessions.rend(); ++it)
		if (it->connected && !it->sources.empty()) { ui.liveSession = it->key; return &*it; }
	if (!app.store.sessions.empty()) { ui.liveSession = app.store.sessions.back().key; return &app.store.sessions.back(); }
	return nullptr;
}

static double LiveNow() { return NowSeconds(); }
static double LiveT() { return ui.playhead >= 0 ? ui.playhead : LiveNow(); }

static ImU32 RoleColor(const LiveSource& s)
{
	const std::string& id = s.id;
	if (id == "GameMode") return col::Purple;
	if (id == "GameState") return col::Teal;
	if (id.find(".PlayerState") != std::string::npos) return col::Pass;
	if (id.size() >= 5 && id.compare(id.size() - 5, 5, ".Pawn") == 0) return col::Warn;
	if (id.rfind("PC", 0) == 0 && id.find('.') == std::string::npos) return col::Accent;
	if (s.kind == "struct") return col::Info;
	if (id.find(".Pawn.") != std::string::npos) return IM_COL32(0xB0, 0x86, 0x4A, 255);
	return col::Muted;
}

static const char* RoleIcon(const LiveSource& s)
{
	if (s.id == "GameMode" || s.id == "GameState") return ICON_GAME;
	if (s.kind == "struct") return ICON_CODE;
	if (s.id.rfind("PC", 0) == 0 && s.id.find('.') == std::string::npos) return ICON_GAME;
	return ICON_TAG;
}

static float Recency(double changedAt, double window = 1.5)
{
	if (ui.playhead >= 0) return 0;
	double age = LiveNow() - changedAt;
	return age < 0 || age > window ? 0.f : (float)(1.0 - age / window);
}

// ============================================================================ nav

static void DrawLiveNav(App& app)
{
	SectionHeader("연결");
	if (app.live.listening)
	{
		int n = app.live.ConnectedCount();
		StatusDot(n ? col::Pass : col::Info, 4, n == 0);
		ImGui::SameLine();
		ImGui::TextUnformatted(n ? Fmt("게임 %d개 연결됨", n).c_str() : Fmt("127.0.0.1:%d 대기 중", app.live.port).c_str());
	}
	else
	{
		StatusDot(col::Fail);
		ImGui::SameLine();
		ImGui::PushTextWrapPos(0);
		TextColored(col::Fail, app.live.error.empty() ? "수신 꺼짐" : app.live.error.c_str());
		ImGui::PopTextWrapPos();
		if (ImGui::SmallButton("다시 시도")) app.live.Start(app.live.port);
	}
	SectionHeader("세션");
	if (app.store.sessions.empty()) TextDim("아직 연결된 게임이 없습니다.");
	LiveSession* cur = LiveCurrentSession(app);
	for (auto it = app.store.sessions.rbegin(); it != app.store.sessions.rend(); ++it)
	{
		LiveSession& s = *it;
		ImGui::PushID(s.key);
		StatusDot(s.connected ? col::Pass : s.offline ? col::Info : col::Muted, 4, s.connected);
		ImGui::SameLine();
		if (ImGui::Selectable(s.name.c_str(), cur && cur->key == s.key)) { ui.liveSession = s.key; ui.liveSource.clear(); ui.liveGraphKey.clear(); }
		std::string world = Str(s.hello, "world");
		Tooltip((s.name + "\n" + (world.empty() ? "" : "월드 " + world + "\n") + Fmt("메시지 %d · 소스 %d", s.messages, (int)s.sources.size())).c_str());
		ImGui::PopID();
	}
	if (!cur) return;
	SectionHeader("상태 소스", std::to_string(cur->sources.size()).c_str());
	SearchBox("##livefilter", ui.liveFilter, sizeof(ui.liveFilter), "필터", -FLT_MIN);
	ImGui::BeginChild("sources", ImVec2(0, ImGui::GetContentRegionAvail().y - S(190)));
	for (auto& id : cur->order)
	{
		auto it = cur->sources.find(id);
		if (it == cur->sources.end()) continue;
		LiveSource& src = it->second;
		if (!ContainsNoCase(src.label + " " + src.type + " " + src.id, ui.liveFilter)) continue;
		ImGui::PushID(id.c_str());
		float flash = Recency(src.lastT, 1.0);
		if (flash > 0 && src.history.size() > 1 && !src.history.back().changes.empty())
		{
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight()), WithAlpha(col::Warn, 0.18f * flash), S(3));
		}
		int depth = (int)std::count(id.begin(), id.end(), '.');
		ImGui::Indent(S(10) * depth);
		TextColored(src.removed ? col::Muted : RoleColor(src), RoleIcon(src));
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, src.removed ? col::Muted : col::Text);
		if (ImGui::Selectable(src.label.c_str(), ui.liveSource == id)) { ui.liveSource = id; ui.liveField.clear(); }
		ImGui::PopStyleColor();
		ImGui::Unindent(S(10) * depth);
		Tooltip((src.id + "\n" + src.type + (src.object.empty() ? "" : "\n" + src.object) + Fmt("\n변경 %d회", src.changeCount)).c_str());
		ImGui::PopID();
	}
	ImGui::EndChild();
	SectionHeader("자동 관찰");
	bool changed = ImGui::Checkbox("GameMode·플레이어·Pawn 자동 관찰", &app.liveSettings.autoObserve);
	Tooltip("코드 없이 GameMode, GameState, PlayerController, PlayerState, Pawn과 컴포넌트를 주기적으로 읽습니다.");
	changed |= ImGui::Checkbox("엔진 프로퍼티 포함", &app.liveSettings.engineProps);
	Tooltip("끄면 프로젝트(BP·C++)가 선언한 프로퍼티만 보냅니다.");
	ImGui::SetNextItemWidth(S(120));
	if (ImGui::SliderFloat("주기", &app.liveSettings.interval, 0.05f, 2.0f, "%.2f초")) changed = true;
	if (changed && !ImGui::IsItemActive()) app.BroadcastLiveSettings();
	ImGui::SetNextItemWidth(-S(40));
	bool add = ImGui::InputTextWithHint("##watch", "관찰할 클래스 (예: BP_Enemy_C)", ui.watchInput, sizeof(ui.watchInput), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	add |= ImGui::Button(ICON_ADD);
	if (add && ui.watchInput[0])
	{
		std::string c = ui.watchInput;
		if (std::find(app.liveSettings.watch.begin(), app.liveSettings.watch.end(), c) == app.liveSettings.watch.end())
		{
			app.liveSettings.watch.push_back(c);
			app.live.Broadcast({ { "t", "watch" }, { "class", c }, { "max", 16 } });
			app.SaveSettings();
		}
		ui.watchInput[0] = 0;
	}
	for (size_t i = 0; i < app.liveSettings.watch.size(); ++i)
	{
		ImGui::PushID((int)i);
		if (ImGui::SmallButton(ICON_CLOSE))
		{
			app.live.Broadcast({ { "t", "unwatch" }, { "class", app.liveSettings.watch[i] } });
			app.liveSettings.watch.erase(app.liveSettings.watch.begin() + i);
			app.SaveSettings();
			ImGui::PopID();
			break;
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(app.liveSettings.watch[i].c_str());
		ImGui::PopID();
	}
}

// ============================================================================ graphs

static void BuildObjectGraph(App& app, LiveSession& s)
{
	double t = LiveT();
	std::vector<GraphNode> nodes;
	std::vector<GraphLink> links;
	std::map<std::string, int> byObject, byId;
	for (auto& id : s.order)
	{
		auto it = s.sources.find(id);
		if (it == s.sources.end()) continue;
		const LiveSource& src = it->second;
		if (src.removed && ui.playhead < 0) continue;
		if (ui.playhead >= 0 && src.firstT > t) continue;
		GraphNode n;
		n.id = id;
		n.title = src.label;
		n.subtitle = src.type + (src.autoSource ? "  · 자동" : "");
		n.accent = RoleColor(src);
		n.icon = RoleIcon(src);
		const json& value = app.store.ValueAt(src, t);
		std::vector<std::pair<std::string, const json*>> leaves;
		FlattenJson(value, "", leaves, 200);
		for (auto& [path, v] : leaves)
		{
			GraphRow row;
			row.key = path;
			row.value = ShortObjectName(LeafText(*v));
			if (row.value.size() > 40) row.value = row.value.substr(0, 37) + "…";
			auto ch = src.changedAt.find(path);
			if (ch != src.changedAt.end()) row.flash = Recency(ch->second);
			if (v->is_boolean()) row.color = v->get<bool>() ? col::Pass : col::Muted;
			n.rows.push_back(std::move(row));
		}
		// recently changed fields first so they stay visible
		std::stable_sort(n.rows.begin(), n.rows.end(), [](const GraphRow& a, const GraphRow& b) { return a.flash > b.flash; });
		n.footer = Fmt("필드 %d · 변경 %d", (int)leaves.size(), src.changeCount);
		if (leaves.empty()) n.footer = src.kind == "object" ? (app.liveSettings.engineProps ? "프로퍼티 없음" : "프로젝트 프로퍼티 없음 · 엔진 프로퍼티 포함으로 전체 보기") : "빈 값";
		float f = 0;
		if (!src.history.empty() && !src.history.back().changes.empty()) f = Recency(src.lastT, 0.8);
		n.flash = f;
		n.dim = src.removed;
		byId[id] = (int)nodes.size();
		if (!src.object.empty()) byObject[src.object] = (int)nodes.size();
		nodes.push_back(std::move(n));
	}
	// ownership and references
	std::set<std::pair<int, int>> seen;
	for (auto& [id, index] : byId)
	{
		const LiveSource& src = s.sources[id];
		auto own = byObject.find(src.owner);
		if (!src.owner.empty() && own != byObject.end() && own->second != index && seen.insert({ own->second, index }).second)
			links.push_back({ own->second, index, "소유", 1, false, 0, IM_COL32(0x9A, 0xA0, 0xA8, 200) });
		else if (src.owner.empty() || own == byObject.end())
		{
			// id hierarchy (PC0 -> PC0.Pawn -> PC0.Pawn.Health)
			auto dot = id.rfind('.');
			if (dot != std::string::npos)
			{
				auto parent = byId.find(id.substr(0, dot));
				if (parent != byId.end() && seen.insert({ parent->second, index }).second)
					links.push_back({ parent->second, index, "", 1, false, 0, IM_COL32(0x9A, 0xA0, 0xA8, 160) });
			}
		}
		std::vector<std::pair<std::string, const json*>> leaves;
		FlattenJson(app.store.ValueAt(src, LiveT()), "", leaves, 200);
		for (auto& [path, v] : leaves)
		{
			if (!v->is_string()) continue;
			auto target = byObject.find(v->get<std::string>());
			if (target == byObject.end() || target->second == index) continue;
			if (!seen.insert({ index, target->second }).second) continue;
			links.push_back({ index, target->second, path, 1, true, 0, WithAlpha(col::Info, 0.85f) });
		}
	}
	ui.liveGraph.rowLimit = 7;
	ui.liveGraph.horizontal = true;
	ui.liveGraph.Set(std::move(nodes), std::move(links));
}

static std::vector<const FieldMachine*> MachinesOf(const LiveSource& src)
{
	std::vector<const FieldMachine*> list;
	for (auto& [path, fm] : src.machines)
		if (fm.discrete) list.push_back(&fm);
	std::stable_sort(list.begin(), list.end(), [](const FieldMachine* a, const FieldMachine* b) {
		bool ea = a->kind == "string", eb = b->kind == "string";
		if ((a->transitions > 0) != (b->transitions > 0)) return a->transitions > 0;
		if (ea != eb) return ea;
		return a->transitions > b->transitions;
	});
	return list;
}

static void BuildMachineGraph(App& app, const LiveSource& src, const FieldMachine& fm)
{
	double t = LiveT();
	// state at the playhead
	std::string current = fm.current;
	double enteredAt = fm.enteredAt;
	if (ui.playhead >= 0)
	{
		current.clear();
		for (auto& [when, value] : fm.timeline)
		{
			if (when > t) break;
			current = value;
			enteredAt = when;
		}
	}
	std::vector<GraphNode> nodes;
	std::vector<GraphLink> links;
	std::map<std::string, int> index;
	for (auto& [value, st] : fm.states)
	{
		GraphNode n;
		n.id = value;
		n.title = value;
		n.accent = fm.kind == "bool" ? (value == "true" ? col::Pass : col::Muted) : col::Accent;
		double total = st.total + (value == current && ui.playhead < 0 ? t - enteredAt : 0);
		n.rows.push_back({ "진입", Fmt("%d회", st.enters) });
		n.rows.push_back({ "누적", Duration(total) });
		n.rows.push_back({ "평균", st.enters ? Duration(total / st.enters) : "-" });
		n.active = value == current;
		if (n.active)
		{
			double in = t - enteredAt;
			double avg = st.enters ? ImMax(0.5, total / st.enters) : 5.0;
			n.progress = (float)ImClamp(in / ImMax(avg * 1.5, 1.0), 0.0, 1.0);
			n.footer = "현재 상태 · " + Duration(in);
		}
		index[value] = (int)nodes.size();
		nodes.push_back(std::move(n));
	}
	for (auto& [edge, st] : fm.edges)
	{
		auto a = index.find(edge.first), b = index.find(edge.second);
		if (a == index.end() || b == index.end()) continue;
		GraphLink l;
		l.from = a->second;
		l.to = b->second;
		l.label = Fmt("%d회", st.count);
		l.weight = (float)st.count;
		l.flash = ui.playhead < 0 ? Recency(st.last, 1.2) : 0;
		links.push_back(l);
	}
	ui.machineGraph.rowLimit = 4;
	ui.machineGraph.horizontal = true;
	ui.machineGraph.circular = true;
	ui.machineGraph.follow = ui.liveFollowGraph;
	ui.machineGraph.Set(std::move(nodes), std::move(links));
}

// ============================================================================ main

static void LiveGuide(App& app)
{
	ImGui::Spacing();
	ImVec2 avail = ImGui::GetContentRegionAvail();
	float width = ImMin(avail.x - S(40), S(620));
	ImGui::SetCursorPosX((avail.x - width) * 0.5f);
	ImGui::BeginChild("guide", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
	Title(ICON_EYE "  State Monitor 시작하기");
	TextDim("실행 중인 게임의 상태를 이 창에서 실시간으로 봅니다. 프로젝트 코드는 필요 없습니다.");
	ImGui::Spacing();
	auto step = [&](int n, bool done, const char* title, const char* body) {
		ImGui::PushID(n);
		TextColored(done ? col::Pass : col::Accent, done ? ICON_OK : Fmt("%d", n).c_str());
		ImGui::SameLine();
		TextBold(title);
		ImGui::Indent(S(24));
		ImGui::PushTextWrapPos(0);
		TextDim(body);
		ImGui::PopTextWrapPos();
		ImGui::PopID();
	};
	step(1, app.stateMonitor != 0, "StateMonitor 설치", "도구 화면이나 아래 버튼으로 설치합니다. 기본 설치는 에디터·PIE 전용이라 패키지 빌드에 영향이 없습니다.");
	ImGui::BeginDisabled(!app.attached || app.stateMonitor != 0 || app.runner.running);
	if (ImGui::Button("설치 (에디터·PIE 전용)")) app.Run("sm-install");
	ImGui::EndDisabled();
	ImGui::Unindent(S(24));
	ImGui::Spacing();
	std::string howTo = Fmt("에디터에서 Play(PIE) 하거나, 아래 버튼으로 게임 창을 띄웁니다. 게임이 시작되면 이 창(127.0.0.1:%d)으로 자동 연결합니다.", app.live.port);
	if (app.live.port != 47731) howTo += Fmt(" 기본 포트가 아니므로 게임을 -StateMonitorPort=%d로 실행하세요.", app.live.port);
	step(2, false, "게임 실행", howTo.c_str());
	ImGui::BeginDisabled(!app.attached || app.stateMonitor == 0);
	if (PrimaryButton((std::string(ICON_PLAY) + "  게임 실행 (play)").c_str())) app.Run("play");
	ImGui::EndDisabled();
	Tooltip("UnrealEditor로 프로젝트를 -game 모드로 띄웁니다.");
	ImGui::Unindent(S(24));
	ImGui::Spacing();
	step(3, false, "상태 보기", "GameMode, GameState, 플레이어, Pawn과 컴포넌트가 노드로 나타나고 값이 바뀌면 깜빡입니다. 열거형·bool 필드는 '상태 전이'에서 상태 머신으로 봅니다.");
	ImGui::Unindent(S(24));
	ImGui::Spacing();
	step(4, false, "원하는 대상 추가 (선택)", "왼쪽 '관찰할 클래스'에 BP_Enemy_C 같은 클래스 이름을 넣거나, Blueprint에서 Watch State Object / Submit State Snapshot 노드를 호출합니다.");
	ImGui::Unindent(S(24));
	ImGui::Spacing();
	if (ImGui::Button((std::string(ICON_OPEN) + "  녹화 파일 열기...").c_str()))
	{
		fs::path p;
		if (PickOpen(g_hwnd, false, L"State Monitor 녹화 열기", p, L"JSON Lines", L"*.jsonl"))
			PushToast("녹화를 열었습니다", Fmt("메시지 %d개", app.store.Import(p)), col::Info);
	}
	ImGui::EndChild();
}

static void DrawLiveMain(App& app)
{
	LiveSession* s = LiveCurrentSession(app);
	ViewHeader(ICON_EYE, "State Monitor", s ? s->name : "연결 대기");
	// toolbar
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(380));
	bool live = ui.playhead < 0;
	if (live) { StatusDot(col::Fail, 4, true); ImGui::SameLine(); TextColored(col::Fail, "LIVE"); }
	else
	{
		if (ImGui::Button((std::string(ICON_PLAY) + " 실시간으로").c_str())) ui.playhead = -1;
		Tooltip("재생 위치를 최신으로 되돌립니다.");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!app.attached || app.stateMonitor == 0);
	if (ImGui::Button((std::string(ICON_GAME) + " 게임 실행").c_str())) app.Run("play");
	ImGui::EndDisabled();
	Tooltip("프로젝트를 -game 모드로 띄웁니다. StateMonitor가 설치되어 있어야 합니다.");
	ImGui::SameLine();
	if (ToolButton(ICON_SAVE, "녹화 저장 (JSON Lines)", false, !app.store.recording.empty()))
	{
		fs::path p;
		if (PickSaveFile(g_hwnd, ToWide(app.projectName + "_state.jsonl"), L"JSON Lines", L"*.jsonl", L"jsonl", p))
			PushToast(app.store.Export(p) ? "녹화 저장됨" : "저장 실패", PathUtf8(p), col::Pass);
	}
	ImGui::SameLine();
	if (ToolButton(ICON_OPEN, "녹화 열기"))
	{
		fs::path p;
		if (PickOpen(g_hwnd, false, L"State Monitor 녹화 열기", p, L"JSON Lines", L"*.jsonl")) app.store.Import(p);
	}
	ImGui::SameLine();
	if (ToolButton(ICON_DELETE, "지우기 (연결은 유지)")) { app.store.Clear(); ui.liveSource.clear(); ui.playhead = -1; ui.liveGraph.Clear(); ui.machineGraph.Clear(); }
	ImGui::SameLine();
	if (ToolButton(ICON_REFRESH, "게임에 전체 상태 다시 요청")) app.live.Broadcast({ { "t", "resync" } });

	if (!s || s->sources.empty())
	{
		LiveGuide(app);
		return;
	}
	// mode switch
	const char* modes[] = { ICON_GRAPH "  객체 그래프", ICON_SYNC "  상태 전이" };
	Segmented("livemode", modes, 2, ui.liveMode);
	ImGui::SameLine(0, S(20));
	GraphView& g = ui.liveMode == 0 ? ui.liveGraph : ui.machineGraph;
	if (ToolButton(ICON_REFRESH, "자동 배치")) g.RequestLayout();
	ImGui::SameLine();
	if (ToolButton(ICON_SEARCH, "전체 보기 (F)")) g.RequestFrame();
	ImGui::SameLine();
	if (ToolButton(ICON_PIN, "현재 상태 따라가기", ui.liveFollowGraph)) ui.liveFollowGraph = !ui.liveFollowGraph;
	ImGui::SameLine();
	if (ToolButton(ICON_GRAPH, "미니맵", g.showMinimap)) g.showMinimap = !g.showMinimap;

	if (ui.liveMode == 0)
	{
		ImGui::SameLine();
		TextDim("실선: 소유  ·  점선: 참조  ·  노란 테두리: 방금 바뀜");
		BuildObjectGraph(app, *s);
		int clicked = ui.liveGraph.Draw("##livegraph", ImVec2(0, 0));
		if (clicked >= 0) { ui.liveSource = ui.liveGraph.nodes[clicked].id; }
		return;
	}
	// state machine
	auto srcIt = s->sources.find(ui.liveSource);
	if (srcIt == s->sources.end())
	{
		EmptyState(ICON_GRAPH, "상태 소스를 고르세요", "왼쪽 목록이나 객체 그래프에서 소스를 고르면 열거형·bool·이름 필드의 상태 전이를 노드 그래프로 보여 줍니다.");
		return;
	}
	auto machines = MachinesOf(srcIt->second);
	if (machines.empty())
	{
		EmptyState(ICON_GRAPH, "상태로 볼 필드가 없습니다", "열거형, bool, 이름, 작은 정수 필드가 있어야 상태 머신으로 그릴 수 있습니다.");
		return;
	}
	const FieldMachine* fm = nullptr;
	for (auto* m : machines) if (m->path == ui.liveField) fm = m;
	if (!fm) { fm = machines.front(); ui.liveField = fm->path; }
	ImGui::SameLine(0, S(20));
	ImGui::SetNextItemWidth(S(260));
	if (ImGui::BeginCombo("##field", (srcIt->second.label + "." + fm->path).c_str()))
	{
		for (auto* m : machines)
			if (ImGui::Selectable(Fmt("%s  (상태 %d · 전이 %d)", m->path.c_str(), (int)m->states.size(), m->transitions).c_str(), m == fm))
			{
				ui.liveField = m->path;
				ui.machineGraph.Clear();
			}
		ImGui::EndCombo();
	}
	std::string key = std::to_string(s->key) + "|" + ui.liveSource + "|" + ui.liveField;
	if (key != ui.liveGraphKey) { ui.machineGraph.Clear(); ui.liveGraphKey = key; }
	BuildMachineGraph(app, srcIt->second, *fm);
	ImGui::SameLine();
	TextDim(Fmt("상태 %d · 전이 %d", (int)fm->states.size(), fm->transitions));
	ui.machineGraph.Draw("##machine", ImVec2(0, 0));
}

// ============================================================================ inspector

static void PropertyTree(const json& v, const std::string& prefix, const LiveSource& src, const json* previous)
{
	if (v.is_object())
	{
		for (auto it = v.begin(); it != v.end(); ++it)
		{
			std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
			const json* prev = previous && previous->is_object() && previous->contains(it.key()) ? &(*previous)[it.key()] : nullptr;
			const json& child = it.value();
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(path.c_str());
			auto ch = src.changedAt.find(path);
			float flash = ch != src.changedAt.end() ? Recency(ch->second, 2.0) : 0;
			if (flash > 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, WithAlpha(col::Warn, 0.25f * flash));
			if (child.is_object() || (child.is_array() && !child.empty()))
			{
				bool open = ImGui::TreeNodeEx(it.key().c_str(), ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen, "%s", it.key().c_str());
				ImGui::TableNextColumn();
				TextDim(child.is_array() ? Fmt("[%d]", (int)child.size()) : "{…}");
				if (open)
				{
					PropertyTree(child, path, src, prev);
					ImGui::TreePop();
				}
			}
			else
			{
				bool selected = ui.inspectorPath == path;
				if (ImGui::TreeNodeEx(it.key().c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAllColumns |
				                                         (selected ? ImGuiTreeNodeFlags_Selected : 0), "%s", it.key().c_str()), ImGui::IsItemClicked())
				{
					ui.inspectorPath = path;
					if (src.machines.count(path) && src.machines.at(path).discrete) { ui.liveField = path; ui.liveMode = 1; }
				}
				ImGui::TableNextColumn();
				std::string full = LeafText(child);
				std::string text = ShortObjectName(full);
				ImU32 c = child.is_boolean() ? (child.get<bool>() ? col::Pass : col::Muted) : child.is_number() ? col::Info : child.is_string() ? col::Text : col::Dim;
				TextColored(c, text.c_str());
				if (prev && *prev != child) Tooltip(("이전 값: " + LeafText(*prev)).c_str());
				else if (text != full) Tooltip(full.c_str());
			}
			ImGui::PopID();
		}
	}
	else if (v.is_array())
	{
		for (size_t i = 0; i < v.size(); ++i)
		{
			std::string path = prefix + "[" + std::to_string(i) + "]";
			const json* prev = previous && previous->is_array() && i < previous->size() ? &(*previous)[i] : nullptr;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID((int)i);
			if (v[i].is_object() || v[i].is_array())
			{
				bool open = ImGui::TreeNodeEx("##e", ImGuiTreeNodeFlags_SpanAllColumns, "[%d]", (int)i);
				ImGui::TableNextColumn();
				TextDim("{…}");
				if (open) { PropertyTree(v[i], path, src, prev); ImGui::TreePop(); }
			}
			else
			{
				ImGui::TreeNodeEx("##e", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAllColumns, "[%d]", (int)i);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(LeafText(v[i]).c_str());
			}
			ImGui::PopID();
		}
	}
}

static void DrawLiveInspector(App& app)
{
	LiveSession* s = LiveCurrentSession(app);
	if (!s) { TextDim("연결된 게임이 없습니다."); return; }
	auto it = s->sources.find(ui.liveSource);
	if (it == s->sources.end())
	{
		SectionHeader("세션");
		KeyValue("이름", s->name, 80);
		KeyValue("월드", Str(s->hello, "world"), 80);
		KeyValue("넷 모드", Str(s->hello, "netMode"), 80);
		KeyValue("엔진", Str(s->hello, "engine"), 80);
		KeyValue("빌드", Str(s->hello, "build"), 80);
		KeyValue("PID", Str(s->hello, "pid"), 80);
		KeyValue("메시지", std::to_string(s->messages), 80);
		KeyValue("상태", s->connected ? "연결됨" : s->offline ? "녹화" : "끊김", 80);
		ImGui::Spacing();
		TextDim("소스를 고르면 현재 값과 변경 이력이 표시됩니다.");
		return;
	}
	LiveSource& src = it->second;
	TextColored(RoleColor(src), RoleIcon(src));
	ImGui::SameLine();
	Title(src.label);
	TextDim(src.type + "  ·  " + (src.kind == "object" ? "객체" : "구조체") + (src.autoSource ? "  ·  자동 관찰" : ""));
	if (!src.object.empty())
	{
		ImGui::PushTextWrapPos(0);
		TextDim(src.object);
		ImGui::PopTextWrapPos();
		if (ImGui::SmallButton((std::string(ICON_COPY) + " 경로 복사").c_str())) CopyToClipboard(src.object);
	}
	if (src.truncated) TextColored(col::Warn, ICON_WARNING " 한도에 걸려 일부 필드가 잘렸습니다.");
	if (ImGui::BeginTabBar("srctabs"))
	{
		if (ImGui::BeginTabItem("값"))
		{
			const json& value = app.store.ValueAt(src, LiveT());
			const json* prev = src.history.size() >= 2 ? &src.history[src.history.size() - 2].value : nullptr;
			if (ImGui::BeginTable("props", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("필드", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn("값", ImGuiTableColumnFlags_WidthStretch, 1.2f);
				ImGui::TableHeadersRow();
				PropertyTree(value, "", src, ui.playhead < 0 ? prev : nullptr);
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(Fmt("변경 %d###hist", src.changeCount).c_str()))
		{
			ImGui::BeginChild("hist");
			for (auto h = src.history.rbegin(); h != src.history.rend(); ++h)
			{
				if (h->changes.empty()) continue;
				ImGui::PushID((int)h->seq);
				bool atPlayhead = ui.playhead >= 0 && std::fabs(ui.playhead - h->t) < 1e-6;
				std::string when = Fmt("%.2f초 전", LiveNow() - h->t);
				if (ImGui::Selectable(when.c_str(), atPlayhead)) ui.playhead = h->t;
				for (auto& c : h->changes)
				{
					ImGui::Indent(S(12));
					ImGui::TextUnformatted(c.path.c_str());
					ImGui::SameLine();
					TextColored(col::Fail, c.oldValue.c_str());
					ImGui::SameLine();
					TextDim("→");
					ImGui::SameLine();
					TextColored(col::Pass, c.newValue.c_str());
					ImGui::Unindent(S(12));
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("상태 필드"))
		{
			for (auto* m : MachinesOf(src))
			{
				ImGui::PushID(m->path.c_str());
				if (ImGui::Selectable(m->path.c_str(), ui.liveField == m->path)) { ui.liveField = m->path; ui.liveMode = 1; }
				ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(110));
				TextDim(Fmt("상태 %d · 전이 %d", (int)m->states.size(), m->transitions));
				ImGui::PopID();
			}
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("JSON"))
		{
			std::string text = app.store.ValueAt(src, LiveT()).dump(2);
			if (ImGui::SmallButton((std::string(ICON_COPY) + " 복사").c_str())) CopyToClipboard(text);
			ImGui::PushFont(g_fonts.mono);
			ImGui::InputTextMultiline("##json", text.data(), text.size() + 1, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
			ImGui::PopFont();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

// ============================================================================ timeline and events (bottom dock)

static void DrawLiveTimeline(App& app)
{
	LiveSession* s = LiveCurrentSession(app);
	double now = LiveNow();
	double end = ui.playhead >= 0 && ui.timelineEnd > 0 ? ui.timelineEnd : now;
	if (ui.playhead < 0) ui.timelineEnd = now;
	double span = ui.timelineSpan;
	double start = end - span;
	// controls
	bool live = ui.playhead < 0;
	if (ToolButton(live ? ICON_PAUSE : ICON_PLAY, live ? "일시 정지 (재생 위치 고정)" : "실시간으로", !live))
		ui.playhead = live ? now : -1;
	ImGui::SameLine();
	auto step = [&](int dir) {
		if (!s) return;
		double t = LiveT();
		double best = dir < 0 ? -1 : DBL_MAX;
		for (auto& e : app.store.events)
		{
			if (e.session != s->key || e.changes.empty()) continue;
			if (!ui.liveSource.empty() && e.source != ui.liveSource) continue;
			if (dir < 0 && e.t < t - 1e-6) best = ImMax(best, e.t);
			if (dir > 0 && e.t > t + 1e-6) best = ImMin(best, e.t);
		}
		if (best >= 0 && best < DBL_MAX) { ui.playhead = best; ui.timelineEnd = ImMax(ui.timelineEnd, best + span * 0.2); }
	};
	if (ToolButton(u8"", "이전 변경")) step(-1);
	ImGui::SameLine();
	if (ToolButton(u8"", "다음 변경")) step(1);
	ImGui::SameLine();
	TextDim(live ? "실시간" : Fmt("%.2f초 전", now - ui.playhead));
	ImGui::SameLine(0, S(20));
	ImGui::SetNextItemWidth(S(140));
	float spanF = (float)ui.timelineSpan;
	if (ImGui::SliderFloat("##span", &spanF, 5, 600, "범위 %.0f초", ImGuiSliderFlags_Logarithmic)) ui.timelineSpan = spanF;
	ImGui::SameLine();
	TextDim(ui.liveSource.empty() ? "모든 소스" : "선택: " + ui.liveSource);
	if (!s || s->sources.empty())
	{
		TextDim("연결된 게임의 변경이 이곳에 시간순으로 표시됩니다.");
		return;
	}
	// lanes
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 size = ImGui::GetContentRegionAvail();
	float labelW = S(170);
	float laneH = S(20);
	ImGui::InvisibleButton("timeline", size);
	bool hovered = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(p, p + size, true);
	dl->AddRectFilled(p, p + size, IM_COL32(0x19, 0x1A, 0x1D, 255));
	float x0 = p.x + labelW, x1 = p.x + size.x - S(8);
	auto xOf = [&](double t) { return x0 + (float)((t - start) / span) * (x1 - x0); };
	// ticks
	double tickStep = span <= 10 ? 1 : span <= 60 ? 5 : span <= 300 ? 30 : 60;
	for (double t = std::ceil(start / tickStep) * tickStep; t <= end; t += tickStep)
	{
		float x = xOf(t);
		dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + size.y), IM_COL32(255, 255, 255, 10));
		std::string label = Fmt("-%.0fs", now - t);
		dl->AddText(ImVec2(x + 2, p.y + 1), col::Muted, label.c_str());
	}
	float y = p.y + S(18);
	// selected state field lane
	auto srcIt = s->sources.find(ui.liveSource);
	if (srcIt != s->sources.end() && srcIt->second.machines.count(ui.liveField))
	{
		const FieldMachine& fm = srcIt->second.machines.at(ui.liveField);
		dl->AddText(ImVec2(p.x + S(6), y + S(2)), col::Text, ("◆ " + ui.liveField).c_str());
		ImU32 palette[] = { col::Accent, col::Teal, col::Purple, col::Warn, col::Pass, col::Info, col::Minor, col::Fail };
		std::map<std::string, int> colorIndex;
		for (size_t i = 0; i < fm.timeline.size(); ++i)
		{
			double a = fm.timeline[i].first;
			double b = i + 1 < fm.timeline.size() ? fm.timeline[i + 1].first : now;
			if (b < start || a > end) continue;
			int ci = colorIndex.emplace(fm.timeline[i].second, (int)colorIndex.size()).first->second;
			ImVec2 ra(ImMax(xOf(a), x0), y + S(2)), rb(ImMin(xOf(b), x1), y + laneH - S(2));
			dl->AddRectFilled(ra, rb, WithAlpha(palette[ci % 8], 0.55f), S(3));
			if (rb.x - ra.x > S(40))
			{
				dl->PushClipRect(ra, rb, true);
				dl->AddText(ra + ImVec2(S(4), S(1)), col::Text, fm.timeline[i].second.c_str());
				dl->PopClipRect();
			}
		}
		y += laneH + S(2);
	}
	// one lane per source
	for (auto& id : s->order)
	{
		if (y > p.y + size.y) break;
		auto it = s->sources.find(id);
		if (it == s->sources.end()) continue;
		const LiveSource& src = it->second;
		bool sel = id == ui.liveSource;
		if (sel) dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + size.x, y + laneH), WithAlpha(col::Accent, 0.12f));
		dl->AddText(ImVec2(p.x + S(6), y + S(2)), sel ? col::Text : col::Dim, src.label.c_str());
		for (auto& h : src.history)
		{
			if (h.t < start || h.t > end || h.changes.empty()) continue;
			float x = xOf(h.t);
			float hgt = ImMin(laneH - S(4), S(4) + h.changes.size() * S(2));
			dl->AddRectFilled(ImVec2(x - 1.5f, y + (laneH - hgt) * 0.5f), ImVec2(x + 1.5f, y + (laneH + hgt) * 0.5f), RoleColor(src));
		}
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().MousePos.y >= y && ImGui::GetIO().MousePos.y < y + laneH &&
		    ImGui::GetIO().MousePos.x < x0)
			ui.liveSource = id;
		y += laneH;
	}
	// playhead
	double ph = LiveT();
	float px = xOf(ph);
	dl->AddLine(ImVec2(px, p.y), ImVec2(px, p.y + size.y), col::Fail, S(1.5f));
	dl->AddTriangleFilled(ImVec2(px - S(5), p.y), ImVec2(px + S(5), p.y), ImVec2(px, p.y + S(7)), col::Fail);
	dl->PopClipRect();
	ImGuiIO& io = ImGui::GetIO();
	if (ImGui::IsItemActive() && io.MousePos.x >= x0)
	{
		double t = start + (io.MousePos.x - x0) / (x1 - x0) * span;
		ui.playhead = ImClamp(t, app.store.minT, now);
		if (ui.timelineEnd < 0) ui.timelineEnd = now;
	}
	if (hovered && io.MouseWheel != 0) ui.timelineSpan = ImClamp(ui.timelineSpan * (io.MouseWheel > 0 ? 0.8 : 1.25), 2.0, 3600.0);
}

static void DrawLiveEvents(App& app)
{
	LiveSession* s = LiveCurrentSession(app);
	static char filter[128] = {};
	SearchBox("##evfilter", filter, sizeof(filter), ICON_SEARCH "  소스, 필드, 값", S(260));
	ImGui::SameLine();
	TextDim(Fmt("이벤트 %d", (int)app.store.events.size()));
	if (!s) return;
	if (ImGui::BeginTable("events", 4, kTable))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("시각", ImGuiTableColumnFlags_WidthFixed, S(80));
		ImGui::TableSetupColumn("소스", ImGuiTableColumnFlags_WidthFixed, S(160));
		ImGui::TableSetupColumn("필드", ImGuiTableColumnFlags_WidthStretch, 1);
		ImGui::TableSetupColumn("변경", ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableHeadersRow();
		std::vector<std::pair<const LiveEvent*, const LiveChange*>> rows;
		for (auto it = app.store.events.rbegin(); it != app.store.events.rend() && rows.size() < 5000; ++it)
		{
			if (it->session != s->key) continue;
			if (it->changes.empty())
			{
				if (ContainsNoCase(it->source, filter)) rows.push_back({ &*it, nullptr });
				continue;
			}
			for (auto& c : it->changes)
				if (ContainsNoCase(it->source + " " + c.path + " " + c.newValue + " " + c.oldValue, filter)) rows.push_back({ &*it, &c });
		}
		ImGuiListClipper clip;
		clip.Begin((int)rows.size());
		double now = LiveNow();
		while (clip.Step())
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
			{
				auto [e, c] = rows[i];
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (ImGui::Selectable(Fmt("-%.1fs", now - e->t).c_str(), ui.playhead >= 0 && std::fabs(ui.playhead - e->t) < 1e-6, ImGuiSelectableFlags_SpanAllColumns))
				{
					ui.playhead = e->t;
					ui.liveSource = e->source;
					if (c && s->sources.count(e->source) && s->sources.at(e->source).machines.count(c->path)) ui.liveField = c->path;
				}
				ImGui::TableNextColumn(); ImGui::TextUnformatted(e->source.c_str());
				ImGui::TableNextColumn();
				if (!c) TextDim(e->added ? "소스 추가" : e->removed ? "소스 제거" : "");
				else ImGui::TextUnformatted(c->path.c_str());
				ImGui::TableNextColumn();
				if (c)
				{
					TextColored(col::Fail, c->oldValue.c_str());
					ImGui::SameLine();
					TextDim("→");
					ImGui::SameLine();
					TextColored(col::Pass, c->newValue.c_str());
				}
				ImGui::PopID();
			}
		ImGui::EndTable();
	}
}
