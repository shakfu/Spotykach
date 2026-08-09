// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

// A tempo-synced stereo delay with selectable CHARACTER and stereo TOPOLOGY - the second engine to
// consume the platform transport (it reads tempo; it does not subscribe to ticks).
//
//   SIZE  -> musical division (tempo-locked)     POS   -> feedback
//   SOS   -> wet/dry mix                         PITCH -> output transpose (+/-1 octave, centre = unity)
//   ENV   -> feedback tone (a one-pole low-pass in the loop: up = open/clean, down = darker repeats)
//   Reel/Slice/Drift switch (ConfigId::Mode, per deck) -> character: Clean / Tape / Shimmer
//   Route switch (ConfigId::Route)                     -> topology: Stereo / DoubleMono / Ping-pong
//
// Character (changes only the feedback path; knob meanings stay fixed across modes):
//   Clean   - clean digital repeats (identical to the original delay when ENV is up).
//   Tape    - a wow/flutter LFO on the read time + the tone low-pass + soft saturation: warbly,
//             degrading dub/analog repeats.
//   Shimmer - the feedback is pitch-shifted +12 each pass, so repeats climb into an octave wash.
// Topology:
//   DoubleMono - two independent mono delays (deck A -> L with its own controls, deck B -> R with its).
//   Stereo     - linked: both delays share deck A's controls (a coherent stereo delay; deck B inert).
//   Ping-pong  - linked + cross-feedback: each deck's colored feedback feeds the OTHER, so echoes bounce.
//
// The platform clock is injected read-only via EngineContext (ITransport). capabilities() =
// CapOwnDisplay | CapDualDeck. Each tap's delay line is borrowed SDRAM (arena-sub-allocated, ~6 s).
class DelayEngine : public IEngine {
public:
    enum Mode : uint8_t { Clean = 0, Tape, Shimmer, ModeCount };

    DelayEngine() = default;
    ~DelayEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override {}
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override { return CapOwnDisplay | CapDualDeck; }

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md). set_param stores every id blindly
    // into _param[], so without these the descriptor would advertise all 24 and a host sweep would
    // "pass" on ids this engine never reads - asserting nothing. These are the ids process() consumes:
    // POS=feedback, ENV=feedback tone, SIZE=division, SPEED, MIX, MODAMP=mod depth.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::Env))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::ModAmp));
    }
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>((1u << static_cast<uint32_t>(ConfigId::Route))
                                     | (1u << static_cast<uint32_t>(ConfigId::Mode)));
    }
    // Layer-3 names for `describe` (docs/dev/terminal-osc.md). The address stays the stable layer-2
    // slot, so one control-surface layout still binds to every build; only the printed name changes.
    // A tempo-divided delay: none of these slots mean what granular's vocabulary calls them.
    const char* param_label(ParamId id) const override {
        switch (id) {
            case ParamId::Pos:     return "feedback";
            case ParamId::Size:    return "division";
            case ParamId::Speed:   return "pitch";
            case ParamId::Env:     return "tone";
            case ParamId::ModAmp:  return "mod depth";
            default: return nullptr;   // fall back to the layer-2 slot name
        }
    }

#endif

    void  set_param(ParamId id, DeckRef::Ref deck, float value) override;
    float param(ParamId id, DeckRef::Ref deck) const override;
    void  set_mod_speed(DeckRef::Ref deck, float value, bool sync) override; // MODFREQ -> mod LFO rate
    bool  on_play_pad(DeckRef::Ref deck, bool reverse) override;             // Play pad -> Freeze toggle
    bool  set_config(ConfigId id, DeckRef::Ref deck, int value) override; // Mode -> character; Route -> topology
    Route route() const override { return _route; }                       // report topology for the route LED
    void  render(DisplayModel& m) override;

private:
    NOCOPY(DelayEngine)

    // Crossfading two-head delay-line pitch shifter. Reused for the PITCH output transpose and the
    // Shimmer feedback shift: two read heads half a window apart, raised-cosine crossfaded so the
    // wraparound is seamless; transparent at unity ratio. Read rate = `ratio` (output transposed by it).
    struct Shifter {
        static constexpr size_t kWin = 2048;
        float  buf[kWin] = {};
        size_t w = 0;
        float  phase = 0.f;
        float  process(float x, float ratio);
        float  read(float off) const;
        void   clear() { for (size_t i = 0; i < kWin; i++) buf[i] = 0.f; w = 0; phase = 0.f; }
    };

    // One delay line + its smoothed controls. read_color()/write_out() are split (rather than one
    // process()) so Ping-pong can compute both taps' feedback, then write each from the OTHER's.
    struct Tap {
        float*  buf = nullptr;
        size_t  len = 0, w = 0;
        float   sr = 48000.f, min_d = 1.f, max_d = 1.f;
        int     div = 0;
        float   beats = 0.25f, target_d = 1.f;
        float   fb = 0.f, mix = 0.f, ratio = 1.f, tone_coef = 1.f; // targets (set per block from _param)
        uint8_t mode = Clean;
        float   s_delay = 1.f, s_fb = 0.f, s_mix = 0.f, s_ratio = 1.f; // per-sample smoothed
        float   tone_lp = 0.f;   // one-pole state for the feedback tone filter
        float   mod_ph = 0.f, mod_rate = 0.f, mod_depth = 0.f; // delay-time mod LFO (MODFREQ/MOD_AMT; Tape adds a floor)
        bool    frozen = false;  // Play-pad freeze: loop the buffer at unity feedback, ignoring new input
        bool    reversed = false; // Rev-pad: read the buffer backwards over a delay-length window
        float   rev_ph = 0.f;    // reverse-read phase (0..window), two heads crossfaded at the wrap
        float   peak = 0.f;
        float   _x = 0.f, _wet = 0.f; // stashed input + read wet (for the read/write split)
        Shifter out_shift;       // PITCH output transpose
        Shifter fb_shift;        // Shimmer feedback shift (+12)

        void  init(void* mem, float sample_rate, size_t length);
        void  set_div(float norm);   // SIZE 0..1 -> musical-division index + beats
        void  set_tone(float env);   // ENV 0..1 -> feedback low-pass coefficient
        void  set_target(float bpm); // recompute target_d from bpm (per block)
        float read_buf(float off) const; // linear-interpolated read `off` samples behind the write head
        float read_rev(float win);   // crossfaded backward read over a `win`-sample window (reverse delay)
        float read_color(float x_in); // smooth + read the tap + colorize -> the feedback signal (pre *fb)
        float write_out(float fbsig); // write x + fb*fbsig, return the wet/dry-mixed (PITCH-transposed) output
    };

    static DeckRef::Ref _safe(DeckRef::Ref d) { return d < DeckRef::Count ? d : DeckRef::A; }
    void apply_params(Tap& t, DeckRef::Ref src); // derive a tap's targets from _param[*][src] + _mode[src]

    const ITransport* _transport = nullptr;
    Tap     _tap[DeckRef::Count];
    uint8_t _mode[DeckRef::Count]  = { Clean, Clean }; // per-deck character (ConfigId::Mode)
    float   _mod_rate[DeckRef::Count] = { 0.f, 0.f };  // per-deck mod LFO rate in Hz (MODFREQ)
    bool    _freeze[DeckRef::Count]   = { false, false }; // per-deck Freeze (Play pad)
    bool    _reverse[DeckRef::Count]  = { false, false }; // per-deck Reverse (Rev pad)
    Route   _route = Route::Stereo;                    // ConfigId::Route topology
    float   _param[static_cast<size_t>(ParamId::Count)][DeckRef::Count] = {};
};

};
