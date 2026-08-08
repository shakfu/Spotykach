// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/engine_leds.h"   // FxLeds/PlayLeds/AltLeds/TransportLeds/DeckLeds/RingGeometry
#include "engine/display_model.h"
#include "engine/granular/core.h"
#include "engine/granular/speed.map.h"
#include "nocopy.h"

#include <cstdint>


namespace spotykach {

// The granular looper as an IEngine. It owns the Core graph, forwards the audio lifecycle, and
// (after the input migration) owns all granular *input* meaning: parameters, MIDI, and pad
// gestures - see the grouped methods below. The refactor is PAUSED at this input-decoupled
// milestone; the output/IO side (LEDs, CV, gate, storage) still reaches the graph through the
// core() escape hatch documented at that method.
class GranularEngine : public IEngine {
public:
    GranularEngine() = default;
    ~GranularEngine() override = default;

    void init(const EngineContext& ctx) override;  // pre-seeds the param cache (impl in .cpp, -Os)
    void prepare() override { _core.prepare(); }
    void process(const float* const* in, float** out, size_t size) override {
        _core.process(in, out, size);
    }

    // Parameter API (Phase 3a). The platform will drive these in place of reaching into Core
    // directly. set_param owns the mode-dependent dispatch/fan-out (Reel/Slice/Drift); the
    // deck arg is ignored for global params. param() returns the last-set value (cache).
    void set_param(ParamId id, DeckRef::Ref deck, float value) override;
    float param(ParamId id, DeckRef::Ref deck) const override;

    // Categorical config (item 3a-0). Maps the platform's switch selectors to Core enums and owns
    // the side effects (panner inference on mode/route, per-deck LFO palette). set_config returns
    // whether Mode changed; toggle_grit_mode returns the reseed values; tempo_to_fit the fit BPM.
    bool       set_config(ConfigId id, DeckRef::Ref deck, int value) override;
    float      tempo_to_fit(DeckRef::Ref deck, float fraction) override;
    GritReseed toggle_grit_mode(DeckRef::Ref deck) override;

    // Knob layout (item 3a-3): map the deck's granular Mode to the platform-facing DeckLayout, and
    // report tempo-fit eligibility (Slice && non-empty), so the platform stops reading Core's Mode.
    DeckLayout deck_layout(DeckRef::Ref deck) override;
    bool       size_sets_tempo(DeckRef::Ref deck) override;

    Capabilities capabilities() const override;

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md). Derived from the set_param /
    // set_config switches in granular_engine.cpp, which are authoritative: every id below reaches real
    // work. Deliberately EXCLUDED, though the switch names them:
    //   AltPos, Aux - explicit no-ops here ("granular doesn't claim CapAltPos / CapAux"), so a sweep
    //                 would set them and assert on a value that went nowhere.
    //   Tempo, KeyInterval, ModSpeed - platform-owned; the platform never routes them through
    //                 set_param, so describe drops them regardless (see param_is_platform_owned).
    // NOT yet verified on hardware.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::FluxFb))
             | (1u << static_cast<uint32_t>(ParamId::Env))
             | (1u << static_cast<uint32_t>(ParamId::EnvSize))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Win))
             | (1u << static_cast<uint32_t>(ParamId::PolySlice))
             | (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::FluxIntensity))
             | (1u << static_cast<uint32_t>(ParamId::GritIntensity))
             | (1u << static_cast<uint32_t>(ParamId::FluxMix))
             | (1u << static_cast<uint32_t>(ParamId::GritMix))
             | (1u << static_cast<uint32_t>(ParamId::Feedback))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::ModAmp))
             | (1u << static_cast<uint32_t>(ParamId::ClickMix))
             | (1u << static_cast<uint32_t>(ParamId::PanSpeed))
             | (1u << static_cast<uint32_t>(ParamId::PanRange))
             | (1u << static_cast<uint32_t>(ParamId::Crossfade));
    }
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>((1u << static_cast<uint32_t>(ConfigId::Route))
                                     | (1u << static_cast<uint32_t>(ConfigId::ModType))
                                     | (1u << static_cast<uint32_t>(ConfigId::LfoShape))
                                     | (1u << static_cast<uint32_t>(ConfigId::Mode))
                                     | (1u << static_cast<uint32_t>(ConfigId::StartModOn))
                                     | (1u << static_cast<uint32_t>(ConfigId::SizeModOn)));
    }
