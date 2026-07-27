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
#include <gnuradio/blocks/vector_source.h>

#include <chrono>
#include <cmath>
#include <thread>

#include "../src/dsp/ChannelBlockFM.h"

using namespace sdrscan;

namespace {

// A constant-envelope complex tone segment (a rotating phasor, |sample| == amplitude for every
// sample) - simpler to reason about than a real cosine for power-based squelch/RSSI checks,
// since its instantaneous power is exactly amplitude^2 with no time-varying envelope to average
// out first.
std::vector<gr_complex> makeToneSegment(double freqHz, float amplitude, double durationSec, int sampleRate) {
    size_t n = static_cast<size_t>(durationSec * sampleRate);
    std::vector<gr_complex> out(n);
    double phaseStep = 2.0 * M_PI * freqHz / sampleRate;
    for (size_t i = 0; i < n; i++) {
        double phase = phaseStep * static_cast<double>(i);
        out[i] = gr_complex(amplitude * std::cos(phase), amplitude * std::sin(phase));
    }
    return out;
}

// getStatus() debounces the raw squelch reading (SQUELCH_DEBOUNCE_SECONDS) rather than acting
// on it instantly - a real transition only sticks once observed as persisting across two polls,
// same as the Scanner's own ~100ms polling loop. A single post-run() getStatus() call would
// therefore always see the *old* stable state for whatever just transitioned; this mirrors the
// real polling pattern by priming the transition and then re-checking once the debounce window
// has elapsed.
ChannelStatus settledStatus(const std::shared_ptr<ChannelBlockFM>& channel) {
    channel->getStatus();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    return channel->getStatus();
}

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
        /*deviation_hz=*/2500, ctcssToneHz, /*squelchNoiseMargin_dB=*/std::nullopt,
        /*noiseSquelchThreshold_dB=*/std::nullopt, [](ChannelStatusUpdate) {});

    tb->connect(carrier, 0, add, 0);
    tb->connect(noise, 0, add, 1);
    tb->connect(add, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);

    tb->run();

    return settledStatus(channel);
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
        rfSampleRate, audioSampleRate, 2500, std::nullopt, std::nullopt, std::nullopt,
        [](ChannelStatusUpdate) {});
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
        /*deviation_hz=*/2500, /*ctcssToneHz=*/std::nullopt, /*squelchNoiseMargin_dB=*/std::nullopt,
        /*noiseSquelchThreshold_dB=*/std::nullopt, [](ChannelStatusUpdate) {});

    tb->connect(carrier, 0, add, 0);
    tb->connect(noise, 0, add, 1);
    tb->connect(add, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);

    tb->run();
    CHECK(settledStatus(channel) == ChannelStatus::ACTIVE);

    const auto& samples = sink->data();
    REQUIRE(!samples.empty());
    double energy = 0.0;
    for (float s : samples) energy += static_cast<double>(s) * s;
    CHECK(energy > 0.0);
}

