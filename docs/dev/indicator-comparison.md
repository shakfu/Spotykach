# Indicator usage across engines — a comparison

Companion to [`indicator-grammar.md`](indicator-grammar.md), which reverse-engineers the full indicator vocabulary from the reference `granular` engine. This document asks the follow-up question: **do the other engines actually use that vocabulary, or are they leaving capability on the table?**

Short answer: **most engines use a small fraction of what the panel can express.** The rich grammar (mode-hued arcs, position/grain dots, red pickup-deviation overlays, breathe, clock-locked blink, the eight named per-deck indicators, storage rings) is almost entirely a `granular` phenomenon. Own-display engines converge on a common minimal dialect — *level arc + pitch/position dot + play dot + mode LEDs* — and several (`softcut`, `shuttle`, `chuck`, `bard`) go further but **re-implement** platform features rather than reuse them.

> **2026-07-31 audit refresh (TODO P0).** §7 below re-runs this comparison against the current tree, across *every* engine including the ones that postdate the original write-up (`bard`, `pstretch`, `glitch`, `qdelay`, `softcut`, `mosc`). Net finding: the shared toolkit `src/engine/indicators.h` now exists and `tape` + `shuttle` are migrated onto it, but **13 own-display engines still hand-roll everything** and call zero toolkit helpers. The per-engine table and the ranked migration worklist are in §7.

---

## 1. Two rendering paths (why usage splits the way it does)

There are two ways an engine's indicators reach the LEDs (`src/ui/core.ui.leds.cpp`, `src/engine/iengine.h`):

- **Co-authored (granular only, plus its clone `graincloud`).** `capabilities()` does *not* set `CapOwnDisplay`. The engine reports *semantics* (`deck_leds/fx_leds/play_leds/alt_leds/mix/route`

  - `render_ring()`); the **platform** owns the palette, breathe/blink timers, the knob-value "deviation" pickup overlays (`_show_value`), storage rings, and all eight named indicators. This path is where the full grammar lives.

- **Own-display (every other engine).** `capabilities()` sets `CapOwnDisplay`. The engine fills a `DisplayModel` in `render()` and the platform **blits it verbatim** (`_blit_display()`), doing *no* palette/blink/value interpretation. The engine gets a blank canvas and must draw everything itself.

The consequence is structural, not incidental: **an own-display engine that wants breathe, blink, value-pickup feedback, or storage animation has to re-code it**, because those live in the platform's granular path. Most don't bother — hence the thin dialect below.

---

## 2. What's on offer (the grammar, from `indicator-grammar.md`)

| Capability | Primitive / field | Meaning in granular |
|---|---|---|
| Ring arc | `ring[].set_segment()` | loop region, level, progress, value bar |
| Ring dots | `ring[].add_point()` / `set_point()` | playheads, targets, step ticks, slot markers |
| Two-layer color | `set_hex_color` + `set_point_hex_color` | arc hue vs dot hue, additive overlay |
| Breathe | brightness = `0.7+sin·0.15` | idle / alive |
| Value pickup overlay | `_show_value` (platform) | red deviation arc + target dot on knob turn |
| Blink | `_clock_led_on` / timers | sync-locked vs free vs fast-clear |
| **8 named per-deck LEDs** | `play rev grit flux gate_in cycle alt fader` | transport, FX, LFO, gate, modifier, crossfade |
| 5 global LEDs | `mode_left/center/right`, `clock_in`, `spot` | route topology, clock source, system |
| Color as identity | mode/FX/clock/tape palettes | hue = meaning |

---

## 3. Usage matrix

Counts are raw references to each `DisplayModel` field / `LEDRing` primitive in an engine's `render()` (a proxy for "does it use this at all", not a quality score). Faust engines (`chorus/filter/voice` and others) inherit render from `faust/faust_chain.h` / `faust_fx.h`.

