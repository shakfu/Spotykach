// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Shared indicator toolkit — the reusable half of the "indicator grammar" (docs/dev/indicator-
// grammar.md, docs/dev/indicator-comparison.md). Own-display engines fill a DisplayModel in
// render() onto a blank canvas; before this header the rich vocabulary (value-pickup feedback,
// breathe, selector/slot/progress rings, the direction-coded transport color, the canonical
// palette) lived only inside the platform's granular path, so every engine that wanted it
// re-implemented it (softcut/shuttle hand-rolled breathe + slot rings; reso/chuck/csound each
// rebuilt the selector ring; every engine re-declared mode colors). This header lifts all of it
// into one place so any render() can call it.
//
// Contract-only + hardware-free: depends solely on other src/engine/ contract headers (LEDRing,
// DisplayModel, Mode/ClockSource), never on hw/ui. Pure functions — time-based motion (breathe,
// blink) takes `now_ms` explicitly (from EngineContext's ITimeSource) rather than reaching for a
// clock, so the helpers stay testable and side-effect-free. Header-only + inline; the ring
// primitives already run only in the 62 Hz LED tick, never the audio path.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "engine/led.ring.h"      // LEDRing drawing canvas
#include "engine/display_model.h" // DisplayModel + DisplayModel::Indicator
#include "engine/mode.h"          // Mode, ClockSource, GritMode

namespace spotykach {

// =====================================================================================
// 1. PALETTE — the canonical, instrument-wide colors. "Hue = identity": every engine that
//    includes this speaks the same color language (mode/FX/clock/tape), instead of re-declaring
//    near-but-not-equal constants. These are the exact values the platform uses in
//    core.ui.leds.cpp; that file is the historical origin and should migrate onto these.
// =====================================================================================
namespace pal {
    // Neutral / utility
    inline constexpr uint32_t kBlack  = 0x000000;
    inline constexpr uint32_t kWhite  = 0xffffff;
    inline constexpr uint32_t kRed    = 0xff0000;  // recording, value deviation, error/overdub
    inline constexpr uint32_t kGreen  = 0x00ff00;  // internal clock, forward transport
    inline constexpr uint32_t kBlue   = 0x0000ff;
    inline constexpr uint32_t kPink   = 0xff00ff;  // TRS clock
    inline constexpr uint32_t kTurq   = 0x00FFEF;  // MIDI clock
    inline constexpr uint32_t kCyan   = 0x00a0ff;  // reverse transport (softcut/shuttle)
    inline constexpr uint32_t kAmber  = 0xffaa00;  // saving / busy
    inline constexpr uint32_t kErr    = 0xff6000;  // rejected action / error flash
    inline constexpr uint32_t kFrozen = 0x404040;  // engaged-but-stopped (dim white)

    // Mode identity (granular's three looper modes; reused as a generic 3-way selector hue set)
    inline constexpr uint32_t kReel   = 0xf7941d;  // orange
    inline constexpr uint32_t kSlice  = 0x0064ff;  // blue
    inline constexpr uint32_t kDrift  = 0xc850ff;  // purple

    // FX identity
    inline constexpr uint32_t kFlux     = 0xFF6565; // tape-delay (coral)
    inline constexpr uint32_t kSoftFx   = 0xFFD524; // grit — drive (yellow)
    inline constexpr uint32_t kHarshFx  = 0xFF9A24; // grit — reduce (orange)

    // Tape/SD folders — lexicographic so card folders (B G P R T Y) sort in visual order.
    inline constexpr uint32_t kTape[6] = { kBlue, kGreen, kPink, kRed, kTurq, 0xffDE21 /*yellow*/ };