TEST_CASE("Adaptive squelch opens based on the live noise floor plus margin, not the fixed threshold") {
    constexpr int rfSampleRate = 240'000;
    constexpr int audioSampleRate = 16'000;

    // Adaptive squelch's threshold is only pushed to the real GNU Radio squelch block from
    // getStatus() (called from Scanner's polling thread, not the flowgraph's own worker thread -
    // see the note on ChannelBlockFM::refreshAdaptiveSquelchThreshold()). So unlike a plain fixed
    // threshold, the block never re-evaluates once a run() completes with stale data already
    // flowing through it - this test has to run phase 1 (learn the noise floor) to completion,
    // explicitly call getStatus() once to push the resulting threshold (exactly what Scanner's
    // ~100ms poll would do in production before the signal changes), and only then run phase 2
    // through the now-correctly-thresholded block, rather than batching both phases through a
    // single run() and hoping getStatus() catches it mid-flight.
    auto channel = gnuradio::make_block_sptr<ChannelBlockFM>(
        "test-channel", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*squelchThreshold_dB=*/100.0, /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0,
        /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0, rfSampleRate, audioSampleRate,
        /*deviation_hz=*/2500, /*ctcssToneHz=*/std::nullopt, /*squelchNoiseMargin_dB=*/6.0,
        /*noiseSquelchThreshold_dB=*/std::nullopt, [](ChannelStatusUpdate) {});

    // Phase 1: a weak tone standing in for the band's quiet noise floor (~-60 dBFS). Long enough
    // to seed noiseFloor_dBFS_ from at least one RSSI update (needs >= 4000 fmQuadRate_ samples,
    // i.e. >= 0.25s of RF time here - see ChannelBlockBase::updateRSSI/RSSI_UPDATE_FREQ_HZ).
    {
        auto quiet = makeToneSegment(1000.0, 0.001f, 0.4, rfSampleRate);
        auto tb1 = gr::make_top_block("test-adaptive-squelch-phase1");
        auto source1 = gr::blocks::vector_source_c::make(quiet, false);
        auto sink1 = gr::blocks::null_sink::make(sizeof(float));
        tb1->connect(source1, 0, channel, 0);
        tb1->connect(channel, 0, sink1, 0);
        tb1->run();
        tb1->disconnect_all();
    }

    channel->getStatus(); // pushes noiseFloor + margin to blockPowerSquelch_ before phase 2 runs

    // Phase 2: a moderate signal (~-34 dBFS) - clearly 26dB above the phase-1 noise floor (and
    // therefore above noiseFloor+margin), but nowhere near the deliberately absurd fixed
    // squelchThreshold above, so only adaptive mode can open the squelch here.
    auto signal = makeToneSegment(1000.0, 0.02f, 0.3, rfSampleRate);
    auto tb2 = gr::make_top_block("test-adaptive-squelch-phase2");
    auto source2 = gr::blocks::vector_source_c::make(signal, false);
    auto sink2 = gr::blocks::null_sink::make(sizeof(float));
    tb2->connect(source2, 0, channel, 0);
    tb2->connect(channel, 0, sink2, 0);
    tb2->run();

    CHECK(settledStatus(channel) == ChannelStatus::ACTIVE);
}

TEST_CASE("Without adaptive squelch configured, the same moderate signal stays squelched under a high fixed threshold") {
    constexpr int rfSampleRate = 240'000;
    constexpr int audioSampleRate = 16'000;

    auto quiet = makeToneSegment(1000.0, 0.001f, 0.4, rfSampleRate);
    auto signal = makeToneSegment(1000.0, 0.02f, 0.3, rfSampleRate);
    std::vector<gr_complex> samples = quiet;
    samples.insert(samples.end(), signal.begin(), signal.end());

    auto tb = gr::make_top_block("test-adaptive-squelch-control");
    auto source = gr::blocks::vector_source_c::make(samples, false);
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    auto channel = gnuradio::make_block_sptr<ChannelBlockFM>(
        "test-channel", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*squelchThreshold_dB=*/100.0, /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0,
        /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0, rfSampleRate, audioSampleRate,
        /*deviation_hz=*/2500, /*ctcssToneHz=*/std::nullopt, /*squelchNoiseMargin_dB=*/std::nullopt,
        /*noiseSquelchThreshold_dB=*/std::nullopt, [](ChannelStatusUpdate) {});

    tb->connect(source, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);
    tb->run();

    CHECK(settledStatus(channel) == ChannelStatus::IDLE);
}

