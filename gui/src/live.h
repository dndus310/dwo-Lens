// State Monitor live link: a localhost TCP server that games connect to, and the store that keeps
// per-session sources, value history, change events and per-field state machines.
// Protocol: UTF-8 JSON Lines. See plugin/StateMonitor/README.md.
#pragma once

#include "util.h"

// ============================================================================ server

struct LiveMessage
{
	int client = 0;
	double t = 0;
	json body;
	std::string raw;
	bool connected = false;     // synthetic connect / disconnect notices
	bool disconnected = false;
};

class LiveServer
{
public:
	~LiveServer() { Stop(); }

	bool Start(int newPort)
	{
		Stop();
		port = newPort;
		WSADATA wsa;
		if (!wsaReady)
		{
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error = "WSAStartup 실패"; return false; }
			wsaReady = true;
		}
		listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener == INVALID_SOCKET) { error = "소켓을 만들 수 없습니다"; return false; }
		BOOL exclusive = TRUE;
		setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons((u_short)port);
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || listen(listener, 8) == SOCKET_ERROR)
		{
			error = "포트 " + std::to_string(port) + "을(를) 사용할 수 없습니다 (다른 dwo-Lens 창이 열려 있을 수 있습니다)";
			closesocket(listener);
			listener = INVALID_SOCKET;
			return false;
		}
		error.clear();
		listening = true;
		acceptThread = std::thread([this] { AcceptLoop(); });
		return true;
	}

	void Stop()
	{
		listening = false;
		if (listener != INVALID_SOCKET) { closesocket(listener); listener = INVALID_SOCKET; }
		if (acceptThread.joinable()) acceptThread.join();
		std::vector<std::shared_ptr<Client>> all;
		{
			std::lock_guard<std::mutex> lock(clientsMutex);
			all = clients;
			clients.clear();
		}
		for (auto& c : all)
		{
			shutdown(c->sock, SD_BOTH);
			closesocket(c->sock);
			if (c->thread.joinable()) c->thread.join();
		}
	}

	void Drain(std::vector<LiveMessage>& out)
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		for (auto& m : queue) out.push_back(std::move(m));
		queue.clear();
	}

	bool Send(int clientId, const json& message)
	{
		std::string line = message.dump() + "\n";
		std::lock_guard<std::mutex> lock(clientsMutex);
		for (auto& c : clients)
			if (c->id == clientId && c->alive)
			{
				std::lock_guard<std::mutex> sendLock(c->sendMutex);
				size_t sent = 0;
				while (sent < line.size())
				{
					int n = send(c->sock, line.data() + sent, (int)(line.size() - sent), 0);
					if (n <= 0) return false;
					sent += (size_t)n;
				}
				return true;
			}
		return false;
	}

	void Broadcast(const json& message)
	{
		std::vector<int> ids;
		{
			std::lock_guard<std::mutex> lock(clientsMutex);
			for (auto& c : clients)
				if (c->alive) ids.push_back(c->id);
		}
		for (int id : ids) Send(id, message);
	}

	int ConnectedCount()
	{
		std::lock_guard<std::mutex> lock(clientsMutex);
		int n = 0;
		for (auto& c : clients) n += c->alive ? 1 : 0;
		return n;
	}

	std::atomic<bool> listening{ false };
	std::string error;
	int port = 47731;
	std::atomic<long long> bytesIn{ 0 };

private:
	struct Client
	{
		int id = 0;
		SOCKET sock = INVALID_SOCKET;
		std::thread thread;
		std::mutex sendMutex;
		std::atomic<bool> alive{ true };
	};

	void Push(LiveMessage m)
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (queue.size() > 200000) queue.pop_front();
		queue.push_back(std::move(m));
	}

	void AcceptLoop()
	{
		while (listening)
		{
			SOCKET s = accept(listener, nullptr, nullptr);
			if (s == INVALID_SOCKET) break;
			BOOL nodelay = TRUE;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
			auto c = std::make_shared<Client>();
			c->id = ++nextId;
			c->sock = s;
			{
				std::lock_guard<std::mutex> lock(clientsMutex);
				// forget finished clients
				clients.erase(std::remove_if(clients.begin(), clients.end(), [](const std::shared_ptr<Client>& x) {
					if (!x->alive && x->thread.joinable()) { x->thread.join(); return true; }
					return false;
				}), clients.end());
				clients.push_back(c);
			}
			LiveMessage hello;
			hello.client = c->id;
			hello.t = NowSeconds();
			hello.connected = true;
			Push(std::move(hello));
			c->thread = std::thread([this, c] { ReadLoop(c); });
		}
	}

	void ReadLoop(std::shared_ptr<Client> c)
	{
		std::string pending;
		char buffer[16384];
		while (true)
		{
			int n = recv(c->sock, buffer, sizeof(buffer), 0);
			if (n <= 0) break;
			bytesIn += n;
			pending.append(buffer, n);
			size_t pos;
			while ((pos = pending.find('\n')) != std::string::npos)
			{
				std::string line = pending.substr(0, pos);
				pending.erase(0, pos + 1);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (line.empty()) continue;
				LiveMessage m;
				m.client = c->id;
				m.t = NowSeconds();
				try { m.body = json::parse(line); }
				catch (...) { continue; }
				m.raw = std::move(line);
				Push(std::move(m));
			}
			if (pending.size() > 64 * 1024 * 1024) pending.clear();
		}
		c->alive = false;
		closesocket(c->sock);
		LiveMessage bye;
		bye.client = c->id;
		bye.t = NowSeconds();
		bye.disconnected = true;
		Push(std::move(bye));
	}

	bool wsaReady = false;
	SOCKET listener = INVALID_SOCKET;
	std::thread acceptThread;
	std::mutex clientsMutex;
	std::vector<std::shared_ptr<Client>> clients;
	std::mutex queueMutex;
	std::deque<LiveMessage> queue;
	std::atomic<int> nextId{ 0 };
};

