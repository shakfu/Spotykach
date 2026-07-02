# Terminal control channel (USB-C)

Status: **design sketch, unbuilt.** No code exists yet. This documents a proposed general capability - a bidirectional text/command channel over the Daisy Seed's USB-C port - usable across all engines for runtime control and, primarily, for **testing engine features and properties from a host script**. Everything is gated behind a compile-time flag and costs nothing when off.

Scope note: this is about testing **engines**, not the physical board. The hardware is assumed working; the goal is to recreate QA software that exercises an engine's control surface, state, and (optionally) its audio output - deterministically, without knobs, patch cables, or MIDI gear.

## Why

- **On-target engine testing.** Today `make test` is entirely off-target (`host/` unit tests compiled for the host). Some engine behaviour only exists on the device (real sample rate, SDRAM buffers, the actual audio ISR, QSPI persistence). A terminal is the missing on-target complement: a host script drives an engine into a known state, exercises a feature, reads back a property, and asserts - over USB-C, reproducibly.

- **Runtime control.** The same channel lets a host set parameters, flip config, and trigger pads/transport without hardware - scriptable and human-typable. Testing is the strong motivation; control is the same mechanism used interactively.

- It also retires two one-way workarounds: the `Expose` print pipe (`src/expose.h`) becomes an on-demand `query` instead of throttled spew, and the `CHUCK_BRINGUP` LED-blink boot markers (`src/app.cpp:71-82`) become a text boot trace.

## The key realization: `IEngine` is already a hardware-independent test surface

Engine testing needs almost nothing from the HAL. The entire `IEngine` input surface is already deck-addressable and hardware-free - the platform merely drives it from knobs/jacks today. A terminal that invokes `IEngine` methods by name gives a host script **complete deterministic control of an engine with no physical input**:

- `set_param(ParamId, deck, float)`, `set_config(ConfigId, deck, int)`, `set_mod_speed` (`src/engine/iengine.h:52-75`)

- `cv_voct / cv_mix / cv_size_pos / cv_crossfade` - inject CV *values* directly, no jack (`iengine.h:112-115`)

- `on_gate_trigger` - inject a gate, no jack (`iengine.h:118`)

- `handle_midi_note / handle_midi_message / handle_midi_transport` - inject MIDI, no cable (`iengine.h:88-93`)

- `on_record_pad / on_play_pad / on_seq_trigger / clear_buffer` - drive pads (`iengine.h:99-109`)

So "stimulus" is the reflective dispatcher generalized from params to the whole input surface. There is no separate HAL probe/actuate target; the engine's own contract is the seam.

## The one hard constraint: the Logger already owns USB-C

The USB-C connector is the STM32H750 USB OTG FS **internal** peripheral (`FS_INTERNAL`). The Daisy Logger already owns it TX-only (`src/common.h:37` `INFS_LOG_TARGET = LOGGER_INTERNAL`, started at `src/app.cpp:213`). Two `UsbHandle::Init(FS_INTERNAL)` calls on the same peripheral collide, so the terminal service must **own the internal CDC bidirectionally** and log output must flow through it. That is a feature: one CDC stream becomes both console output and command input. When `SPK_TERMINAL` is off, nothing changes and the Logger keeps the port.

