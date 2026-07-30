// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// A free-capable allocator over a fixed byte pool, for Csound's SDRAM heap (roadmap #2 in
// docs/dev/csound-impl.md). It replaces the original bump pool: a bump allocator never reclaims, so
// csoundReset + recompile (patch swapping) exhausts the 12 MB pool after a handful of swaps. This
// allocator coalesces on free, so a reset's freed megabytes return to the pool and patch-swapping
// is sustainable.
//
// DESIGN: a classic boundary-tag allocator with segregated free lists.
//   - Every block carries a 16-byte header { total size (low bit = in-use), size of the physical
//     previous block }. The prev-size field is the boundary tag that lets free() find and coalesce
//     the backward neighbour with no separate footer; the forward neighbour is (block + size).
//   - Free blocks are threaded on doubly-linked lists segregated by size class (floor(log2)), so
//     alloc() finds a fit in roughly O(1) instead of scanning the whole heap.
//   - alloc() splits an oversized block; free() coalesces both neighbours; realloc() grows in place
//     when the forward neighbour is free (and shrinks in place), else copies.
//
// Payloads are 16-byte aligned (matches the original pool's guarantee). The unit is header-only and
// depends only on the C++ standard library (no Csound, no Daisy), so host/test_csound_alloc.cpp
// exercises it directly against a small pool. csound_alloc.cpp instantiates one over the 12 MB
// .sdram_bss array and routes the --wrap'd C malloc family to it.
//
// CONCURRENCY: not thread-safe / not reentrant (neither was the bump pool it replaces). Today this
// is fine - Csound allocates at init() (main thread) and, for an already-compiled orchestra, the
// audio ISR's csoundPerformKsmps does not allocate from core opcodes. When roadmap #1/#3 add live
// recompile or note-triggered instrument instantiation, allocation can occur inside the ISR while
// the main loop is mid-call; that path must be serialised (e.g. recompile from the main loop with
// the audio path quiesced) before it is relied upon.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace spotykach {

class CsoundPool {
public:
    // Lay down a single free block spanning the pool. base must be 16-aligned (the .sdram_bss array
    // is alignas(16)); bytes is rounded down to a 16-multiple. Called from csound_heap_arm(), i.e.
    // AFTER _hw.Init() so the SDRAM the pool lives in is powered up.
    void init(void* base, std::size_t bytes) noexcept
    {
        _base = static_cast<std::uint8_t*>(base);
        _cap  = bytes & ~static_cast<std::size_t>(kAlign - 1);
        for (int i = 0; i < kClasses; i++) _heads[i] = nullptr;
        if (_cap < kMinBlock) { _cap = 0; return; }
        Block* b = reinterpret_cast<Block*>(_base);
        b->size  = _cap;          // whole pool, free (low bit clear)
        b->prev  = 0;             // no physical previous block
        link(b);
    }

    // malloc. Returns a 16-aligned payload, or nullptr if the pool can't satisfy the request (the
    // caller then falls back to the SRAM heap).
    void* alloc(std::size_t n) noexcept
    {
        const std::size_t need = block_bytes(n);
        Block* b = find_fit(need);
        if (!b) return nullptr;
        unlink(b);
        b->size |= kUsed;         // mark used BEFORE carve, so the remainder can't coalesce back in
        carve(b, need);           // split off the remainder if it's worth keeping
        return payload_of(b);
    }

    // free. p must be a payload this pool returned (the caller routes non-pool pointers elsewhere).
    void release(void* p) noexcept
    {
        if (!p) return;
        Block* b = block_of(p);
        b->size &= ~kUsed;                      // mark free

        Block* nb = next(b);                    // coalesce forward
        if (nb && !used(nb)) {
            unlink(nb);
            b->size = size_of(b) + size_of(nb);
        }
        Block* pb = prevb(b);                   // coalesce backward
        if (pb && !used(pb)) {
            unlink(pb);
            pb->size = size_of(pb) + size_of(b);
            b = pb;
        }
        fix_next_prev(b);                       // the surviving block's forward neighbour
        link(b);
    }

