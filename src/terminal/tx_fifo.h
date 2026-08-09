// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Layer [1] TX side: the outgoing reply FIFO, drained non-blocking each process(). Everything here
// runs on the main loop (dispatch enqueues, flush_tx drains), so it is single-threaded - no atomics.
// See docs/dev/terminal-transport.md.
//
// Sized to hold a full `describe` dump (~1.5 KB) so verb_describe can be a plain sequence of write()
// calls that drains over successive process() iterations (2 KB SRAM is trivial on the H750). peek()
// copies the pending bytes into caller scratch WITHOUT advancing; commit() advances only after a
// successful TransmitInternal, so a busy/absent host never loses queued bytes. A full FIFO drops and
// latches _overflow, which the transport reports as `err overflow` on the next drained line.

#include <cstddef>
#include <cstdint>

namespace spotykach {

class TxFifo {
  public:
    // Append n bytes; on insufficient space, drop the whole write and latch overflow (partial replies
    // would corrupt the line-framed protocol, so it is all-or-nothing).
    void enqueue(const char* s, size_t n) {
        if (n > kCap - count()) { _overflow = true; return; }
        for (size_t i = 0; i < n; ++i)
            _buf[(_head + i) & kMask] = static_cast<uint8_t>(s[i]);
        _head += n;
    }

    // Copy up to max pending bytes into dst without advancing (a snapshot for TransmitInternal).
    size_t peek(uint8_t* dst, size_t max) const {
        size_t n = count();
        if (n > max) n = max;
        for (size_t i = 0; i < n; ++i)
            dst[i] = _buf[(_tail + i) & kMask];
        return n;
    }

    // Advance past n bytes after a successful transmit.
    void commit(size_t n) { _tail += n; }

    size_t count() const { return static_cast<size_t>(_head - _tail); }
    size_t free_space() const { return kCap - count(); }
    bool   empty() const { return _head == _tail; }
    bool   take_overflow() { bool o = _overflow; _overflow = false; return o; }

  private:
    // Line codec: 2 KB holds a full describe dump, which drains over successive process() iterations.
    // OSC: the descriptor is ONE bundle and a bundle cannot be streamed, so the whole thing has to fit
    // at once - which turns the dispatch doc's FIFO-sizing recommendation into a requirement. 8 KB
    // rather than the spec's projected 4 KB because that projection was taken from a MASKED engine; an
    // engine still on the default all-live masks advertises ~6 KB of descriptor. See osc_addr.cpp.
#if SPK_TERMINAL_OSC
    static constexpr uint32_t kCap  = 8192;   // power of two
#else
    static constexpr uint32_t kCap  = 2048;   // holds a full describe dump; power of two
#endif
    static constexpr uint32_t kMask = kCap - 1;
    uint8_t  _buf[kCap];
    uint32_t _head = 0;   // write cursor (free-running, masked)
    uint32_t _tail = 0;   // read cursor
    bool     _overflow = false;
};

}  // namespace spotykach