```text
ENGINE      | RING: seg pnt setpt bri | play rev | modeLCR | breathe/blink | named LEDs used
------------|-------------------------|----------|---------|---------------|-----------------
granular*   |       3   2    0    1   |  (query) | (query) |   yes (plat)  | ALL 8 + globals
graincloud* |       3   2    0    1   |  (query) | (query) |   yes (plat)  | ALL 8 + globals
------------|-------------------------|----------|---------|---------------|-----------------
chuck       |       8   4    0    0   |   5   0  |   yes   |      no       | play, mode
softcut     |       4   1    2    5   |   2   0  |   yes   |   yes (own)   | play, mode
shuttle     |       3   1    2    4   |   1   0  |   yes   |   yes (own)   | play, mode
radio       |       2   3    0    0   |   1   0  |   yes   |      no       | play, mode
csound      |       3   1    0    0   |   2   0  |   yes   |      no       | play, mode
glitch      |       2   2    0    0   |   1   0  |   yes   |      no       | play, mode
pstretch    |       1   2    0    0   |   1   0  |   yes   |      no       | play, mode
reso        |       1   0    2    0   |   1   0  |   yes   |      no       | play, mode
mosc        |       1   0    2    0   |   1   0  |   yes   |      no       | play, mode
tape †      |   (via toolkit helpers) |   1   0  |   yes   |  yes (tk)     | play, mode, grit, flux, cycle, fader
reverb      |       2   0    0    2   |   1   0  |   yes   |      no       | play, mode
edrums      |       0   0    2    0   |   1   1  |   no    |      no       | play, REV
delay       |       1   0    0    0   |   1   0  |   yes   |      no       | play, mode
qdelay      |       1   0    0    0   |   1   0  |   yes   |      no       | play, mode
passthrough |       1   0    0    0   |   1   0  |   no    |      no       | play
chorus      |    (faust: level arc + play, if Traits::meter)               | play
filter      |    (faust: level arc + play, if Traits::meter)               | play
voice       |    (faust: level arc + play, if Traits::meter)               | play
```

`†` = migrated onto the shared toolkit (`indicators.h`): ring pictures via `ring::` helpers, breathe/blink from `motion::` (`yes (tk)`), plus the named FX/fader LEDs and knob-value overlays — the enhancement phase, not the pre-migration minimal dialect the rest of this matrix snapshots.

`*` = co-authored path (query structs, not `render()`; breathe/blink/value overlays supplied by the platform).

**The columns that are all-zero across every own-display engine tell the story:** `grit`, `flux`, `gate_in`, `cycle`, `alt`, `fader`, `clock_in`, `spot` — **eight of the panel's indicators are used by *no* own-display engine.** `rev` is used by exactly one (`edrums`).

---

## 4. Per-engine notes

### reso (the requested engine)

`render()` at `src/engine/reso/reso_engine.cpp:289`. Draws, per deck:

