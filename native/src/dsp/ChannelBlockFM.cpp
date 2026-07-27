#include "ChannelBlockFM.h"
#include "Const.h"

#include <gnuradio/filter/firdes.h>

#include <cmath>
#include <iostream>
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

// Minimum stage-2 decimation splitDecimation() will settle for (see ChannelBlockBase.h) - low
// enough that the intermediate rate stays comfortably above 2x halfBandwidth (with margin for
// stage 2's own halfBandwidth/4 transition) even in the narrowest realistic case, high enough
// that stage 1's own transition band (intermediateRate/2 - halfBandwidth) stays wide, which is
// what keeps stage 1 cheap despite running at the full RF rate.
constexpr int kMinStage2Decim = 4;

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
                                std::optional<double> squelchNoiseMargin_dB,
                                std::function<void(ChannelStatusUpdate)> statusCallback)
    : ChannelBlockBase(channelId, label, mute, solo, hold, squelchThreshold, audioGain_dB,
                        dwellTime_s, audioSampleRate, squelchNoiseMargin_dB, std::move(statusCallback)),
      deviation_hz_(deviation_hz),
      rfSampleRate_(rfSampleRate),
      ctcssToneHz_(ctcssToneHz) {

    fmQuadRate_ = audioSampleRate_;
    if (deviation_hz_ > audioSampleRate_) {
        // Wideband (Broadcast FM, e.g. BFM_EAS) needs a much higher demod ("quad") rate than
        // the final audio rate - find the smallest multiple of audioSampleRate_ that's wide
        // enough to cover a WBFM station (~200kHz) and evenly divides rfSampleRate_, mirroring
        // Channel.py's ChannelBlock_FM. The audio filter below decimates back down to
        // audioSampleRate_ in the same step it applies the audio bandpass, so everything past
        // that point (RSSI aside, which stays at fmQuadRate_) is unaffected by this branch.
        int n = static_cast<int>(std::ceil(200000.0 / audioSampleRate_));
        int fmQuadMultiple = -1;
        while (fmQuadMultiple < 0) {
            if (rfSampleRate_ % (audioSampleRate_ * n) == 0) {
                fmQuadMultiple = n;
            } else if (rfSampleRate_ < audioSampleRate_ * n) {
                throw std::runtime_error("ChannelBlockFM: unable to find an FM quad rate for this wideband deviation");
            } else {
                n++;
            }
        }
        fmQuadRate_ = audioSampleRate_ * fmQuadMultiple;
    }

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
    //
    // Two-stage channelization (see native/README.md): a single filter doing the full sharp
    // (halfBandwidth/4 transition, 80dB) selectivity directly at rfSampleRate_ was measured on
    // real hardware at 3700-5400+ taps run continuously at 2+ Msps - genuinely enough compute
    // (~10 billion MACs/sec) to be the actual cause of chronic real-time audio starvation, not
    // any of the call-overhead/timing issues fixed earlier. splitDecimation() finds a
    // stage1/stage2 split when the ratio is large enough to benefit: stage 1 frequency-translates
    // and coarsely decimates with a *wide* transition band (cheap despite the high input rate,
    // and still 80dB stopband - that's what matters for rejecting the spur above, not transition
    // narrowness) down to an intermediate rate; stage 2 applies the exact same sharp selectivity
    // as before, just evaluated at that much lower rate, where the identical absolute-Hz
    // transition width is a far larger fraction of the sample rate and needs far fewer taps.
    // Falls back to the original single-stage design when the ratio's too small to be worth
    // splitting (blockChannelFilter_ stays null - see the header comment).
    auto [stage1Decim, stage2Decim] = splitDecimation(inputDecimation, kMinStage2Decim);
    int intermediateRate = rfSampleRate_ / stage1Decim;
    std::vector<float> stage1Taps;
    if (stage2Decim > 1) {
        double stage1Transition = intermediateRate / 2.0 - halfBandwidth;
        stage1Taps = gr::filter::firdes::low_pass_2(1.0, rfSampleRate_, halfBandwidth, stage1Transition, 80.0);
    } else {
        stage1Taps = gr::filter::firdes::low_pass_2(1.0, rfSampleRate_, halfBandwidth, halfBandwidth / 4.0, 80.0);
    }
    blockFreqXlatingFilter_ = gr::filter::freq_xlating_fir_filter_ccf::make(
        stage1Decim, stage1Taps, freqOffset_Hz, rfSampleRate_);

    std::vector<float> stage2Taps;
    if (stage2Decim > 1) {
        stage2Taps = gr::filter::firdes::low_pass_2(1.0, intermediateRate, halfBandwidth, halfBandwidth / 4.0, 80.0);
        blockChannelFilter_ = gr::filter::fir_filter_ccf::make(stage2Decim, stage2Taps);
    }
    // Temporary diagnostic (see native/README.md's adaptive-squelch/starvation investigation).
    std::cerr << "ChannelBlockFM " << channelId << ": rfSampleRate=" << rfSampleRate_
              << " stage1Decim=" << stage1Decim << " stage1Taps=" << stage1Taps.size()
              << " stage2Decim=" << stage2Decim << " stage2Taps=" << stage2Taps.size() << "\n";

    blockPowerSquelch_ = gr::analog::pwr_squelch_cc::make(
        effectiveSquelchThreshold(), 1.0 / (fmQuadRate_ * SQUELCH_TC), 0, false);

    double demodGain = fmQuadRate_ / (2.0 * M_PI * deviation_hz_);
    blockQuadDemod_ = gr::analog::quadrature_demod_cf::make(demodGain);

    std::vector<double> fftaps, fbtaps;
    computeDeemphTaps(75e-6, fmQuadRate_, fftaps, fbtaps);
    blockDeemph_ = gr::filter::iir_filter_ffd::make(fftaps, fbtaps, false);

    blockCtcssSquelch_ = gr::analog::ctcss_squelch_ff::make(
        fmQuadRate_, static_cast<float>(ctcssToneHz_.value_or(100.0)), 0.0f, 0, 0, false);
    applyCtcssLevel();
    blockCtcssSquelchSink_ = gr::blocks::null_sink::make(sizeof(float));
    blockAudioGate_ = gr::blocks::mute_ff::make(false);

    ///
    // Audio filter + gain

    // Doubles as the fmQuadRate_ -> audioSampleRate_ decimator when wideband (see above) - a
    // decimating band-pass FIR filter is a standard way to combine audio shaping with the
    // final rate reduction in one block. audioDecim is 1 (no-op) in the narrowband case.
    int audioDecim = fmQuadRate_ / audioSampleRate_;
    blockAudioFilter_ = gr::filter::fir_filter_fff::make(
        audioDecim, gr::filter::firdes::band_pass(1, fmQuadRate_, 200, 3500, 100));
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
    // blockChannelFilter_ (when present) is the final channelized signal - squelch/RSSI read
    // from whichever stage actually produced it, matching the single-stage behavior exactly
    // when there's no split (see the header comment on blockChannelFilter_).
    gr::basic_block_sptr channelized = blockFreqXlatingFilter_;
    if (blockChannelFilter_) {
        connect(blockFreqXlatingFilter_, 0, blockChannelFilter_, 0);
        channelized = blockChannelFilter_;
    }
    connect(channelized, 0, blockPowerSquelch_, 0);
    connect(blockPowerSquelch_, 0, blockQuadDemod_, 0);
    connect(blockQuadDemod_, 0, blockDeemph_, 0);
    // blockCtcssSquelch_ is a side tap (status only, see the header note) - the real audio path
    // runs through blockAudioGate_ instead, which we drive explicitly from getStatus().
    connect(blockDeemph_, 0, blockCtcssSquelch_, 0);
    connect(blockCtcssSquelch_, 0, blockCtcssSquelchSink_, 0);
    connect(blockDeemph_, 0, blockAudioGate_, 0);
    connect(blockAudioGate_, 0, blockAudioFilter_, 0);
    connect(blockAudioFilter_, 0, blockAudioGain_, 0);
    connect(blockAudioGain_, 0, blockAudioMute_, 0);

    // RSSI chain
    connect(channelized, 0, blockRssiComplexToMag2_, 0);
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
        blockPowerSquelch_->set_threshold(effectiveSquelchThreshold());
    }
}

