# Terminal control channel (USB-C)

Status: **design sketch, unbuilt.** No code exists yet. This documents a proposed general capability - a bidirectional text/command channel over the Daisy Seed's USB-C port - usable across all engines for runtime control and, equally, for observing and driving the hardware from a host script. Everything is gated behind a compile-time flag and costs nothing when off.

## Why

Two motivations, the second arguably stronger than the first:

- **Runtime control.** A host can set parameters, flip config, trigger pads/transport on the device without a knob or a MIDI controller - scriptable, human-typable, engine-agnostic.

- **Hardware testing.** The channel turns the board into something a host script can *observe and drive*. Today `make test` is entirely off-target (`host/` unit tests); a terminal is the missing on-target complement - it reads raw ADC/CV/touch/gate state and drives DAC/gate/LED outputs so a host can assert on real electrical behaviour. It also replaces two existing workarounds: the one-way `Expose` print pipe (`src/expose.h`) and the `CHUCK_BRINGUP` LED-blink boot markers (`src/app.cpp:71-82`), which exist only because the panel is unobservable during `Init()`.

## The one hard constraint: the Logger already owns USB-C

The USB-C connector is the STM32H750 USB OTG FS **internal** peripheral (`FS_INTERNAL`). The Daisy Logger already owns it TX-only (`src/common.h:37` `INFS_LOG_TARGET = LOGGER_INTERNAL`, started at `src/app.cpp:213`). Two `UsbHandle::Init(FS_INTERNAL)` calls on the same peripheral collide, so the terminal service must **own the internal CDC bidirectionally** and log output must flow through it. That is a feature, not a fight: one CDC stream becomes both console output and command input - exactly what a terminal is. When `SPK_TERMINAL` is off, nothing changes and the Logger keeps the port.

