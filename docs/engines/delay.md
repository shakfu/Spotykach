# Delay — tempo-synced stereo delay, with characters + topologies

`ENGINE=delay` · `src/engine/delay/delay_engine.{h,cpp}` · class `DelayEngine`

Two delay lines (deck A → left, deck B → right) whose delay time **locks to the transport tempo**, stepping through musical divisions. The second engine to consume the platform transport (it *reads* tempo; it does not subscribe to ticks). The Reel/Slice/Drift switch picks the delay **character** and the routing switch picks the stereo **topology**.

> Implementation, the file map, and dev notes live in [`docs/dev/delay-impl.md`](../dev/delay-impl.md).

## Signal path (per deck)

A single delay line over borrowed SDRAM (sub-allocated from the engine arena, sized to hold the longest division at the slowest tempo, ~6 s). Per sample, split into `read_color()` then `write_out()` (so ping-pong can cross the two taps' feedback):

1. Fractional read `s_delay` samples behind the write head (Tape adds a wow/flutter LFO to the read time). When the deck is **reversed** (Rev pad), the read instead walks *backwards* over a delay-length window, with two heads half a window apart raised-cosine crossfaded so the wrap is click-free (the same seamless-wrap trick the pitch shifter uses).

2. **Colorize the feedback signal:** a one-pole low-pass (the ENV tone control), then — by character — Tape soft-saturates it, or Shimmer pitch-shifts it +12.

3. Write `input + feedback·colorized` (in ping-pong, the colorized feedback comes from the *other* deck). Tape/Shimmer soft-clip the loop so the colored feedback can't run away; Clean is bit-for-bit.

4. Output `dry·(1−mix) + pitch(wet)·mix` — a crossfading two-head **pitch shifter** transposes the heard wet (the PITCH knob), bypassed at unity. The same shifter type does the Shimmer feedback shift. All controls are one-pole smoothed per sample (no zipper).

## Tempo sync

`process()` reads `transport->tempo()` once per block and sets each tap's target delay from the selected musical division: `delay_samples = sr · (60/bpm) · beats`. The per-sample smoother glides the change, so turning tempo (or patching a different clock) ramps the delay time rather than clicking.

The division table (`kDivBeats`, ascending so SIZE up = longer): `1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/4., 1/2` (straight + dotted + triplet).

## Control map

![Delay control surface](../media/delay-controls.svg)

*Generated from [`docs/diagrams/controls/delay.json`](../diagrams/controls/delay.json) via `make diagrams`.*

| Control | `ParamId` / config | Function |
|---|---|---|
| **SIZE** | `Size` | musical division (stepped; the ring shows the selected division) |
| **POS** | `Pos` | feedback (capped ·0.95) |
| **SOS** | `Mix` | wet/dry mix |
| **PITCH** | `Speed` | transpose the heard wet (±1 octave, centre = unity) |
| **ENV** | `Env` | feedback tone — a one-pole low-pass in the loop: up = open/clean, down = darker repeats (dub) |
| **MODFREQ** | `set_mod_speed` | mod-LFO rate (~0.05 .. 12 Hz) — chorus/flange/vibrato when paired with MOD_AMT |
| **MOD_AMT** | `ModAmp` | mod-LFO depth — modulates the delay time 0 .. ~12 ms (all characters; Tape keeps a small floor) |
| **Play pad** | `on_play_pad` | **Freeze** (per deck): loop the buffer at unity feedback; the input still passes dry so you can play over it |
| **Rev pad** | `on_play_pad` (reverse) | **Reverse** (per deck): read the buffer backwards over a delay-length window — repeats play in reverse |
| **Reel/Slice/Drift switch** | `ConfigId::Mode` (per deck) | **character**: Clean / Tape / Shimmer |
| **Route switch** | `ConfigId::Route` | **topology**: Stereo / DoubleMono / Ping-pong |

Knob meanings are fixed across characters — the mode only changes the feedback-path treatment.

### Characters (`ConfigId::Mode`, per deck; ring tint blue / amber / violet)

- **Clean** — clean digital repeats. Identical to the original delay when ENV is up.

- **Tape** — a slow wow/flutter LFO on the read time + the tone low-pass + soft saturation in the loop: warbly, degrading dub/analog repeats.

- **Shimmer** — the feedback is pitch-shifted **+12** each pass (a second crossfading shifter), so repeats climb into an octave wash.

### Topologies (`ConfigId::Route`; shown on the mode L/C/R LEDs)

- **DoubleMono** — two independent mono delays (deck A → L and deck B → R, each with its own controls).

- **Stereo** — linked: both delays share deck A's controls (a coherent stereo delay; deck B's strip inert).

- **Ping-pong** — linked + **cross-feedback**: each deck's colored feedback feeds the *other*, so echoes bounce L↔R.

`capabilities()` = `CapOwnDisplay | CapDualDeck`; `route()` reports the topology for the route LED. The engine renders its own display (a division arc tinted by character + a play indicator: white when frozen, cyan when reversed, else green lit by the input); the platform composites the clock indicators over it.
