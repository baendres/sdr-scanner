#include "RfRingBuffer.h"

#include <gnuradio/io_signature.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace sdrscan {

RfRingBuffer::RfRingBuffer(size_t capacity) : buf_(capacity + 1) {}

void RfRingBuffer::write(const gr_complex* data, int n) {
    int written = 0;
    while (written < n) {
        std::unique_lock<std::mutex> lock(mutex_);
        size_t capacity = buf_.size();
        size_t spaceLeft = (head_ >= tail_) ? (capacity - (head_ - tail_) - 1) : (tail_ - head_ - 1);
        if (spaceLeft == 0) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        int toWrite = std::min<int>(n - written, static_cast<int>(spaceLeft));
        // May wrap - copy in at most two contiguous runs.
        int firstRun = std::min<int>(toWrite, static_cast<int>(capacity - head_));
        std::memcpy(&buf_[head_], data + written, firstRun * sizeof(gr_complex));
        if (toWrite > firstRun) {
            std::memcpy(&buf_[0], data + written + firstRun, (toWrite - firstRun) * sizeof(gr_complex));
        }
        head_ = (head_ + toWrite) % capacity;
        written += toWrite;
    }
}

int RfRingBuffer::read(gr_complex* dest, int maxItems) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t capacity = buf_.size();
    size_t available = (head_ >= tail_) ? (head_ - tail_) : (capacity - tail_ + head_);
    int toRead = static_cast<int>(std::min<size_t>(available, static_cast<size_t>(maxItems)));
    if (toRead <= 0) return 0;

    int firstRun = std::min<int>(toRead, static_cast<int>(capacity - tail_));
    std::memcpy(dest, &buf_[tail_], firstRun * sizeof(gr_complex));
    if (toRead > firstRun) {
        std::memcpy(dest + firstRun, &buf_[0], (toRead - firstRun) * sizeof(gr_complex));
    }
    tail_ = (tail_ + toRead) % capacity;
    return toRead;
}

RfRingBufferSinkBlock::RfRingBufferSinkBlock(std::shared_ptr<RfRingBuffer> ring)
    : gr::sync_block("RfRingBufferSink",
                      gr::io_signature::make(1, 1, sizeof(gr_complex)),
                      gr::io_signature::make(0, 0, 0)),
      ring_(std::move(ring)) {}

int RfRingBufferSinkBlock::work(int noutput_items,
                                 gr_vector_const_void_star& input_items,
                                 gr_vector_void_star& /*output_items*/) {
    const gr_complex* in = static_cast<const gr_complex*>(input_items[0]);
    ring_->write(in, noutput_items);
    return noutput_items;
}

RfRingBufferSourceBlock::RfRingBufferSourceBlock(std::shared_ptr<RfRingBuffer> ring)
    : gr::sync_block("RfRingBufferSource",
                      gr::io_signature::make(0, 0, 0),
                      gr::io_signature::make(1, 1, sizeof(gr_complex))),
      ring_(std::move(ring)) {}

int RfRingBufferSourceBlock::work(int noutput_items,
                                   gr_vector_const_void_star& /*input_items*/,
                                   gr_vector_void_star& output_items) {
    gr_complex* out = static_cast<gr_complex*>(output_items[0]);

    // Actively wait for data here rather than returning 0 immediately. This block has no
    // GNU Radio-native input connection (it bridges from a plain memory buffer filled by a
    // different flowgraph/thread), so the TPB scheduler has no buffer-notification machinery
    // to wake it promptly when new data lands - a 0 return leaves the delay before the next
    // work() call up to the scheduler's own backoff heuristics, which can be far longer than
    // the data was actually behind by. Polling here keeps that retry latency small and bounded
    // instead. The iteration cap is a safety valve so a genuinely stalled capture side can't
    // hang stop()/wait() on this block forever - it falls back to the old "return 0" behavior.
    constexpr int kMaxWaitIterations = 2000; // ~1s at 500us/iteration
    for (int i = 0; i < kMaxWaitIterations; i++) {
        int numRead = ring_->read(out, noutput_items);
        if (numRead > 0) return numRead;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return 0;
}

} // namespace sdrscan
