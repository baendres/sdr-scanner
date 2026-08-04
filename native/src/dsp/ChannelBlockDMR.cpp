#include "ChannelBlockDMR.h"
#include "Const.h"

#include <gnuradio/filter/firdes.h>
#include <gnuradio/sptr_magic.h>

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace sdrscan {

namespace {

// DMR repeaters are 12.5kHz-spaced; half that (with a little margin) is the standard channel
// filter width used by other DMR/C4FM decoders.
constexpr double kHalfBandwidthHz = 6250.0;
// DSDcc's DSDRate4800 (10 samples/symbol @ 4800 baud) is what setDecodeMode(DSDDecodeDMR,...)
// selects internally - see DsdccDecodeBlock. The channelizer below always lands exactly on this
// rate regardless of rfSampleRate, via a rational resampler correcting whatever the integer
// decimation stages don't evenly reach - see its comment. Shared with Const.h's
// DMR_DISCRIMINATOR_RATE_HZ, which OpenWebRX+'s digiham (a from-scratch, non-DSDcc DMR decoder)
// also targets - see native/README.md - confirming this is a DMR-signal-math constant (4800 baud
// x 10 samples/symbol), not a DSDcc-specific quirk.
constexpr int kDiscriminatorRate = DMR_DISCRIMINATOR_RATE_HZ;
// Assumed DMR (ETSI TS 102 361) 4FSK outer symbol deviation - unverified against real DMR RF in
// this sandbox (no hardware available); the quad-demod gain this drives just needs to land
// DSDcc's auto-leveling symbol tracker in a reasonable range (see DsdccDecodeBlock.cpp's note),
// so exact calibration isn't critical, but flag for real-world tuning if decode quality is poor.
constexpr double kDmrPeakDeviationHz = 1944.0;
// mbelib decodes at a fixed 8kHz.
constexpr int kMbeAudioRate = 8000;

constexpr int kMinStage2Decim = 4; // see ChannelBlockFM.cpp's identical constant

} // namespace

ChannelBlockDMR::ChannelBlockDMR(const std::string& channelId,
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
                                   int dmrSlot,
                                   std::optional<uint32_t> talkgroupFilter,
                                   std::shared_ptr<DsdccDecodeBlock> existingDecodeBlock,
                                   bool otherSlotPresent,
                                   std::function<void(ChannelStatusUpdate)> statusCallback)
    // No power squelch for DMR (see header) - the base class still wants a value, it's just
    // never read back through effectiveSquelchThreshold() here.
    : ChannelBlockBase(channelId, label, mute, solo, hold, /*squelchThreshold=*/-150.0,
                        audioGain_dB, dwellTime_s, audioSampleRate, /*squelchNoiseMargin_dB=*/std::nullopt,
                        std::move(statusCallback)),
      dmrSlot_(dmrSlot),
      talkgroupFilter_(talkgroupFilter) {

    if (dmrSlot_ != 1 && dmrSlot_ != 2) {
        throw std::runtime_error("ChannelBlockDMR: dmrSlot must be 1 or 2");
    }

    if (existingDecodeBlock) {
        // Shared-tap instance: the other slot's ChannelBlockDMR already built the real front
        // end + decoder. Our own copy of the window's RF stream is unused - discard it (GNU
        // Radio requires every hier_block2 boundary port connected internally, same reasoning
        // as ChannelBlockFM's CTCSS side-tap null_sink).
        decodeBlock_ = existingDecodeBlock;
        blockRfDiscardSink_ = gr::blocks::null_sink::make(sizeof(gr_complex));
        connect(self(), 0, blockRfDiscardSink_, 0);
    } else {
        // Doesn't need to divide rfSampleRate evenly (see the rational-resampler correction
        // below) - just picks how much the two integer FIR stages decimate before that
        // correction, so this floors to the largest whole decimation that keeps the
        // intermediate rate at or above kDiscriminatorRate (preserving channel bandwidth/
        // anti-aliasing margin), same as it always has for already-compatible rates.
        int inputDecimation = std::max(1, rfSampleRate / kDiscriminatorRate);
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

        // The integer decimation stages above land the RF rate at rfSampleRate/inputDecimation,
        // which only equals kDiscriminatorRate exactly when rfSampleRate happens to be a whole
        // multiple of it. DSDcc's symbol-timing recovery needs samples at exactly 48000Hz (see
        // kDiscriminatorRate's comment) or it'll never lock onto the real 4800-baud symbol clock,
        // so correct the remainder with a rational resampler computed as an exact fraction
        // (avoiding any floating-point rate error): the two decimation stages produce
        // rfSampleRate/inputDecimation exactly (an exact rational, not necessarily integer, since
        // FIR decimation just keeps every Nth sample regardless of what "sample rate" labels it),
        // so resampling that by kDiscriminatorRate*inputDecimation/rfSampleRate lands on exactly
        // kDiscriminatorRate. Reduced via GCD to the smallest equivalent ratio, same common-
        // factor-reduction idea as the decoded-audio resampler below.
        int64_t resamplerInterp = static_cast<int64_t>(kDiscriminatorRate) * inputDecimation;
        int64_t resamplerDecim = rfSampleRate;
        int64_t g = std::gcd(resamplerInterp, resamplerDecim);
        resamplerInterp /= g;
        resamplerDecim /= g;

        gr::basic_block_sptr discriminatorInput = channelized;
        if (resamplerInterp != resamplerDecim) {
            auto rateCorrectorTaps = gr::filter::firdes::low_pass(
                1.0, static_cast<double>(resamplerInterp),
                0.5 * std::min(1.0, static_cast<double>(resamplerInterp) / static_cast<double>(resamplerDecim)),
                0.05);
            blockRateCorrector_ = gr::filter::rational_resampler_ccf::make(
                static_cast<int>(resamplerInterp), static_cast<int>(resamplerDecim), rateCorrectorTaps);
            connect(channelized, 0, blockRateCorrector_, 0);
            discriminatorInput = blockRateCorrector_;
        }

        double demodGain = kDiscriminatorRate / (2.0 * M_PI * kDmrPeakDeviationHz);
        blockQuadDemod_ = gr::analog::quadrature_demod_cf::make(demodGain);

        decodeBlock_ = gnuradio::make_block_sptr<DsdccDecodeBlock>(
            DSDcc::DSDDecoder::DSDDecodeDMR, /*tdmaStereo=*/true);

        connect(self(), 0, blockFreqXlatingFilter_, 0);
        connect(discriminatorInput, 0, blockQuadDemod_, 0);
        connect(blockQuadDemod_, 0, decodeBlock_, 0);

        if (!otherSlotPresent) {
            // No channel for the other timeslot exists in this window, so nothing will ever
            // connect decodeBlock_'s other output port - but DsdccDecodeBlock's io_signature
            // requires both connected regardless (GNU Radio's flowgraph validation fails
            // otherwise: "insufficient connected output ports"). Discard it, same reasoning as
            // blockRfDiscardSink_ above.
            int unusedSlotPort = (dmrSlot_ == 1) ? 1 : 0;
            blockUnusedSlotSink_ = gr::blocks::null_sink::make(sizeof(float));
            connect(decodeBlock_, unusedSlotPort, blockUnusedSlotSink_, 0);
        }
    }

    // Resample mbelib's fixed 8kHz decoded audio up/down to this window's audioSampleRate_ -
    // same common-factor-reduction technique ScanWindowBlock uses for its own rate matching.
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

    int slotPort = (dmrSlot_ == 1) ? 0 : 1;
    connect(decodeBlock_, slotPort, blockResampler_, 0);
    connect(blockResampler_, 0, blockAudioGain_, 0);
    connect(blockAudioGain_, 0, blockAudioGate_, 0);
    connect(blockAudioGate_, 0, blockAudioMute_, 0);

    connectVolume(blockAudioGain_, 0);
}

void ChannelBlockDMR::setForceActive(bool forceActive) {
    forceActive_ = forceActive;
}

void ChannelBlockDMR::setSquelchValue(double squelchThreshold) {
    // No-op: DMR has no adjustable squelch threshold, see the class header.
    squelchThreshold_ = squelchThreshold;
}

void ChannelBlockDMR::setAudioGain(double audioGain_dB) {
    audioGainFactor_ = dbToRatio(audioGain_dB);
    blockAudioGain_->set_k(static_cast<float>(audioGainFactor_));
}

std::optional<uint32_t> ChannelBlockDMR::currentTalkgroup() const {
    const char* text = (dmrSlot_ == 1)
        ? decodeBlock_->decoder().getDMRDecoder().getSlot0Text()
        : decodeBlock_->decoder().getDMRDecoder().getSlot1Text();
    if (!text) return std::nullopt;
    // See DsdccDMR::textVoiceEmbeddedSignalling: target talkgroup is an 8-digit zero-padded
    // decimal at fixed offset 18 in the 27-byte slotNlight buffer.
    std::string s(text);
    if (s.size() < 26) return std::nullopt;
    std::string digits = s.substr(18, 8);
    if (!std::all_of(digits.begin(), digits.end(), ::isdigit)) return std::nullopt;
    return static_cast<uint32_t>(std::stoul(digits));
}

ChannelStatus ChannelBlockDMR::getStatus() {
    bool voiceOn = (dmrSlot_ == 1) ? decodeBlock_->decoder().getVoice1On() : decodeBlock_->decoder().getVoice2On();

    bool talkgroupOk = true;
    if (talkgroupFilter_.has_value() && voiceOn) {
        auto tg = currentTalkgroup();
        talkgroupOk = tg.has_value() && *tg == *talkgroupFilter_;
    }

    bool rawUnmuted = forceActive_ || (voiceOn && talkgroupOk);
    bool unmuted = forceActive_ ? true : debounceSquelch(rawUnmuted);
    setAudioGateMuted(blockAudioGate_, !unmuted);
    return computeAndReportStatus(unmuted);
}

} // namespace sdrscan
