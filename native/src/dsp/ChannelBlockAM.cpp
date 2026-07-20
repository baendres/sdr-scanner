#include "ChannelBlockAM.h"
#include "Const.h"

#include <gnuradio/filter/firdes.h>

#include <algorithm>
#include <stdexcept>

namespace sdrscan {

ChannelBlockAM::ChannelBlockAM(const std::string& channelId,
                                const std::string& label,
                                bool mute,
                                TriBool solo,
                                bool hold,
                                double squelchThreshold,
                                double audioGain_dB,
                                double dwellTime_s,
                                int64_t channelFreq_hz,
                                int64_t hardwareFreq_hz,
                                int rfSampleRate,
                                int audioSampleRate,
                                std::function<void(ChannelStatusUpdate)> statusCallback)
    : ChannelBlockBase(channelId, label, mute, solo, hold, squelchThreshold, audioGain_dB,
                        dwellTime_s, audioSampleRate, std::move(statusCallback)),
      rfSampleRate_(rfSampleRate) {

    audioGainFactor_ = dbToRatio(audioGain_dB) * kFixedAudioGainFactor;

    if (rfSampleRate_ % audioSampleRate_ != 0) {
        throw std::runtime_error("ChannelBlockAM: RF sample rate is not a multiple of the audio sample rate");
    }
    int inputDecimation = rfSampleRate_ / audioSampleRate_;
    double freqOffset_Hz = static_cast<double>(channelFreq_hz - hardwareFreq_hz);

    ///
    // Input channelization + squelch + demod

    // See the matching note in ChannelBlockFM.cpp - low_pass_2's explicit stopband attenuation
    // (vs. low_pass's default ~53dB) gives more rejection to a hardware-originated spur that
    // aliases into the passband through decimation.
    blockFreqXlatingFilter_ = gr::filter::freq_xlating_fir_filter_ccf::make(
        inputDecimation,
        gr::filter::firdes::low_pass_2(1.0, rfSampleRate_, 4000, 2000, 80.0),
        freqOffset_Hz,
        rfSampleRate_);

    blockPowerSquelch_ = gr::analog::pwr_squelch_cc::make(
        squelchThreshold_, 1.0 / (audioSampleRate_ * SQUELCH_TC), 0, false);

    blockAgc_ = gr::analog::feedforward_agc_cc::make(static_cast<int>(audioSampleRate_ * 0.2), 0.5f);
    blockAmDemod_ = gr::blocks::complex_to_mag::make(1);

    ///
    // Audio filter + gain

    blockAudioFilter_ = gr::filter::fir_filter_fff::make(
        1, gr::filter::firdes::band_pass(1, audioSampleRate_, 200, 3500, 100));
    blockAudioGain_ = gr::blocks::multiply_const_ff::make(static_cast<float>(audioGainFactor_));

    ///
    // RSSI

    blockRssiComplexToMag2_ = gr::blocks::complex_to_mag_squared::make(1);
    blockRssiLowPass_ = gr::filter::single_pole_iir_filter_ff::make(1.0 / (audioSampleRate_ * RSSI_LOWPASS_TC), 1);
    int rssiDecimation = std::max(1, static_cast<int>(audioSampleRate_ / RSSI_UPDATE_FREQ_HZ));
    blockRssiDecimate_ = gr::blocks::keep_one_in_n::make(sizeof(float), rssiDecimation);
    blockRssi_ = std::make_shared<Mag2ToPowerBlock>([this](float dBFS) { updateRSSI(dBFS); });

    ///
    // Connections - RF chain

    connect(self(), 0, blockFreqXlatingFilter_, 0);
    connect(blockFreqXlatingFilter_, 0, blockPowerSquelch_, 0);
    connect(blockPowerSquelch_, 0, blockAgc_, 0);
    connect(blockAgc_, 0, blockAmDemod_, 0);
    connect(blockAmDemod_, 0, blockAudioFilter_, 0);
    connect(blockAudioFilter_, 0, blockAudioGain_, 0);
    connect(blockAudioGain_, 0, blockAudioMute_, 0);

    // RSSI chain
    connect(blockFreqXlatingFilter_, 0, blockRssiComplexToMag2_, 0);
    connect(blockRssiComplexToMag2_, 0, blockRssiLowPass_, 0);
    connect(blockRssiLowPass_, 0, blockRssiDecimate_, 0);
    connect(blockRssiDecimate_, 0, blockRssi_, 0);

    // Volume
    connectVolume(blockAudioGain_, 0);
}

void ChannelBlockAM::setForceActive(bool forceActive) {
    forceActive_ = forceActive;
    if (forceActive) {
        blockPowerSquelch_->set_threshold(-150.0);
    } else {
        blockPowerSquelch_->set_threshold(squelchThreshold_);
    }
}

void ChannelBlockAM::setSquelchValue(double squelchThreshold) {
    squelchThreshold_ = squelchThreshold;
    if (!forceActive_) {
        blockPowerSquelch_->set_threshold(squelchThreshold_);
    }
}

void ChannelBlockAM::setAudioGain(double audioGain_dB) {
    audioGainFactor_ = dbToRatio(audioGain_dB) * kFixedAudioGainFactor;
    blockAudioGain_->set_k(static_cast<float>(audioGainFactor_));
}

ChannelStatus ChannelBlockAM::getStatus() {
    return computeAndReportStatus(blockPowerSquelch_->unmuted());
}

} // namespace sdrscan
