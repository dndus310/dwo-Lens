// Read-only node graph canvas in the style of Blueprint / state machine editors:
// pan (drag empty space or middle mouse), zoom (wheel), drag nodes, F to frame, minimap, layered auto layout.
#pragma once

#include "ui.h"

struct GraphRow
{
	std::string key, value;
	float flash = 0;         // 0..1, fades a highlight after a change
	ImU32 color = 0;         // optional value color
};

struct GraphNode
{
	std::string id;
	std::string title;
	std::string subtitle;
	std::string icon;
	ImU32 accent = col::Accent;
	std::vector<GraphRow> rows;
	std::string footer;
	bool active = false;     // drawn as the current state
	bool dim = false;        // drawn faded (never visited, disconnected)
	float progress = -1;     // 0..1 bar along the bottom, <0 hides it
	float flash = 0;
	// layout (world units)
	ImVec2 pos{ 0, 0 };
	ImVec2 size{ 0, 0 };
	bool placed = false;
	bool moved = false;      // the user dragged it; keep it where it is
	int layer = 0;
};

struct GraphLink
{
	int from = -1, to = -1;
	std::string label;
	float weight = 1;        // 1..n, drives thickness
	bool dashed = false;
	float flash = 0;
	ImU32 color = 0;
};

class GraphView
{
public:
	std::vector<GraphNode> nodes;
	std::vector<GraphLink> links;
	int selected = -1;
	bool horizontal = true;
	bool circular = false;   // state machines: nodes on a ring, curved transitions
	bool showMinimap = true;
	bool follow = false;     // keep the active node in view
	float rowLimit = 8;

	// Replaces the graph content while keeping positions of nodes that already existed.
	void Set(std::vector<GraphNode> newNodes, std::vector<GraphLink> newLinks)
	{
		std::unordered_map<std::string, const GraphNode*> old;
		for (auto& n : nodes) old[n.id] = &n;
		bool added = false;
		std::string selectedId = selected >= 0 && selected < (int)nodes.size() ? nodes[selected].id : "";
		for (auto& n : newNodes)
		{
			auto it = old.find(n.id);
			if (it != old.end() && it->second->placed)
			{
				n.pos = it->second->pos;
				n.placed = true;
				n.moved = it->second->moved;
				n.layer = it->second->layer;
			}
			else added = true;
		}
		bool removed = newNodes.size() < nodes.size();
		nodes = std::move(newNodes);
		links = std::move(newLinks);
		selected = -1;
		for (int i = 0; i < (int)nodes.size(); ++i)
			if (nodes[i].id == selectedId) selected = i;
		if (added || removed) layoutDirty = true;
		if (firstContent && !nodes.empty()) { needFrame = true; firstContent = false; }
	}

	void Clear()
	{
		interacted = false;
		nodes.clear();
		links.clear();
		selected = -1;
		firstContent = true;
	}

	void RequestLayout(bool reframe = true)
	{
		interacted = false;
		for (auto& n : nodes) n.moved = false;
		layoutDirty = true;
		if (reframe) needFrame = true;
	}

	void RequestFrame() { needFrame = true; }

