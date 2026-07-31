// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
//
// SKETCH - QSPI-target only. Compiles against libchuck.a (chuck.h on the include path, the shim
// sysroot + ck_prelude.h force-included) in the BOOT_QSPI build; not part of the SRAM engine bundle.
// See chuck_engine.h / docs/dev/chuck-impl.md.

#include "engine/chuck/chuck_engine.h"
#include "engine/chuck/chuck_patch.h"   // chuck_path / scan_chuck_patches / aux_to_index / read_program
#include "engine/indicators.h"          // shared indicator toolkit (docs/dev/indicator-comparison.md §7)
#include "config.h"                     // Config::dynamic() midi_channel_a/b for the channel->deck map

// NOTE: we deliberately do NOT pull in the shim's ck_prelude.h here. That force-include exists so
// ChucK's *source* (compiled into libchuck.a) finds the POSIX functions it calls without including
// their headers; the engine TU only needs the ChucK *headers* to parse, and it already includes the
// Daisy stack (via iengine.h), whose newlib unistd.h declares usleep/getcwd/... - pulling in the
// prelude on top of that just yields conflicting redeclarations (usleep(unsigned) vs useconds_t). The
// ChucK feature defines (__DISABLE_THREADS__, __PLATFORM_LINUX__, ...) come from the Makefile chuck
// branch and MUST match the set libchuck.a was built with (ABI: they drop members from ChucK classes).
#include "chuck.h"            // provided by the QSPI build's -Ithirdparty/chuck/src/core
#include "chuck_globals.h"    // Chuck_Globals_Manager: setGlobalFloat (host -> .ck program)
#include "chuck_vm.h"         // Chuck_VM::spork (re-spork cached code) + Chuck_VM_Code (the cached bytecode)
#include "chuck_compile.h"    // Chuck_Compiler::output() - the just-emitted code to pin in the cache

#ifdef METER
#include "meter.h"            // Meter::cpu().load - the platform's whole-callback CpuLoadMeter, driven
                             // by app.cpp's OnBlockStart/OnBlockEnd. The on-panel meter reads it in render().
#endif

#include <cstdlib>            // malloc/free (routed to the SDRAM pool by chuck_alloc.cpp's --wrap)
#include <cstdio>             // snprintf - format the bring-up error capture
#include <cstring>            // strncpy
#include <cmath>             // std::sqrt - per-channel output RMS for the level meter
#include <typeinfo>          // typeid - name the thrown exception type in the capture
#include <exception>         // std::exception

// Bring-up bisection (build: make engine-chuck CHUCKLVL=N). Splits ChucK init into stages so the
// first stage that fails to boot (panel goes solid-white instead of rendering) is the culprit:
//   0 = skip all (= NOCHUCK)         1 = new ChucK() + setParam only
//   2 = + ChucK::init() (loads built-in types/UGens)   3 = + compileCode() (the recursive parser) = full
#ifndef CHUCK_RUNTIME_LEVEL
#define CHUCK_RUNTIME_LEVEL 3
#endif
#ifdef CHUCK_SKIP_RUNTIME            // NOCHUCK=1 is the level-0 alias
#undef  CHUCK_RUNTIME_LEVEL
#define CHUCK_RUNTIME_LEVEL 0
#endif

namespace spotykach {

// Arms the SDRAM pool for ChucK's allocations (chuck_alloc.cpp). Called after _hw.Init() (so SDRAM is
// live), before `new ChucK()`. No-op-safe if the --wrap allocator isn't linked.
void chuck_heap_arm() noexcept;
// SDRAM pool diagnostics (chuck_alloc.cpp): the patch-swap leak probe. used() is an O(live-blocks) walk -
// call only with the audio path quiesced (note_pool_usage(), at a swap), never per-block/per-render.
std::size_t chuck_pool_used()     noexcept;
std::size_t chuck_pool_capacity() noexcept;

// ---------------------------------------------------------------------------------------------
// Built-in fallback program. The engine compiles this when there is no SD .ck (everything, for now -
// the SD bank is M3). It mirrors Csound's kOrchestra: a single drone voice reading the panel globals,
// so a card-less unit always sounds. The globals it declares are the engine's param vocabulary - keep
// them in sync with channel_for() below; an SD program names the same globals to be driven by the same
// knobs. Globals default to 0 until the host writes them (set_param -> setGlobalFloat).
//
// SawOsc -> LPF -> dac drone: Speed sweeps the pitch, Size opens the filter, Mix sets the level (with
// a floor so there is always sound at Mix=0 on boot). A tight control loop (10::ms) repolls globals.
// ---------------------------------------------------------------------------------------------
static const char* kProgram = R"chuck(
global float speedA;
global float mixA;
global float sizeA;

SawOsc s => LPF f => dac;
while( true )
{
    110.0 + speedA * 770.0    => s.freq;     // Speed -> pitch (110..880 Hz)
    1500.0 + sizeA * 6000.0   => f.freq;     // Size  -> brightness (undriven => 1500 Hz)
    (0.15 + mixA * 0.85) * 0.3 => s.gain;    // Mix   -> level, with a floor so boot is audible
    10::ms => now;
}
)chuck";

