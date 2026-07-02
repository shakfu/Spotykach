# The grammar of indicators — `granular` engine

An inventory and analysis of every visual indicator the default `granular` engine drives: the two LED **rings** (their segment/arc and dot/point layers), the discrete named **LEDs**, and the **color / brightness / blink** vocabulary that gives them meaning. The goal is to name the reusable "grammar" so future engines can speak it consistently.

> **Speaking the grammar from your engine:** the reusable half of this vocabulary now lives in a > shared, engine-callable header — **`src/engine/indicators.h`**. Any own-display engine's `render()` > can call it for value-pickup feedback, breathe, selector/slot/progress/level rings, the > direction-coded transport color, and the canonical palette — instead of re-implementing them. See > [§8. Shared helper API](#8-shared-helper-api-engineindicatorsh) below and the gap analysis in > [`indicator-comparison.md`](indicator-comparison.md).

Source of truth for this analysis:

- `src/hw/hardware.h` — the physical LED map (`Hardware::LedId`).

- `src/engine/led.ring.{h,cpp}` — the ring canvas + its drawing primitives.

- `src/engine/indicators.h` — the **shared helper toolkit** (§8) engines call from `render()`.

- `src/engine/display_model.h`, `src/engine/engine_leds.h` — the engine↔platform LED contract.

- `src/ui/core.ui.leds.cpp` — the platform renderer (palette, blink, overlays).

- `src/engine/granular/granular_engine.cpp` — the engine's own `render_ring()` (steady state).

---

## 0. Who draws what: a co-authored display

Granular is **not** an own-display engine (`capabilities()` does not set `CapOwnDisplay`), so its indicators are drawn by *two* parties, and the grammar is split accordingly:

- **The engine reports semantics.** It answers query structs — `deck_leds()`, `fx_leds()`, `play_leds()`, `alt_leds()`, `mix()`, `route()` (`src/engine/engine_leds.h`) — and it draws the *steady-state* ring picture in `render_ring()`. It never touches hardware or picks colors for the named LEDs.

- **The platform (`CoreUI`) owns presentation.** `core.ui.leds.cpp` holds the entire color palette, all blink/breathe timers, storage overlays, and the knob-value "deviation" overlays. It composites everything and blits to the WS2812 chain.

Contrast: an own-display engine fills a `DisplayModel` in `render()` and the platform just blits it verbatim (`_blit_display()`, `core.ui.leds.cpp:241`). Granular instead goes through the richer `_draw_leds()` / `_draw_ring()` path. **So granular's grammar is the reference grammar** — it is the one place where the full vocabulary (below) is actually spoken.

---

## 1. Indicator inventory (count + type)

The panel is a single WS2812 chain of **85 pixels** (`LedId::LED_LAST`), partitioned into:

| Class | Count | What |
|---|---|---|
| **Ring pixels** | 2 × 32 = **64** | `LED_RING_A`, `LED_RING_B` — one 32-LED ring per deck |
| **Per-deck discrete LEDs** | 2 × 8 = **16** | `PLAY, REV, GRIT, FLUX, GATE_IN, CYCLE, ALT, FADER` |
| **Global discrete LEDs** | **5** | `MODE_LEFT, MODE_CENTER, MODE_RIGHT, CLOCK_IN, SPOTY_PAD` |
| **Total single LEDs** | **21** | |

So there are **two rings** and **21 discrete indicators**. There is *no* separate physical "inner" and "outer" ring — each deck has exactly one 32-pixel ring. The "outer / inner" duality is a **rendering** distinction inside that one ring: a base **segment (arc)** layer and an overlaid **point (dot)** layer (see §2). Reading the user's terms onto the code:

- **"outer ring"** → the `set_segment()` arc layer (the continuous fill: loop region, progress, value bar).

- **"inner ring" / "ring dots"** → the `add_point()` / `set_point()` marker layer (playheads, deviation targets, step ticks, slot markers) drawn *on top of* the arc.

- **"leds"** → the 21 discrete named indicators around the rings.

---

## 2. The ring as a two-layer canvas

`LEDRing` (`led.ring.h`) is a hardware-free 32-pixel canvas with **two independently-colored layers**, which is the core of the ring grammar:

