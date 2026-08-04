// ChannelBlockDMR tests - no SDR hardware and no real DMR signal generator available in this
// sandbox (unlike test_channel_eas.cpp/test_ctcss_squelch.cpp, synthesizing a real DMR C4FM
// burst is far more involved than an FM-modulated tone), so these focus on what's verifiable
// without one: the shared-decoder wiring between two TS1/TS2 channels at the same frequency,
// forceActive/mute independence between slots, and that the full front end (channelizer -> quad
// demod -> DsdccDecodeBlock -> resampler -> gate) runs against noise without crashing or
// throwing. Real DMR decode correctness needs a captured IQ recording against real hardware -
// see native/README.md.

#include <catch2/catch_test_macros.hpp>

#include <gnuradio/top_block.h>
#include <gnuradio/analog/noise_source.h>
#include <gnuradio/blocks/head.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/sptr_magic.h>

#include "../src/dsp/ChannelBlockDMR.h"

using namespace sdrscan;

namespace {

// Divisible by 48000 (ChannelBlockDMR's discriminator rate) so most of these tests exercise the
// plain integer-decimation path - see the dedicated non-multiple-rate test below for the rational-
// resampler correction path.
constexpr int kRfSampleRate = 2'400'000;
constexpr int kAudioSampleRate = 16'000;

} // namespace

// Regression test for a real production issue: DSDcc's DMR frame sync needs continuous samples
// across several TDMA bursts to lock on, but the base class's 0.1s getMinimumScanTime() default
// (sized for analog squelch) let the round-robin scheduler hop away long before that - even with
// a strong, close-range signal, confirmed via real hardware showing sync constantly flapping and
// never holding. See ChannelBlockDMR.h's getMinimumScanTime() override comment for the full story.
TEST_CASE("ChannelBlockDMR: getMinimumScanTime() is long enough for DSDcc to establish sync") {
    auto dmr = gnuradio::make_block_sptr<ChannelBlockDMR>(
        "dmr", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kRfSampleRate, kAudioSampleRate, /*dmrSlot=*/1, /*talkgroupFilter=*/std::nullopt,
        /*existingDecodeBlock=*/nullptr, /*otherSlotPresent=*/false, [](ChannelStatusUpdate) {});

    CHECK(dmr->getMinimumScanTime() >= 1.0);
}

TEST_CASE("ChannelBlockDMR: second slot at the same freq_hz shares the first slot's decodeBlock()") {
    auto ts1 = gnuradio::make_block_sptr<ChannelBlockDMR>(
        "ts1", "Test TS1", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kRfSampleRate, kAudioSampleRate, /*dmrSlot=*/1, /*talkgroupFilter=*/std::nullopt,
        /*existingDecodeBlock=*/nullptr, /*otherSlotPresent=*/true, [](ChannelStatusUpdate) {});

    REQUIRE(ts1->decodeBlock() != nullptr);

    auto ts2 = gnuradio::make_block_sptr<ChannelBlockDMR>(
        "ts2", "Test TS2", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kRfSampleRate, kAudioSampleRate, /*dmrSlot=*/2, /*talkgroupFilter=*/std::nullopt,
        /*existingDecodeBlock=*/ts1->decodeBlock(), /*otherSlotPresent=*/true, [](ChannelStatusUpdate) {});

    CHECK(ts2->decodeBlock() == ts1->decodeBlock());
}

TEST_CASE("ChannelBlockDMR: rejects a slot number other than 1 or 2") {
    CHECK_THROWS(gnuradio::make_block_sptr<ChannelBlockDMR>(
        "bad", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kRfSampleRate, kAudioSampleRate, /*dmrSlot=*/3, /*talkgroupFilter=*/std::nullopt,
        /*existingDecodeBlock=*/nullptr, /*otherSlotPresent=*/false, [](ChannelStatusUpdate) {}));
}

