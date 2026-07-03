// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Layer [1] of the terminal channel: the lock-free SPSC byte ring between the USB RX interrupt
// (producer) and the main loop (consumer). See docs/dev/terminal-transport.md.
//
// Single core, single cache: no cache-coherency problem between the IRQ and the main loop (the same
// core sees its own cache). The only hazards are compiler/CPU reordering, handled by `volatile`
// free-running indices plus a __DMB() between the data copy and the index publish (producer) and
// between the index read and the data read (consumer). This is a standard Cortex-M7 SPSC ring, not a
// multi-core structure. The buffer lives in default SRAM/DTCM (512 B, not DMA memory, no cache
// maintenance) - keep it out of SDRAM.

#include <cstddef>
#include <cstdint>

namespace spotykach {

// Data-memory barrier between the data copy and the index publish/read (mirrors CMSIS __DMB, without
// pulling the whole CMSIS/daisy header into this file). A compiler "memory" clobber also blocks
// reordering; on the single-core M7 the DMB pairs the producer's publish with the consumer's read.
#if defined(__arm__)
static inline void spk_dmb() { __asm volatile("dmb 0xF" ::: "memory"); }
#else
static inline void spk_dmb() { __asm volatile("" ::: "memory"); }   // host: compiler barrier only
#endif

class RxRing {
  public:
    // Producer (USB IRQ). Copies as many bytes as fit; drops the remainder and latches _overflow.
    void push(const uint8_t* src, size_t n) {
        uint32_t head = _head;                        // producer owns _head
        uint32_t freeb = kCap - (head - _tail);       // _tail is volatile (written by consumer)
        if (n > freeb) { _overflow = true; n = freeb; }
        for (size_t i = 0; i < n; ++i)
            _buf[(head + i) & kMask] = src[i];
        spk_dmb();                                       // data lands before the index is published
        _head = head + n;                              // publish
    }

    // Consumer (main loop). Returns bytes copied out, up to max.
    size_t pop(uint8_t* dst, size_t max) {
        uint32_t tail  = _tail;                        // consumer owns _tail
        uint32_t avail = _head - tail;                 // _head is volatile (written by producer)
        spk_dmb();                                       // read index before reading data
        size_t n = avail < max ? avail : max;
        for (size_t i = 0; i < n; ++i)
            dst[i] = _buf[(tail + i) & kMask];
        _tail = tail + n;                              // publish
        return n;
    }

    bool take_overflow() { bool o = _overflow; _overflow = false; return o; }

  private:
    static constexpr uint32_t kCap  = 512;             // power of two; line commands are short
    static constexpr uint32_t kMask = kCap - 1;
    uint8_t           _buf[kCap];
    volatile uint32_t _head = 0;                       // producer -> consumer
    volatile uint32_t _tail = 0;                       // consumer -> producer
    volatile bool     _overflow = false;
};

}  // namespace spotykach