	// Draws the canvas filling `size`. Returns the index of a node that was clicked this frame, or -1.
	int Draw(const char* id, ImVec2 size)
	{
		ImGuiIO& io = ImGui::GetIO();
		if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
		if (size.y <= 0) size.y = ImGui::GetContentRegionAvail().y;
		size.x = ImMax(size.x, 50.f);
		size.y = ImMax(size.y, 50.f);
		ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
		bool hoveredCanvas = ImGui::IsItemHovered();
		bool activeCanvas = ImGui::IsItemActive();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->PushClipRect(origin, origin + size, true);
		dl->AddRectFilled(origin, origin + size, IM_COL32(0x19, 0x1A, 0x1D, 255));

		Measure();
		if (layoutDirty) { Layout(); layoutDirty = false; }
		// keep everything in view until the user takes over (dock sizes settle over the first frames)
		if (!interacted && (ImAbs(size.x - lastSize.x) > 1 || ImAbs(size.y - lastSize.y) > 1)) needFrame = true;
		lastSize = size;
		if (needFrame && !nodes.empty()) { Frame(size); needFrame = false; }

		// grid
		float step = 24.0f * zoom * g_scale;
		if (step > 6)
		{
			ImVec2 off(fmodf(pan.x, step), fmodf(pan.y, step));
			for (float x = off.x; x < size.x; x += step) dl->AddLine(origin + ImVec2(x, 0), origin + ImVec2(x, size.y), IM_COL32(255, 255, 255, 8));
			for (float y = off.y; y < size.y; y += step) dl->AddLine(origin + ImVec2(0, y), origin + ImVec2(size.x, y), IM_COL32(255, 255, 255, 8));
		}

		auto toScreen = [&](ImVec2 w) { return origin + pan + w * zoom * g_scale; };
		hovered = -1;
		ImVec2 mouse = io.MousePos;
		for (int i = (int)nodes.size() - 1; i >= 0 && hoveredCanvas; --i)
		{
			ImVec2 a = toScreen(nodes[i].pos), b = toScreen(nodes[i].pos + nodes[i].size);
			if (mouse.x >= a.x && mouse.y >= a.y && mouse.x <= b.x && mouse.y <= b.y) { hovered = i; break; }
		}

		// minimap hit test first so it does not start a node drag
		ImVec2 mmA, mmB;
		bool overMinimap = false;
		if (showMinimap && !nodes.empty())
		{
			mmB = origin + size - ImVec2(S(10), S(10));
			mmA = mmB - ImVec2(S(190), S(120));
			overMinimap = hoveredCanvas && mouse.x >= mmA.x && mouse.y >= mmA.y && mouse.x <= mmB.x && mouse.y <= mmB.y;
		}

		int clicked = -1;
		if (ImGui::IsItemActivated()) interacted = true;
		if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			if (overMinimap) dragMode = 3;
			else if (hovered >= 0) { dragMode = 1; dragNode = hovered; selected = hovered; clicked = hovered; dragDistance = 0; }
			else { dragMode = 2; selected = -1; }
		}
		if (ImGui::IsItemActivated() && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
			dragMode = 2;
		if (activeCanvas)
		{
			if (dragMode == 1 && dragNode >= 0 && dragNode < (int)nodes.size() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2))
			{
				nodes[dragNode].pos += io.MouseDelta / (zoom * g_scale);
				nodes[dragNode].moved = true;
			}
			else if (dragMode == 2)
				pan += io.MouseDelta;
		}
		else dragMode = 0;
		if (hoveredCanvas && io.MouseWheel != 0 && !overMinimap)
		{
			interacted = true;
			float old = zoom;
			zoom = ImClamp(zoom * (io.MouseWheel > 0 ? 1.12f : 1 / 1.12f), 0.2f, 2.5f);
			ImVec2 local = mouse - origin - pan;
			pan -= local * (zoom / old - 1.0f);
		}
		if (hoveredCanvas && ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.WantTextInput) Frame(size);
		if (follow && interacted)
			for (auto& n : nodes)
				if (n.active)
				{
					ImVec2 c = toScreen(n.pos + n.size * 0.5f);
					if (c.x < origin.x + size.x * 0.15f || c.x > origin.x + size.x * 0.85f || c.y < origin.y + size.y * 0.15f || c.y > origin.y + size.y * 0.85f)
						pan += ((origin + size * 0.5f) - c) * 0.12f;
				}

		// links
		for (auto& l : links)
		{
			if (l.from < 0 || l.to < 0 || l.from >= (int)nodes.size() || l.to >= (int)nodes.size()) continue;
			DrawLink(dl, l, toScreen);
		}
		// nodes
		for (int i = 0; i < (int)nodes.size(); ++i) DrawNode(dl, i, toScreen);

		if (showMinimap && !nodes.empty()) DrawMinimap(dl, origin, size, mmA, mmB, overMinimap);

		// overlay help
		char zoomText[32];
		snprintf(zoomText, sizeof(zoomText), "%d%%", (int)(zoom * 100));
		dl->AddText(origin + ImVec2(S(10), size.y - S(22)), col::Muted, zoomText);
		dl->PopClipRect();

