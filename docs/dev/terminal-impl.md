# Terminal channel - phase 1 implementation state

Status: **phase 1 built and build-verified; not yet run on hardware.** This records what actually
landed against the four design specs ([`terminal-control.md`](terminal-control.md),
[`terminal-transport.md`](terminal-transport.md), [`terminal-dispatch.md`](terminal-dispatch.md),
[`terminal-tools.md`](terminal-tools.md)) - the file layout, the deviations forced by the real
codebase, the footprint constraint that gates which engines can host it, and what remains. Built
2026-07-03.

Everything is behind `SPK_TERMINAL` and costs **nothing** when off (verified: the terminal TUs compile
to 0 bytes and the default `granular` binary's SRAM_EXEC is byte-identical to before, 186768 B).

## How to build and run

```
make ENGINE=delay TERMINAL=1                 # lean engine, fits at -O2
make ENGINE=tape  TERMINAL=1 OPT=-Os         # near-full engine, needs -Os
make ENGINE=mosc APP_TYPE=BOOT_QSPI LDSCRIPT=alt_qspi.lds TERMINAL=1   # QSPI-execute, unlimited room
make engine-delay TERMINAL=1                  # clean build + DFU flash (one-shot)
make test-hw                                  # host pytest harness over USB-C (skips w/o a device)
```

`TERMINAL=1` defines `-DSPK_TERMINAL=1`. Because it adds virtuals to `IEngine` (changing the engine
vtable), **toggle it only on a clean build**; the `engine-*` one-shot targets already `make clean`, so
pass `TERMINAL=1` to them. A `build/.terminal-stamp` rebuilds the platform + terminal TUs on a toggle,
but the engine object's vtable is only guaranteed correct from clean.

## File map (what realizes which layer)

Platform service under `src/terminal/`, parallel to `src/transport/` (added to the `CPP_SOURCES`
wildcard; bodies fully under `#if SPK_TERMINAL`, so non-terminal builds link empty objects - the
`SPK_USE_STREAM` pattern).

| Layer / role | File | Notes |
|---|---|---|
| Contract types | `src/engine/terminal_io.h` | `CommandView`, `ITextOut` (abstract), `TextSink` (reply formatter) - engine-side so `IEngine` needs nothing from `src/terminal/` |
| IEngine hooks | `src/engine/iengine.h` | `handle_command` + `live_params`/`live_configs` virtuals, all `#if SPK_TERMINAL` |
| Capability bit | `src/engine/engine_params.h` | `CapTerminal = 1u << 10` |
| [1] RX ring | `src/terminal/rx_ring.h` | SPSC, `volatile` indices + `spk_dmb()` (inline `dmb 0xF`, no CMSIS include) |
| [1] TX FIFO | `src/terminal/tx_fifo.h` | 2 KB, single-threaded, `peek`/`commit` so a busy host never loses bytes |
| [1] line buffer | `src/terminal/line_assembler.h` | 128 B bound; over-long lines swallowed to their `\n`, reported once |
| [1] USB + pump | `src/terminal/terminal.{h,cpp}` | owns `FS_INTERNAL`, static RX trampoline -> file-scope `g_rx`, non-blocking `flush_tx` |
| shared state | `src/terminal/term_state.h` | `TermState{ test_mode }`, shared by terminal + dispatch |
| [2] tokenizer | `src/terminal/command.h` | in-place split, `kMaxArgs = 6` |
| [2] coercion | `src/terminal/fmt.{h,cpp}` | `parse_f32/i32/deck/onoff` (libc parse ok; only *print* avoids `%f`) |
| [2] formatting | `src/terminal/text_sink.cpp` | `TextSink` impl; float via integer decomposition (no `_printf_float`) |
| [2] names/meta | `src/terminal/names.{h,cpp}` | id<->name tables + `describe` scope/range/labels; numeric-id fallback |
| [3] dispatch | `src/terminal/dispatch.{h,cpp}` | verb table + handlers + `describe`; forwards unknowns to `handle_command` |
| integration | `src/app.cpp` | `_terminal` member; `init(_engine)` after `Log::StartLog`; `process()` + push `test_mode()` each Loop |
| `mode test` | `src/ui/core.ui.{h,cpp}` | `set_input_frozen()` gates `read_cv`/`process_gate_in`/the knob apply-pass |
| host tooling | `tools/` | `skdev/` client lib, `skterm.py` REPL, `conftest.py` + `test_generic.py` + `test_tape.py`, `README.md` |

## Deviations from the design specs (and why)

1. **The terminal owns `FS_INTERNAL` itself; there is no Logger coexistence to manage.** The transport
   spec's central premise was "the Logger already owns USB-C (internal)", so the terminal must attach
   only its RX callback and not re-init. But the **Makefile forces
   `-DINFS_LOG_TARGET=daisy::LOGGER_EXTERNAL`** (overriding `common.h`'s `LOGGER_INTERNAL` default), so
   the Logger - when present at all (`INFS_LOG`/`DEBUG`) - is on the *external* port. Nothing owns
   `FS_INTERNAL`, so `Terminal::init()` calls `Init(FS_INTERNAL)` unconditionally
   (`SPK_TERMINAL_INIT_USB`, default 1). Consequence: replies flow on USB-C, any logs flow on the
   external port - separate streams, so the host reply stream is clean. The `is_log` (`[`-prefix)
   filter still works; it just never fires. If a build ever puts the Logger back on the internal port,
   set `SPK_TERMINAL_INIT_USB=0`.

2. **`describe` config lines carry no scope token.** The dispatch spec's rendered example shows
   `config mode deck 0:slice ...`, but its own `parse_describe` sketch reads `tok[2:]` as `int:label`
   pairs (no scope). Firmware emits the parser-consistent form: `config <name> <i:label>...`. `param`
   and `query` lines *do* carry scope (`deck`/`global`). The host `parse_describe` was additionally
   made tolerant of both forms. Verified: a sample of the firmware's exact output round-trips through
   `parse_describe`.

3. **`mode test` knob freeze is one line, not a per-call-site guard.** The spec named `_ui.tick()` as
   the knob consult point, but the knob->engine writes actually live in the `process()` apply pass
   (`core.ui.cpp`, the `if (_apply.test(...)) _engine.set_param(...)` block). Freezing is
   `if (_input_frozen) _apply.reset();` just before that block, so every `_apply.test` reads false and
   no pot value reaches the engine; the `_mv[]` pickup caches still track, so knobs don't jump when
   test mode releases. CV (`read_cv`) and gate (`process_gate_in`) are the spec's clean early-returns.
   The flag is pushed from `app.cpp` each Loop (`_ui.set_input_frozen(_terminal.test_mode())`); it is a
   plain bool written on the main loop and read in the audio/TIM5 ISRs (benign single-byte).

4. **Contract types are abstract to avoid a dependency cycle.** `TextSink` writes through an abstract
   `ITextOut`, so `engine/terminal_io.h` pulls in no USB/`src/terminal/` types; `Terminal` implements
   `ITextOut`. The three `IEngine` virtuals are `#if SPK_TERMINAL` (zero vtable slots when off), so no
   engine overrides them yet - all use the "all live" / `return false` defaults, which is why the
   generic sweep must tolerate ignored params until an engine narrows its `live_*` masks.

## Footprint - the constraint that gates hosting

Enabling the channel links the **USB-device CDC stack + ~6 KB of terminal code = ~19-25 KB of
SRAM_EXEC**, because a normal build never brings USB up (Logger off/external, no METER). SRAM_EXEC is
only 186 KB and several engines already sit near the ceiling, so the channel does **not** fit
everywhere:

| Engine | Result | SRAM_EXEC |
|---|---|---|
| passthrough | fits (-O2) | lean |
| delay | fits (-O2) | ~94% |
| tape | overflow at -O2 -> fits at `-Os` | ~98% (-Os) |
| mosc (and csound/chuck) | fits (QSPI-execute) | 0% SRAM_EXEC (code in QSPI) |
| granular | overflow at -O2 **and** -Os | 98% before terminal |
| reso | overflow at -O2 and -Os | Rings DSP already large |

Rule of thumb: lean SRAM engines fit at -O2, near-full ones need `OPT=-Os`, and the tightest
(granular, reso) can host it only from a QSPI-execute build. The linker says
`region SRAM_EXEC overflowed by N bytes` when it won't fit - that is the signal to switch to `-Os` or
QSPI, not a code fault. The terminal code itself is ~6 KB (`dispatch` 3.3 KB, `text_sink` 0.8 KB,
`names` 0.8 KB, `terminal` 0.7 KB, `fmt` 0.3 KB); the rest is the USB stack and is not reducible.

## Verification status

Done (host / build):
- Zero-cost-off proven (terminal objects 0 bytes; default binary unchanged).
- Clean compile + link with `TERMINAL=1` on passthrough, delay, tape (`-Os`), mosc (QSPI).
- Host `tools/` all `py_compile`-clean; `parse_describe` round-trips the firmware's exact `describe`
  block (engine/version, param scope+range, config label maps, queries, caps hex).
- Fixed a pre-existing `-j` build race (the stamp recipes' `mkdir -p build` vs the core Makefile's
  plain `mkdir build`): all three stamps now order-only-depend on `$(BUILD_DIR)` instead of each
  racing to create it.

Not yet done (needs a flashed device - the checks from `terminal-transport.md` "To verify on
hardware"):
- CDC RX re-arm across back-to-back packets (a >64-byte paste arriving intact through the ring).
- `TransmitInternal` busy-return cadence with a draining vs silent host (confirm `flush_tx` never
  spins).
- Enumeration VID/PID as the host sees it (affects the `tools/` port glob).
- End-to-end `make test-hw` against a real engine (the generic round-trip sweep + `test_tape.py`).

## Not in phase 1 (unchanged from the specs)

`measure` (L2 audio-property tap, `SPK_TERMINAL_MEASURE`), `stim` (test-signal source,
`SPK_TERMINAL_STIM`), the OSC/SLIP codec (`SPK_TERMINAL_OSC`), per-engine `live_params()`/
`live_configs()` masks and any engine-specific `handle_command` verbs (every engine currently uses the
defaults), and enumeration of engine-specific `query` names inside `describe`.
