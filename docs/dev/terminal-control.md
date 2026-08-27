# Terminal control channel (USB-C)

Status: **built and hardware-verified (2026-07-31).** See [`terminal-impl.md`](terminal-impl.md) for what landed, the bring-up history and the deviations. This documents the capability - a bidirectional text/command channel over the Daisy Seed's USB-C port - usable across all engines for runtime control and, primarily, for **testing engine features and properties from a host script**. Everything is gated behind a compile-time flag and costs nothing when off.

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

```text
  host tooling        pytest harness | dumb terminal | python REPL
  ----------------------------------------------------- USB-C CDC ------
  [4] targets         (A) IEngine stimulus/query   (B) engine-specific verbs
  [3] dispatch        Command{ verb, argv[] }  ->  IEngine method or engine handler
  [2] codec/framing   line-ASCII (default)  |  OSC + SLIP (opt-in)
  [1] transport       UsbHandle CDC on FS_INTERNAL  +  SPSC ring buffer
```

## Data flow and threading

Thread-safe by construction - the receive callback does the minimum, everything else runs on the main loop where control input already lives.

```text
  USB IRQ  --ReceiveCallback-->  SPSC ring buffer (lock-free, raw bytes)   [producer: ISR]
                                          |
  main Loop() --drain--> line/SLIP assembler --> codec decode --> dispatch [consumer: main loop]
```

- The receive callback (`UsbHandle::SetReceiveCallback`, `lib/libDaisy/src/hid/usb.h:38`) runs in USB interrupt context and does **nothing but copy bytes into a single-producer/single-consumer ring**. No parsing, no allocation, no engine calls in the ISR.

- Assembly, decode, and dispatch run in `Loop()` (`src/app.cpp:243`), beside `_ui.process()` and `_stream.process()`. One added `terminal.process()` call, guarded by `#if SPK_TERMINAL`.

- This is the **same execution context `handle_midi_message` already uses** (main loop, per `src/engine/iengine.h:92`). So terminal-injected stimulus reaches the engine exactly where MIDI does, and inherits MIDI's concurrency contract with the audio ISR - no new hazard. Determinism (keeping physical input from racing the injected stimulus) is handled by test mode below.

The layer-[1] byte pipe - RX interrupt callback, the SPSC ring, non-blocking TX, and Logger coexistence on the one shared CDC device - is specified in detail in [`terminal-transport.md`](terminal-transport.md).

## The command model: params, queries and actions

"Stimulus and observation" below is the shape of the channel, but three different kinds of thing travel over it and they behave differently enough to be worth naming.

| | Param / config | Query | Action |
|---|---|---|---|
| What it is | a value the engine **holds** | a value the engine **computes** | an operation the engine **performs** |
| Direction | read and write | read only | write only, in the property sense |
| Idempotent | yes - setting twice = setting once | yes - reading twice = same answer | usually not |
| Backed by | `set_param`/`param`, `set_config` | `audio_is_empty`, `route`, `mix` | `on_gate_trigger`, `on_record_pad`, `clear_buffer` |
| Example | `set param size A 0.5` | `query empty A` | `gate A` |

The two axes that separate them: **is there a stored value behind it**, and **does calling it change anything**. A param has a value and no side effect. A query has no stored value of its own - it is derived - and no side effect. An action has no value and exists *for* its side effect.

### Actions can return values

They do already:

```text
pad play A          -> ok empty=1     # did the play pad find an empty deck?
config mode A 1     -> ok 1           # did that actually change the value?
```

Both are "do the thing, and tell me something about what just happened". So a reply is **not** what distinguishes an action from a query, and reasoning as though it were leads to the wrong design.

### What actually distinguishes them: repeatability

- A **query's** answer describes state that exists independently of your asking. Ask twice, get the same answer. Asking is free.

- An **action's** answer describes *the transition it just caused*. Ask twice and the second answer may differ **because** you asked the first time - `config mode A 1` returns `1` then `0`; `pad play A` may report a different `empty` because the first press started playback.

That is the only property the tooling actually needs: **can a generic host call this blindly, in any order, without consequences?** The descriptor-driven sweep in `tools/test_generic.py` rests entirely on that question - it calls every advertised query, and never an action. Everything else here is taxonomy.

### The blurry cases in this codebase

