// Route Csound's heap to SDRAM, leaving the platform's heap in SRAM.
//
// THE PROBLEM (spotykach QSPI build): the linker's default heap must stay in SRAM, because global
// constructors malloc before _hw.Init() powers up the SDRAM controller - a heap in SDRAM faults
// there. But Csound mallocs megabytes at csoundCreate/CompileCSD, far more than the ~270 KB SRAM
// heap holds.
//
// THE FIX: keep the default heap in SRAM (linker/alt_qspi.lds), and route ONLY Csound's allocations to a
// dedicated SDRAM pool, armed by CsoundEngine::init() (which runs AFTER _hw.Init(), so SDRAM is
// live by then). We intercept the C malloc family via linker --wrap; when armed, allocations come
// from the SDRAM pool, otherwise they pass through to the real SRAM heap.
//
// The pool is a free-capable allocator (CsoundPool, csound_pool.h), NOT a bump pool: free/realloc
// reclaim memory and coalesce, so csoundReset + recompile (patch swapping, roadmap #1/#2 in
// docs/dev/csound-impl.md) returns its megabytes to the pool instead of leaking them. On pool exhaustion
// we fall back to the real SRAM heap so a request never hard-fails; in_pool() then routes each
// pointer's free/realloc back to whichever heap it came from.
//
// ALIGNED ALLOCATIONS: Csound's memalloc.c (csoundCallocAligned, beta17+) calls C11 aligned_alloc for
// cache-line-aligned buffers (e.g. the circular buffer's 32-byte alignment). We --wrap it too, both to
// route those buffers into the SDRAM pool AND because nano-libc's aligned_alloc is implemented on top of
// posix_memalign, which nosys does not provide (link error: undefined reference to `posix_memalign`).
// The pool hands out 16-aligned payloads, so align<=16 requests pass straight through; for align>16 we
// over-allocate and return an aligned pointer whose 16-byte control slot (at handle-16) lets __wrap_free
// recover the real pool block. See __wrap_aligned_alloc below.
//
// Built only into the ENGINE=csound target (see the Makefile csound branch's --wrap LDFLAGS).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/csound/csound_pool.h"

#ifndef CSOUND_SDRAM_BSS
#define CSOUND_SDRAM_BSS __attribute__((section(".sdram_bss")))
#endif

extern "C" {
void* __real_malloc(size_t);
void  __real_free(void*);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
}

namespace {

constexpr size_t kPoolBytes = 12u * 1024u * 1024u;   // Csound setup is a few MB; SDRAM has room
alignas(16) CSOUND_SDRAM_BSS std::uint8_t g_pool[kPoolBytes];
spotykach::CsoundPool g_alloc;
bool g_armed = false;

inline bool in_pool(const void* p) {
    return p >= static_cast<const void*>(g_pool) && p < static_cast<const void*>(g_pool + kPoolBytes);
}

// Control slot planted at [handle-16, handle) for an over-aligned (align>16) allocation. `tag` sits
// exactly where block_of(handle)->size would be read, and has bit0 (the pool's used flag) clear, so
// g_alloc.is_used_payload(handle) reports false and the free path knows this is an aligned handle, not
// a direct payload. `base` is the real pool payload to release. 16 bytes, matching the pool's header.
struct AlignedCtl { std::uintptr_t tag; void* base; };
// bit0 (the pool's used flag) clear, so is_used_payload() reports false. All-ones-but-bit0 is also
// not a valid 16-multiple block size at any pointer width, a useful sanity sentinel.
constexpr std::uintptr_t kAlignedTag = ~static_cast<std::uintptr_t>(1);

inline AlignedCtl* aligned_ctl(void* handle) {
    return reinterpret_cast<AlignedCtl*>(reinterpret_cast<std::uintptr_t>(handle) - sizeof(AlignedCtl));
}

} // namespace

namespace spotykach {
// Lay down the free pool over the SDRAM array and arm interception. Call once, after _hw.Init()
// (SDRAM live), before csoundCreate.
void csound_heap_arm() noexcept {
    g_alloc.init(g_pool, kPoolBytes);
    g_armed = true;
}
}

