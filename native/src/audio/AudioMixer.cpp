#include "AudioMixer.h"
#include "../dsp/Const.h"

#include <gnuradio/io_signature.h>

#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace sdrscan {

namespace {
constexpr size_t kRingBufferCapacity = AUDIO_SAMPLERATE; // ~1s of audio per receiver
// Discard backlog beyond this to avoid unbounded latency buildup. Must stay comfortably above
// AudioMixer::kTargetLatencySeconds's worth of samples, or this would trim away the deliberate
// buffering margin that constant exists to maintain.
constexpr int kMixerBufferTargetLen = AUDIO_SAMPLERATE / 3; // ~333ms
}

///
// AudioRingBuffer

AudioRingBuffer::AudioRingBuffer(size_t capacity) : buf_(capacity + 1) {}

void AudioRingBuffer::write(const float* data, int n) {
    int written = 0;
    while (written < n) {
        std::unique_lock<std::mutex> lock(mutex_);
        size_t capacity = buf_.size();
        size_t spaceLeft;
        if (head_ >= tail_) {
            spaceLeft = capacity - (head_ - tail_) - 1;
        } else {
            spaceLeft = tail_ - head_ - 1;
        }
        if (spaceLeft == 0) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        int toWrite = std::min<int>(n - written, static_cast<int>(spaceLeft));
        for (int i = 0; i < toWrite; i++) {
            buf_[head_] = data[written + i];
            head_ = (head_ + 1) % capacity;
        }
        written += toWrite;
    }
}

int AudioRingBuffer::read(std::vector<float>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t capacity = buf_.size();
    int count = 0;
    while (tail_ != head_) {
        out.push_back(buf_[tail_]);
        tail_ = (tail_ + 1) % capacity;
        count++;
    }
    return count;
}

///
// AudioRingBufferSinkBlock

AudioRingBufferSinkBlock::AudioRingBufferSinkBlock(std::shared_ptr<AudioRingBuffer> ring)
    : gr::sync_block("AudioRingBufferSink",
                      gr::io_signature::make(1, 1, sizeof(float)),
                      gr::io_signature::make(0, 0, 0)),
      ring_(std::move(ring)) {}

int AudioRingBufferSinkBlock::work(int noutput_items,
                                    gr_vector_const_void_star& input_items,
                                    gr_vector_void_star& /*output_items*/) {
    const float* in = static_cast<const float*>(input_items[0]);
    ring_->write(in, noutput_items);
    return noutput_items;
}

///
// AudioMixer

AudioMixer::AudioMixer(int numInputStreams, std::vector<std::shared_ptr<AudioOutput>> outputs)
    : numInputStreams_(numInputStreams), outputs_(std::move(outputs)) {
    for (int i = 0; i < numInputStreams_; i++) {
        ringBuffers_.push_back(std::make_shared<AudioRingBuffer>(kRingBufferCapacity));
    }
}

AudioMixer::~AudioMixer() {
    stop();
}

std::shared_ptr<AudioRingBufferSinkBlock> AudioMixer::createReceiverSink(int receiverIndex) {
    return std::make_shared<AudioRingBufferSinkBlock>(ringBuffers_.at(receiverIndex));
}

void AudioMixer::start() {
    stopFlag_ = false;
    mixThread_ = std::thread(&AudioMixer::run, this);
}

void AudioMixer::stop() {
    stopFlag_ = true;
    if (mixThread_.joinable()) mixThread_.join();
}

