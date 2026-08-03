#include "Database.h"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <cstring>

namespace sdrscan {

using json = nlohmann::json;

namespace {

// Kept in sync with sql/schema.sql (that file is the human-readable reference /
// what you'd hand to the sqlite3 CLI; this is the copy actually compiled in).
const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS scanner_settings (
    key   TEXT PRIMARY KEY,
    value TEXT
);
CREATE TABLE IF NOT EXISTS receivers (
    id          TEXT PRIMARY KEY,
    type        TEXT NOT NULL,
    device_arg  TEXT,
    driver      TEXT,
    gain        REAL,
    gains_json  TEXT,
    enabled     INTEGER NOT NULL DEFAULT 1,
    sort_order  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS channels (
    id                TEXT PRIMARY KEY,
    freq_hz           INTEGER NOT NULL,
    label             TEXT,
    mode              TEXT NOT NULL DEFAULT 'FM',
    audio_gain_db     REAL NOT NULL DEFAULT 0,
    dwell_time_s      REAL NOT NULL DEFAULT 3.0,
    squelch_threshold REAL NOT NULL DEFAULT -55,
    ctcss_tone_hz     REAL,
    squelch_noise_margin_db REAL,
    noise_squelch_threshold_db REAL,
    dmr_slot          INTEGER,
    dmr_talkgroup_filter INTEGER,
    enabled           INTEGER NOT NULL DEFAULT 1,
    disable_until     REAL,
    mute              INTEGER NOT NULL DEFAULT 0,
    solo              INTEGER,
    hold              INTEGER NOT NULL DEFAULT 0,
    sort_order        INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS outputs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    type        TEXT NOT NULL,
    config_json TEXT NOT NULL DEFAULT '{}',
    enabled     INTEGER NOT NULL DEFAULT 1
);
)SQL";

// RAII wrapper for a prepared statement.
class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db));
        }
    }
    ~Stmt() { sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bindText(int idx, const std::string& v) { sqlite3_bind_text(stmt_, idx, v.c_str(), -1, SQLITE_TRANSIENT); }
    void bindNullableText(int idx, const std::optional<std::string>& v) {
        if (v) bindText(idx, *v); else sqlite3_bind_null(stmt_, idx);
    }
    void bindInt64(int idx, int64_t v) { sqlite3_bind_int64(stmt_, idx, v); }
    void bindInt(int idx, int v) { sqlite3_bind_int(stmt_, idx, v); }
    void bindDouble(int idx, double v) { sqlite3_bind_double(stmt_, idx, v); }
    void bindNullableDouble(int idx, const std::optional<double>& v) {
        if (v) bindDouble(idx, *v); else sqlite3_bind_null(stmt_, idx);
    }
    void bindNull(int idx) { sqlite3_bind_null(stmt_, idx); }

    bool step() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("sqlite step failed: ") + sqlite3_errmsg(sqlite3_db_handle(stmt_)));
    }

    std::string colText(int idx) const {
        const unsigned char* t = sqlite3_column_text(stmt_, idx);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    std::optional<std::string> colNullableText(int idx) const {
        if (sqlite3_column_type(stmt_, idx) == SQLITE_NULL) return std::nullopt;
        return colText(idx);
    }
    int64_t colInt64(int idx) const { return sqlite3_column_int64(stmt_, idx); }
    int colInt(int idx) const { return sqlite3_column_int(stmt_, idx); }
    double colDouble(int idx) const { return sqlite3_column_double(stmt_, idx); }
    std::optional<double> colNullableDouble(int idx) const {
        if (sqlite3_column_type(stmt_, idx) == SQLITE_NULL) return std::nullopt;
        return colDouble(idx);
    }
    std::optional<bool> colNullableBool(int idx) const {
        if (sqlite3_column_type(stmt_, idx) == SQLITE_NULL) return std::nullopt;
        return colInt(idx) != 0;
    }
    std::optional<int64_t> colNullableInt64(int idx) const {
        if (sqlite3_column_type(stmt_, idx) == SQLITE_NULL) return std::nullopt;
        return colInt64(idx);
    }
    void bindNullableInt64(int idx, const std::optional<int64_t>& v) {
        if (v) bindInt64(idx, *v); else sqlite3_bind_null(stmt_, idx);
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

bool columnExists(sqlite3* db, const std::string& table, const std::string& column) {
    Stmt s(db, "PRAGMA table_info(" + table + ")");
    while (s.step()) {
        if (s.colText(1) == column) return true; // column index 1 = name
    }
    return false;
}

} // namespace

Database::Database(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "unknown error";
        throw std::runtime_error("Failed to open database '" + path + "': " + err);
    }
    // Reasonable durability/perf tradeoff for a single-writer local app.
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::exec(const std::string& sql) {
    char* errMsg = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string msg = errMsg ? errMsg : "unknown error";
        sqlite3_free(errMsg);
        throw std::runtime_error("sqlite exec failed: " + msg);
    }
}