#endif

    // MIDI meaning (Phase 3c). The platform parses MIDI and clocks transport; the engine
    // decides what notes and transport mean for this instrument.
    // handle_midi_note: channel->deck, note->speed, trigger. Returns the triggered deck
    // (or DeckRef::Count if the channel is unmatched) so the platform can flash the gate LED.
    DeckRef::Ref handle_midi_note(uint8_t channel, uint8_t note) override;
    void handle_midi_transport(bool start) override; // true=start/continue, false=stop

    // FX pads (Phase 3c). The platform owns the pad gesture + modifier state; the engine
    // owns what flux/grit do. set_fx is momentary on/off; toggle_fx_lock is the alt+pad latch.
    void set_fx(DeckRef::Ref, FxKind, bool on) override;
    void toggle_fx_lock(DeckRef::Ref, FxKind) override;

    // Play/Rev pads (Phase 3c). The platform owns tap/hold + storage/tape + LED; the engine
    // owns the granular play/record/stop decision. on_play_pad returns is_empty (for the LED).
    void stop_if_generating(DeckRef::Ref) override;
    void clear_buffer(DeckRef::Ref) override;
    void on_record_pad(DeckRef::Ref, bool reverse) override;
    bool on_play_pad(DeckRef::Ref, bool reverse) override;

    // Seq pads (Phase 3c). Platform owns storage/tape + the hold-to-clear timer; the engine
    // owns the sequencer: arm/disarm the track, trigger it, and clear the recorded sequence.
    void on_seq_toggle_arm(DeckRef::Ref) override;
    void on_seq_trigger(DeckRef::Ref) override;
    void clear_sequence(DeckRef::Ref) override;
    void disarm_track(DeckRef::Ref) override; // disarm the deck's track if armed (Alt-pad action)

    // Modulator speed (Phase 3c). sync = the Alt modifier (LFO sync vs free).
    void set_mod_speed(DeckRef::Ref, float value, bool sync) override;

    // CV inputs (Phase 3c). The platform reads + calibrates each jack and routes by role; the
    // engine decides what each CV does. cv_voct caches the V/Oct speed for the gate trigger.
    void cv_mix(DeckRef::Ref, float value) override;
    void cv_size_pos(DeckRef::Ref, float value) override;
    void cv_voct(DeckRef::Ref, float value) override;
    void cv_crossfade(float value) override;

    // Gate (Phase 3c). on_gate_trigger fires the deck at the last V/Oct speed; the platform
    // owns edge/latency detection + the gate-out pulse timing. gate_out_triggered reports a
    // loop-reset event for the platform's gate-out.
    void on_gate_trigger(DeckRef::Ref) override;
    bool gate_out_triggered(DeckRef::Ref) override;

    // Storage audio port (Phase 3c, "TapeStorage" capability). The platform's Storage owns the
    // tape/slot state machine + SD I/O; it gets the deck's loop buffer as a raw byte range to
    // save/load. `frames` in audio_apply_loaded is the card's WAV-derived size_audio().
    bool   audio_is_empty(DeckRef::Ref) override;
    uint8_t* audio_data(DeckRef::Ref) override;
    size_t audio_recorded_bytes(DeckRef::Ref) override;
    size_t audio_capacity_bytes(DeckRef::Ref) override;
    void   audio_apply_loaded(DeckRef::Ref, size_t frames) override;

    // Transport is no longer an engine concern: the platform owns the Transport service and injects a
    // read-only ITransport via EngineContext (Core subscribes to its ticks). The old transport_*
    // forwarding group + transport_leds() were removed from IEngine when the Driver was split.

    // CV outputs (DAC, block-rate). Fills n samples of the two modulator CV channels. Faithful to
    // the old per-sample DACCallback loop: each sample is zeroed then written by the deck's mod.
    void process_cv(float* cv0, float* cv1, size_t n) override {
        auto& ma = _core.mod(DeckRef::A);
        auto& mb = _core.mod(DeckRef::B);
        for (size_t i = 0; i < n; i++) {
            cv0[i] = 0.f; cv1[i] = 0.f;
            ma.process(cv0[i]);
            mb.process(cv1[i]);
        }
    }

    // LED indicator state (LED migration Round 2). The platform reads these to render the
    // flux/grit, play/rev, and alt indicators; it keeps the colors + blink/timer/storage logic.
    FxLeds   fx_leds(DeckRef::Ref) override;
    PlayLeds play_leds(DeckRef::Ref) override;
    AltLeds  alt_leds(DeckRef::Ref) override;

    // Topology state for the ISR LED render (_draw_leds) and launch-quant display. (Clock indicator
    // state moved to the platform Transport - transport_leds() is gone from IEngine.)
    DeckLeds      deck_leds(DeckRef::Ref) override;
    float         mix() const override;   // A/B crossfade (fader LEDs)
    Route         route() const override; // channel topology (mode L/C/R LED)

    // Steady-state ring draw (LED migration Round 3). Caller must have cleared the ring and set
    // the default (mode) color + 0.5 brightness; this draws the empty/recording/playing segment +
    // heads on that baseline and returns the geometry the platform's pos/size/overdub overlays use.
    RingGeometry render_ring(LEDRing& ring, DeckRef::Ref, float breathe_brightness) override;

private:
    NOCOPY(GranularEngine)

    static DeckRef::Ref _safe_ref(DeckRef::Ref ref) { return ref < DeckRef::Count ? ref : DeckRef::A; }

    Core _core;
    SpeedMap<60> _speed_map;
    float _voct_speed[DeckRef::Count] = { 1.f, 1.f }; // last V/Oct CV speed, used by gate triggers
    float _param_cache[static_cast<size_t>(ParamId::Count)][DeckRef::Count] = {};
};

};
