// ChannelBlockP25Voice tests - same constraint as test_channel_dmr.cpp (no real P25 signal
// generator available in this sandbox), so this covers construction/validation and that the
// full front end runs against noise without crashing. Real P25 decode correctness needs a
// captured IQ recording against real hardware - see native/README.md.

#include <catch2/catch_test_macros.hpp>

#include <gnuradio/top_block.h>
#include <gnuradio/analog/noise_source.h>
#include <gnuradio/blocks/head.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/sptr_magic.h>

#include "../src/dsp/ChannelBlockP25Voice.h"

using namespace sdrscan;

namespace {

// Divisible by 48000 (ChannelBlockP25Voice's required discriminator rate).
constexpr int kRfSampleRate = 2'400'000;
constexpr int kAudioSampleRate = 16'000;

} // namespace

// Regression test - see ChannelBlockDMR.h's getMinimumScanTime() override comment (same
// continuous-samples-for-frame-sync reasoning applies to P25 Phase 1's DSDcc-based decode).
TEST_CASE("ChannelBlockP25Voice: getMinimumScanTime() is long enough for DSDcc to establish sync") {
    auto p25v = gnuradio::make_block_sptr<ChannelBlockP25Voice>(
        "p25v", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kRfSampleRate, kAudioSampleRate, [](ChannelStatusUpdate) {});

    CHECK(p25v->getMinimumScanTime() >= 1.0);
}

TEST_CASE("ChannelBlockP25Voice: rejects an RF sample rate that isn't a whole multiple of 48000Hz") {
    CHECK_THROWS(gnuradio::make_block_sptr<ChannelBlockP25Voice>(
        "bad", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        /*rfSampleRate=*/1'000'000, kAudioSampleRate, [](ChannelStatusUpdate) {}));
}

TEST_CASE("ChannelBlockP25Voice: forceActive forces ACTIVE regardless of decoder sync state") {
    auto tb = gr::make_top_block("test-p25voice");

    auto channel = gnuradio::make_block_sptr<ChannelBlockP25Voice>(
        "p25v", "Test P25", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0,
        kRfSampleRate, kAudioSampleRate, [](ChannelStatusUpdate) {});

    REQUIRE(channel->decodeBlock() != nullptr);
    CHECK(channel->getStatus() == ChannelStatus::IDLE);

    auto noise = gr::analog::noise_source_c::make(gr::analog::GR_GAUSSIAN, 0.1, 42);
    auto head = gr::blocks::head::make(sizeof(gr_complex), static_cast<uint64_t>(kRfSampleRate * 0.05));
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    tb->connect(noise, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);

    REQUIRE_NOTHROW(tb->run());

    channel->setForceActive(true);
    CHECK(channel->getStatus() == ChannelStatus::FORCE_ACTIVE);
}