// ============================================================================ store

struct LiveChange
{
	std::string path, type, oldValue, newValue, kind;
};

struct LiveEntry
{
	double t = 0;
	long long seq = 0;
	json value;
	std::vector<LiveChange> changes;
};

struct StateStat
{
	int enters = 0;
	double total = 0;
	double enteredAt = -1;
};

struct TransitionStat
{
	int count = 0;
	double last = -1;
};

// Discrete field tracked as a state machine (enum, bool, name, small int).
struct FieldMachine
{
	std::string path;
	std::string kind;           // bool, string, int
	bool discrete = true;
	std::string current;
	double enteredAt = 0;
	int transitions = 0;
	std::map<std::string, StateStat> states;
	std::map<std::pair<std::string, std::string>, TransitionStat> edges;
	std::vector<std::pair<double, std::string>> timeline;   // value changes for swimlanes
	std::string lastFrom, lastTo;
	double lastAt = -1;
};

struct LiveSource
{
	std::string id, label, type, kind, object, owner;
	bool autoSource = false;
	bool removed = false;
	bool truncated = false;
	json value = json::object();
	double firstT = 0, lastT = 0;
	long long seq = 0;
	int changeCount = 0;
	std::deque<LiveEntry> history;
	std::map<std::string, FieldMachine> machines;
	std::map<std::string, double> changedAt;    // leaf path -> time of last change
};

struct LiveSession
{
	int key = 0;
	int client = 0;
	bool connected = false;
	bool offline = false;       // loaded from a recording
	json hello = json::object();
	std::string name;
	double connectedAt = 0, lastAt = 0;
	int messages = 0;
	std::map<std::string, LiveSource> sources;
	std::vector<std::string> order;
};

struct LiveEvent
{
	double t = 0;
	int session = 0;
	std::string source;
	std::vector<LiveChange> changes;
	bool added = false;
	bool removed = false;
};

// Flattens a JSON value into leaf paths ("Stats.Health", "Items[2].Name").
static void FlattenJson(const json& v, const std::string& prefix, std::vector<std::pair<std::string, const json*>>& out, int limit = 600)
{
	if ((int)out.size() >= limit) return;
	if (v.is_object())
	{
		for (auto it = v.begin(); it != v.end(); ++it)
			FlattenJson(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key(), out, limit);
	}
	else if (v.is_array())
	{
		for (size_t i = 0; i < v.size(); ++i) FlattenJson(v[i], prefix + "[" + std::to_string(i) + "]", out, limit);
		if (v.empty()) out.push_back({ prefix, &v });
	}
	else out.push_back({ prefix, &v });
}

// The plugin reports change paths as "Stats/Items/2/Name"; the viewer uses "Stats.Items[2].Name".
static std::string NormalizePath(const std::string& path)
{
	if (path.find('/') == std::string::npos) return path;
	std::string out;
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string::npos) end = path.size();
		std::string part = path.substr(start, end - start);
		bool index = !part.empty() && std::all_of(part.begin(), part.end(), ::isdigit);
		if (index) out += "[" + part + "]";
		else if (!part.empty()) out += (out.empty() ? "" : ".") + part;
		start = end + 1;
	}
	return out;
}

// "/Game/Map.Map:PersistentLevel.BP_Hero_C_0" -> "BP_Hero_C_0"
static std::string ShortObjectName(const std::string& s)
{
	if (s.size() < 2 || s[0] != '/' || s.find(' ') != std::string::npos) return s;
	size_t cut = s.find_last_of(".:");
	return cut == std::string::npos ? s : s.substr(cut + 1);
}

