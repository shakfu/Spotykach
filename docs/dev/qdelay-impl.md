# Dev notes — dub/ambient delay flavor (`qdelay` engine)

Implementation, the file map, and dev notes for `ENGINE=qdelay`. The user-facing reference (signal path, control map, characters, topologies) is [`docs/engines/qdelay.md`](../engines/qdelay.md). qdelay is a second flavor of the [`delay`](../engines/delay.md) engine — it shares that engine's control grammar and most of its DSP, swapping only the character palette to **Clean / Diffuse / Duck**. Read [`docs/dev/delay-impl.md`](delay-impl.md) first; this page covers only the deltas.

## What is reused vs. new

Carried over from `delay` (re-derived in `qdelay_engine.{h,cpp}`, not shared at the type level — the two engines never link together): the division table + tempo sync, the per-sample control smoothing, the crossfading two-head PITCH `Shifter`, the `read_buf`/`read_rev` reads (Freeze + Reverse pad gestures), and the Stereo / DoubleMono / Ping-pong routing in `process()`.

New to this flavor:

- **Diffuse** routes the per-tap feedback through a shared stereo diffuser. The diffuser is [`src/dsp/diffuser.h`](../../src/dsp/diffuser.h), a header-only, JUCE-free port of qdelay's `Diffusor` (8 cascaded Schroeder allpasses per channel, L/R coefficient tables detuned for width). It has **no heap in the audio path**: `Diffuser::capacity_floats(sr)` reports the float count both channels need and `init(mem, sr)` sub-allocates every stage buffer from one caller-supplied block (the SDRAM arena, after the two delay lines). `set_size(0..1)` maps SIZE to the allpass read-tap offset depth (qdelay's `0.9 - 0.9*size`). A null `mem` degrades to a clean passthrough.

- **Duck** attenuates the heard wet by the input level. Each `Tap` runs a tiny inline peak follower (`duck_env`, fast-attack / slow-release one-pole over `|x|`); `duck_gain()` returns `1 - duck_env·kDuckAmt` clamped to `[kDuckFloor, 1]` and `write_out()` applies it to the wet term only. The **feedback loop is not ducked**, so the tail keeps building under a busy input and is revealed in the gaps. (The shared `src/dsp/follower.h` was not reused: it is mean-square based with a large `amp^2·1000` scaling tuned as a sidechain trigger, which is awkward for a smooth wet duck.)

### Diffuser routing detail

`process()` runs the read/colorize → (diffuse) → write split so the stereo diffuser can sit between the two taps' read and write. `read_color()` does the forward/reverse read + the ENV tone low-pass only; the engine then runs the diffuser over the `(fa, fb)` feedback pair **once per sample** and each tap takes the diffused copy only if *it* is in Diffuse mode (`diffA`/`diffB`). That lets DoubleMono mix a Diffuse deck with a Clean/Duck deck, and keeps the diffuser off the per-sample path entirely when neither deck is in Diffuse (`any_diffuse`). The diffuser's `set_size` tracks deck A's SIZE so the smear scales with the musical division.

## Memory / cost

Normal SRAM build. The two ~6 s delay lines and the diffuser (~235 KB of allpass buffers at 48 kHz) all live in the SDRAM arena, so SRAM_EXEC is unaffected — the ARM link reports ~77 % SRAM_EXEC (`186 KB` region), comfortable headroom. The diffuser adds ~16 allpass taps/sample (cheap linear-interp reads) only while a Diffuse deck is active.

## Tests

- `make -C host test-qdelay` (`host/test_qdelay.cpp`): a fed-back delay rings out and more feedback lengthens the tail; the three characters are distinct + finite; **Diffuse** differs from Clean and stays bounded; **Duck** suppresses the wet under continuous input (steady wet energy collapses ~1500 → ~0) then the tail rings out once the input stops; ping-pong cross-feeds while DoubleMono does not; **Reverse** differs from forward, rings out, stays bounded, and toggles back exactly; every character stays finite + bounded at maximum feedback.

- `make -C host test-diffuser` (`host/test_diffuser.cpp`): the diffuser standalone — an impulse smears into a decaying tail; `clear()` returns it to the initial state exactly; `set_size` changes the response; the L/R detune decorrelates identical mono input; and a null memory block is a clean passthrough.

## License

GPLv3, not MIT like the rest of the repo. `src/dsp/diffuser.h` is a derivative of [qdelay](https://github.com/tiagolr/qdelay)'s GPLv3 `Diffusor` (the allpass topology + coefficient tables are copied), so it and `src/engine/qdelay/` are GPLv3, and any `ENGINE=qdelay` firmware is a combined work distributed under GPLv3. The GPLv3 source files carry `SPDX-License-Identifier: GPL-3.0-only` headers; the full license text and rationale are in [`src/engine/qdelay/LICENSE`](../../src/engine/qdelay/LICENSE) and [`NOTICE.md`](../../src/engine/qdelay/NOTICE.md). The Reverse gesture and the duck follower were written independently (standard technique) and are not affected.

## Files

`src/engine/qdelay/qdelay_engine.{h,cpp}` + the shared header-only `src/dsp/diffuser.h`; build via `engine_select.h` (`SPK_ENGINE_QDELAY`) + `Makefile` (`ENGINE=qdelay`, `make engine-qdelay`). Host tests: `host/test_qdelay.cpp`, `host/test_diffuser.cpp`. Control diagram spec: `docs/diagrams/controls/qdelay.json`.

## Notes / possible improvements

- Diffuse only smears the feedback; a pre/post diffusion option (qdelay exposes both) would need a control the panel doesn't have.

- Duck depth/sensitivity (`kDuckAmt`/`kDuckFloor`) and the diffuser smear (`set_smear`, fixed at qdelay's 0.75) are compile-time constants; none of them has a free knob on the surface.

- The diffuser is a single stereo instance keyed off deck A's SIZE; a fully independent per-deck diffuser in DoubleMono would double the buffer budget for little musical gain.
