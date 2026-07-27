#include "SoapyReceiver.h"
#include "../dsp/Const.h"
#include "../util/Time.h"

#include <gnuradio/soapy/soapy_types.h>
#include <SoapySDR/Device.hpp>

#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace sdrscan {

namespace {
// Curated list matching Receiver.py's Receiver_RTLSDR.SAMPLE_RATES - rates known to decimate
// down cleanly, rather than trusting the RTL-SDR's raw (misleadingly continuous) reported range.
const std::vector<int> kRtlSdrSampleRates = {1'024'000, 1'536'000, 1'792'000, 1'920'000, 2'048'000};

// Receiver.py's README documents 'streamArgs: bufflen=131072,buffers=8' and 'tunePause: 0.020'
// as a matched pair: on a hop, stale samples from the OLD window are still sitting in the
// async read queue, and tunePause exists specifically to let those drain before the new
// window's squelch/demod start trusting what comes out - too short a pause relative to the
// buffer depth means the new window briefly processes old-frequency samples, which the README
// calls out by name as a cause of "invalid squelch breaks" (audible as spurious noise/false
// triggers right after a hop). This is a real per-hop cost: the receiver's entire flowgraph
// produces zero audio samples (for any window, not just the one hopped away from) for the
// full settle duration, since the RF selector is already routed to the discard port and the
// audio selector hasn't been pointed at the new window yet (see startWindow()) - on real
// hardware this showed up as AudioMixer's "falling behind real time" warning, chronically,
// whenever the scanner spent much of its time round-robin-hopping through idle windows.
// This receiver type's streamArgs (below) requests buffers=32 - 4x Python's calibrated
// buffers=8/tunePause=20ms pair - on the theory that a deeper queue survives more completion
// latency (see that comment). Real-hardware logs showed this was never actually taking
// effect: SoapyRTLSDR always logs its own default "Allocating 15 zero-copy buffers"
// regardless of the streamArgs override, so the settle time had been sized for a 32-buffer
// depth that was never real. Scaled to the actual depth instead (15/8 * 20ms rounded up) -
// shrinks the per-hop dead time instead of masking it with a longer scan dwell, which would
// trade away scan responsiveness to fix a cost that was miscalibrated in the first place. If
// buffers=32 needs to genuinely take effect (or "invalid squelch breaks" reappear), revisit
// both together - this value assumes the 15-buffer default is what's actually in use.
constexpr int kRtlSdrTuneSettleMs = 40;
} // namespace

SoapyReceiver::SoapyReceiver(ReceiverConfig config,
                              std::function<void(ChannelStatusUpdate)> statusCallback,
                              std::shared_ptr<AudioRingBufferSinkBlock> audioSink)
    : config_(std::move(config)), statusCallback_(std::move(statusCallback)), audioSink_(std::move(audioSink)) {

    std::string dev;
    std::string streamArgs;
    std::string deviceArg = config_.deviceArg.value_or("");
    if (config_.type == ReceiverType::RTL_SDR) {
        dev = "driver=rtlsdr";
        // SoapyRTLSDR's async read queue defaults to 15 buffers (visible in its own
        // "Allocating 15 zero-copy buffers" log) - sized for bare-metal USB latency. Over a
        // virtualized USB passthrough (e.g. WSL2 usbipd), extra completion latency can run
        // that queue dry, making readStream() hit its own internal timeout repeatedly - which
        // shows up downstream as silence in clean, fixed ~100ms multiples, not the irregular
        // jitter you'd expect from real RF/processing delay. A deeper queue gives the async
        // callback thread enough slack to absorb that latency without starving the reader.
        streamArgs = "buffers=32";
    } else {
        if (!config_.driver.has_value()) {
            throw std::runtime_error("SoapyReceiver: 'driver' is required for SOAPY receiver type");
        }
        dev = "driver=" + *config_.driver;
    }

    source_ = gr::soapy::source::make(dev, "fc32", 1, deviceArg, streamArgs, {""}, {""});
    source_->set_gain_mode(0, false);
    source_->set_frequency_correction(0, 0);

    if (!config_.gains.empty()) {
        for (const auto& [name, value] : config_.gains) {
            source_->set_gain(0, name, value);
        }
    } else {
        source_->set_gain(0, config_.gain.value_or(20.0));
    }
}