    inline constexpr uint32_t mode(Mode m) {
        switch (m) { case Mode::Slice: return kSlice; case Mode::Drift: return kDrift; default: return kReel; }
    }
    inline constexpr uint32_t grit(GritMode g) { return g == GritMode::Reduce ? kHarshFx : kSoftFx; }
    inline constexpr uint32_t clock(ClockSource::Source s) {
        switch (s) { case ClockSource::internal: return kGreen; case ClockSource::ts4: return kPink;
                     case ClockSource::midi: return kTurq; default: return kWhite; }
    }
    inline constexpr uint32_t tape(int idx) { return kTape[(idx < 0 ? 0 : idx) % 6]; }
} // namespace pal

// =====================================================================================
// 2. MOTION — breathe & blink. Time-based so a stopped-but-loaded panel reads as "on, ready"
//    instead of powered-off. Two breathe presets, semantically differentiated by intent:
//      - standby: dim ambient glow (0.35..0.60) for an idle/loaded voice  (softcut/shuttle idiom)
//      - alive:   bright pulse     (0.70..1.00) for an active/empty voice  (granular idiom)
//    Both are raised-cosine over `period_ms`. Pass now_ms from EngineContext's ITimeSource.
// =====================================================================================
namespace motion {
    // Smooth pulse in [lo, hi] with the given period - a smoothstepped triangle, visually a
    // raised-cosine but with NO libm dependency (stmlib/rings use trig lookup tables, so pulling in
    // libm cos here overflowed tight engines). General form behind the two presets.
    inline float breathe(uint32_t now_ms, float lo, float hi, uint32_t period_ms = 2400) {
        float t = static_cast<float>(now_ms % period_ms) / static_cast<float>(period_ms); // 0..1 phase
        t = t < 0.5f ? t * 2.f : (1.f - t) * 2.f;   // fold to a 0..1..0 triangle
        t = t * t * (3.f - 2.f * t);                // smoothstep -> raised-cosine-like ease
        return lo + (hi - lo) * t;
    }
    inline float breathe_standby(uint32_t now_ms, uint32_t period_ms = 2400) { return breathe(now_ms, 0.35f, 0.60f, period_ms); }
    inline float breathe_alive  (uint32_t now_ms, uint32_t period_ms = 2400) { return breathe(now_ms, 0.70f, 1.00f, period_ms); }

    // Square-wave blink: true for the first half of each period. For arm/record/attention states.
    inline bool blink(uint32_t now_ms, uint32_t period_ms = 500) { return (now_ms % period_ms) < (period_ms / 2); }
} // namespace motion

// =====================================================================================
// 3. RING helpers — each draws one semantic picture onto a LEDRing. They set the ring's color +
//    brightness themselves, so a call is self-contained. Call ring.set_updated() after composing
//    (or use the DisplayModel-level helpers which do it for you). kRingLeds mirrors LEDRing's size.
// =====================================================================================
inline constexpr uint8_t kRingLeds = 32;

namespace ring {
    // LEVEL METER — an arc from 0 to `level01` (clamped) in `color`. The bread-and-butter
    // activity/output meter. Pass any headroom scaling in `level01` (e.g. peak * 1.5f).
    inline void level(LEDRing& r, float level01, uint32_t color, float brightness = 1.f) {
        if (level01 <= 1e-4f) return;
        r.set_hex_color(color);
        r.set_brightness(brightness);
        r.set_segment(0.f, std::clamp(level01, 0.f, 0.999f));
    }

    // PROGRESS — an arc from 0 to `frac01` for a bounded operation (save/load, countdown).
    inline void progress(LEDRing& r, float frac01, uint32_t color, float brightness = 1.f) {
        r.set_hex_color(color);
        r.set_brightness(brightness);
        r.set_segment(0.f, std::clamp(frac01, 0.f, 0.999f));
    }

    // WINDOW — an arc of length `len01` starting at `start01` (both normalized; the arc wraps past
    // 1.0). The general form behind level()/progress() (which pin the start at 0): a loop region,
    // grain spread, or any sub-range. `len01` is clamped to a near-full ring to avoid the exact-1.0
    // wrap-onto-itself glitch.
    inline void window(LEDRing& r, float start01, float len01, uint32_t color, float brightness = 1.f) {
        r.set_hex_color(color);
        r.set_brightness(brightness);
        r.set_segment(start01, start01 + std::clamp(len01, 0.f, 0.999f));
    }

    // PLAYHEAD — a dot at normalized position `pos01` (wrapped), overlaid on whatever is beneath.
    // The moving read-head marker. `hue` defaults to white so it reads on any arc color.
    inline void playhead(LEDRing& r, float pos01, float brightness = 1.f, uint32_t hue = pal::kWhite) {
        pos01 -= std::floor(pos01);
        r.set_point_hex_color(hue);
        r.add_point(pos01, brightness);
    }

