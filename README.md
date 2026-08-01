# sk-engines: A Spotykach (platform/engine fork)

A fork of the official [Synthux Academy Spotykach](https://synthux.academy/store/spotykach) firmware, restructured as a fixed hardware/UI **platform** with a swappable DSP **engine** architecture.

The hardware and interaction model remain constant across firmware variants: multi-function encoders with pickup behavior and LED ring feedback, pad gestures, transport controls, SD-card sample storage, CV/gate I/O, and MIDI support. Individual firmware builds replace only the DSP engine and its parameter set. Clocking and transport are also provided as shared platform services, allowing any engine to synchronize to the same internal, TS4, or MIDI clock sources.

The platform is intentionally decoupled from any specific engine. Core subsystems in `src/hw/`, `src/ui/`, `src/memory/`, and `src/transport/` contain no engine-specific dependencies, and build-time checks enforce this separation (`make check-boundary`).

Current engines include:

1. [granular](docs/engines/granular.md): dual-deck granular looper (the original reference spotykach firmware as an engine)

2. [delay](docs/engines/delay.md): tempo-synchronized stereo delay with switchable characters (Clean / Tape / Shimmer on the Reel/Slice/Drift switch) and stereo topologies (Stereo / DoubleMono / Ping-pong on the route switch), plus a feedback tone control, a modulation LFO (chorus/flange/vibrato), a Play-pad freeze, and a Rev-pad reverse

3. [edrums](docs/engines/edrums.md): four-voice Euclidean drum machine with tweakable synthesized drums

4. [reso](docs/engines/reso.md): resonator/plucked-string instrument based on the [Mutable Instruments Rings DSP code](https://github.com/pichenettes/eurorack/tree/master/rings)

5. [tape](docs/engines/tape.md): dual streaming tape deck (two independent record/playback decks, SD-streamed, no in-memory length cap)

6. [shuttle](docs/engines/shuttle.md): buffer-based bipolar/reverse varispeed tape (four in-RAM tracks, PITCH as a capstan-speed knob - noon stops, CW forward, CCW reverse)

7. [reverb](docs/engines/reverb.md): route-aware stereo reverb with three all-Faust algorithms (Dattorro plate / Zita-rev1 hall / Greyhole, generated from [Faust](https://faust.grame.fr) sources) - the Reel/Slice/Drift switch selects the algorithm, and DoubleMono routing runs an independent mono plate on each deck (the heavier hall/Greyhole stay single-voice in a stereo route, a CPU cap validated on hardware)

8. [gigaverb](docs/engines/gigaverb.md): stereo reverb authored in Max/MSP **gen~** and translated to C++ via [gen-dsp](https://github.com/shakfu/gen-dsp) (Tom Erbe's gigaverb)

9. [radio](docs/engines/radio.md): dual virtual [RadioMusic](https://github.com/TomWhitwell/RadioMusic) - two independent virtual radios (one per deck) over a shared SD library of banks, with the signature free-running "virtual playhead" (each station seems to keep broadcasting while you are tuned elsewhere): PITCH = station tuning, POS = start offset, Alt+PITCH = bank, SIZE = varispeed, ENV = inter-station static, Play/gate = re-tune. Streams headerless raw 16-bit-mono `.raw` files (the original RadioMusic format, at 48 kHz)

10. [chorus](docs/engines/chorus.md): stereo chorus - the demo of the **generated** Faust path (author a `.dsp` + a small JSON manifest, no hand-written C++)

11. [filter](docs/engines/filter.md): dual resonant filter, one independent voice per channel - the generated **parallel (DoubleMono) dual-deck** demo

12. [voice](docs/engines/voice.md): drone oscillator (deck A) into a resonant filter (deck B) - the generated **series (chain) dual-deck** demo (instrument -> FX)

13. [passthrough](docs/engines/passthrough.md): minimal stereo passthrough engine demonstrating the platform API

14. [csound](docs/engines/csound.md): a full [Csound](https://csound.com) 7 instance as a synth — the **patch** (a `.csd` orchestra) defines the sound. Load orchestras from the SD card and switch between them live with Alt+PITCH, and play them over MIDI. Unlike the other engines it is a **QSPI build** (Csound's ~2 MB of code is too big for SRAM) and needs a one-time `libcsound.a` cross-build (`scripts/fetch_csound.sh`)

15. [chuck](docs/engines/chuck.md): the [ChucK](https://chuck.stanford.edu) language + VM as a synth — the **patch** (a `.ck` program) defines the sound, with strongly-timed concurrent voices. Load programs from the SD card and switch between them live with Alt+PITCH (memory-stable: each patch compiles once, then switches are instant). Like csound it is a **QSPI build** with a one-time `libchuck.a` cross-build (`scripts/fetch_chuck.sh`); MIDI is planned

16. [mosc](docs/engines/mosc.md): a dual **macro-oscillator** giving each deck a full 24-engine [Mutable Instruments Plaits](https://github.com/pichenettes/eurorack/tree/master/plaits) voice (virtual analog, FM, wavetable, granular, additive, chord, speech, modal, drums, …). PITCH = note, Alt+PITCH = engine select (24 models), SIZE/POS/ENV = harmonics/timbre/morph, MOD_AMT/MODFREQ = LPG decay/colour; the Mode switch picks Gate vs Drone and the Routing switch combines the two voices (Stereo / DoubleMono / a GenerativeStereo out+aux spread). A **QSPI build** (the 24-engine voice is too big for SRAM) but it needs no SD card or external library — the Plaits DSP is vendored in-tree. Cloned from the reso engine (same author, shared `stmlib`)

17. [softcut](docs/engines/softcut.md): a dual-deck crossfaded **overdub looper** on monome's [softcut-lib](https://github.com/monome/softcut-lib) (4 voices, 2 per deck). Unlike the buffer-tape engines, softcut plays and records the same loop at once with subsample-accurate click-free crossfades and **interpolated overdub** — layering live input onto a running loop with an ENV-knob feedback control (sound-on-sound). It's a **record-defines-loop** looper: on an empty voice the first Alt+Play records a fresh take and the second closes the loop at the length you played; a voice with content overdubs on top. PITCH = bipolar varispeed/reverse, POS/SIZE = loop window, ENV = overdub feedback, the FLUX pad = a sweepable filter, MOD_AMT/MODFREQ = loop crossfade time / rate slew; Seq realigns all voices (click-free `cutToPos` sync), Alt+PITCH loads loop clips from SD, and **Alt+Seq / Alt+Rev save** the full take / trimmed loop back to the card. A normal **SRAM build** (the softcut DSP is tiny); 4 voices is the hardware-measured CPU budget, with 10.9 s buffers per voice. The vendored core lives in `src/engine/softcut/vendor`

18. [qdelay](docs/engines/qdelay.md): a dub/ambient **flavor of the delay** (inspired by [qdelay](https://github.com/tiagolr/qdelay)) — the same delay grammar (tempo-synced divisions, feedback, mix, PITCH, ENV tone, mod LFO, Freeze/Reverse pads, Stereo/DoubleMono/Ping-pong) with the character palette swapped to **Clean / Diffuse / Duck** (Reel/Slice/Drift switch). Diffuse runs the feedback through an 8-stage allpass diffuser (a JUCE-free port of qdelay's `Diffusor`) for a dense reverb-like wash; Duck attenuates the wet under the dry input so repeats bloom in the gaps. A normal **SRAM build** (the diffuser's buffers live in SDRAM)

Four further engines live in the tree and build the same way, but are not (yet) part of the numbered list above: [bard](docs/engines/bard.md) (bookmark-navigated audiobook decks, SD-streamed), [glitch](docs/engines/glitch.md) (dual-deck lo-fi/circuit-bent noise voice, GPLv3), [pstretch](docs/engines/pstretch.md) (real-time PaulStretch ambient time-smear), and [graincloud](docs/engines/graincloud.md) (a granular-cloud variant of the looper).

### At a glance

| Engine | Type | SD card | Build | Authored in | Notes |
| --- | --- | --- | --- | --- | --- |
| [granular](docs/engines/granular.md) | Looper / sampler | Optional (save/load loops) | SRAM | C++ | Default build; the original firmware as an engine |
| [graincloud](docs/engines/graincloud.md) | Looper / sampler | Optional (save/load loops) | SRAM (`-Os`) | C++ | Granular variant with a GrainflowLib cloud |
| [tape](docs/engines/tape.md) | Looper / recorder | **Required** (streams) | SRAM | C++ | Two SD-streamed decks, no length cap |
| [shuttle](docs/engines/shuttle.md) | Looper | Optional (load slots) | SRAM | C++ | Four in-SDRAM tracks, bipolar varispeed |
| [softcut](docs/engines/softcut.md) | Looper (overdub) | Optional (load/save clips) | SRAM | C++ | Vendored monome softcut-lib, 4 voices |
| [radio](docs/engines/radio.md) | Player / instrument | **Required** (streams) | SRAM | C++ | Dual virtual RadioMusic, `.raw`/`.wav` banks |
| [bard](docs/engines/bard.md) | Player | **Required** (streams) | SRAM (`-Os`) | C++ | Audiobook decks; not yet hardware-tested |
| [delay](docs/engines/delay.md) | Effect | — | SRAM | C++ | Clean / Tape / Shimmer characters |
| [qdelay](docs/engines/qdelay.md) | Effect | — | SRAM | C++ | Clean / Diffuse / Duck; **GPLv3** |
| [reverb](docs/engines/reverb.md) | Effect | — | SRAM (`-Os`) | Faust | Plate / hall / Greyhole, route-aware |
| [gigaverb](docs/engines/gigaverb.md) | Effect | — | SRAM | gen~ | Tom Erbe's gigaverb via gen-dsp |
| [chorus](docs/engines/chorus.md) | Effect | — | SRAM | Faust | Generated single-deck demo |
| [filter](docs/engines/filter.md) | Effect | — | SRAM | Faust | Generated parallel (DoubleMono) demo |
| [pstretch](docs/engines/pstretch.md) | Effect | Optional (file source) | SRAM | C++ | Clean-room PaulStretch, vendored FFT |
| [edrums](docs/engines/edrums.md) | Instrument | — | SRAM | C++ | Four-voice Euclidean drum machine |
| [reso](docs/engines/reso.md) | Instrument (+ resonator) | — | SRAM (`-Os`) | C++ | Mutable Instruments Rings DSP |
| [mosc](docs/engines/mosc.md) | Instrument | — | **QSPI** | C++ | Dual 24-engine Plaits voice, vendored in-tree |
| [glitch](docs/engines/glitch.md) | Instrument | — | SRAM | C++ | 12 Noisferatu algorithms; **GPLv3** |
| [voice](docs/engines/voice.md) | Instrument | — | SRAM | Faust | Generated series (osc → filter) demo |
| [csound](docs/engines/csound.md) | Instrument (scriptable) | Optional (`.csd` patches) | **QSPI** | C++ host + Csound | Needs a one-time `libcsound.a` cross-build |
| [chuck](docs/engines/chuck.md) | Instrument (scriptable) | Optional (`.ck` patches) | **QSPI** | C++ host + ChucK | Needs a one-time `libchuck.a` cross-build |
| [passthrough](docs/engines/passthrough.md) | Utility | — | SRAM | C++ | Minimal stereo passthrough |

**Build** is where the code executes: *SRAM* is the normal `make ENGINE=<name>` build; *QSPI* engines are too large for the 186 KB execution SRAM and run from external flash via their own one-shot target (`make engine-<name>`). *SD card* marks whether the card is required at runtime, only used for optional load/save, or unused. `-Os` notes engines built size-optimized to fit.

Engines can be authored in three ways:

1. Using [C++](docs/engine-types/cpp.md) against `IEngine`

2. Using [**Faust**](docs/engine-types/faust.md) (via [cyfaust](https://github.com/shakfu/cyfaust))

3. Using Max/MSP's [**gen~**](docs/engine-types/gen.md) language (via [gen-dsp](https://github.com/shakfu/gen-dsp))

The latter two generate C++ that the platform wraps behind the same contract. The three methods are documented in [`docs/engine-types/`](docs/engine-types/).

Originally started as a feature-extension fork of the upstream firmware, the project evolved into a platform/engine architecture that enables new instruments to reuse the existing hardware and interaction language rather than reimplement them. See [`docs/architecture.md`](docs/architecture.md) for an overview of the design and instructions for creating new engines.

## Setup

Clone the repo recursively or run `git submodule update --init --recursive` to update submodules (libDaisy + DaisySP)

Note that the ws2812 driver requires a slight modification to libDaisy, as such the libDaisy submodule commit ref points at a specific branch within the bleeptools fork (which is based on Infrasonic Audio fork), containing a few other modifications as well, namely within the MIDI and mpr121 classes.

## Compiling the Firmware

First you must build the libraries. For convenience there is a target in the `Makefile` for this, so you simply need to run the following command from a terminal (in the root directory of this repository):

`make -j8 libs`

_Note: The `-j8` flag runs make with 8 parallel jobs, feel free to change the number. It works best if you use the number of cores (including hyperthread cores) on your machine._

Then, build the actual firmware code:

`make -j8`

If successful the compiled binaries will end up in the `build/` directory along with many other intermediate build files:

```text
spotykach.bin
spotykach.elf
```

The `.elf` file is mainly used for debugging. The `.bin` file is the one that the DFU utilities will flash onto the Seed.

### Build options

The firmware is a fixed hardware/UI **platform** that hosts a swappable DSP **engine**, chosen at build time with the `ENGINE` variable:

- `make -j8` — the granular looper (default; `ENGINE=granular`).

- `make -j8 ENGINE=delay` — a tempo-synced stereo delay (musical divisions, feedback, pitch-shifted taps) with **Clean / Tape / Shimmer** characters (Reel/Slice/Drift switch), **Stereo / DoubleMono / Ping-pong** topologies (route switch), a feedback tone control on ENV, a delay-time **modulation LFO** (MODFREQ/MOD_AMT → chorus/flange/vibrato), a **Freeze** hold on the Play pad, and a **Reverse** on the Rev pad.

- `make -j8 ENGINE=qdelay` — a dub/ambient **flavor of the delay** (inspired by [qdelay](https://github.com/tiagolr/qdelay)): the same control grammar with the character palette swapped to **Clean / Diffuse / Duck** (Reel/Slice/Drift switch). **Diffuse** runs the feedback through an 8-stage allpass diffuser (a JUCE-free port of qdelay's `Diffusor`) for a dense, reverb-like wash; **Duck** attenuates the wet under the dry input so the repeats bloom in the gaps. Reuses the delay's tempo sync, PITCH transpose, ENV tone, mod LFO, Freeze/Reverse pads and the three topologies. A normal SRAM build (the diffuser's buffers live in SDRAM).

- `make -j8 ENGINE=edrums` — a four-drum Euclidean drum machine (two drums per deck, Rev-pad swaps the editable one; polymeter, live model select) with **synthesized voices you shape live**: per-drum gain, decay, and grit/flux macros for drive, pitch-sweep, brightness, and body↔noise balance.

- `make -j8 ENGINE=reso` — a resonator/pluck voice on the Mutable Instruments Rings DSP (modal / sympathetic-string / string / FM / string+reverb models on Alt+PITCH; three excite modes — discrete plucks, live-input resonator, scatter cloud). Vendored Rings/stmlib live under `src/engine/reso/thirdparty/`.

- `make -j8 ENGINE=tape` — two independent mono **tape decks** (A/B) that play and record arbitrarily long takes to the SD card, removing the in-SDRAM loop-length cap. Per deck: Play pad = play, Alt+Play = record, PITCH = varispeed, Alt+POS = pan, MIX = volume, ENV = loop mode (none / loop / faded / Frippertronics), Alt+PITCH = tape-slot select (8 slots under `/tapes/`); the routing switch and mix fader place/blend the two decks. Streams mono float WAV through lock-free per-deck SDRAM rings drained by a main-loop FatFs pump.

- `make -j8 ENGINE=shuttle` — a **buffer-based bipolar/reverse varispeed tape**: four in-RAM mono tracks (two per deck), all playing at once. PITCH is a capstan-speed knob (noon = stop, clockwise = forward to +2x, counter-clockwise = reverse to -2x); the Play pad snaps the focused track to unity. POS/SIZE set a per-track loop window, Alt+PITCH loads a `/tapes/` slot into RAM, the Rev pad swaps a deck's focused track, and the Seq pad re-aligns all four tracks to a common downbeat (declicked). Random-access in-SDRAM buffers (30 s/track) trade unbounded length for trivial reverse/freeze/looping.

- `make -j8 ENGINE=radio` — a **dual virtual [RadioMusic](https://github.com/TomWhitwell/RadioMusic)**: two independent virtual radios (one per deck) browsing a shared `/radio/<bank>/` SD library, blended by the crossfader and routing switch. Its signature is the **free-running virtual playhead** — every station seems to keep broadcasting while you are tuned elsewhere (a per-deck frame clock seeks each opened station to `(clock + START) mod length`). **PITCH** tunes stations (+ V/oct CV), **POS** sets the start offset (+ CV), **Alt+PITCH** picks the bank, **SIZE** adds 0.5–2x varispeed, **ENV** brings in inter-station static, **Play pad / gate-in** re-tune. Streams **16-bit-mono `.raw`** (headerless, the original RadioMusic format) or **`.wav`** (self-describing, carries its own rate) — mix freely in a bank; convert with [`scripts/convert_radio_audio.py`](scripts/convert_radio_audio.py).

- `make -j8 ENGINE=reverb` — a **route-aware** stereo reverb with **three all-Faust algorithms** (a Dattorro plate, a Zita-rev1 hall, and a Greyhole, the **Reel/Slice/Drift switch** selects live), generated from [Faust](https://faust.grame.fr) sources by cyfaust. **DoubleMono** routing runs an independent mono plate per deck; the heavy hall/Greyhole are single-voice (stereo-route only), a cap that keeps two delay-line-heavy voices off the SDRAM bus at once. Built at `-Os` (Greyhole is the heaviest voice). Regenerate the kernels with `make faust-kernels`.

- `make -j8 ENGINE=gigaverb` — a stereo reverb (Tom Erbe's **gigaverb**) authored in Max/MSP **gen~** and translated to C++ by [gen-dsp](https://github.com/shakfu/gen-dsp). The engine directory is generated from a gen~ export with `make gen-engines` (or `scripts/gen_engine.py`); see [`docs/engine-types/gen.md`](docs/engine-types/gen.md).

- `make -j8 ENGINE=chorus` / `filter` / `voice` — engines **generated** from a Faust `.dsp` + a small JSON manifest with no hand-written C++ (`make faust-engine MANIFEST=…`): **chorus** (a stereo chorus, the single-deck demo), **filter** (a resonant filter per channel, the parallel dual-deck demo), and **voice** (a drone oscillator into a filter, the series dual-deck demo). See [`docs/engine-types/faust.md`](docs/engine-types/faust.md).

- `make -j8 ENGINE=passthrough` — a minimal stereo-passthrough variant.

- `make engine-csound` — a full **[Csound](https://csound.com) 7** synth, where the **patch defines the sound**: load `.csd` orchestras from the SD card, switch between them live with Alt+PITCH, and play over MIDI. This one is special — a **QSPI** build (Csound's ~2 MB of code is too big for SRAM, so it executes from flash), built with its own one-shot target rather than `ENGINE=`. It needs a one-time `scripts/fetch_csound.sh` to fetch + cross-build `libcsound.a`. See [`docs/engines/csound.md`](docs/engines/csound.md). Ready-to-copy example orchestras are in [`examples/csound/`](examples/csound/).

- `make engine-chuck` — the **[ChucK](https://chuck.stanford.edu)** language + VM as a synth, where the **patch defines the sound**: load `.ck` programs from the SD card and switch between them live with Alt+PITCH. Like csound it is a **QSPI** build with its own one-shot target and a one-time `scripts/fetch_chuck.sh` to cross-build `libchuck.a`. Patches compile once and are cached, so live switching is instant and memory-stable. See [`docs/engines/chuck.md`](docs/engines/chuck.md); example programs in [`examples/chuck/`](examples/chuck/).

- `make engine-mosc` — a dual **macro-oscillator**: each deck is a full 24-engine **[Mutable Instruments Plaits](https://github.com/pichenettes/eurorack/tree/master/plaits)** voice (PITCH = note, Alt+PITCH = engine select, SIZE/POS/ENV = harmonics/timbre/morph; Mode = Gate/Drone, Routing = Stereo / DoubleMono / GenerativeStereo). Like csound/chuck it is a **QSPI** build (the 24-engine voice is too big for SRAM) with its own one-shot target — but it needs **no** SD card or library fetch: the Plaits DSP is vendored in-tree. See [`docs/engines/mosc.md`](docs/engines/mosc.md).

Switching `ENGINE` does not require `make clean`. Other build flags: `DEBUG=1` (enables UART logging) and `LOFI_INT16=1` (16-bit loop buffer, doubling record time). See [`docs/architecture.md`](docs/architecture.md) for the platform/engine design, [`docs/engines/`](docs/engines/) for a per-engine reference, and [`docs/engine-types/`](docs/engine-types/) for the three ways to author an engine (native C++, Faust, gen~).

There is also an **opt-in CMake build** (an in-progress alternative; the `make` build above stays canonical): `make -f Makefile.cmake ENGINE=<engine>` configures and builds via CMake, with output in `build-cmake/<engine>/` instead of `build/`. It mirrors the same commands (`program-dfu`, `engine-<name>`, `DEBUG=1`, `LOFI_INT16=1`) and caches each engine in its own dir, so switching engines never forces a rebuild.

### Editor tooling (clangd)

The repo's includes (the libDaisy header set, `-Isrc`, the build-time `-DSPK_ENGINE_*` define) aren't discoverable by clangd on their own, so generate a `compile_commands.json` from a real build:

`bear -- make -j8`

It is git-ignored and is a snapshot of whichever `ENGINE` you built (granular by default) — regenerate after adding files or changing flags.

## Flashing the Firmware

The bootloader version used in this project enables USB DFU firmware updating from the _external_ USB port - i.e. the USB-C port on the rear of the main PCB, NOT the one on the Seed. Application firmware can only be flashed using the USB-C port.

1. Compile the firmware using the steps above

2. Connect the USB-C connector on the main PCB to the computer (ensure the cable is not power-only)

3. Hold the `Reset` button on the back of the unit for ~3 seconds. The leds under bottom pads going to "breathe" in white.

4. Run the command `make program-dfu` from a terminal

`make program-dfu` flashes whatever is currently in `build/` (it does not rebuild). To flash a non-default engine, build it first in the same step, e.g. `make ENGINE=passthrough && make program-dfu`.

For convenience there are one-shot targets that **clean + build + flash** a variant (put the device in DFU mode first, as in step 3): `make engine-granular` (the looper), `make engine-delay`, `make engine-qdelay`, `make engine-edrums`, `make engine-reso`, `make engine-tape`, `make engine-shuttle`, `make engine-radio`, `make engine-reverb`, `make engine-chorus`, `make engine-filter`, `make engine-voice`, `make engine-gigaverb`, and `make engine-passthrough`. (`make engine-csound`, `make engine-chuck`, and `make engine-mosc` also exist but are QSPI builds — csound/chuck need a one-time `libcsound.a` / `libchuck.a` prerequisite, mosc needs none — see [`docs/engines/csound.md`](docs/engines/csound.md), [`docs/engines/chuck.md`](docs/engines/chuck.md), [`docs/engines/mosc.md`](docs/engines/mosc.md).)

Once finished, the device will automatically boot the new firmware. This can "brick" (temporarily) the device and require reinstallation of either the bootloader, the firmware binary, or both.

## The SD card

Ten engines read the card, using eight folder layouts and four incompatible audio formats — and the firmware converts nothing, so a wrong-format file plays as noise rather than being rejected. You do not have to learn that: build a correct card, and check one that misbehaves.

```sh
make sdcard SDCARD_OUT=/media/SK          # build a complete card (folders, configs, patches, demo audio)
make check-sdcard CARD=/media/SK          # explain anything that will not work, with the fix
python3 scripts/sk_card.py convert --engine tape /media/SK loop.mp3   # add your own audio
```

A prebuilt `sk-card-<version>.zip` ships with each release. See [`docs/sd-card.md`](docs/sd-card.md).

## Testing

There are two independent suites: one that runs on your machine, and one that drives a flashed device.

### Off-target (no hardware)

The engines and the platform's hardware-free layers compile for the host against a small `<daisy.h>` shim, so most DSP and all of the control-plane logic is testable without a device:

```sh
make -C host test      # engine + DSP suites (delay, tape, reso, granular, csound, the terminal codec, ...)
make -C test test      # small standalone unit tests (wav, config, dividers, ...) - 116 checks
```

Note that a bare `make test` in the repo root does **nothing** — it matches the `test/` directory rather than a target. Use the two commands above.

### On-target (a flashed device)

`TERMINAL=1` builds in a bidirectional text/command channel over the rear USB-C port — the same port used for DFU — which lets a host script drive an engine and assert on what it reports. It is opt-in and costs nothing when off: the terminal translation units compile to zero bytes and a build without the flag is byte-identical.

```sh
make -j8 ENGINE=delay TERMINAL=1     # then flash as described above
make test-hw                         # pytest harness over USB-C
```

`make test-hw` needs `pyserial` and `pytest`; it picks up a project virtualenv (`.venv`) if there is one, and **skips cleanly when no device is attached**, so it is safe to leave in a pipeline. With a device it drives every parameter the engine declares, across both decks, inside an input-isolated mode where knobs, CV, gate and switches cannot perturb the run.

The tests are generic: they are driven entirely by the device's own `describe` output, so one file tests every engine build. Nothing is hardcoded per engine.

For poking at a device by hand there is a REPL:

```sh
python tools/skterm.py
sk> describe                 # the device's whole control surface
sk> set param feedback A 0.75
sk> get param feedback A     # -> ok 0.7500
sk> query empty A            # engine state
```

The channel and its design are documented in [`docs/dev/terminal-control.md`](docs/dev/terminal-control.md) (what and why), [`docs/dev/terminal-dispatch.md`](docs/dev/terminal-dispatch.md) (the command grammar), [`docs/dev/terminal-tools.md`](docs/dev/terminal-tools.md) (the host side) and [`docs/dev/terminal-impl.md`](docs/dev/terminal-impl.md) (what actually landed, including the hardware bring-up history).

## Architecture & developer docs

Firmware internals are documented under [`docs/`](docs/) — start with [`docs/architecture.md`](docs/architecture.md), which covers the hardware platform, the platform/engine seam (`IEngine`), and how to slot in a new engine. [`docs/engines/`](docs/engines/) documents each engine in detail plus the shared transport and knob-routing model, and [`docs/engine-types/`](docs/engine-types/) covers the three engine-authoring methods (native C++, Faust/cyfaust, gen~/gen-dsp). Notable changes are tracked in [`CHANGELOG.md`](CHANGELOG.md).

## License

This project is **MIT** (see [`LICENSE`](LICENSE)), **except the `qdelay` and `glitch` engines**, which are **GPLv3**:

- **`qdelay`** — `src/dsp/diffuser.h` is a port of [qdelay](https://github.com/tiagolr/qdelay)'s GPLv3 `Diffusor`, so that file and `src/engine/qdelay/` (and any firmware built with `ENGINE=qdelay`) are distributed under GPLv3 — see [`src/engine/qdelay/NOTICE.md`](src/engine/qdelay/NOTICE.md) and [`src/engine/qdelay/LICENSE`](src/engine/qdelay/LICENSE).
- **`glitch`** — `src/engine/glitch/glitch_voice.h` ports the algorithms of the GPLv3 [Noisferatu](https://github.com/rob-scape/noisferatu), so `src/engine/glitch/` (and any firmware built with `ENGINE=glitch`) is distributed under GPLv3 — see [`src/engine/glitch/NOTICE.md`](src/engine/glitch/NOTICE.md) and [`src/engine/glitch/LICENSE`](src/engine/glitch/LICENSE).

Every other engine and the platform itself remain MIT and do not include the GPLv3 code. Vendored third-party DSP keeps its own upstream license alongside the code (e.g. `stmlib`).
