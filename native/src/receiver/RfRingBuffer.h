#pragma once

#include <gnuradio/sync_block.h>
#include <gnuradio/gr_complex.h>

#include <memory>
#include <mutex>
#include <vector>

namespace sdrscan {

// RF-rate analog of AudioMixer's AudioRingBuffer - bridges a continuously-running hardware
// capture flowgraph to a separate, freely start/stop-able per-window processing flowgraph.
//
// Why this exists: window hops used to retune by fully stopping and restarting the SDR's GNU
// Radio flowgraph, which forces the driver to fully re-negotiate the USB stream on every hop
// (SoapySDR logs "Allocating N zero-copy buffers" each time) - expensive, and severe over a
// virtualized USB passthrough. Switching to gr::top_block::lock()/unlock() for the same
// reconfiguration didn't help: GNU Radio's scheduler implements that "live reconfiguration" by
// internally stopping and restarting every block in the flowgraph anyway, including the
// hardware source - so the same expensive USB stream teardown/rebuild still happened on every
// hop, just via a different code path. The only way to guarantee the source is never touched
// again after its first start is to put it in a *different* flowgraph than the one that gets
// reconfigured per hop, bridged through plain memory instead of a GNU Radio connection.
class RfRingBuffer {
public:
    explicit RfRingBuffer(size_t capacity);

    // Blocks (briefly sleeping) if the buffer is full - the capture flowgraph is expected to
    // always run faster than it fills in practice; if it doesn't, this applies backpressure
    // rather than silently dropping RF samples.
    void write(const gr_complex* data, int n);

    // Reads up to maxItems into dest, returns the count actually read (0 if empty - the
    // caller, a source block, is expected to handle "nothing ready yet" gracefully).
    int read(gr_complex* dest, int maxItems);

private:
    std::vector<gr_complex> buf_;
    size_t head_ = 0;
    size_t tail_ = 0;
    mutable std::mutex mutex_;
};

// Sink for the always-running capture flowgraph: source -> this -> RfRingBuffer.
class RfRingBufferSinkBlock : public gr::sync_block {
public:
    explicit RfRingBufferSinkBlock(std::shared_ptr<RfRingBuffer> ring);

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

private:
    std::shared_ptr<RfRingBuffer> ring_;
};

// Source for the per-window processing flowgraph: this -> RfRingBuffer -> ScanWindowBlock.
class RfRingBufferSourceBlock : public gr::sync_block {
public:
    explicit RfRingBufferSourceBlock(std::shared_ptr<RfRingBuffer> ring);

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

private:
    std::shared_ptr<RfRingBuffer> ring_;
};

} // namespace sdrscan
