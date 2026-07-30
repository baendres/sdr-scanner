#include "Scanner.h"
#include "../dsp/Const.h"
#include "../util/Time.h"
#include "../util/Uuid.h"
#include "../audio/AudioOutput.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace sdrscan {

Scanner::Scanner(Database& db) : db_(db) {}

Scanner::~Scanner() {
    stop();
}

void Scanner::loadConfigFromDatabase() {
    settings_ = db_.loadScannerSettings();

    std::lock_guard<std::mutex> lock(configMutex_);
    channelConfigsById_.clear();
    for (auto& cc : db_.listChannels()) {
        channelConfigsById_[cc.id] = cc;
    }
    receiverConfigs_ = db_.listReceivers();
    outputConfigs_ = db_.listOutputs();
}

void Scanner::start() {
    // Zero receivers is a valid (if useless) starting state - notably including a fresh
    // database on first run. Scanner still starts (audioMixer_ with zero input streams just
    // mixes silence, buildWindows() no-ops on an empty receivers_), so the web UI comes up and
    // a receiver can be added/enabled through it - see native/README.md and the settings
    // page's "Scan for Receivers" button. Previously this threw here, which meant the process
    // - including the HTTP server - never started at all without at least one receiver already
    // in the database, a chicken-and-egg problem for a first-time setup.
    std::vector<std::shared_ptr<AudioOutput>> outputs;
    for (const auto& oc : outputConfigs_) {
        if (!oc.enabled) continue;
        outputs.push_back(createAudioOutput(oc));
    }
    if (outputs.empty()) {
        outputs.push_back(createAudioOutput(OutputConfig{0, "local", "{}", true}));
    }
    // Disabled receivers stay in receiverConfigs_/the DB (so the settings page can still show
    // and re-enable them) but shouldn't actually open hardware or spin up a scan thread - same
    // "enabled" semantics as outputConfigs_ just above.
    std::vector<ReceiverConfig> enabledReceivers;
    for (auto& rc : receiverConfigs_) {
        if (rc.enabled) enabledReceivers.push_back(rc);
    }

    audioMixer_ = std::make_unique<AudioMixer>(static_cast<int>(enabledReceivers.size()), outputs);

    for (size_t i = 0; i < enabledReceivers.size(); i++) {
        auto sink = audioMixer_->createReceiverSink(static_cast<int>(i));
        receivers_.push_back(std::make_unique<SoapyReceiver>(
            enabledReceivers[i],
            [this](ChannelStatusUpdate update) { onChannelStatus(std::move(update)); },
            sink));
    }

    buildWindows();
    audioMixer_->start();

    for (auto& receiver : receivers_) {
        SoapyReceiver* rx = receiver.get();
        std::string rxId = rx->id();
        receiverThreads_.emplace_back([this, rx, rxId]() {
            rx->run(
                [this, rxId]() { return getNextScanWindowId(rxId); },
                [this, rxId](const std::string& windowId) { onReceiverWindowStart(rxId, windowId); },
                [this, rxId](const std::string& windowId) { onReceiverWindowDone(rxId, windowId); });
        });
    }

    maintenanceThread_ = std::thread(&Scanner::runMaintenanceLoop, this);
}

void Scanner::stop() {
    if (stopFlag_.exchange(true)) return; // already stopped/stopping

    for (auto& receiver : receivers_) receiver->stop();
    for (auto& t : receiverThreads_) {
        if (t.joinable()) t.join();
    }
    if (maintenanceThread_.joinable()) maintenanceThread_.join();
    if (audioMixer_) audioMixer_->stop();
}

ScannerSnapshot Scanner::getSnapshot() const {
    ScannerSnapshot snapshot;
    snapshot.settings = settings_;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        for (const auto& [id, cc] : channelConfigsById_) snapshot.channels.push_back(cc);
        snapshot.receivers = receiverConfigs_;
        snapshot.outputs = outputConfigs_;
    }
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        for (const auto& [id, status] : channelStatusById_) snapshot.channelStatuses.push_back(status);
    }
    snapshot.restartRequired = restartRequired_;
    return snapshot;
}

