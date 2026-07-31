// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)
#include <cstddef>   // size_t (transitive-include hygiene; host build)

#include "engine/iengine.h"
#include "engine/chuck/chuck_patch.h"        // kMaxChuckSlots (bank size) + the SD .ck selector
#include "engine/csound/csound_reload.h"     // ReloadGate: lock-free VM<->ISR handoff for live swap
#include "engine/chuck/chuck_midi_in.h"      // MidiMessage / MidiMsgQueue: lock-free raw-MIDI ring (MIDI-in)

// SKETCH (2026-06, M2): ChuckEngine wraps a ChucK runtime (the language/VM/UGens) behind IEngine,
// modelled directly on CsoundEngine. Like Csound, it builds ONLY in the QSPI firmware target - the
// ChucK core links ~1.1 MB of .text (far over the 186 KB SRAM_EXEC budget), so code lives in QSPI
// flash and the VM heap in SDRAM (alt_qspi.lds + the chuck_alloc.cpp --wrap pool); it cannot link
// into the SRAM engine bundle. See docs/dev/chuck-impl.md for the roadmap and the build recipe.
//
// The mapping onto IEngine is small and clean (the same shape as Csound):
//   init()      -> new ChucK / setParam(SR/INPUT/OUTPUT_CHANNELS) / init() / compileCode(program)
//   process()   -> interleave in -> ck->run(in,out,n) (SAMPLE=float) -> de-interleave out
//   set_param() -> globals()->setGlobalFloat(name, value)   (the .ck program reads the global)
//   render()    -> output-level meter on the rings
// The ChucK program (the .ck text) defines the synthesis AND the control vocabulary; the platform's
// knobs drive whichever globals the program declares (speedA/mixA/sizeA/... - see channel_for()).
//
// SCOPE: M3 adds the SD `.ck` patch bank + Alt+PITCH live swap (chuck_patch.h + the ReloadGate). The
// built-in kProgram is slot 0 of the bank and the always-available fallback. MIDI-in (M4) is the next
// step (mirrors how CsoundEngine grew). ChucK's live swap differs from Csound's: there is ONE
// persistent VM (`_ck`) - a reload is removeAllShreds() + compileCode(newText) on it, not a new
// instance. The ReloadGate still gates the VM out of the audio path during that mutation, because
// removeAllShreds/compileCode are processed inside run() and (under __DISABLE_THREADS__) the message
// queues are not lock-protected against a concurrent run(). See chuck_engine.cpp do_reload.

// ChucK's host class + compiled-code object, forward-declared so the contract pulls in no ChucK headers
// (chuck.h is included only in the .cpp, which exists only in the QSPI build). Chuck_VM_Code is the
// emitted bytecode for one compiled program; the engine pins (add-refs) one per distinct patch so swaps
// re-spork it instead of recompiling (the compile-once cache - see do_reload / load_program).
class ChucK;
class Chuck_VM_Code;

namespace spotykach {

class ChuckEngine : public IEngine {
public:
    // --- required audio lifecycle ---------------------------------------------------------------
    void init(const EngineContext& ctx) override;
    void prepare() override;
    void process(const float* const* in, float** out, size_t size) override;

    // --- control --------------------------------------------------------------------------------
    Capabilities capabilities() const override;
    void         set_param(ParamId id, DeckRef::Ref d, float v) override;
    float        param(ParamId id, DeckRef::Ref d) const override;

    // Alt+PITCH (CapAux) held on deck A: the patch selector. set_param(Aux) previews; this drives the
    // selector display + (on the held->release edge) commits a live recompile in prepare().
    void         set_aux_active(DeckRef::Ref d, bool active) override;

    // MIDI -> the re-introduced ChucK MidiIn device. handle_midi_message() carries the FULL channel-voice
    // stream (real velocity, NoteOff, CC, pitch-bend, aftertouch, program change) plus system realtime;
    // it enqueues raw bytes on the lock-free ring here (main loop), drained + injected into the VM in
    // process() (audio ISR) right before run(), where a shred blocked on `min => now` is woken. So a patch
    // uses `MidiIn min; min.recv(msg);` exactly as on the desktop. handle_midi_note() is kept ONLY to map
    // channel->deck for the panel gate-in LED (it no longer feeds the VM). See chuck_engine.cpp and
    // docs/dev/chuck-midi-in-porting.md.
    DeckRef::Ref handle_midi_note(uint8_t channel, uint8_t note) override;
    void         handle_midi_message(uint8_t status, uint8_t data1, uint8_t data2) override;

