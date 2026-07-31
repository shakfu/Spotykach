// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "engine/reso/reso_engine.h"
#include "engine/indicators.h"   // shared indicator toolkit (docs/dev/indicator-comparison.md §7)

#include "engine/arena.h"

#include "rings/dsp/part.h"
#include "rings/dsp/patch.h"
#include "rings/dsp/performance_state.h"

#include <new>
#include <algorithm>

using namespace spotykach;

namespace {

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Cubic soft clip (unity slope near 0, saturates to +-1 at +-1.5). A local helper so we don't pull in
// daisysp here (its macros would clash with stmlib).
inline float soft_clip(float x) {
    if (x < -1.5f) return -1.f;
    if (x >  1.5f) return  1.f;
    return x - (4.f / 27.f) * x * x * x;
}

} // namespace

// All Rings/stmlib types are confined to this Impl (see the PIMPL note in the header).
struct ResoEngine::Impl {
    enum class Mode : uint8_t { Reel = 0, Slice = 1, Drift = 2 };
    static constexpr int     kModels      = 5;  // modal / symp.string / string / FM / string+reverb
    static constexpr uint8_t kFlashFrames = 6;
    static constexpr size_t  kReverbWords = 32768;

    struct Deck {
        rings::Part             part;            // the Rings engine (constructed in the arena)
        rings::Patch            patch{};
        rings::PerformanceState ps{};
        Mode  mode  = Mode::Slice;
        int   model = 2;                         // RESONATOR_MODEL_STRING

        float pitch_n = 0.5f, size_n = 0.5f, pos_n = 0.3f, env_n = 0.5f,
              modamp_n = 0.3f, mix_n = 1.f, modspeed_n = 0.f;
        float cv_semis = 0.f;   // V/Oct CV transposition in semitones (0 when nothing patched)
        float note = 60.f;

        bool     strum_pending = false;
        float    exc_burst = 0.f, exc_burst_len = 1.f; // Reel: triggered excitation burst (samples)
        float    drift_timer = 0.f, arp_timer = 0.f;
        int      arp_idx = 0;
        uint32_t rng = 0xC0FFEEu;
        float    reel_lp = 0.f, level = 0.f;
        uint8_t  flash = 0;
        bool     aux_held = false;   // platform: Alt+PITCH model-select is held -> draw the selector
    };

    ITransport* transport = nullptr;
    float       sr = 48000.f;
    Deck        deck[DeckRef::Count];
    float       param_cache[static_cast<size_t>(ParamId::Count)][DeckRef::Count] = {};

