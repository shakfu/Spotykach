// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "abi_tag.h"                // SPK_ABI_BEGIN/END - IEngine's vtable changes with SPK_TERMINAL
#include "engine/engine_context.h"  // EngineContext (contract; type-clean since item 5b)
#include "engine/deck_ref.h"      // DeckRef (the A/B selector every method takes)
#include "engine/mode.h"          // Route, ClockSource (contract-owned, items 5b/5c)
#include "engine/engine_params.h" // ParamId, FxKind, Capabilities
#include "engine/wav_cues.h"      // WavCues (platform-parsed WAV markers for CapWavCues engines)
#include "engine/display_model.h" // DisplayModel
#include "engine/engine_leds.h"   // FxLeds/PlayLeds/AltLeds/TransportLeds/DeckLeds/RingGeometry
#if SPK_TERMINAL
#include "engine/terminal_io.h"   // CommandView/TextSink for the terminal channel (phase-1 test surface)
#endif

namespace spotykach {
SPK_ABI_BEGIN   // IEngine gains virtuals under SPK_TERMINAL; see abi_tag.h

// IEngine is the contract between the fixed hardware/UI platform and a swappable DSP engine.
// A "firmware variant" is a different IEngine implementation behind the same platform.
//
// Audio lifecycle (init/prepare/process) is pure-virtual: every engine must implement it; the
// audio path flows app -> IEngine -> engine -> DSP.
//
// Item 2 (interface lift, Strategy A): the rest of the platform-driven surface is declared here
// with **no-op default bodies** - an engine overrides only what it supports, and capabilities()
// advertises which optional regions are live. CoreUI/Storage drive the engine through this
// interface instead of a concrete GranularEngine.
//
// The platform-driven input/output surface lives on IEngine (items 2-3): params/config, MIDI, pads,
// CV/gate, storage, the LED queries, and render(DisplayModel). `core()` is gone; CoreUI/Storage hold
// only IEngine. The contract carries no granular types (Phase 5 R1/R2): DeckRef/Mode/Route/ModType/
// GritMode/DeckSource/ClockSource are contract-owned (engine/...). Transport is NOT on IEngine: the
// Driver was split into a platform Transport service (src/transport/) that the platform owns and
// injects into engines read-only via EngineContext (ITransport) - so tempo-synced non-granular
// engines (the delay; a future Euclidean sequencer) share one clock.
class IEngine {
public:
    virtual ~IEngine() = default;

    // --- Audio lifecycle (required of every engine) ---------------------------------------------
    // Allocate/initialise from the injected platform context (buffers, sample rate, clock).
    virtual void init(const EngineContext& ctx) = 0;
    // Non-real-time, main-loop housekeeping.
    virtual void prepare() = 0;
    // Real-time audio block (audio ISR). Keep per-sample work inside non-virtual engine code.
    virtual void process(const float* const* in, float** out, size_t size) = 0;

    // --- Capabilities: which optional regions this engine opts into --------------------------
    virtual Capabilities capabilities() const { return 0; }

    // --- Parameters (platform control -> logical ParamId; engine owns the mode-dep dispatch) -
    virtual void  set_param(ParamId, DeckRef::Ref, float) {}
    virtual float param(ParamId, DeckRef::Ref) const { return 0.f; }
    virtual void  set_mod_speed(DeckRef::Ref, float value, bool sync) {}

    // Re-seed request. An engine whose deck->value mapping can change underfoot (e.g. edrums' Rev-pad
    // drum swap repoints a deck's knobs at a different drum) sets this; the platform polls it each loop
    // and, when true, re-seeds its MValue pickup cache from param() for that deck. Without it the next
    // knob touch would snap the newly-focused target to the stale knob value. Returns true ONCE per
    // change and self-clears. Default false: engines with a fixed deck->value mapping need nothing.
    virtual bool  take_param_reseed(DeckRef::Ref) { return false; }

    // UI hint: the Alt+PITCH (Aux) selector is currently held for this deck (Alt down on a CapAux
    // engine, and that deck's PITCH is not claimed by an fx touch). The platform pushes it each loop so
    // an own-display engine can show its Aux selector the whole time Alt is held, not just on a change.
    // Default no-op: engines that don't draw an Aux selector ignore it.
    virtual void  set_aux_active(DeckRef::Ref, bool active) {}

    // --- Categorical config (item 3a-0): switch-position writes the platform used to make
    //     directly on Core. The engine maps the selector int to its enums and owns side effects.
    //     set_config returns true iff the value CHANGED (only Mode uses this, so the platform can
    //     re-apply size for the new mode). tempo_to_fit gives the BPM that fits `fraction` of the
    //     deck's loop (the Slice tap-hold gesture). toggle_grit_mode cycles the grit sub-effect and
    //     returns the now-active intensity/mix for the platform's MValue reseed. ---
    virtual bool       set_config(ConfigId, DeckRef::Ref, int) { return false; }
    virtual float      tempo_to_fit(DeckRef::Ref, float fraction) { return 0.f; }
    virtual GritReseed toggle_grit_mode(DeckRef::Ref) { return {}; }

