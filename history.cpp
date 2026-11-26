#include "history.h"

#include <limits>
#include <stdexcept>

namespace {
using nlohmann::json;

void Check(int result, sqlite3* db) {
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        Check(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr), db);
    }
    ~Statement() { sqlite3_finalize(stmt_); }

    void Bind(int index, const std::string& value) {
        Check(sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT), db_);
    }
    void Bind(int index, int64_t value) {
        Check(sqlite3_bind_int64(stmt_, index, value), db_);
    }
    bool Next() {
        const int result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        Check(result, db_);
        return false;
    }
    void Reset() { Check(sqlite3_reset(stmt_), db_); }
    const char* Text(int column) {
        return reinterpret_cast<const char*>(sqlite3_column_text(stmt_, column));
    }
    int64_t Number(int column) { return sqlite3_column_int64(stmt_, column); }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

void AddDelta(Statement& insert, Statement& update, const std::string& day,
              const std::string& name, const TrafficDelta& delta) {
    insert.Bind(1, day);
    insert.Bind(2, name);
    insert.Next();
    insert.Reset();
    update.Bind(1, day);
    update.Bind(2, name);
    update.Bind(3, delta.upload);
    update.Bind(4, delta.download);
    update.Next();
    update.Reset();
}
}  // namespace

History::History(const std::string& file) {
    try {
        if (sqlite3_open_v2(file.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot open traffic history");
        Check(sqlite3_busy_timeout(db_, 1500), db_);
        Check(sqlite3_exec(db_,
            "CREATE TABLE IF NOT EXISTS daily ("
            "day TEXT NOT NULL, process TEXT COLLATE NOCASE NOT NULL, "
            "up INTEGER NOT NULL, down INTEGER NOT NULL, PRIMARY KEY(day, process)) WITHOUT ROWID;"
            "CREATE TABLE IF NOT EXISTS daily_nodes ("
            "day TEXT NOT NULL, node TEXT NOT NULL, "
            "up INTEGER NOT NULL, down INTEGER NOT NULL, PRIMARY KEY(day, node)) WITHOUT ROWID;"
            "CREATE TABLE IF NOT EXISTS node_quotas ("
            "node TEXT PRIMARY KEY, bytes INTEGER NOT NULL CHECK(bytes > 0)) WITHOUT ROWID;"
            "CREATE TABLE IF NOT EXISTS hourly ("
            "day TEXT NOT NULL, hour INTEGER NOT NULL, up INTEGER NOT NULL, down INTEGER NOT NULL, "
            "PRIMARY KEY(day, hour)) WITHOUT ROWID;",
            nullptr, nullptr, nullptr), db_);
    } catch (...) {
        sqlite3_close(db_);
        throw;
    }
}

History::~History() { sqlite3_close(db_); }

void History::Record(const std::string& day, int hour, const std::vector<TrafficDelta>& deltas, int64_t sampledAt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!deltas.empty()) {
        Check(sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), db_);
        try {
            Statement insert(db_, "INSERT OR IGNORE INTO daily VALUES(?1, ?2, 0, 0)");
            Statement update(db_, "UPDATE daily SET up = up + ?3, down = down + ?4 WHERE day = ?1 AND process = ?2");
            Statement insertNode(db_, "INSERT OR IGNORE INTO daily_nodes VALUES(?1, ?2, 0, 0)");
            Statement updateNode(db_, "UPDATE daily_nodes SET up = up + ?3, down = down + ?4 "
                                      "WHERE day = ?1 AND node = ?2");
            int64_t up = 0, down = 0;
            for (const auto& delta : deltas) {
                const auto max = std::numeric_limits<int64_t>::max();
                if (delta.upload > max - up || delta.download > max - down)
                    throw std::overflow_error("hourly sample exceeds counter maximum");
                AddDelta(insert, update, day, delta.process, delta);
                if (!delta.node.empty()) AddDelta(insertNode, updateNode, day, delta.node, delta);
                up += delta.upload;
                down += delta.download;
            }
            Statement insertHour(db_, "INSERT OR IGNORE INTO hourly VALUES(?1, ?2, 0, 0)");
            insertHour.Bind(1, day);
            insertHour.Bind(2, hour);
            insertHour.Next();
            Statement updateHour(db_, "UPDATE hourly SET up = up + ?3, down = down + ?4 "
                                      "WHERE day = ?1 AND hour = ?2");
            updateHour.Bind(1, day);
            updateHour.Bind(2, hour);
            updateHour.Bind(3, up);
            updateHour.Bind(4, down);
            updateHour.Next();
            Check(sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr), db_);
        } catch (...) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }
    collecting_ = true;
    lastSample_ = sampledAt;
}

void History::SetUnavailable() {
    std::lock_guard<std::mutex> lock(mutex_);
    collecting_ = false;
}

void History::SetQuota(const std::string& node, int64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_, bytes ? "INSERT OR REPLACE INTO node_quotas VALUES(?1, ?2)"
                                    : "DELETE FROM node_quotas WHERE node = ?1");
    statement.Bind(1, node);
    if (bytes) statement.Bind(2, bytes);
    statement.Next();
}

