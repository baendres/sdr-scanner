#include "HelperBlocks.h"
#include "Const.h"

#include <gnuradio/io_signature.h>
#include <gnuradio/fft/window.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

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

namespace {
int binNum(double freqHz, int fftSize, int sampleRate) {
    return static_cast<int>(std::lround(freqHz * fftSize / sampleRate));
}
} // namespace

namespace {
// Absolute floor on the alert-tone bin's |X|^2 (Blackman-Harris windowed, unnormalized FFT of
// +/-1.0 full-scale audio). A -60 dBFS 1050Hz tone at kFftSize=1024 lands around 3e-2 here;
// 1e-6 is ~45dB below that, so it only rejects digital silence / denormal-level residue.
constexpr float kMinTonePower = 1e-6f;
} // namespace

EasToneDetectBlock::EasToneDetectBlock(ActiveCallback cb,
                                        const std::vector<double>& testTonesHz,
                                        double refLowHz,
                                        double refHighHz,
                                        double thresholdDb,
                                        int fftSize,
                                        int sampleRate)
    : gr::sync_block("EasToneDetect",
                      gr::io_signature::make(1, 1, sizeof(float) * fftSize),
                      gr::io_signature::make(0, 0, 0)),
      cb_(std::move(cb)),
      fftSize_(fftSize),
      window_(gr::fft::window::blackmanharris(fftSize)),
      fft_(fftSize),
      refLowIndex_(binNum(refLowHz, fftSize, sampleRate)),
      refHighIndex_(binNum(refHighHz, fftSize, sampleRate)),
      thresholdRatio_(std::pow(10.0, thresholdDb / 10.0)) {

    for (double toneHz : testTonesHz) {
        testIndexes_.push_back(binNum(toneHz, fftSize, sampleRate));
    }

    int outLen = fft_.outbuf_length();
    for (int idx : testIndexes_) {
        if (idx <= 0 || idx >= outLen - 1) {
            throw std::runtime_error("EasToneDetectBlock: alert tone bin out of FFT range");
        }
    }
    if (refLowIndex_ < 0 || refHighIndex_ >= outLen || refLowIndex_ > refHighIndex_) {
        throw std::runtime_error("EasToneDetectBlock: reference band out of FFT range");
    }
}

int EasToneDetectBlock::work(int noutput_items,
                              gr_vector_const_void_star& input_items,
                              gr_vector_void_star& /*output_items*/) {
    const float* in = static_cast<const float*>(input_items[0]);
    float* inbuf = fft_.get_inbuf();
    const gr_complex* outbuf = fft_.get_outbuf();

    auto mag2 = [&](int i) { return std::norm(outbuf[i]); };

    for (int v = 0; v < noutput_items; v++) {
        const float* vec = in + static_cast<size_t>(v) * fftSize_;
        for (int i = 0; i < fftSize_; i++) {
            inbuf[i] = vec[i] * window_[i];
        }
        fft_.execute();

        float refPwr = 0.0f;
        for (int i = refLowIndex_; i <= refHighIndex_; i++) {
            refPwr = std::max(refPwr, mag2(i));
        }

        // Comparisons must be strict and the tone must clear an absolute floor: with all-zero
        // input (FM power squelch closed - pwr_squelch_cc with gate=false emits zeros) every bin
        // is 0, and the old non-strict `p < ...` checks all evaluated 0 < 0 == false, i.e.
        // silence read as "tone detected" on every frame. That latched NOAA/EAS channels
        // ACTIVE forever (and pinned a receiver to that window, starving every other window
        // of scan time) whenever their squelch was closed.
        bool active = true;
        for (int idx : testIndexes_) {
            float p = mag2(idx);
            if (!(p > kMinTonePower) || !(p > refPwr * static_cast<float>(thresholdRatio_))
                || !(p > mag2(idx - 1)) || !(p > mag2(idx + 1))) {
                active = false;
                break;
            }
        }
        cb_(active);
    }

    return noutput_items;
}

} // namespace sdrscan
