// Shared helpers: text conversion, JSON access, shell, file dialogs and the command runner.
#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "nlohmann/json.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

static HWND g_hwnd = nullptr;

// ============================================================================ text

static std::string ToUtf8(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

static std::wstring ToWide(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

// Child output mixes UTF-8 (Python) and the console code page (cmd, PowerShell).
static std::string ConsoleToUtf8(const std::string& raw)
{
	if (raw.empty()) return raw;
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0) > 0)
		return raw;
	UINT cp = GetOEMCP();
	int n = MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), w.data(), n);
	return ToUtf8(w);
}

static std::string PathUtf8(const fs::path& p) { return p.u8string(); }

static std::string Lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)::tolower(c); });
	return s;
}

static bool ContainsNoCase(const std::string& hay, const char* needle)
{
	if (!needle || !*needle) return true;
	return Lower(hay).find(Lower(needle)) != std::string::npos;
}

// Subsequence match used by the command palette ("anz" matches "analyze").
static int FuzzyScore(const std::string& text, const std::string& query)
{
	if (query.empty()) return 1;
	std::string t = Lower(text), q = Lower(query);
	if (t.find(q) != std::string::npos) return 1000 - (int)t.find(q);
	size_t at = 0;
	int score = 0;
	for (char c : q)
	{
		size_t p = t.find(c, at);
		if (p == std::string::npos) return 0;
		score += p == at ? 3 : 1;
		at = p + 1;
	}
	return score;
}

static std::string Fmt(const char* fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

static std::string Bytes(double b)
{
	const char* units[] = { "B", "KB", "MB", "GB", "TB" };
	int u = 0;
	while (b >= 1024 && u < 4) { b /= 1024; ++u; }
	return u == 0 ? Fmt("%.0f B", b) : Fmt("%.1f %s", b, units[u]);
}

static std::string Duration(double seconds)
{
	if (seconds < 1) return Fmt("%.0fms", seconds * 1000);
	if (seconds < 60) return Fmt("%.1f초", seconds);
	if (seconds < 3600) return Fmt("%d분 %d초", (int)seconds / 60, (int)seconds % 60);
	return Fmt("%d시간 %d분", (int)seconds / 3600, ((int)seconds % 3600) / 60);
}

// Run ids look like "<line>-yyyyMMdd-HHmmss"; returns "MM-dd HH:mm".
static std::string StampOf(const std::string& id)
{
	static const std::regex re(R"((\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2}))");
	std::smatch m;
	if (!std::regex_search(id, m, re)) return id;
	return m[2].str() + "-" + m[3].str() + " " + m[4].str() + ":" + m[5].str();
}

static std::string SortKeyOf(const std::string& id)
{
	auto p = id.find('-');
	return p == std::string::npos ? id : id.substr(p + 1);
}

// ============================================================================ json

static std::optional<json> ReadJson(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) return std::nullopt;
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
		text.erase(0, 3);
	try { return json::parse(text); }
	catch (...) { return std::nullopt; }
}

static std::string ReadText(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string Str(const json& j, const char* key, const std::string& fallback = "")
{
	if (!j.is_object()) return fallback;
	auto it = j.find(key);
	if (it == j.end() || it->is_null()) return fallback;
	if (it->is_string()) return it->get<std::string>();
	return it->dump();
}

static long long Num(const json& j, const char* key)
{
	if (!j.is_object()) return 0;
	auto it = j.find(key);
	if (it == j.end()) return 0;
	if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
	return it->is_number() ? it->get<long long>() : 0;
}

static double Real(const json& j, const char* key, double fallback = 0)
{
	if (!j.is_object()) return fallback;
	auto it = j.find(key);
	return (it != j.end() && it->is_number()) ? it->get<double>() : fallback;
}

// Stable references so ImGui IDs never point at temporaries.
static const json& Arr(const json& j, const char* key)
{
	static const json empty = json::array();
	if (!j.is_object()) return empty;
	auto it = j.find(key);
	return (it != j.end() && it->is_array()) ? *it : empty;
}

static const json& Obj(const json& j, const char* key)
{
	static const json empty = json::object();
	if (!j.is_object()) return empty;
	auto it = j.find(key);
	return (it != j.end() && it->is_object()) ? *it : empty;
}

static std::string JoinStrings(const json& list, const char* sep = ", ")
{
	std::string s;
	for (auto& v : list)
	{
		if (!s.empty()) s += sep;
		s += v.is_string() ? v.get<std::string>() : v.dump();
	}
	return s;
}

// ============================================================================ shell and dialogs

static void OpenWithShell(const fs::path& path)
{
	ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void RevealInExplorer(const fs::path& path)
{
	std::wstring args = L"/select,\"" + path.wstring() + L"\"";
	ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void CopyToClipboard(const std::string& text) { ImGui::SetClipboardText(text.c_str()); }

static bool PickSaveFile(HWND owner, const std::wstring& defaultName, const wchar_t* typeName, const wchar_t* pattern,
                         const wchar_t* ext, fs::path& out)
{
	IFileSaveDialog* dialog = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return false;
	COMDLG_FILTERSPEC types[] = { { typeName, pattern } };
	dialog->SetFileTypes(1, types);
	dialog->SetDefaultExtension(ext);
	dialog->SetFileName(defaultName.c_str());
	bool ok = false;
	if (SUCCEEDED(dialog->Show(owner)))
	{
		IShellItem* item = nullptr;
		if (SUCCEEDED(dialog->GetResult(&item)))
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				out = path;
				CoTaskMemFree(path);
				ok = true;
			}
			item->Release();
		}
	}
	dialog->Release();
	return ok;
}

static bool PickOpen(HWND owner, bool folder, const wchar_t* title, fs::path& out, const wchar_t* typeName = nullptr,
                     const wchar_t* pattern = nullptr)
{
	IFileOpenDialog* dialog = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return false;
	DWORD options = 0;
	dialog->GetOptions(&options);
	dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
	if (!folder && typeName)
	{
		COMDLG_FILTERSPEC types[] = { { typeName, pattern } };
		dialog->SetFileTypes(1, types);
	}
	dialog->SetTitle(title);
	bool ok = false;
	if (SUCCEEDED(dialog->Show(owner)))
	{
		IShellItem* item = nullptr;
		if (SUCCEEDED(dialog->GetResult(&item)))
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				out = path;
				CoTaskMemFree(path);
				ok = true;
			}
			item->Release();
		}
	}
	dialog->Release();
	return ok;
}