    // --- panel feedback -------------------------------------------------------------------------
    // Rings show the output level meter (or, while Alt is held, the patch selector); Play LEDs show
    // running state; the centre mode LED shows the program source (cyan = an SD slot, white = built-in).
    void render(DisplayModel& m) override;

private:
    // Build the ONE persistent ChucK VM: new ChucK / setParam / init / start - NO program compiled yet
    // (load_program does that). Returns the started instance, or nullptr on failure (throw - partial VM
    // deleted, or NOCHUCK). Created once at init() and kept for the whole session: the VM (hence its
    // built-in type system) is NEVER destroyed, because ChucK never frees its global namespace on teardown
    // (chuck_type.cpp Chuck_Env::cleanup TODO) - so destroy+recreate leaked the whole type system per swap.
    ChucK* build_vm();
    // Compile `text` into the live VM and spork one instance (the host ChucK::compileCode path, wrapped in
    // the g_chuck_init_* bring-up capture). Returns false on compile error / throw. Each call retains one
    // compilation context in the type system (ChucK does not free it) - so load_program calls this AT MOST
    // ONCE per distinct patch and caches the result; thereafter swaps re-spork the cached code.
    bool compile_and_spork(const char* text);
    // The compiled-code cache cell for a program slot (-1 = the built-in, else an SD slot number), or
    // nullptr if out of range. Caching is keyed by slot (not availability index) so it survives rescans.
    Chuck_VM_Code** cache_cell(int slot);
    // Load the program for availability index `target` into the live VM: re-spork its cached Chuck_VM_Code
    // if present (no recompile -> no leak), else read + compile_and_spork + cache. Falls back to the
    // built-in when an SD slot can't be read/compiled. Sets _patch_loaded. Returns the availability index
    // actually loaded (target, or 0 if it fell back to the built-in), or -1 if even the built-in failed.
    int load_program(int target);
    // Program text for an availability index (built-in or an SD slot). SD reads go into `buf`.
    const char* program_for(int avail_index, char* buf, bool* from_sd);
    // Re-probe /chuck/<n>.ck existence and rebuild the [built-in, present slots...] selectable list.
    void rescan_bank();
    // Gated swap for availability index `target` (main loop only): take the VM out of the audio path,
    // synchronously remove the current shreds (removeAllShreds + a 1-frame run() to flush the deferred
    // removal), load_program(target) (re-spork cached or compile+cache), reseed, republish. ONE VM is
    // kept alive across swaps - only the shreds change - so the type system is never re-leaked.
    void do_reload(int target);
    // Replay cached knob values into the freshly compiled program's globals (post-reload pickup).
    void reseed_globals();
    // Sample live SDRAM pool usage into _pool_used/_pool_used_peak + the g_chuck_pool_* SWD globals.
    // Call ONLY with the audio path quiesced (ISR not allocating) - i.e. between the ReloadGate take()
    // and publish() in do_reload(), or before the init() publish. The patch-swap leak probe: a value
    // that climbs monotonically across swaps is the ChucK per-VM type-system leak (chuck-impl.md).
    void note_pool_usage();

    ReloadGate   _gate;             // owns the live VM pointer (handoff: main loop <-> audio ISR)
    ChucK* _ck    = nullptr;        // the persistent ChucK VM (one instance for the session)
    IStreamDeck* _stream = nullptr; // SD service, kept for bank rescans / patch reloads
    float  _sr    = 48000.f;
    float  _block = 256.f;          // platform block size; == run() numFrames per process() call
    int    _in_ch  = 2;
    int    _out_ch = 2;
    float  _peak_l = 0.f, _peak_r = 0.f;  // per-channel output peak (fast attack, slow decay)
    float  _rms_l  = 0.f, _rms_r  = 0.f;  // per-channel output RMS (smoothed) - the loudness meter
    bool   _patch_loaded = false;   // true => running an SD slot; false => the built-in

    // --- compile-once cache (the leak fix) -------------------------------------------------------
    // One add-ref'd Chuck_VM_Code per distinct program, so a swap re-sporks cached bytecode instead of
    // recompiling (each compile retains a context ChucK never frees). Keyed by slot: [built-in] + the SD
    // slots. Session-lifetime (never evicted), so total compiles - hence total leaked contexts - are
    // bounded by the number of distinct patches (<= kMaxChuckSlots + 1), NOT the number of swaps.
    // CAVEAT: code is cached across SD rescans, so editing a slot's .ck file mid-session won't take effect
    // until power-cycle; and because all patches share one user namespace (no per-swap reset), two patches
    // defining the same top-level type name would collide on the second's compile. Fine for the bank model.
    Chuck_VM_Code* _builtin_code = nullptr;
    Chuck_VM_Code* _slot_code[kMaxChuckSlots] = {nullptr};

