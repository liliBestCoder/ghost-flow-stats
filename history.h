#pragma once

#include <winsqlite/winsqlite3.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct TrafficDelta {
    std::string process;
    std::string node;
    int64_t upload;
    int64_t download;
};

class History {
public:
    explicit History(const std::string& file);
    ~History();
    History(const History&) = delete;
    History& operator=(const History&) = delete;

    void Record(const std::string& day, int hour, const std::vector<TrafficDelta>& deltas, int64_t sampledAt);
    void SetUnavailable();
    void SetQuota(const std::string& node, int64_t bytes);
    nlohmann::json Dashboard(const std::string& fromDay, const std::string& throughDay,
                             const std::string& previousFrom, const std::string& previousThrough);

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
    bool collecting_ = false;
    int64_t lastSample_ = 0;
};
