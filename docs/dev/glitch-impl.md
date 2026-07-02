# Dev notes — dual lo-fi glitch voice (`glitch` engine)

Implementation/bring-up notes for `ENGINE=glitch`. The user-facing reference (controls, the 12-algorithm map, routing, build commands) is [`docs/engines/glitch.md`](../engines/glitch.md); this file holds the internals, the port, the file map, and the risks.

## TL;DR — where we are

- **Builds + host-tested.** `make -j8 ENGINE=glitch` links at ~79% SRAM_EXEC (no record path, no Faust kernels, no arena). `make -C host test` includes `test-glitch` (all 12 algorithms finite/bounded/non-silent, plus the control surface: Aux select + readback, both params, master pitch, ENV tone, Play-pad regen, routing, deck independence) - all green.

- **Not yet hardware-verified.** The pot/pad/LED/CV paths have no host harness; the audio of each algorithm and the by-ear retuning are the open items.

## Source — Noisferatu

The algorithms are ported from Rob Scape's [Noisferatu](https://github.com/rob-scape/noisferatu) (upstream `firmware/Noisferatu/algos.h`), an Arduino sketch. The original is a single oscillator on a low-rate PWM target (10-bit signed output, `pgm_read_byte` for the one sample-based algo - i.e. an AVR-class build) running at **16 kHz**. The engine is a clean reimplementation - it does **not** vendor or `#include` the original header; `glitch_voice.h` is the from-scratch port.

### Curation — why 12 of ~50

Noisferatu's `algos.h` has ~50 playable entry points, but only ~12-15 are sonically distinct - it accreted variations in a sketch (GW1/2/3 differ only in `#define`s; the 9 boolean-logic ops overlap heavily; GW9 literally calls GW1's buffer fill). The pick keeps the **distinctive** ones that are **not already served** by the vendored Plaits (`mosc`) / Rings (`reso`) DSP, which out-class the plain noise primitives:

- **Buffer glitch** (Sparse Glitch, Wander Window, Bit Mangle) - the lo-fi/circuit-bent core; address bit-mangling especially has no equivalent elsewhere in the tree.

- **Logic-noise** (Tri XOR, Square NAND, FM Noise, Ring Mod) - Benjolin/Atari-punk metallic tones.

- **Generative scale blips** (Phrygian, Pentatonic, Bernoulli) - generative-melody primitives.

- **Rhythmic noise** (Dust, Noise Rhythm).

Dropped: the redundant GW presets, the plain noise/dust primitives that Plaits already does better, and the Vinyl Crackle algo (it needs a PROGMEM sample table, `sample.h`, that is not in the source).

## The port — three things the original needed

`src/engine/glitch/glitch_voice.h` is a clean, **header-only, per-instance** reimplementation (the engine `#include`s it; the test does too). Three changes from the sketch:

1. **De-Arduino'd.** No `<Arduino.h>` - fixed-width ints from `<cstdint>`, and the sketch's own xorshift32 PRNG replaces the one `random()` call. (The original's whole Arduino surface was tiny: `<Arduino.h>` for int types, one `random()`, `SAMPLE_RATE` from a `hardware.h`, and PROGMEM for the dropped crackle algo.)

2. **De-globaled.** The sketch keeps every algorithm's state in file-scope `volatile` globals (~246 of them) - fine for one oscillator, but a **multi-instance bug** for a dual-deck engine (both decks would share state). Here all per-run DSP state is a member (the `State` struct, reset on algorithm switch), so deck A and B are independent. Only one algorithm runs per voice at a time, so the algorithms share a small pool of generic oscillator/clock/envelope fields rather than carrying 12 disjoint sets.

3. **Sample-rate agnostic (16 kHz -> 48 kHz).** The sketch baked two rate-specific constants:

   - **Phase increments** as `freq * 2^32/16000`. Here phase-per-Hz is `2^32 / sample_rate` from the injected rate, so frequencies are correct at 48 kHz (the baked form would have played everything 3x sharp).

   - **Envelope decay coefficients** for 16 kHz (e.g. `0.9963` for a 50 ms blip). Here each is retuned with `d_fs = d16 ^ (16000/fs)` so the wall-clock envelope *time* is preserved, and the `< 800 samples` envelope length becomes `0.05 * fs`.

   Output is the sketch's 10-bit signed integer (`-512..+511`), scaled by `1/512` to float; the engine soft-limits the bus.

A couple of latent sketch issues were fixed in passing and noted in the code: the `bitPosition % 12` underflow that could shift by a huge count (now wrapped 0..11), and the Phrygian/FM-Noise paths where a computed value (the scale-degree pitch; the coincidence ratio index) was never actually applied - completed so the controls do what their names say.

## Engine wrapper

`glitch_engine.{h,cpp}` (`GlitchEngine : IEngine`) is modeled on the [radio](../engines/radio.md) engine: dual deck, `CapOwnDisplay | CapDualDeck | CapAux`, route-switch topology, crossfader blend, own-display ring with the algorithm selector. It is **self-contained** - no SDRAM arena, no SD/stream, no transport. The two `glitch::Voice` members (each an ~8 KB `int16` buffer) live directly in the engine object (it is a static value member in `app.cpp`), so the buffers land in `.bss`.

- **`process()` (ISR):** per sample, one `Voice::process()` per deck, a one-pole low-pass (the ENV tone), volume, then mix to the soft-limited stereo bus per the routing switch. No allocation, no virtual dispatch inside the voice.

- **Control:** `set_param` maps SIZE->p1, POS->p2, PITCH->master pitch, ENV->tone, MIX->volume, Aux->algorithm select; `on_play_pad` regenerates the buffer. `prepare()` is a no-op.

### Knob map -> Voice

`Voice` exposes `set_algo / set_p1 / set_p2 / set_pitch / regen / process`. `p1`/`p2`/`pitch` are 0..1; the voice's `recompute()` (control rate) derives every algorithm's increments, periods, and coefficients from them, so the per-sample switch stays branch-light. Per-algorithm param ranges are in the user doc's algorithm table.

## Buffer-player silence fix

The three buffer algorithms share one 4000-sample glitch buffer, regenerated (GW1 "sparse glitchy" pattern: chunked noise/silence + one triangle blip) on algorithm select and on the Play pad. Two density knobs per algorithm guard against dead silence:

- **Density** (`_buf_noise_prob`): Sparse Glitch keeps the original 1% (its sparseness is the point); Wander Window and Bit Mangle use 35% so there is material to chew on.

- **Silence-run cap** (`_max_silence_run`): Wander Window reads only an 80-sample slice, and the GW1 chunk model can chain silent chunks into runs well over 80 - which trapped the wandering window in silence for some PRNG seeds (caught by the `test-glitch` "produces signal" assertion). For Wander Window the regen caps consecutive silence below the window size, and the window starts at a random buffer position. Sparse Glitch leaves silence uncapped; Bit Mangle jumps the whole buffer so it needs no cap.

## Files

New:

- `src/engine/glitch/glitch_voice.h` — header-only `glitch::Voice` (the 12 algorithms, per-instance, retuned). `glitch::Algo` enum + `kAlgoCount`.

- `src/engine/glitch/glitch_engine.{h,cpp}` — the `IEngine` wrapper.

- `host/test_glitch.cpp` — host suite (wired into `make -C host test`).

- `docs/engines/glitch.md`, `docs/dev/glitch-impl.md`, `docs/diagrams/controls/glitch.json`.

Edited:

- `src/engine/engine_select.h`, `Makefile`, `host/Makefile` — register `glitch`.

## Risks / watch-items

- **By-ear retuning.** Frequencies and envelope *times* are preserved across the 16 kHz -> 48 kHz move, but the sonic *character* (chunk sizes, blip ranges, density) was tuned by the original author at 16 kHz; some constants may want a pass on hardware.

- **Aliasing.** The oscillators are naive on purpose; the pitched algorithms alias hard on high notes. The ENV tone low-pass is the mitigation. Band-limiting would change the lo-fi character and is not planned.

- **First-cut param ranges** (the `expmap` frequency/rate ranges in `recompute()`) are tunable by ear.

- **No hardware test yet** of the pot/pad/LED paths.

## What's left

- Hardware bring-up + a by-ear pass on the param ranges and buffer density.

- Optional: per-algorithm default tone, or a second buffer so the buffer players can run different material per deck (today both share the same regen pattern per voice, which is already independent across decks).