static std::string LeafText(const json& v)
{
	if (v.is_string()) return v.get<std::string>();
	if (v.is_null()) return "null";
	if (v.is_array() && v.empty()) return "[]";
	if (v.is_number_float())
	{
		double d = v.get<double>();
		if (std::fabs(d - std::round(d)) < 1e-9 && std::fabs(d) < 1e12) return Fmt("%.0f", d);
		return Fmt("%.3f", d);
	}
	return v.dump();
}

class LiveStore
{
public:
	std::vector<LiveSession> sessions;
	std::deque<LiveEvent> events;
	std::vector<std::pair<double, std::string>> recording;   // raw lines with receive time
	bool recordingOn = true;
	long long revision = 0;
	double minT = -1, maxT = 0;
	int distinctLimit = 24;
	size_t historyLimit = 4000;

	LiveSession* Find(int key)
	{
		for (auto& s : sessions)
			if (s.key == key) return &s;
		return nullptr;
	}

	LiveSession* ByClient(int client)
	{
		for (auto it = sessions.rbegin(); it != sessions.rend(); ++it)
			if (!it->offline && it->client == client) return &*it;
		return nullptr;
	}

	void Clear()
	{
		sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [](const LiveSession& s) { return !s.connected; }), sessions.end());
		for (auto& s : sessions) { s.sources.clear(); s.order.clear(); s.messages = 0; }
		events.clear();
		recording.clear();
		minT = -1;
		maxT = 0;
		++revision;
	}

	// Returns true if the message was a hello (the caller sends its settings to that client).
	bool Apply(const LiveMessage& m, int offlineSession = 0)
	{
		Touch(m.t);
		if (m.connected)
		{
			LiveSession s;
			s.key = ++nextKey;
			s.client = m.client;
			s.connected = true;
			s.connectedAt = s.lastAt = m.t;
			s.name = "연결 " + std::to_string(m.client);
			sessions.push_back(std::move(s));
			++revision;
			return false;
		}
		if (m.disconnected)
		{
			if (LiveSession* s = ByClient(m.client)) { s->connected = false; s->lastAt = m.t; }
			++revision;
			return false;
		}
		LiveSession* s = offlineSession ? Find(offlineSession) : ByClient(m.client);
		if (!s) return false;
		if (recordingOn && !s->offline && !m.raw.empty())
		{
			recording.push_back({ m.t, m.raw });
			if (recording.size() > 400000) recording.erase(recording.begin(), recording.begin() + 100000);
		}
		s->lastAt = m.t;
		++s->messages;
		const json& b = m.body;
		std::string type = Str(b, "t");
		if (type == "hello")
		{
			s->hello = b;
			std::string world = Str(b, "world");
			std::string nm = Str(b, "project", "게임");
			std::string mode = Str(b, "netMode");
			long long pie = b.value("pie", -1LL);
			s->name = nm + (pie >= 0 ? " · PIE " + std::to_string(pie) : "") + (mode.empty() || mode == "Standalone" ? "" : " · " + mode);
			++revision;
			return true;
		}
		if (type == "world")
		{
			s->hello["world"] = Str(b, "world");
			++revision;
			return false;
		}
		if (type == "remove")
		{
			auto it = s->sources.find(Str(b, "id"));
			if (it != s->sources.end())
			{
				it->second.removed = true;
				LiveEvent e;
				e.t = m.t;
				e.session = s->key;
				e.source = it->first;
				e.removed = true;
				PushEvent(std::move(e));
			}
			++revision;
			return false;
		}
		if (type == "snap") ApplySnap(*s, b, m.t);
		return false;
	}

	// Value of a source at time t (the newest history entry not after t). Falls back to the current value.
	const json& ValueAt(const LiveSource& src, double t) const
	{
		if (t < 0 || src.history.empty() || t >= src.lastT) return src.value;
		auto it = std::upper_bound(src.history.begin(), src.history.end(), t, [](double x, const LiveEntry& e) { return x < e.t; });
		if (it == src.history.begin()) return src.history.front().value;
		return std::prev(it)->value;
	}

	// Saves the raw message log as JSON Lines with the receive time in "rt".
	bool Export(const fs::path& path) const
	{
		std::ofstream out(path, std::ios::binary);
		if (!out) return false;
		for (auto& [t, raw] : recording)
		{
			json j;
			try { j = json::parse(raw); }
			catch (...) { continue; }
			j["rt"] = t;
			out << j.dump() << "\n";
		}
		return (bool)out;
	}

	// Loads a recording as an offline session. Returns the number of messages.
	int Import(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return 0;
		LiveSession s;
		s.key = ++nextKey;
		s.offline = true;
		s.name = PathUtf8(path.stem()) + " (녹화)";
		sessions.push_back(std::move(s));
		int key = sessions.back().key;
		// replay so the recording ends now; the timeline then shows it like a session that just finished
		std::vector<LiveMessage> messages;
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()) continue;
			LiveMessage m;
			try { m.body = json::parse(line); }
			catch (...) { continue; }
			m.t = Real(m.body, "rt", 0);
			messages.push_back(std::move(m));
		}
		double last = messages.empty() ? 0 : messages.back().t;
		double now = NowSeconds();
		double base = messages.empty() ? now : now - (last - messages.front().t);
		for (auto& m : messages)
		{
			m.t = now - (last - m.t);
			Apply(m, key);
		}
		int count = (int)messages.size();
		if (LiveSession* added = Find(key)) added->connectedAt = base;
		++revision;
		return count;
	}

