// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "engine/istreamdeck.h"
#include "engine/pstretch/pstretch_voice.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

// pstretch - a real-time, clean-room PaulStretch ambient time-smear. Each deck (A/B) runs one mono
// Stretcher over its input channel (A = left, B = right, like the delay engine); the two are blended by
// the crossfader and placed by the routing switch. PaulStretch turns audio into a diffuse, evolving
// spectral wash by FFT-ing large overlapping windows and randomizing the phases; here the analysis head
// crawls through (or freezes on) a multi-second ring of live input, so big stretch settings smear the
// recent past into an ambient drone.
//
// Per-deck control map:
//   SIZE  -> STRETCH amount (1x .. 64x, exponential) - how slowly the read head crawls.
//   POS   -> DIFFUSION / smear (0 = clean window resynthesis .. 1 = full phase-randomized wash).
//   PITCH -> pitch shift of the grain (+/- 1 octave).
//   ENV   -> output tone (one-pole low-pass; ambient softening).
//   MIX   -> dry/wet.
//   Mode switch (per deck) -> SOURCE select. The 3-position toggle is silkscreened (top/middle/bottom)
//               Reel/Slice/Drift (legacy granular labels); ConfigId::Mode values 0=Slice, 1=Reel, 2=Drift
//               map to: Slice(0) = Live (smear the recent input), Reel(1) = Capture (grab the recent ring
//               and loop the stretch *through* it), Drift(2) = SD-file (stream a clip from /pstretch).
//   Play pad -> FREEZE the read head (infinite evolving drone on the current spot) - works in any source.
//   Rev pad  -> in Capture mode, RE-GRAB the recent ring at the current instant (the switch position
//               itself grabs on entry; the pad refreshes it). No-op in Live/SD.
//   Alt+PITCH (Aux) -> CLIP select: pick which clip in /pstretch the SD source plays (takes effect live if
//               that deck is streaming, else on the next switch to SD). CapAux; held = show the selector.
//   Alt+POS (AltPos) -> SCRUB (SD only): seek the stream playhead to a position in the clip (debounced, so a
//               sweep opens the spot you land on; freeze + scrub auditions frozen spots). CapAltPos.
//   Cycle (ModSpeed) / Glow (ModAmp) -> per-deck mod LFO rate / depth (depth 0 = off). The Size/Pos mod
//               switch (StartModOn/SizeModOn) picks the target - Pos=diffusion, Size=stretch, both=tone
//               (pitch is modulated via the V/Oct CV jack); Mod Type (ModType/LfoShape) picks sine/triangle
//               or Follow (input follower). Alt+Cycle (set_mod_speed sync) locks the LFO rate to the tempo.
//   CV in -> additive on top of the knobs: V/Oct=pitch (cv_voct), Size/Pos=stretch (cv_size_pos),
//               Mix=dry/wet (cv_mix), Crossfade=A/B blend (cv_crossfade); ~0 when unpatched so the knob rules.
//   Gate in (on_gate_trigger) -> re-grab in Capture, else toggle freeze (rhythmic re-sampling / stutter).
//   CV/gate out -> Mod CV out (process_cv) emits each deck's LFO as a 0..1 CV; gate out (gate_out_triggered)
//               pulses on every LFO cycle (a tempo-synced clock when Alt+Cycle-synced). The LFO free-runs
//               regardless of depth, so pstretch is usable as a plain LFO/clock source.
//   Mix fader (Crossfade) -> A/B blend.   Routing switch -> stereo topology.
//
// SD-file source (Phase 2): the analysis head consumes the source at input_rate/stretch (~1 KB/s at 50x),
// so a clip is STREAMED slowly into the per-voice ring (kept a margin ahead of the head) rather than
// RAM-loaded - an arbitrarily long file plays for hours. It reuses the platform StreamDeck stack (the
// radio engine's template), gated by SPK_USE_STREAM; the engine pulls source frames from `ctx.stream`
// each block and feeds them to the voice. The source is orthogonal to the stretch: live/capture/SD all
// share the same FFT, window, phase-smear, freeze, and every knob.
//
// Self-contained DSP: a vendored radix-2 FFT (engine/pstretch/fft.h), no CMSIS-DSP. The per-voice rings
// (~0.5 MB each) and the shared FFT/window scratch are sub-allocated from the injected SDRAM arena (the
// StreamDeck's own SD rings are separate platform SDRAM, injected by the app).
class PstretchEngine : public IEngine {
public:
    PstretchEngine() = default;
    ~PstretchEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override;          // main-loop: scan the SD clip folder once (FatFs, not ISR-safe)
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override { return CapOwnDisplay | CapDualDeck | CapAux | CapAltPos; }

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md): the ids this engine actually
    // consumes, so a host sweep exercises only real parameters instead of the whole ParamId enum.
    // Derived from the engine's own ParamId/ConfigId use; NOT yet verified on hardware.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::ModAmp))
             | (1u << static_cast<uint32_t>(ParamId::AltPos))
             | (1u << static_cast<uint32_t>(ParamId::Aux))
             | (1u << static_cast<uint32_t>(ParamId::Crossfade))
             | (1u << static_cast<uint32_t>(ParamId::Env));
    }
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>((1u << static_cast<uint32_t>(ConfigId::Route))
                                     | (1u << static_cast<uint32_t>(ConfigId::Mode))
                                     | (1u << static_cast<uint32_t>(ConfigId::ModType))
                                     | (1u << static_cast<uint32_t>(ConfigId::LfoShape))
                                     | (1u << static_cast<uint32_t>(ConfigId::StartModOn))
                                     | (1u << static_cast<uint32_t>(ConfigId::SizeModOn)));
    }
    // Layer-3 names for `describe` (docs/dev/terminal-osc.md). The address stays the stable layer-2
    // slot, so one control-surface layout still binds to every build; only the printed name changes.
    // Real-time PaulStretch. SIZE is the stretch factor (up to 64x), not a size.
    const char* param_label(ParamId id) const override {
        switch (id) {
            case ParamId::Size:    return "stretch";
            case ParamId::Pos:     return "diffusion";
            case ParamId::Speed:   return "pitch";
            case ParamId::Env:     return "tone";
            case ParamId::Mix:     return "wet";
            case ParamId::ModAmp:  return "glow";
            case ParamId::Aux:     return "clip";
            case ParamId::AltPos:  return "scrub";
            default: return nullptr;   // fall back to the layer-2 slot name
        }
    }