void AudioMixer::run() {
    std::vector<std::deque<float>> mixBuffers(numInputStreams_);

    for (auto& o : outputs_) o->reconnect();

    // Matches Python's AudioServer.run() (`os.nice(-5)`, before its own mix loop) - the mixing
    // loop is a small, latency-sensitive amount of work competing for CPU against the receiver
    // threads' much heavier DSP (demod, filtering, tone-detect FFTs, ...). A higher scheduling
    // priority here means the OS is less likely to delay exactly the thread whose delays are
    // audible as clicks/gaps in every output stream at once, without meaningfully starving the
    // receiver threads (this thread does very little work per iteration). Requires
    // CAP_SYS_NICE (root, or the container's `privileged: true`, already needed for USB
    // access) - silently no-ops rather than failing if unavailable, same as Python's own
    // try/except around os.nice() there.
    if (setpriority(PRIO_PROCESS, gettid(), -5) != 0) {
        std::cerr << "AudioMixer: couldn't raise thread priority (needs root/CAP_SYS_NICE): "
                  << std::strerror(errno) << "\n";
    }

    auto startTime = std::chrono::steady_clock::now();
    int64_t samplesMixed = 0;

    // GNU Radio's own scheduler delivers decoded audio in bursts, not a smooth trickle - each
    // block in a channel's demod chain only runs once enough input has accumulated, and this
    // chain decimates the RF rate down to audio rate by a large factor. That burstiness is
    // normal, not a bug. Mixing with zero margin (i.e. always emitting exactly what "should"
    // exist by now per the wall clock) turns every gap between bursts into a permanent hole in
    // the output, even though the "missing" audio arrives moments later - it just lands after
    // the hole instead of filling it. Trailing the wall clock by this much gives bursty
    // production room to land before its samples are actually needed.
    constexpr double kTargetLatencySeconds = 0.1;

    // Diagnostic only (see the note below): counts samples where a stream's buffer was empty
    // at mix time, i.e. silence got fabricated in place of real (not-yet-produced) audio.
    std::vector<int64_t> starvedSamples(numInputStreams_, 0);
    auto lastStarvationReport = startTime;

    while (!stopFlag_) {
        for (int i = 0; i < numInputStreams_; i++) {
            std::vector<float> inBuf;
            int numRead = ringBuffers_[i]->read(inBuf);
            if (numRead > 0) {
                mixBuffers[i].insert(mixBuffers[i].end(), inBuf.begin(), inBuf.end());
            }
        }

        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        double targetElapsed = std::max(0.0, elapsed - kTargetLatencySeconds);
        int64_t samplesToMix = static_cast<int64_t>(targetElapsed * AUDIO_SAMPLERATE) - samplesMixed;

        std::vector<int16_t> newSamples;
        newSamples.reserve(static_cast<size_t>(std::max<int64_t>(0, samplesToMix)));
        for (int64_t s = 0; s < samplesToMix; s++) {
            float outSum = 0.0f;
            for (int i = 0; i < numInputStreams_; i++) {
                auto& buf = mixBuffers[i];
                if (!buf.empty()) {
                    outSum += buf.front();
                    buf.pop_front();
                } else {
                    // This stream's receiver hasn't produced this sample yet (it's paced by
                    // real RF/demod throughput, not wall clock) - contributing silence here
                    // rather than waiting is what keeps this loop's ~1ms tick rate, but it
                    // means the output stream gets a genuine gap, not just delivery jitter.
                    starvedSamples[i]++;
                }
            }
            int32_t iOut = static_cast<int32_t>(outSum * 32767.0f);
            if (iOut > 32767) iOut = 32767;
            if (iOut < -32767) iOut = -32767;
            newSamples.push_back(static_cast<int16_t>(iOut));
        }
        samplesMixed += samplesToMix;

        for (int i = 0; i < numInputStreams_; i++) {
            auto& buf = mixBuffers[i];
            if (static_cast<int>(buf.size()) > kMixerBufferTargetLen) {
                int toDrop = static_cast<int>(buf.size()) - kMixerBufferTargetLen;
                buf.erase(buf.begin(), buf.begin() + toDrop);
            }
        }

        for (auto& o : outputs_) o->send(newSamples);

        auto now = std::chrono::steady_clock::now();
        if (now - lastStarvationReport >= std::chrono::seconds(1)) {
            for (int i = 0; i < numInputStreams_; i++) {
                if (starvedSamples[i] > 0) {
                    double pct = 100.0 * static_cast<double>(starvedSamples[i]) / AUDIO_SAMPLERATE;
                    std::cerr << "AudioMixer: receiver " << i << " starved for " << starvedSamples[i]
                              << " samples in the last ~1s (~" << pct << "% silence-filled - "
                              << "its audio production is falling behind real time)\n";
                }
                starvedSamples[i] = 0;
            }
            lastStarvationReport = now;
        }

        // Yield rather than sleep: this loop's pacing is what determines how promptly mixed
        // audio gets generated and sent, so any coarseness here shows up directly as audible
        // gaps. Under WSL2/Hyper-V, short timed sleeps can get coalesced up to the VM's
        // timer-interrupt granularity (observed as a persistent ~100ms stall pattern that
        // survived unrelated fixes elsewhere in the pipeline) - sched_yield() cedes the CPU
        // without going through that timed-wait path, trading CPU usage for tighter pacing.
        std::this_thread::yield();
    }

    for (auto& o : outputs_) o->close();
}

} // namespace sdrscan