    // --- patch-swap leak instrumentation (always built; the panel readout is METER-gated) ----------
    // Live SDRAM pool usage, sampled once per swap by note_pool_usage() with the ISR quiesced (so the
    // used_bytes() walk is race-free and off the hot path). render() (METER) draws _pool_used as ring B's
    // arc and _pool_used_peak as a high-water dot: climbing together across swaps = a leak; arc falling
    // back while the dot stays high = fragmentation (used recovered, the failure is elsewhere). Also
    // mirrored to g_chuck_pool_* for SWD readout on the bare Pod.
    size_t _pool_used      = 0;
    size_t _pool_used_peak = 0;
    size_t _pool_cap       = 0;

    // --- CPU-overrun safeguard (mute-and-wait) --------------------------------------------------
    // A patch whose ck->run() can't finish within the audio block would let the (highest-priority) audio
    // ISR saturate the CPU and starve the main loop, freezing the knobs + Alt+PITCH selector with no way
    // out. process() times run() via the DWT cycle counter; if it overruns the block for kOverrunBlocks
    // consecutive blocks, _panic latches: process() then outputs silence (skips run()), which frees the
    // CPU so the main loop + selector stay alive and the user can Alt+PITCH to another patch. The panic
    // is mute-and-wait (no auto-fallback to the built-in); do_reload() clears it on the next swap.
    bool     _panic       = false;  // latched: current patch muted for overrunning real-time
    uint32_t _overrun_n   = 0;      // consecutive overrunning blocks
    uint32_t _block_cycles = 0;     // run() budget in CPU cycles (est. at init, refined to the true
                                    // block period at runtime via the min inter-block gap)
    uint32_t _last_entry  = 0;      // DWT->CYCCNT at the previous process() entry
    static constexpr uint32_t kOverrunBlocks = 16;  // ~85 ms of sustained overrun before muting

    // Interleaved scratch buffers for ck->run(): ChucK exchanges interleaved float[numFrames*chans].
    // Allocated from the SDRAM pool at init() (NOT held in the engine object), so they cost the
    // SRAM-tight platform nothing - the same reason CsoundEngine keeps its spin/spout in libcsound's
    // SDRAM heap rather than in SRAM. Sized for the max block (256) x 2 channels; process() clamps.
    static constexpr int kMaxBlock = 256;
    float* _inbuf  = nullptr;   // [kMaxBlock * _in_ch], from the SDRAM pool
    float* _outbuf = nullptr;   // [kMaxBlock * _out_ch], from the SDRAM pool

    // --- patch bank / Alt+PITCH selection -------------------------------------------------------
    static constexpr int kMaxAvail = kMaxChuckSlots + 1;   // the built-in plus every present slot
    bool _present[kMaxChuckSlots] = {false};
    int  _avail_slot[kMaxAvail] = {-1};   // [0] = -1 (built-in); then the present slot numbers
    int  _avail_n   = 1;                  // count of selectable programs (>= 1: the built-in)
    int  _sel       = 0;                  // committed availability index (what's compiled)
    int  _sel_preview = 0;                // Alt+PITCH preview index (shown while held)
    bool _aux_held  = false;              // deck A Alt held -> draw the selector
    bool _rescan_pending = false;         // refresh the bank on the next prepare() (set on Alt press)
    bool _reload_pending = false;         // commit a patch change on the next prepare()
    int  _reload_target  = 0;             // availability index to compile on the pending reload

    // Boot auto-load of the first SD patch once the card mounts (~1 s after boot), independent of the
    // Alt selector. One-shot; the throttle keeps the rescan off every main-loop iteration.
    bool     _boot_loaded = false;
    uint16_t _probe_div   = 0;

    // Cached 0..1 knob values for param() pickup readback + post-reload reseed, per (ParamId, deck).
    static constexpr int kSlots = 16;     // 8 per deck
    float _cache[kSlots] = {0.f};

    // Pending raw MIDI messages: handle_midi_message() (main loop) pushes, process() (audio ISR) drains
    // and injects into the ChucK MidiIn device right before run(). Sized for a dense block (CC sweeps +
    // notes); a full ring drops the newest message.
    MidiMsgQueue<64> _midi;
};

} // namespace spotykach