std::vector<std::map<std::string, std::string>> SoapyReceiver::scanAvailableDevices() {
    std::vector<std::map<std::string, std::string>> devices;
    try {
        for (const auto& kwargs : SoapySDR::Device::enumerate()) {
            devices.emplace_back(kwargs.begin(), kwargs.end());
        }
    } catch (const std::exception& e) {
        std::cerr << "SoapyReceiver::scanAvailableDevices: enumeration failed: " << e.what() << "\n";
    }
    return devices;
}

std::vector<int> SoapyReceiver::getSampleRates() {
    if (cachedSampleRates_.has_value()) return *cachedSampleRates_;

    if (config_.type == ReceiverType::RTL_SDR) {
        cachedSampleRates_ = kRtlSdrSampleRates;
        return *cachedSampleRates_;
    }

    // Generic Soapy: query the device's supported rates and prefer ones that divide down
    // cleanly to AUDIO_SAMPLERATE (mirrors Receiver_SOAPY.getSampleRates in Receiver.py).
    std::set<int> rates;
    for (const auto& range : source_->get_sample_rate_range(0)) {
        rates.insert(static_cast<int>(range.minimum()));
        rates.insert(static_cast<int>(range.maximum()));
    }

    auto primeFactorCount = [](int64_t n) {
        int count = 0;
        for (int64_t f = 2; f * f <= n; f++) {
            while (n % f == 0) {
                count++;
                n /= f;
            }
        }
        if (n > 1) count++;
        return count;
    };

    std::set<int> preferred;
    for (int r : rates) {
        if (r < MAX_RF_SAMPLERATE && primeFactorCount(r) >= 4 && r % AUDIO_SAMPLERATE == 0) {
            preferred.insert(r);
        }
    }
    if (!preferred.empty()) rates = preferred;

    cachedSampleRates_ = std::vector<int>(rates.begin(), rates.end());
    return *cachedSampleRates_;
}

void SoapyReceiver::postScanWindowConfigs(std::vector<ScanWindowConfig> configs) {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    pendingConfigs_ = std::move(configs);
}

std::shared_ptr<ScanWindow> SoapyReceiver::buildWindow(const ScanWindowConfig& cfg) {
    int rfSampleRate = ScanWindow::selectRfSampleRate(getSampleRates(), cfg.rfBandwidth);
    return std::make_shared<ScanWindow>(cfg, rfSampleRate, statusCallback_);
}

void SoapyReceiver::applyPendingConfigsIfAny() {
    std::optional<std::vector<ScanWindowConfig>> configs;
    {
        std::lock_guard<std::mutex> lock(mailboxMutex_);
        if (pendingConfigs_.has_value()) {
            configs = std::move(pendingConfigs_);
            pendingConfigs_.reset();
        }
    }
    if (!configs.has_value()) return;

    rebuildGraph(std::move(*configs));
}

void SoapyReceiver::rebuildGraph(std::vector<ScanWindowConfig> configs) {
    shutdownGraph();

    if (configs.empty()) {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        scanWindowsById_.clear();
        scanWindowIndexById_.clear();
        return;
    }

    std::unordered_map<std::string, std::shared_ptr<ScanWindow>> rebuilt;
    for (const auto& cfg : configs) {
        rebuilt[cfg.id] = buildWindow(cfg);
    }

    // A fresh top_block each rebuild (rather than reusing one across rebuilds) means there's
    // never a need to explicitly disconnect the previous topology - the old one, and every
    // block exclusive to it, is simply destroyed once this local shared_ptr replaces it. source_
    // is the only block that survives from the previous graph, and GNU Radio blocks are fine
    // being wired into a new flowgraph once their old one has been fully stopped (shutdownGraph()
    // above already guarantees that).
    receiverTopBlock_ = gr::make_top_block("SDR " + config_.id);

    int windowCount = static_cast<int>(rebuilt.size());
    rfDiscardPortIndex_ = windowCount;
    rfSelector_ = gr::blocks::selector::make(sizeof(gr_complex), 0, rfDiscardPortIndex_);
    audioSelector_ = gr::blocks::selector::make(sizeof(float), 0, 0);
    rfDiscardSink_ = gr::blocks::null_sink::make(sizeof(gr_complex));

    receiverTopBlock_->connect(source_, 0, rfSelector_, 0);

    std::unordered_map<std::string, int> indexById;
    int idx = 0;
    for (auto& [id, window] : rebuilt) {
        indexById[id] = idx;
        receiverTopBlock_->connect(rfSelector_, idx, window->block(), 0);
        receiverTopBlock_->connect(window->block(), 0, audioSelector_, idx);
        idx++;
    }
    receiverTopBlock_->connect(rfSelector_, rfDiscardPortIndex_, rfDiscardSink_, 0);
    receiverTopBlock_->connect(audioSelector_, 0, audioSink_, 0);

    {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        scanWindowsById_ = std::move(rebuilt);
        scanWindowIndexById_ = std::move(indexById);
    }

    graphBuilt_ = true;
    ensureRunning();
}