extern "C" {

void __wrap_free(void* p);   // forward decl: __wrap_realloc's aligned-handle path frees via it

void* __wrap_malloc(std::size_t n) {
    if (!g_armed) return __real_malloc(n);
    void* p = g_alloc.alloc(n);
    return p ? p : __real_malloc(n);            // pool exhausted -> SRAM fallback
}

void* __wrap_calloc(std::size_t nmemb, std::size_t sz) {
    if (!g_armed) return __real_calloc(nmemb, sz);
    const std::size_t n = nmemb * sz;
    void* p = g_alloc.alloc(n ? n : 1);
    if (p) { std::memset(p, 0, n); return p; }
    return __real_calloc(nmemb, sz);            // fallback zeroes for us
}

void* __wrap_realloc(void* old, std::size_t n) {
    if (!old)          return __wrap_malloc(n);
    if (!in_pool(old)) return __real_realloc(old, n);   // a real (SRAM) block
    if (!g_alloc.is_used_payload(old)) {
        // An over-aligned handle (see __wrap_aligned_alloc). Csound never reallocs its aligned
        // buffers, so this is a defensive path: relocate to a fresh (16-aligned) block and copy.
        // realloc does not promise to preserve over-alignment, so dropping to 16 is conformant.
        void* base = aligned_ctl(old)->base;
        if (n == 0) { __wrap_free(old); return nullptr; }
        void* np = __wrap_malloc(n);
        if (!np) return nullptr;                        // old handle left intact
        const std::size_t avail = g_alloc.payload(base)
                                  - (static_cast<std::uint8_t*>(old) - static_cast<std::uint8_t*>(base));
        std::memcpy(np, old, avail < n ? avail : n);
        __wrap_free(old);
        return np;
    }
    if (n == 0)        { g_alloc.release(old); return nullptr; }
    void* p = g_alloc.grow(old, n);
    if (p) return p;
    // Pool can't satisfy the grow (even after coalescing): relocate to the SRAM heap, then free
    // the old pool block. realloc must preserve contents up to the smaller of old/new size.
    p = __real_malloc(n);
    if (p) {
        const std::size_t oldn = g_alloc.payload(old);
        std::memcpy(p, old, oldn < n ? oldn : n);
        g_alloc.release(old);
    }
    return p;                                   // null leaves old intact (realloc contract)
}

void __wrap_free(void* p) {
    if (!p) return;
    if (!in_pool(p)) { __real_free(p); return; }
    if (g_alloc.is_used_payload(p)) { g_alloc.release(p); return; }   // direct pool payload
    g_alloc.release(aligned_ctl(p)->base);                            // over-aligned handle
}

// aligned_alloc. Csound (memalloc.c) requests cache-line-aligned buffers; we serve them from the pool.
// C11 requires n to be a multiple of `align` and `align` a power of two, which Csound satisfies.
void* __wrap_aligned_alloc(std::size_t align, std::size_t n) {
    if (align <= 16) {                          // pool payloads are already 16-aligned
        if (!g_armed) return __real_malloc(n);
        void* p = g_alloc.alloc(n ? n : 1);
        return p ? p : __real_malloc(n);        // pool exhausted -> SRAM fallback (>=8-aligned)
    }
    // align > 16: over-allocate a normal 16-aligned block, then hand back a pointer aligned to `align`
    // with a control slot at [handle-16, handle). We keep over-aligned blocks pool-only: the recovery
    // in __wrap_free relies on in_pool(handle), so falling back to SRAM here would strand the handle.
    // The 12 MB pool is sized so this never fails for Csound's small aligned buffers; nullptr just
    // makes Csound's memdie longjmp cleanly rather than corrupt anything.
    if (!g_armed) return nullptr;
    const std::size_t slack = align + sizeof(AlignedCtl);   // worst-case shift + the control slot
    void* base = g_alloc.alloc(n + slack);
    if (!base) return nullptr;
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t handle = (raw + sizeof(AlignedCtl) + (align - 1))
                                  & ~(static_cast<std::uintptr_t>(align) - 1);
    AlignedCtl* ctl = aligned_ctl(reinterpret_cast<void*>(handle));
    ctl->tag  = kAlignedTag;
    ctl->base = base;
    return reinterpret_cast<void*>(handle);
}

} // extern "C"
