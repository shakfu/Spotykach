// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "engine/mosc/mosc_engine.h"
#include "engine/indicators.h"   // shared indicator toolkit (docs/dev/indicator-comparison.md §7)

#include "engine/arena.h"

#include "plaits/dsp/dsp.h"
#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"

#include <new>
#include <algorithm>

using namespace spotykach;

namespace {

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

} // namespace

// All Plaits/stmlib types are confined to this Impl (see the PIMPL note in the header).
struct MoscEngine::Impl {
    static constexpr int     kEngines      = plaits::kMaxEngines;  // 24
    static constexpr size_t  kScratchBytes = 16384;                // per-voice shared engine RAM (plaits.cc)
    static constexpr size_t  kBlock        = plaits::kBlockSize;   // 12: Plaits' native render block
    static constexpr uint8_t kFlashFrames  = 6;
    // Spare-CV modulation depth: how far a full-scale CV jack reading swings the destination param.
    // Tunable; 1.0 maps a unit of (calibrated, signed) CV to the full 0..1 parameter range.
    static constexpr float   kHarmCvDepth   = 1.0f;  // CV_SIZE_POS -> harmonics
    static constexpr float   kTimbreModAmt  = 1.0f;  // CV_MIX -> timbre (Plaits attenuverter amount)

    // Trigger behaviour selected by the Mode switch. Gate fires the LPG/decay envelope on each
    // pad/gate/MIDI strike (percussive/plucked notes); Drone bypasses the LPG so the engine runs open.
    enum class Mode : uint8_t { Gate = 0, Drone = 1 };

    struct Deck {
        plaits::Voice       voice;
        plaits::Patch       patch{};
        plaits::Modulations mod{};
        Mode  mode   = Mode::Gate;
        int   engine = 8;  // 8 = virtual_analog_engine (a musical default; see Voice::Init order)

        // Normalized [0,1] knob state.
        float pitch_n = 0.5f, harm_n = 0.5f, timbre_n = 0.5f, morph_n = 0.5f,
              decay_n = 0.5f, color_n = 0.5f, level_n = 1.f;
        float cv_semis  = 0.f;  // V/Oct CV transposition in semitones (0 when nothing patched)
        float cv_harm   = 0.f;  // CV_SIZE_POS jack -> harmonics modulation (signed, ~0 unpatched)
        float cv_timbre = 0.f;  // CV_MIX jack -> timbre modulation (signed, ~0 unpatched)

        int     trig_blocks = 0;  // >0: hold mod.trigger high for that many render blocks (Gate)
        float   level = 0.f;      // smoothed output level (display)
        uint8_t flash = 0;
        bool    aux_held = false;  // platform: Alt+PITCH engine-select is held -> draw the selector
    };

    float sr = 48000.f;
    Deck  deck[DeckRef::Count];
    Route _route = Route::Stereo;   // global audio routing (the Mode A/B/C switch -> ConfigId::Route)
    float param_cache[static_cast<size_t>(ParamId::Count)][DeckRef::Count] = {};

    static DeckRef::Ref safe(DeckRef::Ref d) { return d < DeckRef::Count ? d : DeckRef::A; }
    static float pitch_to_note(float n) { return 24.f + clampf(n, 0.f, 1.f) * 60.f; } // MIDI C1..C6
    // The deck's current base note: PITCH knob plus the V/Oct CV transposition.
    static float base_note(const Deck& dk) { return pitch_to_note(dk.pitch_n) + dk.cv_semis; }

    void setup(const EngineContext& ctx, Arena& ar) {
        sr = ctx.sample_rate;
        for (int d = 0; d < DeckRef::Count; d++) {
            Deck& dk = deck[d];
            // Each Voice gets its own persistent 16 KB region: Voice::Init resets the allocator between
            // engines, so all 24 engines for THIS deck share this block (only the active one is live).
            uint8_t* scratch = ar.alloc<uint8_t>(kScratchBytes, 16);
            stmlib::BufferAllocator alloc(scratch, kScratchBytes);
            dk.voice.Init(&alloc);
            apply(static_cast<DeckRef::Ref>(d));
        }
        using PI = ParamId;
        auto P = [](PI id) { return static_cast<size_t>(id); };
        for (int d = 0; d < DeckRef::Count; d++) {
            const Deck& dk = deck[d];
            param_cache[P(PI::Speed)][d]    = dk.pitch_n;
            param_cache[P(PI::Size)][d]     = dk.harm_n;
            param_cache[P(PI::Pos)][d]      = dk.timbre_n;
            param_cache[P(PI::Env)][d]      = dk.morph_n;
            param_cache[P(PI::ModAmp)][d]   = dk.decay_n;
            param_cache[P(PI::ModSpeed)][d] = dk.color_n;
            param_cache[P(PI::Mix)][d]      = dk.level_n;
            param_cache[P(PI::Aux)][d]      = static_cast<float>(dk.engine) / (kEngines - 1);
        }
    }

