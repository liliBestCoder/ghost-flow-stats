#define WIN32_LEAN_AND_MEAN
#include "loopback_ui.h"
#include "env_utf8.h"
#include "history.h"
#include "str_conv.h"

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <charconv>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using nlohmann::json;
namespace fs = std::filesystem;

struct HttpHandle {
    HINTERNET value;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};

struct Handle {
    HANDLE value;
    ~Handle() { if (value) CloseHandle(value); }
};

INTERNET_PORT LocalPort(const std::string& origin) {
    constexpr std::string_view prefix = "http://127.0.0.1:";
    if (origin.compare(0, prefix.size(), prefix) != 0) throw std::runtime_error("expected loopback API");
    const char* end = origin.data() + origin.size();
    INTERNET_PORT port = 0;
    const auto parsed = std::from_chars(origin.data() + prefix.size(), end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !port) throw std::runtime_error("invalid API port");
    return port;
}

json ReadGhostJson(HINTERNET connection, const std::wstring& header, const wchar_t* path) {
    const HttpHandle request{WinHttpOpenRequest(connection, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)};
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!request.value ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSendRequest(request.value, header.c_str(), static_cast<DWORD>(header.size()),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr))
        throw std::runtime_error("Ghost API unavailable");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        throw std::runtime_error("Ghost API request rejected");
    std::string response;
    char chunk[8192];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, chunk, sizeof(chunk), &read) ||
            response.size() + read > 32 * 1024 * 1024)
            throw std::runtime_error("invalid Ghost API response");
        if (!read) break;
        response.append(chunk, read);
    }
    return json::parse(response);
}

bool IsUpstreamName(const std::string& name) {
    return !name.empty() && name != "Direct" && name != "Block" && name != "Unavailable target";
}

json ListQuotaNodes(HINTERNET connection, const std::wstring& header) {
    const auto config = ReadGhostJson(connection, header, L"/config");
    const auto upstream = config.find("upstream");
    auto names = json::array();
    std::set<std::string> seen;
    if (upstream != config.end() && upstream->is_array()) {
        for (const auto& node : *upstream) {
            if (!node.is_object()) continue;
            const auto id = node.find("id");
            if (id != node.end() && *id == "direct") continue;
            const auto name = node.find("name");
            if (name == node.end() || !name->is_string()) continue;
            const auto& label = name->get_ref<const std::string&>();
            const auto reported = ghost_env::SanitizeStatField(
                ghost_env::TruncateUtf8(label, ghost_env::kMaxNodeNameBytes));
            if (IsUpstreamName(reported) && seen.insert(reported).second)
                names.push_back({{"value", reported}, {"label", label}});
        }
    }
    return {{"status", "ok"}, {"nodes", names}};
}

uint64_t FileTimeTicks(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

uint64_t ProcessStart(DWORD pid) {
    const Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!process.value || !GetProcessTimes(process.value, &created, &exited, &kernel, &user)) return 0;
    return FileTimeTicks(created);
}

std::string LocalDay(time_t now, int offset = 0) {
    std::tm day{};
    localtime_s(&day, &now);
    day.tm_mday += offset;
    day.tm_hour = 12;
    day.tm_isdst = -1;
    std::mktime(&day);
    char result[16]{};
    std::strftime(result, sizeof(result), "%Y-%m-%d", &day);
    return result;
}

struct Counters {
    uint64_t up;
    uint64_t down;
};

int64_t Difference(uint64_t current, uint64_t previous) {
    if (current < previous || current - previous > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return 0;
    return static_cast<int64_t>(current - previous);
}

void Collect(History& history, HANDLE stop, HINTERNET connection, const std::wstring& header) {
    using Identity = std::pair<DWORD, uint64_t>;
    std::map<Identity, Counters> previous;
    uint64_t lastPoll = 0;
    do {
        try {
            FILETIME started{};
            GetSystemTimeAsFileTime(&started);
            const auto snapshot = ReadGhostJson(connection, header, L"/process-stats");
            const auto& stats = snapshot.at("stats");
            if (!stats.is_array()) throw std::runtime_error("invalid process list");
            std::map<Identity, Counters> next;
            std::vector<TrafficDelta> deltas;
            for (const auto& row : stats) {
                const auto pid = row.at("pid").get<DWORD>();
                const auto process = row.at("name").get<std::string>();
                const uint64_t born = ProcessStart(pid);
                if (!born || born > FileTimeTicks(started) || process.empty()) continue;
                auto node = row.value("node", std::string{});
                if (!IsUpstreamName(node)) node.clear();
                const Counters current{row.at("up").get<uint64_t>(), row.at("down").get<uint64_t>()};
                const Identity id{pid, born};
                if (!next.emplace(id, current).second) continue;
                const auto old = previous.find(id);
                // Already-running processes start with a baseline. A process
                // born inside a sampled interval can contribute from zero.
                if (old == previous.end() && (!lastPoll || born < lastPoll)) continue;
                const Counters base = old != previous.end() ? old->second : Counters{};
                const auto up = Difference(current.up, base.up);
                const auto down = Difference(current.down, base.down);
                if (up || down) deltas.push_back({process, std::move(node), up, down});
            }
            if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;
            const auto now = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &now);
            history.Record(LocalDay(now), local.tm_hour, deltas, now);
            previous.swap(next);
            lastPoll = FileTimeTicks(started);
        } catch (const std::exception&) {
            history.SetUnavailable();
        }
    } while (WaitForSingleObject(stop, 5000) == WAIT_TIMEOUT);
}
}  // namespace