(The `METER` build's CDC writer is a separate concern - it uses `FS_EXTERNAL` (GPIO31/32, which also collide with the gate outputs) and is debug-only. Only one of these can be the console; they do not share the internal port.)

## Layering

Four layers; only the bottom two touch hardware. The command model is deliberately **codec-agnostic**, so line-ASCII vs OSC is a compile flag, not a rewrite.

```
  host tooling        dumb terminal | python REPL | web-serial GUI
  ----------------------------------------------------- USB-C CDC ------
  [4] targets         (A) platform-reflective  (B) engine verbs  (C) HAL probe/actuate
  [3] dispatch        Command{ target, verb, argv[] }  ->  one of the three targets
  [2] codec/framing   line-ASCII (default)  |  OSC + SLIP (opt-in)
  [1] transport       UsbHandle CDC on FS_INTERNAL  +  SPSC ring buffer
```

## Data flow and threading

Thread-safe by construction - the receive callback does the minimum, everything else runs on the main loop where control input already lives.

```
  USB IRQ  --ReceiveCallback-->  SPSC ring buffer (lock-free, raw bytes)   [producer: ISR]
                                          |
  main Loop() --drain--> line/SLIP assembler --> codec decode --> dispatch [consumer: main loop]
```

- The receive callback (`UsbHandle::SetReceiveCallback`, `lib/libDaisy/src/hid/usb.h:38`) runs in USB interrupt context and does **nothing but copy bytes into a single-producer/single-consumer ring**. No parsing, no allocation, no engine calls in the ISR.

- Assembly, decode, and dispatch run in `Loop()` (`src/app.cpp:243`), beside `_ui.process()` and `_stream.process()`. One added `terminal.process()` call, guarded by `#if SPK_TERMINAL`.

- This is the **same execution context `handle_midi_message` already uses** (main loop, per `src/engine/iengine.h:92`). So terminal writes inherit MIDI's exact concurrency contract with the audio ISR - no new hazard on the *control* path. The *actuation* path is different and is the subject of the test-mode section below.

## Three command targets

The dispatcher routes a decoded `Command` to one of three targets. The first two are the control sketch; the third is what hardware testing adds.

### A. Platform-reflective - free for every engine

`IEngine` already exposes a generic control surface: `set_param(ParamId, deck, float)`, `param()`, `set_config(ConfigId, deck, int)`, `set_fx`, transport (`src/engine/iengine.h:52-97`). The dispatcher handles these centrally against `IEngine`, so commands like

```
set param filter A 0.7
get param mix
config mode A 2
```

work on **all engines with zero per-engine code**. This is the general capability - it lands the moment the channel exists.

### B. Engine-specific verbs - opt-in

One new virtual on `IEngine`, consistent with the interface-lift philosophy ("an engine overrides only what it supports", `src/engine/iengine.h:24`):

```cpp
// iengine.h - one addition, no-op default
virtual bool handle_command(const CommandView& cmd, TextSink& reply) { return false; }
```

`CommandView` is a span over the already-tokenized argv (verb + args), so the engine never touches the codec. Returning `false` -> dispatcher reports "unknown command". A `CapTerminal` bit in the existing `Capabilities` bitmask advertises that an engine has custom verbs (used by `help`/`describe`).

### C. HAL probe/actuate - the hardware-test target

This target sits **below the engine**, against `Hardware` (`src/hw/hardware.h`). The probes you want already exist as dead code inside `logDebugInfo` (`src/app.cpp:310-313`):

```cpp
hw.GetAnalogControlValue(Hardware::CTRL_PITCH_A);   // ADC / pots
hw.GetControlVoltageValue(Hardware::CV_V_OCT_A);    // CV inputs
hw.GetMpr121TouchStates();                          // touch pads
```

Those, plus gate in/out, the 74HC165 shift registers, WS2812 rings, and DAC CV out, form a probe/actuate vocabulary that is engine-independent:

- **Probe (read).** Safe from the main loop, benign:
  ```
  probe adc pitch_a          -> 0.734
  probe cv voct_a            -> +1.02v
  probe touch                -> 0b0000100100
  probe gate_in a            -> 1
  probe shift 0              -> 0xA3
  probe expose               -> P1=.. P3=..   (replaces the throttled Expose spew)
  ```

- **Actuate (drive).** Drives an output directly to verify the electrical/optical path, decoupled from the engine. **Not safe without arbitration** (next section):
  ```
  set dac 0 2.5v
  pulse gate_out a 5ms
  led ring_a 3 ff0000
  ```

Probe/actuate subsumes `Expose` (on-demand instead of throttled) and the `CHUCK_BRINGUP` blink markers (a text boot trace - `[boot] fmc ok / sdram ok / engine.init ok` - beats counting LED flashes).

## Test-mode arbitration - the real hazard

Probes are read-only and safe. **Actuation is not**, because the outputs are already owned by the running system:

- LEDs are rendered by `T5Callback` -> `render_leds()` every ~4th tick (`src/app.cpp:121-128`); a terminal `led ...` write is overwritten on the next render.

- The DAC is filled every block by `DACCallback` -> `_engine.process_cv()` (`src/app.cpp:145-148`).

- Gate out is driven by the engine's `gate_out_triggered()` (`src/engine/iengine.h:119`).

So direct actuation fights the renderer/engine for the same pins. Two resolutions, chosen by which use-case dominates:

### Option 1 - explicit test mode (recommended for QA / bring-up)

A `mode test` command suspends `render_leds()`, the DAC engine call, and gate-out, handing those outputs to the terminal until `mode run`. Clean electrical isolation: the device stops being an instrument while under test, and every actuation command has unambiguous ownership.

- Simplest to reason about; matches manufacturing-QA and bring-up, where you *want* the engine out of the way.

- Implementation: a global `terminal_test_mode` flag the three output sites consult and early-return from, plus the terminal driving the raw HAL writes directly.

### Option 2 - override latch (for a live-instrument console)

A small "forced value + TTL" layer the renderer/DAC/gate paths consult, so a terminal write wins for N ms then decays back to engine control. Outputs stay live; you can poke a playing instrument.

- More code: every output path (LED composite, `process_cv`, gate-out) must honour the latch.

- Better for debugging a running patch, worse for a clean pass/fail electrical test.

The two pull the arbitration layer in opposite directions - this is the decision the testing use-case forces that pure control did not. **Recommendation:** ship Option 1 first (it is a single flag and serves bring-up immediately); add the latch later only if a live console is wanted.

## Timing caveat

USB CDC round-trip is ~1 ms with host-scheduler jitter. Terminal-driven tests are **functional** ("does the gate fire", "does the CV read back ~2.5 V", "does pad 4 register") - not **timing-accurate** ("gate-to-audio latency is 1.3 ms"). Anything sub-millisecond must be measured on-device and *reported* over the terminal, never clocked from the host. Do not write host-side timing assertions; they measure USB jitter, not the device.

## Codec: line-ASCII default, OSC opt-in

| | line-ASCII (`SPK_TERMINAL`) | OSC + SLIP (`SPK_TERMINAL_OSC`) |
|---|---|---|
| Framing | `\n`-delimited | SLIP (RFC 1055) over the byte stream |
| Wire | `set param filter A 0.7\n` | `/set/param ,sf "filterA" 0.7` + type tags, 4-byte aligned |
| Flash cost | tokenizer + dispatch table (tiny) | + OSC parser + SLIP + type coercion |
| Host tooling | any serial terminal, `echo >`, pyserial | liblo, TouchOSC, Max/Pd, `[oscparse]` |
| Human-typable | yes | no (binary) |
| Test-scriptable | trivial (text diff) | worse (binary asserts) |

**Line-ASCII is the floor and the default.** It is the lightest possible thing, works with a dumb terminal today, is trivially scriptable, and text is easy to assert on in host tests. OSC is a drop-in *alternate codec behind the same dispatcher* for wiring the device into a Max/Pd/TouchOSC rig - it decodes OSC messages into the identical internal `Command`. Note that raw OSC over serial has no length prefix, so it needs a framing layer; SLIP is the convention and is the real cost beyond the parser.

### Line grammar (sketch)

```
line      := verb SP arg* NL
verb      := "get" | "set" | "config" | "probe" | "pulse" | "led" | "mode" | "help" | "describe" | <engine-verb>
arg       := token                      ; whitespace-separated, no quoting in v1
deck      := "A" | "B"
value     := float | int | "<n>v" | hex ; codec coerces; dispatcher validates against ParamId range
reply     := "ok" [SP result] NL | "err" SP reason NL
```

## Device self-description (`describe`) - what enables the GUI terminal

For the "sophisticated / GUI terminal that loads context for the specific device", the device must describe itself. `describe` emits the engine name, the `version.h` banner, and the param/config/probe tables (id, name, deck-scope, range). A host GUI/REPL consumes this to build autocompletion, macros, and value sliders **without hardcoding per-engine knowledge** - point a new firmware at the same tool and it reconfigures itself.

This costs flash: the `ParamId`/`ConfigId` enums are numeric today and need string name tables. Gate it behind a **sub-flag** `SPK_TERMINAL_REFLECT` so a flash-tight build (e.g. reverb) can ship the channel without the name tables.

There is a symmetry worth noting: `IEngine` lets the *platform* test-drive any engine through one contract; a terminal plus `describe` lets a *host* test-drive any firmware build through one protocol. The board becomes scriptable at the same seam, and one host test-runner can sweep all engine builds.

## Host tooling tiers (all speak the same line protocol)

- **Tier 0 - dumb terminal, zero code.** `tio /dev/tty.usbmodem*` or `screen ... 115200`. Works the instant line-ASCII ships.

- **Tier 1 - `tools/skterm.py` (pyserial + readline).** History, pretty-printed replies, tab-completion and macros driven by `describe`. ~150 lines, cross-platform. This is also the natural home for on-target hardware tests (pytest driving probes/actuators and asserting on replies).

- **Tier 2 - GUI.** A Textual TUI or a **Web Serial** page (browser, zero install) that loads the `describe` descriptor and renders forms/macros. Web Serial is the most portable "GUI terminal that loads device context" with no toolchain.

## Compile-flag matrix and footprint

Everything under `#if SPK_TERMINAL`, following the `SPK_USE_STREAM` / `METER` pattern - **zero cost when off.** Proposed location `src/terminal/`, parallel to `src/transport/` (it is a platform service, not an engine).

| Flag | Adds |
|------|------|
| `SPK_TERMINAL` | the channel + SPSC ring + line-ASCII codec + targets A and C(probe) + `mode test` isolation |
| `SPK_TERMINAL_OSC` | OSC + SLIP codec (swaps/adds behind the same dispatcher) |
| `SPK_TERMINAL_REFLECT` | `describe` + `ParamId`/`ConfigId`/probe name tables (flash cost) |

Line codec + ring + dispatch is small. OSC and the reflect tables are the two heavier, separately-gated add-ons. Actuation-latch (arbitration Option 2) is deferred and not represented here.

## Open decisions

1. **Primary use-case: QA/bring-up vs live console.** Decides the arbitration layer - Option 1 (test mode) vs Option 2 (override latch). They share transport and codec but pull in opposite directions. Recommendation: Option 1 first.

2. **Log routing.** Route `Log`/`LOG_TAGGED` through the terminal CDC (unified console - recommended) or set `LOGGER_NONE` and keep the terminal reply-only (simpler, but loses the boot trace)?

3. **Reflection day-one?** Ship `describe` + name tables immediately (enables the GUI terminal and generic cross-engine test sweeps, adds flash) or start with a hand-written command list?

## Alternative framing: USB-MIDI

Instead of a text/OSC channel, expose control as a **USB-MIDI class device** and reuse `handle_midi_message` almost verbatim - near-zero new parser, first-class DAW tooling. But it is not human-typable, awkward for string/reflective commands, and unusable for the HAL probe/actuate and `describe` reflection that make hardware testing work. Text wins for scripting, testing, and self-description; MIDI wins for musical/automation control. They can coexist later as a composite USB device; pick text as the primary for this capability.
```