    // realloc. Grows in place when the forward neighbour is free and big enough, shrinks in place,
    // else allocates a new block and copies. Returns nullptr only if a required new block can't be
    // had (the old block is left intact in that case, per realloc semantics).
    void* grow(void* p, std::size_t n) noexcept
    {
        Block*            b   = block_of(p);
        const std::size_t cur = size_of(b);
        const std::size_t need = block_bytes(n);

        if (need <= cur) {                      // same size or shrink
            carve(b, need);                     // split off the tail and return it to the pool
            return p;
        }

        Block* nb = next(b);                    // try to absorb the forward neighbour
        if (nb && !used(nb) && cur + size_of(nb) >= need) {
            unlink(nb);
            b->size = (cur + size_of(nb)) | kUsed;
            fix_next_prev(b);
            carve(b, need);
            return p;
        }

        void* np = alloc(n);                    // relocate
        if (!np) return nullptr;                // old block untouched
        const std::size_t old_payload = cur - kHdr;
        std::memcpy(np, p, old_payload < n ? old_payload : n);
        release(p);
        return np;
    }

    // Usable payload bytes of an allocated pool block (for the realloc-to-SRAM copy in the shim).
    std::size_t payload(void* p) const noexcept { return size_of(block_of(p)) - kHdr; }

    // True iff p is the payload of a currently-allocated (used) pool block. The aligned_alloc shim
    // (csound_alloc.cpp) uses this to tell a direct pool payload from an over-aligned handle: an
    // over-aligned handle stores a control word (bit0 clear) where block_of(handle)->size would sit,
    // so this reports false for it. A genuine live payload's size field always has the used bit set,
    // making the distinction collision-free.
    bool is_used_payload(const void* p) const noexcept
    {
        return reinterpret_cast<const Block*>(static_cast<const u8*>(p) - kHdr)->size & kUsed;
    }

    bool owns(const void* p) const noexcept
    {
        return p >= _base + kHdr && p < _base + _cap;
    }

    // --- introspection (tests + a future memory meter) ----------------------------------------
    std::size_t capacity()  const noexcept { return _cap; }
    std::size_t used_bytes() const noexcept
    {
        std::size_t u = 0;
        for (std::uint8_t* p = _base; p < _base + _cap;) {
            const Block* b = reinterpret_cast<const Block*>(p);
            if (used(b)) u += size_of(b);
            p += size_of(b);
        }
        return u;
    }
    std::size_t largest_free() const noexcept
    {
        std::size_t m = 0;
        for (std::uint8_t* p = _base; p < _base + _cap;) {
            const Block* b = reinterpret_cast<const Block*>(p);
            if (!used(b) && size_of(b) > m) m = size_of(b);
            p += size_of(b);
        }
        return m > kHdr ? m - kHdr : 0;
    }
    // Walk every block and assert the boundary-tag invariants hold. Test-only sanity, but cheap.
    bool validate() const noexcept
    {
        std::uint8_t* p = _base;
        std::size_t   prev = 0;
        while (p < _base + _cap) {
            const Block* b = reinterpret_cast<const Block*>(p);
            const std::size_t s = size_of(b);
            if (s < kMinBlock)             return false;   // no undersized block
            if (s & (kAlign - 1))          return false;   // every block 16-aligned in size
            if (b->prev != prev)           return false;   // backward tag matches the real neighbour
            prev = s;
            p   += s;
        }
        return p == _base + _cap;                          // blocks tile the pool exactly
    }

private:
    using u8 = std::uint8_t;

    // 16-byte header so the payload that follows is 16-aligned. `size` is the block's total byte
    // count including the header; its low bit is the in-use flag (sizes are 16-multiples, so the
    // low 4 bits are free). `prev` is the total size of the physically-preceding block (0 if none).
    struct Block { std::size_t size; std::size_t prev; };
    // A free block stores list links in the first 16 bytes of its (otherwise unused) payload.
    struct Node  { Node* next; Node* prev; };

    static constexpr std::size_t kAlign    = 16;
    static constexpr std::size_t kHdr      = 16;            // >= sizeof(Block), 16-aligned
    static constexpr std::size_t kMinBlock = kHdr + 16;     // header + room for the free-list Node
    static constexpr std::size_t kUsed     = 1;
    static constexpr int         kClasses  = 28;            // floor(log2) buckets; covers > 2^33