void Scanner::emit(ScannerEvent event) {
    if (eventCallback_) eventCallback_(event);
}

///
// Hot updates

void Scanner::updateChannel(const std::string& channelId,
                             const std::function<void(ChannelConfig&)>& mutator,
                             const std::function<void(ChannelBlockBase&, const ChannelConfig&)>& liveUpdate) {
    ChannelConfig updated;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        auto it = channelConfigsById_.find(channelId);
        if (it == channelConfigsById_.end()) {
            throw std::runtime_error("Channel not found: " + channelId);
        }
        mutator(it->second);
        updated = it->second;
        db_.upsertChannel(updated);
    }

    for (auto& receiver : receivers_) {
        receiver->withChannel(channelId, [&](ChannelBlockBase& block) { liveUpdate(block, updated); });
    }

    ScannerEvent event;
    event.type = ScannerEventType::ChannelConfigChanged;
    event.channelConfig = updated;
    emit(event);
}

void Scanner::setChannelSquelch(const std::string& channelId, double squelchThreshold) {
    updateChannel(
        channelId,
        // An explicit absolute value wins over adaptive mode - mirrors ChannelBlockBase's own
        // setSquelchValue() clearing squelchNoiseMargin_dB_, so the persisted config and the
        // live block stay in sync (see ChannelConfig::squelchNoiseMargin_dB).
        [&](ChannelConfig& cc) { cc.squelchThreshold = squelchThreshold; cc.squelchNoiseMargin_dB.reset(); },
        [](ChannelBlockBase& block, const ChannelConfig& cc) { block.setSquelchValue(cc.squelchThreshold); });
}

void Scanner::setChannelCtcssTone(const std::string& channelId, std::optional<double> toneHz) {
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) { cc.ctcssToneHz = toneHz; },
        [](ChannelBlockBase& block, const ChannelConfig& cc) { block.setCtcssTone(cc.ctcssToneHz); });
}

void Scanner::setChannelSquelchNoiseMargin(const std::string& channelId, std::optional<double> marginDb) {
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) { cc.squelchNoiseMargin_dB = marginDb; },
        [](ChannelBlockBase& block, const ChannelConfig& cc) { block.setSquelchNoiseMargin(cc.squelchNoiseMargin_dB); });
}

void Scanner::setChannelNoiseSquelchThreshold(const std::string& channelId, std::optional<double> thresholdDb) {
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) { cc.noiseSquelchThreshold_dB = thresholdDb; },
        [](ChannelBlockBase& block, const ChannelConfig& cc) { block.setNoiseSquelchThreshold(cc.noiseSquelchThreshold_dB); });
}

void Scanner::setChannelAudioGain(const std::string& channelId, double audioGain_dB) {
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) { cc.audioGain_dB = audioGain_dB; },
        [](ChannelBlockBase& block, const ChannelConfig& cc) { block.setAudioGain(cc.audioGain_dB); });
}

void Scanner::setChannelDwellTime(const std::string& channelId, double dwellTime_s) {
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) { cc.dwellTime_s = dwellTime_s; },
        [](ChannelBlockBase& block, const ChannelConfig& cc) { block.setDwellTime(cc.dwellTime_s); });
}

void Scanner::setChannelMute(const std::string& channelId, bool mute) {
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) { cc.mute = mute; },
        [](ChannelBlockBase& block, const ChannelConfig& cc) { block.setMute(cc.mute); });
}

void Scanner::setChannelSolo(const std::string& channelId, TriBool solo) {
    // Matches Python's ChannelSolo semantics: soloing one channel implicitly puts every
    // *other* channel into the tri-state (muted-by-solo unless also soloed); clearing the
    // last active solo returns every channel to plain mute-based behavior.
    std::vector<std::string> allChannelIds;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        auto it = channelConfigsById_.find(channelId);
        if (it == channelConfigsById_.end()) throw std::runtime_error("Channel not found: " + channelId);
        it->second.solo = solo;
        bool soloActive = (solo.has_value() && *solo) ||
                           std::any_of(channelConfigsById_.begin(), channelConfigsById_.end(),
                                       [](const auto& kv) { return kv.second.solo.has_value() && *kv.second.solo; });
        for (auto& [id, cc] : channelConfigsById_) {
            cc.solo = soloActive ? cc.solo : std::nullopt;
            db_.upsertChannel(cc);
            allChannelIds.push_back(id);
        }
    }

    for (const auto& id : allChannelIds) {
        TriBool value;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            value = channelConfigsById_.at(id).solo;
        }
        for (auto& receiver : receivers_) {
            receiver->withChannel(id, [&](ChannelBlockBase& block) { block.setSolo(value); });
        }
        ScannerEvent event;
        event.type = ScannerEventType::ChannelConfigChanged;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            event.channelConfig = channelConfigsById_.at(id);
        }
        emit(event);
    }
}

