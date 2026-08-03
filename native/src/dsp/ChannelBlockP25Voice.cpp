#include "ChannelBlockP25Voice.h"
#include "Const.h"

#include <gnuradio/filter/firdes.h>
#include <gnuradio/sptr_magic.h>

#include <algorithm>
#include <stdexcept>

namespace sdrscan {

namespace {

// P25 Phase 1 conventional voice channels are 12.5kHz-spaced C4FM, same symbol rate/family as
// DMR - see ChannelBlockDMR.cpp's identical constants for the same caveats (unverified against
// real RF in this sandbox).
constexpr double kHalfBandwidthHz = 6250.0;
constexpr int kDiscriminatorRate = 48000;
constexpr double kP25PeakDeviationHz = 1800.0; // TIA-102 C4FM outer symbol deviation
constexpr int kMbeAudioRate = 8000;
constexpr int kMinStage2Decim = 4;

} // namespace

ChannelBlockP25Voice::ChannelBlockP25Voice(const std::string& channelId,
                                             const std::string& label,
                                             bool mute,
                                             TriBool solo,
                                             bool hold,
                                             double audioGain_dB,
                                             double dwellTime_s,
                                             int64_t channelFreq_hz,
                                             int64_t hardwareFreq_hz,
                                             int rfSampleRate,
                                             int audioSampleRate,
                                             std::function<void(ChannelStatusUpdate)> statusCallback)
    : ChannelBlockBase(channelId, label, mute, solo, hold, /*squelchThreshold=*/-150.0,
                        audioGain_dB, dwellTime_s, audioSampleRate, /*squelchNoiseMargin_dB=*/std::nullopt,
                        std::move(statusCallback)) {

    if (rfSampleRate % kDiscriminatorRate != 0) {
        throw std::runtime_error(
            "ChannelBlockP25Voice: RF sample rate must be a whole multiple of 48000Hz for P25 channels");
    }
    int inputDecimation = rfSampleRate / kDiscriminatorRate;
    double freqOffset_Hz = static_cast<double>(channelFreq_hz - hardwareFreq_hz);

    auto [stage1Decim, stage2Decim] = splitDecimation(inputDecimation, kMinStage2Decim);
    int intermediateRate = rfSampleRate / stage1Decim;
    std::vector<float> stage1Taps;
    if (stage2Decim > 1) {
        double stage1Transition = intermediateRate / 2.0 - kHalfBandwidthHz;
        stage1Taps = gr::filter::firdes::low_pass_2(1.0, rfSampleRate, kHalfBandwidthHz, stage1Transition, 80.0);
    } else {
        stage1Taps = gr::filter::firdes::low_pass_2(1.0, rfSampleRate, kHalfBandwidthHz, kHalfBandwidthHz / 4.0, 80.0);
    }
    blockFreqXlatingFilter_ = gr::filter::freq_xlating_fir_filter_ccf::make(
        stage1Decim, stage1Taps, freqOffset_Hz, rfSampleRate);

    gr::basic_block_sptr channelized = blockFreqXlatingFilter_;
    if (stage2Decim > 1) {
        auto stage2Taps = gr::filter::firdes::low_pass_2(1.0, intermediateRate, kHalfBandwidthHz, kHalfBandwidthHz / 4.0, 80.0);
        blockChannelFilter_ = gr::filter::fir_filter_ccf::make(stage2Decim, stage2Taps);
        connect(blockFreqXlatingFilter_, 0, blockChannelFilter_, 0);
        channelized = blockChannelFilter_;
    }

    double demodGain = kDiscriminatorRate / (2.0 * M_PI * kP25PeakDeviationHz);
    blockQuadDemod_ = gr::analog::quadrature_demod_cf::make(demodGain);

    decodeBlock_ = gnuradio::make_block_sptr<DsdccDecodeBlock>(
        DSDcc::DSDDecoder::DSDDecodeP25P1, /*tdmaStereo=*/false);

    connect(self(), 0, blockFreqXlatingFilter_, 0);
    connect(channelized, 0, blockQuadDemod_, 0);
    connect(blockQuadDemod_, 0, decodeBlock_, 0);

    blockUnusedSlot2Sink_ = gr::blocks::null_sink::make(sizeof(float));
    connect(decodeBlock_, 1, blockUnusedSlot2Sink_, 0);

    int interp = audioSampleRate_;
    int decim = kMbeAudioRate;
    {
        int n = 2;
        while (n < interp) {
            if (interp % n == 0 && decim % n == 0) {
                interp /= n;
                decim /= n;
            } else {
                n++;
            }
        }
    }
    auto resamplerTaps = gr::filter::firdes::low_pass(
        1.0, interp, 0.5 * std::min(1.0, static_cast<double>(interp) / decim), 0.05);
    blockResampler_ = gr::filter::rational_resampler_fff::make(interp, decim, resamplerTaps);
    blockAudioGain_ = gr::blocks::multiply_const_ff::make(audioGainFactor_);
    blockAudioGate_ = gr::blocks::mute_ff::make(false);

    connect(decodeBlock_, 0, blockResampler_, 0);
    connect(blockResampler_, 0, blockAudioGain_, 0);
    connect(blockAudioGain_, 0, blockAudioGate_, 0);
    connect(blockAudioGate_, 0, blockAudioMute_, 0);

    connectVolume(blockAudioGain_, 0);
}

void ChannelBlockP25Voice::setForceActive(bool forceActive) {
    forceActive_ = forceActive;
}

void ChannelBlockP25Voice::setSquelchValue(double squelchThreshold) {
    // No-op: P25 has no adjustable squelch threshold, see the class header.
    squelchThreshold_ = squelchThreshold;
}

void ChannelBlockP25Voice::setAudioGain(double audioGain_dB) {
    audioGainFactor_ = dbToRatio(audioGain_dB);
    blockAudioGain_->set_k(static_cast<float>(audioGainFactor_));
}

ChannelStatus ChannelBlockP25Voice::getStatus() {
    bool voiceOn = decodeBlock_->decoder().getVoice1On();
    bool rawUnmuted = forceActive_ || voiceOn;
    bool unmuted = forceActive_ ? true : debounceSquelch(rawUnmuted);
    setAudioGateMuted(blockAudioGate_, !unmuted);
    return computeAndReportStatus(unmuted);
}

} // namespace sdrscan
