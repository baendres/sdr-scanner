#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../config/Types.h"

struct sqlite3;

namespace sdrscan {

// Thin wrapper around SQLite: owns the on-disk config database that replaces
// the old sdrscan.yaml. All reads/writes go through here so the control API's
// live updates and app startup always see the same source of truth.
//
// Safe to call from multiple threads (guarded internally by a mutex) - the
// HTTP server thread pool and the main thread both touch this.
class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Creates tables if they don't exist (idempotent, safe to call every startup).
    void initSchema();

    bool isEmpty(); // true if there are no channels and no receivers configured yet

    // Scanner-wide settings (key/value)
    ScannerSettings loadScannerSettings();
    void saveScannerSetting(const std::string& key, const std::string& value);

    // Channels
    std::vector<ChannelConfig> listChannels();
    void upsertChannel(const ChannelConfig& cc);
    void deleteChannel(const std::string& id);

    // Receivers
    std::vector<ReceiverConfig> listReceivers();
    void upsertReceiver(const ReceiverConfig& rc);
    void deleteReceiver(const std::string& id);

    // Outputs
    std::vector<OutputConfig> listOutputs();
    int64_t upsertOutput(const OutputConfig& oc); // returns id (assigns one for id==0)
    void deleteOutput(int64_t id);

private:
    void exec(const std::string& sql);

    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

} // namespace sdrscan