| Layer | Primitive | Color setter | Purpose |
|---|---|---|---|
| **Segment / arc** | `set_segment(start, size, sharp)` | `set_hex_color()` + `set_brightness()` | continuous regions: the loop, progress bars, value bars |
| **Point / dot** | `add_point(pos, bright, sharp, over)` / `set_point(idx, bright)` | `set_point_hex_color()` | discrete markers: playheads, targets, step ticks |

Two mechanics make the layers read as "arc + dots on top of it":

1. **Anti-aliasing.** Non-`sharp` draws call `extrapolate()` (`led.ring.cpp:10`): a fractional position spreads its brightness across the two straddling pixels, so arcs and dots glide smoothly between the 32 discrete LEDs. `sharp = true` snaps to a single pixel (used for hard step markers and the record head).

2. **Additive overlay.** `_set(..., overlay=true)` *adds* color (`_colors[idx] += color*(0.85·b+0.15)`, `led.ring.cpp:124`) rather than replacing it, so a dot brightens/tints the arc underneath instead of erasing it. `add_point(..., over=true)` is the default; `set_segment` replaces.

`set_point(idx, …)` addresses a pixel by **exact index** (not normalized position) — this is the "evenly spaced ticks" idiom used for slot markers, key-interval ticks, and size-quarter counts.

A global `kBrightnessMult = 0.8` scales every ring pixel, and a produce/consume handshake (`is_updated()` + a `_colors_cache`) lets the main loop draw while the 62 Hz TIM5 ISR blits.

---

## 3. Ring semantics — what each picture means (granular)

The engine's `render_ring()` (`granular_engine.cpp:347`) draws the **steady state**; the platform's `_draw_ring()` (`core.ui.leds.cpp:355`) draws the **transient overlays** and the alternate knob-value / storage screens. Together:

**Steady-state (engine, `render_ring`):**

| Deck state | Arc (segment) | Dots (points) |
|---|---|---|
| Empty & not armed | full circle, **mode color**, breathing at `0.5×breathe` | — |
| Recording (not overdub) | arc `0 → rec_size`, mode color | **red** dot at write head (sharp) |
| Playing (`!empty`) | loop region: Reel/Slice `start→start+size`; Drift = grain spread `×.95` centered on start | **white** dot per active grain playhead, brightness = that grain's envelope (up to `kVoxCount`) |

**Transient overlays (platform, composited on the playing arc):**

- **Position** deviation — white dot, on-move-diff only (`ParamId::Pos`).

- **Size** change — a **red** arc between current and target size + a white target dot; Drift draws a symmetric red/white spread around the start (`core.ui.leds.cpp:414`).

- **Overdub head** — a **red** dot at the write head, drawn last so it sits on top.

**Alternate ring screens (platform replaces the whole ring):**

- **Storage**: *saving* = white progress arc; *loading* = tape-color progress arc; *selecting* = slot dots via `_show_slots()`.

- **Knob value editing** (`_show_value`, `core.ui.leds.cpp:502`): turning a knob paints its value as an arc from `0` in that param's context color, plus a **red deviation arc + red target dot** showing target-vs-current until pickup ("tracking") completes. This is the shared pickup-feedback idiom for Mix, Feedback, Win, Env, Mod, Click, Tempo, Pan, Flux/Grit, Pitch, etc.

- **Step displays** via `set_point`: key intervals, size-quarters, poly/mono slice, slots — evenly spaced ticks, every 4th tick brightened/white as a beat accent.

---

## 4. Discrete LED grammar

Grouped mirror-image left (A) / right (B), with a few shared globals. Each is driven either live in `_draw_leds()` or via a `_draw_*` helper feeding the `LED` wrapper (`core.ui.h:107`).