    static DeckRef::Ref safe(DeckRef::Ref d) { return d < DeckRef::Count ? d : DeckRef::A; }
    static float rand01(uint32_t& s) {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) * (1.f / 16777216.f);
    }
    static float pitch_to_note(float n) { return 24.f + clampf(n, 0.f, 1.f) * 60.f; } // MIDI C1..C6
    // The deck's current base note: PITCH knob (pitch_n) plus the V/Oct CV transposition (cv_semis).
    static float base_note(const Deck& dk) { return pitch_to_note(dk.pitch_n) + dk.cv_semis; }

    static rings::ResonatorModel model_for(int m) {
        switch (m) {
            case 0:  return rings::RESONATOR_MODEL_MODAL;
            case 1:  return rings::RESONATOR_MODEL_SYMPATHETIC_STRING;
            case 2:  return rings::RESONATOR_MODEL_STRING;
            case 3:  return rings::RESONATOR_MODEL_FM_VOICE;
            default: return rings::RESONATOR_MODEL_STRING_AND_REVERB;
        }
    }

    void setup(const EngineContext& ctx, Arena& ar) {
        transport = ctx.transport;
        sr = ctx.sample_rate;
        for (int d = 0; d < DeckRef::Count; d++) {
            Deck& dk = deck[d];
            uint16_t* rbuf = ar.alloc<uint16_t>(kReverbWords);
            dk.part.Init(rbuf);
            dk.part.set_polyphony(1);
            dk.part.set_model(model_for(dk.model));
            dk.note = base_note(dk);
            apply(static_cast<DeckRef::Ref>(d));
        }
        using PI = ParamId;
        auto P = [](PI id) { return static_cast<size_t>(id); };
        for (int d = 0; d < DeckRef::Count; d++) {
            const Deck& dk = deck[d];
            param_cache[P(PI::Speed)][d]    = dk.pitch_n;
            param_cache[P(PI::Size)][d]     = dk.size_n;
            param_cache[P(PI::Pos)][d]      = dk.pos_n;
            param_cache[P(PI::Env)][d]      = dk.env_n;
            param_cache[P(PI::ModAmp)][d]   = dk.modamp_n;
            param_cache[P(PI::Mix)][d]      = dk.mix_n;
            param_cache[P(PI::ModSpeed)][d] = dk.modspeed_n;
            param_cache[P(PI::Aux)][d]      = static_cast<float>(dk.model) / (kModels - 1);
        }
    }

    void apply(DeckRef::Ref dref) {
        Deck& dk = deck[safe(dref)];
        dk.patch.structure  = clampf(dk.modamp_n, 0.f, 1.f);
        dk.patch.brightness = clampf(dk.env_n,    0.f, 1.f);
        dk.patch.damping    = clampf(dk.size_n,   0.f, 1.f);
        dk.patch.position   = clampf(dk.pos_n,    0.f, 1.f);
        dk.ps.internal_exciter = (dk.mode != Mode::Reel); // Reel = fed from outside
        dk.ps.internal_strum   = false;
        dk.ps.internal_note    = false;
        dk.ps.chord = 0;
        dk.ps.tonic = 0.f;
        dk.ps.fm    = 0.f;
        dk.part.set_model(model_for(dk.model));
    }

    void trigger(DeckRef::Ref dref, float note) {
        Deck& dk = deck[safe(dref)];
        dk.note  = note;
        dk.flash = kFlashFrames;
        if (dk.mode == Mode::Reel) {
            // Reel uses an external exciter, so Rings' internal plucker is off: inject our own short
            // noise burst to excite the body on a trigger. SIZE (damping) sets how long it sustains.
            dk.exc_burst = dk.exc_burst_len = 0.004f * sr; // ~4 ms
        } else {
            dk.strum_pending = true;                       // Slice/Drift: Rings strums internally
        }
    }

    void advance_scheduler(DeckRef::Ref dref, size_t n) {
        Deck& dk = deck[safe(dref)];
        if (dk.mode == Mode::Drift) {
            dk.drift_timer -= static_cast<float>(n);
            if (dk.drift_timer <= 0.f) {
                const float interval = (0.03f + (1.f - dk.modspeed_n) * 0.5f) * sr; // 30..530 ms
                dk.drift_timer = interval * (0.5f + rand01(dk.rng));
                const float spread = dk.modamp_n * 24.f; // up to 2 octaves
                trigger(dref, base_note(dk) + (rand01(dk.rng) * 2.f - 1.f) * spread);
            }
        } else if (dk.mode == Mode::Slice) {
            if (dk.modspeed_n < 0.05f) return;     // arp off -> manual plucks only
            dk.arp_timer -= static_cast<float>(n);
            if (dk.arp_timer <= 0.f) {
                float bpm = transport ? transport->tempo() : 120.f;
                if (bpm < 20.f) bpm = 120.f;
                static const float kDiv[4] = { 1.f, 2.f, 4.f, 8.f };
                int di = static_cast<int>(dk.modspeed_n * 3.999f);
                di = di < 0 ? 0 : (di > 3 ? 3 : di);
                const float nps = (bpm / 60.f) * kDiv[di];
                dk.arp_timer = sr / (nps > 0.1f ? nps : 0.1f);
                static const int kArp[4] = { 0, 4, 7, 12 };
                trigger(dref, base_note(dk) + static_cast<float>(kArp[dk.arp_idx & 3]));
                dk.arp_idx++;
            }
        }
    }

    void process(const float* const* in, float** out, size_t size) {
        const size_t MB = rings::kMaxBlockSize; // 24
        float inbuf[rings::kMaxBlockSize], outbuf[rings::kMaxBlockSize], auxbuf[rings::kMaxBlockSize];

        for (int d = 0; d < DeckRef::Count; d++) {
            Deck& dk = deck[d];
            const float* din = in ? in[d] : nullptr;
            float lvl = 0.f;

            size_t off = 0;
            while (off < size) {
                size_t n = size - off;
                if (n > MB) n = MB;

                advance_scheduler(static_cast<DeckRef::Ref>(d), n);

                if (dk.mode == Mode::Reel) {
                    // Exciter = live input (play the body with audio) + a short triggered noise burst
                    // (so the pad/gate/MIDI actually plucks it). No unconditional drone: silent unless
                    // fed or triggered.
                    for (size_t i = 0; i < n; i++) {
                        float burst = 0.f;
                        if (dk.exc_burst > 0.f) {
                            // 2.0 gain: the external-exciter path is ~4x quieter than Rings' internal
                            // plucker, so match a Slice pluck's level. SoftClip catches any peak.
                            burst = (rand01(dk.rng) * 2.f - 1.f) * 2.0f * (dk.exc_burst / dk.exc_burst_len);
                            dk.exc_burst -= 1.f;
                        }
                        dk.reel_lp += 0.3f * (burst - dk.reel_lp); // soften the burst
                        const float live = din ? din[off + i] : 0.f;
                        inbuf[i] = live + dk.reel_lp;
                    }
                } else {
                    // Slice/Drift use Rings' INTERNAL exciter (internal_exciter = true): the pitched
                    // pluck is the only intended source. Rings still sums whatever is on `in` into the
                    // resonator (part.cc: external input is low-passed in, then the pluck is added on
                    // top), so feeding live input here layered a continuous UNPITCHED drone from the
                    // codec input over every pluck - audible on hardware (live ADC), silent on the host
                    // test (zero input). Feed silence: external input belongs to Reel only.
                    for (size_t i = 0; i < n; i++) inbuf[i] = 0.f;
                }

                dk.ps.note  = dk.note;
                dk.ps.strum = dk.strum_pending;
                dk.part.Process(dk.ps, dk.patch, inbuf, outbuf, auxbuf, n);
                dk.strum_pending = false;

                for (size_t i = 0; i < n; i++) {
                    const float dry = din ? din[off + i] : 0.f;
                    const float y   = dry * (1.f - dk.mix_n) + outbuf[i] * dk.mix_n;
                    out[d][off + i] = soft_clip(y);
                    const float a = y < 0.f ? -y : y;
                    if (a > lvl) lvl = a;
                }
                off += n;
            }
            dk.level += 0.3f * (lvl - dk.level);
        }
    }

    void set_param(ParamId id, DeckRef::Ref deckr, float v) {
        const auto d = safe(deckr);
        param_cache[static_cast<size_t>(id)][d] = v;
        Deck& dk = deck[d];
        switch (id) {
            case ParamId::Speed:  dk.pitch_n = v; dk.note = base_note(dk); break;
            case ParamId::Size:   dk.size_n   = v; apply(d); break;
            case ParamId::Pos:    dk.pos_n    = v; apply(d); break;
            case ParamId::Env:    dk.env_n    = v; apply(d); break;
            case ParamId::ModAmp: dk.modamp_n = v; apply(d); break;
            case ParamId::Mix:    dk.mix_n    = v;           break;
            case ParamId::Aux: {
                const int m = std::min(std::max(static_cast<int>(v * (kModels - 1) + 0.5f), 0), kModels - 1);
                dk.model = m; apply(d);
            } break;
            default: break;
        }
    }

    float param(ParamId id, DeckRef::Ref deckr) const {
        return param_cache[static_cast<size_t>(id)][safe(deckr)];
    }

    void set_aux_active(DeckRef::Ref deckr, bool held) { deck[safe(deckr)].aux_held = held; }

    void set_mod_speed(DeckRef::Ref deckr, float v) {
        const auto d = safe(deckr);
        deck[d].modspeed_n = clampf(v, 0.f, 1.f);
        param_cache[static_cast<size_t>(ParamId::ModSpeed)][d] = v;
    }

    bool set_config(ConfigId id, DeckRef::Ref deckr, int value) {
        if (id == ConfigId::Mode) {
            const Mode nm = value == 2 ? Mode::Drift : value == 1 ? Mode::Reel : Mode::Slice;
            const auto d = safe(deckr);
            if (deck[d].mode != nm) { deck[d].mode = nm; apply(d); return true; }
        }
        return false;
    }

    DeckRef::Ref handle_midi_note(uint8_t channel, uint8_t note) {
        const DeckRef::Ref d = (channel & 1) ? DeckRef::B : DeckRef::A;
        deck[d].pitch_n = clampf((static_cast<float>(note) - 24.f) / 60.f, 0.f, 1.f);
        param_cache[static_cast<size_t>(ParamId::Speed)][d] = deck[d].pitch_n;
        trigger(d, static_cast<float>(note) + deck[d].cv_semis);
        return d;
    }

    void cv_voct(DeckRef::Ref deckr, float value) {
        // V/Oct CV is an ADDITIVE transposition (CalibratedVOct returns semitones, 0 at 0V) on top of
        // the PITCH knob - it must NOT overwrite the knob. The previous version wrote pitch_n every
        // block (~500 Hz from read_cv), so it pinned pitch to the unpatched CV reading and the knob did
        // nothing. cv_semis is summed into the pluck note via base_note(); it does not touch pitch_n or
        // the param cache (which mirror the knob), and it does not rewrite dk.note (which would clobber
        // a trigger's arp/drift offset). With nothing patched, cv_semis == 0 and the knob alone rules.
        deck[safe(deckr)].cv_semis = value;
    }

    void trigger_here(DeckRef::Ref deckr) {
        const auto d = safe(deckr);
        trigger(d, base_note(deck[d]));
    }

    void render(DisplayModel& m) {
        m.clear();
        // Canonical Reel/Slice/Drift hues from the shared palette. reso carries its OWN Mode enum
        // (Reel=0/Slice=1/Drift=2), not the platform Mode, so it indexes pal:: directly rather than
        // going through pal::mode()/led::mode_leds() (which take spotykach::Mode).
        static const uint32_t kMode[3] = { pal::kReel, pal::kSlice, pal::kDrift };
        for (int d = 0; d < DeckRef::Count; d++) {
            Deck& dk = deck[d];
            const uint32_t col = kMode[static_cast<int>(dk.mode)];
            float level = dk.level * 1.5f;
            if (level > 1.f) level = 1.f;
            m.ring[d].set_hex_color(col);
            if (level > 1e-3f) m.ring[d].set_segment(0.f, level * 0.999f);
            if (dk.aux_held) {
                // Alt+PITCH model selector: all kModels options evenly spaced, the selected one bright.
                ring::selector(m.ring[d], kModels, dk.model, pal::kWhite, 0.12f);
            } else {
                ring::playhead(m.ring[d], dk.pitch_n, 1.f);   // pitch position dot
            }
            m.ring[d].set_updated();
            m.play[d] = { col, dk.flash > 0 ? 1.f : 0.12f };
            if (dk.flash > 0) dk.flash--;
        }
        const auto am = deck[0].mode;
        m.mode_center = { pal::kReel,  am == Mode::Reel  ? 1.f : 0.1f };
        m.mode_left   = { pal::kSlice, am == Mode::Slice ? 1.f : 0.1f };
        m.mode_right  = { pal::kDrift, am == Mode::Drift ? 1.f : 0.1f };
    }
};