// ============================================================================ process runner

// Runs one command at a time inside a job object so cancelling also stops UnrealEditor-Cmd children.
class Runner
{
public:
	~Runner() { Cancel(); Join(); }

	bool Start(const std::wstring& commandLine, const fs::path& workDir, const std::string& label)
	{
		if (running) return false;
		Join();
		{
			std::lock_guard<std::mutex> lock(mutex);
			AppendLocked("");
			AppendLocked("> " + label);
		}
		SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
		HANDLE readPipe = nullptr, writePipe = nullptr;
		if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
		SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW si{ sizeof(si) };
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdOutput = writePipe;
		si.hStdError = writePipe;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

		PROCESS_INFORMATION pi{};
		std::wstring cmd = commandLine;
		BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
		                         CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, workDir.wstring().c_str(), &si, &pi);
		CloseHandle(writePipe);
		if (!ok)
		{
			CloseHandle(readPipe);
			std::lock_guard<std::mutex> lock(mutex);
			AppendLocked("[gui] 실행 실패: CreateProcess error " + std::to_string(GetLastError()));
			return false;
		}
		job = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
		AssignProcessToJobObject(job, pi.hProcess);
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);

		process = pi.hProcess;
		running = true;
		finished = false;
		current = label;
		startedAt = std::chrono::steady_clock::now();
		reader = std::thread([this, readPipe] { ReadLoop(readPipe); });
		return true;
	}

	void Cancel()
	{
		if (running && job)
		{
			TerminateJobObject(job, 1);
			std::lock_guard<std::mutex> lock(mutex);
			AppendLocked("[gui] 취소됨");
		}
	}

	// True once per completed command; the UI uses it to refresh run history.
	bool ConsumeFinished()
	{
		bool expected = true;
		return finished.compare_exchange_strong(expected, false);
	}

	double ElapsedSeconds() const
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
	}

	void Clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		lines.clear();
		++version;
	}

	std::string LastLine()
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto it = lines.rbegin(); it != lines.rend(); ++it)
			if (!it->empty()) return *it;
		return {};
	}

	template <typename F> void WithLines(F&& fn)
	{
		std::lock_guard<std::mutex> lock(mutex);
		fn(lines);
	}

	std::atomic<bool> running{ false };
	int exitCode = 0;
	size_t version = 0;
	std::string current;

private:
	void AppendLocked(const std::string& line)
	{
		lines.push_back(line);
		if (lines.size() > 20000) lines.pop_front();
		++version;
	}

	void ReadLoop(HANDLE pipe)
	{
		std::string pending;
		char buffer[4096];
		DWORD read = 0;
		while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
		{
			pending.append(buffer, read);
			size_t pos;
			while ((pos = pending.find('\n')) != std::string::npos)
			{
				std::string line = pending.substr(0, pos);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				pending.erase(0, pos + 1);
				std::lock_guard<std::mutex> lock(mutex);
				AppendLocked(ConsoleToUtf8(line));
			}
		}
		CloseHandle(pipe);
		WaitForSingleObject(process, INFINITE);
		DWORD code = 0;
		GetExitCodeProcess(process, &code);
		CloseHandle(process);
		CloseHandle(job);
		process = nullptr;
		job = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!pending.empty()) AppendLocked(ConsoleToUtf8(pending));
			AppendLocked("[gui] 종료 코드 " + std::to_string(code) + " (" + Duration(ElapsedSeconds()) + ")");
		}
		exitCode = (int)code;
		running = false;
		finished = true;
	}

	void Join()
	{
		if (reader.joinable()) reader.join();
	}

	std::mutex mutex;
	std::deque<std::string> lines;
	std::thread reader;
	HANDLE process = nullptr;
	HANDLE job = nullptr;
	std::atomic<bool> finished{ false };
	std::chrono::steady_clock::time_point startedAt;
};

static double NowSeconds()
{
	static const auto start = std::chrono::steady_clock::now();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
