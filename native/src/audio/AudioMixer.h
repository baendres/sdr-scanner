#pragma once

#include <gnuradio/sync_block.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "AudioOutput.h"

namespace sdrscan {

// Mutex-guarded circular buffer of floats, written by a receiver's flowgraph thread and
// drained by the AudioMixer thread. Replaces Python's HighPerformanceCircularBuffer, which
// existed only because the old design split receivers across OS processes and needed
// shared_memory; here everything is one process, so a plain guarded buffer is sufficient and
// far simpler.
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacity);

    // Blocks (briefly sleeping) if the buffer is full, same backpressure behavior the
    // Python version had - the mixer thread is expected to drain faster than receivers fill.
    void write(const float* data, int n);

    // Drains all currently available samples into `out` (appended), returns count read.
    int read(std::vector<float>& out);

private:
    std::vector<float> buf_;
    size_t head_ = 0;
    size_t tail_ = 0;
    mutable std::mutex mutex_;
};

// GNU Radio sink block placed at the end of a receiver's active ScanWindow flowgraph;
// forwards samples into that receiver's AudioRingBuffer. Direct analog of
// AudioServer.py's AudioSender_grEmbeddedPythonBlock.
class AudioRingBufferSinkBlock : public gr::sync_block {
public:
    explicit AudioRingBufferSinkBlock(std::shared_ptr<AudioRingBuffer> ring);

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

private:
    std::shared_ptr<AudioRingBuffer> ring_;
};

// Mixes the audio streams from every receiver down to one stream and fans it out to the
// configured AudioOutputs. Direct port of AudioServer.py's AudioServer, minus the
// multiprocessing/shared_memory plumbing (see AudioRingBuffer).
class AudioMixer {
public:
    AudioMixer(int numInputStreams, std::vector<std::shared_ptr<AudioOutput>> outputs);
    ~AudioMixer();

    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // Returns the flowgraph sink block a receiver should connect its active ScanWindow's
    // audio output to.
    std::shared_ptr<AudioRingBufferSinkBlock> createReceiverSink(int receiverIndex);

    void start();
    void stop();

private:
    void run();

    int numInputStreams_;
    std::vector<std::shared_ptr<AudioRingBuffer>> ringBuffers_;
    std::vector<std::shared_ptr<AudioOutput>> outputs_;

    std::thread mixThread_;
    std::atomic<bool> stopFlag_{false};
};

} // namespace sdrscan