| LED (×2 unless noted) | Meaning | Color | Brightness / blink |
|---|---|---|---|
| `PLAY` / `REV` | transport + direction; also record & storage-select | **mode color** playing; **red** recording; **white**/tape-color in storage | on=1.0; queued-play flashes on `_clock_led_on`; hold-clear blinks 50 ms |
| `GRIT` | grit FX + mode | **yellow** (soft) / **orange** (reduce/harsh) | on=1.0, off=0.5 (always lit dim) |
| `FLUX` | flux (tape delay) FX | **coral** `kDelayColor` | on=1.0, off=0.5 |
| `CYCLE` | LFO / modulator | mode color (Follow), clock-source color (synced), else white | brightness = live LFO phase (`_lfo_a/_lfo_b`) |
| `GATE_IN` | CV gate activity | **mode color** | idle 0.25, flashes to 1.0 on a trigger (10-tick decay) |
| `ALT` | Alt modifier + track/seq state | **white** (Alt held / arm blink); count-in / **red** (recording) | empty-indicator blinks 8× @80 ms; track blink |
| `FADER` (1 each) | A/B crossfade | **white** | brightness = `mix` (A = `1-mix`, B = `mix`) |
| `MODE_L/C/R` (global) | route topology (DoubleMono / Stereo / GenerativeStereo) | **white** | one lit at 0.8 |
| `CLOCK_IN` (global) | clock source + key beat | source color (green/pink/turquoise), **white** on key beat | flashes per clock (11-tick window) |
| `SPOTY_PAD` (global) | system / boot | — | boot animation (`_draw_launching`) |

---

## 5. The color palette (the semantic axis)

Colors are **meaning-bearing**, defined once at the top of `core.ui.leds.cpp`:

**Mode identity** (the dominant hue for a deck — arc, gate, play):

| Mode | Name | Hex |
|---|---|---|
| Reel | orange | `0xf7941d` |
| Slice | blue | `0x0064ff` |
| Drift | purple | `0xc850ff` |

**FX identity:**

| FX | Color | Hex |
|---|---|---|
| Flux (delay) | coral | `0xFF6565` |
| Grit — soft | yellow | `0xFFD524` |
| Grit — reduce/harsh | orange | `0xFF9A24` |

**Utility colors** (cross-cutting, mode-independent):

- **White** — neutral / target / on-beat / deviation-in-value.

- **Red** — recording, overdub head, value deviation, error.

- **Green / pink / turquoise** — clock source: internal / TRS(TS4) / MIDI (`clock_source_color`).

- **Green + white** — count-in.

**Tape colors** (`kTapeColor`, 6 entries) — Blue, Green, Pink, Red, Turquoise, Yellow, ordered *lexicographically* so SD-card folders (`B G P R T Y`) sort in the same visual order.

---

## 6. The brightness / motion axis

Brightness is the second grammatical axis, orthogonal to color:

- **Breathe** (`_breathe_led`, `core.ui.leds.cpp:608`): `0.7 + sin·0.15`, the "alive/idle" pulse — applied to the empty ring, the selected storage slot, and value-deviation arcs.

- **Baselines**: ring segment default `0.5`; value arcs `0.6`; many overlays `0.8`; dots `0.9`; target/deviation dots `0.95`; global `×0.8` (`kBrightnessMult`).

- **Dim-vs-lit as state**: FX LEDs are *always* lit — `0.5` = off, `1.0` = on — so absence reads as a distinct state, not darkness.

- **Blink timing is meaning**:

  - `_clock_led_on` — an 11-tick window (`render_leds`, `core.ui.leds.cpp:98`) that gates all *clock-synced* flashes (queued play, Slice arm/rec, count-in, key beat).

  - Slice arm/record blinks *on the clock*; other modes blink *free* at ~80 ms.

  - Hold-to-clear blinks fast (50 ms); the empty-buffer nudge blinks the Alt LED 8× at 80 ms.

---

## 7. Summary — the grammar in one paragraph

Every deck owns **one 32-pixel ring** rendered as two overlaid layers — a **segment/arc** for continuous quantities (loop region, progress, value bars) and additive **dots** for discrete markers (grain playheads, edit targets, step ticks) — plus **8 discrete LEDs**; five more LEDs are global. **Hue carries identity** (each mode and FX has a fixed color; clock and tape have their own), **red/white are the universal utility hues** (record / deviation / neutral / on-beat), **brightness carries level and liveness** (breathe = idle, dim-but-lit = off-state, full = active), and **blink rate carries sync** (clock-locked vs free vs fast-clear). The engine speaks only *semantics* through query structs and `render_ring()`; the platform owns the *palette, timing, and overlays* — so this file describes a vocabulary that is co-authored by `granular_engine.cpp` and `core.ui.leds.cpp`.

