#include "ChannelBlockFM.h"
#include "Const.h"

#include <gnuradio/filter/firdes.h>

#include <cmath>
#include <stdexcept>

namespace sdrscan {

namespace {

// Standard GNU Radio FM de-emphasis IIR design (matches gr::filter::fm_deemph's math,
// which isn't exposed as a standalone C++ block - see native/README.md).
void computeDeemphTaps(double tau, double sampleRate, std::vector<double>& fftaps, std::vector<double>& fbtaps) {
    double w_c = 1.0 / tau;
    double w_ca = 2.0 * sampleRate * std::tan(w_c / (2.0 * sampleRate));
    double k = -w_ca / (2.0 * sampleRate);
    double z1 = -1.0;
    double p1 = (1.0 + k) / (1.0 - k);
    double b0 = -k / (1.0 - k);

    fftaps = {b0 * 1.0, b0 * -z1};
    fbtaps = {1.0, -p1};
}

} // namespace

ChannelBlockFM::ChannelBlockFM(const std::string& channelId,
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
                                int deviation_hz,
                                std::optional<double> ctcssToneHz,
                                std::function<void(ChannelStatusUpdate)> statusCallback)
    : ChannelBlockBase(channelId, label, mute, solo, hold, squelchThreshold, audioGain_dB,
                        dwellTime_s, audioSampleRate, std::move(statusCallback)),
      deviation_hz_(deviation_hz),
      rfSampleRate_(rfSampleRate),
      ctcssToneHz_(ctcssToneHz) {

    if (deviation_hz_ > audioSampleRate_) {
        // Wideband (Broadcast FM) needs a quad rate multiple of the audio rate - deferred,
        // see native/README.md ("Explicitly Deferred").
        throw std::runtime_error("ChannelBlockFM: deviation exceeds audio sample rate (BFM not yet supported)");
    }
    fmQuadRate_ = audioSampleRate_;

    if (rfSampleRate_ % fmQuadRate_ != 0) {
        throw std::runtime_error("ChannelBlockFM: RF sample rate is not a multiple of the FM quad rate");
    }
    int inputDecimation = rfSampleRate_ / fmQuadRate_;
    double freqOffset_Hz = static_cast<double>(channelFreq_hz - hardwareFreq_hz);
    double halfBandwidth = deviation_hz_ + 3000;

    ///
    // Input channelization + squelch + demod

    // low_pass_2's explicit stopband attenuation (vs. low_pass's default ~53dB Hamming window)
    // matters here specifically: real-hardware testing turned up a spur (likely from the RTL-SDR's
    // own internal clock/PLL, reproduced even with the antenna disconnected and independent of
    // this app's own code - a minimal bare GNU Radio flowgraph shows it too) that aliases into the
    // audio band through decimation. A tighter anti-aliasing filter here, before decimation, is the
    // standard remedy regardless of the spur's exact source frequency.
    blockFreqXlatingFilter_ = gr::filter::freq_xlating_fir_filter_ccf::make(
        inputDecimation,
        gr::filter::firdes::low_pass_2(1.0, rfSampleRate_, halfBandwidth, halfBandwidth / 4.0, 80.0),
        freqOffset_Hz,
        rfSampleRate_);

    blockPowerSquelch_ = gr::analog::pwr_squelch_cc::make(
        squelchThreshold_, 1.0 / (fmQuadRate_ * SQUELCH_TC), 0, false);

    double demodGain = fmQuadRate_ / (2.0 * M_PI * deviation_hz_);
    blockQuadDemod_ = gr::analog::quadrature_demod_cf::make(demodGain);

    std::vector<double> fftaps, fbtaps;
    computeDeemphTaps(75e-6, fmQuadRate_, fftaps, fbtaps);
    blockDeemph_ = gr::filter::iir_filter_ffd::make(fftaps, fbtaps, false);

    blockCtcssSquelch_ = gr::analog::ctcss_squelch_ff::make(
        fmQuadRate_, static_cast<float>(ctcssToneHz_.value_or(100.0)), 0.0f, 0, 0, false);
    applyCtcssLevel();
    blockCtcssSquelchSink_ = gr::blocks::null_sink::make(sizeof(float));
    blockCtcssGate_ = gr::blocks::mute_ff::make(false);

    ///
    // Audio filter + gain

    blockAudioFilter_ = gr::filter::fir_filter_fff::make(
        1, gr::filter::firdes::band_pass(1, audioSampleRate_, 200, 3500, 100));
    blockAudioGain_ = gr::blocks::multiply_const_ff::make(audioGainFactor_);

    ///
    // RSSI

    blockRssiComplexToMag2_ = gr::blocks::complex_to_mag_squared::make(1);
    blockRssiLowPass_ = gr::filter::single_pole_iir_filter_ff::make(1.0 / (fmQuadRate_ * RSSI_LOWPASS_TC), 1);
    int rssiDecimation = std::max(1, static_cast<int>(fmQuadRate_ / RSSI_UPDATE_FREQ_HZ));
    blockRssiDecimate_ = gr::blocks::keep_one_in_n::make(sizeof(float), rssiDecimation);
    blockRssi_ = std::make_shared<Mag2ToPowerBlock>([this](float dBFS) { updateRSSI(dBFS); });

    ///
    // Connections - RF chain

    connect(self(), 0, blockFreqXlatingFilter_, 0);
    connect(blockFreqXlatingFilter_, 0, blockPowerSquelch_, 0);
    connect(blockPowerSquelch_, 0, blockQuadDemod_, 0);
    connect(blockQuadDemod_, 0, blockDeemph_, 0);
    // blockCtcssSquelch_ is a side tap (status only, see the header note) - the real audio path
    // runs through blockCtcssGate_ instead, which we drive explicitly from getStatus().
    connect(blockDeemph_, 0, blockCtcssSquelch_, 0);
    connect(blockCtcssSquelch_, 0, blockCtcssSquelchSink_, 0);
    connect(blockDeemph_, 0, blockCtcssGate_, 0);
    connect(blockCtcssGate_, 0, blockAudioFilter_, 0);
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

void ChannelBlockFM::applyCtcssLevel() {
    // NOTE: this is *not* how CTCSS-disabled/forceActive bypass is implemented (a "set level
    // to 0 so it always passes" trick turned out to be unreliable: a signal with literally no
    // energy in the narrow CTCSS analysis band - e.g. an unmodulated carrier, or a genuinely
    // quiet moment - still reads unmuted()==false even at level 0, since the comparison is a
    // strict `energy > level`). The actual bypass lives in getStatus() below; this just keeps
    // the configured level accurate for when CTCSS is really in use.
    blockCtcssSquelch_->set_level(ctcssToneHz_.has_value() ? CTCSS_DEFAULT_LEVEL : 0.0f);
}

void ChannelBlockFM::setForceActive(bool forceActive) {
    forceActive_ = forceActive;
    if (forceActive) {
        blockPowerSquelch_->set_threshold(-150.0);
    } else {
        blockPowerSquelch_->set_threshold(squelchThreshold_);
    }
}

void ChannelBlockFM::setSquelchValue(double squelchThreshold) {
    squelchThreshold_ = squelchThreshold;
    if (!forceActive_) {
        blockPowerSquelch_->set_threshold(squelchThreshold_);
    }
}

void ChannelBlockFM::setAudioGain(double audioGain_dB) {
    audioGainFactor_ = dbToRatio(audioGain_dB);
    blockAudioGain_->set_k(static_cast<float>(audioGainFactor_));
}

void ChannelBlockFM::setCtcssTone(std::optional<double> toneHz) {
    ctcssToneHz_ = toneHz;
    blockCtcssSquelch_->set_frequency(static_cast<float>(toneHz.value_or(100.0)));
    applyCtcssLevel();
}

ChannelStatus ChannelBlockFM::getStatus() {
    // CTCSS only gates when actually configured (and never overrides forceActive) - see the
    // note in applyCtcssLevel() for why we don't rely on level==0 to mean "always passes".
    bool ctcssOk = forceActive_ || !ctcssToneHz_.has_value() || blockCtcssSquelch_->unmuted();
    // Drive the real inline gate from this same decision (see the header note on
    // blockCtcssGate_/blockCtcssSquelch_) - this is polled roughly every ms from the
    // receiver's scheduling loop, plenty responsive for a sub-audible tone gate.
    blockCtcssGate_->set_mute(!ctcssOk);
    bool unmuted = blockPowerSquelch_->unmuted() && ctcssOk;
    return computeAndReportStatus(unmuted);
}

} // namespace sdrscan
