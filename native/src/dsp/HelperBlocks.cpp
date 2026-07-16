#include "HelperBlocks.h"
#include "Const.h"

#include <gnuradio/io_signature.h>

#include <cmath>

namespace sdrscan {

Mag2ToPowerBlock::Mag2ToPowerBlock(Callback cb)
    : gr::sync_block("Mag2ToPower",
                      gr::io_signature::make(1, 1, sizeof(float)),
                      gr::io_signature::make(0, 0, 0)),
      cb_(std::move(cb)) {}

int Mag2ToPowerBlock::work(int noutput_items,
                            gr_vector_const_void_star& input_items,
                            gr_vector_void_star& /*output_items*/) {
    const float* in = static_cast<const float*>(input_items[0]);
    if (noutput_items > 0) {
        float mag2 = in[noutput_items - 1];
        float dBFS = mag2 <= 0.0f ? DBFS_FLOOR : 10.0f * std::log10(mag2);
        cb_(dBFS);
    }
    return noutput_items;
}

MagToPowerLowPassBlock::MagToPowerLowPassBlock(Callback cb, double attackAlpha, double decayAlpha)
    : gr::sync_block("MagToPowerLowPass",
                      gr::io_signature::make(1, 1, sizeof(float)),
                      gr::io_signature::make(0, 0, 0)),
      cb_(std::move(cb)),
      attackAlpha_(attackAlpha),
      decayAlpha_(decayAlpha),
      curMag2Avg_(std::pow(10.0, DBFS_FLOOR / 10.0)) {}

int MagToPowerLowPassBlock::work(int noutput_items,
                                  gr_vector_const_void_star& input_items,
                                  gr_vector_void_star& /*output_items*/) {
    const float* in = static_cast<const float*>(input_items[0]);
    for (int i = 0; i < noutput_items; i++) {
        double mag2 = static_cast<double>(in[i]) * in[i];
        if (mag2 > curMag2Avg_) {
            curMag2Avg_ = (attackAlpha_ * mag2) + ((1 - attackAlpha_) * curMag2Avg_);
        } else {
            curMag2Avg_ = (decayAlpha_ * mag2) + ((1 - decayAlpha_) * curMag2Avg_);
        }
    }
    float dBFS = curMag2Avg_ <= 0.0 ? DBFS_FLOOR : static_cast<float>(10.0 * std::log10(curMag2Avg_));
    cb_(dBFS);
    return noutput_items;
}

} // namespace sdrscan