TEST_CASE("Noise squelch rejects broadband noise that clears power squelch but has no real capture") {
    // Regression test for the "squelch pops" investigation (see native/README.md's "FM noise
    // squelch" note): a real-hardware audio capture showed brief broadband noise bursts that
    // legitimately persisted long enough to clear both the power squelch and debounce, since
    // duration/power alone can't distinguish a genuine short transmission from a noise impulse
    // that happens to last just as long. Noise squelch adds a second, independent test - FM's
    // capture effect suppresses high-frequency ("hiss") content once a real signal captures the
    // receiver; broadband noise doesn't, regardless of duration.
    constexpr int rfSampleRate = 240'000;
    constexpr int audioSampleRate = 16'000;

    auto makeChannel = [&](std::optional<double> noiseSquelchThreshold_dB) {
        return gnuradio::make_block_sptr<ChannelBlockFM>(
            "test-channel", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
            /*squelchThreshold_dB=*/-80.0, /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0,
            /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0, rfSampleRate, audioSampleRate,
            /*deviation_hz=*/2500, /*ctcssToneHz=*/std::nullopt, /*squelchNoiseMargin_dB=*/std::nullopt,
            noiseSquelchThreshold_dB, [](ChannelStatusUpdate) {});
    };

    auto runClean = [&](const std::shared_ptr<ChannelBlockFM>& channel) {
        // A strong, clean, unmodulated carrier - constant instantaneous frequency, so quad-demod
        // output is essentially DC with no high-frequency ("hiss") content at all.
        auto signal = makeToneSegment(1000.0, 0.1f, 0.3, rfSampleRate);
        auto tb = gr::make_top_block("test-noise-squelch-clean");
        auto source = gr::blocks::vector_source_c::make(signal, false);
        auto sink = gr::blocks::null_sink::make(sizeof(float));
        tb->connect(source, 0, channel, 0);
        tb->connect(channel, 0, sink, 0);
        tb->run();
    };
    auto runNoisy = [&](const std::shared_ptr<ChannelBlockFM>& channel) {
        // Broadband noise at a power level well above squelchThreshold_dB above - "loud" by
        // the power squelch's own measure, but no real signal captured, so FM demod of it is
        // itself broadband noise (the well-known "FM demodulated noise" characteristic that
        // noise squelch exploits).
        auto noise = gr::analog::noise_source_c::make(gr::analog::GR_GAUSSIAN, 0.1, 42);
        auto head = gr::blocks::head::make(sizeof(gr_complex), static_cast<int>(rfSampleRate * 0.3));
        auto tb = gr::make_top_block("test-noise-squelch-noisy");
        auto sink = gr::blocks::null_sink::make(sizeof(float));
        tb->connect(noise, 0, head, 0);
        tb->connect(head, 0, channel, 0);
        tb->connect(channel, 0, sink, 0);
        tb->run();
    };

    // Probe both scenarios' raw reference-band levels first (noise squelch not configured, so
    // this doesn't affect gating) - avoids hardcoding an exact dB threshold that would be
    // fragile to firdes/GNU Radio version-specific gain details.
    auto probeClean = makeChannel(std::nullopt);
    runClean(probeClean);
    probeClean->getStatus(); // lets the reference-band callback populate noiseRefLevel()
    REQUIRE(probeClean->noiseRefLevel().has_value());
    float cleanLevel = *probeClean->noiseRefLevel();

    auto probeNoisy = makeChannel(std::nullopt);
    runNoisy(probeNoisy);
    probeNoisy->getStatus();
    REQUIRE(probeNoisy->noiseRefLevel().has_value());
    float noisyLevel = *probeNoisy->noiseRefLevel();

    REQUIRE(noisyLevel > cleanLevel + 6.0); // meaningfully noisier, not just measurement jitter
    double threshold = (cleanLevel + noisyLevel) / 2.0;

    // With a threshold sitting between the two measured levels, the clean "signal" should open
    // as normal, but the noise burst should stay squelched despite clearing the power threshold.
    auto cleanChannel = makeChannel(threshold);
    runClean(cleanChannel);
    CHECK(settledStatus(cleanChannel) == ChannelStatus::ACTIVE);

    auto noisyChannel = makeChannel(threshold);
    runNoisy(noisyChannel);
    CHECK(settledStatus(noisyChannel) == ChannelStatus::IDLE);
}