#endif

    void  set_param(ParamId id, DeckRef::Ref d, float v) override;
    float param(ParamId id, DeckRef::Ref d) const override;
    void  set_aux_active(DeckRef::Ref d, bool active) override; // Alt held -> show the clip selector
    bool  set_config(ConfigId id, DeckRef::Ref d, int value) override;
    Route route() const override { return _route; }

    // Modulation: a per-deck LFO (or input follower) on the Cycle/Glow knobs, routed to a selectable target.
    void  set_mod_speed(DeckRef::Ref d, float value, bool sync) override; // Cycle (MODFREQ) -> LFO rate

    // CV inputs (additive, on top of the knobs; ~0 when unpatched so the knob alone rules).
    void  cv_voct(DeckRef::Ref d, float value) override;      // V/Oct  CV -> pitch  (calibrated semitones)
    void  cv_size_pos(DeckRef::Ref d, float value) override;  // Size/Pos CV -> stretch (normalized offset)
    void  cv_mix(DeckRef::Ref d, float value) override;       // Mix    CV -> dry/wet (normalized offset)
    void  cv_crossfade(float value) override;                 // Crossfade CV -> A/B blend (normalized offset)

    // Gate in: re-grab in Capture, else toggle freeze - rhythmic re-sampling / stutter from a clock.
    void  on_gate_trigger(DeckRef::Ref d) override;

    // CV / gate OUT: emit each deck's LFO as a 0..1 CV (Mod CV out; also pulses the Cycle LED), and fire a
    // gate on every LFO cycle - a tempo-synced clock out when the LFO is clock-synced (Alt+Cycle).
    void  process_cv(float* cv0, float* cv1, size_t n) override;
    bool  gate_out_triggered(DeckRef::Ref d) override;

    DeckLeds deck_leds(DeckRef::Ref d) override;                 // Cycle-LED sync/follow indicator

    bool  on_play_pad(DeckRef::Ref d, bool reverse) override;   // Play = freeze; Rev = re-grab (Capture)

    void  render(DisplayModel& m) override;

