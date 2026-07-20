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

// ~0.5s of headroom at the highest RF rate this app will ever configure - generous enough to
// absorb the brief gap while windowTopBlock_ is being reconfigured during a hop without ever
// overflowing (see RfRingBuffer.h).
constexpr size_t kRfRingBufferCapacity = static_cast<size_t>(MAX_RF_SAMPLERATE) / 2;
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

    rfRingBuffer_ = std::make_shared<RfRingBuffer>(kRfRingBufferCapacity);

    captureTopBlock_ = gr::make_top_block("SDR Capture " + config_.id);
    rfRingBufferSink_ = std::make_shared<RfRingBufferSinkBlock>(rfRingBuffer_);
    captureTopBlock_->connect(source_, 0, rfRingBufferSink_, 0);

    windowTopBlock_ = gr::make_top_block("SDR Window " + config_.id);
    rfRingBufferSource_ = std::make_shared<RfRingBufferSourceBlock>(rfRingBuffer_);
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

    try {
        source_->set_frequency(0, window->hardwareFreq_hz());

        if (!captureStarted_) {
            // First window ever for this receiver: the USB/SDR stream doesn't exist yet, so
            // this is the one time captureTopBlock_ (and therefore the hardware) actually
            // starts. It is never stopped again until receiver shutdown - see the header
            // comment.
            source_->set_sample_rate(0, window->rfSampleRate());
            captureTopBlock_->start();
            captureStarted_ = true;
        }

        // windowTopBlock_ contains no hardware, so freely reconfiguring/restarting it per hop
        // is cheap - no USB stream re-negotiation, unlike the capture side.
        windowTopBlock_->connect(rfRingBufferSource_, 0, window->block(), 0);
        windowTopBlock_->connect(window->block(), 0, audioSink_, 0);
        windowTopBlock_->start();
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
    windowTopBlock_->stop();
    windowTopBlock_->wait();
    windowTopBlock_->disconnect(rfRingBufferSource_, 0, currentWindow_->block(), 0);
    windowTopBlock_->disconnect(currentWindow_->block(), 0, audioSink_, 0);
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

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (windowRunning_) stopCurrentWindow();

    // The hardware capture flowgraph is only ever stopped here, at real receiver shutdown.
    if (captureStarted_) {
        captureTopBlock_->stop();
        captureTopBlock_->wait();
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
