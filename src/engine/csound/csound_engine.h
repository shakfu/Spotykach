// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)
#include <cstddef>   // size_t (transitive-include hygiene; host build)

#include "engine/iengine.h"
#include "engine/csound/csound_midi.h"     // MIDI note->event mapping + the lock-free pending-note ring
#include "engine/csound/csound_reload.h"   // ReloadGate: lock-free instance handoff for live patch swap
#include "engine/csound/csound_patch.h"    // kMaxPatchSlots (bank size)

// SKETCH (2026-06): CsoundEngine wraps a Csound 7 instance behind IEngine. It builds ONLY in the
// QSPI firmware target - code in QSPI flash, heap in SDRAM (the Csound port's custom linker script),
// stock Daisy v5.4 bootloader, linked against libcsound.a. It CANNOT link into the SRAM engine
// bundle: ~2 MB of code vs the 186 KB SRAM_EXEC budget, and the spotykach board only boots SRAM.
// See docs/dev/csound-impl.md for the why and the build recipe.
//
// The mapping onto IEngine is small and clean:
//   init()      -> csoundCreate / SetHostAudioIO / options / CompileCSD / Start
//   process()   -> de-interleave in -> spin, csoundPerformKsmps, spout -> de-interleave out
//   set_param() -> csoundSetControlChannel(name, value)   (orchestra reads it with chnget)
//   handle_midi_note() -> a finite Csound note event (csound_midi.h)
//   Alt+PITCH (CapAux) -> select a patch from the SD bank; release recompiles live (csound_reload.h)
// The orchestra (the .csd) defines the synthesis AND the control vocabulary; the platform's knobs
// drive whichever channels the orchestra names.

// Csound's opaque instance type, forward-declared so the contract pulls in no Csound headers
// (csound.h is included only in the .cpp, which exists only in the QSPI build).
typedef struct CSOUND_ CSOUND;

namespace spotykach {

class CsoundEngine : public IEngine {
public:
    // --- required audio lifecycle ---------------------------------------------------------------
    void init(const EngineContext& ctx) override;
    void prepare() override;
    void process(const float* const* in, float** out, size_t size) override;

    // --- control --------------------------------------------------------------------------------
    Capabilities capabilities() const override;

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md). Without them the descriptor would
    // advertise all 24 ids and a host sweep would "pass" on ids that name no Csound channel - set_param
    // returns early with `chan == nullptr`, so nothing is written and nothing is asserted.
    //
    // These are exactly the ids channel_for() maps (csound_engine.cpp:78-90): PITCH/MIX/SIZE/ENV/FB/MODAMP, which a patch
    // reads by name, plus AUX (Alt+PITCH patch selection), which set_param handles ahead of the channel
    // lookup. ModSpeed IS mapped (`modspA/B`) but stays out: it is platform-owned and reaches the engine
    // through set_mod_speed, and `describe` filters it regardless.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Env))
             | (1u << static_cast<uint32_t>(ParamId::Feedback))
             | (1u << static_cast<uint32_t>(ParamId::ModAmp))
             | (1u << static_cast<uint32_t>(ParamId::Aux));
    }
    // This engine implements no set_config - the patch, not a panel switch, decides its topology.
    ConfigMask live_configs() const override { return static_cast<ConfigMask>(0); }
#endif
    void         set_param(ParamId id, DeckRef::Ref d, float v) override;
    float        param(ParamId id, DeckRef::Ref d) const override;

    // MIDI NoteOn -> a Csound note event (channel->deck, note->Hz), enqueued on the lock-free ring and
    // scheduled in process(). NoteOn only - no NoteOff/velocity, so notes are finite (csound_midi.h).
    DeckRef::Ref handle_midi_note(uint8_t channel, uint8_t note) override;

    // Alt+PITCH (CapAux) held on deck A: the patch selector. set_param(Aux) previews; this drives the
    // selector display + (on the held->release edge) commits a live recompile in prepare().
    void set_aux_active(DeckRef::Ref d, bool active) override;

    // --- panel feedback -------------------------------------------------------------------------
    // Rings show the output level meter (or, while Alt is held, the patch selector); Play LEDs show
    // running state; the centre mode LED shows the patch source (cyan = SD, white = built-in).
    void render(DisplayModel& m) override;

private:
    // Create + configure + compile `text` (a CSD) + start; returns the new instance (caller publishes
    // it via the gate) or nullptr on failure. Does NOT touch _gate/_ksmps/_midi_instr.
    CSOUND* build_instance(const char* text, float block_size);
    // Make `cs` the live instance: set _ksmps/_midi_instr, then publish to the gate (ordered so the
    // ISR sees the new ksmps/instr together with the new pointer).
    void    publish_instance(CSOUND* cs);
    // Orchestra text for an availability index (built-in or an SD slot). SD reads go into `buf`.
    const char* orchestra_for(int avail_index, char* buf, bool* from_sd);
    // Re-probe /csound/<n>.csd existence and rebuild the [built-in, present slots...] selectable list.
    void    rescan_bank();
    // Gated destroy-old + build-new + publish, for availability index `target` (main loop only).
    void    do_reload(int target);
    // Replay cached knob values into a freshly built instance's control channels (post-reload pickup).
    void    reseed_channels(CSOUND* cs);

    ReloadGate   _gate;                 // owns the live CSOUND* (handoff: main loop <-> audio ISR)
    IStreamDeck* _stream = nullptr;     // SD service, kept for bank rescans / patch reloads
    float        _block  = 256.f;       // platform block size (for --ksmps on a recompile)
    float        _sr     = 48000.f;
    int          _ksmps  = 0;           // == block size; guards the process() copy (see publish order)
    int          _midi_instr = 0;       // Csound instr number of "MidiNote" (0 => MIDI notes dropped)
    float        _level  = 0.f;         // output peak meter (written in process(), read in render())
    bool         _patch_loaded = false; // true => running an SD slot; false => the built-in

    NoteQueue<32> _notes;               // pending MIDI notes: main loop pushes, process() (ISR) drains

    // --- patch bank / Alt+PITCH selection -------------------------------------------------------
    static constexpr int kMaxAvail = kMaxPatchSlots + 1;   // the built-in plus every present slot
    bool _present[kMaxPatchSlots] = {false};
    int  _avail_slot[kMaxAvail] = {-1};   // [0] = -1 (built-in); then the present slot numbers
    int  _avail_n   = 1;                  // count of selectable orchestras (>= 1: the built-in)
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
    static constexpr int kSlots = 16;     // 8 per deck (Speed/Mix/Size/Env/Feedback/ModSpeed/ModAmp)
    float _cache[kSlots] = {0.f};
};

} // namespace spotykach
