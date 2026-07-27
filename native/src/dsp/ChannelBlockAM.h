#pragma once

#include <gnuradio/filter/freq_xlating_fir_filter.h>
#include <gnuradio/analog/pwr_squelch_cc.h>
#include <gnuradio/analog/feedforward_agc_cc.h>
#include <gnuradio/blocks/complex_to_mag.h>
#include <gnuradio/filter/fir_filter_blk.h>
#include <gnuradio/blocks/multiply_const.h>
#include <gnuradio/blocks/complex_to_mag_squared.h>
#include <gnuradio/filter/single_pole_iir_filter_ff.h>
#include <gnuradio/blocks/keep_one_in_n.h>
#include <gnuradio/blocks/mute.h>

#include "ChannelBlockBase.h"

namespace sdrscan {

// AM demodulator, direct C++ port of Channel.py's ChannelBlock_AM. No CTCSS (that's an FM
// sub-audible-tone scheme, doesn't apply to AM).
class ChannelBlockAM : public ChannelBlockBase {
public:
    // Matches Python's ChannelBlock_AM.FIXED_AUDIO_GAIN_FACTOR - AM demod (complex_to_mag)
    // comes out quieter than FM, so the audio gain is scaled up to give comparable volume.
    static constexpr double kFixedAudioGainFactor = 3.0;

    ChannelBlockAM(const std::string& channelId,
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
                   std::optional<double> squelchNoiseMargin_dB,
                   std::function<void(ChannelStatusUpdate)> statusCallback);

    void setForceActive(bool forceActive) override;
    void setSquelchValue(double squelchThreshold) override;
    void setSquelchNoiseMargin(std::optional<double> marginDb) override;
    void setAudioGain(double audioGain_dB) override;
    ChannelStatus getStatus() override;

private:
    int rfSampleRate_;

    gr::filter::freq_xlating_fir_filter_ccf::sptr blockFreqXlatingFilter_;
    // Second channelization stage - see the matching header comment on ChannelBlockFM; null
    // unless splitDecimation() found a worthwhile split.
    gr::filter::fir_filter_ccf::sptr blockChannelFilter_;
    gr::analog::pwr_squelch_cc::sptr blockPowerSquelch_;
    gr::analog::feedforward_agc_cc::sptr blockAgc_;
    gr::blocks::complex_to_mag::sptr blockAmDemod_;
    // Real inline gate driven from getStatus()'s debounced squelch decision (see
    // ChannelBlockFM's blockAudioGate_ for the same pattern/reasoning).
    gr::blocks::mute_ff::sptr blockAudioGate_;
    gr::filter::fir_filter_fff::sptr blockAudioFilter_;
    gr::blocks::multiply_const_ff::sptr blockAudioGain_;

    gr::blocks::complex_to_mag_squared::sptr blockRssiComplexToMag2_;
    gr::filter::single_pole_iir_filter_ff::sptr blockRssiLowPass_;
    gr::blocks::keep_one_in_n::sptr blockRssiDecimate_;
    std::shared_ptr<Mag2ToPowerBlock> blockRssi_;
};

} // namespace sdrscan
