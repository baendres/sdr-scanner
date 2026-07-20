// Synthetic-signal tests for ChannelBlockFM - no SDR hardware needed. Builds a small
// flowgraph with a GNU Radio signal generator standing in for a receiver, feeds it directly
// into a ChannelBlockFM, and checks the squelch opens/stays closed based on signal strength
// relative to the configured threshold - the same behavior a real over-the-air signal would
// need to produce to be heard.

#include <catch2/catch_test_macros.hpp>

#include <gnuradio/top_block.h>
#include <gnuradio/analog/sig_source.h>
#include <gnuradio/analog/noise_source.h>
#include <gnuradio/blocks/add_blk.h>
#include <gnuradio/blocks/head.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/blocks/vector_sink.h>

#include "../src/dsp/ChannelBlockFM.h"

using namespace sdrscan;

namespace {

ChannelStatus runTrial(float carrierAmplitude, double squelchThreshold_dB, std::optional<double> ctcssToneHz = std::nullopt) {
    constexpr int rfSampleRate = 240'000; // multiple of AUDIO_SAMPLERATE (16000)
    constexpr int audioSampleRate = 16'000;

    auto tb = gr::make_top_block("test");

    // Stand-in for a receiver: an unmodulated carrier (well above/below the noise floor
    // depending on the test) at the channel's tuned offset, plus a little noise so the
    // squelch's power averaging has something realistic to work with.
    auto carrier = gr::analog::sig_source_c::make(rfSampleRate, gr::analog::GR_COS_WAVE, 1000.0, carrierAmplitude);
    auto noise = gr::analog::noise_source_c::make(gr::analog::GR_GAUSSIAN, 1e-3, 42);
    auto add = gr::blocks::add_cc::make(1);
    auto head = gr::blocks::head::make(sizeof(gr_complex), rfSampleRate / 4); // 0.25s
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    auto channel = gnuradio::make_block_sptr<ChannelBlockFM>(
        "test-channel", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        squelchThreshold_dB, /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0,
        /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0, rfSampleRate, audioSampleRate,
        /*deviation_hz=*/2500, ctcssToneHz, [](ChannelStatusUpdate) {});

    tb->connect(carrier, 0, add, 0);
    tb->connect(noise, 0, add, 1);
    tb->connect(add, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);

    tb->run();

    return channel->getStatus();
}

} // namespace

TEST_CASE("ChannelBlockFM squelch stays closed for a weak signal") {
    CHECK(runTrial(/*carrierAmplitude=*/0.0005f, /*squelchThreshold_dB=*/-20.0) == ChannelStatus::IDLE);
}

TEST_CASE("ChannelBlockFM squelch opens for a strong signal") {
    CHECK(runTrial(/*carrierAmplitude=*/1.0f, /*squelchThreshold_dB=*/-20.0) == ChannelStatus::ACTIVE);
}

TEST_CASE("ChannelBlockFM forceActive opens squelch regardless of signal strength") {
    constexpr int rfSampleRate = 240'000;
    constexpr int audioSampleRate = 16'000;

    auto tb = gr::make_top_block("test-force");
    auto carrier = gr::analog::sig_source_c::make(rfSampleRate, gr::analog::GR_COS_WAVE, 1000.0, 0.0001f);
    auto head = gr::blocks::head::make(sizeof(gr_complex), rfSampleRate / 4);
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    auto channel = gnuradio::make_block_sptr<ChannelBlockFM>(
        "test-channel", "Test", false, std::nullopt, false, -20.0, 0.0, 3.0, 0, 0,
        rfSampleRate, audioSampleRate, 2500, std::nullopt, [](ChannelStatusUpdate) {});
    channel->setForceActive(true);

    tb->connect(carrier, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);
    tb->run();

    CHECK(channel->getStatus() == ChannelStatus::FORCE_ACTIVE);
}

TEST_CASE("ChannelBlockFM passes real audio through when CTCSS is not configured") {
    // Regression test: gr::analog::ctcss_squelch_ff has no "detect only" mode - it always
    // zeroes its own output when it doesn't consider itself unmuted, and a signal with no
    // literal tone at the (arbitrary, unused) analysis frequency essentially never reads
    // unmuted() there. Leaving it inline in the real audio path when CTCSS isn't configured
    // silently zeroed all audio despite squelch being open and getStatus() correctly reporting
    // ACTIVE - status "looked" fine while the channel was actually deaf. This must go through
    // getStatus() at least once (it's what drives the real inline gate - see ChannelBlockFM.h).
    constexpr int rfSampleRate = 240'000;
    constexpr int audioSampleRate = 16'000;

    auto tb = gr::make_top_block("test-ctcss-off-passthrough");
    auto carrier = gr::analog::sig_source_c::make(rfSampleRate, gr::analog::GR_COS_WAVE, 1000.0, 1.0f);
    auto noise = gr::analog::noise_source_c::make(gr::analog::GR_GAUSSIAN, 1e-3, 42);
    auto add = gr::blocks::add_cc::make(1);
    auto head = gr::blocks::head::make(sizeof(gr_complex), rfSampleRate / 4); // 0.25s
    auto sink = gr::blocks::vector_sink_f::make();

    auto channel = gnuradio::make_block_sptr<ChannelBlockFM>(
        "test-channel", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*squelchThreshold_dB=*/-20.0, /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0,
        /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0, rfSampleRate, audioSampleRate,
        /*deviation_hz=*/2500, /*ctcssToneHz=*/std::nullopt, [](ChannelStatusUpdate) {});

    tb->connect(carrier, 0, add, 0);
    tb->connect(noise, 0, add, 1);
    tb->connect(add, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);

    tb->run();
    CHECK(channel->getStatus() == ChannelStatus::ACTIVE);

    const auto& samples = sink->data();
    REQUIRE(!samples.empty());
    double energy = 0.0;
    for (float s : samples) energy += static_cast<double>(s) * s;
    CHECK(energy > 0.0);
}