// --- ResoEngine: thin forwarders to Impl (which lives in the SDRAM arena) ------------------------
void ResoEngine::init(const EngineContext& ctx)
{
    Arena ar(ctx.arena);
    void* mem = ar.alloc<uint8_t>(sizeof(Impl), alignof(Impl));
    _p = new (mem) Impl();
    _p->setup(ctx, ar);
}

void ResoEngine::process(const float* const* in, float** out, size_t size)
{
    if (_p) _p->process(in, out, size);
}

void  ResoEngine::set_param(ParamId id, DeckRef::Ref d, float v) { if (_p) _p->set_param(id, d, v); }
float ResoEngine::param(ParamId id, DeckRef::Ref d) const        { return _p ? _p->param(id, d) : 0.f; }
void  ResoEngine::set_mod_speed(DeckRef::Ref d, float v, bool)   { if (_p) _p->set_mod_speed(d, v); }
void  ResoEngine::set_aux_active(DeckRef::Ref d, bool held)      { if (_p) _p->set_aux_active(d, held); }
bool  ResoEngine::set_config(ConfigId id, DeckRef::Ref d, int v) { return _p ? _p->set_config(id, d, v) : false; }

DeckRef::Ref ResoEngine::handle_midi_note(uint8_t ch, uint8_t note)
{
    return _p ? _p->handle_midi_note(ch, note) : DeckRef::Count;
}
void ResoEngine::cv_voct(DeckRef::Ref d, float v)        { if (_p) _p->cv_voct(d, v); }
void ResoEngine::on_gate_trigger(DeckRef::Ref d)         { if (_p) _p->trigger_here(d); }
bool ResoEngine::on_play_pad(DeckRef::Ref d, bool)       { if (_p) _p->trigger_here(d); return false; }
void ResoEngine::on_seq_trigger(DeckRef::Ref d)          { if (_p) _p->trigger_here(d); }
void ResoEngine::render(DisplayModel& m)                 { if (_p) _p->render(m); }
