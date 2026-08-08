# graincloud engine

`ENGINE=graincloud` · `src/engine/granular/` + `src/engine/graincloud/` · class `GraincloudEngine`

A polyphonic grain **cloud** (engine #11 in [`docs/engine-ideas.md`](../engine-ideas.md)) - the granular the looper isn't: dozens of grains with independent pitch / pan / position scattered over the recorded buffer. It is built as a **flavour of the `granular` engine**: the same source tree compiled with `SPK_GRAIN_GF` (so it keeps granular's recording, SD save/load, dual-deck, crossfade, FX and UI verbatim) with only the grain-generation core replaced - `Generator::process` sums a GrainflowLib cloud (`src/engine/graincloud/gf_cloud.{h,cpp}`) reading the recorded `Buffer` instead of driving the granular Vox. It is its own engine (`SPK_ENGINE_GRAINCLOUD`) with its own IEngine wrapper; plain `granular` is untouched and a granular build never compiles a line of GrainflowLib.

> Implementation, the GrainflowLib port, the per-block/per-sample bridge, and the file map live in [`docs/dev/graincloud-impl.md`](../dev/graincloud-impl.md).

## How to use it

**Exactly like the granular engine** - same record, save/load, pad and transport gestures. If you know granular, you know graincloud. The only difference is the sound: instead of granular's tape-loop scanning, the recorded buffer is granulated into a continuous cloud (it plays whenever a deck has audio; there is no separate "play").

- **Record** the live input: hold **Alt + Play** (or **Alt + Rev**) on a deck (Play LED turns red); press again to stop.

- **Save / load** to the SD card: **TAP-hold + Play** to enter slot-select, turn **PITCH** to choose a slot, then **Alt + Play** to save / **Alt + Rev** (or plain Play) to load. (Granular's storage UX.)

- **Crossfader** blends deck A vs B; record into the deck you're listening to.

## Controls

Per deck:

| Control | cloud meaning |
|---|---|
| **POS** | cloud centre / playhead scrub position |
| **SIZE** | grain size (duration, ~8 ms – 1.5 s) |
| **PITCH** | grain transpose (independent of playhead speed) |
| **ENV** | position spray of grain starts |
| **MODFREQ** | grain density (overlap) |
| **MOD_AMT** | per-grain pitch + pan spread |
| **SOS** | dry/wet |
| **Mode switch (L/C/R)** | grain direction: top = reverse, centre = forward, down = random |
| **Alt + PITCH** | playhead speed (down = freeze, noon = 1x, up = 4x) |
| **Alt + POS** | vibrato depth |
| **Alt + SOS** | glisson (per-grain pitch glide) |

The cloud knobs are tapped from the raw knob values directly (bypassing granular's mode-dependent routing), so they always control the cloud. The GrainflowLib character controls (direction / playhead speed / vibrato / glisson) sit on the otherwise-idle Mode switch and the three routable Alt layers (Alt+PITCH/POS/SOS). Pong is implemented but unmapped (only three Alt knob-layers route on this platform).

Grain count is 8 per deck (a conservative, CPU-safe default; raise after a `METER` run). The cloud guarantees >=2 overlapping grains for a smooth (non-tremolo) envelope.

## Build / flash

```text
make -j8 ENGINE=graincloud        # compiles the granular tree with SPK_GRAIN_GF + vendored GrainflowLib, at -Os
make ENGINE=graincloud program-dfu
make engine-graincloud            # one-shot: clean + build + flash
```

Builds at `-Os` (the granular code + the GrainflowLib templates overflow the execution SRAM at `-O2`). The grain scratch lives in fast RAM (static per-deck), only the recorded buffer is SDRAM. The vendored, de-STL'd GrainflowLib is under `src/engine/graincloud/thirdparty/grainflow/`.

> **On-device cost** of the scattered grain reads is unconfirmed; if a high-density patch glitches, lower the grain count in `gf_cloud.h` (`kMaxGrains`) and/or run `METER=1` to find the ceiling.