---

## 8. Shared helper API (`engine/indicators.h`)

The comparison doc's recommendation is now implemented. `src/engine/indicators.h` is a header-only, **hardware-free contract header** (depends only on `led.ring.h` / `display_model.h` / `mode.h`, so it passes `check-boundary` and is includable by any engine). It turns each piece of the grammar above into a named, self-contained call. Motion helpers take `now_ms` explicitly (from `EngineContext`'s `ITimeSource`) so they stay pure and testable.

It is organized into five namespaces, one per grammatical axis:

### `pal::` — the canonical palette (§5)

One instrument-wide source of truth for "hue = identity", so engines stop re-declaring near-but-not- equal constants. Constants (`pal::kReel/kSlice/kDrift`, `kFlux/kSoftFx/kHarshFx`, `kWhite/kRed/kGreen/ kPink/kTurq/kCyan/kAmber/kErr/kFrozen`, `kTape[6]`) plus lookups:

```cpp
pal::mode(Mode::Slice)          // -> 0x0064ff   (mode identity hue)
pal::grit(GritMode::Reduce)     // -> harsh orange
pal::clock(ClockSource::midi)   // -> turquoise
pal::tape(idx)                  // -> the idx-th SD-folder color
```

### `motion::` — breathe & blink (§6)

Two breathe presets, **semantically differentiated by intent**, plus a blink square wave:

```cpp
motion::breathe_standby(now_ms) // 0.35..0.60 dim ambient — "loaded / idle, ready"
motion::breathe_alive(now_ms)   // 0.70..1.00 bright pulse — "active / empty"
motion::blink(now_ms, 500)      // bool, 500 ms square — arm/record/attention
```

### `ring::` — one call per ring picture (§2, §3)

Each sets its own color + brightness and draws a complete picture onto a `LEDRing`:

| Call | Picture |
|---|---|
| `ring::level(r, level01, color)` | activity/output meter arc from 0 |
| `ring::progress(r, frac01, color)` | bounded-operation arc (save/load) |
| `ring::window(r, start01, len01, color)` | arbitrary-start arc — loop region / grain spread / sub-range |
| `ring::playhead(r, pos01, bright, hue)` | moving read-head dot (wrapped, overlaid) |
| `ring::value(r, value, knob, picked_up, color, breathe)` | **value bar + red pickup-deviation overlay** |
| `ring::value(r, value01, color)` | value bar only (no soft-takeover) |
| `ring::selector(r, count, sel, hue)` | choose-one-of-N dots, selected bright |
| `ring::slots(r, count, sel, used_mask, hue)` | SD slots: selected/used/empty dots + backdrop |

`ring::value` is the headline addition — the `_show_value` pickup feedback (red deviation arc from the stored value to the physical knob + a bright target dot) that previously existed **only** in the platform's granular path. `picked_up == false` means the knob hasn't caught the value yet, so the gap is shown; pass `true` (or use the 3-arg overload) once the knob owns the value.

### `transport_view(...)` — direction-coded transport color (§5)

The record-red / forward-green / reverse-cyan / frozen-dim-white / error-amber / idle logic that `softcut` and `shuttle` each hand-rolled, as one function returning `{rgb, brightness, live}`:

```cpp
auto tv = transport_view(rolling, recording, speed, error,
                         /*idle_hue*/ trackHue, /*idle_brightness*/ motion::breathe_standby(now) * 0.5f);
led::transport(m, deck, tv);                // -> play pad
if (tv.live) ring::level(r, 1.f, tv.rgb);   // reuse the same color for the ring, branch on .live
```

### `led::` — the discrete named indicators (§4)

Wire the panel LEDs own-display engines almost never set, one line each:

```cpp
led::mode_leds(m, active_mode);        // Reel/Slice/Drift center/left/right, active bright
led::route_leds(m, route);             // route on the SAME L/C/R LEDs (the mode_leds alternative)
led::clock(m, src, on_beat, is_key);   // clock_in for CapTransport engines
led::fader_balance(m, mix01);          // fader[A]/fader[B] for CapDualDeck balance
led::cycle(m, deck, phase01, hue);     // LFO/mod glow on the cycle LED
led::grit(m, deck, amount01, gmode);   // grit/drive/saturation FX LED (yellow/orange)
led::flux(m, deck, amount01);          // flux FX LED (coral) — delay/filter/etc.
led::transport(m, deck, tv);           // play pad from a TransportView
```