void SoapyReceiver::ensureRunning() {
    if (!graphRunning_ && receiverTopBlock_) {
        receiverTopBlock_->start();
        graphRunning_ = true;
    }
}

void SoapyReceiver::shutdownGraph() {
    if (graphRunning_ && receiverTopBlock_) {
        receiverTopBlock_->stop();
        receiverTopBlock_->wait();
    }
    graphRunning_ = false;
    graphBuilt_ = false;
    currentWindow_.reset();
    windowRunning_ = false;
    windowTimeout_ = 0.0;
    rfDiscardPortIndex_ = -1;
    rfSelector_.reset();
    audioSelector_.reset();
    rfDiscardSink_.reset();
    receiverTopBlock_.reset();
}

bool SoapyReceiver::startWindow(const std::string& windowId) {
    if (!graphBuilt_) return false; // no windows configured yet

    std::shared_ptr<ScanWindow> window;
    int windowIdx = -1;
    {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        auto it = scanWindowsById_.find(windowId);
        if (it == scanWindowsById_.end()) return false; // config changed out from under us; skip
        window = it->second;
        auto idxIt = scanWindowIndexById_.find(windowId);
        if (idxIt == scanWindowIndexById_.end()) return false; // shouldn't happen, but be defensive
        windowIdx = idxIt->second;
    }

    try {
        if (window->rfSampleRate() != lastSetSampleRate_) {
            // Changing sample rate takes ~100ms on real hardware - avoid it unless the newly
            // selected window actually needs a different one (matches Receiver.py's
            // _tuneSource optimization).
            source_->set_sample_rate(0, window->rfSampleRate());
            lastSetSampleRate_ = window->rfSampleRate();
        }
        source_->set_frequency(0, window->hardwareFreq_hz());
        ensureRunning();
    } catch (const std::exception& e) {
        // GNU Radio/SoapySDR surface hardware and USB communication failures (a flaky I2C
        // write to the tuner, a dropped USB connection, ...) as exceptions - this must not be
        // allowed to escape this receiver's dedicated thread uncaught, since an uncaught
        // exception on any thread calls std::terminate() and aborts the *entire* process,
        // taking every other receiver and channel down with it. Treat it the same as "window
        // vanished under us": skip this attempt and let the scheduler retry (this window, or
        // another) after a cooldown.
        std::cerr << "SoapyReceiver " << config_.id << ": failed to start window " << windowId
                   << ": " << e.what() << "\n";
        nextStartAttemptAllowedAt_ = nowUnixSeconds() + 1.0;
        return false;
    }

    // Let the retuned signal settle before routing it into this window's squelch/demod chain -
    // feeding it transitional/settling samples right after a retune can trip a false
    // squelch-open or a demod glitch (matches Receiver.py's tunePause - see kRtlSdrTuneSettleMs
    // for why RTL-SDR needs longer than Python's default here).
    int settleMs = (config_.type == ReceiverType::RTL_SDR) ? kRtlSdrTuneSettleMs : 20;
    std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));

    hopCount_++;
    hopSettleMsTotal_ += settleMs;

    audioSelector_->set_input_index(windowIdx);
    rfSelector_->set_output_index(windowIdx);

    currentWindow_ = window;
    windowTimeout_ = nowUnixSeconds() + currentWindow_->getMinimumScanTime();
    windowRunning_ = true;
    return true;
}

