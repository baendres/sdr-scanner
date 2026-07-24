// Verifies AudioMixer::isAlive() correctly detects a hung mixer thread - the mechanism
// Scanner's runMaintenanceLoop uses to port Tony's Scanner.py `audioServerProcess.is_alive()`
// watchdog (see native/src/scanner/Scanner.cpp and Scanner.h's setAudioMixerDiedCallback).
// Real end-to-end test: a custom AudioOutput blocks send() forever once signaled, genuinely
// hanging the mixer thread inside its own run() loop the same way a real stuck output
// implementation would - no mocking of AudioMixer's internals.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "../src/audio/AudioMixer.h"

using namespace sdrscan;

namespace {

class BlockingAudioOutput : public AudioOutput {
public:
    void reconnect() override {}
    void close() override {}
    void send(const std::vector<int16_t>&) override {
        if (blocked_) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !blocked_.load(); });
        }
    }

    void block() { blocked_ = true; }
    void unblock() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            blocked_ = false;
        }
        cv_.notify_all();
    }

private:
    std::atomic<bool> blocked_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

bool waitUntil(const std::function<bool()>& pred, std::chrono::seconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

} // namespace

TEST_CASE("AudioMixer::isAlive() reports alive while running normally") {
    auto output = std::make_shared<BlockingAudioOutput>();
    AudioMixer mixer(0, {output});

    CHECK(mixer.isAlive()); // hasn't ticked yet - shouldn't read as dead

    mixer.start();
    REQUIRE(waitUntil([&] { return mixer.isAlive(); }, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(mixer.isAlive());

    mixer.stop();
}

TEST_CASE("AudioMixer::isAlive() detects a hung mixer thread") {
    auto output = std::make_shared<BlockingAudioOutput>();
    AudioMixer mixer(0, {output});
    mixer.start();
    REQUIRE(waitUntil([&] { return mixer.isAlive(); }, std::chrono::seconds(2)));

    output->block();

    // kHeartbeatTimeoutSeconds is 5s in production code - poll well past that.
    bool becameDead = waitUntil([&] { return !mixer.isAlive(); }, std::chrono::seconds(8));
    output->unblock(); // let the thread unstick so stop()/destruction can join it
    mixer.stop();

    CHECK(becameDead);
}
