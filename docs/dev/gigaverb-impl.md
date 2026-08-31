# Dev notes — gigaverb (`gigaverb` engine)

Implementation, the file map, and dev notes for `ENGINE=gigaverb`. The user-facing reference (control map, build commands, the reverb fold-in) is [`docs/engines/gigaverb.md`](../engines/gigaverb.md).

## Status at a glance

- 2-in / 2-out, 8 parameters, no `[buffer]`s. Stereo, so it maps 1:1 onto the platform bus.

- Links and fits: `make ENGINE=gigaverb` -> SRAM_EXEC **159,004 B (59.7% of 260 KB)** at the default `-O2`, no overflow, no `-Os` needed. (Re-measured 2026-08-31; read "158732 B (83.3% of 186 KB)" while the region was still 186 KB.)

- `SRAM` (data) stays flat (~52 KB) despite the reverb delay lines, because all gen~ state is bump-allocated from the injected SDRAM arena.

- `capabilities() = 0`: stereo audio + knob params only (no custom display, MIDI, CV-out, or sequencing).

- Not yet flashed/heard on hardware.

## Files

The shared `src/engine/gen/` family and the generated `src/engine/gigaverb/` tree are described in [engine-types/gen.md](../engine-types/gen.md#files). gigaverb-specific:

- `src/engine/gigaverb/gigaverb_engine.h` - the per-engine glue (traits + the control map in [gigaverb.md](../engines/gigaverb.md)). Generated once, then hand-tuned; preserved across re-generation unless `--force-glue`.

- `src/engine/gigaverb/gen/` - the copied gen~ export (Tom Erbe's gigaverb). Also the default source for `make gen-engines` (`GEN_EXPORTS` points back at it), so a regen is reproducible from the checkout.

## Notes / TODO

- No display: `capabilities() = 0`, so the platform shows its default UI. A meter would need `render()`.

- Reverb tail / CPU not measured on hardware. Fits SRAM_EXEC at 83% with headroom before `-Os` is needed.

- The `Mix` knob drives gigaverb's `dry` level (not a wet/dry blend) in this standalone build; retune the map if a true mix is wanted (the `reverb` fold-in does this for you).