// Map a platform ParamId (+ deck) to a ChucK global name + a cache slot, or nullptr for params this
// engine ignores. The 'A'/'B' suffix lets one program carry both decks. Eight slots per deck
// (kSlots=16). A program reads whichever of these globals it declares; undeclared writes are dropped
// by ChucK harmlessly. Mirrors CsoundEngine::channel_for so the two engines share a knob vocabulary.
static const char* global_for(ParamId id, DeckRef::Ref d, int& slot)
{
    const bool A = (d == DeckRef::A);
    const int  base = A ? 0 : 8;
    switch (id) {
        case ParamId::Speed:    slot = base + 0; return A ? "speedA"  : "speedB";
        case ParamId::Mix:      slot = base + 1; return A ? "mixA"    : "mixB";
        case ParamId::Size:     slot = base + 2; return A ? "sizeA"   : "sizeB";
        case ParamId::Env:      slot = base + 3; return A ? "envA"    : "envB";
        case ParamId::Feedback: slot = base + 4; return A ? "fbA"     : "fbB";
        case ParamId::ModSpeed: slot = base + 5; return A ? "modspA"  : "modspB";
        case ParamId::ModAmp:   slot = base + 6; return A ? "modampA" : "modampB";
        default:                slot = -1;       return nullptr;
    }
}

// The (ParamId) set global_for maps - iterated to reseed a freshly compiled program's globals.
static const ParamId kMappedParams[] = {
    ParamId::Speed, ParamId::Mix, ParamId::Size, ParamId::Env,
    ParamId::Feedback, ParamId::ModSpeed, ParamId::ModAmp,
};

// Scratch buffer size for an SD-loaded program. A `.ck` patch is a few KB; 64 KB is generous head-
// room. It is malloc'd from the (armed) SDRAM pool, used only during compile, then freed.
static constexpr int kPatchMax = 64 * 1024;

// Map a linear amplitude (0..1, where 1.0 = 0 dBFS) to a 0..1 ring position on a dB scale
// (kMeterFloorDb .. 0 dB). A linear ring barely moves for musical signal - everything crowds the
// bottom - so the level meter uses dB to read like a real meter. Returns 0 at/below the floor, 1 at or
// above 0 dBFS. (The CPU meter stays linear: it is a fraction of the block budget, not a signal level.)
static inline float meter_db_norm(float amp)
{
    constexpr float kMeterFloorDb = -60.f;
    if (amp <= 0.f) return 0.f;
    const float db = 20.f * std::log10(amp);
    if (db <= kMeterFloorDb) return 0.f;
    if (db >= 0.f)           return 1.f;
    return (db - kMeterFloorDb) / -kMeterFloorDb;   // (db + 60) / 60
}

// --- Bring-up crash capture (QSPI Pod debug; see docs/dev/chuck-pod-poc.md) -------------------
// On bare metal an uncaught C++ exception out of ChucK init runs std::terminate -> abort, which spins
// forever in newlib's _exit (dark LED, all fault registers zero - exactly the Pod symptom we localized
// over SWD). We wrap the runtime bring-up in try/catch: on a throw we record which stage was running
// and the exception type + what(), leave the gate unpublished (silent, recoverable boot), let it run
// instead of aborting. Read g_chuck_init_stage / g_chuck_init_error over SWD to see the root cause.
extern "C" {
volatile uint32_t g_chuck_init_stage     = 0;    // 1=new ChucK  2=init()  3=compileCode()  9=succeeded
char              g_chuck_init_error[192] = "";   // cause on failure ("" if none); read over SWD

// Patch-swap leak instrumentation. note_pool_usage() refreshes these once per swap (and at init), with
// the ISR quiesced. A used_kb that climbs monotonically across swaps (never recovering) is the ChucK
// per-VM type-system leak (chuck-impl.md root cause). Read over SWD on the bare Pod (mdw the addresses
// from build/spotykach.map); the cased unit reads the same figure off the panel in a METER build.
volatile uint32_t g_chuck_pool_used_kb = 0;   // live pool usage right after the last swap
volatile uint32_t g_chuck_pool_peak_kb = 0;   // high-water mark across all swaps (only rises on a leak)
volatile uint32_t g_chuck_pool_cap_kb  = 0;   // pool capacity (constant once armed)
volatile uint32_t g_chuck_swaps        = 0;   // completed patch swaps; pair with used_kb for per-swap growth

// Override newlib's weak abort/__assert_func so a bare-metal assert/abort (which otherwise spins
// silently in _exit - the dark-LED symptom) instead RECORDS its cause into g_chuck_init_error before
// parking. This catches the failures the try/catch can't: a fired assert() in ChucK's compiler, or an
// exception that std::terminate->abort()s because it could not unwind through the generated C parser
// frames (chuck.tab.c / chuck.yy.c, built without unwind tables) to reach our catch. Read the buffer
// over SWD to see file:line:expr (assert) or the caller address (abort -> addr2line).
void __assert_func(const char* file, int line, const char* func, const char* expr)
{
    std::snprintf(g_chuck_init_error, sizeof(g_chuck_init_error), "assert %s:%d %s(): %s",
                  file ? file : "?", line, func ? func : "?", expr ? expr : "?");
    while (1) { __asm__ volatile("nop"); }        // park; cause captured for SWD readout
}

void abort(void)
{
    // Crude backtrace: this toolchain's nano libstdc++ compiles every std::__throw_* to `bl abort`
    // (no exceptions), so abort() is the only signal. Scan the live stack for return addresses in the
    // QSPI .text range and list the first several; addr2line them to recover the ChucK call path that
    // reached the throw helper. (-O2 omits frame pointers, so a precise unwind isn't available.)
    if (g_chuck_init_error[0] == '\0') {          // don't clobber a more specific assert message
        uintptr_t sp;
        __asm__ volatile("mov %0, sp" : "=r"(sp));
        char* p = g_chuck_init_error;
        char* const end = p + sizeof(g_chuck_init_error);
        p += std::snprintf(p, end - p, "abort bt:");
        const uintptr_t* s = reinterpret_cast<const uintptr_t*>(sp);
        for (int i = 0, n = 0; i < 256 && n < 12 && p < end - 10; i++) {
            const uintptr_t v = s[i];
            if (v >= 0x90040000u && v < 0x90200000u && (v & 1u)) {   // thumb return addr in QSPI text
                p += std::snprintf(p, end - p, " %08lx", static_cast<unsigned long>(v & ~1u));
                n++;
            }
        }
    }
    while (1) { __asm__ volatile("nop"); }
}
}