- **`set_config` returning `changed`** is a setter whose *effect* is idempotent but whose *return* is not. That is why `test_config_query_round_trip` asserts via `query route` rather than on the `changed` flag: the flag is a transition report, not state.

- **`IEngine::take_param_reseed()`** returns true once and self-clears - a read with a side effect, i.e. a latching query. If it were ever advertised, a sweep would silently consume it.

- **`toggle_grit_mode()`** is an action that returns the resulting state, so it reads like a getter.

These are why [`terminal-target-b.md`](terminal-target-b.md) ends up tagging entries by whether they are **safe to call** rather than sorting them into categories: the category is a judgment call, the safety property is the thing the sweep needs, and the two do not always agree.

## Stimulus and observation

### Stimulus - drive the full IEngine input surface (target A)

The dispatcher maps a verb to an `IEngine` method against the active engine. Because the whole input surface is hardware-free, this works for every engine:

```text
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

> Shipped, unused. See [`terminal-target-b.md`](terminal-target-b.md) for the redesign that would make > it worth using.

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
| Wire | `set param size A 0.5\n` | `/sk/a/param/size ,f 0.5` - type tags, 4-byte aligned, deck in the path |
| Flash cost | tokenizer + dispatch table (tiny) | + OSC parser + SLIP + type coercion |
| Host tooling | pyserial, any serial terminal, `echo >` | liblo, TouchOSC, Max/Pd, `[oscparse]` |
| Test-scriptable | trivial (text diff / assert) | worse (binary asserts) |

**Line-ASCII is the floor and the default** - lightest, works with a dumb terminal, and text is trivial to assert on from a pytest harness. OSC is a drop-in *alternate codec behind the same dispatcher* for wiring the device into a Max/Pd/TouchOSC rig; it decodes into the identical internal `Command`. Raw OSC over serial has no length prefix, so it needs SLIP framing - the real cost beyond the parser.

The codec grammar, verb catalog, `IEngine` binding, `mode test` consult points, and reply grammar are specified in full in [`terminal-dispatch.md`](terminal-dispatch.md). Params/config are addressed **by name** in phase 1 (names survive enum reorders and are the point of a typable channel); the flat id<->name table is cheap enough to live in the base `SPK_TERMINAL` flag.

## Device self-description (`describe`) - phase 1

`describe` is the introspection endpoint: the device reports its own control surface so a host configures itself instead of hardcoding per-engine knowledge. It emits the engine name, the `version.h` banner, and a **structured descriptor**: per parameter, its name, deck-scope, and range; per config, its enum labels; the query vocabulary; and the capability mask. A host harness consumes this to enumerate what a build exposes and run a **generic test sweep across all engine builds** without per-engine test code - the same leverage `IEngine` gives the platform, now given to the host. It also drives REPL autocompletion.

Ownership of the metadata is split so the per-engine burden is near zero:

- **Platform-owned (static tables).** Names (the flat id<->name table already in base), deck-scope (global vs per-deck is a property of the `ParamId`/`ConfigId`, implied by the enum), range conventions (most params are normalized 0..1; the few that are not - `Tempo`, `KeyInterval` - are fixed special-cases), and the `ConfigId` enum labels.

- **Engine-owned (one masks pair).** A liveness mask so the descriptor lists only what the engine actually implements - without it `describe` over-reports and a generic round-trip sweep gets false failures on ignored params. Two one-line constants per engine, default "all live":

  ```cpp
  virtual ParamMask  live_params()  const { return ~0u; }   // bitset over ParamId (24 < 32)
  virtual ConfigMask live_configs() const { return ~0u; }   // bitset over ConfigId
  ```

The `SPK_TERMINAL_REFLECT` flag remains only as an opt-**out** for a flash-tight build (e.g. reverb) that wants the named channel without the descriptor tables; it defaults **on** whenever `SPK_TERMINAL` is set. The descriptor format and streaming are specified in [`terminal-dispatch.md`](terminal-dispatch.md).

## Host tooling

- **pytest harness (the point).** A `tools/` helper opens the serial port, sends line commands, and asserts on replies. Per-engine on-target tests plus, with `describe`, a generic cross-engine sweep. This is the on-target counterpart to `host/` and the natural target of `make test-hw` (or similar).

- **Dumb terminal / REPL.** `tio /dev/tty.usbmodem*` for interactive poking; a small pyserial REPL (`tools/skterm.py`) with history and `describe`-driven completion for hand-testing.

The `skdev` client library, the pytest harness (`make test-hw`), and `skterm.py` are specified in full in [`terminal-tools.md`](terminal-tools.md).

## Compile-flag matrix and footprint

Everything under `#if SPK_TERMINAL`, following the `SPK_USE_STREAM` / `METER` pattern - **zero cost when off.** Proposed location `src/terminal/`, parallel to `src/transport/` (a platform service, not an engine).