void Database::initSchema() {
    std::lock_guard<std::mutex> lock(mutex_);
    exec(kSchemaSql);
    // CREATE TABLE IF NOT EXISTS above only covers a fresh database - an existing one (from
    // before this column existed) needs an explicit migration to pick it up.
    if (!columnExists(db_, "channels", "squelch_noise_margin_db")) {
        exec("ALTER TABLE channels ADD COLUMN squelch_noise_margin_db REAL");
    }
    if (!columnExists(db_, "channels", "noise_squelch_threshold_db")) {
        exec("ALTER TABLE channels ADD COLUMN noise_squelch_threshold_db REAL");
    }
    if (!columnExists(db_, "channels", "dmr_slot")) {
        exec("ALTER TABLE channels ADD COLUMN dmr_slot INTEGER");
    }
    if (!columnExists(db_, "channels", "dmr_talkgroup_filter")) {
        exec("ALTER TABLE channels ADD COLUMN dmr_talkgroup_filter INTEGER");
    }
}

bool Database::isEmpty() {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "SELECT (SELECT COUNT(*) FROM channels) + (SELECT COUNT(*) FROM receivers)");
    if (s.step()) {
        return s.colInt64(0) == 0;
    }
    return true;
}

ScannerSettings Database::loadScannerSettings() {
    std::lock_guard<std::mutex> lock(mutex_);
    ScannerSettings settings;
    Stmt s(db_, "SELECT key, value FROM scanner_settings");
    while (s.step()) {
        std::string key = s.colText(0);
        std::string value = s.colText(1);
        if (key == "maxChannelsPerWindow") {
            // Defensive clamp against a stale <= 0 value written before Scanner::
            // setMaxChannelsPerWindow started rejecting it (see that function) - buildWindows()
            // infinite-loops on <= 0, so this can't be allowed to load silently.
            int v = std::stoi(value);
            settings.maxChannelsPerWindow = v > 0 ? v : ScannerSettings{}.maxChannelsPerWindow;
        }
        else if (key == "httpHost") settings.httpHost = value;
        else if (key == "httpPort") settings.httpPort = std::stoi(value);
    }
    return settings;
}

void Database::saveScannerSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "INSERT INTO scanner_settings(key, value) VALUES(?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bindText(1, key);
    s.bindText(2, value);
    s.step();
}

