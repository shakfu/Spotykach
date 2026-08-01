# Web front-end: browser-based SD card builder + terminal

Status: **proposal, nothing built.** Scope agreed 2026-08-01 as two phases — an SD card builder/checker,
then a WebSerial terminal. In-browser DFU flashing is explicitly **out of scope** (see
[Non-goals](#non-goals)).

This documents what to build, what it is worth, and — more importantly — the three constraints that
decide whether it is worth building at all. Read [`../sd-card.md`](../sd-card.md) and
[`terminal-control.md`](terminal-control.md) first; this proposes a second front-end onto both, not new
capability.

## Why

The CLI tooling that landed for the SD card (TODO P1.5) solved the *knowledge* problem — eight folder
layouts, four audio formats, rules that fail silently — but left an *access* problem. To use it you need
a checkout, Python 3, and a decoder (ffmpeg, or cysox plus a libsox with the right format handlers
built in). For someone who bought a device and wants to put audio on a card, that is still a
developer's toolchain.

A browser needs none of it. And one browser API in particular changes the calculus:

> **`AudioContext.decodeAudioData` decodes mp3/flac/wav/ogg natively, identically on every machine.**

The entire decoder-backend apparatus in `scripts/sk_card.py` — the cysox probe, the ffmpeg fallback, the
sox fallback, the `find_format` per-file check, the whole [cysox libmad licensing
thread](https://github.com/shakfu/cysox) — exists because desktop audio decoding is inconsistent across
machines. In a browser it is one call with uniform support. The web version of `convert` is *simpler
than the CLI version*, not a reimplementation of it with extra steps.

The terminal is a smaller win but nearly free once the app exists: the device enumerates as a standard
USB CDC serial port, so WebSerial can talk to it with no driver and no pyserial.

## Non-goals

- **Firmware compilation.** Needs the ARM toolchain. Stays in `make`.
- **DFU flashing.** Technically possible with WebUSB, but the [Daisy Web
  Programmer](https://electro-smith.github.io/Programmer/) already flashes this hardware, so the
  marginal value is low against a failure mode (a half-written image) that is the worst in the system.
  Link to theirs.
- **Replacing the CLI.** `make sdcard` stays the release path — it must remain stdlib-only Python so
  `make dist` works with no venv. The web app is an additional consumer of the same rules.
- **Formatting the card.** Browsers cannot. The user formats FAT32 first, as today.

## The three constraints that decide this

### 1. Chromium only

WebSerial, WebUSB and the File System Access API are absent from Firefox and Safari, with no signal that
either will ship them. For a music-hardware audience skewing Mac, "does not work in Safari" is a real
cost, not a footnote.

Mitigation, and it is a decent one: the **card builder degrades gracefully**. Without File System Access
it can still accept files via drag-and-drop and hand back a **generated `.zip`** the user unpacks onto
the card — which works in every browser. Only *in-place* card editing and the terminal are
Chromium-locked. Design for the zip path first and treat direct card access as an enhancement, so the
app is useful everywhere and better in Chrome.

### 2. The terminal does not exist on released firmware

`scripts/build_release.py:176` runs `make ENGINE=<e> SPK_VERSION=<v>` and nothing else — **no
`TERMINAL=1`**. Every binary in `dist/` therefore has no command channel, so a web terminal would be
useful only to people who build their own firmware, which is the audience that already has `skterm.py`.

Shipping terminal-enabled releases is a firmware decision with real costs, and they differ per engine:

- **~19–25 KB of SRAM_EXEC** for the USB-device CDC stack. Since the `SRAM_EXEC` rebalance every engine
  hosts it with margin, but `pstretch` is on its own linker script for exactly this class of reason —
  re-check it rather than assuming.
- **USB MIDI is lost, but only on the QSPI engines.** `USB_MIDI` defaults on only for `APP_TYPE=BOOT_QSPI`
  (`Makefile:412`), and `MidiUsbHandler` claims the same OTG_HS core the panel terminal jack uses — the
  conflict the `#error` in `src/terminal/terminal.h:31` guards. So `chuck`/`csound`/`mosc` would have to
  choose terminal *or* USB MIDI; the ~15 SRAM engines have `USB_MIDI=0` by default and lose nothing.

Three options, in preference order:

1. **Ship both variants** for the engines where it is free (`sk-<engine>-<version>.bin` and
   `-terminal.bin`). Doubles the artifact count and the build time; costs nothing else.
2. **Ship terminal-enabled by default** for non-QSPI engines only, leaving QSPI engines MIDI-capable.
   One artifact per engine, but a silent capability difference between engines.
3. **Do not ship it**, and scope the web terminal to developers. Then phase 2 is optional.

**This decision gates phase 2 and should be made before it starts, not during.** Phase 1 does not
depend on it.

### 3. One source of truth for the card rules

`scripts/card_layout.py` was written specifically so the layout table, the scan rules and the format
specs exist once. A hand-ported JavaScript copy would reintroduce exactly the drift it prevents, and
would rot silently — the firmware-parity tests in `scripts/test_sk_card.py` only guard the Python.

**Requirement: `card_layout.py` gains a `--json` export, and the web app consumes it as data.** The
build step writes `web/card_layout.json`; a test asserts the export round-trips. Nothing about the
layout is typed twice.

What genuinely must be reimplemented in JS is small and testable against fixtures the Python side
already has:

| Piece | Python | Notes |
|---|---|---|
| WAV header writer | `card_audio.write_wav` | ~30 lines; must emit the device's 44-byte header |
| raw writer | `card_audio.write_raw` | trivial |
| WAV header parser | `card_audio.parse_wav` | for `verify`; mirrors the firmware chunk walk |
| verify checks | `sk_card.verify_card` | the rules come from JSON; only the walk is new code |
| synthesis | `card_audio` tone/sweep/… | only if the web app also builds demo cards |

## Phase 1 — the card builder

A static page. No server, no build step beyond bundling.

**Verify.** Point it at a card (File System Access) or drop a folder onto it; it walks the tree and
renders the same findings the CLI produces — wrong format, name too long for the scan, under the 32 KB
floor, AppleDouble stubs, `key=value` config. This is the highest-value screen and needs no audio
decoding at all: it reads headers and filenames.

**Build.** Generate the folder skeleton, configs and per-folder `README.TXT`, plus the
`examples/{chuck,csound}` patches (bundled as data). Either written in place or offered as a zip.
Synthesized demo audio is optional here — the download already exists as a release artifact, so the web
app can simply link `sk-card-<version>.zip` rather than regenerate it.

**Convert.** The part that is genuinely better in a browser. Drop audio in, pick the target engine,
`decodeAudioData` → resample via `OfflineAudioContext` to the engine's rate/channel count → write with
the JS WAV/raw writer → save into the card folder. No install, no format-support lottery, and the
resampling is the browser's, which is well-tested.

Worth noting: `OfflineAudioContext` resampling is not bit-identical to libsox's or ffmpeg's. That is
fine — none of the three agree with each other now — but it means the web app is not a drop-in
replacement for reproducing a specific card byte-for-byte. Say so rather than implying parity.

## Phase 2 — the WebSerial terminal

Gated on constraint 2 above.

The transport is already trivial: USB CDC, VID `0x0483` (`tools/skdev/protocol.py:23`), line-oriented
ASCII, `ok …` / `err …` replies. Baud is irrelevant over CDC (`protocol.py:24` picks 115200 only because
pyserial demands a number). A `TextDecoderStream` over `port.readable` plus newline framing is most of
the client.

What makes it worth more than a serial monitor is that the protocol is **self-describing**: `describe`
returns the platform tables and the engine's liveness mask, so the UI can be *generated* rather than
hard-coded — a slider per advertised param with its real range, buttons for the pads the engine
implements, and nothing for the 24-id enum entries this engine ignores. That is the same descriptor
`tools/test_generic.py` already drives the hardware sweep from, so the semantics are proven.

Screens worth having, in order:

1. **Console** — type commands, see replies, with completion from `describe`'s vocabulary (`skterm.py`
   already does this against the same list).
2. **Generated control surface** — sliders/buttons from the descriptor. The thing a raw terminal cannot
   do and the reason to build a UI at all.
3. **CPU meter** — poll `query cpu` / `cpumin` / `cpumax` with a `reset cpu` button, plotted over time.
   The P2 bench workflow is currently "read numbers repeatedly and notice whether `max` is still
   climbing"; a plot answers convergence at a glance, which is precisely the question that mattered for
   `pstretch` at 8192.
4. **`query usb`** — render the `UsbDiag` bring-up snapshot as a readable table rather than a flag soup.

Do **not** put destructive verbs (`clear`, `preset save`, anything writing the card) behind a single
click without confirmation. `docs/dev/terminal-target-b.md:53` already flags that sweeping controls can
clear a buffer or write the card.

## Deployment

Static files → GitHub Pages, versioned with the firmware so `card_layout.json` matches the binaries it
sits beside. Must be served over HTTPS or `localhost` for the browser APIs to be offered at all. A
service worker makes it work offline, which matters for a tool people use next to hardware rather than
at a desk.

## Testing

The JS format writers must agree with the Python ones byte-for-byte, and that is checkable rather than
assumed: generate fixtures with `card_audio.py`, commit them, and assert the JS output matches. The
`verify` logic can be tested against the same deliberately-broken fixture tree
`scripts/test_sk_card.py` builds. Neither needs a browser — run them in Node.

The terminal client needs a fake device (a scripted `ReadableStream`/`WritableStream` pair) to test
framing and timeout handling without hardware, mirroring how `tools/conftest.py` skips cleanly when no
device is attached.

## Open questions

- **Does the resampling quality matter?** `OfflineAudioContext` is fine for tape/radio material; for
  `pstretch` sources feeding a 50× stretch, artefacts may be more audible. Untested.
- **Where does the app live** — this repo under `web/`, or its own? Same repo is simpler while
  `card_layout.json` is a build artifact of this one.
- **Is phase 2 worth it if the answer to constraint 2 is "do not ship terminal builds"?** Probably not;
  `skterm.py` already serves developers well. Decide constraint 2 first.
- **Should the web app build demo cards at all**, or only link the released zip? Linking is less code
  and guarantees the user gets the checksummed artifact.
