# QDelay — a dub/ambient flavor of the dual delay

`ENGINE=qdelay` · `src/engine/qdelay/qdelay_engine.{h,cpp}` · class `QdelayEngine`

A second "flavor" of the [tempo-synced dual delay](delay.md), inspired by [qdelay](https://github.com/tiagolr/qdelay). It keeps the delay engine's entire control grammar — tempo-locked musical divisions, feedback, mix, PITCH transpose, ENV tone, the mod LFO, the Freeze/Reverse pad gestures and the three stereo topologies — and swaps only the **character palette** from Clean/Tape/Shimmer to **Clean / Diffuse / Duck**. The aim is the smeared, reverb-leaning dub end of the delay spectrum rather than the warble-and-shimmer end.

Because the binding constraint on the hardware is the control surface (a handful of knobs and two 3-way switches), qdelay does not expose the desktop plugin's ~40 parameters. It picks the highest-value, lowest-cost slice — feedback diffusion and ducking — and maps them onto the existing mode switch.

> Implementation, the file map, and dev notes live in [`docs/dev/qdelay-impl.md`](../dev/qdelay-impl.md).

> **License:** unlike the rest of this MIT project, the qdelay engine is **GPLv3** — `src/dsp/diffuser.h` is a port of [qdelay](https://github.com/tiagolr/qdelay)'s GPLv3 `Diffusor`, so the engine and any `ENGINE=qdelay` binary are GPLv3. See [`src/engine/qdelay/NOTICE.md`](../../src/engine/qdelay/NOTICE.md).

## What it shares with `delay`

The division table, tempo sync, control smoothing, the crossfading two-head PITCH shifter, the Freeze (Play pad) and Reverse (Rev pad) gestures, and the Stereo / DoubleMono / Ping-pong topologies are identical to [`delay`](delay.md) — refer there for those mechanics. Two delay lines (deck A → left, deck B → right) live in borrowed SDRAM (~6 s each). Per sample the path splits into `read_color()` then `write_out()` so ping-pong can cross the two taps' feedback and so the stereo diffuser can run on the feedback pair between the read and the write.

## Control map

![QDelay control surface](../media/qdelay-controls.svg)

*Generated from [`docs/diagrams/controls/qdelay.json`](../diagrams/controls/qdelay.json) via `make diagrams`.*

| Control | `ParamId` / config | Function |
|---|---|---|
| **SIZE** | `Size` | musical division (stepped; also sizes the Diffuse smear) |
| **POS** | `Pos` | feedback (capped ·0.95) |
| **SOS** | `Mix` | wet/dry mix |
| **PITCH** | `Speed` | transpose the heard wet (±1 octave, centre = unity) |
| **ENV** | `Env` | feedback tone — one-pole low-pass in the loop: up = open, down = darker repeats |
| **MODFREQ** | `set_mod_speed` | mod-LFO rate (~0.05 .. 12 Hz) — chorus/flange/vibrato with MOD_AMT |
| **MOD_AMT** | `ModAmp` | mod-LFO depth — modulates the delay time 0 .. ~12 ms |
| **Play pad** | `on_play_pad` | **Freeze** (per deck): loop the buffer at unity feedback |
| **Rev pad** | `on_play_pad` (reverse) | **Reverse** (per deck): read the buffer backwards over a delay-length window |
| **Reel/Slice/Drift switch** | `ConfigId::Mode` (per deck) | **character**: Clean / Diffuse / Duck |
| **Route switch** | `ConfigId::Route` | **topology**: Stereo / DoubleMono / Ping-pong |

Knob meanings are fixed across characters — the mode only changes the feedback-path treatment.

## Characters (`ConfigId::Mode`, per deck; ring tint blue / teal / amber)

- **Clean** — clean digital repeats; identical to the delay engine's Clean (bit-for-bit loop when ENV is up).

- **Diffuse** — the feedback runs through an **8-stage allpass diffuser** ([`src/dsp/diffuser.h`](../../src/dsp/diffuser.h), a JUCE-free port of qdelay's `Diffusor`/TARON MiniVerb). Each repeat is smeared a little more than the last, so the tail thickens into a dense, reverb-like dub wash. The diffuser is a single stereo instance over the two taps' feedback; each deck takes the diffused copy only if *it* is in Diffuse, so DoubleMono can run a Diffuse deck alongside a Clean/Duck deck. SIZE sets the smear depth (longer division = looser, more spread-out smear). Its buffers (~235 KB at 48 kHz) sub-allocate from the SDRAM arena, so SRAM is unaffected.

- **Duck** — the heard wet **ducks under the dry input**. A per-tap fast-attack / slow-release peak follower tracks the input level and attenuates the wet send while you play, so the repeats stay out of the way and bloom in the gaps once you stop. The feedback loop itself is *not* ducked, so the tail keeps building under a busy input and is revealed when the input drops.

## Topologies (`ConfigId::Route`)

Identical to [`delay`](delay.md): **DoubleMono** (two independent mono delays), **Stereo** (linked to deck A's controls), **Ping-pong** (linked + cross-feedback, echoes bounce L↔R). In Diffuse/Duck the per-deck colored feedback is what crosses in ping-pong.

## Resources

- `capabilities()` = `CapOwnDisplay | CapDualDeck`; `route()` reports the topology for the route LED.

- Display: a division arc tinted by character + a play indicator (white = frozen, cyan = reversed, else green by input).

- Delay lines + the diffuser are sub-allocated from the engine arena (SDRAM); SRAM_EXEC sits at ~77 % (the diffuser does not touch it), comfortably a normal (non-QSPI) build.

- Host coverage: [`host/test_qdelay.cpp`](../../host/test_qdelay.cpp) (`make -C host test-qdelay`) and the standalone diffuser test [`host/test_diffuser.cpp`](../../host/test_diffuser.cpp) (`make -C host test-diffuser`).

## Build

```text
make engine-qdelay        # clean build + DFU flash
make -j8 ENGINE=qdelay    # compile only
```
