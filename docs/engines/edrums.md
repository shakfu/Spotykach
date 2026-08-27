# Edrums — four-drum Euclidean drum machine

`ENGINE=edrums` · `src/engine/edrums/edrums_engine.{h,cpp}` · class `EdrumsEngine`

A drum machine whose hits are placed by **Euclidean rhythms** and voiced by **synthesized** drums (no samples). It is the first engine to *sequence* off the shared platform transport — it subscribes to the clock's ticks rather than just reading tempo. Status: **working** — four drums (two per deck), confirmed on hardware and covered by the headless slot test (`make -C host test-edrums`); this doc is the as-built player reference.

**Four drums, two editable at a time.** Each of the two decks holds **two drums** (a "slot" pair), and all four sequence and sound at once. The platform stays two-deck: a deck's knobs and ring address whichever of its two drums is **focused**, and the **Rev pad** swaps the focus. The other drum keeps playing in the background. So you build a four-piece kit but only ever edit two voices at a moment — see [Four drums](#four-drums-two-per-deck).

> Implementation, the file map, the hardware-test feedback/resolutions, and the roadmap live in [`docs/dev/edrums-impl.md`](../dev/edrums-impl.md).

---

## Concept

- Four independent drums, two on each deck (**A** / **B**), each = a Euclidean pattern + a drum voice. A deck edits one of its two drums at a time (the **focused** one); see [Four drums](#four-drums-two-per-deck).

- The pattern is a grid of steps; some steps are *onsets* (hits) distributed as evenly as possible (the Euclidean property). On each clock step the drum advances one grid slot and, on an onset, fires its voice.

- All four drums step off the same transport clock, so the rhythm locks to the instrument's tempo and clock source (internal / TS4 / MIDI). `e.reset` realigns every pattern to the bar.

## Four drums: two per deck

The two physical decks each carry **two drums** in a slot pair (`_track[deck][slot]`, slot 0/1). All four always sequence (their own length, density, division) and always sound — focus never gates audio.

- **What focus changes:** only which drum a deck's seven knobs and its ring address, and which one the platform's knob-pickup cache tracks. The backgrounded drum holds its stored parameters and keeps playing.

- **The swap gesture:** tapping a deck's **Rev pad** toggles that deck's focus between its two drums. On a swap the engine asks the platform to re-seed that deck's knob pickup from the newly-focused drum's values, so the pots take over cleanly (a knob does nothing until moved to the new drum's value, then catches — no jumps).

- **Why two-at-a-time:** the panel has two of everything (two rings, two knob banks, two Rev pads). Mapping deck→slot inside the engine keeps the whole platform two-deck; the `IEngine` contract grows by exactly one defaulted method (`take_param_reseed`, the one-shot re-seed poll), so the other engines are unaffected.

- **Seeing the background drum:** the ring shows the focused drum's pattern in its colour; the **Rev LED** carries the backgrounded drum's colour (dim steady, brightening on its hits), so you always see the other drum is there and which it is. Colours are per `(deck, slot)`: deck A warm (slot 0 amber, slot 1 magenta), deck B cool (slot 0 cyan, slot 1 violet).

Defaults assign a four-piece kit — deck A = **Kick** (slot 0) + **Tom** (slot 1), deck B = **Snare** (slot 0) + **Hat** (slot 1) — but the kit **boots silent**: every drum is seeded with zero onsets (`POS=0`). The player builds the groove up from nothing by raising **POS** on each drum (and Rev-swapping each deck to reach the slot-1 pair). Because only the onset count is withheld — model, pitch, decay, and length are all pre-seeded — a drum is in tune and correctly voiced the instant its first onset appears.

## Audio: the synthesized voice

Each drum has one `Voice` (a small abstraction so sample playback could replace it later behind the same `trigger()` / `process()` seam) — four in all. The voice is:

- **Body** — a sine (`dsp/LUTSinOsc`) with a downward **pitch sweep** on trigger (`freq = base_hz · (1 + pitch_env·sweep)`, `pitch_env` decaying over a **per-model time**), plus an optional **second detuned partial** (a ratio of the body, for snare/tom richness) and a gentle **saturation** (`drive` → soft-clip) for punch and harmonics. The classic drum "thump".

- **Noise** — a cheap inline LCG → a 2-pole filter that **band-passes** the noise (snare/clap/tom) or **high-passes** it (the hat's metallic "tss"); the centre/corner tracks pitch (`base_hz · noise_mult`).

- **Two amp envelopes** — body and noise decay **independently** (exponential, `amp *= amp_coef` / `namp *= namp_coef`), so a voice's snap and its tonal ring can have different lengths (e.g. a snare whose noise rings past a short body). `trigger()` re-arms both.

- **Attack click** — an optional brief (~2 ms) bright transient at onset (the kick beater / tom mallet), summed on top.

- **Tone** — the body/noise blend: `out = body·(1−tone) + noise·tone + click`. Each model also carries a per-model `base_scale`, so every drum tunes to its own sensible range under the one shared PITCH knob.

### Drum models (Alt + PITCH, live)

The voice has **5 models**, selected live by holding **Alt** and turning **PITCH** (no commit step — the change applies on the next hit; release Alt and PITCH is pitch again). Each model sets the full voice character — body/noise balance, pitch-sweep amount **and time**, body frequency offset, body **and** noise decay, noise centre/Q and band-vs-high-pass, the second partial, saturation, and attack click. PITCH still drives body frequency + noise colour *within* the model, and the grit/flux macros (see [Live sound macros](#live-sound-macros)) offset the model's drive/sweep/brightness/balance/decay. Per deck, independently. The model baselines all live in the single `kModels` table in `edrums_engine.cpp` — the one surface to tune the kit by ear.

| # | Model | Character |
|---|---|---|
| 0 | Kick | deep sine body, long (~55 ms) pitch drop, beater click + drive, longer decay |
| 1 | Snare | two-partial body + bright band-pass noise that rings past the body |
| 2 | Clap | noisy, multi-burst (3 quick re-triggers ~11 ms apart), tail rings |
| 3 | Closed hat | **high-passed** noise (metallic), very short |
| 4 | Tom | two-partial pitched body, mild drop, mallet click |

Defaults, per drum: deck A = **Kick** (slot 0) + **Tom** (slot 1); deck B = **Snare** (slot 0) + **Hat** (slot 1). Each slot is fully voiced at init (length, pitch, decay, model) but seeded with **zero onsets**, so the kit boots silent and the player builds it up with POS — no drum waits on a knob move to be correctly voiced once raised.

> Why a knob and not a load-pad: synth model switching is instantaneous and free, so it applies live > — the select-then-load ceremony is only needed for expensive operations (sample loading), and is > reserved for that (see roadmap). The same Alt+PITCH control will select sample slots later, the only > difference being a deferred/debounced load instead of an instant apply.

The live model select rides a new platform seam: `CapAux` + `ParamId::Aux`. The platform routes Alt+PITCH to `Aux` only for engines that advertise `CapAux`; granular (which uses Alt+PITCH for pitch-quantize) is unaffected.

The sequencer triggers run in the transport-tick callback (the audio-block context); `trigger()` only re-arms envelope state, so it is allocation/lock free.

### Output routing (the hardware routing switch, control 26)

The 3-position routing switch picks how the four voices reach the two outputs (via `set_config(Route)`, mirroring granular's int mapping so the panel's L/C/R read the same). A pre-limiter trim then a final `SoftLimit` keep the summed bus in range — four simultaneous voices sum hotter than the original two, so the trim avoids constant limiting:

| Switch | `Route` | Edrums behavior |
|---|---|---|
| Centre | `Stereo` | all four voices **summed to both outputs** (a mono drum bus) — the conventional default |
| Left | `DoubleMono` | deck A's two drums → left, deck B's two → right (the split) |
| Right | `GenerativeStereo` | each hit randomly panned across the stereo field |

`route()` reports the mode for the panel's L/C/R LED.

## Sequencer

Each drum runs a Euclidean step sequencer: POS sets how many onsets are spread across the pattern, SIZE sets its length, and ENV rotates it. (Implementation detail in [`docs/dev/edrums-impl.md`](../dev/edrums-impl.md#sequencer-dspcpattern).)

## Control map (as built)

![Edrums control surface](../media/edrums-controls.svg)

*Generated from [`docs/diagrams/controls/edrums.json`](../diagrams/controls/edrums.json) via `make diagrams`.*

The platform gives each deck 7 knobs (see [README](README.md#knobs-how-a-physical-control-reaches-an-engine)); edrums uses all of them:

| Knob | `ParamId` (routing) | Function |
|---|---|---|
| **POS** | `Pos` | density — Euclidean onsets, as a fraction of the current length |
| **SIZE** | `Size` | **pattern length** (2..16 steps) |
| **ENV** | `Env` | **rotation** (pattern shift) |
| **PITCH** | `Speed` | drum pitch — sine body (~30..480 Hz) **and** the noise band centre |
| **SOS** | `Mix` | **per-drum gain** (level) |
| **MOD_AMT** | `ModAmp` | **probability** an onset fires (0..100%, default 100% = full clockwise) |
| **MODFREQ** | `set_mod_speed` | **clock division** — 1/16, 1/8, 1/4 (per deck) |
| **Alt + PITCH** | `Aux` | **drum model** select (live; see below) |
| **grit + SOS** | `GritMix` | **decay** (amp-env time, ~30 ms..1.2 s) |
| **grit + PITCH** | `GritIntensity` | **drive** (saturation) — see [Live sound macros](#live-sound-macros) |
| **flux + PITCH** | `FluxIntensity` | **pitch-sweep amount** (±2 oct on the model) |
| **flux + SOS** | `FluxMix` | **body↔noise balance** |
| **flux + POS** | `FluxFb` | **noise brightness** (filter cutoff, ±2 oct) |
| **Play pad** | `on_play_pad` | **stop / start** the focused drum (stays grid-locked; tails ring out) |
| **Rev pad** | `on_play_pad` | **swap** the deck's focused drum |
| **Alt + Play** | `on_record_pad` | **mute** the focused drum (it keeps stepping, audio silenced) |
| **hold Alt + Seq** (~1.5 s) | `clear_sequence` | **reset that deck's two drums to factory defaults** — see [Presets](#presets-qspi-auto-persist) |

The seven plain knobs act on the deck's **focused** drum; the **Rev pad** swaps focus to the other drum (see [Four drums](#four-drums-two-per-deck)). Density is stored as a fraction and re-derived over the active length, so changing SIZE keeps the relative fill. `POS` and `MOD_AMT` are engine-seeded (the platform reads `param(Pos)` / `param(ModAmp)` for their initial values), so init pre-seeds them: density **0** on every drum (the silent boot — POS seeds to minimum, so turning it up adds hits) and probability 100% (so the knob defaults to "every onset fires" with full clockwise = 100%). The display draws the focused drum's pattern over the active length (the length fills the 32-LED ring; onset lit, playhead bright) in that drum's colour, with a play-LED flash on each hit; the Rev LED carries the backgrounded drum's colour. Colours are per `(deck, slot)`: A slot 0 amber / slot 1 magenta, B slot 0 cyan / slot 1 violet.

### Live sound macros

The plain knobs above run the sequencer + pitch/gain; the per-drum **sound shaping** lives on the **grit** and **flux** pad modifiers (hold the pad, turn PITCH/SOS/POS), so the kit is tweakable in performance rather than fixed per model. Each macro is a **bipolar offset on the model's baseline** — at noon the drum is exactly as the model voiced it, so a fresh kit is unchanged until you move a knob — and like every other knob it addresses the **focused** drum (repointing on a Rev swap):

| Gesture | Macro | Range |
|---|---|---|
| **grit + SOS** | decay (amp-env time) | ~30 ms .. 1.2 s |
| **grit + PITCH** | drive (saturation) | clean .. crunch |
| **flux + PITCH** | pitch-sweep amount | x the model's sweep, ±2 oct |
| **flux + SOS** | body↔noise balance | ±0.5 around the model |
| **flux + POS** | noise brightness (filter cutoff) | ±2 oct |

With PITCH (pitch), SOS (gain) and Alt+PITCH (model), that is **eight live axes per drum** — and all four drums voice independently. The macro channels route through the platform's existing grit/flux held-modifier layer (`GritIntensity`/`GritMix`/`FluxIntensity`/`FluxMix`/`FluxFb`), which is pickup-seeded from `param()`, so no platform change was needed. (Decay sits on `grit+SOS` rather than `Alt+SOS` because the `Feedback` channel `Alt+SOS` routes to is not pickup-seeded.) Not yet exposed: an **independent body-vs-noise (snap) decay** — the voice has separate envelopes for it, but it needs a sixth modifier channel (a small platform addition), so the one decay currently scales both.

### Transport: stop/start and mute

Two **per-drum** performance controls on the Play pad (they address the focused drum, like the knobs; Rev to reach the deck's other drum), neither persisted (every drum boots running + unmuted):

- **Play pad = stop/start** the focused drum. Stopped, that drum makes no sound (no new triggers; a sounding tail rings out), but its **clock keeps running silently** - the pattern stays phase-locked to the grid and the ring playhead keeps sweeping - so restarting drops it back in **in sync** with the other drums/bar, never drifted. Stop each drum independently for a full halt.

- **Alt + Play = mute** the focused drum. It keeps stepping (stays grid-locked) but its audio is silenced; the deck's other drum is unaffected.

The Play LED tracks the **focused** drum's transport state: **off** when stopped, a **dim** hit-flash when running-but-muted, a **full** hit-flash when running and sounding.

### Presets (QSPI auto-persist)

The whole **kit auto-saves to flash**, so your tweaks (all four drums plus the route and per-deck focus) survive a power cycle with no save gesture, and reload at boot. To reset to the factory kit, hold **Alt + Seq** on a deck for ~1.5 s — per deck: hold on deck A to reset kick/tom, on deck B for snare/hat, or both for a full kit reset. (Storage mechanics in [`docs/dev/edrums-impl.md`](../dev/edrums-impl.md#presets-qspi-auto-persist--mechanics).)

### Polymeter

Per-drum length **and** division make all four drums cycle independently (a drum's cycle = `length × division` ticks). On the **external** clock a transport grid reset (`e.reset`) realigns every drum (step phase + pattern position) to the bar; on the **internal** clock there is no reset, so the drums free-run and realign naturally at their least-common-multiple. Tempo/clock-source are the shared transport's (TAP / tap-hold+MODFREQ_A / Alt+TAP — see [README](README.md#clock-control-player-facing)).