void ChannelBlockFM::setSquelchValue(double squelchThreshold) {
    squelchThreshold_ = squelchThreshold;
    // An explicit absolute value is a deliberate "use exactly this threshold" action - it wins
    // over adaptive mode rather than being silently ignored by it.
    squelchNoiseMargin_dB_.reset();
    if (!forceActive_) {
        blockPowerSquelch_->set_threshold(effectiveSquelchThreshold());
    }
}

void ChannelBlockFM::setSquelchNoiseMargin(std::optional<double> marginDb) {
    squelchNoiseMargin_dB_ = marginDb;
    if (!forceActive_) {
        blockPowerSquelch_->set_threshold(effectiveSquelchThreshold());
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

void ChannelBlockFM::refreshAdaptiveSquelchThreshold() {
    // Deliberately not called from updateRSSI() (which runs on the flowgraph's own worker
    // thread) - every other hot-update setter in this codebase is only ever called from
    // Scanner's separate control-plane thread. getStatus() and ChannelBlockEAS (for its internal
    // ChannelBlockFM) both call this from that safe thread instead.
    if (squelchNoiseMargin_dB_.has_value() && !forceActive_) {
        double threshold = effectiveSquelchThreshold();
        if (adaptiveThresholdChanged(threshold)) {
            blockPowerSquelch_->set_threshold(threshold);
        }
    }
}

ChannelStatus ChannelBlockFM::getStatus() {
    refreshAdaptiveSquelchThreshold();

    // CTCSS only gates when actually configured (and never overrides forceActive) - see the
    // note in applyCtcssLevel() for why we don't rely on level==0 to mean "always passes".
    bool ctcssOk = forceActive_ || !ctcssToneHz_.has_value() || blockCtcssSquelch_->unmuted();
    bool rawUnmuted = blockPowerSquelch_->unmuted() && ctcssOk;
    // Debounce filters brief noise spikes from opening/closing the channel on their own (see
    // SQUELCH_DEBOUNCE_SECONDS) - this is what actually drives real audio (blockAudioGate_,
    // polled roughly every ms from the receiver's scheduling loop) as well as the reported
    // status, so a spike that doesn't persist never reaches the listener as a pop. forceActive
    // is an explicit operator override, not a squelch reading, so it bypasses the debounce
    // rather than waiting out its hang time before taking effect.
    bool unmuted = forceActive_ ? true : debounceSquelch(rawUnmuted);
    setAudioGateMuted(blockAudioGate_, !unmuted);
    return computeAndReportStatus(unmuted);
}

} // namespace sdrscan
