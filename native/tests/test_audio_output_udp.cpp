// Regression test for a real self-deadlock: AudioOutputUdp::send() locked mutex_ and then, when
// socketFd_ < 0, called reconnect() - which tried to lock that same non-recursive mutex_ again
// to clear the buffer. That's a guaranteed self-deadlock on the calling thread (std::mutex isn't
// reentrant), hit the very first time send() runs before any external reconnect() call, or any
// time a sendto() failure triggers an internal reconnect mid-stream. Since send() runs on
// AudioMixer's own thread, this would silently freeze all audio output and eventually take the
// whole process down once the liveness watchdog noticed the mixer had stopped ticking.

#include <catch2/catch_test_macros.hpp>

#include "../src/audio/AudioOutputUdp.h"

#include <chrono>
#include <future>
#include <thread>

using namespace sdrscan;

TEST_CASE("AudioOutputUdp::send() doesn't deadlock when the socket needs to (re)connect") {
    // Deliberately skip the usual up-front reconnect() call (AudioMixer::run() normally does
    // this before its loop) so socketFd_ starts at -1 and the very first send() call has to
    // take the internal reconnect path itself - exactly the branch that used to self-deadlock.
    AudioOutputUdp out("127.0.0.1", 40000); // arbitrary local port - just needs socket() to succeed

    // Run send() on its own thread with a bounded wait, rather than calling it directly: a
    // regression here needs to fail this test, not hang the whole suite/CI job forever. Uses a
    // plain std::promise/future (not std::async) specifically because std::async's future
    // blocks in its own destructor until the task completes - which would turn a real deadlock
    // back into an unbounded hang right as this scope ends, defeating the point of wait_for.
    std::promise<void> donePromise;
    std::future<void> doneFuture = donePromise.get_future();
    std::thread t([&] {
        std::vector<int16_t> samples(10, 0);
        out.send(samples);
        donePromise.set_value();
    });

    bool completed = doneFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (completed) {
        t.join();
    } else {
        // send() is genuinely stuck - don't join a thread that will never finish (only reached
        // on an actual regression).
        t.detach();
    }

    REQUIRE(completed);
}