### Best-use demonstration

The design goal is that a full, legible `render()` reads as a short list of intent. A model engine (e.g. `reso`) with breathe, a value-editing overlay, a model selector, mode + clock LEDs:

```cpp
void render(DisplayModel& m) override {
    m.clear();
    const uint32_t now = _time ? _time->now_ms() : 0;

    for (int d = 0; d < DeckRef::Count; d++) {
        auto&          dk  = deck[d];
        const uint32_t hue = pal::mode(dk.mode);         // shared palette — hues agree across engines

        // Play pad: trigger flash, else a standby breathe so "loaded" != "off".
        m.play[d] = dk.flash > 0 ? DisplayModel::Indicator{ hue, 1.f }
                                 : DisplayModel::Indicator{ hue, motion::breathe_standby(now) * 0.5f };
        if (dk.flash > 0) dk.flash--;

        if (dk.editing) {                                // knob turning -> the pickup overlay
            ring::value(m.ring[d], dk.param, dk.knob, dk.picked_up, hue, motion::breathe_alive(now));
        } else if (dk.aux_held) {                        // Alt+PITCH -> model selector
            ring::selector(m.ring[d], kModels, dk.model, hue);
        } else {                                          // steady state -> level meter + pitch head
            ring::level(m.ring[d], dk.level * 1.5f, hue);
            ring::playhead(m.ring[d], dk.pitch_n, 1.f);
        }
        m.ring[d].set_updated();
    }

    led::mode_leds(m, deck[0].mode);                      // was 3 hand-written Indicator literals
    led::clock(m, _clock_src, _on_beat, _is_key_beat);    // was: nothing (CapTransport, no clock LED)
}
```

Compared to the current `reso::render()` (`src/engine/reso/reso_engine.cpp:289`) this adds value-pickup feedback, an idle breathe, and a clock indicator **while shrinking the code**, because each concept is now one named call instead of open-coded `set_hex_color`/`set_segment`/`set_point` sequences plus a re-declared local palette. That is the intended win: the rich half of the grammar becomes vocabulary an engine *invokes*, not machinery it *rebuilds*.

### First adopter: `shuttle`

`src/engine/shuttle/shuttle_engine.cpp` is the first engine migrated onto the toolkit. It landed in two phases: **(1)** a behavior-preserving swap of its hand-rolled breathe, transport-color ladder, slot loop, and read-head dot for `motion::breathe_standby` / `transport_view` / `ring::slots` / `ring::playhead` (deleting its local `kErrColor`/`kRingLeds`), then **(2)** enhancements the shared vocabulary made one-liners: `led::fader_balance` (A/B crossfade, previously unlit), `ring::window` (the loop window drawn as an arc with the read head inside it, replacing a featureless full-ring backdrop), `led::cycle` (wow/flutter modulation glow), and a **param-aware knob-edit overlay** (POS/SIZE redraw the loop window via `ring::window` with a dot at the loop start, so you see the region's position and length; MIX/PITCH show a `ring::value` bar — the "no-deviation" form, correct because shuttle applies knob values immediately). It is the worked example of the §8 pattern: display state expressed as a short list of intent. One lesson from it: a value *bar* fits a scalar (level, speed) but is wrong for a *geometric* param — POS/SIZE describe a region, so they must be drawn as one (an arc + marker), not a 0..value bar.

### Second adopter: `tape`

`src/engine/tape/tape_engine.cpp` is the second engine on the toolkit, and the one that exercises the **on/off transport path** (shuttle validated the idle standby-breathe path; tape validates a real record state). Its `render()` hand-rolled exactly the pieces the toolkit exists to replace, and — unlike `reso` — every hue was an *exact* `pal::` match, so the swap is behavior-preserving: the direction-coded color ladder (`playing→green / recording→red / err→amber / off`) became `transport_view` + `led::transport`; the `(now/110)&1` amber strobe became `motion::blink(now, 220)`; the slot-selector loop became `ring::slots`; the solid ring became `ring::level`; and the local `kErrColor`/`kRingLeds` constants were deleted. Two facts made it exact: tape's play/record states are mutually exclusive (so `transport_view`'s recording-first ordering matches the old playing-first ladder), and a cleared `LEDRing` keeps `_segment_brightness = 1.0`, which is what `ring::level`/`ring::slots` set — so the backdrop brightness is unchanged. `render()` shrank from ~35 lines of open-coded `set_*` calls to a short list of intent.

