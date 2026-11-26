#pragma once

#include <winsock2.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <string>

// Serves the bundled panel on a per-run loopback URL. The Ghost API token
// stays in the native process and is never given to the panel.
class LoopbackUi {
public:
    using Dispatch = std::function<nlohmann::json(const nlohmann::json&)>;

    explicit LoopbackUi(const std::filesystem::path& directory, std::string frameAncestor,
                        std::string requestHeader = "x-chrome-launcher");
    ~LoopbackUi();
    LoopbackUi(const LoopbackUi&) = delete;
    LoopbackUi& operator=(const LoopbackUi&) = delete;

    std::string Url() const;
    void Serve(const Dispatch& dispatch) const;

private:
    SOCKET listener_ = INVALID_SOCKET;
    std::string authority_;
    std::string prefix_;
    std::string frameAncestor_;
    std::string requestHeader_;
    std::string html_;
    std::string javascript_;
    std::string css_;
};
