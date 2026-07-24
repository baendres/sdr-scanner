#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../config/Types.h"
#include "../db/Database.h"
#include "../dsp/ScanWindow.h"
#include "../receiver/SoapyReceiver.h"
#include "../audio/AudioMixer.h"
#include "ScannerEvent.h"

namespace sdrscan {

struct ScannerSnapshot {
    ScannerSettings settings;
    std::vector<ChannelConfig> channels;
    std::vector<ReceiverConfig> receivers;
    std::vector<OutputConfig> outputs;
    std::vector<ChannelStatusUpdate> channelStatuses;
    // True once a receiver/output config has been added/edited/deleted since this process
    // started - those are database-only changes (see upsertReceiverConfig's header note) that
    // won't reach the live receivers_/audioMixer_ until sdrscan restarts. Drives the settings
    // page's "restart needed" banner.
    bool restartRequired = false;
};

// Top-level orchestrator, direct port of Scanner.py: owns the config (backed by Database
// instead of a YAML file), builds/rebuilds ScanWindows across receivers, round-robins scan
// assignments, and is the single point every control-API mutation goes through - see
// native/README.md's "no restart mechanism" for how a call here reaches a live GNU Radio
// block with no flowgraph restart.
class Scanner {
public:
    explicit Scanner(Database& db);
    ~Scanner();

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    void loadConfigFromDatabase();
    void start();
    void stop();

    ScannerSnapshot getSnapshot() const;

    void setEventCallback(std::function<void(const ScannerEvent&)> cb) { eventCallback_ = std::move(cb); }

    // Notified if AudioMixer's thread is ever detected to have silently died (hung or returned
    // without an exception - see AudioMixer::isAlive()'s header comment for why this can't
    // just be "is the thread still running"). Port of Scanner.py's
    // `if not self.audioServerProcess.is_alive(): ...` watchdog - there, a dead AudioServer
    // subprocess left everything else running with silently-dead audio output, so it was
    // treated as fatal and the whole scanner stopped. Optional; if unset, a dead mixer is
    // still logged but nothing else acts on it.
    void setAudioMixerDiedCallback(std::function<void()> cb) { audioMixerDiedCallback_ = std::move(cb); }

    ///
    // Hot updates - applied to every receiver's live copy of the channel with no flowgraph
    // restart, and persisted to the database. Throw std::runtime_error if the id is unknown.

    void setChannelSquelch(const std::string& channelId, double squelchThreshold);
    void setChannelCtcssTone(const std::string& channelId, std::optional<double> toneHz);
    void setChannelAudioGain(const std::string& channelId, double audioGain_dB);
    void setChannelDwellTime(const std::string& channelId, double dwellTime_s);
    void setChannelMute(const std::string& channelId, bool mute);
    void setChannelSolo(const std::string& channelId, TriBool solo);
    void setChannelHold(const std::string& channelId, bool hold);
    void setChannelForceActive(const std::string& channelId, bool forceActive);
    void setChannelEnabled(const std::string& channelId, bool enabled);
    void setChannelDisableUntil(const std::string& channelId, double disableUntilUnixTime);

    ///
    // Structural updates - trigger an in-process ScanWindow rebuild (brief pause on affected
    // receivers only), still with no container/process restart.

    std::string addChannel(ChannelConfig cc); // cc.id is assigned if empty; returns the id
    void removeChannel(const std::string& channelId);
    // Full-record edit of an existing channel (frequency/label/mode, plus everything addChannel
    // accepts) - same upsert-then-rebuild path as addChannel, just requires the id already exist.
    void editChannel(const ChannelConfig& cc);
    void setMaxChannelsPerWindow(int maxChannelsPerWindow);

    ///
    // Receiver/output config - database-only (see native/README.md): these are read once at
    // startup, so edits here take effect on the next restart rather than live. No live receiver
    // or audio output ever gets touched by these.

    std::string upsertReceiverConfig(ReceiverConfig rc); // rc.id is assigned if empty; returns the id
    void deleteReceiverConfig(const std::string& receiverId);
    int64_t upsertOutputConfig(OutputConfig oc); // oc.id is assigned if 0; returns the id
    void deleteOutputConfig(int64_t outputId);

private:
    void buildWindows();
    void runMaintenanceLoop();
    std::string getNextScanWindowId(const std::string& receiverId);
    void onReceiverWindowStart(const std::string& receiverId, const std::string& windowId);
    void onReceiverWindowDone(const std::string& receiverId, const std::string& windowId);
    void onChannelStatus(ChannelStatusUpdate update);

    // Applies `mutator` to the stored ChannelConfig (under lock), persists it, fans the
    // result out to every receiver's live copy via `liveUpdate`, and emits a config-changed
    // event. Shared plumbing for all the hot-update setters above.
    void updateChannel(const std::string& channelId,
                        const std::function<void(ChannelConfig&)>& mutator,
                        const std::function<void(ChannelBlockBase&, const ChannelConfig&)>& liveUpdate);

    void emit(ScannerEvent event);

    Database& db_;
    ScannerSettings settings_;

    mutable std::mutex configMutex_;
    std::unordered_map<std::string, ChannelConfig> channelConfigsById_;
    std::vector<ReceiverConfig> receiverConfigs_;
    std::vector<OutputConfig> outputConfigs_;
    std::atomic<bool> restartRequired_{false}; // see ScannerSnapshot::restartRequired

    mutable std::mutex scheduleMutex_;
    std::vector<ScanWindowConfig> scanWindowConfigs_;
    std::unordered_map<std::string, std::string> receiverCurrentWindow_; // rxId -> windowId ("" idle)
    std::unordered_map<std::string, double> windowLastScan_;             // windowId -> unix time

    mutable std::mutex statusMutex_;
    std::unordered_map<std::string, ChannelStatusUpdate> channelStatusById_;

    std::vector<std::unique_ptr<SoapyReceiver>> receivers_;
    std::vector<std::thread> receiverThreads_;
    std::unique_ptr<AudioMixer> audioMixer_;

    std::function<void(const ScannerEvent&)> eventCallback_;
    std::function<void()> audioMixerDiedCallback_;

    std::atomic<bool> stopFlag_{false};
    std::thread maintenanceThread_;
};

} // namespace sdrscan
