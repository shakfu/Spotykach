# Terminal channel - phase 1 implementation state

Status: **phase 1 built and build-verified; not yet run on hardware.** This records what actually
landed against the four design specs ([`terminal-control.md`](terminal-control.md),
[`terminal-transport.md`](terminal-transport.md), [`terminal-dispatch.md`](terminal-dispatch.md),
[`terminal-tools.md`](terminal-tools.md)) - the file layout, the deviations forced by the real
codebase, the footprint constraint that gates which engines can host it, and what remains. Built
2026-07-03.

Everything is behind `SPK_TERMINAL` and costs **nothing** when off (verified: the terminal TUs compile
to 0 bytes and the default `granular` binary's SRAM_EXEC is byte-identical to before, 186768 B).

## HARDWARE BRING-UP — IN PROGRESS, BLOCKED (session handoff, 2026-07-03)

**Status: first on-hardware bring-up attempted; BLOCKED on USB-C CDC never enumerating.** The build +
DFU-flash pipeline works, the app boots and runs, but the terminal's OTG_FS (USB-C) device never
appears on the host, so nothing has been exercised end-to-end (`make test-hw` unreachable). This
section is the resume point; the "Verification status" section below is the pre-hardware state.

### The blocking bug

Flashed `make ENGINE=delay TERMINAL=1` (fits -O2, 94% SRAM_EXEC) to the cased Spotykach and the USB-C
CDC does **not** enumerate. Established, with evidence:

- **Data path is 100% good.** The DFU bootloader (`0483:df11` "Daisy Bootloader") enumerates cleanly
  and repeatedly on the same cable/port/hub (`3-2.4`, behind a VIA Labs hub), confirmed via `dfu-util
  -l` and `journalctl -k`. So cable, port, and host are fine.
- **The app runs.** Knobs change the panel LEDs → `AppImpl::Init()` completes past `_terminal.init()`
  (`app.cpp:228`) and the main `Loop()` runs. libDaisy's `UsbErrorHandler` is `while(1){}`
  (`hid/usb.cpp:153`), so a failed `Init(FS_INTERNAL)` would freeze the app — it doesn't, so all four
  `USBD_Init/RegisterClass/RegisterInterface/Start` calls returned OK and `USBD_Start` ran `DevConnect`.
- **Yet the host sees LITERALLY NOTHING** on the app's OTG_FS — no `ttyACM`, no `0483:5740`, not even a
  failed-enumeration line in `journalctl`. A physical unplug/replug produced zero host events. That
  means the D+ pullup is never seen by the host — the enumeration fails *before* descriptors/IRQs
  matter (a descriptor/VTOR/IRQ fault would still log "new full-speed USB device" then an error).

### Ruled out (do not re-investigate without new evidence)

1. **VBUS sensing.** Hypothesis: libDaisy sets `hpcd_USB_OTG_FS.Init.vbus_sensing_enable = ENABLE`
   (`usbd/usbd_conf.c:424`; the OTG_HS core has it DISABLE at :476) and Daisy USB-C VBUS isn't on PA9.
   Tried a post-`Init` register poke in `Terminal::init()` (clear `GCCFG.VBDEN`, set
   `GOTGCTL.BVALOEN|BVALOVAL`, soft-disconnect/reconnect via `DCTL.SDIS`). **No effect.** Reverted.
   (Also: stock `Logger<LOGGER_INTERNAL>` uses the same config and enumerates for other Daisy users, so
   VBUS-sensing-ENABLE is not itself the blocker.)
2. **Dual OTG_HS + OTG_FS bring-up.** This build uniquely inits the *external* logger's OTG_HS
   (`Log::StartLog` at `app.cpp:219`, because the Makefile forces `-DINFS_LOG=1
   -DINFS_LOG_TARGET=LOGGER_EXTERNAL`) right before the terminal's OTG_FS. Tried skipping the external
   logger entirely (gate `StartLog`/`LOG_TAGGED` under `#if !SPK_TERMINAL`). **No effect.** Reverted.
   (Pins are independent: FS=PA11/PA12, HS=PB14/PB15; libDaisy's `FS_BOTH` does HS-then-FS in this same
   order.)
3. **USB 48 MHz clock.** libDaisy `sys/system.cpp` enables it: `HSI48State = RCC_HSI48_ON` (:429),
   `UsbClockSelection = RCC_USBCLKSOURCE_HSI48` (:514), applied by `_hw.Init()`. Present.
4. **Init call is wrong/incomplete.** `LoggerImpl<LOGGER_INTERNAL>::Init()` is *literally* just
   `usb_handle_.Init(FS_INTERNAL)` (`hid/logger_impl.h:56`) — byte-identical to the terminal's call.

### Leading theory (unverified)

This project has **never brought up OTG_FS in the app before** — the Makefile forces the Logger to
`LOGGER_EXTERNAL` (OTG_HS) and `METER` uses `FS_EXTERNAL`, so the USB-C port (OTG_FS) is *only* ever
touched by the bootloader for DFU. The terminal is the first app-side OTG_FS user, so the
**bootloader→app OTG_FS handoff** (or something in this app's boot that leaves the FS transceiver
unpowered/un-pulled) has never been exercised. Because the host sees no pullup at all, the next probe
targets the *physical layer*: is `DCTL.SDIS` clear (pullup asserted) and is `GCCFG.PWRDWN` set
(transceiver powered) after `Init()`?

### Hardware reality that changed the plan

The **cased Spotykach hides the Daisy Seed onboard LED AND the SWD pads** (per
[`chuck-pod-poc.md`](chuck-pod-poc.md)); the **bare Daisy Pod exposes both**. The device is Daisy
Seed-based (`daisy::DaisySeed seed; seed.Init(true)`, `hw/hardware.cpp:48`). So debugging moves to a
**Pod as a stock-Seed reference**, which also plugs into the dev host — enabling a direct
firmware-vs-board isolation and the repo's existing **SWD debug workflow** (ST-Link V3 mini + OpenOCD,
`pod/daisy_qspi.cfg`, `make -f pod/Makefile.chuck program-swd` / `openocd-attach`).