namespace {
ChannelConfig rowToChannel(const Stmt& s) {
    ChannelConfig cc;
    cc.id = s.colText(0);
    cc.freq_hz = s.colInt64(1);
    cc.label = s.colText(2);
    auto mode = channelModeFromString(s.colText(3));
    cc.mode = mode.value_or(ChannelMode::FM);
    cc.audioGain_dB = s.colDouble(4);
    cc.dwellTime_s = s.colDouble(5);
    cc.squelchThreshold = s.colDouble(6);
    cc.ctcssToneHz = s.colNullableDouble(7);
    cc.squelchNoiseMargin_dB = s.colNullableDouble(8);
    cc.noiseSquelchThreshold_dB = s.colNullableDouble(9);
    if (auto slot = s.colNullableInt64(10)) cc.dmrSlot = static_cast<int>(*slot);
    if (auto tg = s.colNullableInt64(11)) cc.dmrTalkgroupFilter = static_cast<uint32_t>(*tg);
    cc.enabled = s.colInt(12) != 0;
    cc.disableUntil = s.colNullableDouble(13);
    cc.mute = s.colInt(14) != 0;
    cc.solo = s.colNullableBool(15);
    cc.hold = s.colInt(16) != 0;
    cc.sortOrder = s.colInt(17);
    return cc;
}
} // namespace

std::vector<ChannelConfig> Database::listChannels() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChannelConfig> out;
    Stmt s(db_, "SELECT id, freq_hz, label, mode, audio_gain_db, dwell_time_s, squelch_threshold, "
                "ctcss_tone_hz, squelch_noise_margin_db, noise_squelch_threshold_db, "
                "dmr_slot, dmr_talkgroup_filter, enabled, "
                "disable_until, mute, solo, hold, sort_order "
                "FROM channels ORDER BY sort_order, freq_hz");
    while (s.step()) out.push_back(rowToChannel(s));
    return out;
}

void Database::upsertChannel(const ChannelConfig& cc) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_,
        "INSERT INTO channels(id, freq_hz, label, mode, audio_gain_db, dwell_time_s, squelch_threshold, "
        "ctcss_tone_hz, squelch_noise_margin_db, noise_squelch_threshold_db, dmr_slot, dmr_talkgroup_filter, "
        "enabled, disable_until, mute, solo, hold, sort_order) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET freq_hz=excluded.freq_hz, label=excluded.label, mode=excluded.mode, "
        "audio_gain_db=excluded.audio_gain_db, dwell_time_s=excluded.dwell_time_s, "
        "squelch_threshold=excluded.squelch_threshold, ctcss_tone_hz=excluded.ctcss_tone_hz, "
        "squelch_noise_margin_db=excluded.squelch_noise_margin_db, "
        "noise_squelch_threshold_db=excluded.noise_squelch_threshold_db, "
        "dmr_slot=excluded.dmr_slot, dmr_talkgroup_filter=excluded.dmr_talkgroup_filter, "
        "enabled=excluded.enabled, disable_until=excluded.disable_until, mute=excluded.mute, "
        "solo=excluded.solo, hold=excluded.hold, sort_order=excluded.sort_order");
    s.bindText(1, cc.id);
    s.bindInt64(2, cc.freq_hz);
    s.bindText(3, cc.label);
    s.bindText(4, channelModeToString(cc.mode));
    s.bindDouble(5, cc.audioGain_dB);
    s.bindDouble(6, cc.dwellTime_s);
    s.bindDouble(7, cc.squelchThreshold);
    s.bindNullableDouble(8, cc.ctcssToneHz);
    s.bindNullableDouble(9, cc.squelchNoiseMargin_dB);
    s.bindNullableDouble(10, cc.noiseSquelchThreshold_dB);
    s.bindNullableInt64(11, cc.dmrSlot ? std::optional<int64_t>(*cc.dmrSlot) : std::nullopt);
    s.bindNullableInt64(12, cc.dmrTalkgroupFilter ? std::optional<int64_t>(*cc.dmrTalkgroupFilter) : std::nullopt);
    s.bindInt(13, cc.enabled ? 1 : 0);
    s.bindNullableDouble(14, cc.disableUntil);
    s.bindInt(15, cc.mute ? 1 : 0);
    if (cc.solo.has_value()) s.bindInt(16, *cc.solo ? 1 : 0); else s.bindNull(16);
    s.bindInt(17, cc.hold ? 1 : 0);
    s.bindInt(18, cc.sortOrder);
    s.step();
}