- **arc** = envelope level meter (`level·1.5` clamped), in a local 3-color mode palette (`0xffcc00 / 0x00aaff / 0xaa00ff` — note these are *re-declared constants*, close to but not equal to the platform's `kReelColor/kSliceColor/kDriftColor`);

- **one white dot** = pitch position, or, while Alt+PITCH is held, **5 evenly-spaced dots** = the resonator-model selector with the active model bright;

- **play** indicator = mode color, flashed on trigger via a manual `flash` down-counter;

- **mode L/C/R** = the three model/mode colors, active one bright.

It's a clean, representative member of the minimal dialect. What it does **not** do, though the panel supports it: no breathe on idle, no value feedback when you turn Size/Structure/Brightness/Damping (you turn a knob and the ring shows nothing), no clock indicator despite `CapTransport`, and it hand- rolls a mode palette instead of sharing one.

### mosc, delay, qdelay, tape, reverb, glitch, pstretch, radio, csound

All variations on *level/activity arc + a dot or two + play + mode LEDs*. `radio` and `glitch` and `pstretch` add expressive dots (spectral/scan/grain positions). `csound`/`chuck` add an Alt-held **patch selector** ring (dots per program) — the same idiom `reso` uses for models and `softcut/shuttle` use for tape slots, each **implemented independently**.

### chuck

The richest `render()` (most primitive calls): patch-selector ring, running/stopped play color, per-deck arcs and dots. Still confined to ring + play + mode; no breathe/blink/named-LED use.

### softcut & shuttle (the "reinventors")

These go furthest — and in doing so **duplicate platform capability**:

- **Own breathe**: a raised-cosine `0.35+0.25·…` over `now_ms()%2400` (`softcut_engine.cpp`, `shuttle_engine.cpp`) — a hand-rolled copy of the platform's `_breathe_led()`.

- **Own storage-slot ring**: an Alt-held tape-slot selector (selected bright / used mid / empty dim) — a hand-rolled copy of the platform's `_show_slots()` / storage progress ring.

- **Direction-coded transport color** (record red / fwd green / rev cyan / frozen white) packed into the play dot + ring, instead of the dedicated `rev` LED that exists for exactly this.

They are the *best-looking* non-granular engines precisely because they re-created features the platform already has — evidence the capability is desirable and the reuse path is missing.

### edrums

The only own-display engine to light a second named LED (`rev`), used as a second trigger/deck indicator. No mode LEDs. Shows the named indicators *can* be driven from `render()` — nobody else does.

### chorus, filter, voice (and other Faust engines)

Inherit `render()` from `faust/faust_chain.h:122` / `faust_fx.h:153`: a level-meter arc + play dot, **compile-time gated by `Traits::meter`**. If a Faust engine's manifest doesn't set `meter`, its panel is entirely dark. This is the floor of the spectrum.

---

## 5. Findings — where capability is left on the table

1. **Eight indicators are dead for every engine but granular.** `grit`, `flux`, `gate_in`, `cycle`, `alt`, `fader`, `clock_in`, `spot` are never set outside the granular path. Some are granular- specific by label (grit/flux), but `cycle` (an LFO/mod indicator), `fader` (A/B balance), `clock_in` (sync source), and `alt` (modifier feedback) are **generic** and would be meaningful on many engines — e.g. `reso/softcut` have `CapTransport` but show no clock, `mosc/reso` have LFO-ish params but no `cycle` glow. *(Partly resolved: the `tape` enhancement lights `grit` (the filter its pad drives), `flux` (saturation), `cycle` (wow/flutter), and `fader` (crossfade) via new `led::grit`/`led::flux`/ `led::cycle`/`led::fader_balance` — the first own-display engine to drive them. `gate_in`/`alt`/`spot`/ `clock_in` remain unused off the granular path.)*

2. **No knob-value feedback off the granular path.** The platform's `_show_value` pickup overlay (the red deviation arc + target dot that makes granular's knobs legible, and the whole tracking/pickup UX) is keyed off `MValue`/`ParamId` inside the platform's `_draw_ring`. Own-display engines get **none of it**: turning a knob on reso/mosc/etc. produces no visual response at all. This is the single biggest expressive gap. *(`shuttle` and now `tape` close it engine-side via `ring::value`

   - param-aware overlays — tape shows a value bar for scalars, a `ring::selector` for the ENV loop-mode, and markers for PITCH/pan; the same pattern is ready for the other engines.)*

3. **Breathe / blink must be re-coded.** Only `softcut` and `shuttle` bother, by copying the math. Everyone else has a static (often black-when-idle) panel. "Idle but ready" vs "off" is indistinguishable on most engines.

4. **Palette duplication.** Mode colors are re-declared per engine (`reso`'s `0xffcc00…` vs the platform's `kReelColor 0xf7941d…`), so hues drift between engines and none share the tape/clock palettes. The grammar's "hue = identity" only holds *within* an engine.

5. **Selector-ring idiom reinvented 5×.** The Alt-held "dots around the ring, active one bright" pattern appears independently in reso (models), chuck/csound (patches), softcut/shuttle (slots). It's clearly a common need with no shared helper.

6. **The floor is dark.** Faust engines with `meter=false` show nothing; `passthrough` shows only a level arc + play. Nothing signals mode, activity, or readiness.

7. **The routing-switch block is reinvented 6×.** Every engine whose L/C/R LEDs show the channel `Route` (tape/shuttle/softcut/radio/glitch/pstretch) ships a byte-for-byte copy of `if DoubleMono → mode_left / Stereo → mode_center / else → mode_right`, white 0.8. Promoted to `led::route_leds(m, route)` during the tape migration — the mode-LED analog of `led::mode_leds`.

---

## 6. Recommendations