private:
    NOCOPY(PstretchEngine)

    // FFT / analysis window. Build-time selectable (PSTRETCH_WINDOW=4096 for a lighter, snappier smear;
    // any 2^k the vendored FFT supports). 8192 is the default (a lusher, smoother wash): finer frequency
    // resolution but coarser time resolution, and ~2x the on-chip SRAM working set - which is why ola/fifo
    // move to the SDRAM arena at >= 8192 (see the buffer-placement note below). 8192 is flashed on the H7
    // (2026-07-01) and runs clean by ear; a formal METER CPU number is still pending. The 4096 build is the
    // meter-verified one (avg ~32% / max ~64%).
#ifndef PSTRETCH_WINDOW
#define PSTRETCH_WINDOW 8192
#endif
    static constexpr int      kWindow     = PSTRETCH_WINDOW;   // ~85 ms at 4096, ~171 ms at 8192
    static constexpr int      kHop        = kWindow / 2; // 50% overlap
    static constexpr uint32_t kRing       = 1u << 18;    // 262144 = ~5.46 s (live history / capture span)
    static constexpr size_t   kMaxFrames  = 128;
    static constexpr float    kCenterGain = 0.70710678f;
    // Pipelined-worker budget: max work TICKS (kChunk samples/bins or kChunk FFT butterflies each) the two
    // voices may do PER BLOCK, shared. A 4096 hop is ~126 ticks; its output lasts ~21 blocks, so two voices
    // need ~12 ticks/block - budget a little above that, low enough that one block never overruns. Meter-tuned
    // at 4096 (=14). At 8192 both the per-hop tick count and the number of output blocks grow, so the needed
    // per-block budget rises only slightly (~ with log2(N)); 16 keeps a similar margin. The 8192 value was a
    // derived starting point that plays clean by ear on hardware; retune with METER if audio ever underruns.
    static constexpr int      kWorkBudget = kWindow >= 8192 ? 16 : 14;

    // Per-deck source: what fills the analysis ring (orthogonal to the stretch). Selected by the Mode switch.
    enum class Source { Live, Capture, SD };

    // What the per-deck mod LFO (Cycle/Glow) drives. Chosen by the Size/Pos mod-routing switch:
    // Pos-only -> Diffusion, Size-only -> Stretch, both -> Tone. (Diffusion and Tone have no CV jack, so the
    // LFO is their only modulation route; Pitch and Stretch can also be modulated by their CV jacks.)
    enum class ModTarget { Diffusion, Stretch, Tone };

    // Fold the base knob values + CV offsets + LFO/follower modulation into the voice for deck i this block,
    // and return the effective dry/wet for the mix. `in` is the deck's live input (for the follower).
    float _derive_and_push(int i, const float* in, size_t n);
    void  _update_mod_target(int i);     // re-derive the LFO target from the Size/Pos mod-switch bits
    void  _recompute_xfade();            // derive _gA/_gB from the crossfade knob + its CV offset

    void _apply(DeckRef::Ref d);         // push cached params into a voice
    void _roll_random_pans();
    void _set_source(int i, Source s);   // switch deck i's source (exit the old, enter the new)
    void _open_sd(int i);                // arm SD on deck i (voice into SD mode + open the selected clip)
    void _open_clip(int i, uint32_t start_frame = 0);   // (re)open the deck's clip from /pstretch, looping
    void _ensure_scan();                 // scan /pstretch into _clips once (main-loop / FatFs)
    void _apply_scrub();                 // settle Alt+POS scrub re-seeks (main-loop / FatFs)
    void _build_path(char* dst, const char* name) const;   // "pstretch/<name>" into dst (kPathMax)

    static constexpr int kMaxClips = 32;            // clip-folder scan capacity
    static constexpr int kPathMax  = 32;            // "pstretch/" + 8.3 name + NUL
    static constexpr const char* kClipDir = "pstretch";   // SD folder scanned for source clips
    static constexpr uint32_t kScrubSettleMs = 60;  // debounce: re-seek only after Alt+POS stops moving
    static constexpr float    kScrubStep     = 0.01f; // ignore scrub moves smaller than 1% of the clip

    pstretch::FFT    _fft;
    pstretch::Shared _shared;
    pstretch::Stretcher _voice[2];

    // The FFT working set lives in on-chip SRAM (engine .bss), NOT the SDRAM arena: the transform's
    // scattered access to these (~128 KB total) dominates its cost, and scattered SDRAM access on the H7
    // is ~10x slower. Only the big per-voice input ring (~1 MB each) stays in the SDRAM arena - it is read
    // roughly sequentially, which SDRAM tolerates.
    float    _cos[kWindow / 2], _sin[kWindow / 2];   // FFT twiddle tables (shared, read-only)
    uint16_t _brev[kWindow];                          // FFT bit-reversal table (shared, read-only)
    float    _window[kWindow], _invnorm[kHop];        // window + 50%-overlap COLA gain (shared, read-only)
    static constexpr int kLut = 1024;                  // cos/sin LUT size for the phase smear (power of two)
    float    _lutc[kLut], _luts[kLut];                 // shared cos/sin table
    float    _re[2][kWindow], _im[2][kWindow];         // per-deck FFT scratch (the pipeline interleaves voices)
    // OLA accumulator + output FIFO: accessed sequentially, so unlike re/im they tolerate SDRAM. They stay
    // in on-chip SRAM at the default 4096 (byte-identical to the shipping layout), but move to the SDRAM
    // arena at >= 8192, where the full dual-deck FFT working set would otherwise overflow SRAM (~91 KB over).
    float*   _ola[2];                                  // per-deck overlap-add accumulator (pointer)
    float*   _fifo[2];                                 // per-deck output FIFO (pointer; holds up to 2 hops)
