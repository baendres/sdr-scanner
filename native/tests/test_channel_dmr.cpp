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
#include "../src/dsp/ScanWindow.h"

using namespace sdrscan;

namespace {

// Divisible by 48000 (ChannelBlockDMR's required discriminator rate) - see ChannelBlockDMR.cpp.
constexpr int kRfSampleRate = 2'400'000;
constexpr int kAudioSampleRate = 16'000;

} // namespace

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

TEST_CASE("ChannelBlockDMR: rejects an RF sample rate that isn't a whole multiple of 48000Hz") {
    CHECK_THROWS(gnuradio::make_block_sptr<ChannelBlockDMR>(
        "bad", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        /*rfSampleRate=*/1'000'000, kAudioSampleRate, /*dmrSlot=*/1, /*talkgroupFilter=*/std::nullopt,
        /*existingDecodeBlock=*/nullptr, /*otherSlotPresent=*/false, [](ChannelStatusUpdate) {}));
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
// until the "give DMR windows their own RF bandwidth" fix let DMR windows actually build at all).
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

// Regression test for a real crash: SoapyReceiver::rebuildGraph() used to pick an RF sample rate
// with no regard for whether the window contained a DMR channel, so on hardware whose narrowest
// covering rate wasn't a multiple of 48000Hz (e.g. an RTL-SDR offering 2048000, which isn't -
// see kRtlSdrSampleRates in SoapyReceiver.cpp), ChannelBlockDMR's constructor would throw on the
// receiver's own thread, uncaught, killing the whole process (see ChannelBlockBase's/
// SoapyReceiver::startWindow()'s "uncaught exception aborts the entire process" note).
TEST_CASE("ScanWindow::selectRfSampleRate skips non-48000-multiple rates when DMR is required") {
    // Mirrors SoapyReceiver.cpp's kRtlSdrSampleRates - only 1536000 and 1920000 are whole
    // multiples of 48000.
    const std::vector<int> rtlSdrRates = {1'024'000, 1'536'000, 1'792'000, 1'920'000, 2'048'000};

    // Without the DMR constraint, the narrowest rate that covers the bandwidth wins as before.
    CHECK(ScanWindow::selectRfSampleRate(rtlSdrRates, /*rfBandwidth=*/1'000'000,
                                          /*requireDmrCompatibleRate=*/false) == 1'024'000);

    // With it, that same narrowest-covering rate (1024000) isn't a multiple of 48000, so it must
    // be skipped in favor of the next one that is (1536000), not thrown from ChannelBlockDMR.
    CHECK(ScanWindow::selectRfSampleRate(rtlSdrRates, /*rfBandwidth=*/1'000'000,
                                          /*requireDmrCompatibleRate=*/true) == 1'536'000);
}

TEST_CASE("ScanWindow::selectRfSampleRate throws when no rate is both wide enough and DMR-compatible") {
    const std::vector<int> rates = {1'024'000, 2'048'000}; // neither is a multiple of 48000
    CHECK_THROWS(ScanWindow::selectRfSampleRate(rates, /*rfBandwidth=*/500'000,
                                                 /*requireDmrCompatibleRate=*/true));
}