Tape also motivated the one structural **toolkit addition** this migration needed: `led::route_leds(m, route)`. Six engines (tape/shuttle/softcut/radio/glitch/pstretch) ship a byte-for-byte copy of the same routing-switch block (`DoubleMono→left / Stereo→center / GenerativeStereo→right`, white 0.8). It lives beside `led::mode_leds` because it drives the same three L/C/R LEDs — an engine calls one or the other depending on whether that switch selects a channel `Route` or a Reel/Slice/Drift `Mode`.

**Then the enhancement phase** — the point of the exercise is not just to refactor but to make each engine *speak more of the grammar*. Tape's panel was a flat solid-color ring + play + route; it now uses:

- **a record-level meter** (`ring::level` fed by a per-deck peak follower added to `process()`) — while *recording*, the ring is a VU of the incoming signal. During *playback* the ring is instead a single steady-color fill: a moving level arc reads as too volatile to watch under a loop, so metering is scoped to recording (where a VU earns its keep) and playback stays calm;

- **a varispeed marker** — a `ring::playhead` dot at the PITCH position, drawn *inside the steady ring* (playback and idle) so tape's signature ±2-octave varispeed (previously shown *nowhere*) is always visible; turning PITCH just moves that marker. Note: PITCH deliberately gets **no** separate knob overlay — an overlay would replace the bright steady ring with a dim backdrop and read as "the ring goes dark while I turn," so params already visible in the steady ring are excluded from the overlay set;

- **an idle standby breathe** (`motion::breathe_standby`) on a stopped deck *whose slot is loaded*, so "ready" reads distinctly from both "off" and an empty/unloaded deck (which stays dark). Making that gate reliable required a **boot slot scan** in `prepare()`: the `_slot_used` cache is otherwise filled only when the Alt+PITCH selector opens (too late for a fresh boot), so `prepare()` re-probes the card until it mounts, and a successful play/record also marks its slot used;

- **param-aware knob-value overlays** (`_show_value` idiom): turning any knob flashes a picture for ~700 ms — MIX/drive/char/cutoff/reso/wow-rate/crossfade as a `ring::value` bar, **ENV as a 4-way `ring::selector`** (None/Plain/Faded/Fripp), PITCH and pan as markers vs their references — where before a knob turn showed nothing at all;

- **four of the "dead" named LEDs** (grammar §4): `led::grit` = the resonant filter the grit pad drives (yellow — the LED tracks its own pad), `led::flux` = tape saturation (coral), `led::cycle` = wow/flutter glow, `led::fader_balance` = the A/B crossfade.

This added `led::grit`/`led::flux` to the toolkit (the drive/saturation and second-FX LEDs, the granular always-lit convention generalized). Net: tape went from the comparison doc's "minimal dialect" to nearly the full vocabulary, and the additions are reusable by the next engines. The lesson repeats shuttle's: a value *bar* fits a scalar, but a *geometric* control (PITCH, pan, loop-mode) must be drawn as a marker/selector.

### What stays in the platform (not in the helper)

The helper lifts the **stateless, drawable** grammar. Three pieces remain platform-owned by design because they need platform state, not engine state: the **clock-locked blink window** (`_clock_led_on`, an 11-tick counter tied to the transport tick — engines get a free-running `motion::blink` instead), the **storage state machine** (`DeckStorage` save/load + slot enumeration — engines pass a fraction to `ring::progress` or a `used_mask` to `ring::slots`, but the SD/tape state lives in the platform), and the granular **FX/named-LED semantics** (`grit`/`flux` are labels specific to the granular panel). Everything else an own-display engine needs from `render()` is in `indicators.h`.