int main() {
    if (!GetEnvironmentVariableW(L"GHOST_PLUGIN_ID", nullptr, 0)) return 1;
    try {
        std::string line;
        if (!std::getline(std::cin, line) || line.size() > 65536) throw std::runtime_error("missing handshake");
        const auto hello = json::parse(line);
        if (hello.at("v") != 1 || hello.at("pluginId") != "com.ghostproxifier.flow-stats")
            throw std::runtime_error("invalid handshake");
        const auto origin = hello.at("apiBase").get<std::string>();
        const auto port = LocalPort(origin);
        const auto token = hello.at("token").get<std::string>();
        if (token.empty() || token.size() > 128 ||
            token.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz{}-") != std::string::npos)
            throw std::runtime_error("invalid plugin token");
        const std::wstring header = L"X-Ghost-Plugin-Token: " + StringToWChar(token) + L"\r\n";
        const auto stopName = StringToWChar(hello.at("stopEvent").get<std::string>());
        const Handle stop{OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str())};
        if (!stop.value) throw std::runtime_error("missing stop event");
        const HttpHandle session{WinHttpOpen(L"Ghost Flow Stats", WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.value || !WinHttpSetTimeouts(session.value, 1000, 1000, 1500, 2000))
            throw std::runtime_error("HTTP session unavailable");
        const HttpHandle connection{WinHttpConnect(session.value, L"127.0.0.1", port, 0)};
        if (!connection.value) throw std::runtime_error("HTTP connection unavailable");
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock)) throw std::runtime_error("Winsock startup failed");
        const fs::path dir(StringToWChar(hello.at("pluginDir").get<std::string>()));
        const fs::path data(StringToWChar(hello.at("dataDir").get<std::string>()));
        History history(WCharToString((data / L"traffic.sqlite3").c_str()));
        const LoopbackUi ui(dir / "ui", origin + " 'self'", "x-ghost-flow-ui");
        std::thread collector([&] { Collect(history, stop.value, connection.value, header); });
        std::thread panel([&] {
            ui.Serve([&](const json& request) -> json {
                const auto action = request.value("action", std::string("dashboard"));
                if (action == "listQuotaNodes") return ListQuotaNodes(connection.value, header);
                if (action == "setQuota") {
                    const auto node = request.at("node").get<std::string>();
                    const auto& quota = request.at("bytes");
                    if (node.size() > 512 || node.find_first_not_of(' ') == std::string::npos ||
                        std::any_of(node.begin(), node.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
                        !quota.is_number_integer() || quota < 0 || quota > 1000000000000000LL)
                        return {{"status", "error"}, {"error", "invalid_quota"}};
                    history.SetQuota(node, quota.get<int64_t>());
                    return {{"status", "ok"}};
                }
                if (action != "dashboard") return {{"status", "error"}, {"error", "invalid_action"}};
                const int days = request.value("days", 14);
                if (days != 1 && days != 7 && days != 14 && days != 30 && days != 60 && days != 180)
                    return {{"status", "error"}, {"error", "invalid_range"}};
                const auto now = std::time(nullptr);
                return history.Dashboard(LocalDay(now, 1 - days), LocalDay(now),
                                         LocalDay(now, 1 - 2 * days), LocalDay(now, -days));
            });
        });
        std::cout << json({{"v", 1}, {"ok", true}, {"uiUrl", ui.Url()}}).dump() << std::endl;
        WaitForSingleObject(stop.value, INFINITE);
        collector.join();
        // The panel listener can still be blocked in accept. All writes have
        // finished before the process releases the listener and database.
        ExitProcess(0);
    } catch (const std::exception& error) {
        std::cout << json({{"v", 1}, {"ok", false}, {"error", error.what()}}).dump(
            -1, ' ', false, json::error_handler_t::replace) << std::endl;
        return 1;
    }
}
