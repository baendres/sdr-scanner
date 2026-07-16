#include "ScanWindow.h"
#include "Const.h"
#include "ChannelBlockFM.h"
#include "ChannelBlockAM.h"
#include "../util/Uuid.h"

#include <gnuradio/filter/firdes.h>
#include <gnuradio/sptr_magic.h>

#include <algorithm>
#include <stdexcept>

namespace sdrscan {

namespace {

std::shared_ptr<ChannelBlockBase> buildChannelBlock(const ChannelConfig& cc,
                                                      int64_t hardwareFreq_hz,
                                                      int rfSampleRate,
                                                      int audioSampleRate,
                                                      std::function<void(ChannelStatusUpdate)> statusCallback) {
    switch (cc.mode) {
        case ChannelMode::FM:
        case ChannelMode::NFM: {
            int deviation_hz = (cc.mode == ChannelMode::NFM) ? 2500 : 5000;
            return gnuradio::make_block_sptr<ChannelBlockFM>(
                cc.id, cc.label, cc.mute, cc.solo, cc.hold, cc.squelchThreshold, cc.audioGain_dB,
                cc.dwellTime_s, cc.freq_hz, hardwareFreq_hz, rfSampleRate, audioSampleRate,
                deviation_hz, cc.ctcssToneHz, statusCallback);
        }
        case ChannelMode::AM:
            return gnuradio::make_block_sptr<ChannelBlockAM>(
                cc.id, cc.label, cc.mute, cc.solo, cc.hold, cc.squelchThreshold, cc.audioGain_dB,
                cc.dwellTime_s, cc.freq_hz, hardwareFreq_hz, rfSampleRate, audioSampleRate,
                statusCallback);
    }
    throw std::runtime_error("buildChannelBlock: unhandled ChannelMode");
}

} // namespace

ScanWindowBlock::ScanWindowBlock(const std::vector<std::shared_ptr<ChannelBlockBase>>& channels,
                                  int audioSampleRate, int globalAudioSampleRate)
    : gr::hier_block2("ScanWindow",
                       gr::io_signature::make(1, 1, sizeof(gr_complex)),
                       gr::io_signature::make(1, 1, sizeof(float))) {

    mixerAdd_ = gr::blocks::add_ff::make(1);

    int channelIdx = 0;
    for (const auto& channel : channels) {
        connect(self(), 0, channel, 0);
        connect(channel, 0, mixerAdd_, channelIdx);
        channelIdx++;
    }

    if (audioSampleRate == globalAudioSampleRate) {
        connect(mixerAdd_, 0, self(), 0);
        return;
    }

    // Need to resample this window's audio to the global rate - find the smallest
    // interpolation/decimation ratio via common-factor reduction (mirrors ScanWindow.py).
    int interp = globalAudioSampleRate;
    int decim = audioSampleRate;
    int n = 2;
    while (n < interp) {
        if (interp % n == 0 && decim % n == 0) {
            interp /= n;
            decim /= n;
        } else {
            n++;
        }
    }

    auto taps = gr::filter::firdes::low_pass(
        1.0, interp, 0.5 * std::min(1.0, static_cast<double>(interp) / decim), 0.05);
    resampler_ = gr::filter::rational_resampler_fff::make(interp, decim, taps);
    connect(mixerAdd_, 0, resampler_, 0);
    connect(resampler_, 0, self(), 0);
}

ScanWindow::ScanWindow(const ScanWindowConfig& config,
                        int rfSampleRate,
                        std::function<void(ChannelStatusUpdate)> statusCallback)
    : id_(config.id.empty() ? makeUuid() : config.id),
      hardwareFreq_hz_(config.hardwareFreq_hz),
      rfSampleRate_(rfSampleRate),
      audioSampleRate_(selectAudioSampleRate(rfSampleRate)) {

    for (const auto& cc : config.channelConfigs) {
        channels_.push_back(buildChannelBlock(cc, hardwareFreq_hz_, rfSampleRate_, audioSampleRate_, statusCallback));
    }

    scanWindowBlock_ = gnuradio::make_block_sptr<ScanWindowBlock>(channels_, audioSampleRate_, AUDIO_SAMPLERATE);
}

std::shared_ptr<ChannelBlockBase> ScanWindow::channelById(const std::string& channelId) const {
    for (const auto& c : channels_) {
        if (c->id() == channelId) return c;
    }
    return nullptr;
}

bool ScanWindow::isActive() {
    bool active = false;
    for (auto& c : channels_) {
        if (c->getStatus() != ChannelStatus::IDLE) {
            active = true;
        }
    }
    return active;
}

double ScanWindow::getMinimumScanTime() const {
    if (!minimumScanTime_.has_value()) {
        double maxTime = 0.1;
        for (const auto& c : channels_) {
            maxTime = std::max(maxTime, c->getMinimumScanTime());
        }
        minimumScanTime_ = maxTime;
    }
    return *minimumScanTime_;
}

int ScanWindow::selectRfSampleRate(const std::vector<int>& availableRates, int64_t rfBandwidth) {
    std::vector<int> candidates;
    for (int r : availableRates) {
        if (r >= rfBandwidth) candidates.push_back(r);
    }
    if (candidates.empty()) {
        throw std::runtime_error("ScanWindow: no available RF sample rate covers the required bandwidth");
    }
    return *std::min_element(candidates.begin(), candidates.end());
}

int ScanWindow::selectAudioSampleRate(int rfSampleRate) {
    if (rfSampleRate % AUDIO_SAMPLERATE == 0) {
        return AUDIO_SAMPLERATE;
    }
    for (int n = rfSampleRate / AUDIO_SAMPLERATE; n > 0; n--) {
        if (rfSampleRate % n == 0) {
            return rfSampleRate / n;
        }
    }
    throw std::runtime_error("ScanWindow: could not find a suitable audio sample rate");
}

} // namespace sdrscan
