// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "engine/reverb/reverb_engine.h"
#include "engine/arena.h"
#include "engine/indicators.h"           // shared indicator toolkit (docs/dev/indicator-comparison.md §7)
#include "engine/faust/faust_capture.h"  // shared faustgen::CaptureUI / Bind / Role

// The generated kernels. Each declares `class mydsp` inside namespace spotykach::rv_<name>; the
// faust_arch.h base types (dsp/UI/Meta, pulled in by these headers) live in the global namespace.
#include "engine/reverb/faust_kernel_dattorro.h"
#include "engine/reverb/faust_kernel_zita.h"
#include "engine/reverb/faust_kernel_greyhole.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new> // placement new

namespace spotykach {

namespace {

// The six reverb-agnostic knob roles. Each concrete reverb binds these to its own Faust sliders; the
// platform's physical knobs map onto them (see ReverbEngine::set_param). Order matches ReverbEngine::_v[].
enum Knob { K_Mix = 0, K_Decay, K_Damp, K_Tone, K_SizeA, K_SizeB, K_Count };

// The zone-capture machinery (Role / Bind / CaptureUI) is shared by every Faust-backed engine and lives
// in engine/faust/faust_capture.h (faustgen::). Bind.role is a generic int index into the Role table;
// reverb's unscoped Knob enum converts to it directly. The shared CaptureUI keeps the optional "Level"
// pin reverb relies on (level_out), so this engine's behaviour is unchanged.
using faustgen::Role;
using faustgen::Bind;
using CaptureUI = faustgen::CaptureUI<false>; // reverb doesn't read slider defaults; skip that capture

} // namespace

// Base wrapper: holds the bound role table + a stable display name. compute()/init() are supplied by
// the per-kernel subclass below (each owns a concrete ::mydsp by value, so the big delay-line state
// lives wherever the object is placed - i.e. in the SDRAM arena).
struct ReverbVoice {
    Role role[K_Count];
    virtual ~ReverbVoice() = default;
    virtual void init(int sample_rate) = 0;
    virtual void compute(FAUSTFLOAT** in, FAUSTFLOAT** out, int n) = 0;
    virtual const char* name() const = 0;
    // virtual so a non-Faust voice (GigaverbVoice) can route knobs through its own param API instead
    // of the Role/zone table; Dattorro/Zita use this default.
    virtual void set_knob(Knob k, float v) { role[k].set(v); }
};

namespace {

// --- Dattorro plate ----------------------------------------------------------------------------
// "Diffusion 1/2" appear in both the Input and Feedback boxes; the section field disambiguates them.
struct DattorroVoice : ReverbVoice {
    rv_dattorro::mydsp dsp;
    void init(int sr) override {
        dsp.init(sr);
        static const Bind kBinds[] = {
            { "Input",    "Prefilter",   K_Tone,  0 },
            { "Input",    "Diffusion 1", K_SizeA, 0 },
            { "Input",    "Diffusion 2", K_SizeA, 1 },
            { "Feedback", "Diffusion 1", K_SizeB, 0 },
            { "Feedback", "Diffusion 2", K_SizeB, 1 },
            { "Feedback", "Decay Rate",  K_Decay, 0 },
            { "Feedback", "Damping",     K_Damp,  0 },
            { nullptr,    "Dry/Wet Mix", K_Mix,   0 }, // -1 dry .. +1 wet (linear-mapped from 0..1)
        };
        FAUSTFLOAT* level = nullptr;
        CaptureUI ui; ui.roles = role; ui.binds = kBinds;
        ui.nbinds = static_cast<int>(sizeof(kBinds) / sizeof(kBinds[0])); ui.level_out = &level;
        dsp.buildUserInterface(&ui);
        if (level) *level = -6.f; // hold output gain fixed (dB)
    }
    void compute(FAUSTFLOAT** in, FAUSTFLOAT** out, int n) override { dsp.compute(n, in, out); }
    const char* name() const override { return "PLATE"; }
};

// --- Zita-rev1 hall ----------------------------------------------------------------------------
struct ZitaVoice : ReverbVoice {
    rv_zita::mydsp dsp;
    void init(int sr) override {
        dsp.init(sr);
        static const Bind kBinds[] = {
            { nullptr, "Wet/Dry Mix", K_Mix,   0, true }, // zita: +1=dry..-1=wet, so invert for "up = wet"
            { nullptr, "Mid RT60",    K_Decay, 0 }, // main decay time
            { nullptr, "HF Damping",  K_Damp,  0 },
            { nullptr, "Low RT60",    K_Tone,  0 }, // low-band decay -> tonal character
            { nullptr, "In Delay",    K_SizeA, 0 }, // pre-delay
            { nullptr, "Eq1 Level",   K_SizeB, 0 }, // tail tone: low-mid peaking EQ (+/-15 dB, mid=flat)
        };
        FAUSTFLOAT* level = nullptr;
        CaptureUI ui; ui.roles = role; ui.binds = kBinds;
        ui.nbinds = static_cast<int>(sizeof(kBinds) / sizeof(kBinds[0])); ui.level_out = &level;
        dsp.buildUserInterface(&ui);
        if (level) *level = 0.f; // unity output (dB); Eq1 Level is a knob (SizeB), Eq2 + EQ freqs stay default
    }
    void compute(FAUSTFLOAT** in, FAUSTFLOAT** out, int n) override { dsp.compute(n, in, out); }
    const char* name() const override { return "HALL"; }
};

// --- Greyhole (modulated diffusion) -------------------------------------------------------------
// The third Faust voice (replaces the former gen~ gigaverb): Greyhole - a modulated diffusion network
// with a pitch-shifter in the feedback, a lush evolving/ambient reverb very unlike the static plate/hall.
// Its .dsp owns the dry/wet crossfade, so the Mix role binds straight to its "Mix" slider like the plate;
// all six knob roles map to a real control.
struct GreyholeVoice : ReverbVoice {
    rv_greyhole::mydsp dsp;
    void init(int sr) override {
        dsp.init(sr);
        static const Bind kBinds[] = {
            { nullptr, "Mix",       K_Mix,   0 }, // dry/wet (0..1 wet, like the plate)
            { nullptr, "Feedback",  K_Decay, 0 }, // tail length (PITCH)
            { nullptr, "Damp",      K_Damp,  0 }, // HF damping (ENV)
            { nullptr, "Diffusion", K_Tone,  0 }, // smear/character (POS)
            { nullptr, "Size",      K_SizeA, 0 }, // network size (SIZE)
            { nullptr, "ModDepth",  K_SizeB, 0 }, // modulation depth (MODAMT)
        };
        CaptureUI ui; ui.roles = role; ui.binds = kBinds;
        ui.nbinds = static_cast<int>(sizeof(kBinds) / sizeof(kBinds[0]));
        dsp.buildUserInterface(&ui);
    }
    void compute(FAUSTFLOAT** in, FAUSTFLOAT** out, int n) override { dsp.compute(n, in, out); }
    const char* name() const override { return "GREY"; }
};

// ParamId (physical knob) -> reverb role. Returns K_Count for ids this engine does not map.
Knob role_for(ParamId id) {
    switch (id) {
        case ParamId::Mix:    return K_Mix;   // SOS
        case ParamId::Speed:  return K_Decay; // PITCH (biggest knob) -> Decay, the main character control
        case ParamId::Env:    return K_Damp;  // ENV
        case ParamId::Pos:    return K_Tone;  // POS   -> Tone
        case ParamId::Size:   return K_SizeA; // SIZE
        case ParamId::ModAmp: return K_SizeB; // MOD_AMT
        default:              return K_Count;
    }
}

} // namespace

void ReverbEngine::init(const EngineContext& ctx)
{
    Arena ar(ctx.arena);
    const int sr = static_cast<int>(ctx.sample_rate);
    // One full set of voices PER DECK so each deck's selected reverb has independent delay-line state
    // (DoubleMono runs both at once). Dattorro ~126 KB + Zita ~937 KB + Greyhole per deck.
    for (int d = 0; d < DeckRef::Count; d++) {
        if (void* m = ar.alloc<uint8_t>(sizeof(DattorroVoice), alignof(DattorroVoice))) _rv[d][0] = new (m) DattorroVoice();
        if (void* m = ar.alloc<uint8_t>(sizeof(ZitaVoice),     alignof(ZitaVoice)))     _rv[d][1] = new (m) ZitaVoice();
        if (void* m = ar.alloc<uint8_t>(sizeof(GreyholeVoice), alignof(GreyholeVoice))) _rv[d][2] = new (m) GreyholeVoice();

        for (int v = 0; v < kReverbCount; v++) if (_rv[d][v]) _rv[d][v]->init(sr);
        _active[d] = 0;
        apply_all_knobs(static_cast<DeckRef::Ref>(d));
        _peak[d] = 0.f;
    }
    _route = Route::Stereo;
}

void ReverbEngine::apply_all_knobs(DeckRef::Ref deck)
{
    ReverbVoice* rv = _rv[deck][eff_voice(deck)];
    if (!rv) return;
    for (int k = 0; k < K_Count; k++) rv->set_knob(static_cast<Knob>(k), _v[deck][k]);
}

void ReverbEngine::process(const float* const* in, float** out, size_t size)
{
    if (!out || !out[0] || !out[1]) return;

    if (_route == Route::DoubleMono) {
        // Two independent MONO reverbs: in[d] -> deck d's PLATE -> out[d]. DoubleMono is plate-only
        // (eff_voice caps it): the hall/greyhole are too heavy to run two-up. The Faust plate is stereo,
        // so each deck is fed mono (input fanned to both channels) with one output kept; the discarded
        // channel lands in a throwaway scratch. compute() is allocation-free -> ISR-safe.
        static constexpr size_t kMaxBlock = 128; // platform block is 96
        float scratch[kMaxBlock];
        const int n = static_cast<int>(size < kMaxBlock ? size : kMaxBlock);
        for (int d = 0; d < DeckRef::Count; d++) {
            ReverbVoice* rv  = _rv[d][eff_voice(d)];
            const float* din = in ? in[d] : nullptr;
            if (!rv || !din) { std::memset(out[d], 0, size * sizeof(float)); _peak[d] = 0.f; continue; }
            FAUSTFLOAT* ins[2]  = { const_cast<float*>(din), const_cast<float*>(din) };
            FAUSTFLOAT* outs[2] = { out[d], scratch };
            rv->compute(ins, outs, n);
            float peak = 0.f;
            for (int i = 0; i < n; i++) peak = std::fmax(peak, std::fabs(out[d][i]));
            _peak[d] = peak;
        }
        return;
    }

    // Stereo / GenerativeStereo: ONE stereo voice (deck A's selection) reverberates the stereo pair.
    ReverbVoice* rv = _rv[DeckRef::A][_active[DeckRef::A]];
    if (!rv || !in || !in[0] || !in[1]) {
        std::memset(out[0], 0, size * sizeof(float));
        std::memset(out[1], 0, size * sizeof(float));
        _peak[0] = _peak[1] = 0.f;
        return;
    }
    FAUSTFLOAT* ins[2]  = { const_cast<float*>(in[0]), const_cast<float*>(in[1]) };
    FAUSTFLOAT* outs[2] = { out[0], out[1] };
    rv->compute(ins, outs, static_cast<int>(size));
    float peak = 0.f;
    for (size_t i = 0; i < size; i++) {
        peak = std::fmax(peak, std::fabs(out[0][i]));
        peak = std::fmax(peak, std::fabs(out[1][i]));
    }
    _peak[0] = _peak[1] = peak;
}

// Knobs act per deck and drive that deck's effective voice (its plate in DoubleMono, its switch-
// selected voice in a stereo route). In a stereo route only deck A is heard (deck B updates an idle voice).
void ReverbEngine::set_param(ParamId id, DeckRef::Ref deck, float v)
{
    v = std::clamp(v, 0.f, 1.f);
    const Knob k = role_for(id);
    if (k == K_Count || deck >= DeckRef::Count) return;
    _v[deck][k] = v;
    if (ReverbVoice* rv = _rv[deck][eff_voice(deck)]) rv->set_knob(k, v);
}

float ReverbEngine::param(ParamId id, DeckRef::Ref deck) const
{
    const Knob k = role_for(id);
    if (k == K_Count || deck >= DeckRef::Count) return 0.f;
    return _v[deck][k];
}

// ConfigId::Route sets the process() topology; ConfigId::Mode (the Reel/Slice/Drift switch) selects the
// voice per deck. _active tracks each deck's switch even in a stereo route, but the route decides the
// EFFECTIVE voice (eff_voice): DoubleMono forces plate, so the mode switch only takes effect in a stereo
// route. Route wire values: 0=Stereo, 1=DoubleMono, 2=GenerativeStereo (see core.ui.cpp).
bool ReverbEngine::set_config(ConfigId id, DeckRef::Ref deck, int value)
{
    if (id == ConfigId::Route) {
        const Route r = value == 2 ? Route::GenerativeStereo : value == 1 ? Route::DoubleMono : Route::Stereo;
        if (r == _route) return false;
        _route = r;
        // The effective voice flips (plate <-> switch selection), so re-seed both decks' knobs into it.
        apply_all_knobs(DeckRef::A);
        apply_all_knobs(DeckRef::B);
        return true;
    }
    if (id != ConfigId::Mode || deck >= DeckRef::Count) return false;
    const int idx = std::clamp(value, 0, kReverbCount - 1);
    if (idx == _active[deck] || !_rv[deck][idx]) return false;
    _active[deck] = idx;
    apply_all_knobs(deck); // re-seed this deck's now-effective voice from its cached knob values
    return true;
}

void ReverbEngine::render(DisplayModel& m)
{
    m.clear();
    // Plate = cool blue, Hall = violet, Greyhole = teal (a clear third colour vs the two cool ones).
    const uint32_t kColor[kReverbCount] = { 0x00aaffu, 0x8800ffu, 0x33ffccu };
    // Algorithm colour + the effective voice index: DoubleMono forces plate (hall/greyhole are stereo-only);
    // a stereo route uses deck A's Reel/Slice/Drift selection.
    const bool     dual  = (_route == Route::DoubleMono);
    const int      vidx  = dual ? 0 /*plate*/ : _active[DeckRef::A];
    const uint32_t color = kColor[vidx];

    // The 3 mode LEDs (left/center/right) sit at the Reel/Slice/Drift switch, which on the reverb selects
    // the ALGORITHM - so light the active position in that algorithm's colour. The third position
    // (greyhole) reads as a distinct teal against plate-blue / hall-violet, and you can see which algorithm
    // is active even in silence. (Route is no longer shown here; in DoubleMono the two independent per-deck
    // rings make it visible.)
    DisplayModel::Indicator* mode_led[3] = { &m.mode_left, &m.mode_center, &m.mode_right };
    if (vidx >= 0 && vidx < kReverbCount) *mode_led[vidx] = { color, 0.85f };

    // Per-deck ring. The reverb used to light the ring only with the live output level, so the panel was
    // dark whenever the input was quiet and showed nothing about the patch. Instead, draw three things at
    // once, so the ring is always legible:
    //   - colour   = the active algorithm (plate blue / hall violet / greyhole teal), readable in silence;
    //   - arc      = the DECAY knob (PITCH) - how long the tail is - so turning it gives instant feedback;
    //   - brightness = the output level, layered on top, so the arc pulses with signal and visibly fades
    //                  out as the reverb tail decays ("is it doing anything?" answered).
    // DoubleMono runs an independent plate per deck (hall/greyhole are stereo-only), so each ring shows its
    // own deck's decay; a stereo route shows deck A's voice + decay on both rings (one shared voice).
    for (int d = 0; d < DeckRef::Count; d++) {
        const float decay = _v[dual ? d : DeckRef::A][K_Decay];
        const float level = _peak[d] > 1.f ? 1.f : _peak[d];
        // Dim full-ring baseline in the algorithm colour (never fully dark; cf. the radio/tape ring idiom),
        // then the DECAY arc on top, its brightness pulsing with the output level so the tail visibly fades.
        ring::level(m.ring[d], 0.999f, color, 0.10f);
        ring::level(m.ring[d], decay,  color, 0.35f + 0.60f * level);
        m.ring[d].set_updated();
        m.play[d] = { color, 0.35f + 0.60f * level }; // play pad: algorithm colour, also pulsing with level
    }
}

};