private:
	int nextKey = 0;

	void Touch(double t)
	{
		if (minT < 0 || t < minT) minT = t;
		maxT = ImMax(maxT, t);
	}

	void PushEvent(LiveEvent e)
	{
		events.push_back(std::move(e));
		if (events.size() > 60000) events.erase(events.begin(), events.begin() + 10000);
	}

	void ApplySnap(LiveSession& s, const json& b, double t)
	{
		std::string id = Str(b, "id");
		if (id.empty()) return;
		bool isNew = !s.sources.count(id);
		LiveSource& src = s.sources[id];
		if (isNew)
		{
			src.id = id;
			src.firstT = t;
			s.order.push_back(id);
		}
		src.removed = false;
		src.label = Str(b, "label", id);
		src.type = Str(b, "type");
		src.kind = Str(b, "kind", "struct");
		src.object = Str(b, "object");
		src.owner = Str(b, "owner");
		src.autoSource = b.value("auto", false);
		src.truncated = b.value("truncated", false);
		src.seq = b.value("seq", 0LL);
		src.lastT = t;
		auto vit = b.find("value");
		if (vit != b.end())
		{
			if (vit->is_string())
			{
				try { src.value = json::parse(vit->get<std::string>()); }
				catch (...) { src.value = json::object(); }
			}
			else src.value = *vit;
		}
		LiveEntry entry;
		entry.t = t;
		entry.seq = src.seq;
		entry.value = src.value;
		for (auto& c : Arr(b, "changes"))
		{
			if (Str(c, "path").empty()) continue;   // the root "added" entry of a first snapshot
			LiveChange ch{ NormalizePath(Str(c, "path")), Str(c, "type"), Str(c, "old"), Str(c, "new"), Str(c, "change", "modified") };
			src.changedAt[ch.path] = t;
			entry.changes.push_back(std::move(ch));
		}
		src.changeCount += (int)entry.changes.size();
		UpdateMachines(src, t);
		LiveEvent e;
		e.t = t;
		e.session = s.key;
		e.source = id;
		e.changes = entry.changes;
		e.added = isNew;
		if (isNew || !e.changes.empty()) PushEvent(std::move(e));
		src.history.push_back(std::move(entry));
		if (src.history.size() > historyLimit) src.history.pop_front();
		++revision;
	}

	void UpdateMachines(LiveSource& src, double t)
	{
		std::vector<std::pair<std::string, const json*>> leaves;
		FlattenJson(src.value, "", leaves);
		for (auto& [path, v] : leaves)
		{
			if (path.empty()) continue;
			std::string kind = v->is_boolean() ? "bool" : v->is_string() ? "string" : v->is_number_integer() ? "int" : "";
			if (kind.empty()) continue;
			std::string value = LeafText(*v);
			auto it = src.machines.find(path);
			if (it == src.machines.end())
			{
				FieldMachine fm;
				fm.path = path;
				fm.kind = kind;
				fm.current = value;
				fm.enteredAt = t;
				fm.states[value].enters = 1;
				fm.states[value].enteredAt = t;
				fm.timeline.push_back({ t, value });
				src.machines.emplace(path, std::move(fm));
				continue;
			}
			FieldMachine& fm = it->second;
			if (!fm.discrete || fm.current == value) continue;
			StateStat& from = fm.states[fm.current];
			from.total += t - fm.enteredAt;
			TransitionStat& edge = fm.edges[{ fm.current, value }];
			++edge.count;
			edge.last = t;
			fm.lastFrom = fm.current;
			fm.lastTo = value;
			fm.lastAt = t;
			StateStat& to = fm.states[value];
			++to.enters;
			to.enteredAt = t;
			fm.current = value;
			fm.enteredAt = t;
			++fm.transitions;
			fm.timeline.push_back({ t, value });
			if (fm.timeline.size() > 5000) fm.timeline.erase(fm.timeline.begin(), fm.timeline.begin() + 1000);
			if ((int)fm.states.size() > distinctLimit) fm.discrete = false;   // a counter or free text, not a state
		}
	}
};
