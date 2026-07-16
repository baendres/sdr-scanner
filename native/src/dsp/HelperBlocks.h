#pragma once

#include <gnuradio/sync_block.h>

#include <functional>

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

} // namespace sdrscan