| Flag | Adds |
|------|------|
| `TERMINAL=1` (`SPK_TERMINAL`) | the whole channel: transport + SPSC ring + line codec + name tables + target A stimulus + L0/L1 observation + target B hook + `mode test` input isolation + `describe` |
| `TERMPORT=int` | put the channel on OTG_FS (PA11/PA12, a bare Seed/Pod's own USB). **Default is external** - OTG_HS on PB14/PB15, where the Spotykach's rear USB-C is wired |
| `USBDIAG=1` | USB bring-up readout on the panel LEDs + onboard LED (`query usb` reports the same as text without it) |
| `USB_MIDI=1` | **incompatible** - `MidiUsbHandler` claims the same OTG_HS core; a compile error, not a silent conflict |
| `SPK_TERMINAL_MEASURE` | *(phase 2, unbuilt)* L2 audio-property analyzer + `measure` verb |
| `SPK_TERMINAL_STIM` | *(phase 3, unbuilt)* built-in test-signal source for through-processing engines |
| `SPK_TERMINAL_OSC` | OSC + SLIP codec behind the same dispatcher (`make ... TERMINAL=1 OSC=1`). Built 2026-08-09, off-target green, unflashed. Address space in [`terminal-osc.md`](terminal-osc.md) |

`SPK_TERMINAL_REFLECT` no longer exists: the descriptor tables proved cheap enough to keep unconditional, so `describe` is always present. Footprint and which engines can host the channel are in [`terminal-impl.md`](terminal-impl.md).

## Phasing

- **Phase 1 (day-one, `SPK_TERMINAL`).** Transport + ring + line codec + flat names + target A stimulus + L0/L1 observation + target B hook + `mode test` + **`describe` introspection** (platform descriptor tables + per-engine liveness masks). Lands deterministic control-and-state engine tests fast, on self-generating and through-processing engines alike (control/state need no audio), and enables generic cross-engine sweeps from day one.

- **Phase 2 (`SPK_TERMINAL_MEASURE`).** L2 audio-property tap. Enables output assertions for self-generating engines.

- **Phase 3 (`SPK_TERMINAL_STIM`).** Test-signal injection, extending L2 to through-processing engines.

- **Built, out of phase order (`SPK_TERMINAL_OSC`).** The music-rig codec - see [`terminal-osc.md`](terminal-osc.md). It was last in this phasing because it depends on nothing the other phases add; when the decision to ship `TERMINAL=1` was taken it became buildable immediately, ahead of `measure` and `stim`, which remain unbuilt.

## Open decisions - all resolved

1. **Test depth for phase 1.** L0/L1 (control + state, no audio), as planned. `measure` and `stim` remain phased behind sub-flags and are still unbuilt.

2. **Log routing.** Moot in practice: `INFS_LOG=1` is set only under `DEBUG=1`, so a normal build has no logger and nothing contends for the port.

3. **Reflection timing.** `describe` shipped in phase 1 and the per-engine `live_params()`/`live_configs()` masks are what make a generic sweep meaningful - the `descr` line reports `masked=0|1` so a host can skip engines still on the default.

4. **Which USB port** - not foreseen, and the one that cost two sessions. The channel belongs on whichever peripheral the board's jack is wired to, which on the Spotykach is OTG_HS. See [`terminal-impl.md`](terminal-impl.md).

## Alternative framing: USB-MIDI

Control could instead be a **USB-MIDI class device** reusing `handle_midi_message` almost verbatim - near-zero parser, first-class DAW tooling. But it is not human-typable, awkward for `get`/`query`/`measure` readback, and unusable for the text assertions a test harness needs. Text wins for testing and self-description; MIDI wins for musical automation. They can coexist later as a composite USB device; text is the primary here.