    void apply(DeckRef::Ref dref) {
        Deck& dk = deck[safe(dref)];
        dk.patch.harmonics  = clampf(dk.harm_n,   0.f, 1.f);
        dk.patch.timbre     = clampf(dk.timbre_n, 0.f, 1.f);
        dk.patch.morph      = clampf(dk.morph_n,  0.f, 1.f);
        dk.patch.decay      = clampf(dk.decay_n,  0.f, 1.f);
        dk.patch.lpg_colour = clampf(dk.color_n,  0.f, 1.f);
        dk.patch.frequency_modulation_amount = 0.f;
        dk.patch.timbre_modulation_amount    = kTimbreModAmt; // CV_MIX -> timbre attenuator depth
        dk.patch.morph_modulation_amount     = 0.f;
        // Voice::Render feeds patch.engine to HysteresisQuantizer2 as the INTEGER base (engine_cv adds
        // a continuous offset, which we leave at 0), so an int-valued engine index selects directly.
        dk.patch.engine = static_cast<float>(dk.engine);
    }

    void trigger(DeckRef::Ref dref) {
        Deck& dk = deck[safe(dref)];
        dk.flash = kFlashFrames;
        // Hold the trigger high a few render blocks so Voice sees a clean rising-then-falling edge
        // (its trigger_delay_ is a per-block shift register). Drone mode ignores the trigger entirely.
        dk.trig_blocks = 3;
    }

    // Per-block (control-rate) setup of a deck's modulation + base note. Constant across the kBlock
    // chunks within one process() call; the trigger value is the only thing advanced per chunk.
    void setup_block(Deck& dk) {
        const bool gate = (dk.mode == Mode::Gate);
        // Spare CV jacks -> Plaits modulation. CV_SIZE_POS adds to harmonics (summed directly, no
        // patched flag); CV_MIX drives timbre (needs timbre_patched + the attenuverter set in apply()).
        // Both read ~0 when nothing is patched, so the knobs rule until a cable is in.
        dk.mod.engine = dk.mod.note = dk.mod.frequency = dk.mod.morph = 0.f;
        dk.mod.level = 1.f;
        dk.mod.harmonics = clampf(dk.cv_harm * kHarmCvDepth, -1.f, 1.f);
        dk.mod.timbre    = clampf(dk.cv_timbre, -1.f, 1.f);
        dk.mod.frequency_patched = false;
        dk.mod.timbre_patched    = true;  // route CV_MIX through the timbre attenuator
        dk.mod.morph_patched = dk.mod.level_patched = false;
        dk.mod.trigger_patched = gate; // Gate: LPG/decay env fires on trigger; Drone: open
        dk.patch.note = base_note(dk);
    }