    // --- Knob layout (item 3a-3): lets the platform branch the SIZE/ENV pot handling and the apply
    //     pass without reading Core's Mode. deck_layout describes how those knobs behave for this
    //     deck; size_sets_tempo reports whether the SIZE tap-hold tempo-fit gesture is eligible
    //     (granular: Slice && non-empty). Defaults suit a modeless engine. ---
    // (non-const: the granular override forwards to the non-const Core, as transport_* does)
    virtual DeckLayout deck_layout(DeckRef::Ref) { return DeckLayout::single; }
    virtual bool       size_sets_tempo(DeckRef::Ref) { return false; }

    // --- MIDI meaning ------------------------------------------------------------------------
    virtual DeckRef::Ref handle_midi_note(uint8_t channel, uint8_t note) { return DeckRef::Count; }
    virtual void      handle_midi_transport(bool start) {}
    // Raw 3-byte MIDI message (status incl. channel, data1, data2) for engines that consume the full
    // MIDI vocabulary rather than just NoteOn - e.g. ChucK delivers these to a patch's MidiIn. Default
    // no-op: engines using only handle_midi_note ignore it. Called on the main loop (the UI MIDI poll).
    virtual void      handle_midi_message(uint8_t status, uint8_t data1, uint8_t data2) {}

    // --- FX pads -----------------------------------------------------------------------------
    virtual void set_fx(DeckRef::Ref, FxKind, bool on) {}
    virtual void toggle_fx_lock(DeckRef::Ref, FxKind) {}

    // --- Play/Rev pads (on_play_pad returns is_empty for the LED) -----------------------------
    virtual void stop_if_generating(DeckRef::Ref) {}
    virtual void clear_buffer(DeckRef::Ref) {}
    virtual void on_record_pad(DeckRef::Ref, bool reverse) {}
    virtual bool on_play_pad(DeckRef::Ref, bool reverse) { return true; }

    // --- Seq pads ----------------------------------------------------------------------------
    virtual void on_seq_toggle_arm(DeckRef::Ref) {}
    virtual void on_seq_trigger(DeckRef::Ref) {}
    virtual void clear_sequence(DeckRef::Ref) {}
    virtual void disarm_track(DeckRef::Ref) {}

    // --- CV inputs ---------------------------------------------------------------------------
    virtual void cv_mix(DeckRef::Ref, float value) {}
    virtual void cv_size_pos(DeckRef::Ref, float value) {}
    virtual void cv_voct(DeckRef::Ref, float value) {}
    virtual void cv_crossfade(float value) {}

    // --- Gate (gate_out_triggered reports a loop-reset for the platform's gate-out) ----------
    virtual void on_gate_trigger(DeckRef::Ref) {}
    virtual bool gate_out_triggered(DeckRef::Ref) { return false; }

    // --- Storage audio port (TapeStorage capability) -----------------------------------------
    virtual bool     audio_is_empty(DeckRef::Ref) { return true; }
    virtual uint8_t* audio_data(DeckRef::Ref) { return nullptr; }
    virtual size_t   audio_recorded_bytes(DeckRef::Ref) { return 0; }
    virtual size_t   audio_capacity_bytes(DeckRef::Ref) { return 0; }
    virtual void     audio_apply_loaded(DeckRef::Ref, size_t frames) {}

    // --- WAV cue markers (CapWavCues) --------------------------------------------------------
    // After a WAV finishes loading for a CapWavCues engine, the platform parses the file's `cue `
    // chunk and calls this with the markers (sample-frame offsets into the just-loaded audio).
    // Default no-op; the engine assigns meaning (slice starts, loop points, jump markers, ...).
    virtual void     on_wav_cues(DeckRef::Ref, const WavCues&) {}

    // --- Transport: NOT an engine concern. The platform owns the Transport service and injects a
    //     read-only ITransport via EngineContext; an engine that wants tempo/ticks reads or subscribes
    //     to it (e.g. granular Core, the tempo-synced delay). The old transport_* forwarding group was
    //     removed here when the Driver was split into the platform Transport. -----------------------

