// Route ChucK's heap to SDRAM, leaving the platform's heap in SRAM.
//
// Identical strategy to csound_alloc.cpp (see its header comment for the full rationale): the
// linker's default heap must stay in SRAM, because global constructors malloc before _hw.Init()
// powers up the SDRAM controller - a heap in SDRAM faults there. But ChucK mallocs megabytes at
// `new ChucK()` / init() / compileCode(). So we keep the default heap in SRAM (linker/alt_qspi.lds) and
// route ONLY ChucK's allocations to a dedicated SDRAM pool, armed by ChuckEngine::init() (which runs
// AFTER _hw.Init(), so SDRAM is live). We intercept the C malloc family via linker --wrap; when armed,
// allocations come from the SDRAM pool, otherwise they pass through to the real SRAM heap.
//
// The pool is the same free-capable coalescing allocator the Csound engine uses (csound_pool.h - it
// is engine-agnostic, depending only on the C++ stdlib). ChucK is STL/heap-heavy and the eventual M3
// live-recompile (removeAllShreds + compileCode) churns the pool, so a bump pool would leak; this one
// coalesces on free. (Roadmap: promote csound_pool.h to a shared SdramPool; reused as-is here to keep
// the proven Csound build byte-identical.) On pool exhaustion we fall back to the real SRAM heap so a
// request never hard-fails; in_pool() then routes each pointer's free/realloc back to its heap.
//
// Built only into the ENGINE=chuck target (see the Makefile chuck branch's --wrap LDFLAGS).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/csound/csound_pool.h"

#ifndef CHUCK_SDRAM_BSS
#define CHUCK_SDRAM_BSS __attribute__((section(".sdram_bss")))
#endif

extern "C" {
void* __real_malloc(size_t);
void  __real_free(void*);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
}

namespace {

// ChucK's BASE footprint (built-in type system + the full STK UGen registration) is already several MB
// per VM, and that base is NOT freed by a patch reset (CK_MSG_CLEARVM only clears the user namespace),
// so a 12 MB pool sat near-full at baseline with no headroom for swapping - exhausting fast. Since the
// engine-arena shrink (SPK_NO_ENGINE_ARENA) freed ~48 MB of SDRAM, give ChucK a generous 40 MB pool:
// one VM is then a small fraction, leaving ample room for sustained patch swaps. (Total SDRAM: 40 MB
// pool + ~2 MB stream rings + token arena ~= 42 MB of 64 MB.)
constexpr size_t kPoolBytes = 40u * 1024u * 1024u;
alignas(16) CHUCK_SDRAM_BSS std::uint8_t g_pool[kPoolBytes];
spotykach::CsoundPool g_alloc;                        // the shared, engine-agnostic SDRAM allocator
bool g_armed = false;

inline bool in_pool(const void* p) {
    return p >= static_cast<const void*>(g_pool) && p < static_cast<const void*>(g_pool + kPoolBytes);
}

// The pool is shared between the main loop (ChucK setGlobalFloat -> new/delete) and the audio ISR
// (ck->run() -> VM allocations). CsoundPool is NOT reentrant - concurrent alloc/free corrupts its free
// list (we caught a HardFault in release() following a 0xaa..-pattern pointer). Serialize every pool
// op with a short PRIMASK critical section. RAII-restore so it nests (an ISR alloc inside is fine).
struct CritSec {
    uint32_t primask;
    CritSec()  { primask = __get_PRIMASK(); __disable_irq(); }
    ~CritSec() { if (!primask) __enable_irq(); }
};

} // namespace

namespace spotykach {
// Lay down the free pool over the SDRAM array and arm interception. Call once, after _hw.Init()
// (SDRAM live), before `new ChucK()`.
void chuck_heap_arm() noexcept {
    g_alloc.init(g_pool, kPoolBytes);
    g_armed = true;
}

// SDRAM pool diagnostics for the patch-swap leak hunt (read by chuck_engine.cpp). used_bytes() is an
// O(live-blocks) walk - call it ONLY off the audio hot path and with the audio path quiesced (e.g. once
// per patch swap inside do_reload, where the ReloadGate is taken so the ISR isn't allocating), NEVER
// per-block or per-render: a per-render walk under PRIMASK is what starved the ISR before (see the
// chuck-impl.md "observer-effect meter bug"). No CritSec here - the caller guarantees no concurrent
// pool mutation at the sample point.
std::size_t chuck_pool_used()     noexcept { return g_alloc.used_bytes(); }
std::size_t chuck_pool_capacity() noexcept { return g_alloc.capacity(); }
}

extern "C" {

void* __wrap_malloc(std::size_t n) {
    if (!g_armed) return __real_malloc(n);
    void* p;
    { CritSec cs; p = g_alloc.alloc(n); }
    return p ? p : __real_malloc(n);            // pool exhausted -> SRAM fallback
}

void* __wrap_calloc(std::size_t nmemb, std::size_t sz) {
    if (!g_armed) return __real_calloc(nmemb, sz);
    const std::size_t n = nmemb * sz;
    void* p;
    { CritSec cs; p = g_alloc.alloc(n ? n : 1); }
    if (p) { std::memset(p, 0, n); return p; }
    return __real_calloc(nmemb, sz);            // fallback zeroes for us
}

void* __wrap_realloc(void* old, std::size_t n) {
    if (!old)          return __wrap_malloc(n);
    if (!in_pool(old)) return __real_realloc(old, n);   // a real (SRAM) block
    if (n == 0)        { CritSec cs; g_alloc.release(old); return nullptr; }
    void* p;
    { CritSec cs; p = g_alloc.grow(old, n); }
    if (p) return p;
    // Pool can't satisfy the grow (even after coalescing): relocate to the SRAM heap, then free the
    // old pool block. realloc must preserve contents up to the smaller of old/new size.
    p = __real_malloc(n);
    if (p) {
        std::size_t oldn;
        { CritSec cs; oldn = g_alloc.payload(old); }
        std::memcpy(p, old, oldn < n ? oldn : n);
        { CritSec cs; g_alloc.release(old); }
    }
    return p;                                   // null leaves old intact (realloc contract)
}

void __wrap_free(void* p) {
    if (!p) return;
    if (in_pool(p)) { CritSec cs; g_alloc.release(p); }
    else            __real_free(p);
}

} // extern "C"