    // VALUE + PICKUP — THE feedback that makes knobs legible (granular's _show_value, lifted).
    // Draws the value as a 0..`value` bar in `color`; then, while the physical knob has NOT yet
    // caught the stored value (`picked_up == false`), overlays a red deviation arc from value->knob
    // with a bright target dot at the knob, so the user sees which way and how far to turn to take
    // over. Clears the ring first, so call it as the top-level ring picture while a param is being
    // edited. `breathe` dims the deviation overlay (pass motion::breathe_alive(now) for the live
    // pulse, or leave the default). For engines with no soft-takeover, use the 3-arg overload.
    inline void value(LEDRing& r, float value, float knob, bool picked_up,
                       uint32_t color = pal::kWhite, float breathe = 0.85f) {
        r.clear();
        r.set_brightness(0.6f);
        r.set_hex_color(color);
        r.set_segment(0.f, std::clamp(value, 0.f, 0.999f));
        if (picked_up) return;                      // knob owns the value — no gap to show
        r.set_brightness(breathe * 0.6f);
        r.set_hex_color(pal::kRed);
        r.set_point_hex_color(pal::kRed);
        r.set_segment(std::min(value, knob), std::max(value, knob));
        r.add_point(knob, 0.95f);
    }
    // No-pickup overload: just the value bar (knob directly drives the value).
    inline void value(LEDRing& r, float value01, uint32_t color = pal::kWhite) {
        r.clear();
        r.set_brightness(0.6f);
        r.set_hex_color(color);
        r.set_segment(0.f, std::clamp(value01, 0.f, 0.999f));
    }

    // SELECTOR — `count` evenly-spaced dots, the `selected` one bright and the rest dim. The
    // Alt-held "choose one of N" ring (models / patches / routes). Optional faint backdrop arc.
    inline void selector(LEDRing& r, int count, int selected, uint32_t hue = pal::kWhite,
                         float dim = 0.15f, uint32_t backdrop = pal::kBlack) {
        if (count <= 0) return;
        if (backdrop != pal::kBlack) { r.set_hex_color(backdrop); r.set_brightness(1.f); r.set_segment(0.f, 0.999f); }
        r.set_point_hex_color(hue);
        for (int i = 0; i < count; i++)
            r.set_point(static_cast<uint8_t>(i * kRingLeds / count), i == selected ? 1.f : dim);
    }

    // SLOTS — like selector, but each slot also carries a used/empty state (bright = selected,
    // mid = used, dim = empty), driven by a bitmask. The SD tape-slot picker (softcut/shuttle).
    inline void slots(LEDRing& r, int count, int selected, uint32_t used_mask, uint32_t hue = pal::kWhite,
                      uint32_t backdrop = 0x202020) {
        if (count <= 0) return;
        if (backdrop != pal::kBlack) { r.set_hex_color(backdrop); r.set_brightness(1.f); r.set_segment(0.f, 0.999f); }
        r.set_point_hex_color(hue);
        for (int i = 0; i < count; i++) {
            const bool used = (used_mask >> i) & 1u;
            const float b = (i == selected) ? 1.f : (used ? 0.45f : 0.12f);
            r.set_point(static_cast<uint8_t>(i * kRingLeds / count), b);
        }
    }
} // namespace ring

// =====================================================================================
// 4. TRANSPORT color — the direction-coded state color softcut & shuttle both hand-rolled
//    identically. One source of truth: recording=red, forward=green, reverse=cyan, engaged-but-
//    frozen=dim white, error=amber, idle=the caller's ready hue (typically a standby breathe).
//    `live` lets the caller reuse the same color for the ring and branch on active vs idle.
// =====================================================================================
struct TransportView { uint32_t rgb; float brightness; bool live; };

inline TransportView transport_view(bool rolling, bool recording, float speed, bool error,
                                    uint32_t idle_hue = pal::kBlack, float idle_brightness = 0.f) {
    if (error)                    return { pal::kErr,    1.f, true };
    if (recording)                return { pal::kRed,    1.f, true };
    if (rolling && speed > 0.f)   return { pal::kGreen,  1.f, true };
    if (rolling && speed < 0.f)   return { pal::kCyan,   1.f, true };
    if (rolling)                  return { pal::kFrozen, 0.5f, true };
    return { idle_hue, idle_brightness, false };   // idle: caller's ready glow (or off)
}

// =====================================================================================
// 5. NAMED-INDICATOR helpers — drive the panel's discrete LEDs that own-display engines almost
//    never set. Each takes the DisplayModel so a render() wires a whole indicator in one line.
// =====================================================================================
namespace led {
    // Play/transport pad from a TransportView.
    inline void transport(DisplayModel& m, int deck, const TransportView& tv) {
        m.play[deck] = { tv.rgb, tv.brightness };
    }

