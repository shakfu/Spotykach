# Terminal channel - phase 1 implementation state

Status: **phase 1 working on hardware (2026-07-31).** This records what actually landed against the four design specs ([`terminal-control.md`](terminal-control.md), [`terminal-transport.md`](terminal-transport.md), [`terminal-dispatch.md`](terminal-dispatch.md), [`terminal-tools.md`](terminal-tools.md)) - the file layout, the deviations forced by the real codebase, the footprint constraint that gates which engines can host it, and what remains. Built 2026-07-03.

Everything is behind `SPK_TERMINAL` and costs **nothing** when off (verified: the terminal TUs compile to 0 bytes and the default `granular` binary's SRAM_EXEC is byte-identical to before, 186768 B).

## HARDWARE BRING-UP — RESOLVED, PHASE 1 WORKING ON BOTH BOARDS (2026-07-31)

**Root cause: the channel was on the wrong USB peripheral.** The Spotykach's panel USB-C is wired to **OTG_HS (PB14/PB15**, Seed pins D29/D30), not to the Seed's own OTG_FS pins (PA11/PA12). The app was bringing up `FS_INTERNAL` and driving two pins this board connects to nothing, so the host never saw an attach. The board's bootloader is libDaisy's `extdfu` variant, which serves DFU over OTG_HS - which is why DFU always enumerated on the very connector the app could not use. Fixed by defaulting `SPK_TERMINAL_PORT_EXTERNAL` to 1 (`TERMPORT=int` for a bare Seed/Pod).

Verified on hardware: `skterm.py` connects, `describe` parses (24 params, 6 configs), `query usb` returns, params round-trip. Also verified on a Daisy Pod with `TERMPORT=int`, whose USB connector genuinely is OTG_FS.

`query usb` on the Spotykach, working:

```
ok boot=3 region=3 clkcfg=1 hsi48=1 usbsel=3 usb33den=1 usb33rdy=1 phy=1 pullup=1 \
   vbussense=0 vbusovr=0 dp=1 dm=1 rst=0 sof=1
```

### Why this took two sessions, and what the tree was already saying

The Makefile routes the logger to `LOGGER_EXTERNAL` **unconditionally**. That is a statement about which jack this hardware exposes, and it was read as an incidental detail: deviation #1 below observed it and concluded "nothing owns `FS_INTERNAL`, so the terminal must init it" - the opposite inference. `terminal-transport.md` was then written on the premise that the channel shares the *internal* port with the Logger. Every subsequent hypothesis (VBUS sensing, clock config, supply ordering, the bootloader handoff) was a search for a fault on a peripheral that was working perfectly.

Two boards also differ in ways that muddied the comparison: the Spotykach runs a v6.1+ bootloader (`boot=3`, so `clkcfg=1` - it configures its own clocks) while the Pod runs a pre-v6 one (`boot=0`, `clkcfg=0`, taking the `skip_clocks` path). Neither mattered in the end.

### What actually identified it

A live, on-panel register readout. The decisive reading was a **completely healthy OTG_FS core** - clocks, supply, transceiver powered, `DCTL.SDIS` clear, and both pads still in AF10 - with **zero host activity**. A controller doing everything right into total silence means the wire is not there. That combination is the signature of a disconnected port, and no single-bit pass/fail experiment could have produced it.

Two lessons worth keeping:

- **An init-time snapshot is not enough.** The first probe captured state once in `Terminal::init()` and could not have detected anything that changed afterwards. It had to become a live re-read (`usb_diag_refresh`, called every main loop) before it could be trusted.

- **`sof` is the reliable host-activity bit; `rst` is not.** The bus reset is transient and the ISR clears `GINTSTS` long before the main loop samples it, so `usb_reset_seen` reads 0 even on a fully working link (see the capture above). Judge "is the host talking to us" by `sof`.

### A build-system trap that cost a hardware debugging cycle (fixed 2026-07-31)

After the port fix, `TERMINAL=1` builds came up with a **frozen, garbled panel** (both rings stuck amber, panel button dead) while the terminal channel itself answered `query usb` normally. The port was blamed first; it was innocent - `TERMPORT=int` reproduced it too.

Root cause: this project builds under **GNU Make 3.81** (what macOS ships), which compares mtimes at whole-second resolution. The `.terminal-stamp` mechanism relies on the stamp becoming *newer* than the objects that depend on it, so any object compiled in the same second the stamp was rewritten was judged up to date and skipped. A flag toggle therefore produced a **partial** rebuild whose stale subset depended on timing - hence intermittent.

That is fatal here because these flags change **type layout**, not just behaviour: `SPK_TERMINAL` adds `_input_frozen` to `CoreUI`, `_terminal` to `AppImpl` and three virtuals to `IEngine`; `TERM_USBDIAG` adds another `CoreUI` member. Linking stale against fresh objects puts every later member at the wrong offset. A fresh `app.o` kept the channel working while a stale `core.ui.leds.o` rendered the panel from garbage - which is exactly the symptom, and why it looked like a USB fault rather than a build fault.

Fixed by making a stamp change **delete the objects** (`rm -f build/*.o`) rather than relying on mtime ordering, and by making all three stamps prerequisites of `$(OBJECTS)` so the recipes run before any compilation and cannot race a `-j` build. Toggling any of TERMINAL / USBDIAG / TERMPORT now forces a full rebuild, which is what the flags actually require.

The general lesson: a stamp file that encodes a flag is only safe if the flag cannot change type layout, or if the recipe deletes rather than out-dates. The Makefile's old advice ("toggling TERMINAL is a clean build") was correct and the stamp quietly undermined it by making a partial rebuild look valid.

### Diagnostics that remain in the tree

- `USBDIAG=1` - the panel/onboard-LED readout (`AppImpl::usb_diag_tick`, `CoreUI::_draw_usb_diag`). Worth keeping: it is the only way to get an answer out of a cased unit whose channel is down.

- `SPK_TERMINAL_USB33_PREINIT` (default on) - a genuine ordering correction to libDaisy, which enables the VDD33_USB detector only *after* `USBD_Start` has already connected and never waits for `USB33RDY`.

- `VBUSOFF=1` - **now dead scaffolding.** It targets an OTG_FS problem that never existed, and libDaisy already configures OTG_HS with `vbus_sensing_enable = DISABLE` (`vbussense=0` above, with the override not run). Candidate for removal.

### HISTORICAL - the blocking bug as first characterized (2026-07-03), resolved above

Flashed `make ENGINE=delay TERMINAL=1` (fits -O2, 94% SRAM_EXEC) to the cased Spotykach and the USB-C CDC does **not** enumerate. Established, with evidence:

- **Data path is 100% good.** The DFU bootloader (`0483:df11` "Daisy Bootloader") enumerates cleanly and repeatedly on the same cable/port/hub (`3-2.4`, behind a VIA Labs hub), confirmed via `dfu-util -l` and `journalctl -k`. So cable, port, and host are fine.

- **The app runs.** Knobs change the panel LEDs → `AppImpl::Init()` completes past `_terminal.init()` (`app.cpp:228`) and the main `Loop()` runs. libDaisy's `UsbErrorHandler` is `while(1){}` (`hid/usb.cpp:153`), so a failed `Init(FS_INTERNAL)` would freeze the app — it doesn't, so all four `USBD_Init/RegisterClass/RegisterInterface/Start` calls returned OK and `USBD_Start` ran `DevConnect`.

- **Yet the host sees LITERALLY NOTHING** on the app's OTG_FS — no `ttyACM`, no `0483:5740`, not even a failed-enumeration line in `journalctl`. A physical unplug/replug produced zero host events. That means the D+ pullup is never seen by the host — the enumeration fails *before* descriptors/IRQs matter (a descriptor/VTOR/IRQ fault would still log "new full-speed USB device" then an error).

### HISTORICAL - hypotheses pursued while the channel was on the wrong port

All of these were searches for a fault on OTG_FS, which was healthy throughout. Kept for the record only.

1. ~~**VBUS sensing.**~~ **BACK IN PLAY - this is now the leading hypothesis (see the status section above).** The original reasoning was: libDaisy sets `hpcd_USB_OTG_FS.Init.vbus_sensing_enable = ENABLE` (`usbd/usbd_conf.c:424`; the OTG_HS core has it DISABLE at :476) and Daisy USB-C VBUS may not be on PA9; a post-`Init` register poke in `Terminal::init()` (clear `GCCFG.VBDEN`, set `GOTGCTL.BVALOEN|BVALOVAL`, soft-disconnect/reconnect via `DCTL.SDIS`) had **no effect**, so it was reverted.

   Two problems with retiring it on that basis. First, the poke ran *after* `USBD_Start` had already asserted `DevConnect`; for the host to notice, the sequence must assert `DCTL.SDIS` to disconnect **first**, change `GCCFG`/`GOTGCTL`, wait a few ms so the host registers the disconnect, and only then clear `SDIS`. A poke without that settle window looks exactly like "no effect" whether or not it worked. Second, the supporting argument - that stock `Logger<LOGGER_INTERNAL>` uses the same config and enumerates for other Daisy users - compares against flash-resident builds on a stock Seed, whose USB connector routes VBUS to PA9; it says nothing about a custom board's panel connector. The cleaner test is to set `vbus_sensing_enable = DISABLE` in the vendored `usbd_conf.c` and let the HAL configure the core correctly from reset (needs `make -C lib/libDaisy`).

2. **Dual OTG_HS + OTG_FS bring-up.** This build uniquely inits the *external* logger's OTG_HS (`Log::StartLog` at `app.cpp:219`, because the Makefile forces `-DINFS_LOG=1 -DINFS_LOG_TARGET=LOGGER_EXTERNAL`) right before the terminal's OTG_FS. Tried skipping the external logger entirely (gate `StartLog`/`LOG_TAGGED` under `#if !SPK_TERMINAL`). **No effect.** Reverted. (Pins are independent: FS=PA11/PA12, HS=PB14/PB15; libDaisy's `FS_BOTH` does HS-then-FS in this same order.)

3. ~~**USB 48 MHz clock.**~~ **NOT ruled out - the original reasoning does not hold for this build.** It cited `sys/system.cpp` enabling `HSI48State = RCC_HSI48_ON` (:429) and `UsbClockSelection = RCC_USBCLKSOURCE_HSI48` (:514) as "applied by `_hw.Init()`". But both live inside `System::ConfigureClocks()`, which `System::Init()` **skips entirely** when `config.skip_clocks` is set (`sys/system.cpp:220-224`), and `daisy_seed.cpp:110-117` sets `skip_clocks = true` whenever the bootloader is `LT_v6_0` **and** the program memory region is not internal flash. Every build in this repo is `APP_TYPE = BOOT_SRAM` (`Makefile:368`), so whether the app configures the USB clock at all is a runtime property of the installed bootloader - it cannot be settled by reading libDaisy. Now measured directly (see the resume plan below).

   The same correction applies to the supporting argument that stock `Logger<LOGGER_INTERNAL>` enumerates for other Daisy users: the standard Daisy examples are flash-resident images programmed through the ST system bootloader, where `ConfigureClocks()` always runs and there is no Daisy-bootloader-to-app OTG_FS handoff. That observation does not transfer to a bootloader-launched SRAM app.

4. **Init call is wrong/incomplete.** `LoggerImpl<LOGGER_INTERNAL>::Init()` is *literally* just `usb_handle_.Init(FS_INTERNAL)` (`hid/logger_impl.h:56`) — byte-identical to the terminal's call.

### Leading theory (unverified)

This project has **never brought up OTG_FS in the app before** — the Makefile forces the Logger to `LOGGER_EXTERNAL` (OTG_HS) and `METER` uses `FS_EXTERNAL`, so the USB-C port (OTG_FS) is *only* ever touched by the bootloader for DFU. The terminal is the first app-side OTG_FS user, so the **bootloader→app OTG_FS handoff** (or something in this app's boot that leaves the FS transceiver unpowered/un-pulled) has never been exercised. Because the host sees no pullup at all, the next probe targets the *physical layer*: is `DCTL.SDIS` clear (pullup asserted) and is `GCCFG.PWRDWN` set (transceiver powered) after `Init()`?

### Hardware reality that changed the plan

The **cased Spotykach hides the Daisy Seed onboard LED AND the SWD pads** (per [`chuck-pod-poc.md`](chuck-pod-poc.md)); the **bare Daisy Pod exposes both**. The device is Daisy Seed-based (`daisy::DaisySeed seed; seed.Init(true)`, `hw/hardware.cpp:48`). So debugging moves to a **Pod as a stock-Seed reference**, which also plugs into the dev host — enabling a direct firmware-vs-board isolation and the repo's existing **SWD debug workflow** (ST-Link V3 mini + OpenOCD, `pod/daisy_qspi.cfg`, `make -f pod/Makefile.chuck program-swd` / `openocd-attach`).

### RESUME PLAN (next session)

0. **Read the bring-up probe first (built, no debugger needed).** `Terminal::init()` now captures every register that has to be right before a D+ pullup can exist, into `UsbDiag` (`src/terminal/usb_diag.{h,cpp}`), and `make ENGINE=delay TERMINAL=1 USBDIAG=1` parks the app and blinks the verdict on the Daisy onboard LED - six groups in dependency order, **1 blink = bad, 2 = good**:

   | Group | Reads | Bad means |
   |---|---|---|
   | 1 | clocks configured (not the `skip_clocks` path) | the app never ran `ConfigureClocks()`; USB clock is whatever the bootloader left |
   | 2 | `RCC_CR.HSI48RDY` | no 48 MHz source |
   | 3 | `RCC_D2CCIP2R.USBSEL == 3` (HSI48) | USB clocked from the wrong source, or not at all |
   | 4 | `PWR_CR3.USB33RDY` | transceiver supply never validated |
   | 5 | `GCCFG.PWRDWN` | transceiver powered down |
   | 6 | `DCTL.SDIS` clear | core is soft-disconnected - no pullup |

   Groups 1-4 are new ground; 5-6 are the SWD reads the old plan deferred to step 2, now available without a debugger. The first group reading ONE is the layer to investigate. Expected-good is six double-blinks. Once the port does enumerate, the same snapshot is readable as `query usb`.

   **The `USBDIAG` build is a normal, fully runnable app.** The blink is non-blocking (driven from `Loop()`, touching only the onboard LED, which nothing else uses), so panel, audio, and the boot-button DFU escape all behave as usual. This matters: the escape hatch lives entirely in app code - `Hardware::ProcessDigitalControls` (`src/hw/hardware.cpp:224-231`) resets on BOOT release, and `AppImpl::Loop` calls `ResetToBootloader` on a 3s hold - so **any diagnostic that parks before `Loop()` removes the only software route into DFU**. A first cut of this probe did exactly that and had to be recovered through the bootloader's own reset-time DFU window. Do not reintroduce a park.

   Also landed with it: `Terminal::init()` now enables the VDD33_USB level detector and waits for `USB33RDY` **before** `UsbHandle::Init()` (`SPK_TERMINAL_USB33_PREINIT`, default 1). libDaisy calls `HAL_PWREx_EnableUSBVoltageDetector()` *after* `InitFS()` - i.e. after `USBD_Start` has already asserted `DevConnect` - and never waits for the ready flag (`hid/usb.cpp:88-102`), so the core could be told to connect before its supply was valid. If the port now enumerates, that ordering was the bug; set the flag to 0 to confirm by reverting to libDaisy's order.

1. ~~**Pod journal test.**~~ **DONE 2026-07-31: the Pod enumerates.** Firmware is cleared; the fault is the cased Spotykach's board or bootloader image. Note the confound this test carries, in case it is repeated: the Pod's bootloader version is its own variable, so "Pod works" would also be explained by a newer bootloader. It happened not to apply here - the Pod reports `boot=0` (`LT_v6_0`), the same pre-v6 class, so the boot path really was like-for-like.

2. **SWD register read (precise).** Attach ST-Link, `openocd-attach`, and read after `Init()`: `USB_OTG_FS->GCCFG` (PWRDWN bit = transceiver power), the device `DCTL.SDIS` (pullup), `GINTSTS`, and `hUsbDeviceFS.dev_state`. This ends the guessing about *why* the pullup isn't asserted.

2. **SWD register read (only if step 0 comes back all-good).** Attach ST-Link, `openocd-attach`, and read `GINTSTS` and `hUsbDeviceFS.dev_state` - the state the LED probe cannot express. `GCCFG`/`DCTL` are already covered by groups 5-6.

3. **Peripheral-reset control (untried, cheap).** If step 0 is all-good and the port is still silent, the remaining candidate is the bootloader-to-app OTG_FS handoff: the Daisy bootloader runs its own DFU device on this peripheral immediately before the jump. Force `__HAL_RCC_USB1_OTG_FS_FORCE_RESET()` / `RELEASE_RESET()` before `Init()` and re-probe. That separates "core left in a stale state" from "clock/supply".

### Diagnostic scaffolding in the tree (all OFF by default)

- `Makefile`: `ifeq ($(USBDIAG),1) C_DEFS += -DTERM_USBDIAG=1`, plus a `build/.usbdiag-stamp` so `app.o` rebuilds on a toggle. Command-line `C_DEFS+=...` does NOT work (it clobbers the in-Makefile `C_DEFS` incl. `-DSTM32H750xx`); use the `USBDIAG=1` switch. Temporary - remove once USB-C works.

- `src/terminal/usb_diag.{h,cpp}`: the `UsbDiag` snapshot, its two capture points, and `usb_supply_bringup()`. 152 B of flash when `SPK_TERMINAL` is on, 0 when off.

- `src/app.cpp`: `AppImpl::usb_diag_tick()` under `#if SPK_TERMINAL && TERM_USBDIAG`, called once per `Loop()` iteration - the onboard-LED probe. Non-blocking (a millisecond-timed segment schedule), so the app runs normally; `USBDIAG=1` costs ~340 B (delay: 180844 B vs 180508 B SRAM_EXEC).

- All prior experiments (VBUS poke, external-logger skip) remain reverted. Decide whether to keep or drop the `USBDIAG` scaffolding once the root cause is found.

### Transport fixes landed alongside the probe

Two defects found by review of the shipped `flush_tx`, both fixed before the next bring-up attempt so they cannot be mistaken for USB faults:

- **TX staging buffer was reused while a transfer was still in flight.** `CDC_Transmit_FS` returns `USBD_OK` once the packet is *queued*; in non-DMA mode the HAL copies out of the caller's buffer later, from the TX-FIFO-empty interrupt (`usbd_cdc_if.c:300-310`). The single `_scratch[64]` was refilled by the next `process()` - microseconds later, far inside a 1 ms USB frame - corrupting the packet the peripheral was about to send. Symptom would have been intermittently garbled or duplicated `describe` output. Now two slots, swapped only on a successful transmit: a transmit only succeeds when `TxState` is clear, which proves the other slot is free.

- **Dropped replies were silently swallowed.** `flush_tx` called `_tx.take_overflow()` and discarded the result, against the transport spec's promise that a TX overflow is latched and reported. With a synchronous host (one command outstanding) a lost reply reads as an unexplained timeout. Now reported as `err tx-overflow` from `process()`; if that enqueue does not fit either, `TxFifo` re-latches and it retries.

### Side finding — shuttle footprint (not a bug)

`make ENGINE=shuttle TERMINAL=1` fails at -O2 with `region SRAM_EXEC overflowed by 8396 bytes`. This is the expected footprint ceiling, not a code fault: `OPT=-Os` fits (98.07% SRAM_EXEC, links clean). Shuttle belongs with tape in the fit table below (stream/near-full engine → needs `-Os`).

## How to build and run

```
make ENGINE=delay TERMINAL=1                 # lean engine, fits at -O2
make ENGINE=tape  TERMINAL=1 OPT=-Os         # near-full engine, needs -Os
make ENGINE=mosc APP_TYPE=BOOT_QSPI LDSCRIPT=linker/alt_qspi.lds TERMINAL=1   # QSPI-execute, unlimited room
make ENGINE=delay TERMINAL=1 USBDIAG=1        # USB bring-up probe; parks + blinks, does not run
make engine-delay TERMINAL=1                  # clean build + DFU flash (one-shot)
make test-hw                                  # host pytest harness over USB-C (skips w/o a device)
```

`TERMINAL=1` defines `-DSPK_TERMINAL=1`. Because it adds virtuals to `IEngine` (changing the engine vtable), **toggle it only on a clean build**; the `engine-*` one-shot targets already `make clean`, so pass `TERMINAL=1` to them. A `build/.terminal-stamp` rebuilds the platform + terminal TUs on a toggle, but the engine object's vtable is only guaranteed correct from clean.

## File map (what realizes which layer)

Platform service under `src/terminal/`, parallel to `src/transport/` (added to the `CPP_SOURCES` wildcard; bodies fully under `#if SPK_TERMINAL`, so non-terminal builds link empty objects - the `SPK_USE_STREAM` pattern).

| Layer / role | File | Notes |
|---|---|---|
| Contract types | `src/engine/terminal_io.h` | `CommandView`, `ITextOut` (abstract), `TextSink` (reply formatter) - engine-side so `IEngine` needs nothing from `src/terminal/` |
| IEngine hooks | `src/engine/iengine.h` | `handle_command` + `live_params`/`live_configs` virtuals, all `#if SPK_TERMINAL` |
| Capability bit | `src/engine/engine_params.h` | `CapTerminal = 1u << 10` |
| [1] RX ring | `src/terminal/rx_ring.h` | SPSC, `volatile` indices + `spk_dmb()` (inline `dmb 0xF`, no CMSIS include) |
| [1] TX FIFO | `src/terminal/tx_fifo.h` | 2 KB, single-threaded, `peek`/`commit` so a busy host never loses bytes |
| [1] line buffer | `src/terminal/line_assembler.h` | 128 B bound; over-long lines swallowed to their `\n`, reported once |
| [1] USB + pump | `src/terminal/terminal.{h,cpp}` | owns `FS_INTERNAL`, static RX trampoline -> file-scope `g_rx`, non-blocking `flush_tx` (ping-ponged TX staging) |
| [1] USB probe | `src/terminal/usb_diag.{h,cpp}` | `UsbDiag` clock/supply/pullup snapshot; pre-Init `USB33RDY` bring-up; read via `query usb` or the `USBDIAG` LED probe |
| [1] CPU meter | `src/terminal/cpu_stat.{h,cpp}` | reads the platform `CpuLoadMeter` (`src/meter.h`) for `query cpu`/`cpumin`/`cpumax` + `reset cpu`; same ARM/host split as `usb_diag` |
| shared state | `src/terminal/term_state.h` | `TermState{ test_mode }`, shared by terminal + dispatch |
| [2] tokenizer | `src/terminal/command.h` | in-place split, `kMaxArgs = 6` |
| [2] coercion | `src/terminal/fmt.{h,cpp}` | `parse_f32/i32/deck/onoff` (libc parse ok; only *print* avoids `%f`) |
| [2] formatting | `src/terminal/text_sink.cpp` | `TextSink` impl; float via integer decomposition (no `_printf_float`) |
| [2] names/meta | `src/terminal/names.{h,cpp}` | id<->name tables + `describe` scope/range/labels; numeric-id fallback |
| [3] dispatch | `src/terminal/dispatch.{h,cpp}` | verb table + handlers + `describe`; forwards unknowns to `handle_command` |
| integration | `src/app.cpp` | `_terminal` member; `init(_engine)` after `Log::StartLog`; `process()` + push `test_mode()` each Loop |
| `mode test` | `src/ui/core.ui.{h,cpp}` | `set_input_frozen()` gates `read_cv`/`process_gate_in`/the knob apply-pass |
| host tooling | `tools/` | `skdev/` client lib, `skterm.py` REPL, `conftest.py` + `test_generic.py` + `test_tape.py` (need a device) + `test_descriptor.py` (does not), `README.md` |
| off-target tests | `host/test_terminal.cpp` | codec/dispatch/format/ring/FIFO/describe against a mock `IEngine`; no device |

## Deviations from the design specs (and why)

1. **The terminal owns the CDC device itself; there is no Logger coexistence to manage.** (The port choice in this item was WRONG - see the resolved section above; the channel is on `FS_EXTERNAL`. Note also that `INFS_LOG=1` is set only under `DEBUG=1`, so a normal build has no logger at all and never brings up a second OTG core.) The transport spec's central premise was "the Logger already owns USB-C (internal)", so the terminal must attach only its RX callback and not re-init. But the **Makefile forces `-DINFS_LOG_TARGET=daisy::LOGGER_EXTERNAL`** (overriding `common.h`'s `LOGGER_INTERNAL` default), so the Logger - when present at all (`INFS_LOG`/`DEBUG`) - is on the *external* port. Nothing owns `FS_INTERNAL`, so `Terminal::init()` calls `Init(FS_INTERNAL)` unconditionally (`SPK_TERMINAL_INIT_USB`, default 1). Consequence: replies flow on USB-C, any logs flow on the external port - separate streams, so the host reply stream is clean. The `is_log` (`[`-prefix) filter still works; it just never fires. If a build ever puts the Logger back on the internal port, set `SPK_TERMINAL_INIT_USB=0`.

2. **`describe` config lines carry no scope token.** The dispatch spec's rendered example shows `config mode deck 0:slice ...`, but its own `parse_describe` sketch reads `tok[2:]` as `int:label` pairs (no scope). Firmware emits the parser-consistent form: `config <name> <i:label>...`. `param` and `query` lines *do* carry scope (`deck`/`global`). The host `parse_describe` was additionally made tolerant of both forms. Verified: a sample of the firmware's exact output round-trips through `parse_describe`.

3. **`mode test` knob freeze is one line, not a per-call-site guard.** The spec named `_ui.tick()` as the knob consult point, but the knob->engine writes actually live in the `process()` apply pass (`core.ui.cpp`, the `if (_apply.test(...)) _engine.set_param(...)` block). Freezing is `if (_input_frozen) _apply.reset();` just before that block, so every `_apply.test` reads false and no pot value reaches the engine; the `_mv[]` pickup caches still track, so knobs don't jump when test mode releases. CV (`read_cv`) and gate (`process_gate_in`) are the spec's clean early-returns. The flag is pushed from `app.cpp` each Loop (`_ui.set_input_frozen(_terminal.test_mode())`); it is a plain bool written on the main loop and read in the audio/TIM5 ISRs (benign single-byte).

4. **Contract types are abstract to avoid a dependency cycle.** `TextSink` writes through an abstract `ITextOut`, so `engine/terminal_io.h` pulls in no USB/`src/terminal/` types; `Terminal` implements `ITextOut`. The three `IEngine` virtuals are `#if SPK_TERMINAL` (zero vtable slots when off), so no engine overrides them yet - all use the "all live" / `return false` defaults, which is why the generic sweep must tolerate ignored params until an engine narrows its `live_*` masks.

## Footprint - the constraint that gates hosting

Enabling the channel links the **USB-device CDC stack + ~6 KB of terminal code = ~19-25 KB of SRAM_EXEC**, because a normal build never brings USB up (Logger off/external, no METER). SRAM_EXEC is only 186 KB and several engines already sit near the ceiling, so the channel does **not** fit everywhere:

> ### The rebalance below BROKE `pstretch` outright - found and FIXED 2026-08-01
>
> **Resolved:** `pstretch` now builds and is **flashed and confirmed working on hardware** (2026-08-01,
> `make engine-pstretch`). The fix is a per-engine linker script,
> [`linker/alt_sram_pstretch.lds`](../../linker/alt_sram_pstretch.lds) (200K/312K), selected
> automatically on `ENGINE=pstretch`; the other 20 engines stay on the 300K split. Same precedent as
> `linker/alt_qspi_chuck.lds`. The hardware check is the part that counts - moving the code/data
> boundary links clean either way, so only a boot proves the layout. Still unflashed: the `TERMINAL=1`
> pstretch image, a different binary at 94.38%/97.40%.
>
> The diagnosis is kept below because the failure mode is worth recognising again.
>
> **`pstretch` did not link at all - terminal or not, clean build, any window.** It was not a
> terminal-hosting limitation; the channel was incidental. `make ENGINE=pstretch` on the committed
> `v0.6.1` tree failed with `region SRAM overflowed by 80576 bytes`.
>
> Cause: `993210f` ("more hardware tests") moved 114K from the data `SRAM` region into `SRAM_EXEC`
> (186K -> 300K) so the terminal would fit everywhere. `pstretch` is the one engine that lived in the
> region that paid for it - its FFT working set needs **297664 B** of data `SRAM`, 89% of the old 326K.
> The new region is 212K, leaving it 80.5K short, which is the overflow exactly. Restoring the
> pre-rebalance script links it immediately (`SRAM_EXEC 164328 B / 86.28%`, `SRAM 297664 B / 89.17%`).
>
> So the table below is measuring the wrong axis for this engine, and "every engine now fits" was true
> only of `SRAM_EXEC`.
>
> **The split is a genuine three-way squeeze, and only a narrow band satisfies all of it.** Measured with
> `pstretch TERMINAL=1`:
>
> | `SRAM_EXEC` | result |
> |---|---|
> | 186K | `SRAM_EXEC` overflows by 2836 B |
> | **200K** | **links** - `SRAM_EXEC` 94.38%, `SRAM` 97.40% |
> | 210K | `SRAM` overflows by 1920 B |
> | 300K (current) | `SRAM` overflows by 13504 B (94080 B with the default window) |
>
> A single *global* 200K split would restore pstretch and keep the channel, but at 94%/97% it leaves
> almost no headroom for either region - not obviously right for the other 20 engines, all of which are
> comfortable at 300K. Hence the per-engine script: the squeeze is confined to the engine that has it,
> and nothing else moves.
>
> **The lesson worth keeping:** the rebalance was justified per-engine on `SRAM_EXEC` alone, and every
> engine did fit on that axis - so "every engine now fits" looked verified. `pstretch` is large in
> *both* halves at once, so the axis that broke it was the one nobody was measuring. When a change
> trades one region against another, the engine to check is the one with the largest total, not the
> largest code.

`SRAM_EXEC` was rebalanced from 186K to 300K on 2026-07-31 (see
`alt_sram.lds`): the old ceiling was a linker-script split, not silicon, and it left ~260K idle in the
data region while code sat at 99%. Measured with the terminal enabled:

| Engine | SRAM_EXEC | data SRAM | was (at 186K) |
|---|---|---|---|
| delay (-O2) | 59.42% | 45.49% | 95.84% |
| tape (-Os) | 61.56% | 30.49% | 99.29% |
| softcut (-Os) | 62.78% | 47.58% | **overflowed** |
| bard (-Os) | 63.48% | 68.81% | **overflowed** |
| reso (-Os) | 69.01% | 30.18% | **overflowed** |
| reverb (-Os) | 69.54% | 33.99% | **overflowed** |
| granular (-O2) | 69.93% | 35.41% | **overflowed at -O2 and -Os** |

granular + terminal was flashed and verified on hardware (214812 B, 24K past the old ceiling), which is
what proves the bootloader copies more than the old region - a link-time pass alone would not have.

The QSPI-execute engines (mosc/csound/chuck) still need `USB_MIDI=0`, since `MidiUsbHandler` claims the
same OTG_HS core as the terminal.

Historical rule of thumb, now mostly moot: engines needed `-Os` or a QSPI-execute build to host the channel. With the rebalanced split none of that applies - `-Os` remains worthwhile on the heavy engines for its own sake, not to make the terminal fit. The terminal code itself is ~6 KB (`dispatch` 3.3 KB, `text_sink` 0.8 KB, `names` 0.8 KB, `terminal` 0.7 KB, `fmt` 0.3 KB); the rest is the USB stack and is not reducible.

## CPU load over the channel (added 2026-08-01)

The channel now reports the platform `CpuLoadMeter`: `query cpu` / `cpumin` / `cpumax` (percent of the
block budget) and `reset cpu` to clear the extremes. Design and the `METER` interaction are in
[`terminal-dispatch.md` "CPU load"](terminal-dispatch.md#cpu-load---query-cpu--cpumin--cpumax); the short
version is that `METER=1` needed a second USB device on the OTG core the terminal itself uses, so the
two flags could never be combined - and a `TERMINAL=1` build now drives the meter directly instead.

This is what makes TODO.md's P2 a *measured* pass rather than a listening one: `reset cpu` -> drive the
engine -> `query cpumax` is scriptable per engine, where before the numbers needed a build flag that
conflicted with the channel collecting them.

Footprint: `delay TERMINAL=1` went 59.42% -> 59.66% SRAM_EXEC (~740 B). Zero-cost-off holds -
`cpu_stat.o` is 0 bytes with `TERMINAL` off, and with neither `METER` nor `SPK_TERMINAL` the
`SPK_CPU_METER` guard is undefined, so `ProcessAudio` is unchanged.

**HARDWARE-VERIFIED 2026-08-01 on `pstretch TERMINAL=1`.** `describe` advertises all three queries
(12 platform queries total), `reset cpu` replies `ok`, and the readings are real:

```
reset cpu -> ok
  after 1s  avg=  41.5647  min=   2.0025  max=  87.6321
  after 3s  avg=  43.4994  min=   2.0025  max=  88.2400
  after 6s  avg=  40.5273  min=   2.0025  max=  91.3133
```

Two things this first real use established beyond "it works":

- **`min` is genuinely useful, and it is not noise.** 2.0025% is the callback with the FFT not running
  in that block - i.e. the floor cost of the platform (UI tick, CV read, marshalling) as distinct from
  the engine. Reading avg/min/max together separates "this engine is expensive" from "this engine is
  spiky", which a single average cannot.

- **A rising `max` is the signal to watch.** Sampling repeatedly after one `reset cpu` shows whether the
  peak has converged. Above it had not (87.6 -> 91.3 and still climbing), meaning the worst case had not
  been found yet; the same engine at `WINDOW=4096` converged within 0.3 pp. That distinction is only
  visible because `reset cpu` bounds the interval and the query can be re-read - it is the main reason
  the reset verb earns its place.

The `nan` window predicted in `cpu_stat.h` (a read landing between the reset and the next block end) was
not observed in practice, as expected - the reset and the first query are a USB round-trip apart.

## Verification status

Done (host / build):
- **Off-target test coverage of everything below the USB transport** (`host/test_terminal.cpp`, run by `make -C host test-terminal`, part of `make -C host test`). The codec, dispatcher, name/describe tables, reply formatter, ring, TX FIFO and line assembler are all hardware-free, so they run on the desktop against a recording mock `IEngine`; only `src/terminal/terminal.cpp` (which owns `daisy::UsbHandle`) is excluded. Covers the tokenizer and its arg limits, CRLF vs bare LF and the over-long-line swallow, ring wraparound/overflow, the TX FIFO's peek/commit discipline and all-or-nothing enqueue, every coercion including its rejections, float formatting by integer decomposition (rounding carry, nan/inf, INT32_MIN, hex), every phase-1 verb bound to the exact `IEngine` call and the exact reply bytes, the full error taxonomy, target-B forwarding, and `describe` (liveness masks honoured, scope tags, CRLF framing, `end` terminator).

- **The firmware/host descriptor loop is closed.** The C++ test writes the exact `describe` block to `host/build/describe_sample.txt`; `tools/test_descriptor.py` parses that file through the real `skdev.descriptor.parse_describe`. It needs no device, so a descriptor format change fails in CI rather than on the bench. `skdev/__init__.py` now re-exports its serial-backed names lazily (PEP 562) so `skdev.descriptor` imports without pyserial.

- Zero-cost-off proven (terminal objects 0 bytes; default binary unchanged).

- Clean compile + link with `TERMINAL=1` on passthrough, delay, tape (`-Os`), mosc (QSPI).

- Host `tools/` all `py_compile`-clean; `parse_describe` round-trips the firmware's exact `describe` block (engine/version, param scope+range, config label maps, queries, caps hex).

- Fixed a pre-existing `-j` build race (the stamp recipes' `mkdir -p build` vs the core Makefile's plain `mkdir build`): all three stamps now order-only-depend on `$(BUILD_DIR)` instead of each racing to create it.

Done on hardware (Daisy Pod, macOS, 2026-07-31) - the checks from `terminal-transport.md` "To verify on hardware":
- `TransmitInternal` cadence with a draining host: the 938-byte `describe` dump (~15 packets) arrives whole and parses, so `flush_tx` neither spins nor corrupts across packets.

- Enumeration as the host sees it: a `/dev/tty.usbmodem*` node appears and the `tools/` VID-based discovery finds it unaided.

- End-to-end command path: connect, `describe`, `caps`, `query`, and a `set`/`get` param round-trip.

Not yet done:
- CDC RX re-arm across back-to-back packets (a >64-byte line arriving intact through the ring; send a >128-char line and expect `err line-too-long`, then a normal command, to prove the swallowed tail is not re-parsed).

- ~~`make test-hw` end to end.~~ **DONE 2026-08-01 - run against real hardware for the first time: `30 passed, 3 skipped`** (on `pstretch TERMINAL=1`, at both window sizes). Its stated blocker is gone: the note here used to read "no engine implements `live_params()`", which was already stale (11 did) and is now false. Every engine reports a real mask, confirmed live on the device - `describe` returns `masked=True` with 9 params advertised for pstretch instead of the whole 24-id `ParamId` enum, so the generic sweep tests only what the engine actually implements.

  Two host-side defects the first real run exposed, both fixed:

  - **A collection-time device open aborted the whole session.** `test_generic.py` opens the `Device` at MODULE level (the `parametrize` decorator calls `_params()`), and caught only `Timeout`. Anything else - here `SerialException` for a permission-denied port - became a pytest *collection* error, which killed the entire run including `test_descriptor.py`, the one file that needs no device. Both `_params()` and the `device` fixture now also catch `OSError` (pyserial's `SerialException` subclasses it); the fixture skips with the actual reason rather than a generic "no device attached", since a silent skip is indistinguishable from having no hardware.

  - **The documented "no-ops safely without hardware" guarantee only ever covered `Timeout`.** It now covers a port that exists but will not open: permission denied, or the port already held by `skterm.py` / `screen`.

  Operational note: the port is `root:dialout` mode 660. `sudo usermod -aG dialout $USER` is the fix, but it does **not** apply to already-open shells - use `sg dialout -c '...'` or re-login, or the run keeps failing after the group is added.

  How the remaining engines got their masks matters for keeping them honest. The three **generated** families derive theirs instead of listing them, because a hand-written mask would be a second copy of a table the generator owns and would drift on the next regeneration:

  - `FaustEngine` / `FaustChainEngine` (chorus, filter, voice) - from `Role::bound()`, i.e. which `ParamId`s actually captured a kernel slider at `init()` from the Traits bind table. The chain wrapper takes the **union** of both stages, which bind different tables.
  - `GenEngine` (gigaverb) - from `W::index_of(id) >= 0`, the wrap's own `ParamId` -> gen-parameter mapping (now a documented member of the wrap contract).

  The rest are hand-written against their `set_param` switch, which is the only authority they have: edrums, graincloud (granular's list plus `Aux`/`AltPos` for the cloud layer), reso, mosc, csound, chuck. `passthrough` is deliberately **empty** - it overrides neither `set_param` nor `set_config`, so the inherited all-live mask was advertising 24 params on an engine that has none.

### Host tooling defects found on first real use (2026-07-31, fixed)

`tools/` had never been executed against a device; `py_compile` catches syntax, not wrong API usage.

- `skdev/protocol.py` `open_serial`: `serial.Serial(..., dtr=True)` raises `ValueError` - `dtr` is a property, not a constructor keyword (pyserial's ctor has `dsrdtr`, which is flow control, a different thing). Fatal: nothing could connect. Now set after opening. Immaterial to this device either way - the firmware's `CDC_SET_CONTROL_LINE_STATE` handler is a no-op - but some hosts gate output on DTR.

- `skterm.py` `_install_completer`: `readline.parse_and_bind("tab: complete")` is GNU-readline syntax, silently ignored by the libedit backend macOS ships, so tab completion did nothing. Now detects the backend and binds `bind ^I rl_complete`.

## Not in phase 1 (unchanged from the specs)

`measure` (L2 audio-property tap, `SPK_TERMINAL_MEASURE`), `stim` (test-signal source, `SPK_TERMINAL_STIM`), the OSC/SLIP codec (`SPK_TERMINAL_OSC`), per-engine `live_params()`/ `live_configs()` masks and any engine-specific `handle_command` verbs (every engine currently uses the defaults), and enumeration of engine-specific `query` names inside `describe`.
