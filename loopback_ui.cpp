#define WIN32_LEAN_AND_MEAN
#include "loopback_ui.h"

#include <windows.h>
#include <bcrypt.h>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string_view>

namespace {
using nlohmann::json;

struct Socket {
    SOCKET value;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};

std::string LoadAsset(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("missing UI asset");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string RandomPrefix() {
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("random URL unavailable");
    constexpr char hex[] = "0123456789abcdef";
    std::string prefix = "/";
    for (const auto byte : bytes) {
        prefix += hex[byte >> 4];
        prefix += hex[byte & 15];
    }
    return prefix + "/";
}

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

bool ReadRequest(SOCKET client, Request& request) {
    constexpr size_t maxHeader = 8192;
    constexpr size_t maxBody = 8192;
    const ULONGLONG deadline = GetTickCount64() + 3000;
    std::string buffer;
    char chunk[4096];
    const auto receive = [&] {
        if (GetTickCount64() >= deadline) return false;
        const int count = recv(client, chunk, sizeof(chunk), 0);
        if (count <= 0) return false;
        buffer.append(chunk, count);
        return true;
    };
    size_t end = std::string::npos;
    while (end == std::string::npos) {
        if (buffer.size() >= maxHeader || !receive()) return false;
        end = buffer.find("\r\n\r\n");
    }
    const size_t lineEnd = buffer.find("\r\n");
    const size_t methodEnd = buffer.find(' ');
    if (end > maxHeader || methodEnd >= lineEnd) return false;
    const size_t pathEnd = buffer.find(' ', methodEnd + 1);
    if (pathEnd >= lineEnd || buffer.compare(pathEnd, lineEnd - pathEnd, " HTTP/1.1") != 0)
        return false;
    request.method = buffer.substr(0, methodEnd);
    request.path = buffer.substr(methodEnd + 1, pathEnd - methodEnd - 1);

    for (size_t start = lineEnd + 2; start < end;) {
        const size_t next = buffer.find("\r\n", start);
        const size_t colon = buffer.find(':', start);
        if (colon >= next) return false;
        std::string name = buffer.substr(start, colon - start);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const size_t valueStart = buffer.find_first_not_of(" \t", colon + 1);
        const auto value = valueStart < next ? buffer.substr(valueStart, next - valueStart) : "";
        if (!request.headers.emplace(std::move(name), value).second) return false;
        start = next + 2;
    }
    if (request.headers.count("transfer-encoding")) return false;
    size_t length = 0;
    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
        const auto& value = it->second;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), length);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || length > maxBody)
            return false;
    }
    const size_t bodyStart = end + 4;
    while (buffer.size() - bodyStart < length) {
        if (!receive()) return false;
    }
    request.body = buffer.substr(bodyStart, length);
    return true;
}

void Send(SOCKET client, std::string_view data) {
    while (!data.empty()) {
        const int count = send(client, data.data(), static_cast<int>(data.size()), 0);
        if (count <= 0) return;
        data.remove_prefix(count);
    }
}

void Reply(SOCKET client, const char* status, const char* type,
           std::string_view body, const std::string& frameAncestor) {
    const std::string header = std::string("HTTP/1.1 ") + status +
        "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"
        "\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none';"
        " script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none';"
        " form-action 'none'; frame-ancestors " + frameAncestor + "\r\n\r\n";
    Send(client, header);
    Send(client, body);
}
}  // namespace

LoopbackUi::LoopbackUi(const std::filesystem::path& directory, std::string frameAncestor,
                       std::string requestHeader)
    : prefix_(RandomPrefix()), frameAncestor_(std::move(frameAncestor)),
      requestHeader_(std::move(requestHeader)),
      html_(LoadAsset(directory / "index.html")), javascript_(LoadAsset(directory / "app.js")),
      css_(LoadAsset(directory / "style.css")) {
    Socket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (socket.value == INVALID_SOCKET) throw std::runtime_error("UI socket unavailable");
    const BOOL exclusive = TRUE;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    if (setsockopt(socket.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0 ||
        bind(socket.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(socket.value, SOMAXCONN) != 0 ||
        getsockname(socket.value, reinterpret_cast<sockaddr*>(&address), &size) != 0)
        throw std::runtime_error("UI bind failed");
    authority_ = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    listener_ = socket.value;
    socket.value = INVALID_SOCKET;
}

LoopbackUi::~LoopbackUi() { closesocket(listener_); }

std::string LoopbackUi::Url() const { return "http://" + authority_ + prefix_; }

void LoopbackUi::Serve(const Dispatch& dispatch) const {
    for (;;) {
        Socket client{accept(listener_, nullptr, nullptr)};
        if (client.value == INVALID_SOCKET) return;
        const DWORD timeout = 1500;
        setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        const auto reply = [&](const char* status, const char* type, std::string_view body) {
            Reply(client.value, status, type, body, frameAncestor_);
        };
        Request request;
        if (!ReadRequest(client.value, request) || request.headers["host"] != authority_) {
            reply("400 Bad Request", "text/plain", "");
        } else if (request.method == "GET" && request.path == prefix_) {
            reply("200 OK", "text/html; charset=utf-8", html_);
        } else if (request.method == "GET" && request.path == prefix_ + "app.js") {
            reply("200 OK", "text/javascript; charset=utf-8", javascript_);
        } else if (request.method == "GET" && request.path == prefix_ + "style.css") {
            reply("200 OK", "text/css; charset=utf-8", css_);
        } else if (request.method == "POST" && request.path == prefix_ + "api" &&
                   request.headers[requestHeader_] == "1" &&
                   request.headers["origin"] == "http://" + authority_ &&
                   request.headers["content-type"] == "application/json") {
            try {
                const auto result = dispatch(json::parse(request.body));
                reply("200 OK", "application/json", result.dump());
            } catch (const json::exception&) {
                reply("400 Bad Request", "application/json", R"({"status":"error","error":"invalid_request"})");
            } catch (const std::exception&) {
                reply("502 Bad Gateway", "application/json", R"({"status":"error","error":"host_unavailable"})");
            }
        } else {
            reply("404 Not Found", "text/plain", "");
        }
    }
}