    // Mode selector LEDs (the Reel/Slice/Drift convention: center/left/right), active one bright.
    // Matches reso/mosc; colors come from the shared palette so all engines agree.
    inline void mode_leds(DisplayModel& m, Mode active, float on = 1.f, float off = 0.1f) {
        m.mode_center = { pal::kReel,  active == Mode::Reel  ? on : off };
        m.mode_left   = { pal::kSlice, active == Mode::Slice ? on : off };
        m.mode_right  = { pal::kDrift, active == Mode::Drift ? on : off };
    }

    // Routing-switch position on the SAME three L/C/R LEDs, lit white: DoubleMono=left, Stereo=center,
    // GenerativeStereo=right. Engines whose L/C/R switch selects a channel Route (not a Reel/Slice/Drift
    // Mode) hand-roll this identical three-way block — tape/shuttle/softcut/radio/glitch/pstretch all
    // ship a byte-for-byte copy. This is the one call; an engine drives route_leds() OR mode_leds(),
    // never both (they own the same LEDs). clear() has already turned the other two off.
    inline void route_leds(DisplayModel& m, Route route, float on = 0.8f) {
        switch (route) {
            case Route::DoubleMono:       m.mode_left   = { pal::kWhite, on }; break;
            case Route::Stereo:           m.mode_center = { pal::kWhite, on }; break;
            case Route::GenerativeStereo: m.mode_right  = { pal::kWhite, on }; break;
        }
    }

    // Clock source indicator — source-colored, white on the key beat. For CapTransport engines
    // that today show no clock at all.
    inline void clock(DisplayModel& m, ClockSource::Source src, bool on_beat = false, bool is_key_beat = false) {
        m.clock_in = { (is_key_beat && on_beat) ? pal::kWhite : pal::clock(src), on_beat ? 1.f : 0.4f };
    }

    // A/B balance faders (white, brightness = share). For CapDualDeck engines with a crossfade.
    inline void fader_balance(DisplayModel& m, float mix01) {
        m.fader[0] = { pal::kWhite, 1.f - mix01 };
        m.fader[1] = { pal::kWhite, mix01 };
    }

    // LFO / modulation "cycle" glow — brightness tracks the modulator phase/depth.
    inline void cycle(DisplayModel& m, int deck, float phase01, uint32_t hue = pal::kWhite) {
        m.cycle[deck] = { hue, std::clamp(phase01, 0.f, 1.f) };
    }

    // GRIT FX indicator — the drive/saturation/decimation LED (granular's yellow-soft / orange-harsh).
    // Brightness = how hard the FX is driven; `off` is the always-lit floor (0.5 = granular's "lit but
    // idle" convention, 0 = dark when neutral). An FX engine wires its saturation amount here.
    inline void grit(DisplayModel& m, int deck, float amount01, GritMode g = GritMode::Drive, float off = 0.5f) {
        m.grit[deck] = { pal::grit(g), off + (1.f - off) * std::clamp(amount01, 0.f, 1.f) };
    }

    // FLUX FX indicator — the coral tape-delay/filter LED. Same always-lit convention as grit(); use it
    // for any second per-deck FX (delay, resonant filter, …).
    inline void flux(DisplayModel& m, int deck, float amount01, float off = 0.5f) {
        m.flux[deck] = { pal::kFlux, off + (1.f - off) * std::clamp(amount01, 0.f, 1.f) };
    }
} // namespace led

} // namespace spotykach
