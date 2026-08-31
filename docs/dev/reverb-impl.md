# Dev notes — route-aware Faust reverb (`reverb` engine)

Implementation/bring-up notes for `ENGINE=reverb`. The user-facing reference (the three algorithms, route-aware topology, control map, build/flash commands) is [`docs/engines/reverb.md`](../engines/reverb.md); this file holds the status, the Faust footprint and on-device load measurements, the file map, and the "adding a reverb" how-to.

## Status at a glance

- Implemented and integrated end to end (engine, build system, three Faust kernels: plate/hall/greyhole).

- Host-tested: `make -C host test-reverb` (param round-trip, both algorithms ring out, every knob role is bound, the mode switch swaps the kernel, the DoubleMono plate-only cap, and channel isolation). Plus `make -C host bench-reverb` for the process() cost and `make ENGINE=reverb METER=1` for on-device CPU load (non-blocking serial; won't hang).

- CPU measured on hardware (METER build): one hall ~18.6%, DoubleMono two plates ~41%/61% peak; the capped corner (two halls) ~70%/89% can no longer occur. The reverb is SDRAM-latency-bound, so the cap is what keeps the worst block comfortably under the 2 ms deadline.

- Links and fits: `make ENGINE=reverb` -> SRAM_EXEC **69.8%** (185,720 B of 260 KB), no overflow.

- `SRAM` (data) stays flat at ~52 KB despite a few MB of reverb delay-line state, because every kernel is placement-new'd into the SDRAM arena.

## Measured device load (`METER` build, worst block)

| Config | Avg | Peak |
|---|---|---|
| 1 voice (Stereo, hall) | ~18.6% | — |
| DoubleMono, two plates (the cap) | ~40.8% | ~60.8% |
| ~~DoubleMono, two halls~~ | ~~70.5%~~ | ~~88.9%~~ (prevented by the cap) |

The reverb is **memory-latency-bound** (delay lines in SDRAM), so two heavy voices scale ~3.8x, not 2x - the host benchmark (which can't model SDRAM) under-predicted both the absolute load and the scaling. The cap keeps the worst case at two plates (~61% peak). To lift the cap and allow two heavies later, the levers are: move the plate to internal SRAM (de-contends a plate+heavy pairing), or downsample/shrink the hall (a smaller FDN, not just shorter delays - per-sample cost is set by tap *count*, not buffer size).

## Footprint (Faust 2.85.5)

Arena placement-new keeps `SRAM` (data) flat regardless of delay-line size, so **`SRAM_EXEC` (code) is the binding constraint** - not SDRAM (the per-deck voices live inside the static 48 MB arena, whose region usage is unchanged). From `-Wl,--print-memory-usage` (`SRAM_EXEC` is **260 KB** since commit `993210f`; it was 186 KB when this table was first written, and the percentages below were re-measured 2026-08-31):

| Build | SRAM_EXEC | (against the old 186 KB) |
|---|---|---|
| `passthrough` (platform floor) | 149,584 B (56.2%) | 78% |
| `reverb` (plate + hall + greyhole, all Faust, **`-Os`**) | 185,720 B (69.8%) | 97.4% |
| `reverb` at `-O2` (historical counter-example) | ~201 KB -> overflowed 186 KB by ~11 KB | overflow |

Greyhole (modulated diffusion + a feedback pitch-shifter) is the heaviest voice; with it the three-voice build overflowed the **then-186 KB** `SRAM_EXEC` at `-O2` by ~11 KB, so the reverb branch of the `Makefile` sets **`OPT = -Os`** (as reso does). **Worth revisiting:** ~201 KB fits the 260 KB region with room to spare, so the overflow this works around no longer exists — re-measure before removing the override, since `-Os` may still be wanted for CPU headroom. The `METER` build adds ~12 KB (the USB CDC stack) on top - already over even at `-Os`, so the on-device CPU meter needs a voice dropped to measure. **Greyhole's per-block device CPU is unmeasured** (the host `bench_reverb` only times the plate); verify on hardware.

## Files

- `src/engine/reverb/dattorro.dsp`, `zita.dsp`, `greyhole.dsp` - the three active reverb sources. `voice.dsp` - the retained spike voice (not built).

- `src/engine/reverb/faust_kernel_<name>.h` - **generated** (do not hand-edit), one `class mydsp` per namespace `rv_<name>`. Regenerate with `make faust-kernels`.

- `src/engine/reverb/reverb_engine.{h,cpp}` - the `IEngine` wrapper (per-deck arena construction, `CaptureUI` + per-reverb bind tables, role mapping, mode-switch + route handling, render). `DattorroVoice`/`ZitaVoice`/`GreyholeVoice` are three peer Faust kernels; no gen~.

- `host/test_reverb.cpp`, `host/bench_reverb.cpp` - the host test (plate/hall/greyhole) and the process() cost benchmark.

- Registered in `src/engine/engine_select.h` and the root `Makefile` (`ENGINE=reverb`, `engine-reverb` flash target, `faust-kernels` codegen target).

## Adding a reverb

Follow the generic [add-an-algorithm](../engine-types/faust.md#setup-build-add) steps (drop `<name>.dsp`, append its `FAUST_KERNELS` spec, bump `ReverbEngine::kReverbCount`, register a bind table + concrete `ReverbVoice`). Then re-check `SRAM_EXEC`: with two reverbs it sits at ~92%, so a third Faust algorithm likely needs `OPT = -Os` on the reverb branch of the `Makefile`.

## Notes / TODO

- **CPU measured on hardware** (`make ENGINE=reverb METER=1`, `Meter::cpu` over the external USB CDC, non-blocking): one stereo hall **~18.6%**, DoubleMono two plates **~40.8% avg / ~61% peak**, single ~13-19%. The capped corner (two halls) measured ~70%/89% before the cap and can no longer occur. The reverb is **SDRAM-latency-bound** - cost scales super-linearly with concurrent heavy voices (~3.8x for two halls, from D-cache contention), which the host bench (~1.7x, no SDRAM model) badly under-predicted. To lift the cap later: move the plate's delay lines to internal AXI SRAM (de-contends a plate+heavy pairing - a hall's 937 KB can't fit internally), or shrink the hall's FDN *order* (per-sample cost is tap count, not delay length).

- **Hardware bring-up checks:** the mode-switch position -> voice-index order, and the route -> L/C/R LED order, are mapped to the natural 0/1/2; confirm they match the silkscreen on first flash (each is a one-line reorder if not).

- Licensing: the chosen demos are MIT; confirm the license of any `stdfaust.lib` functions a new reverb pulls before shipping it.
