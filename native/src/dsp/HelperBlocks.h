#pragma once

#include <gnuradio/sync_block.h>
#include <gnuradio/fft/fft.h>

#include <functional>
#include <vector>

namespace sdrscan {

// Converts the most recent sample of a mag^2 stream (as processed each work() call) to
// dBFS and invokes a callback. Used for RSSI, fed from a decimated/lowpassed mag^2 stream
// so "most recent sample" is already a meaningful running value.
class Mag2ToPowerBlock : public gr::sync_block {
public:
    using Callback = std::function<void(float dBFS)>;

    explicit Mag2ToPowerBlock(Callback cb);

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

private:
    Callback cb_;
};

// Tracks an averaged power (mag^2) for a signal stream with separate attack/decay alpha,
// converts to dBFS, and invokes a callback with the latest value on every sample. Used for
// channel volume metering (fed directly from demodulated audio, not decimated).
class MagToPowerLowPassBlock : public gr::sync_block {
public:
    using Callback = std::function<void(float dBFS)>;

    MagToPowerLowPassBlock(Callback cb, double attackAlpha, double decayAlpha);

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

private:
    Callback cb_;
    double attackAlpha_;
    double decayAlpha_;
    double curMag2Avg_;
};

// Real-input FFT tone-attention detector. Direct C++ port of Channel.py's
// ToneDetect_EmbeddedPythonBlock, folded together with the logpwrfft_f chain that fed it -
// GNU Radio's `fft.logpwrfft_f` is a GRC/Python-only hierarchical block with no C++ class
// (same situation as fm_deemph/nbfm_rx - see native/README.md "Simplifications"), so this
// does the windowing/FFT/magnitude steps itself instead.
//
// Input is a stream of fftSize-sample float vectors (an upstream gr::blocks::stream_to_vector
// groups the raw audio stream into these). Each vector is windowed, FFT'd, and checked for
// energy at each of testTonesHz at least thresholdDb above a reference band
// (refLowHz-refHighHz) *and* locally peaked (its bin's power exceeds both neighbors, not just
// the threshold) - the same test Python used, done here directly in linear power instead of
// via a separate dB-conversion stage (a ratio >= 10^(thresholdDb/10) is equivalent to Python's
// dB-domain `- >= thresholdDb` subtraction, since these are power - not amplitude - bins).
class EasToneDetectBlock : public gr::sync_block {
public:
    using ActiveCallback = std::function<void(bool active)>;

    EasToneDetectBlock(ActiveCallback cb,
                        const std::vector<double>& testTonesHz,
                        double refLowHz,
                        double refHighHz,
                        double thresholdDb,
                        int fftSize,
                        int sampleRate);

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

private:
    ActiveCallback cb_;
    int fftSize_;
    std::vector<float> window_;
    gr::fft::fft_real_fwd fft_;
    std::vector<int> testIndexes_;
    int refLowIndex_;
    int refHighIndex_;
    double thresholdRatio_;
};

} // namespace sdrscan