void Database::deleteChannel(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "DELETE FROM channels WHERE id = ?");
    s.bindText(1, id);
    s.step();
}

std::vector<ReceiverConfig> Database::listReceivers() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ReceiverConfig> out;
    Stmt s(db_, "SELECT id, type, device_arg, driver, gain, gains_json, enabled, sort_order "
                "FROM receivers ORDER BY sort_order");
    while (s.step()) {
        ReceiverConfig rc;
        rc.id = s.colText(0);
        auto type = receiverTypeFromString(s.colText(1));
        rc.type = type.value_or(ReceiverType::RTL_SDR);
        rc.deviceArg = s.colNullableText(2);
        rc.driver = s.colNullableText(3);
        rc.gain = s.colNullableDouble(4);
        if (auto gainsJson = s.colNullableText(5)) {
            try {
                // NOTE: parse into a named local first - `json::parse(...).items()` would bind
                // the iteration_proxy to a temporary `json` that's destroyed before the loop
                // body runs (range-for only extends the lifetime of the range-init-expression's
                // own result, not sub-expression temporaries), silently iterating nothing.
                json parsed = json::parse(*gainsJson);
                for (auto& [k, v] : parsed.items()) rc.gains[k] = v.get<double>();
            } catch (const json::exception&) { /* ignore malformed gains_json */ }
        }
        rc.enabled = s.colInt(6) != 0;
        rc.sortOrder = s.colInt(7);
        out.push_back(rc);
    }
    return out;
}

void Database::upsertReceiver(const ReceiverConfig& rc) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_,
        "INSERT INTO receivers(id, type, device_arg, driver, gain, gains_json, enabled, sort_order) "
        "VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET type=excluded.type, device_arg=excluded.device_arg, "
        "driver=excluded.driver, gain=excluded.gain, gains_json=excluded.gains_json, "
        "enabled=excluded.enabled, sort_order=excluded.sort_order");
    s.bindText(1, rc.id);
    s.bindText(2, receiverTypeToString(rc.type));
    s.bindNullableText(3, rc.deviceArg);
    s.bindNullableText(4, rc.driver);
    s.bindNullableDouble(5, rc.gain);
    if (rc.gains.empty()) {
        s.bindNull(6);
    } else {
        json j = rc.gains;
        s.bindText(6, j.dump());
    }
    s.bindInt(7, rc.enabled ? 1 : 0);
    s.bindInt(8, rc.sortOrder);
    s.step();
}

void Database::deleteReceiver(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "DELETE FROM receivers WHERE id = ?");
    s.bindText(1, id);
    s.step();
}

std::vector<OutputConfig> Database::listOutputs() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OutputConfig> out;
    Stmt s(db_, "SELECT id, type, config_json, enabled FROM outputs ORDER BY id");
    while (s.step()) {
        OutputConfig oc;
        oc.id = s.colInt64(0);
        oc.type = s.colText(1);
        oc.configJson = s.colText(2);
        oc.enabled = s.colInt(3) != 0;
        out.push_back(oc);
    }
    return out;
}

int64_t Database::upsertOutput(const OutputConfig& oc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (oc.id != 0) {
        Stmt s(db_, "UPDATE outputs SET type=?, config_json=?, enabled=? WHERE id=?");
        s.bindText(1, oc.type);
        s.bindText(2, oc.configJson);
        s.bindInt(3, oc.enabled ? 1 : 0);
        s.bindInt64(4, oc.id);
        s.step();
        return oc.id;
    }
    Stmt s(db_, "INSERT INTO outputs(type, config_json, enabled) VALUES(?, ?, ?)");
    s.bindText(1, oc.type);
    s.bindText(2, oc.configJson);
    s.bindInt(3, oc.enabled ? 1 : 0);
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

void Database::deleteOutput(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "DELETE FROM outputs WHERE id = ?");
    s.bindInt64(1, id);
    s.step();
}

} // namespace sdrscan