		if (hovered >= 0 && dragMode == 0 && !nodes[hovered].rows.empty() && zoom < 0.55f)
		{
			ImGui::BeginTooltip();
			TextBold(nodes[hovered].title);
			for (auto& r : nodes[hovered].rows) ImGui::Text("%s  %s", r.key.c_str(), r.value.c_str());
			ImGui::EndTooltip();
		}
		return clicked;
	}

	float zoom = 1.0f;
	ImVec2 pan{ 40, 40 };
	int hovered = -1;

private:
	bool layoutDirty = true;
	bool needFrame = true;
	bool interacted = false;
	ImVec2 lastSize{ 0, 0 };
	bool firstContent = true;
	int dragMode = 0;   // 1 node, 2 pan, 3 minimap
	int dragNode = -1;
	float dragDistance = 0;

	static constexpr float kNodeWidth = 230;
	static constexpr float kHeader = 30;
	static constexpr float kRow = 19;

	void Measure()
	{
		for (auto& n : nodes)
		{
			float width = kNodeWidth;
			ImVec2 t = g_fonts.bold->CalcTextSizeA(g_fonts.bold->FontSize, FLT_MAX, 0, n.title.c_str()) / g_scale;
			width = ImMax(width, t.x + 40);
			for (auto& r : n.rows)
			{
				ImVec2 k = g_fonts.body->CalcTextSizeA(g_fonts.body->FontSize * 0.9f, FLT_MAX, 0, r.key.c_str()) / g_scale;
				ImVec2 v = g_fonts.body->CalcTextSizeA(g_fonts.body->FontSize * 0.9f, FLT_MAX, 0, r.value.c_str()) / g_scale;
				width = ImMax(width, ImMin(k.x + v.x + 40, 380.f));
			}
			int rows = (int)ImMin((float)n.rows.size(), rowLimit);
			float h = kHeader + (n.subtitle.empty() ? 0 : 16) + rows * kRow + (n.rows.size() > (size_t)rowLimit ? kRow : 0) +
			          (n.footer.empty() ? 0 : 20) + (n.progress >= 0 ? 8 : 0) + 10;
			n.size = ImVec2(width, h);
		}
	}

	// Layered layout: longest-path layers on the DAG left after dropping DFS back edges, then barycenter ordering.
	void Layout()
	{
		int n = (int)nodes.size();
		if (n == 0) return;
		if (circular)
		{
			float maxW = 0, maxH = 0;
			for (auto& node : nodes) { maxW = ImMax(maxW, node.size.x); maxH = ImMax(maxH, node.size.y); }
			float rx = n <= 1 ? 0 : ImMax(maxW * 0.9f, n * (maxW + 60) / (2 * IM_PI));
			float ry = n <= 1 ? 0 : ImMax(maxH * 1.2f, rx * 0.62f);
			for (int i = 0; i < n; ++i)
			{
				GraphNode& node = nodes[i];
				if (node.moved) continue;
				float a = -IM_PI * 0.5f + 2 * IM_PI * i / n;
				node.pos = ImVec2(cosf(a) * rx, sinf(a) * ry) - node.size * 0.5f;
				node.placed = true;
			}
			return;
		}
		std::vector<std::vector<int>> out(n), in(n);
		for (auto& l : links)
			if (l.from >= 0 && l.to >= 0 && l.from < n && l.to < n && l.from != l.to) out[l.from].push_back(l.to);
		// drop back edges
		std::vector<int> state(n, 0);
		std::vector<std::vector<int>> dag(n);
		for (int root = 0; root < n; ++root)
		{
			if (state[root]) continue;
			std::vector<std::pair<int, size_t>> stack{ { root, 0 } };
			state[root] = 1;
			while (!stack.empty())
			{
				auto& [v, i] = stack.back();
				if (i < out[v].size())
				{
					int w = out[v][i++];
					if (state[w] == 1) continue;   // back edge
					dag[v].push_back(w);
					if (state[w] == 0) { state[w] = 1; stack.push_back({ w, 0 }); }
				}
				else { state[v] = 2; stack.pop_back(); }
			}
		}
		for (int v = 0; v < n; ++v)
			for (int w : dag[v]) in[w].push_back(v);
		// longest path layering (Kahn)
		std::vector<int> indeg(n), layer(n, 0), order;
		for (int v = 0; v < n; ++v) indeg[v] = (int)in[v].size();
		std::deque<int> q;
		for (int v = 0; v < n; ++v)
			if (!indeg[v]) q.push_back(v);
		while (!q.empty())
		{
			int v = q.front();
			q.pop_front();
			order.push_back(v);
			for (int w : dag[v])
			{
				layer[w] = ImMax(layer[w], layer[v] + 1);
				if (--indeg[w] == 0) q.push_back(w);
			}
		}
		int layers = 0;
		for (int v = 0; v < n; ++v) layers = ImMax(layers, layer[v] + 1);
		std::vector<std::vector<int>> rows(layers);
		for (int v : order) rows[layer[v]].push_back(v);
		std::vector<float> rank(n, 0);
		auto assign = [&](int l) { for (int i = 0; i < (int)rows[l].size(); ++i) rank[rows[l][i]] = (float)i; };
		for (int l = 0; l < layers; ++l) assign(l);
		for (int sweep = 0; sweep < 6; ++sweep)
		{
			bool down = sweep % 2 == 0;
			for (int k = 1; k < layers; ++k)
			{
				int l = down ? k : layers - 1 - k;
				auto& ref = down ? in : dag;
				std::vector<std::pair<float, int>> keyed;
				for (int v : rows[l])
				{
					float sum = 0;
					int count = 0;
					for (int u : ref[v]) { sum += rank[u]; ++count; }
					keyed.push_back({ count ? sum / count : rank[v], v });
				}
				std::stable_sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first < b.first; });
				for (int i = 0; i < (int)keyed.size(); ++i) rows[l][i] = keyed[i].second;
				assign(l);
			}
		}
		// coordinates
		float gapMain = 90, gapCross = 28;
		float cursorMain = 0;
		std::vector<float> extentCross(layers, 0);
		for (int l = 0; l < layers; ++l)
			for (int v : rows[l]) extentCross[l] += (horizontal ? nodes[v].size.y : nodes[v].size.x) + gapCross;
		float maxCross = *std::max_element(extentCross.begin(), extentCross.end());
		for (int l = 0; l < layers; ++l)
		{
			float thick = 0;
			for (int v : rows[l]) thick = ImMax(thick, horizontal ? nodes[v].size.x : nodes[v].size.y);
			float c = (maxCross - extentCross[l]) * 0.5f;
			for (int v : rows[l])
			{
				GraphNode& node = nodes[v];
				node.layer = l;
				if (!node.moved)
				{
					node.pos = horizontal ? ImVec2(cursorMain, c) : ImVec2(c, cursorMain);
					node.placed = true;
				}
				c += (horizontal ? node.size.y : node.size.x) + gapCross;
			}
			cursorMain += thick + gapMain;
		}
	}

	void Frame(ImVec2 size)
	{
		if (nodes.empty()) return;
		ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
		for (auto& n : nodes)
		{
			lo = ImMin(lo, n.pos);
			hi = ImMax(hi, n.pos + n.size);
		}
		ImVec2 extent = (hi - lo) * g_scale;
		float z = ImMin((size.x - S(80)) / ImMax(extent.x, 1.f), (size.y - S(80)) / ImMax(extent.y, 1.f));
		zoom = ImClamp(z, 0.2f, 1.2f);
		ImVec2 center = (lo + hi) * 0.5f * zoom * g_scale;
		pan = size * 0.5f - center;
	}

	static ImVec2 Bezier(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, float t)
	{
		float u = 1 - t;
		return a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + d * (t * t * t);
	}

	template <typename F> void DrawLink(ImDrawList* dl, const GraphLink& l, F toScreen)
	{
		const GraphNode& A = nodes[l.from];
		const GraphNode& B = nodes[l.to];
		float z = zoom * g_scale;
		ImU32 color = l.color ? l.color : IM_COL32(0x9A, 0xA0, 0xA8, 200);
		bool related = selected >= 0 && (l.from == selected || l.to == selected);
		if (selected >= 0 && !related) color = WithAlpha(color, 0.25f);
		if (l.flash > 0) color = Mix(color, col::Pass, l.flash);
		float thick = (1.4f + ImMin(l.weight - 1, 6.f) * 0.7f) * z + (related ? 0.8f * z : 0);
		ImVec2 p0, p1, p2, p3;
		if (circular && l.from != l.to)
		{
			ImVec2 ca = toScreen(A.pos + A.size * 0.5f), cb = toScreen(B.pos + B.size * 0.5f);
			ImVec2 d = cb - ca;
			float len = ImMax(sqrtf(d.x * d.x + d.y * d.y), 1.f);
			d /= len;
			auto edge = [&](ImVec2 c, ImVec2 half, ImVec2 dir) {
				float tx = ImAbs(dir.x) > 1e-4f ? half.x / ImAbs(dir.x) : FLT_MAX;
				float ty = ImAbs(dir.y) > 1e-4f ? half.y / ImAbs(dir.y) : FLT_MAX;
				return c + dir * ImMin(tx, ty);
			};
			ImVec2 nrm(-d.y, d.x);
			float bend = ImMin(60.f * z, len * 0.25f);
			p0 = edge(ca, A.size * 0.5f * z, d) + nrm * 8 * z;
			p3 = edge(cb, B.size * 0.5f * z, d * -1.0f) + nrm * 8 * z;
			p1 = p0 + (p3 - p0) * 0.33f + nrm * bend;
			p2 = p0 + (p3 - p0) * 0.66f + nrm * bend;
		}
		else if (l.from == l.to)
		{
			ImVec2 top = toScreen(A.pos + ImVec2(A.size.x * 0.5f, 0));
			ImVec2 r(30 * z, 0);
			p0 = top - r * 0.5f;
			p3 = top + r * 0.5f;
			p1 = p0 + ImVec2(-20 * z, -50 * z);
			p2 = p3 + ImVec2(20 * z, -50 * z);
		}
		else if (horizontal)
		{
			bool back = B.pos.x <= A.pos.x + A.size.x * 0.5f;
			if (!back)
			{
				p0 = toScreen(A.pos + ImVec2(A.size.x, A.size.y * 0.5f));
				p3 = toScreen(B.pos + ImVec2(0, B.size.y * 0.5f));
				float dx = ImMax(40 * z, (p3.x - p0.x) * 0.5f);
				p1 = p0 + ImVec2(dx, 0);
				p2 = p3 - ImVec2(dx, 0);
			}
			else
			{
				// return edge: leave from the bottom, arrive at the bottom
				p0 = toScreen(A.pos + ImVec2(A.size.x * 0.5f, A.size.y));
				p3 = toScreen(B.pos + ImVec2(B.size.x * 0.5f, B.size.y));
				float dy = 70 * z + ImAbs(p0.x - p3.x) * 0.15f;
				p1 = p0 + ImVec2(0, dy);
				p2 = p3 + ImVec2(0, dy);
			}
		}
		else
		{
			p0 = toScreen(A.pos + ImVec2(A.size.x * 0.5f, A.size.y));
			p3 = toScreen(B.pos + ImVec2(B.size.x * 0.5f, 0));
			float dy = ImMax(40 * z, (p3.y - p0.y) * 0.5f);
			p1 = p0 + ImVec2(0, dy);
			p2 = p3 - ImVec2(0, dy);
		}
		if (l.flash > 0)
			dl->AddBezierCubic(p0, p1, p2, p3, WithAlpha(col::Pass, 0.35f * l.flash), thick + 6 * z);
		if (l.dashed)
		{
			const int segs = 32;
			for (int i = 0; i < segs; i += 2)
				dl->AddLine(Bezier(p0, p1, p2, p3, (float)i / segs), Bezier(p0, p1, p2, p3, (float)(i + 1) / segs), color, thick);
		}
		else dl->AddBezierCubic(p0, p1, p2, p3, color, thick);
		// arrow head
		ImVec2 tip = p3, prev = Bezier(p0, p1, p2, p3, 0.96f);
		ImVec2 dir = tip - prev;
		float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
		if (len > 0.001f)
		{
			dir /= len;
			ImVec2 nrm(-dir.y, dir.x);
			float s = (7 + thick) * 1.0f;
			dl->AddTriangleFilled(tip, tip - dir * s + nrm * s * 0.5f, tip - dir * s - nrm * s * 0.5f, color);
		}
		if (!l.label.empty() && z > 0.35f)
		{
			ImVec2 mid = Bezier(p0, p1, p2, p3, 0.5f);
			float fs = g_fonts.body->FontSize * 0.82f * ImClamp(zoom, 0.6f, 1.2f);
			ImVec2 ts = g_fonts.body->CalcTextSizeA(fs, FLT_MAX, 0, l.label.c_str());
			ImVec2 a = mid - ts * 0.5f - ImVec2(5, 2), b = mid + ts * 0.5f + ImVec2(5, 2);
			dl->AddRectFilled(a, b, IM_COL32(0x22, 0x24, 0x27, 235), 4);
			dl->AddRect(a, b, WithAlpha(color, 0.6f), 4);
			dl->AddText(g_fonts.body, fs, mid - ts * 0.5f, l.flash > 0 ? col::Pass : col::Text, l.label.c_str());
		}
	}

	template <typename F> void DrawNode(ImDrawList* dl, int i, F toScreen)
	{
		GraphNode& n = nodes[i];
		float z = zoom * g_scale;
		ImVec2 a = toScreen(n.pos), b = toScreen(n.pos + n.size);
		float r = 6 * z;
		bool isSel = i == selected, isHov = i == hovered;
		float alpha = n.dim ? 0.55f : 1.0f;
		if (n.active)
			dl->AddRect(a - ImVec2(3, 3) * z, b + ImVec2(3, 3) * z, WithAlpha(col::Pass, 0.45f + 0.25f * sinf((float)ImGui::GetTime() * 4)), r + 3 * z, 0, 3 * z);
		if (n.flash > 0) dl->AddRect(a - ImVec2(2, 2), b + ImVec2(2, 2), WithAlpha(col::Warn, n.flash), r + 2, 0, 2.5f);
		dl->AddRectFilled(a + ImVec2(0, 3 * z), b + ImVec2(0, 3 * z), IM_COL32(0, 0, 0, 70), r);
		dl->AddRectFilled(a, b, WithAlpha(IM_COL32(0x2A, 0x2C, 0x30, 255), alpha), r);
		ImU32 header = n.active ? Mix(n.accent, col::Pass, 0.6f) : n.accent;
		dl->AddRectFilled(a, ImVec2(b.x, a.y + kHeader * z), WithAlpha(Mix(header, IM_COL32(0x2A, 0x2C, 0x30, 255), 0.45f), alpha), r, ImDrawFlags_RoundCornersTop);
		dl->AddRectFilled(a, ImVec2(a.x + 4 * z, a.y + kHeader * z), WithAlpha(header, alpha), r, ImDrawFlags_RoundCornersTopLeft);
		dl->AddRect(a, b, isSel ? col::Accent : isHov ? IM_COL32(0x80, 0x86, 0x90, 255) : WithAlpha(col::Border, 1.0f), r, 0, isSel ? 2.0f * z : 1.0f);
		if (z < 0.28f) return;

		float fsTitle = g_fonts.bold->FontSize * zoom;
		float fs = g_fonts.body->FontSize * 0.9f * zoom;
		ImVec2 cur = a + ImVec2(10 * z, (kHeader * z - fsTitle) * 0.5f);
		if (!n.icon.empty())
		{
			dl->AddText(g_fonts.body, fsTitle, cur, WithAlpha(col::Text, alpha), n.icon.c_str());
			cur.x += fsTitle + 4 * z;
		}
		dl->PushClipRect(a, b, true);
		dl->AddText(g_fonts.bold, fsTitle, cur, WithAlpha(IM_COL32(255, 255, 255, 255), alpha), n.title.c_str());
		float y = a.y + kHeader * z + 4 * z;
		if (!n.subtitle.empty())
		{
			dl->AddText(g_fonts.body, fs * 0.92f, ImVec2(a.x + 10 * z, y), col::Dim, n.subtitle.c_str());
			y += 16 * z;
		}
		if (zoom >= 0.45f)
		{
			int shown = 0;
			for (auto& row : n.rows)
			{
				if (shown >= (int)rowLimit) break;
				if (row.flash > 0)
					dl->AddRectFilled(ImVec2(a.x + 2, y), ImVec2(b.x - 2, y + kRow * z), WithAlpha(col::Warn, 0.25f * row.flash), 2);
				dl->AddText(g_fonts.body, fs, ImVec2(a.x + 10 * z, y + 1 * z), col::Dim, row.key.c_str());
				ImVec2 vs = g_fonts.body->CalcTextSizeA(fs, FLT_MAX, 0, row.value.c_str());
				float vx = ImMax(b.x - 10 * z - vs.x, a.x + (n.size.x * 0.45f) * z);
				dl->AddText(g_fonts.body, fs, ImVec2(vx, y + 1 * z), row.color ? row.color : col::Text, row.value.c_str());
				y += kRow * z;
				++shown;
			}
			if (n.rows.size() > (size_t)rowLimit)
			{
				std::string more = "+" + std::to_string(n.rows.size() - (size_t)rowLimit) + "개 더";
				dl->AddText(g_fonts.body, fs, ImVec2(a.x + 10 * z, y + 1 * z), col::Muted, more.c_str());
				y += kRow * z;
			}
			if (!n.footer.empty())
				dl->AddText(g_fonts.body, fs * 0.92f, ImVec2(a.x + 10 * z, y + 3 * z), col::Dim, n.footer.c_str());
		}
		dl->PopClipRect();
		if (n.progress >= 0)
		{
			ImVec2 pa(a.x + 6 * z, b.y - 7 * z), pb(b.x - 6 * z, b.y - 4 * z);
			dl->AddRectFilled(pa, pb, IM_COL32(255, 255, 255, 25), 2);
			dl->AddRectFilled(pa, ImVec2(pa.x + (pb.x - pa.x) * ImClamp(n.progress, 0.f, 1.f), pb.y), n.active ? col::Pass : col::Info, 2);
		}
	}

	void DrawMinimap(ImDrawList* dl, ImVec2 origin, ImVec2 size, ImVec2 a, ImVec2 b, bool over)
	{
		ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
		for (auto& n : nodes) { lo = ImMin(lo, n.pos); hi = ImMax(hi, n.pos + n.size); }
		float z = zoom * g_scale;
		// include the visible area
		ImVec2 viewLo = (ImVec2(0, 0) - pan) / z, viewHi = (size - pan) / z;
		ImVec2 allLo = ImMin(lo, viewLo), allHi = ImMax(hi, viewHi);
		ImVec2 ext = allHi - allLo;
		float s = ImMin((b.x - a.x - 10) / ImMax(ext.x, 1.f), (b.y - a.y - 10) / ImMax(ext.y, 1.f));
		ImVec2 off = a + ImVec2(5, 5);
		auto map = [&](ImVec2 w) { return off + (w - allLo) * s; };
		dl->AddRectFilled(a, b, IM_COL32(0x22, 0x24, 0x27, over ? 240 : 200), 4);
		dl->AddRect(a, b, col::Border, 4);
		for (auto& n : nodes)
			dl->AddRectFilled(map(n.pos), map(n.pos + n.size), n.active ? col::Pass : WithAlpha(n.accent, 0.8f), 1);
		dl->AddRect(map(viewLo), map(viewHi), IM_COL32(255, 255, 255, 160), 2);
		if (dragMode == 3 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			ImVec2 world = allLo + (ImGui::GetIO().MousePos - off) / s;
			pan = size * 0.5f - world * z;
		}
	}
};
