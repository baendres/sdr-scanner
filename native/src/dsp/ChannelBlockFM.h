#pragma once

#include <gnuradio/filter/freq_xlating_fir_filter.h>
#include <gnuradio/analog/pwr_squelch_cc.h>
#include <gnuradio/analog/quadrature_demod_cf.h>
#include <gnuradio/analog/ctcss_squelch_ff.h>
#include <gnuradio/filter/iir_filter_ffd.h>
#include <gnuradio/filter/fir_filter_blk.h>
#include <gnuradio/blocks/multiply_const.h>
#include <gnuradio/blocks/complex_to_mag_squared.h>
#include <gnuradio/filter/single_pole_iir_filter_ff.h>
#include <gnuradio/blocks/keep_one_in_n.h>
#include <gnuradio/blocks/mute.h>
#include <gnuradio/blocks/null_sink.h>

#include "ChannelBlockBase.h"

namespace sdrscan {

// FM/NFM/WBFM demodulator, direct C++ port of Channel.py's ChannelBlock_FM - with CTCSS
// squelch added as a new capability (see native/README.md "CTCSS Squelch Design"). Also used,
// wrapped by ChannelBlockEAS, as the demod stage for NOAA/BFM_EAS (see ChannelBlockEAS.h).
//
// When deviation_hz exceeds audioSampleRate (wideband/broadcast FM), demodulation runs at a
// higher internal "quad rate" (fmQuadRate_) and the audio filter decimates back down to
// audioSampleRate - narrowband channels are unaffected (fmQuadRate_ == audioSampleRate_ there).
//
// Simplification vs. the Python version: always uses a single-stage
// freq_xlating_fir_filter_ccf for input channelization instead of the Python code's optional
// two-stage FFT-filter split for very high decimation ratios. That was a CPU optimization for
// extreme decimation, not a correctness requirement; revisit if profiling shows it's needed.
class ChannelBlockFM : public ChannelBlockBase {
public:
    ChannelBlockFM(const std::string& channelId,
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
                   std::function<void(ChannelStatusUpdate)> statusCallback);

    void setForceActive(bool forceActive) override;
    void setSquelchValue(double squelchThreshold) override;
    void setSquelchNoiseMargin(std::optional<double> marginDb) override;
    void setAudioGain(double audioGain_dB) override;
    void setCtcssTone(std::optional<double> toneHz) override;
    ChannelStatus getStatus() override;

private:
    // The CTCSS block is always present in the chain (simpler + avoids ever needing to stop
    // the flowgraph to add/remove a block). It's wired as a side tap (fed the same signal, but
    // not inline in the real audio path) purely so its unmuted() reading is available to
    // getStatus() - gr::analog::ctcss_squelch_ff has no "detect only" mode, it always zeroes or
    // truncates its own output when it doesn't consider itself unmuted (see its header: "gate
    // or zero output if CTCSS tone not present"), which would silently mute all real audio
    // whenever CTCSS isn't configured (level=0 is not a reliable "always pass", see
    // applyCtcssLevel()) if it were left inline. blockAudioGate_ is the actual inline gate,
    // driven explicitly from getStatus()'s already-computed (CTCSS AND debounced power-squelch)
    // decision so there's one source of truth for the gating decision.
    void applyCtcssLevel();
    void onNoiseFloorUpdated() override;

    int deviation_hz_;
    int fmQuadRate_;
    int rfSampleRate_;
    std::optional<double> ctcssToneHz_;

    gr::filter::freq_xlating_fir_filter_ccf::sptr blockFreqXlatingFilter_;
    gr::analog::pwr_squelch_cc::sptr blockPowerSquelch_;
    gr::analog::quadrature_demod_cf::sptr blockQuadDemod_;
    gr::filter::iir_filter_ffd::sptr blockDeemph_;
    gr::analog::ctcss_squelch_ff::sptr blockCtcssSquelch_; // side tap only - see the note above
    gr::blocks::null_sink::sptr blockCtcssSquelchSink_; // discards blockCtcssSquelch_'s output -
                                                          // GNU Radio requires every output port
                                                          // connected; we only want its unmuted()
    // Real inline gate for BOTH CTCSS and the debounced power-squelch decision - driven from
    // getStatus() (see the class-level note above). Named generically rather than
    // "blockCtcssGate_" since it now covers more than just CTCSS.
    gr::blocks::mute_ff::sptr blockAudioGate_;
    gr::filter::fir_filter_fff::sptr blockAudioFilter_;
    gr::blocks::multiply_const_ff::sptr blockAudioGain_;

    gr::blocks::complex_to_mag_squared::sptr blockRssiComplexToMag2_;
    gr::filter::single_pole_iir_filter_ff::sptr blockRssiLowPass_;
    gr::blocks::keep_one_in_n::sptr blockRssiDecimate_;
    std::shared_ptr<Mag2ToPowerBlock> blockRssi_;
};

} // namespace sdrscan