(The `METER` build's CDC writer uses `FS_EXTERNAL` (GPIO31/32, which also collide with the gate outputs) and is debug-only. Only one of these can be the console; they do not share the internal port.)

## Layering

Four layers; only the bottom two touch hardware. The command model is deliberately **codec-agnostic**, so line-ASCII vs OSC is a compile flag, not a rewrite.

```
  host tooling        pytest harness | dumb terminal | python REPL
  ----------------------------------------------------- USB-C CDC ------
  [4] targets         (A) IEngine stimulus/query   (B) engine-specific verbs
  [3] dispatch        Command{ verb, argv[] }  ->  IEngine method or engine handler
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

- This is the **same execution context `handle_midi_message` already uses** (main loop, per `src/engine/iengine.h:92`). So terminal-injected stimulus reaches the engine exactly where MIDI does, and inherits MIDI's concurrency contract with the audio ISR - no new hazard. Determinism (keeping physical input from racing the injected stimulus) is handled by test mode below.

## Stimulus and observation

### Stimulus - drive the full IEngine input surface (target A)

The dispatcher maps a verb to an `IEngine` method against the active engine. Because the whole input surface is hardware-free, this works for every engine:

```
set param size A 0.5        -> IEngine::set_param(ParamId::Size, A, 0.5)
set config mode A 2         -> IEngine::set_config(ConfigId::Mode, A, 2)
cv voct A 1.0               -> IEngine::cv_voct(A, 1.0)
gate A                      -> IEngine::on_gate_trigger(A)
midi note 1 60             -> IEngine::handle_midi_note(1, 60)
pad play A                  -> IEngine::on_play_pad(A, false)
```

### Observation - read engine properties, three levels

Stimulus is easy; observation is the design question. Three levels, increasing in scope:

- **L0 - param round-trip.** `get param <id> <deck>` -> `param(id, deck)`. Verifies clamping, mode-dependent mapping, and `take_param_reseed`. Zero new engine code. Tests *properties*.

- **L1 - engine state introspection.** `query <name> <deck>` reports derived state: deck empty/generating, loop length, active mode, tempo, `gate_out_triggered`, `route()`, `mix()`. Some already exist on `IEngine`; the rest go through the engine-specific handler (target B). Tests *features / behavioural state*.

- **L2 - audio-property tap.** Raw audio cannot stream over CDC cheaply, so an on-device analyzer (a signal sibling of the CPU `Meter`) computes RMS / peak / DC / NaN-Inf / zero-crossing over N blocks at a probe point and reports text: `measure rms 100 -> 0.187`. Tests silence, level, blow-ups, crude pitch. This is the only way to assert actual audio behaviour.

### Engine-specific verbs (target B)

One new virtual on `IEngine`, consistent with the interface-lift philosophy ("an engine overrides only what it supports", `src/engine/iengine.h:24`):

```cpp
// iengine.h - one addition, no-op default
virtual bool handle_command(const CommandView& cmd, TextSink& reply) { return false; }
```

`CommandView` is a span over the already-tokenized argv, so the engine never touches the codec. It is the home for L1 `query` state an engine wants to expose and for any engine-unique test verb. Returning `false` -> "unknown command". A `CapTerminal` bit in the existing `Capabilities` bitmask advertises that an engine has custom verbs.

## Test mode - input isolation for determinism

The hazard for engine testing is not output pins; it is that physical knobs, CV jacks, pads, and gate-in are sampled every block (`_ui.tick()` / `read_cv()` in the audio ISR at `src/app.cpp:296-297`; `process_gate_in` in `T5Callback` at `src/app.cpp:121-128`) and would **race or overwrite the injected stimulus**, destroying reproducibility.

`mode test` therefore **freezes the physical input path**: the UI stops feeding knob/CV/pad/gate reads into the engine, so the engine sees only terminal-injected stimulus. Outputs (LEDs, DAC, audio) keep rendering - useful for watching state. `mode run` restores normal operation.

- Implementation: a single `terminal_test_mode` flag that `_ui.tick()` / `read_cv()` / `process_gate_in` consult and skip their engine-facing writes when set. Small and local; no output-path changes.

- This is deliberately narrower than an output-arbitration scheme. Engine testing wants a clean, quiet input path, not the ability to drive individual pins.

## Audio input injection (deferred - needed for L2 on through-processing engines)

Self-generating engines (mosc, reso self-oscillation, edrums) can be exercised immediately: stimulus in, `measure` out. But through-processing engines (delay, tape, granular, glitch, pstretch) need an *input signal*, and a host cannot feasibly stream audio in over CDC.

The plan is a **built-in test-signal source** enabled in test mode - `stim signal impulse | noise <amp> | sine <hz>` - that replaces the codec input with a deterministic generator. This is a real subsystem, so it is a later phase, not day-one. Until it exists, L2 covers self-generating engines only.

## Codec: line-ASCII default, OSC opt-in

| | line-ASCII (`SPK_TERMINAL`) | OSC + SLIP (`SPK_TERMINAL_OSC`) |
|---|---|---|
| Framing | `\n`-delimited | SLIP (RFC 1055) over the byte stream |
| Wire | `set param size A 0.5\n` | `/set/param ,sf "sizeA" 0.5` + type tags, 4-byte aligned |
| Flash cost | tokenizer + dispatch table (tiny) | + OSC parser + SLIP + type coercion |
| Host tooling | pyserial, any serial terminal, `echo >` | liblo, TouchOSC, Max/Pd, `[oscparse]` |
| Test-scriptable | trivial (text diff / assert) | worse (binary asserts) |

**Line-ASCII is the floor and the default** - lightest, works with a dumb terminal, and text is trivial to assert on from a pytest harness. OSC is a drop-in *alternate codec behind the same dispatcher* for wiring the device into a Max/Pd/TouchOSC rig; it decodes into the identical internal `Command`. Raw OSC over serial has no length prefix, so it needs SLIP framing - the real cost beyond the parser.

### Line grammar (sketch)

```
line      := verb SP arg* NL
verb      := "set" | "get" | "query" | "measure" | "cv" | "gate" | "midi" | "pad" | "mode" | "help" | "describe" | <engine-verb>
arg       := token                      ; whitespace-separated, no quoting in v1
deck      := "A" | "B"
value     := float | int | "<n>v" | hex ; codec coerces; dispatcher validates against ParamId range
reply     := "ok" [SP result] NL | "err" SP reason NL
```

## Device self-description (`describe`)

`describe` emits the engine name, the `version.h` banner, and the param/config/query tables (id, name, deck-scope, range). A host harness consumes this to enumerate what a build exposes and run a **generic test sweep across all engine builds** without per-engine test code - the same leverage `IEngine` gives the platform, now given to the host. It also drives REPL autocompletion.

This costs flash: `ParamId`/`ConfigId` are numeric today and need string name tables. Gate it behind a sub-flag `SPK_TERMINAL_REFLECT` so a flash-tight build (e.g. reverb) can ship the channel without the tables.

## Host tooling

- **pytest harness (the point).** A `tools/` helper opens the serial port, sends line commands, and asserts on replies. Per-engine on-target tests plus, with `describe`, a generic cross-engine sweep. This is the on-target counterpart to `host/` and the natural target of `make test-hw` (or similar).

- **Dumb terminal / REPL.** `tio /dev/tty.usbmodem*` for interactive poking; a small pyserial REPL (`tools/skterm.py`) with history and `describe`-driven completion for hand-testing.

## Compile-flag matrix and footprint

Everything under `#if SPK_TERMINAL`, following the `SPK_USE_STREAM` / `METER` pattern - **zero cost when off.** Proposed location `src/terminal/`, parallel to `src/transport/` (a platform service, not an engine).

| Flag | Adds |
|------|------|
| `SPK_TERMINAL` | channel + SPSC ring + line-ASCII codec + target A stimulus/get + target B hook + L0/L1 + `mode test` input isolation |
| `SPK_TERMINAL_MEASURE` | L2 audio-property analyzer + `measure` verb |
| `SPK_TERMINAL_STIM` | built-in test-signal source (`stim signal ...`) for through-processing engines |
| `SPK_TERMINAL_OSC` | OSC + SLIP codec (behind the same dispatcher) |
| `SPK_TERMINAL_REFLECT` | `describe` + `ParamId`/`ConfigId`/query name tables (flash cost) |

## Phasing

- **Phase 1 (day-one, `SPK_TERMINAL`).** Transport + ring + line codec + target A stimulus + L0/L1 observation + target B hook + `mode test`. Lands deterministic control-and-state engine tests fast, on self-generating and through-processing engines alike (control/state need no audio).

- **Phase 2 (`SPK_TERMINAL_MEASURE`).** L2 audio-property tap. Enables output assertions for self-generating engines.

- **Phase 3 (`SPK_TERMINAL_STIM`).** Test-signal injection, extending L2 to through-processing engines.

- **Later.** `SPK_TERMINAL_REFLECT` (generic cross-engine sweeps), `SPK_TERMINAL_OSC` (music-rig codec).

## Open decisions

1. **Test depth for phase 1.** Confirmed default: L0/L1 (control + state, no audio). `measure` (L2) and `stim` (signal injection) are phased behind sub-flags. Revisit if audio assertions are wanted sooner.

2. **Log routing.** Route `Log`/`LOG_TAGGED` through the terminal CDC (unified console - recommended, keeps the boot trace) or set `LOGGER_NONE` under `SPK_TERMINAL` (simpler transport, no boot trace)?

3. **Reflection timing.** Ship `describe` + name tables in phase 1 to unlock generic cross-engine sweeps (adds flash), or defer and hand-write per-engine tests first?

## Alternative framing: USB-MIDI

Control could instead be a **USB-MIDI class device** reusing `handle_midi_message` almost verbatim - near-zero parser, first-class DAW tooling. But it is not human-typable, awkward for `get`/`query`/`measure` readback, and unusable for the text assertions a test harness needs. Text wins for testing and self-description; MIDI wins for musical automation. They can coexist later as a composite USB device; text is the primary here.