    // --- LED queries (item 2b): the engine reports indicator + ring-geometry state; the platform
    //     owns the color palette + blink/timer/storage/_touched compositing + the MValue overlays.
    //     Defaults are inert (a non-granular engine returns empty state and the granular-specific
    //     platform LED code reads nothing). These + render() below are transitional: item 3 unifies
    //     them into render(DisplayModel&) + the MValue->ParamId value-display toolkit. ---
    virtual FxLeds   fx_leds(DeckRef::Ref) { return {}; }
    virtual PlayLeds play_leds(DeckRef::Ref) { return {}; }
    virtual AltLeds  alt_leds(DeckRef::Ref) { return {}; }
    virtual DeckLeds      deck_leds(DeckRef::Ref) { return {}; }
    virtual float mix() const { return 0.5f; }   // A/B crossfade (fader LEDs)
    virtual Route route() const { return Route::Stereo; } // channel topology (mode L/C/R LED)
    virtual RingGeometry render_ring(LEDRing& ring, DeckRef::Ref, float breathe_brightness) { return {}; }

    // --- CV outputs (DAC). The platform's DAC ISR calls this ONCE per block (not per sample),
    //     fills n samples of the two modulation/LFO CV channels, then converts to the DAC range.
    //     Block-rate by design so the ISR pays no per-sample virtual dispatch. Default = silence. ---
    virtual void process_cv(float* cv0, float* cv1, size_t n) {
        for (size_t i = 0; i < n; i++) { cv0[i] = 0.f; cv1[i] = 0.f; }
    }

    // --- Display: engine fills the panel model; platform blits + composites its overlays.
    //     Default draws nothing; the granular engine still drives LEDs via the queries above until
    //     item 3 moves it to render(DisplayModel&). ---
    virtual void render(DisplayModel&) {}

#if SPK_TERMINAL
    // --- Terminal channel (SPK_TERMINAL, docs/dev/terminal-*.md) ------------------------------
    // Engine-specific verbs and L1 `query` state the platform dispatcher does not know. Returns true
    // iff the verb was RECOGNIZED (including when replying with an error via `reply`); false only if
    // it is not this engine's verb at all (the dispatcher then emits "err unknown-verb"). Default
    // no-op: engines using only the platform-reflective surface need nothing. An engine that
    // implements any should set CapTerminal in capabilities().
    virtual bool handle_command(const CommandView& /*cmd*/, TextSink& /*reply*/) { return false; }

    // Engine-specific state, DECLARED rather than parsed (docs/dev/terminal-target-b.md). Return a
    // static table describing what this engine can report; the platform matches the name, validates the
    // deck from the declared scope, frames the reply, and emits the `safe` entries into `describe` - so
    // a generic host discovers and sweeps engine state with no per-engine code, and `handle_command`
    // above is left for side-effecting verbs that a host must NOT call speculatively.
    //
    // read_engine_query() answers by INDEX into that table: no casts back to the concrete type, no
    // function pointers in flash, and a static_assert in the engine can catch table/enum drift.
    // It must append only the VALUE - the platform writes "ok " and the CRLF.
    virtual EngineQueryTable engine_queries() const { return { nullptr, 0 }; }
    virtual void read_engine_query(uint8_t /*index*/, DeckRef::Ref, TextSink&) {}

    // Liveness masks for `describe`: a bitset over ParamId/ConfigId marking what this engine actually
    // implements, so the descriptor lists only real params (a generic round-trip sweep would else get
    // false failures on ignored params). Default "all live" - describe over-reports and the host sweep
    // must tolerate ignored params until an engine narrows these. (ParamId has 24 values < 32,
    // ConfigId has 6 < 8, so uint32_t/uint8_t suffice.)
    using ParamMask  = uint32_t;
    using ConfigMask = uint8_t;
    // The value `reset` writes for this param. Default 0.5 - the midpoint of the normalized range:
    // deterministic, which is what a test baseline needs, but not necessarily musical. An engine with
    // real neutral values should override (a delay whose feedback default is 0.5 will self-oscillate
    // less alarmingly at 0.2). Only ever called from the terminal, never from the audio path.
    virtual float      param_default(ParamId) const { return 0.5f; }

    virtual ParamMask  live_params()  const { return ~ParamMask{0}; }
    virtual ConfigMask live_configs() const { return static_cast<ConfigMask>(~ConfigMask{0}); }

    // Layer-3 name for a param slot: what this engine actually DOES with it, as opposed to the shared
    // layer-2 slot name (`radio`: speed -> "station"; `tape`: size -> "character"). Purely cosmetic to
    // the device - no address, reply or error derives from it - which is why it defaults to nullptr
    // ("use the kParamNames entry") and no engine is obliged to care. It exists for `describe`, whose
    // OSC form carries a label per row so a control surface can print the engine's word on a fader that
    // is still bound to the stable generic address. One tier up it IS load-bearing: the host-side
    // semantic namespace is generated from these. See docs/dev/terminal-osc.md ("Where the label comes
    // from"). A label that rots misnames a control; it can never make the device unreachable.
    virtual const char* param_label(ParamId) const { return nullptr; }
#endif
};

SPK_ABI_END

};