void SoapyReceiver::stopCurrentWindow() {
    if (!currentWindow_) {
        windowRunning_ = false;
        return;
    }
    // No GNU Radio topology change here - the flowgraph keeps running unchanged, this just
    // stops routing real RF samples to any window (the discard port gets them instead).
    if (rfSelector_) rfSelector_->set_output_index(rfDiscardPortIndex_);
    currentWindow_.reset();
    windowRunning_ = false;
}

void SoapyReceiver::checkCurrentWindow() {
    if (!currentWindow_) return;
    if (!currentWindow_->isActive() && nowUnixSeconds() > windowTimeout_) {
        stopCurrentWindow();
    }
}

void SoapyReceiver::run(const std::function<std::string()>& nextWindowIdProvider,
                         const std::function<void(const std::string&)>& onWindowStart,
                         const std::function<void(const std::string&)>& onWindowDone) {
    while (!stopFlag_) {
        applyPendingConfigsIfAny();

        if (windowRunning_) {
            bool wasRunning = windowRunning_;
            std::string finishedId = currentWindow_ ? currentWindow_->id() : "";
            checkCurrentWindow();
            if (wasRunning && !windowRunning_ && !finishedId.empty()) {
                onWindowDone(finishedId);
            }
        }

        if (!windowRunning_) {
            std::string nextId = nextWindowIdProvider();
            if (!nextId.empty()) {
                // Still have to claim-then-release via the provider/onWindowDone pair even
                // during a post-failure cooldown (see startWindow()'s catch block) - skipping
                // the claim itself would leave Scanner's round-robin scheduling state stuck
                // thinking this window is permanently assigned to this receiver.
                bool started = nowUnixSeconds() >= nextStartAttemptAllowedAt_ && startWindow(nextId);
                if (started) {
                    onWindowStart(nextId);
                } else {
                    // Window vanished under us (config rebuild race), startWindow() failed, or
                    // we're still cooling down after an earlier failure - release the claim so
                    // the scheduler can hand it to someone else (or retry) next time.
                    onWindowDone(nextId);
                }
            }
        }

        double now = nowUnixSeconds();
        if (now - lastHopReportAt_ >= 1.0) {
            if (lastHopReportAt_ != 0.0) {
                std::cerr << "SoapyReceiver " << config_.id << ": " << hopCount_
                          << " hops in the last ~1s, ~" << hopSettleMsTotal_
                          << "ms spent settling (~" << (hopSettleMsTotal_ / 10.0) << "% of wall time)\n";
            }
            hopCount_ = 0;
            hopSettleMsTotal_ = 0.0;
            lastHopReportAt_ = now;
        }

        // This interval sets the polling rate for checkCurrentWindow()/getStatus() on every
        // channel in the active window - real-hardware CPU profiling (top -H) found a 1ms
        // interval here pinned one core near 100%, from the sheer call/lock-acquisition
        // frequency against live GNU Radio blocks (unmuted() reads, etc), not from any specific
        // redundant call (those were separately throttled - see adaptiveThresholdChanged()/
        // setAudioGateMuted()). Nothing downstream needs sub-10ms granularity: window-hop timing
        // works in ~100ms-1s increments (getMinimumScanTime()), squelch debounce is 50ms
        // (SQUELCH_DEBOUNCE_SECONDS), and RSSI updates at 4Hz (RSSI_UPDATE_FREQ_HZ) - so widening
        // this cuts the polling-driven overhead by ~10x with no meaningful loss of
        // responsiveness anywhere else in the system.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (windowRunning_) stopCurrentWindow();
    shutdownGraph();
}

void SoapyReceiver::stop() {
    stopFlag_ = true;
}

void SoapyReceiver::withChannel(const std::string& channelId, const std::function<void(ChannelBlockBase&)>& fn) {
    std::lock_guard<std::mutex> lock(windowsMutex_);
    for (auto& [windowId, window] : scanWindowsById_) {
        auto channel = window->channelById(channelId);
        if (channel) {
            fn(*channel);
        }
    }
}

} // namespace sdrscan
