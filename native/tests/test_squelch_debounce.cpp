// Unit tests for ChannelBlockBase's squelch debounce and adaptive ("noise-relative") threshold
// logic. Unlike the other dsp/ tests, these deliberately don't build a GNU Radio flowgraph:
// debounceSquelch() is keyed off wall-clock time (see SQUELCH_DEBOUNCE_SECONDS in Const.h), not
// flowgraph sample time, and tb->run() processes samples as fast as the CPU allows rather than
// in real time - so a flowgraph-based test would have no reliable way to land inside or outside
// the debounce window. Exercising these protected methods directly through a trivial concrete
// subclass, with real (short) sleeps, is what actually pins down the wall-clock behavior.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "../src/dsp/ChannelBlockBase.h"
#include "../src/dsp/Const.h"

using namespace sdrscan;

namespace {

class TestChannelBlock : public ChannelBlockBase {
public:
    using ChannelBlockBase::ChannelBlockBase;
    using ChannelBlockBase::adaptiveThresholdChanged;
    using ChannelBlockBase::debounceSquelch;
    using ChannelBlockBase::effectiveSquelchThreshold;
    using ChannelBlockBase::updateRSSI;

    void setForceActive(bool) override {}
    void setSquelchValue(double squelchThreshold) override { squelchThreshold_ = squelchThreshold; }
    void setAudioGain(double) override {}
    ChannelStatus getStatus() override { return ChannelStatus::IDLE; }
};

std::shared_ptr<TestChannelBlock> makeBlock(double squelchThreshold, std::optional<double> squelchNoiseMargin_dB) {
    return gnuradio::make_block_sptr<TestChannelBlock>(
        "test-channel", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        squelchThreshold, /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0, /*audioSampleRate=*/16000,
        squelchNoiseMargin_dB, [](ChannelStatusUpdate) {});
}

} // namespace

TEST_CASE("debounceSquelch ignores a spike that doesn't persist past the debounce window") {
    auto block = makeBlock(-55.0, std::nullopt);

    CHECK(block->debounceSquelch(false) == false);
    CHECK(block->debounceSquelch(true) == false); // just transitioned - not stable yet
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // well under SQUELCH_DEBOUNCE_SECONDS (50ms)
    CHECK(block->debounceSquelch(false) == false); // flips closed again before ever opening
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    CHECK(block->debounceSquelch(false) == false); // stayed closed the whole time
}

TEST_CASE("debounceSquelch opens once the raw reading persists past the debounce window") {
    auto block = makeBlock(-55.0, std::nullopt);

    CHECK(block->debounceSquelch(true) == false); // transition just observed
    std::this_thread::sleep_for(std::chrono::milliseconds(90)); // > SQUELCH_DEBOUNCE_SECONDS
    CHECK(block->debounceSquelch(true) == true);
}

TEST_CASE("debounceSquelch closes again once the raw reading drops and persists") {
    auto block = makeBlock(-55.0, std::nullopt);

    block->debounceSquelch(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    REQUIRE(block->debounceSquelch(true) == true);

    CHECK(block->debounceSquelch(false) == true); // just transitioned - still reports the old stable state
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    CHECK(block->debounceSquelch(false) == false);
}

TEST_CASE("effectiveSquelchThreshold falls back to the fixed threshold with no noise floor estimate yet") {
    auto block = makeBlock(-55.0, /*squelchNoiseMargin_dB=*/6.0);
    CHECK(block->effectiveSquelchThreshold() == -55.0);
}

TEST_CASE("effectiveSquelchThreshold ignores the noise floor when adaptive squelch isn't configured") {
    auto block = makeBlock(-55.0, std::nullopt);
    block->updateRSSI(-70.0f); // channel starts inactive, so this seeds noiseFloor_dBFS_
    CHECK(block->effectiveSquelchThreshold() == -55.0);
}

TEST_CASE("effectiveSquelchThreshold tracks noiseFloor + margin once a noise floor estimate exists") {
    auto block = makeBlock(-55.0, /*squelchNoiseMargin_dB=*/6.0);
    block->updateRSSI(-70.0f); // first inactive RSSI reading seeds noiseFloor_dBFS_ directly (no averaging)
    CHECK(block->effectiveSquelchThreshold() == -64.0);
}

TEST_CASE("effectiveSquelchThreshold follows the noise floor as it moves") {
    auto block = makeBlock(-55.0, /*squelchNoiseMargin_dB=*/10.0);
    block->updateRSSI(-70.0f);
    CHECK(block->effectiveSquelchThreshold() == -60.0);
    // Repeated updates average toward the new reading (NOISEFLOOR_LOWPASS_A) rather than
    // snapping to it - after many updates the estimate should have moved substantially toward
    // the new, much noisier band condition.
    for (int i = 0; i < 500; i++) block->updateRSSI(-30.0f);
    CHECK(block->effectiveSquelchThreshold() > -60.0);
    CHECK(block->effectiveSquelchThreshold() < -20.0);
}

TEST_CASE("adaptiveThresholdChanged reports true only on genuine changes") {
    // Regression test: real-hardware testing found getStatus() (and therefore this check) runs
    // on SoapyReceiver's ~1ms window-scheduling loop, not Scanner's ~100ms control-plane poll as
    // originally assumed - repeatedly calling a live squelch block's set_threshold() at that
    // rate, once per adaptive-squelch channel in the active window, was enough overhead to make
    // real receivers fall behind real-time audio production. This is what a caller uses to skip
    // that call unless the value has actually moved.
    auto block = makeBlock(-55.0, /*squelchNoiseMargin_dB=*/6.0);

    CHECK(block->adaptiveThresholdChanged(-50.0) == true); // first call always reports changed
    CHECK(block->adaptiveThresholdChanged(-50.0) == false); // same value repeatedly - no change
    CHECK(block->adaptiveThresholdChanged(-50.0) == false);
    CHECK(block->adaptiveThresholdChanged(-49.5) == true); // genuinely moved
    CHECK(block->adaptiveThresholdChanged(-49.5) == false); // settled at the new value
}