#if PSTRETCH_WINDOW < 8192
    float    _ola_sram[2][kWindow];                    // SRAM backing for the above at small windows
    float    _fifo_sram[2][2 * kHop];
#endif

    // Cached control values (0..1) for readback + the derived per-voice settings.
    float _stretch_n[2] = { 0.5f, 0.5f };   // SIZE
    float _diffuse_n[2] = { 1.0f, 1.0f };   // POS
    float _pitch_n[2]   = { 0.5f, 0.5f };   // PITCH (0.5 = unity)
    float _tone_n[2]    = { 1.f, 1.f };     // ENV knob 0..1 (raw, for readback / pickup)
    float _tone[2]      = { 1.f, 1.f };     // ENV -> one-pole LP coef (= _tone_n^2)
    float _lp[2]        = { 0.f, 0.f };     // tone filter state
    float _wet[2]       = { 1.f, 1.f };     // MIX (dry/wet)
    bool   _frozen[2]   = { false, false };
    Source _source[2]   = { Source::Live, Source::Live };   // Mode switch: Live / Capture / SD per deck

    // --- Modulation (per deck): a free-running LFO (or input-envelope follower) on the Cycle/Glow knobs,
    // routed to one target. Depth defaults to 0, so the engine is byte-for-byte the un-modulated engine
    // until the Glow knob is raised. Applied once per block in _derive_and_push (block rate is ample for
    // the sub-10 Hz ambient rates). Ranges are deliberately gentle for an ambient wash.
    static constexpr float kModRateMin = 0.03f;   // Hz at Cycle=0 (very slow drift)
    static constexpr float kModDiffAmt = 0.5f;    // full-depth diffusion swing (0..1 units)
    static constexpr float kModStrAmt  = 0.35f;   // full-depth stretch swing (normalized SIZE units)
    static constexpr float kModToneAmt = 0.4f;    // full-depth tone swing (LP coef, 0..1 units)
    // Clock-synced LFO (Alt+Cycle): the Cycle knob quantizes to a musical rate = tempo/60 * mult (cycles per
    // beat), so knob CCW = slow (a cycle every few bars) .. CW = fast. Tracks tempo live (recomputed per block).
    static constexpr int   kSyncDivs = 7;
    static constexpr float kSyncMult[kSyncDivs] = { 1.f/16, 1.f/8, 1.f/4, 1.f/2, 1.f, 2.f, 4.f };
    float     _mod_rate[2]   = { 0.f, 0.f };      // LFO rate in Hz (from Cycle)
    float     _mod_speed_n[2] = { 0.3f, 0.3f };   // Cycle knob 0..1 (readback / pickup)
    float     _mod_depth[2]  = { 0.f, 0.f };      // Glow knob 0..1 (0 = modulation off)
    uint8_t   _lfo_shape[2]  = { 0, 0 };          // LfoShape: 0 = sine, 1 = triangle
    bool      _mod_follow[2] = { false, false };  // ModType: false = LFO, true = input-envelope follower
    bool      _mod_synced[2] = { false, false };  // Alt+Cycle: LFO rate locked to the transport tempo
    ModTarget _mod_target[2] = { ModTarget::Diffusion, ModTarget::Diffusion };
    bool      _mod_start_on[2] = { true, true };  // Size/Pos switch bits (derive _mod_target on change)
    bool      _mod_size_on[2]  = { true, true };
    float     _lfo_ph[2]     = { 0.f, 0.f };      // LFO phase 0..1
    float     _foll[2]       = { 0.f, 0.f };      // follower envelope state (fast attack / slow release)
    float     _tone_eff[2]   = { 1.f, 1.f };      // effective tone LP coef this block (base + tone mod)

    // --- CV inputs (additive; ~0 when unpatched). Stored per block by the cv_* hooks, summed in _derive_and_push.
    float     _cv_oct[2]     = { 0.f, 0.f };      // V/Oct  -> pitch octaves (semitones/12)
    float     _cv_str[2]     = { 0.f, 0.f };      // Size/Pos -> stretch (normalized SIZE offset)
    float     _cv_mix[2]     = { 0.f, 0.f };      // Mix    -> dry/wet offset
    float     _cv_xfade      = 0.f;               // Crossfade CV -> A/B blend offset
    float     _lfo_out[2]    = { 0.5f, 0.5f };    // last LFO value as 0..1 CV (Mod CV out + Cycle-LED)
    bool      _gate_out[2]   = { false, false };  // an LFO cycle boundary is pending a gate-out pulse
    float     _sr = 48000.f;                       // engine sample rate (for the LFO/follower time base)
    const ITransport* _transport = nullptr;        // tempo source for the clock-synced LFO (Alt+Cycle)

    // SD-file source mode (Phase 2). The platform stream deck is injected (null on host unless a test
    // supplies one, or when SPK_USE_STREAM is off). /pstretch is scanned once in prepare(); Aux (Alt+PITCH)
    // picks which clip each deck streams.
    IStreamDeck*       _stream = nullptr;
    const ITimeSource* _time   = nullptr;         // for the scrub re-seek settle (null on host -> immediate)
    BankEntry _clips[kMaxClips];
    int       _nclips    = 0;
    bool      _rescan    = true;                  // scan /pstretch on the next prepare()
    int       _clip_sel[2] = { 0, 0 };            // selected clip index per deck
    bool      _clip_dirty[2] = { false, false };  // Aux picked a new clip; re-open on Alt release (in prepare)
    float     _clip_n[2]   = { 0.f, 0.f };        // Aux knob (0..1) for readback
    bool      _aux_held[2] = { false, false };    // Alt held -> draw the clip selector
    // Alt+POS scrub (SD only): re-seek the stream playhead to a normalized position in the clip, debounced
    // so a sweep only opens the spot you land on (mirrors the radio's settle).
    float     _scrub_n[2]      = { 0.f, 0.f };     // Alt+POS knob (0..1) for readback
    float     _scrub_opened[2] = { 0.f, 0.f };     // normalized position last actually opened
    bool      _scrub_pending[2] = { false, false };// a re-seek is waiting for the knob to settle
    uint32_t  _scrub_at[2]     = { 0, 0 };         // ms the pending scrub was last nudged
    char      _pathbuf[2][kPathMax] = {};
    int16_t   _sdraw[pstretch::Stretcher::kSdMaxFeed];   // int16 decode staging (per block, SRAM)
    float     _sdbuf[pstretch::Stretcher::kSdMaxFeed];   // float source frames fed to the voice

    Route _route = Route::Stereo;
    float _xfade = 0.5f, _gA = 1.f, _gB = 1.f;
    float _rndL[2] = { kCenterGain, kCenterGain };
    float _rndR[2] = { kCenterGain, kCenterGain };
    uint32_t _rng = 0x9e3779b9u;
};

} // namespace spotykach