### RESUME PLAN (next session)

1. **Pod journal test (isolates firmware vs Spotykach board).** Flash the as-designed
   `make ENGINE=delay TERMINAL=1` to a Pod, plug the Pod into the dev host, watch
   `journalctl -k -b | grep 'usb 3-'` (or `lsusb | grep 0483`):
   - **Enumerates (`0483:5740` + `ttyACM`)** → firmware is fine; the cased Spotykach's custom-board
     USB-C routing/VBUS is the culprit → pivot to the board wiring.
   - **Also silent** → firmware bug reproduced on stock hardware → go to step 2.
2. **SWD register read (precise).** Attach ST-Link, `openocd-attach`, and read after `Init()`:
   `USB_OTG_FS->GCCFG` (PWRDWN bit = transceiver power), the device `DCTL.SDIS` (pullup), `GINTSTS`,
   and `hUsbDeviceFS.dev_state`. This ends the guessing about *why* the pullup isn't asserted.
3. **LED diagnostic (fallback, Pod only).** Already built into the tree, off by default:
   `make ENGINE=delay TERMINAL=1 USBDIAG=1` parks the app after `Init()` and blinks the Seed onboard
   LED — Group 1 (FAST) = pullup `1`=disconnected/`2`=connected; Group 2 (SLOW) = `GCCFG.PWRDWN`
   `1`=powered-down/`2`=active; expected-good = FAST 2 + SLOW 2. (Readable on a Pod, not the cased unit.)

### Uncommitted working-tree state at handoff (diagnostic scaffolding, all OFF by default)

- `Makefile`: adds `ifeq ($(USBDIAG),1) C_DEFS += -DTERM_USBDIAG=1`. Marked temporary; remove once USB-C
  works. Command-line `C_DEFS+=...` does NOT work (it clobbers the in-Makefile `C_DEFS` incl.
  `-DSTM32H750xx`); use the `USBDIAG=1` switch.
- `src/app.cpp`: `#ifdef TERM_USBDIAG` block at the end of `AppImpl::Init()` (the onboard-LED probe).
- `src/terminal/terminal.cpp`: **back to baseline** (the VBUS register-poke experiment was fully
  reverted; no net change).
- All prior experiments (VBUS poke, external-logger skip) reverted — only the off-by-default diagnostic
  remains. Decide whether to keep or drop the `USBDIAG` scaffolding once the root cause is found.

### Side finding — shuttle footprint (not a bug)

`make ENGINE=shuttle TERMINAL=1` fails at -O2 with `region SRAM_EXEC overflowed by 8396 bytes`. This is
the expected footprint ceiling, not a code fault: `OPT=-Os` fits (98.07% SRAM_EXEC, links clean).
Shuttle belongs with tape in the fit table below (stream/near-full engine → needs `-Os`).

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
| shuttle | overflow at -O2 (by 8396 B) -> fits at `-Os` | 98.07% (-Os) |
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
