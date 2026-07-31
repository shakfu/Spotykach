// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once
#include <cstddef>   // size_t (transitive-include hygiene; host build)

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "nocopy.h"

namespace spotykach {

struct ReverbVoice; // defined in reverb_engine.cpp (wraps one generated kernel + its knob mapping)

// ReverbEngine - a multi-algorithm reverb whose DSP is Faust-generated. Each algorithm is a
// cyfaust-generated kernel (src/engine/reverb/<name>.dsp -> faust_kernel_<name>.h, one per namespace
// rv_<name>); the physical Reel/Slice/Drift mode switch (ConfigId::Mode) selects which one is live.
// Currently three Faust voices: a Dattorro plate, a Zita-rev1 hall, and a Greyhole.
//
// Route-aware (ConfigId::Route), like the rest of the instrument:
//   * Stereo / GenerativeStereo -> ONE stereo voice (deck A's selection) reverberates the stereo pair;
//     deck A's knobs + switch drive it, deck B's strip is inert.
//   * DoubleMono -> an independent MONO reverb per deck (deck A -> left, deck B -> right): each deck's
//     strip drives its own side, both switches live. Stereo voices are run mono (input fanned to both
//     channels, one output kept), so each side costs a full stereo reverb (dual ~= 2x the single cost).
// A full set of voices is allocated PER DECK up front, so the route is a runtime branch in process()
// with no reallocation. The active voice's compute() is allocation-free -> ISR-safe.
//
// Memory: a reverb kernel is hundreds of KB of comb/allpass/FDN delay-line state (Dattorro ~126 KB,
// Zita ~937 KB) - far too big for SRAM. Every kernel is placement-new'd into the injected SDRAM arena
// at init() (the pattern ResoEngine uses for Rings); only pointers live in this object. Per-deck
// allocation is ~2.1 MB (all three Faust voices) - trivial against the 48 MB arena.
//
// Knob map (reverb-agnostic roles; the 0..1 knob is linear-mapped into each slider's native range):
//   SOS   (Mix)    -> Mix    (wet/dry)        PITCH (Speed) -> Decay (tail length; biggest knob)
//   ENV   (Env)    -> Damp   (HF damping)     POS   (Pos)   -> Tone  (plate: prefilter / hall: low RT60)
//   SIZE  (Size)   -> SizeA  (plate: input diffusion / hall: pre-delay)
//   MODAMT(ModAmp) -> SizeB  (plate: tank diffusion  / hall: tail EQ, low-mid peak +/-15 dB)
//   Reel/Slice/Drift switch (ConfigId::Mode) -> reverb algorithm select (per deck)
class ReverbEngine : public IEngine {
public:
    static constexpr int kReverbCount = 3; // dattorro, zita, greyhole (mode switch selects; sync with init())

    ReverbEngine() = default;
    ~ReverbEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override {}
    void process(const float* const* in, float** out, size_t size) override;

    // CapDualDeck: the engine is per-deck capable (used in DoubleMono route). It has no platform runtime
    // effect on knob delivery (both decks' knobs are sent regardless), but advertises the engine's nature.
    Capabilities capabilities() const override { return CapOwnDisplay | CapDualDeck; }

    void  set_param(ParamId id, DeckRef::Ref deck, float value) override;
    float param(ParamId id, DeckRef::Ref deck) const override;
    bool  set_config(ConfigId id, DeckRef::Ref deck, int value) override; // Mode -> voice; Route -> topology
    Route route() const override { return _route; }                       // report topology for the route LED
    void  render(DisplayModel& m) override;

private:
    NOCOPY(ReverbEngine)

    // One full set of voices PER DECK (so deck A's hall and deck B's hall are separate delay-line state),
    // each deck's selected voice, knob cache, and output peak. In a stereo route only deck A's set runs
    // (as the stereo voice); in DoubleMono both run (each mono). All placement-new'd in the SDRAM arena.
    ReverbVoice* _rv[DeckRef::Count][kReverbCount] = {};
    int          _active[DeckRef::Count] = { 0, 0 };
    float        _v[DeckRef::Count][6] = { { 0.4f, 0.7f, 0.5f, 0.6f, 0.5f, 0.5f },
                                           { 0.4f, 0.7f, 0.5f, 0.6f, 0.5f, 0.5f } }; // Mix,Decay,Damp,Tone,SizeA,SizeB
    float        _peak[DeckRef::Count] = { 0.f, 0.f };
    Route        _route = Route::Stereo; // ConfigId::Route; selects the process() topology

    // The voice index actually used for a deck. DoubleMono caps every deck to the PLATE (index 0): the
    // hall and greyhole are stereo (2-in/2-out) and heavy, so they only run as a single stereo voice (in a
    // stereo route), never two-up as mono-per-deck. In a stereo route the deck plays its switch-selected
    // voice (_active). Plate is index 0 by construction (see init()).
    int  eff_voice(int deck) const { return _route == Route::DoubleMono ? 0 : _active[deck]; }
    void apply_all_knobs(DeckRef::Ref deck); // push that deck's cached knobs into its effective voice
};

};