// Regression test: RF sample rates that aren't a whole multiple of 48000Hz used to be rejected
// outright (forcing the whole scanner to share one DMR-compatible sample rate across every
// window/receiver - a real usability problem the user pushed back on, since it meant DMR capped
// every window's bandwidth, not just DMR ones). ChannelBlockDMR now corrects the remainder with
// its own internal rational resampler (see its constructor) instead, so any RF sample rate that
// covers the channel's own bandwidth needs works - this just checks it builds and runs without
// throwing at a rate the old exact-multiple check would have rejected (2048000/48000 = 42.67).
TEST_CASE("ChannelBlockDMR: works at an RF sample rate that isn't a whole multiple of 48000Hz") {
    constexpr int kNonMultipleRfSampleRate = 2'048'000;
    auto tb = gr::make_top_block("test-dmr-nonmultiple-rate");

    auto dmr = gnuradio::make_block_sptr<ChannelBlockDMR>(
        "dmr", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kNonMultipleRfSampleRate, kAudioSampleRate, /*dmrSlot=*/1, /*talkgroupFilter=*/std::nullopt,
        /*existingDecodeBlock=*/nullptr, /*otherSlotPresent=*/false, [](ChannelStatusUpdate) {});

    auto noise = gr::analog::noise_source_c::make(gr::analog::GR_GAUSSIAN, 0.1, 42);
    auto head = gr::blocks::head::make(sizeof(gr_complex),
                                        static_cast<uint64_t>(kNonMultipleRfSampleRate * 0.05));
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    tb->connect(noise, 0, head, 0);
    tb->connect(head, 0, dmr, 0);
    tb->connect(dmr, 0, sink, 0);

    REQUIRE_NOTHROW(tb->run());
}

TEST_CASE("ChannelBlockDMR: forceActive forces one slot ACTIVE without affecting the other") {
    auto tb = gr::make_top_block("test-dmr");

    auto ts1 = gnuradio::make_block_sptr<ChannelBlockDMR>(
        "ts1", "Test TS1", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kRfSampleRate, kAudioSampleRate, /*dmrSlot=*/1, /*talkgroupFilter=*/std::nullopt,
        /*existingDecodeBlock=*/nullptr, /*otherSlotPresent=*/true, [](ChannelStatusUpdate) {});
    auto ts2 = gnuradio::make_block_sptr<ChannelBlockDMR>(
        "ts2", "Test TS2", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kRfSampleRate, kAudioSampleRate, /*dmrSlot=*/2, /*talkgroupFilter=*/std::nullopt,
        /*existingDecodeBlock=*/ts1->decodeBlock(), /*otherSlotPresent=*/true, [](ChannelStatusUpdate) {});

    // Feed a short burst of noise through both so the flowgraph is actually exercised (checked
    // separately below that this doesn't crash), then drive getStatus() directly - it doesn't
    // depend on the flowgraph having run, only on forceActive_/decoder state already sampled.
    auto noise = gr::analog::noise_source_c::make(gr::analog::GR_GAUSSIAN, 0.1, 42);
    auto head = gr::blocks::head::make(sizeof(gr_complex), static_cast<uint64_t>(kRfSampleRate * 0.05));
    auto sink1 = gr::blocks::null_sink::make(sizeof(float));
    auto sink2 = gr::blocks::null_sink::make(sizeof(float));

    tb->connect(noise, 0, head, 0);
    tb->connect(head, 0, ts1, 0);
    tb->connect(head, 0, ts2, 0);
    tb->connect(ts1, 0, sink1, 0);
    tb->connect(ts2, 0, sink2, 0);

    REQUIRE_NOTHROW(tb->run());

    ts1->setForceActive(true);
    CHECK(ts1->getStatus() == ChannelStatus::FORCE_ACTIVE);
    CHECK(ts2->getStatus() == ChannelStatus::IDLE);
}

// Regression test for a real crash: a DMR channel configured alone (no channel for the other
// timeslot at the same freq_hz) used to leave DsdccDecodeBlock's other output port permanently
// unconnected, since only the slot that has a real ChannelBlockDMR ever calls connect() on it.
// DsdccDecodeBlock's io_signature requires exactly 2 connected outputs, so gr::top_block::start()
// - called from SoapyReceiver's own thread when the flowgraph is (re)built - would throw
// "insufficient connected output ports" uncaught, crashing the whole process (this bug was latent
// until an earlier fix let DMR windows actually build at all - see git history for the full
// chain of DMR crash fixes).
TEST_CASE("ChannelBlockDMR: a lone slot with no sibling starts without a flowgraph validation error") {
    auto tb = gr::make_top_block("test-dmr-lone-slot");

    auto ts2Only = gnuradio::make_block_sptr<ChannelBlockDMR>(
        "ts2only", "Test TS2 only", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kRfSampleRate, kAudioSampleRate, /*dmrSlot=*/2, /*talkgroupFilter=*/std::nullopt,
        /*existingDecodeBlock=*/nullptr, /*otherSlotPresent=*/false, [](ChannelStatusUpdate) {});

    auto noise = gr::analog::noise_source_c::make(gr::analog::GR_GAUSSIAN, 0.1, 42);
    auto head = gr::blocks::head::make(sizeof(gr_complex), static_cast<uint64_t>(kRfSampleRate * 0.05));
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    tb->connect(noise, 0, head, 0);
    tb->connect(head, 0, ts2Only, 0);
    tb->connect(ts2Only, 0, sink, 0);

    REQUIRE_NOTHROW(tb->run());
}