    static std::size_t align_up(std::size_t n) noexcept
    {
        return (n + (kAlign - 1)) & ~static_cast<std::size_t>(kAlign - 1);
    }
    // Total block bytes needed to hold an n-byte payload (header + aligned payload, >= kMinBlock).
    static std::size_t block_bytes(std::size_t n) noexcept
    {
        const std::size_t b = kHdr + align_up(n ? n : 1);
        return b < kMinBlock ? kMinBlock : b;
    }
    static std::size_t size_of(const Block* b) noexcept { return b->size & ~static_cast<std::size_t>(kAlign - 1); }
    static bool        used(const Block* b)    noexcept { return b->size & kUsed; }
    static void*       payload_of(Block* b)    noexcept { return reinterpret_cast<u8*>(b) + kHdr; }
    static Block*      block_of(void* p)       noexcept { return reinterpret_cast<Block*>(static_cast<u8*>(p) - kHdr); }
    static Node*       node_of(Block* b)       noexcept { return reinterpret_cast<Node*>(reinterpret_cast<u8*>(b) + kHdr); }
    static Block*      block_of_node(Node* n)  noexcept { return reinterpret_cast<Block*>(reinterpret_cast<u8*>(n) - kHdr); }

    Block* next(Block* b) const noexcept
    {
        u8* p = reinterpret_cast<u8*>(b) + size_of(b);
        return p < _base + _cap ? reinterpret_cast<Block*>(p) : nullptr;
    }
    Block* prevb(Block* b) const noexcept
    {
        return b->prev ? reinterpret_cast<Block*>(reinterpret_cast<u8*>(b) - b->prev) : nullptr;
    }
    // Keep the forward neighbour's boundary tag in sync after b's size changes.
    void fix_next_prev(Block* b) noexcept
    {
        if (Block* nb = next(b)) nb->prev = size_of(b);
    }

    static int size_class(std::size_t s) noexcept
    {
        int lg = 0;                              // floor(log2(s)); s >= kMinBlock >= 32
        while (s >>= 1) lg++;
        int c = lg - 5;                          // 2^5 = 32 -> class 0
        if (c < 0) c = 0;
        if (c >= kClasses) c = kClasses - 1;
        return c;
    }

    void link(Block* b) noexcept
    {
        const int c = size_class(size_of(b));
        Node* n = node_of(b);
        n->prev = nullptr;
        n->next = _heads[c];
        if (_heads[c]) _heads[c]->prev = n;
        _heads[c] = n;
    }
    void unlink(Block* b) noexcept
    {
        const int c = size_class(size_of(b));
        Node* n = node_of(b);
        if (n->prev) n->prev->next = n->next; else _heads[c] = n->next;
        if (n->next) n->next->prev = n->prev;
    }

    // Smallest-class-first search for a block big enough for `need` total bytes.
    Block* find_fit(std::size_t need) noexcept
    {
        for (int c = size_class(need); c < kClasses; c++)
            for (Node* n = _heads[c]; n; n = n->next) {
                Block* b = block_of_node(n);
                if (size_of(b) >= need) return b;   // any node in a higher class fits trivially
            }
        return nullptr;
    }

    // If block b (currently unlinked + treated as the allocation) is larger than `need` by at least
    // a whole minimum block, split the tail off and return it to the free pool. b keeps `need` bytes.
    // The caller is responsible for b's in-use flag; the remainder is always made free here.
    void carve(Block* b, std::size_t need) noexcept
    {
        const std::size_t total = size_of(b);
        if (total - need < kMinBlock) return;        // remainder too small to bother; leave it in b
        const bool b_used = used(b);
        Block* r = reinterpret_cast<Block*>(reinterpret_cast<u8*>(b) + need);
        r->size  = total - need;                     // free
        r->prev  = need;
        b->size  = need | (b_used ? kUsed : 0);
        fix_next_prev(r);                            // r's forward neighbour now points back to r
        release(payload_of(r));                      // coalesce the tail forward + thread it
    }

    u8*         _base = nullptr;
    std::size_t _cap  = 0;
    Node*       _heads[kClasses] = {nullptr};
};

} // namespace spotykach