void Scanner::setChannelHold(const std::string& channelId, bool hold) {
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) { cc.hold = hold; },
        [](ChannelBlockBase& block, const ChannelConfig& cc) { block.setHold(cc.hold); });
}

void Scanner::setChannelForceActive(const std::string& channelId, bool forceActive) {
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) { cc.forceActive = forceActive; },
        [](ChannelBlockBase& block, const ChannelConfig& cc) { block.setForceActive(cc.forceActive); });
}

void Scanner::setChannelEnabled(const std::string& channelId, bool enabled) {
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) {
            cc.enabled = enabled;
            cc.disableUntil.reset();
        },
        [](ChannelBlockBase&, const ChannelConfig&) { /* enable/disable affects window membership, see buildWindows() */ });
    buildWindows();
}

void Scanner::setChannelDisableUntil(const std::string& channelId, double disableUntilUnixTime) {
    if (nowUnixSeconds() >= disableUntilUnixTime) {
        throw std::runtime_error("disableUntil must be in the future");
    }
    updateChannel(
        channelId,
        [&](ChannelConfig& cc) {
            cc.enabled = false;
            cc.disableUntil = disableUntilUnixTime;
        },
        [](ChannelBlockBase&, const ChannelConfig&) {});
    buildWindows();
}

///
// Structural updates

std::string Scanner::addChannel(ChannelConfig cc) {
    if (cc.id.empty()) cc.id = makeUuid();
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        channelConfigsById_[cc.id] = cc;
        db_.upsertChannel(cc);
    }
    buildWindows();
    return cc.id;
}

void Scanner::removeChannel(const std::string& channelId) {
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        channelConfigsById_.erase(channelId);
        db_.deleteChannel(channelId);
    }
    buildWindows();
}

void Scanner::editChannel(const ChannelConfig& cc) {
    if (cc.id.empty()) throw std::runtime_error("editChannel: id is required");
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        if (channelConfigsById_.find(cc.id) == channelConfigsById_.end()) {
            throw std::runtime_error("Channel not found: " + cc.id);
        }
        channelConfigsById_[cc.id] = cc;
        db_.upsertChannel(cc);
    }
    buildWindows();
    ScannerEvent event;
    event.type = ScannerEventType::ChannelConfigChanged;
    event.channelConfig = cc;
    emit(event);
}

std::string Scanner::upsertReceiverConfig(ReceiverConfig rc) {
    if (rc.id.empty()) rc.id = makeUuid();
    std::lock_guard<std::mutex> lock(configMutex_);
    db_.upsertReceiver(rc);
    // Keep the in-memory cache (what getSnapshot() reads) in sync with the database even though
    // the change won't reach the live receivers_/audioMixer_ until restart - otherwise the
    // settings page wouldn't see its own pending edit until then.
    auto it = std::find_if(receiverConfigs_.begin(), receiverConfigs_.end(),
                            [&](const ReceiverConfig& existing) { return existing.id == rc.id; });
    if (it != receiverConfigs_.end()) {
        *it = rc;
    } else {
        receiverConfigs_.push_back(rc);
    }
    restartRequired_ = true;
    return rc.id;
}

void Scanner::deleteReceiverConfig(const std::string& receiverId) {
    std::lock_guard<std::mutex> lock(configMutex_);
    db_.deleteReceiver(receiverId);
    receiverConfigs_.erase(
        std::remove_if(receiverConfigs_.begin(), receiverConfigs_.end(),
                        [&](const ReceiverConfig& rc) { return rc.id == receiverId; }),
        receiverConfigs_.end());
    restartRequired_ = true;
}

