#include "ChannelBlockDMR.h"
#include "Const.h"

#include <gnuradio/filter/firdes.h>
#include <gnuradio/sptr_magic.h>

#include <algorithm>
#include <stdexcept>

namespace sdrscan {

namespace {

// DMR repeaters are 12.5kHz-spaced; half that (with a little margin) is the standard channel
// filter width used by other DMR/C4FM decoders.
constexpr double kHalfBandwidthHz = 6250.0;
// DSDcc's DSDRate4800 (10 samples/symbol @ 4800 baud) is what setDecodeMode(DSDDecodeDMR,...)
// selects internally - see DsdccDecodeBlock. Shared with Const.h's DMR_DISCRIMINATOR_RATE_HZ so
// ScanWindow::selectRfSampleRate() can pick a compatible RF sample rate up front.
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
        if (rfSampleRate % kDiscriminatorRate != 0) {
            throw std::runtime_error(
                "ChannelBlockDMR: RF sample rate must be a whole multiple of 48000Hz for DMR channels");
        }
        int inputDecimation = rfSampleRate / kDiscriminatorRate;
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

        double demodGain = kDiscriminatorRate / (2.0 * M_PI * kDmrPeakDeviationHz);
        blockQuadDemod_ = gr::analog::quadrature_demod_cf::make(demodGain);

        decodeBlock_ = gnuradio::make_block_sptr<DsdccDecodeBlock>(
            DSDcc::DSDDecoder::DSDDecodeDMR, /*tdmaStereo=*/true);

        connect(self(), 0, blockFreqXlatingFilter_, 0);
        connect(channelized, 0, blockQuadDemod_, 0);
        connect(blockQuadDemod_, 0, decodeBlock_, 0);
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