> **Status:** recommendations 1–3 are now implemented in **`src/engine/indicators.h`** — a shared, > hardware-free helper toolkit any engine's `render()` can call (value-pickup overlay, breathe/blink, > selector/slot/progress/level rings, the direction-coded transport color, and the canonical > palette). API + a best-use example are in [`indicator-grammar.md` §8](indicator-grammar.md#8-shared-helper-api-engineindicatorsh). > **`shuttle` is the first engine migrated onto it** (`src/engine/shuttle/shuttle_engine.cpp`): its > hand-rolled breathe/transport-color/slot code was replaced by the helpers (behavior-preserving), > then enhanced with A/B fader LEDs, a loop-window arc, a wow/flutter cycle glow, and knob-turn value > bars — see [`indicator-grammar.md` §8 "First adopter"](indicator-grammar.md#first-adopter-shuttle). > **`tape` is the second** (`src/engine/tape/tape_engine.cpp`), in two phases like shuttle. **(1)** a > behavior-preserving swap: transport-color ladder → `transport_view`/`led::transport`, amber strobe → > `motion::blink`, slot loop → `ring::slots`, solid ring → `ring::level`, routing block → the new > `led::route_leds`. **(2)** then *enhancements that speak more of the grammar*: a record-level meter > (peak follower + `ring::level`, scoped to recording — playback stays a calm solid ring), a varispeed > marker (`ring::playhead` — tape's ±2-octave PITCH was shown nowhere), an idle standby breathe, > param-aware knob-value overlays (MIX/FX bars, ENV as a 4-way > `ring::selector`, PITCH/pan markers), and four of the "dead" named LEDs — `led::grit` (the filter the grit > pad drives), `led::flux` (saturation), `led::cycle` (wow/flutter), `led::fader_balance` (crossfade). See > [`indicator-grammar.md` §8 "Second adopter"](indicator-grammar.md#second-adopter-tape). Migrating tape > added **`led::route_leds`/`led::grit`/`led::flux`** to the toolkit (see finding 7 and finding 1). `softcut` > (shuttle's twin) is deferred until its functionality settles. What remains is migrating the other engines' > `render()` onto the toolkit and lifting `core.ui.leds.cpp`'s constants onto `pal::`.

Ordered by leverage:

1. **Lift the value-pickup overlay into a shared, engine-callable helper.** The `_show_value` deviation/pickup rendering is the highest-value missing feature for own-display engines. Expose it as a `DisplayModel`/`LEDRing` utility (e.g. `draw_value(ring, value, in_value, color)`) that any `render()` can call, so knob turns become legible everywhere — not just granular.

2. **Provide shared platform helpers for breathe, the selector-ring, and progress/storage rings.** `softcut`/`shuttle` prove the demand and the shape; promote their (and the platform's) copies to one reusable set so engines stop hand-rolling `now_ms()` cosines and slot loops.

3. **Publish the canonical palette** (`kReelColor`, mode/clock/tape hues) in a shared header engines include, replacing per-engine constants — restores "hue = identity" across the whole instrument.

4. **Wire the generic named indicators from `render()`.** At minimum drive `clock_in` for `CapTransport` engines and `fader` for `CapDualDeck` engines with a balance; consider `cycle` for engines with an LFO. `edrums` already shows named LEDs are reachable from `render()`.

5. **Give Faust engines a non-blank default.** Even without `meter`, a mode-hued idle breathe + play dot would lift the floor.

The theme: the grammar is rich but **trapped in the platform's granular path**. Turning its key pieces (value feedback, breathe, selector/progress rings, palette) into shared helpers callable from `render()` would let every own-display engine speak the full language instead of the current pidgin.

---

## 7. Audit refresh (2026-07-31, TODO P0) — full current-tree pass

§1–6 were written as the toolkit (`src/engine/indicators.h`) was being introduced and `tape`/`shuttle` migrated. This section re-runs the comparison against the current tree, across **every** engine — including those that postdate the original write-up (`bard`, `pstretch`, `glitch`, `qdelay`, `softcut`, `mosc`).

> **Update 2026-07-31 — mechanical migration landed.** §7.2/§7.3 below capture the *pre-migration* state (the audit). Since then the mechanical dedup in §7.4/§7.5 was applied: **all 15 own-display engines now `#include` and call `src/engine/indicators.h`** — every hand-rolled selector (9×), route block (~8×), meter, and palette constant is retired, and the Faust floor got a non-blank `meter=false` default. All engines build clean on ARM. **Deferred** (net-new indicators needing per-engine data plumbing + on-panel verification, folded into P2): `ring::value` pickup feedback, `led::clock`, `led::cycle` for the LFO engines, and breathe on the renders that have no `ITimeSource`. LED appearance is not hardware-verified yet.

### 7.1 Adoption status

The toolkit exists but almost nobody calls it: `src/engine/indicators.h` is `#include`d by exactly **two** engines. Every other own-display engine hand-rolls the same pictures against raw `LEDRing` + literal hex. *(Pre-migration snapshot; see the 2026-07-31 update above — now all 15 own-display engines call the toolkit.)*

| Group | Engines | State |
|---|---|---|
| **Co-authored** (platform draws the full grammar) | `granular`, `graincloud` | Full grammar, but the *platform* owns it |
| **Migrated onto `indicators.h`** | `tape`, `shuttle` | The intended end-state (value bars, breathe, selector/slots, named FX LEDs) |
| **Own-display, 100 % hand-rolled** | `bard`, `softcut`, `edrums`, `reso`, `mosc`, `delay`, `qdelay`, `reverb`, `radio`, `glitch`, `pstretch`, `csound`, `chuck` | **13 engines** re-implementing what the toolkit provides |
| **Faust floor** | `chorus`, `filter`, `voice` | Level arc + play *iff* `Traits::meter`; otherwise a **dark panel** |

### 7.2 Per-engine — draws now vs. top gaps (→ helper)

| Engine | Draws now | Top gaps left on the table |
|---|---|---|
| **bard** | play, gate_in, grit, flux, cycle, progress+bookmark ring, shelf selector, route L/C/R | Richest hand-roller. Shelf ring→`ring::selector`; route block→`led::route_leds`; transport ladder→`transport_view`; all hex→`pal::`; `led::cycle`. *Caveat: bard repurposes grit=room-colour, flux=amber — `led::grit/flux` fixed hues wouldn't preserve intent; `pal::`+`led::cycle` drop in.* |
| **softcut** | play, mode/route, transport-colour ring, slot picker, **hand-rolled cos breathe** | The one true cos/%2400 breathe→`motion::breathe_standby`; transport ladder→`led::transport`; slots→`ring::slots`; has `CapPitchPickup` but **no `ring::value` pickup feedback** |
| **edrums** | play, **rev** (only user), step-sequencer ring | Sequences off transport but no `led::clock`; handles Route but shows **no route LEDs**→`led::route_leds`; model-flash→`ring::selector` |
| **reso** | level arc, pitch dot, 5-dot model selector, play, mode L/C/R | Selector→`ring::selector`; **`CapTransport` but no `led::clock`**; arp/drift→`led::cycle`; mode hues **drift** from `pal::kReel/Slice/Drift` |
| **mosc** | level arc, pitch dot, **1-dot** engine readout, play, route | 24-engine readout should be N-dot `ring::selector`; route→`led::route_leds`; CV mod→`led::cycle`; `pal::` |
| **delay** | division arc, transport-colour play, route | **Tempo-synced but no `led::clock`**; mod LFO→`led::cycle`; route→`led::route_leds`; Clean colour *is* `pal::kCyan`, re-declared |
| **qdelay** | division arc, transport-colour play, route | Same as delay + Duck env; Clean==`pal::kCyan`, Duck==`pal::kAmber`, re-declared |
| **reverb** | dim baseline + decay arc, algorithm mode LEDs, play | Static `0.10` baseline→`motion::breathe_standby`; decay→`ring::value`; algo choice→`ring::selector`; greyhole ModDepth→`led::cycle/flux` |
| **radio** | station marker, **bank selector**, play, route | Bank ring→`ring::selector`; route→`led::route_leds`; idle-dark→`motion::breathe_standby`; `pal::` |
| **glitch** | algo marker, **algorithm selector**, play, route | Algo ring→`ring::selector`; route→`led::route_leds`; `pal::` |
| **pstretch** | stretch marker, **clip selector**, state-colour play, route | Clip ring→`ring::slots`; stretch marker→`ring::value` (has `CapAltPos` scrub → pickup story); route→`led::route_leds`; state hex→`pal::`+`transport_view` |
| **csound** | level meter, **patch selector**, play, source LED | Patch ring→`ring::selector`; meter→`ring::level`; `0x00c0ff`≈`pal::kCyan`; play→`led::transport` |
| **chuck** | level meter+peak dot, **patch selector**, panic rings, METER rings, play | Same as csound (selector shared *verbatim*); meter→`ring::level`+`ring::playhead`; panic→`transport_view(error)` |
| **chorus / filter / voice** | level arc + play *iff* `meter` | Dark when `meter=false`; no mode/breathe |

### 7.3 Systemic findings — the same code reinvented

1. **Selector ring reinvented 9×** — reso(models), mosc(engines), bard(shelves), softcut(slots), radio(banks), glitch(algos), pstretch(clips), csound(patches), chuck(patches). csound↔chuck are byte-identical. → `ring::selector` / `ring::slots`.

2. **`led::route_leds` copied ~8×** — bard, softcut, radio, glitch, pstretch, mosc, delay, qdelay each ship the identical white L/C/R block.

3. **Transport-colour ladder reinvented ~7×** — softcut, bard, delay, qdelay, pstretch, csound, chuck. → `transport_view` + `led::transport`.

4. **Value-pickup feedback: zero own-display adoption** (except tape/shuttle). The single biggest expressive gap — even `softcut`(`CapPitchPickup`) and `pstretch`(scrub) skip it. → `ring::value`.

5. **Idle = dark** — only `softcut` breathes (by hand); every other stopped panel is indistinguishable from powered-off. → `motion::breathe_standby`.

6. **Clock indicator never lit off the granular path** — `reso`(CapTransport), `delay`/`qdelay`(tempo-synced), `edrums`(sequences off clock) all show no `clock_in`. → `led::clock`.

7. **Palette re-declared in every engine**; `0x00c0ff`/`0x00aaff`/`0x00a0ff` recur as near-misses for `pal::kCyan`, and reso's mode hues drift from the canonical set. → `pal::`.

8. **`alt` and `spot` LEDs: used by no engine at all.**

### 7.4 Migration worklist (ranked by leverage)

The audit itself is desk/host work (done). Applying and confirming the LED changes on the panel is **hardware-gated** and folds into the P2 bench session.

1. **`led::route_leds` + `pal::` sweep** — trivial, mechanical, hits 8 engines, kills the colour drift.

2. **`ring::selector` / `ring::slots`** — retire 9 hand-rolled selectors (csound/chuck share one).

3. **`ring::value` pickup feedback** — highest expressive win; start with the engines that already track pickup (`softcut`, `pstretch`, `reso`).

4. **`motion::breathe_standby`** idle glow everywhere idle==dark; **`led::transport`** for the 7 transport ladders.

5. **`led::clock`** for reso/delay/qdelay/edrums; **`led::cycle`** for the LFO/mod engines.

6. **Faust floor:** a mode-hued idle breathe + play dot when `meter=false`.

`bard` is both the richest hand-roller and a clean migration candidate (selector + route + `pal::` + `led::cycle` all drop in).

### 7.5 Migration sequence (per-engine, by leverage)

The worklist above is ordered by *helper*; this is the same work ordered by *engine* — cleanest / highest-visibility first. `tape` + `shuttle` are already migrated; `granular` / `graincloud` are the co-authored reference (out of scope).

| # | Engine(s) | Why this rank |
|---|---|---|
| 1 | **bard** | Richest hand-roller; selector + route + `pal::` + `led::cycle` all drop in cleanly |
| 2 | **csound + chuck** | Byte-identical patch selector — migrate together, one helper retires two copies |
| 3 | **softcut** | Only true hand-rolled cos-breathe + slots + transport ladder; twin `shuttle` is a proven template |
| 4 | **radio, glitch, pstretch** | Near-identical shape (route block + Alt selector); one repeatable mechanical pass |
| 5 | **reso, mosc** | Selector + missing clock/cycle; reso's mode-hue drift fixed by `pal::` |
| 6 | **delay, qdelay** | Identical twins — tempo-synced `led::clock` + route + `pal::kCyan`/`kAmber` cleanup |
| 7 | **reverb** | Static baseline → `motion::breathe_standby`, decay → `ring::value`, algo → `ring::selector` |
| 8 | **edrums** | Route LEDs + `led::clock` + model `ring::selector` |
| 9 | **chorus / filter / voice** (Faust) | Different shape — non-blank default when `meter=false`; lowest visibility, do last |