int64_t Scanner::upsertOutputConfig(OutputConfig oc) {
    std::lock_guard<std::mutex> lock(configMutex_);
    int64_t id = db_.upsertOutput(oc);
    oc.id = id;
    auto it = std::find_if(outputConfigs_.begin(), outputConfigs_.end(),
                            [&](const OutputConfig& existing) { return existing.id == id; });
    if (it != outputConfigs_.end()) {
        *it = oc;
    } else {
        outputConfigs_.push_back(oc);
    }
    restartRequired_ = true;
    return id;
}

void Scanner::deleteOutputConfig(int64_t outputId) {
    std::lock_guard<std::mutex> lock(configMutex_);
    db_.deleteOutput(outputId);
    outputConfigs_.erase(
        std::remove_if(outputConfigs_.begin(), outputConfigs_.end(),
                        [&](const OutputConfig& oc) { return oc.id == outputId; }),
        outputConfigs_.end());
    restartRequired_ = true;
}

void Scanner::setMaxChannelsPerWindow(int maxChannelsPerWindow) {
    // buildWindows() resizes each window's channel list down to this many, then removes exactly
    // those channels from the allocation set - at <= 0 that resize empties the list without
    // removing anything, so the allocation loop never terminates. Reject before it ever reaches
    // buildWindows() (this throws; HttpServer's request handler turns it into a 400).
    if (maxChannelsPerWindow <= 0) {
        throw std::runtime_error("maxChannelsPerWindow must be positive");
    }
    settings_.maxChannelsPerWindow = maxChannelsPerWindow;
    db_.saveScannerSetting("maxChannelsPerWindow", std::to_string(maxChannelsPerWindow));
    buildWindows();
}

///
// Scan window building - direct port of Scanner.py's buildWindows()

void Scanner::buildWindows() {
    if (receivers_.empty()) return; // called before start(); real build happens in start()

    int64_t bandwidth = -1;
    for (auto& receiver : receivers_) {
        int64_t maxUsable = -1;
        for (int rate : receiver->getSampleRates()) {
            if (rate <= MAX_RF_SAMPLERATE) maxUsable = std::max<int64_t>(maxUsable, rate);
        }
        if (maxUsable < 0) throw std::runtime_error("Receiver has no usable sample rate <= MAX_RF_SAMPLERATE");
        bandwidth = (bandwidth < 0) ? maxUsable : std::min(bandwidth, maxUsable);
    }

    constexpr int64_t kBandEdgeMargin = 200'000;

    std::vector<ChannelConfig> enabledChannels;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        for (auto& [id, cc] : channelConfigsById_) {
            if (cc.isEnabledNow(nowUnixSeconds())) enabledChannels.push_back(cc);
        }
    }

    std::set<int64_t> freqsToAllocate;
    for (auto& cc : enabledChannels) freqsToAllocate.insert(cc.freq_hz);

    std::vector<ScanWindowConfig> newWindows;
    while (!freqsToAllocate.empty()) {
        int64_t lowFreq = *freqsToAllocate.begin();
        int64_t hardwareFreq = lowFreq + bandwidth / 2 - kBandEdgeMargin;
        int64_t highFreq = 2 * hardwareFreq - lowFreq;

        std::vector<ChannelConfig> ccs;
        for (auto& cc : enabledChannels) {
            if (freqsToAllocate.count(cc.freq_hz) && cc.freq_hz >= lowFreq && cc.freq_hz <= highFreq) {
                ccs.push_back(cc);
            }
        }
        std::sort(ccs.begin(), ccs.end(), [](const auto& a, const auto& b) { return a.freq_hz < b.freq_hz; });
        if (static_cast<int>(ccs.size()) > settings_.maxChannelsPerWindow) {
            ccs.resize(settings_.maxChannelsPerWindow);
        }
        for (auto& cc : ccs) freqsToAllocate.erase(cc.freq_hz);

        ScanWindowConfig swc;
        swc.id = makeUuid();
        swc.hardwareFreq_hz = hardwareFreq;
        swc.rfBandwidth = bandwidth;
        swc.channelConfigs = ccs;
        newWindows.push_back(swc);
    }

    {
        std::lock_guard<std::mutex> lock(scheduleMutex_);
        scanWindowConfigs_ = newWindows;
    }

    for (auto& receiver : receivers_) {
        receiver->postScanWindowConfigs(newWindows);
    }

    ScannerEvent event;
    event.type = ScannerEventType::ScanWindowConfigsChanged;
    emit(event);
}