// ---------------------------------------------------------------------------------------------
// VM build (one persistent instance) + compile-once cache + patch bank
// ---------------------------------------------------------------------------------------------
ChucK* ChuckEngine::build_vm()
{
#if CHUCK_RUNTIME_LEVEL == 0
    return nullptr;                              // NOCHUCK: skip the runtime entirely
#else
    ChucK* ck = nullptr;
    try {
        // g_chuck_init_stage / g_chuck_init_error: bring-up readout over SWD (1=new 2=init 3=compile
        // 9=ok). On a throw, the nano libstdc++ may abort() uncatchably; the -u _printf_float fix +
        // the abort/__assert_func overrides capture the cause instead of spinning silently.
        g_chuck_init_stage = 1;
        ck = new ChucK();
        ck->setParam(CHUCK_PARAM_SAMPLE_RATE,     static_cast<int>(_sr));
        ck->setParam(CHUCK_PARAM_INPUT_CHANNELS,  _in_ch);
        ck->setParam(CHUCK_PARAM_OUTPUT_CHANNELS, _out_ch);
        ck->setParam(CHUCK_PARAM_OTF_ENABLE,      0);   // no on-the-fly server (bare metal: no sockets)
        ck->setParam(CHUCK_PARAM_CHUGIN_ENABLE,   0);   // no dynamically-loaded UGen plugins (no dlopen)
        g_chuck_init_stage = 2;
        // init() builds the type system + registers built-in UGens; start() before any run() (globals()
        // is NULL until vm->running(), and a lazy auto-start from the ISR would race the main loop on the
        // pool). Single-threaded here (main loop, gate held by the caller). See docs/dev/chuck-pod-poc.md.
        // NB: no program compiled here - load_program() does that and caches the result.
        if (!ck->init() || !ck->start()) { delete ck; return nullptr; }
        return ck;
    } catch (const std::exception& e) {
        std::snprintf(g_chuck_init_error, sizeof(g_chuck_init_error), "%s: %s", typeid(e).name(), e.what());
    } catch (...) {
        std::strncpy(g_chuck_init_error, "(non-std exception)", sizeof(g_chuck_init_error) - 1);
    }
    if (ck) delete ck;                            // a throw mid-build -> reclaim the partial VM
    return nullptr;
#endif
}

bool ChuckEngine::compile_and_spork(const char* text)
{
    if (!_ck || !text) return false;
    try {
        // compileCode parses + type-checks + emits + sporks one instance (count=1, immediate=FALSE). A
        // syntax error returns FALSE (no throw) -> caller falls back. This is the ONLY path that compiles;
        // it retains a context ChucK never frees, so load_program() gates it behind the cache.
        g_chuck_init_stage = 3;
        if (!_ck->compileCode(text, "", 1)) return false;
        g_chuck_init_stage = 9;
        return true;
    } catch (const std::exception& e) {
        std::snprintf(g_chuck_init_error, sizeof(g_chuck_init_error), "%s: %s", typeid(e).name(), e.what());
    } catch (...) {
        std::strncpy(g_chuck_init_error, "(non-std exception)", sizeof(g_chuck_init_error) - 1);
    }
    return false;
}

Chuck_VM_Code** ChuckEngine::cache_cell(int slot)
{
    if (slot < 0)               return &_builtin_code;          // the built-in
    if (slot < kMaxChuckSlots)  return &_slot_code[slot];       // an SD slot
    return nullptr;
}

