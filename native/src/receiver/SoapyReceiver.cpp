#include "SoapyReceiver.h"
#include "../dsp/Const.h"
#include "../util/Time.h"

#include <gnuradio/soapy/soapy_types.h>

#include <set>
#include <stdexcept>

namespace sdrscan {

namespace {
// Curated list matching Receiver.py's Receiver_RTLSDR.SAMPLE_RATES - rates known to decimate
// down cleanly, rather than trusting the RTL-SDR's raw (misleadingly continuous) reported range.
const std::vector<int> kRtlSdrSampleRates = {1'024'000, 1'536'000, 1'792'000, 1'920'000, 2'048'000};
} // namespace

SoapyReceiver::SoapyReceiver(ReceiverConfig config,
                              std::function<void(ChannelStatusUpdate)> statusCallback,
                              std::shared_ptr<AudioRingBufferSinkBlock> audioSink)
    : config_(std::move(config)), statusCallback_(std::move(statusCallback)), audioSink_(std::move(audioSink)) {

    std::string dev;
    std::string deviceArg = config_.deviceArg.value_or("");
    if (config_.type == ReceiverType::RTL_SDR) {
        dev = "driver=rtlsdr";
    } else {
        if (!config_.driver.has_value()) {
            throw std::runtime_error("SoapyReceiver: 'driver' is required for SOAPY receiver type");
        }
        dev = "driver=" + *config_.driver;
    }

    source_ = gr::soapy::source::make(dev, "fc32", 1, deviceArg, "", {""}, {""});
    source_->set_gain_mode(0, false);
    source_->set_frequency_correction(0, 0);

    if (!config_.gains.empty()) {
        for (const auto& [name, value] : config_.gains) {
            source_->set_gain(0, name, value);
        }
    } else {
        source_->set_gain(0, config_.gain.value_or(20.0));
    }

    topBlock_ = gr::make_top_block("SDR Rx " + config_.id);
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

    if (windowRunning_) {
        stopCurrentWindow();
    }

    std::unordered_map<std::string, std::shared_ptr<ScanWindow>> rebuilt;
    for (const auto& cfg : *configs) {
        rebuilt[cfg.id] = buildWindow(cfg);
    }

    std::lock_guard<std::mutex> lock(windowsMutex_);
    scanWindowsById_ = std::move(rebuilt);
}

bool SoapyReceiver::startWindow(const std::string& windowId) {
    std::shared_ptr<ScanWindow> window;
    {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        auto it = scanWindowsById_.find(windowId);
        if (it == scanWindowsById_.end()) return false; // config changed out from under us; skip
        window = it->second;
    }

    source_->set_frequency(0, window->hardwareFreq_hz());

    if (!flowgraphStarted_) {
        // First window ever for this receiver: the USB/SDR stream doesn't exist yet, so this
        // is the one time a real start() (which allocates the hardware stream) is needed.
        source_->set_sample_rate(0, window->rfSampleRate());
        topBlock_->connect(source_, 0, window->block(), 0);
        topBlock_->connect(window->block(), 0, audioSink_, 0);
        topBlock_->start();
        flowgraphStarted_ = true;
    } else {
        // Live reconfigure: the hardware stream keeps running throughout (see the
        // flowgraphStarted_ comment in the header for why this matters).
        topBlock_->lock();
        topBlock_->connect(source_, 0, window->block(), 0);
        topBlock_->connect(window->block(), 0, audioSink_, 0);
        topBlock_->unlock();
    }

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
    // See startWindow(): this only detaches this window's blocks from the running flowgraph
    // (topBlock_->stop() is never called here) - the hardware stream is left running so the
    // next startWindow() can retune live instead of re-negotiating the USB stream from
    // scratch. The stream is only actually stopped once, in run()'s final shutdown below.
    topBlock_->lock();
    topBlock_->disconnect(source_, 0, currentWindow_->block(), 0);
    topBlock_->disconnect(currentWindow_->block(), 0, audioSink_, 0);
    topBlock_->unlock();
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
                if (startWindow(nextId)) {
                    onWindowStart(nextId);
                } else {
                    // Window vanished under us (config rebuild race) - release the claim so
                    // the scheduler can hand it to someone else (or drop it) next time.
                    onWindowDone(nextId);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (windowRunning_) stopCurrentWindow();

    // stopCurrentWindow() only detaches blocks now (see its comment) - actually stop the
    // hardware stream here, once, at real receiver shutdown.
    if (flowgraphStarted_) {
        topBlock_->stop();
        topBlock_->wait();
    }
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