std::string Scanner::getNextScanWindowId(const std::string& receiverId) {
    std::lock_guard<std::mutex> lock(scheduleMutex_);

    std::set<std::string> running;
    for (auto& [rxId, windowId] : receiverCurrentWindow_) {
        if (!windowId.empty()) running.insert(windowId);
    }

    std::string targetId;
    double targetTime = 0.0;
    for (auto& swc : scanWindowConfigs_) {
        if (running.count(swc.id)) continue;
        double t = windowLastScan_.count(swc.id) ? windowLastScan_[swc.id] : 0.0;
        if (targetId.empty() || t < targetTime) {
            targetId = swc.id;
            targetTime = t;
        }
    }

    if (!targetId.empty()) receiverCurrentWindow_[receiverId] = targetId;
    return targetId;
}

void Scanner::onReceiverWindowStart(const std::string& receiverId, const std::string& windowId) {
    ScannerEvent event;
    event.type = ScannerEventType::ScanWindowStart;
    event.windowId = windowId;
    event.receiverId = receiverId;
    emit(event);
}

void Scanner::onReceiverWindowDone(const std::string& receiverId, const std::string& windowId) {
    {
        std::lock_guard<std::mutex> lock(scheduleMutex_);
        receiverCurrentWindow_[receiverId] = "";
        windowLastScan_[windowId] = nowUnixSeconds();
    }
    ScannerEvent event;
    event.type = ScannerEventType::ScanWindowDone;
    event.windowId = windowId;
    event.receiverId = receiverId;
    emit(event);
}

void Scanner::onChannelStatus(ChannelStatusUpdate update) {
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        channelStatusById_[update.channelId] = update;
    }
    ScannerEvent event;
    event.type = ScannerEventType::ChannelStatusChanged;
    event.channelStatus = update;
    emit(event);
}

void Scanner::runMaintenanceLoop() {
    while (!stopFlag_) {
        // Port of Scanner.py's `if not self.audioServerProcess.is_alive(): ...` watchdog - see
        // setAudioMixerDiedCallback's header comment for why this checks a heartbeat rather
        // than "is the thread still running". Checked on this loop's existing ~5s cadence
        // rather than a tighter one: this only ever fires on a genuine, rare failure, not a
        // hot path worth optimizing the detection latency of.
        //
        // Deliberately doesn't set stopFlag_ or otherwise try to tear things down itself - that
        // would make the real Scanner::stop() (called once the callback below causes the
        // process to actually exit) see stopFlag_ already true and skip its own cleanup
        // entirely (its guard against being run twice - see stop()'s first line). Just ends
        // this loop and leaves the rest of shutdown to whoever handles the callback, the same
        // pattern HttpServer's requestShutdown_ already uses.
        if (audioMixer_ && !audioMixer_->isAlive()) {
            std::cerr << "Scanner: AudioMixer not alive - stopping\n";
            if (audioMixerDiedCallback_) audioMixerDiedCallback_();
            break;
        }

        double now = nowUnixSeconds();
        std::vector<ChannelConfig> reenabled;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            for (auto& [id, cc] : channelConfigsById_) {
                if (cc.disableUntil.has_value() && now > *cc.disableUntil) {
                    cc.enabled = true;
                    cc.disableUntil.reset();
                    db_.upsertChannel(cc);
                    reenabled.push_back(cc);
                }
            }
        }
        if (!reenabled.empty()) {
            buildWindows();
            for (auto& cc : reenabled) {
                ScannerEvent event;
                event.type = ScannerEventType::ChannelConfigChanged;
                event.channelConfig = cc;
                emit(event);
            }
        }

        for (int i = 0; i < 50 && !stopFlag_; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace sdrscan