    void process(const float* const* in, float** out, size_t size) {
        (void) in; // mosc is a pure generator: codec input is unused
        plaits::Voice::Frame fr[DeckRef::Count][kBlock];
        float gain[DeckRef::Count], lvl[DeckRef::Count] = { 0.f, 0.f };

        for (int d = 0; d < DeckRef::Count; d++) {
            gain[d] = clampf(deck[d].level_n, 0.f, 1.f) * (1.f / 32768.f);
            setup_block(deck[d]);
        }

        size_t off = 0;
        while (off < size) {
            size_t n = size - off;
            if (n > kBlock) n = kBlock;

            // Render BOTH voices for this chunk before mixing, so the routing modes that cross the two
            // decks (DoubleMono sum, GenerativeStereo out/aux spread) have both signals on hand.
            for (int d = 0; d < DeckRef::Count; d++) {
                Deck& dk = deck[d];
                if (dk.mode == Mode::Gate) {
                    dk.mod.trigger = dk.trig_blocks > 0 ? 1.f : 0.f;
                    if (dk.trig_blocks > 0) dk.trig_blocks--;
                } else {
                    dk.mod.trigger = 0.f;
                }
                dk.voice.Render(dk.patch, dk.mod, fr[d], n);
            }

            const float gA = gain[0], gB = gain[1];
            for (size_t i = 0; i < n; i++) {
                const float aOut = static_cast<float>(fr[0][i].out) * gA;
                const float bOut = static_cast<float>(fr[1][i].out) * gB;
                float l, r;
                switch (_route) {
                    case Route::DoubleMono:        // both voices summed to a centred mono image
                        l = r = (aOut + bOut) * 0.5f;
                        break;
                    case Route::GenerativeStereo:  // out/aux spread: each channel = one voice's main +
                                                   // the other's aux (Plaits' secondary output) -> width
                        l = (aOut + static_cast<float>(fr[1][i].aux) * gB) * 0.5f;
                        r = (bOut + static_cast<float>(fr[0][i].aux) * gA) * 0.5f;
                        break;
                    default:                       // Route::Stereo: deck A -> L, deck B -> R
                        l = aOut; r = bOut;
                        break;
                }
                out[0][off + i] = l;
                out[1][off + i] = r;
                // Meter each deck off its own voice (not the mixed channel) so the rings stay per-deck.
                const float am = aOut < 0.f ? -aOut : aOut; if (am > lvl[0]) lvl[0] = am;
                const float bm = bOut < 0.f ? -bOut : bOut; if (bm > lvl[1]) lvl[1] = bm;
            }
            off += n;
        }

        for (int d = 0; d < DeckRef::Count; d++)
            deck[d].level += 0.3f * (lvl[d] - deck[d].level);
    }