int ChuckEngine::load_program(int target)
{
    if (!_ck) return -1;
    if (target < 0 || target >= _avail_n) target = 0;
    const int slot = _avail_slot[target];        // -1 = built-in, else the SD slot number

    // 1) Cache hit -> re-spork the pinned bytecode. No recompile, no new context -> no leak. This is the
    //    steady-state path once each distinct patch has been compiled once.
    if (Chuck_VM_Code** cell = cache_cell(slot)) {
        if (*cell && _ck->vm() && _ck->vm()->spork(*cell, NULL, TRUE)) {
            _patch_loaded = (slot >= 0);
            return target;
        }
    }

    // 2) Cache miss -> read the program text, compile+spork once, then pin the emitted code. An SD read
    //    failure makes program_for return the built-in (from_sd=false) -> cache under the built-in cell.
    char* buf = (slot >= 0) ? static_cast<char*>(std::malloc(kPatchMax)) : nullptr;
    bool  from_sd = false;
    const char* text = program_for(target, buf, &from_sd);
    int result = -1;
    if (compile_and_spork(text)) {
        const int eff_slot = from_sd ? slot : -1;            // what actually loaded (built-in on SD fallback)
        if (Chuck_VM_Code** cell = cache_cell(eff_slot)) {
            Chuck_VM_Code* code = _ck->compiler() ? _ck->compiler()->output() : nullptr;
            if (code && !*cell) { code->add_ref(); *cell = code; }   // pin it for future swaps
        }
        _patch_loaded = from_sd;
        result = from_sd ? target : 0;
    }
    if (buf) std::free(buf);
    if (result >= 0) return result;

    // 3) An SD slot failed to compile: fall back to the built-in (cached or freshly compiled) so a bad
    //    patch never kills audio.
    if (slot >= 0) {
        if (_builtin_code && _ck->vm() && _ck->vm()->spork(_builtin_code, NULL, TRUE)) {
            _patch_loaded = false; return 0;
        }
        if (compile_and_spork(kProgram)) {
            Chuck_VM_Code* code = _ck->compiler() ? _ck->compiler()->output() : nullptr;
            if (code && !_builtin_code) { code->add_ref(); _builtin_code = code; }
            _patch_loaded = false; return 0;
        }
    }
    return -1;
}

const char* ChuckEngine::program_for(int avail_index, char* buf, bool* from_sd)
{
    if (avail_index < 0 || avail_index >= _avail_n) avail_index = 0;
    const int slot = _avail_slot[avail_index];
    if (slot < 0) { if (from_sd) *from_sd = false; return kProgram; }   // the built-in
    char path[24];
    chuck_path(slot, path, sizeof(path));
    return read_program(_stream, path, buf, buf ? kPatchMax : 0, kProgram, from_sd);
}

void ChuckEngine::rescan_bank()
{
    scan_chuck_patches(_stream, _present, kMaxChuckSlots);
    int n = 0;
    _avail_slot[n++] = -1;                       // index 0 is always the built-in
    for (int s = 0; s < kMaxChuckSlots; s++)
        if (_present[s] && n < kMaxAvail) _avail_slot[n++] = s;
    _avail_n = n;
    if (_sel >= _avail_n)         _sel = _avail_n - 1;       // keep the selection in range
    if (_sel_preview >= _avail_n) _sel_preview = _avail_n - 1;
}

void ChuckEngine::reseed_globals()
{
    // After a live recompile the new program's globals are at 0; replay the knob positions the platform
    // last sent so the patch picks up the current panel state instead of jumping on the next pot move.
    Chuck_Globals_Manager* g = _ck ? _ck->globals() : nullptr;
    if (!g) return;
    for (ParamId id : kMappedParams) {
        for (int di = 0; di < 2; di++) {
            const DeckRef::Ref d = di == 0 ? DeckRef::A : DeckRef::B;
            int slot = -1;
            const char* name = global_for(id, d, slot);
            if (name && slot >= 0 && slot < kSlots) g->setGlobalFloat(name, _cache[slot]);
        }
    }
}

void ChuckEngine::note_pool_usage()
{
    // Walk the pool for live bytes. Safe here ONLY because every caller invokes this with the audio path
    // quiesced (do_reload between gate take()/publish(); init() before the first publish()), so the ISR
    // is not allocating and the walk sees a stable heap. Never call this per-block or per-render.
    _pool_used = chuck_pool_used();
    if (_pool_cap == 0) _pool_cap = chuck_pool_capacity();
    if (_pool_used > _pool_used_peak) _pool_used_peak = _pool_used;
    g_chuck_pool_used_kb = static_cast<uint32_t>(_pool_used      >> 10);
    g_chuck_pool_peak_kb = static_cast<uint32_t>(_pool_used_peak >> 10);
    g_chuck_pool_cap_kb  = static_cast<uint32_t>(_pool_cap       >> 10);
}