json History::Dashboard(const std::string& fromDay, const std::string& throughDay,
                        const std::string& previousFrom, const std::string& previousThrough) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto totals = [&](const std::string& start, const std::string& end) {
        Statement rows(db_, "SELECT COALESCE(SUM(up),0), COALESCE(SUM(down),0), COUNT(*) "
                            "FROM daily WHERE day BETWEEN ?1 AND ?2");
        rows.Bind(1, start);
        rows.Bind(2, end);
        rows.Next();
        return json{{"up", rows.Number(0)}, {"down", rows.Number(1)}, {"hasRecords", rows.Number(2) != 0}};
    };
    const auto current = totals(fromDay, throughDay);
    auto previous = totals(previousFrom, previousThrough);
    previous["from"] = previousFrom;
    previous["through"] = previousThrough;
    Statement days(db_, "SELECT day, SUM(up), SUM(down) FROM daily "
                        "WHERE day BETWEEN ?1 AND ?2 GROUP BY day ORDER BY day");
    days.Bind(1, fromDay);
    days.Bind(2, throughDay);
    json daily = json::array();
    while (days.Next()) {
        daily.push_back({{"day", days.Text(0)}, {"up", days.Number(1)}, {"down", days.Number(2)}});
    }
    const auto rank = [&](const char* sql) {
        Statement rows(db_, sql);
        rows.Bind(1, fromDay);
        rows.Bind(2, throughDay);
        json result = json::array();
        while (rows.Next()) result.push_back({{"name", rows.Text(0)}, {"bytes", rows.Number(1)}});
        return result;
    };
    const auto changes = [&](const char* sql) {
        Statement rows(db_, sql);
        rows.Bind(1, fromDay);
        rows.Bind(2, previousFrom);
        rows.Bind(3, throughDay);
        json result = json::array();
        while (rows.Next())
            result.push_back({{"name", rows.Text(0)}, {"bytes", rows.Number(1)}, {"previous", rows.Number(2)}});
        return result;
    };
    const auto monthStart = throughDay.substr(0, 7) + "-01";
    Statement quotas(db_, "SELECT q.node, q.bytes, COALESCE(SUM(d.up),0), COALESCE(SUM(d.down),0) "
                         "FROM node_quotas q LEFT JOIN daily_nodes d ON d.node = q.node "
                         "AND d.day BETWEEN ?1 AND ?2 GROUP BY q.node, q.bytes ORDER BY q.node");
    quotas.Bind(1, monthStart);
    quotas.Bind(2, throughDay);
    json limits = json::array();
    while (quotas.Next()) {
        limits.push_back({{"node", quotas.Text(0)}, {"limit", quotas.Number(1)},
                          {"up", quotas.Number(2)}, {"down", quotas.Number(3)}});
    }
    Statement monthDays(db_, "SELECT d.node, d.day, d.up, d.down FROM daily_nodes d "
                             "JOIN node_quotas q ON q.node = d.node WHERE d.day BETWEEN ?1 AND ?2 "
                             "ORDER BY d.node, d.day");
    monthDays.Bind(1, monthStart);
    monthDays.Bind(2, throughDay);
    json quotaDaily = json::object();
    while (monthDays.Next()) {
        quotaDaily[monthDays.Text(0)].push_back({{"day", monthDays.Text(1)},
                                                {"up", monthDays.Number(2)}, {"down", monthDays.Number(3)}});
    }
    Statement hours(db_, "SELECT CAST(strftime('%w', day) AS INTEGER), hour, SUM(up), SUM(down) "
                         "FROM hourly WHERE day BETWEEN ?1 AND ?2 GROUP BY 1, 2 ORDER BY 1, 2");
    hours.Bind(1, fromDay);
    hours.Bind(2, throughDay);
    json hourly = json::array();
    while (hours.Next()) {
        hourly.push_back({{"weekday", hours.Number(0)}, {"hour", hours.Number(1)},
                          {"up", hours.Number(2)}, {"down", hours.Number(3)}});
    }
    return {{"status", "ok"}, {"collecting", collecting_}, {"lastSample", lastSample_},
            {"today", throughDay}, {"fromDay", fromDay},
            {"up", current["up"]}, {"down", current["down"]}, {"daily", daily},
            {"previous", previous}, {"monthStart", monthStart}, {"quotas", limits},
            {"quotaDaily", quotaDaily}, {"hourly", hourly},
            {"processChanges", changes("SELECT process, SUM(CASE WHEN day >= ?1 THEN up+down ELSE 0 END) AS recent, "
                                       "SUM(CASE WHEN day < ?1 THEN up+down ELSE 0 END) AS earlier "
                                       "FROM daily WHERE day BETWEEN ?2 AND ?3 GROUP BY process "
                                       "HAVING recent != earlier ORDER BY ABS(recent - earlier) DESC, process LIMIT 3")},
            {"nodeChanges", changes("SELECT node, SUM(CASE WHEN day >= ?1 THEN up+down ELSE 0 END) AS recent, "
                                    "SUM(CASE WHEN day < ?1 THEN up+down ELSE 0 END) AS earlier "
                                    "FROM daily_nodes WHERE day BETWEEN ?2 AND ?3 GROUP BY node "
                                    "HAVING recent != earlier ORDER BY ABS(recent - earlier) DESC, node LIMIT 3")},
            {"topUp", rank("SELECT process, SUM(up) FROM daily WHERE day BETWEEN ?1 AND ?2 "
                           "GROUP BY process HAVING SUM(up)>0 ORDER BY SUM(up) DESC, process LIMIT 10")},
            {"topDown", rank("SELECT process, SUM(down) FROM daily WHERE day BETWEEN ?1 AND ?2 "
                             "GROUP BY process HAVING SUM(down)>0 ORDER BY SUM(down) DESC, process LIMIT 10")},
            {"topNodeUp", rank("SELECT node, SUM(up) FROM daily_nodes WHERE day BETWEEN ?1 AND ?2 "
                               "GROUP BY node HAVING SUM(up)>0 ORDER BY SUM(up) DESC, node LIMIT 10")},
            {"topNodeDown", rank("SELECT node, SUM(down) FROM daily_nodes WHERE day BETWEEN ?1 AND ?2 "
                                 "GROUP BY node HAVING SUM(down)>0 ORDER BY SUM(down) DESC, node LIMIT 10")}};
}