    void set_param(ParamId id, DeckRef::Ref deckr, float v) {
        const auto d = safe(deckr);
        param_cache[static_cast<size_t>(id)][d] = v;
        Deck& dk = deck[d];
        switch (id) {
            case ParamId::Speed:  dk.pitch_n  = v;            break; // note: applied live in process()
            case ParamId::Size:   dk.harm_n   = v; apply(d);  break;
            case ParamId::Pos:    dk.timbre_n = v; apply(d);  break;
            case ParamId::Env:    dk.morph_n  = v; apply(d);  break;
            case ParamId::ModAmp: dk.decay_n  = v; apply(d);  break;
            case ParamId::ModSpeed: dk.color_n = v; apply(d); break;
            case ParamId::Mix:    dk.level_n  = v;            break;
            case ParamId::Aux: {
                const int e = std::min(std::max(static_cast<int>(v * (kEngines - 1) + 0.5f), 0), kEngines - 1);
                dk.engine = e; apply(d);
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
        deck[d].color_n = clampf(v, 0.f, 1.f);
        param_cache[static_cast<size_t>(ParamId::ModSpeed)][d] = v;
        apply(d);
    }

    bool set_config(ConfigId id, DeckRef::Ref deckr, int value) {
        if (id == ConfigId::Route) {   // global: how the two voices reach the L/R outputs
            _route = value == 2 ? Route::GenerativeStereo
                   : value == 1 ? Route::DoubleMono
                                : Route::Stereo;
            return false;
        }
        if (id == ConfigId::Mode) {    // per-deck: Gate vs Drone trigger behaviour
            const Mode nm = value == 0 ? Mode::Gate : Mode::Drone;
            const auto d = safe(deckr);
            if (deck[d].mode != nm) { deck[d].mode = nm; return true; }
        }
        return false;
    }

    Route route() const { return _route; }

    DeckRef::Ref handle_midi_note(uint8_t channel, uint8_t note) {
        const DeckRef::Ref d = (channel & 1) ? DeckRef::B : DeckRef::A;
        deck[d].pitch_n = clampf((static_cast<float>(note) - 24.f) / 60.f, 0.f, 1.f);
        param_cache[static_cast<size_t>(ParamId::Speed)][d] = deck[d].pitch_n;
        trigger(d);
        return d;
    }

    // V/Oct CV is an ADDITIVE transposition (semitones, 0 at 0V) summed into the note via base_note();
    // it must not overwrite the PITCH knob. With nothing patched, cv_semis == 0 and the knob alone rules.
    void cv_voct(DeckRef::Ref deckr, float value) { deck[safe(deckr)].cv_semis = value; }

    // Spare per-deck CV jacks. The platform delivers calibrated, signed readings (~0 unpatched); they
    // feed Plaits' harmonics/timbre modulation in process(). See the CV map note in the header.
    void cv_mix(DeckRef::Ref deckr, float value)      { deck[safe(deckr)].cv_timbre = value; }
    void cv_size_pos(DeckRef::Ref deckr, float value) { deck[safe(deckr)].cv_harm   = value; }

    void render(DisplayModel& m) {
        m.clear();
        static const uint32_t kColor[2] = { 0x00ffaa /*Gate*/, 0xff8800 /*Drone*/ };
        for (int d = 0; d < DeckRef::Count; d++) {
            Deck& dk = deck[d];
            const uint32_t col = kColor[static_cast<int>(dk.mode)];
            float level = dk.level * 1.5f;
            if (level > 1.f) level = 1.f;
            m.ring[d].set_hex_color(col);
            if (level > 1e-3f) m.ring[d].set_segment(0.f, level * 0.999f);
            if (dk.aux_held) {
                // Alt+PITCH engine selector: all kEngines models around the ring, current pick bright.
                ring::selector(m.ring[d], kEngines, dk.engine, pal::kWhite);
            } else {
                ring::playhead(m.ring[d], dk.pitch_n, 1.f);   // pitch position dot
            }
            m.ring[d].set_updated();
            m.play[d] = { col, dk.flash > 0 ? 1.f : 0.12f };
            if (dk.flash > 0) dk.flash--;
        }
        // The center/left/right LEDs sit under the global routing switch -> show the active Route via the
        // shared helper (per-deck Gate/Drone is shown by the deck ring + play colour above).
        led::route_leds(m, _route);
    }
};

// --- MoscEngine: thin forwarders to Impl (which lives in the SDRAM arena) ------------------------
void MoscEngine::init(const EngineContext& ctx)
{
    Arena ar(ctx.arena);
    void* mem = ar.alloc<uint8_t>(sizeof(Impl), alignof(Impl));
    _p = new (mem) Impl();
    _p->setup(ctx, ar);
}

void MoscEngine::process(const float* const* in, float** out, size_t size)
{
    if (_p) _p->process(in, out, size);
}

void  MoscEngine::set_param(ParamId id, DeckRef::Ref d, float v) { if (_p) _p->set_param(id, d, v); }
float MoscEngine::param(ParamId id, DeckRef::Ref d) const        { return _p ? _p->param(id, d) : 0.f; }
void  MoscEngine::set_mod_speed(DeckRef::Ref d, float v, bool)   { if (_p) _p->set_mod_speed(d, v); }
void  MoscEngine::set_aux_active(DeckRef::Ref d, bool held)      { if (_p) _p->set_aux_active(d, held); }
bool  MoscEngine::set_config(ConfigId id, DeckRef::Ref d, int v) { return _p ? _p->set_config(id, d, v) : false; }
Route MoscEngine::route() const                                  { return _p ? _p->route() : Route::Stereo; }

DeckRef::Ref MoscEngine::handle_midi_note(uint8_t ch, uint8_t note)
{
    return _p ? _p->handle_midi_note(ch, note) : DeckRef::Count;
}
void MoscEngine::cv_mix(DeckRef::Ref d, float v)         { if (_p) _p->cv_mix(d, v); }
void MoscEngine::cv_size_pos(DeckRef::Ref d, float v)    { if (_p) _p->cv_size_pos(d, v); }
void MoscEngine::cv_voct(DeckRef::Ref d, float v)        { if (_p) _p->cv_voct(d, v); }
void MoscEngine::on_gate_trigger(DeckRef::Ref d)         { if (_p) _p->trigger(d); }
bool MoscEngine::on_play_pad(DeckRef::Ref d, bool)       { if (_p) _p->trigger(d); return false; }
void MoscEngine::on_seq_trigger(DeckRef::Ref d)          { if (_p) _p->trigger(d); }
void MoscEngine::render(DisplayModel& m)                 { if (_p) _p->render(m); }