void ChuckEngine::do_reload(int target)
{
    // Take the ONE persistent VM out of the audio path (the ISR sees null -> silence) for the swap. The VM
    // itself is NOT destroyed - destroying it would re-leak ChucK's built-in type system (its global
    // namespace is never freed; see build_vm). _ck stays valid throughout (set_param null-guards anyway).
    _gate.take();

    if (_ck) {
        // Stop the current patch synchronously. removeAllShreds() only flags the removal (enacted at the
        // top of the next compute() tick), and free_shred() detaches the shred's UGens from dac. So flush
        // it with a 1-frame run() NOW, BEFORE sporking the new patch - otherwise that deferred remove-all
        // would also kill the shred load_program() is about to spork (and old dac-connected UGens would
        // keep ticking = a CPU leak). One frame is enough: compute() checks the flag before running shreds.
        _ck->removeAllShreds();
        if (_inbuf && _outbuf) _ck->run(_inbuf, _outbuf, 1);

        // Load the target: re-spork its cached bytecode (no recompile -> no leak) or compile+cache it once.
        const int eff = load_program(target);
        if (eff >= 0) _sel = eff;
    }

    reseed_globals();                                     // replay knob state into the (re)sporked program
    _panic = false;                                       // fresh chance: clear any prior overrun-mute
    _overrun_n = 0;
    note_pool_usage();                                    // leak probe: sample now, ISR still gated off
    g_chuck_swaps++;
    _gate.publish(_ck);                                   // re-arm the ISR with the (same) VM
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------
void ChuckEngine::init(const EngineContext& ctx)
{
    _sr     = ctx.sample_rate;
    _block  = ctx.block_size;
    _stream = ctx.stream;                          // SD service for the patch bank (null on a card-less Pod)
    if (_block > kMaxBlock) _block = kMaxBlock;   // run() scratch is sized for kMaxBlock frames

    // Route ChucK's allocations (the VM/UGen graph, MBs at create/compile) to the SDRAM pool in
    // chuck_alloc.cpp: the platform's default heap stays in SRAM (global ctors malloc before
    // _hw.Init() powers the FMC, so a heap in SDRAM would fault there). Arm AFTER _hw.Init() (we are
    // past it) and before `new ChucK()`. ctx.arena is unused - ChucK's heap comes from this pool.
    chuck_heap_arm();

    // Interleaved run() scratch, from the SDRAM pool (the platform heap stays in SRAM). Allocated once
    // here, never freed (the engine lives for the whole session).
    _inbuf  = static_cast<float*>(std::malloc(sizeof(float) * kMaxBlock * _in_ch));
    _outbuf = static_cast<float*>(std::malloc(sizeof(float) * kMaxBlock * _out_ch));

    // Enable the DWT cycle counter for the CPU-overrun safeguard (idempotent; the RNG seed + the meter
    // also use it). Seed the run() budget with an estimate at 480 MHz (the Daisy H750 boosted clock);
    // process() refines _block_cycles to the true block period at runtime, so the estimate need not be
    // exact - it only has to be in the right ballpark before the first clean block is measured.
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
    _block_cycles = static_cast<uint32_t>(480000000.0f * _block / _sr);

    // Boot: create the one persistent VM, then load the boot program into it. The card usually mounts
    // ~1 s after this init(), so this typically loads the built-in and prepare()'s boot auto-load swaps
    // in the first SD slot once it appears. build_vm() carries the try/catch + g_chuck_init_* bring-up
    // capture; on failure (incl. NOCHUCK) it returns nullptr -> boot silent. load_program() compiles +
    // caches the boot program (and falls back to the built-in if the chosen SD slot won't compile).
    rescan_bank();
    _sel = (_avail_n > 1) ? 1 : 0;               // first SD patch if any, else the built-in
    _sel_preview = _sel;

    _ck = build_vm();
    if (_ck) {
        const int eff = load_program(_sel);       // compiles + caches + sporks the boot program
        if (eff >= 0) {                           // publishing the VM is what makes run() live in the ISR
            _sel = eff;
            note_pool_usage();                    // baseline (swap 0): one VM + one program, ISR not yet live
            _gate.publish(_ck);                   // boot: _cache all 0, so no reseed needed
        }
    }
}

void ChuckEngine::prepare()
{
    // Main-loop housekeeping (off the audio ISR). The heavy patch recompile lives here, gated against
    // the ISR by the ReloadGate. (The MIDI drain, M4, will join it.)
    //
    // Boot auto-load: the SD card mounts ~1 s after power-up (Storage's state machine), after this
    // engine's init() has already booted the built-in. So keep rescanning (throttled - rescan_bank is
    // f_stat I/O) until a slot appears, then load the first SD patch once. After that the user drives
    // patch choice with the Alt selector.
    if (_stream && !_boot_loaded && (_probe_div++ & 0x1FF) == 0) {
        rescan_bank();
        if (_avail_n > 1) {                          // a slot appeared (built-in is index 0)
            _boot_loaded = true;
            _reload_target = 1;                      // the first present SD slot
            _reload_pending = true;
        }
    }

    // While the Alt selector is open, keep rescanning if the bank is still empty so slots appear as
    // soon as the card mounts. Stops once any slot is found.
    if (_aux_held && _avail_n <= 1) _rescan_pending = true;
    if (_rescan_pending) { _rescan_pending = false; rescan_bank(); }
    if (_reload_pending) { _reload_pending = false; do_reload(_reload_target); }
}

void ChuckEngine::process(const float* const* in, float** out, size_t size)
{
    const uint32_t entry = DWT->CYCCNT;           // block-period + overrun timing (CPU-overrun safeguard)
    ChucK* ck = static_cast<ChucK*>(_gate.begin_use());
    // Silence when: not ready (boot fail / mid-reload / alloc fail), OR _panic (this patch was muted for
    // overrunning real-time - skipping run() frees the CPU so the main loop + selector stay alive).
    if (!ck || !_inbuf || !_outbuf || _panic) {
        for (size_t i = 0; i < size; i++) { out[0][i] = 0.f; out[1][i] = 0.f; }
        _peak_l *= 0.90f;                         // let the meters decay while silent
        _peak_r *= 0.90f;
        _rms_l  *= 0.90f;
        _rms_r  *= 0.90f;
        _last_entry = entry;
        _gate.end_use();
        return;
    }

    const size_t n = (size < static_cast<size_t>(kMaxBlock)) ? size : static_cast<size_t>(kMaxBlock);

    // De-interleaved platform in -> ChucK's interleaved input buffer (numFrames * _in_ch).
    const float* il = in ? in[0] : nullptr;
    const float* ir = in ? in[1] : nullptr;
    for (size_t i = 0; i < n; i++) {
        _inbuf[i * 2]     = il ? il[i] : 0.f;
        _inbuf[i * 2 + 1] = ir ? ir[i] : 0.f;
    }

    // Deliver this block's MIDI to the re-introduced ChucK MidiIn device (virtual UART = device 0) here,
    // on the run() thread, right before run() - so a shred blocked on `min => now` is woken by this run()
    // (no ChucK threads on this build; the wake rides the per-VM event buffer that ck->run() services).
    // The full raw stream is forwarded (real velocity, NoteOff, CC, pitch-bend, aftertouch, program
    // change, system realtime), so a patch using `MidiIn min; min.recv(msg);` reads data1/data2/data3
    // exactly as on the desktop. See docs/dev/chuck-midi-in-porting.md.
    {
        MidiMessage m;
        while (_midi.pop(m)) {
            MidiInManager::inject(0, static_cast<t_CKBYTE>(m.status),
                                  static_cast<t_CKBYTE>(m.data1), static_cast<t_CKBYTE>(m.data2));
        }
    }

    // One VM compute call: consumes _inbuf, advances every shred sample-accurately across n frames,
    // fills _outbuf interleaved (numFrames * _out_ch). SAMPLE == float, so no double marshalling.
    const uint32_t run0 = DWT->CYCCNT;
    ck->run(_inbuf, _outbuf, static_cast<long>(n));
    const uint32_t run_cycles = DWT->CYCCNT - run0;

    // Per-channel output metering for render() - peak (max abs) and RMS (sum-of-squares -> sqrt), L and
    // R separately, the same quantities numchuck's get_audio_meters exposes (here shown on the rings).
    float pl = 0.f, pr = 0.f, sl = 0.f, sr = 0.f;
    for (size_t i = 0; i < n; i++) {              // interleaved _outbuf -> de-interleaved out
        const float l = _outbuf[i * 2];
        const float r = _outbuf[i * 2 + 1];
        out[0][i] = l;
        out[1][i] = r;
        const float a = (l < 0 ? -l : l), b = (r < 0 ? -r : r);
        if (a > pl) pl = a;
        if (b > pr) pr = b;
        sl += l * l;
        sr += r * r;
    }
    const float inv = (n > 0) ? 1.f / static_cast<float>(n) : 0.f;
    const float rl = std::sqrt(sl * inv);
    const float rr = std::sqrt(sr * inv);
    // Single-float writes, read in the main loop (render). Peak: fast attack, slow decay. RMS: smoothed.
    _peak_l = (pl > _peak_l) ? pl : _peak_l * 0.90f;
    _peak_r = (pr > _peak_r) ? pr : _peak_r * 0.90f;
    _rms_l += 0.25f * (rl - _rms_l);
    _rms_r += 0.25f * (rr - _rms_r);

    // CPU-overrun safeguard. Learn the true block period (the minimum inter-block gap: under overrun the
    // gap inflates, so the min converges to the real period and corrects the 480 MHz estimate). If run()
    // exceeded a whole block period for kOverrunBlocks consecutive blocks, the patch can't keep up -
    // latch _panic so the next block onward outputs silence and returns the CPU to the main loop.
    if (_last_entry) {
        const uint32_t period = entry - _last_entry;
        if (period > 0 && period < _block_cycles) _block_cycles = period;
    }
    if (run_cycles > _block_cycles) { if (++_overrun_n >= kOverrunBlocks) _panic = true; }
    else                            { _overrun_n = 0; }
    _last_entry = entry;

    _gate.end_use();
}

Capabilities ChuckEngine::capabilities() const
{
    // CapAux: claim Alt+PITCH as the patch selector (ParamId::Aux). CapOwnDisplay: the engine draws
    // the rings (level meter, and the selector while Alt is held).
    return CapAux | CapOwnDisplay;
}

void ChuckEngine::set_param(ParamId id, DeckRef::Ref d, float v)
{
    if (id == ParamId::Aux) {                      // Alt+PITCH: preview a patch (deck A drives selection)
        if (d == DeckRef::A) _sel_preview = aux_to_index(v, _avail_n);
        return;                                    // committed on release (set_aux_active), in prepare()
    }

    int slot = -1;
    const char* name = global_for(id, d, slot);
    if (slot >= 0 && slot < kSlots) _cache[slot] = v;   // cache even while mid-reload (for reseed)
    if (!_ck || !name) return;
    // Called from the main loop while process() runs in the audio ISR. With __DISABLE_THREADS__ the
    // globals queue is drained on the audio thread, so this enqueues a write that the next run()
    // applies - the intended host -> .ck-program path (mirrors Csound's setControlChannel). globals()
    // is NULL until the VM is running; init() now start()s it, but null-guard anyway (a set_param
    // before init() must not deref NULL).
    Chuck_Globals_Manager* g = _ck->globals();
    if (g) g->setGlobalFloat(name, v);
}

float ChuckEngine::param(ParamId id, DeckRef::Ref d) const
{
    if (id == ParamId::Aux)                        // pickup readback: the committed selection's position
        return (_avail_n <= 1) ? 0.f : (static_cast<float>(_sel) + 0.5f) / static_cast<float>(_avail_n);
    int slot = -1;
    global_for(id, d, slot);
    return (slot >= 0 && slot < kSlots) ? _cache[slot] : 0.f;
}

void ChuckEngine::set_aux_active(DeckRef::Ref d, bool held)
{
    if (d != DeckRef::A) return;                   // deck A's Alt+PITCH drives the global patch selection
    if (held && !_aux_held) {                      // press: refresh the bank, start preview at the live one
        _rescan_pending = true;
        _sel_preview = _sel;
    }
    if (!held && _aux_held) {                       // release: commit if the choice changed
        if (_sel_preview != _sel) { _reload_target = _sel_preview; _reload_pending = true; }
    }
    _aux_held = held;
}

DeckRef::Ref ChuckEngine::handle_midi_note(uint8_t channel, uint8_t note)
{
    // ChucK gets the note via handle_midi_message (the full stream); this override exists ONLY to map
    // channel -> deck so the UI can flash the right gate-in LED on a NoteOn. It does NOT enqueue (that
    // would double-deliver the note). Note (unused here) kept for the IEngine signature.
    (void)note;
    if (!_gate.current()) return DeckRef::Count;   // no VM yet -> not playable; tell the UI not to flash

    const Config& c = Config::dynamic();           // configured MIDI channels (same map as CsoundEngine)
    if      (channel == c.midi_channel_a()) return DeckRef::A;
    else if (channel == c.midi_channel_b()) return DeckRef::B;
    return DeckRef::Count;
}

void ChuckEngine::handle_midi_message(uint8_t status, uint8_t data1, uint8_t data2)
{
    // Main loop: only enqueue the raw message. process() (the audio ISR) drains the ring and injects into
    // the ChucK MidiIn device (device 0) right before run(), so the buffer fill + shred wake happen on the
    // run() thread. No channel filtering - a patch sees the whole stream and routes by status/channel
    // itself (the desktop MidiIn contract). A full ring drops the newest message.
    if (!_gate.current()) return;                  // no VM yet -> nowhere to deliver
    _midi.push({ status, data1, data2 });
}

void ChuckEngine::render(DisplayModel& m)
{
    m.clear();
    const bool running = (_gate.current() != nullptr);

    // While Alt is held: the patch selector - one dot per selectable program around each ring, the
    // previewed one bright. (The built-in is index 0; SD slots follow.) Same look as CsoundEngine.
    if (_aux_held) {
        for (int i = 0; i < 2; i++) {
            m.play[i] = { pal::kGreen, running ? 1.f : 0.f };
            ring::selector(m.ring[i], _avail_n, _sel_preview, pal::kCyan, 0.18f, pal::kCyan);
            m.ring[i].set_updated();
        }
        m.mode_center = { pal::kCyan, 0.6f };           // selector hue
        return;
    }

    // CPU-overrun mute: this patch overran real-time and was silenced to keep the controls alive. Solid
    // red rings + red centre so the user knows to Alt+PITCH to another patch (which clears the panic).
    // After the selector branch above, so the selector still draws/works as the escape route.
    if (_panic) {
        for (int i = 0; i < 2; i++) {
            m.play[i] = { pal::kRed, 1.f };
            ring::level(m.ring[i], 0.999f, pal::kRed);   // solid red ring
            m.ring[i].set_updated();
        }
        m.mode_center = { pal::kRed, 0.8f };
        return;
    }

#ifdef METER
    // On-panel CPU + pool meter (build: make engine-chuck METER=1) - the patch-swap leak readout for the
    // cased unit (no SWD access). Ring A = CPU load (the overrun symptom). Ring B = SDRAM pool usage:
    // arc = bytes live right after the last swap, dot = the high-water mark. Swap patches repeatedly and
    // watch ring B: arc + dot climbing together and never recovering = the ChucK per-VM type-system leak;
    // arc falling back while the dot stays high = used recovered (fragmentation, failure is elsewhere).
    // The same METER flag also streams CPU load over USB CDC (app.cpp). See docs/dev/chuck-impl.md.
    {
        float avg = Meter::cpu().load.GetAvgCpuLoad();
        float pk  = Meter::cpu().load.GetMaxCpuLoad();
        if (avg < 0.f) avg = 0.f;
        if (avg > 1.f) avg = 1.f;
        if (pk  < 0.f) pk  = 0.f;
        if (pk  > 1.f) pk  = 1.f;
        const uint32_t col = (pk > 0.85f) ? 0xff2000u : (pk > 0.60f) ? 0xffa000u : 0x00ff00u;

        // Ring A (deck A) = CPU load. Faint base ring = the full block budget; the bright arc = current
        // average load; the dot = the peak (max) load; the colour = peak severity (green/amber/red,
        // red >= 85% = at/over budget -> dropouts; max latches, so one overrun stays visible).
        m.play[0] = { running ? 0x00ff00u : 0x000000u, running ? 1.f : 0.f };
        m.ring[0].set_hex_color(0x0a0a0a);
        m.ring[0].set_segment(0.f, 0.999f);
        if (avg > 0.01f) { m.ring[0].set_hex_color(col); m.ring[0].set_segment(0.f, avg); }
        m.ring[0].add_point(pk, 1.f);
        m.ring[0].set_updated();

        // Ring B (deck B) = SDRAM pool usage as a fraction of capacity. Bright arc = bytes live right
        // after the last swap (_pool_used); dot = the high-water mark (_pool_used_peak). Both are sampled
        // once per swap by note_pool_usage() with the ISR gated - NOT walked here per render (that walk-
        // under-PRIMASK was the old observer-effect bug). Colour by fill severity. A leak drives arc + dot
        // up together every swap and they never fall; fragmentation drops the arc but leaves the dot high.
        const float cap   = (_pool_cap > 0) ? static_cast<float>(_pool_cap) : 1.f;
        float used_frac = static_cast<float>(_pool_used)      / cap;
        float peak_frac = static_cast<float>(_pool_used_peak) / cap;
        if (used_frac > 1.f) used_frac = 1.f;
        if (peak_frac > 1.f) peak_frac = 1.f;
        const uint32_t pcol = (used_frac > 0.85f) ? 0xff2000u : (used_frac > 0.60f) ? 0xffa000u : 0x00ff00u;
        m.play[1] = { running ? 0x00ff00u : 0x000000u, running ? 1.f : 0.f };
        m.ring[1].set_hex_color(0x0a0a0a);                 // faint base = full pool capacity (the scale)
        m.ring[1].set_segment(0.f, 0.999f);
        if (used_frac > 0.001f) { m.ring[1].set_hex_color(pcol); m.ring[1].set_segment(0.f, used_frac); }
        m.ring[1].add_point(peak_frac, 1.f);               // high-water dot
        m.ring[1].set_updated();

        m.mode_center = { 0xff00ffu, 0.6f };               // magenta = meter mode (vs the source LED)
        return;
    }
#endif

    // Per-channel output level meter: ring A = left, ring B = right. The bright arc is RMS (loudness),
    // a dot marks the peak; colour by peak severity (green base, amber past ~-4 dB, red near clip). This
    // is numchuck's per-channel RMS+peak metering, drawn on the two rings instead of returned as values.
    const float rms[2]  = { _rms_l,  _rms_r  };
    const float peak[2] = { _peak_l, _peak_r };
    for (int i = 0; i < 2; i++) {
        const float a = meter_db_norm(rms[i]);         // RMS  -> dB ring position (loudness arc)
        const float p = meter_db_norm(peak[i]);        // peak -> dB ring position (marker)
        // Colour by peak on the dB scale: red near 0 dBFS (>= -3 dB), amber from ~-12 dB, else green.
        const uint32_t col = (p > 0.95f) ? 0xff2000u : (p > 0.80f) ? pal::kAmber : pal::kGreen;
        m.play[i] = { pal::kGreen, running ? 1.f : 0.f };
        m.ring[i].set_hex_color(0x0a0a0a);              // faint full base ring
        m.ring[i].set_segment(0.f, 0.999f);
        if (running && a > 0.001f) ring::level(m.ring[i], a, col);   // bright arc proportional to RMS (dB)
        if (running && p > 0.001f) ring::playhead(m.ring[i], p, 1.f);   // peak marker (dB)
        m.ring[i].set_updated();
    }
    // Centre mode LED tells the program source: cyan = an SD slot, white = the built-in.
    m.mode_center = { _patch_loaded ? pal::kCyan : pal::kWhite, 0.5f };
}

} // namespace spotykach
