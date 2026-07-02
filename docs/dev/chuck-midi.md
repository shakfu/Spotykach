# Dev notes — Enabling MIDI in the ChucK engine (`ENGINE=chuck`) — ANALYSIS

> **Status: ANALYSIS / decision doc (2026-06-24). No code written.** Maps the two viable paths to MIDI in > the ChucK engine and recommends one. Path A (engine-mediated NoteOn, the existing roadmap "M4") is the > low-risk choice for "play the patch from a keyboard"; Path B (true language-level `MidiIn`/`MidiOut`) is a > substantially larger lift that also requires widening the platform MIDI contract to be worthwhile. See > `docs/dev/chuck-impl.md` for the engine roadmap and `src/engine/csound/csound_midi.h` for the Csound M4 > template both paths reuse.

---

## Where MIDI stands today — three closed layers

All three layers below are currently closed to language-level MIDI.

**1. `libchuck.a` is built with `-D__DISABLE_MIDI__`** (`scripts/fetch_chuck.sh:274`). That flag does two things:

- In `chuck_io.cpp:761-955` the entire `MidiIn`/`MidiOut` **class import is `#ifndef __DISABLE_MIDI__`**. So in the current build *`MidiIn`/`MidiOut` are not types in the language at all* — a `.ck` that says `MidiIn min;` fails to **compile**. (`MidiMsg` survives — it is registered outside the guard.)

- In `midiio_rtmidi.cpp:1063` only the `#else` stub branch compiles: empty ctors, `MidiIn::open()` returns FALSE. And `rtmidi.cpp` / `midiio_rtmidi.cpp` are not in the compiled source subset at all (`fetch_chuck.sh:287`; grep for `rtmidi` in the build script returns 0).

**2. The platform forwards only NoteOn + realtime clock.** `core.ui.midi.cpp:33-58` reads libDaisy's `midi_uart` (TRS/UART MIDI) and dispatches *only* `NoteOn -> IEngine::handle_midi_note(channel, note)` and SystemRealTime -> transport / `handle_midi_transport`. **NoteOff, velocity, CC, pitch-bend, and program-change are dropped at the platform layer.** The `IEngine` contract reflects exactly this (`iengine.h:88-89`): two hooks — `handle_midi_note(channel, note)` and `handle_midi_transport(bool)` — both running on the **main loop**, not the audio ISR.

**3. `ChuckEngine` implements neither hook yet** — `chuck_engine.cpp:426` only notes "the MIDI drain, M4, will join it." Nothing reaches a ChucK patch from MIDI today.

This splits into two genuinely different goals; pick between them before writing any code.

---

## Path A — engine-mediated MIDI (no language `MidiIn`; the roadmap's "M4")

Keep `__DISABLE_MIDI__`. The patch never uses `MidiIn`. Instead the engine turns a platform NoteOn into ChucK **globals/events**, which are already fully enabled.

Mechanism, mirroring Csound M4 (`csound_midi.h` + `csound_engine.cpp:283,347`):

- Override `ChuckEngine::handle_midi_note` -> push `{note, deck}` onto a lock-free SPSC `NoteQueue` (the existing `csound_midi.h` ring is reusable verbatim — already host-tested).

- Drain in `process()` (the audio ISR), so all VM mutation stays single-threaded as the project's whole ChucK design demands. For each note either `setGlobalFloat("midiHz", hz)` + `broadcastGlobalEvent(...)`, or spork a finite voice shred. The bridge API (`setGlobalInt/Float`, `signalGlobalEvent`, `broadcastGlobalEvent`) is present in `chuck_globals.h:226-239`.

- NoteOn-only -> notes must be self-terminating (a finite-duration voice), exactly the Csound constraint.

**Cost:** no `libchuck` rebuild, no platform-contract change, fits the existing CritSec/ReloadGate discipline. This is the documented M4 and the low-risk choice if the goal is "play the patch from a keyboard." **Downside:** patch authors do not get the idiomatic `MidiIn min; min.recv(msg)` — they must follow the engine's global-name convention.

---

## Path B — true language-level `MidiIn` / `MidiOut`

Substantially larger. Reading the internals shows the real shape.

**`MidiIn` is a `Chuck_Event` whose data path is RtMidi-fed.** `MidiIn::open` -> `MidiInManager::open` (`midiio_rtmidi.cpp:396`) does three things: `new RtMidiIn` + `openPort` + `setCallback(cb_midi_input)`; allocates a per-`(device,VM)` `CBufferAdvance` ring (`MIDI_BUFFER_SIZE 8192 * sizeof(MidiMsg)=4` -> ~32 KB each); and creates a VM event buffer so a shred can `min => now`. RtMidi's background thread calls `cb_midi_input` (`:639`), which `cbuf->put`s the 3 bytes into every VM's ring and signals the event buffer to wake waiting shreds. `MidiIn::recv` -> `m_buffer->get` drains the ring (`:626`). `MidiOut` is symmetric: `MidiOutManager` -> `RtMidiOut->sendMessage`.

So **RtMidi is the hardware-abstraction ChucK assumes: OS MIDI ports + a dedicated MIDI thread.** On Daisy there is no RtMidi, no OS ports, and `-D__DISABLE_THREADS__`. You cannot simply unset `__DISABLE_MIDI__` — that pulls `midiio_rtmidi.cpp`'s real branch, which `#include "rtmidi.h"` and references `RtMidiIn`/`RtMidiOut`, forcing `rtmidi.cpp` into the build, whose backends are all CoreMIDI/ALSA/WinMM/Jack.

The viable design is a **null-RtMidi backend** that keeps ChucK's class + buffer machinery but replaces only the device layer:

1. **Rebuild `libchuck.a` without `__DISABLE_MIDI__`** so `init_class_Midi` imports the classes and `MidiInManager`'s `CBufferAdvance` + event-buffer logic compiles. (One-time immortal type-system cost — fine.)

2. **Supply a minimal `rtmidi.cpp` stub** (a "null backend") so `midiio_rtmidi.cpp` compiles unmodified: `RtMidiIn::openPort` is a no-op that succeeds, no thread is ever started, `RtMidiOut::sendMessage` routes to the platform UART.

3. **Inject inbound MIDI host-side.** The engine's `handle_midi_note` (still main-loop) enqueues, and the ISR drain calls the manager's `cb_midi_input` (or directly `cbuf->put` + event-buffer signal) — i.e. the *engine* plays the role RtMidi's thread used to. Same NoteQueue -> ISR-drain discipline as Path A; only the drain *target* changes (the `CBufferAdvance` instead of a global).

**Four complications that make B more than "flip the flag":**

- **ISR-safety of `CBufferAdvance`.** Its `put` (`util_buffers.cpp:214`) is **single-writer by design** — the source comment literally reads *"shreds don't get interrupted, so m_write_offset will always be correct, right?"* It increments/wraps `m_write_offset` non-atomically and signals the event buffer. On Daisy the producer is the main loop and the consumer is the audio ISR — the exact main-loop/ISR boundary the project already guards with the PRIMASK `CritSec` (`chuck_alloc.cpp`). The `put` must run under that same CritSec, or be driven from inside the ISR drain. `initialize` also `malloc`s (one-time, at `open`).

- **NoteOff still does not exist.** Even with real `MidiIn`, `core.ui.midi.cpp` forwards only NoteOn. A stock MIDI patch keying on `0x80` / CC / bend gets nothing. So B is only worthwhile if you **also widen the platform**: a new `IEngine::handle_midi_msg(status,d1,d2)` hook and a raw-message branch in `_process_midi`. Otherwise you have paid for the heavy machinery and still deliver NoteOn-only.

- **`MidiOut` needs a platform reach-through.** `send()` must hit `_hw.midi_uart.EnqueueMessage` (+ the `TransmitEnqueuedMessages` already in `tick()`). The engines are deliberately HW-agnostic (they see only `EngineContext`), so this needs a new **midi-sink callback threaded through `EngineContext`** — an architectural addition, not a local change.

- **SDRAM.** ~32 KB ring per open is trivial against the ~50 MB free; worth noting, not a blocker.

---

## Recommendation

- **Goal = "keyboard plays the patch"** -> **Path A.** It is the existing roadmap, needs no `libchuck` rebuild and no platform-contract change, and slots into the CritSec/ReloadGate model already proven on hardware.

- **Goal = "run stock ChucK MIDI `.ck` unmodified / give authors real `MidiIn`/`MidiOut`"** -> **Path B**, but understand it is: rebuild libchuck, write a null-RtMidi backend, host-inject via the manager under CritSec, **plus** widen `core.ui.midi.cpp` + `IEngine` for full messages, **plus** an `EngineContext` midi-out sink. B without that platform widening gives you the API but the same NoteOn-only data — the worst trade.

A pragmatic middle path: do **A now** (it satisfies the playable-keyboard need cheaply), and treat **B** as a later capability gated on whether portability of stock MIDI `.ck` files is actually a product goal.

**Highest-uncertainty item to bench first (if B):** the `CBufferAdvance` put/get behaviour across the main-loop/ISR boundary. That is the load-bearing assumption; confirm it under the CritSec before committing to the rebuild.

---

## Key references

- `scripts/fetch_chuck.sh:274,287` — `-D__DISABLE_MIDI__` and the compiled-source subset (no rtmidi).

- `thirdparty/chuck/src/core/chuck_io.cpp:761-955` — `init_class_Midi`, the guarded `MidiIn`/`MidiOut` import.

- `thirdparty/chuck/src/core/midiio_rtmidi.cpp:396` (`open`), `:528` (`add_vm`), `:626` (`recv`), `:639` (`cb_midi_input`), `:1063` (the `__DISABLE_MIDI__` stub branch).

- `thirdparty/chuck/src/core/util_buffers.cpp:214` — `CBufferAdvance::put`, the single-writer ring.

- `thirdparty/chuck/include/chuck_globals.h:226-239` — `setGlobalInt/Float`, `signal/broadcastGlobalEvent`.

- `src/ui/core.ui.midi.cpp:33-58` — platform MIDI dispatch (NoteOn + realtime only).

- `src/engine/iengine.h:88-89` — the `handle_midi_note` / `handle_midi_transport` hooks.

- `src/engine/csound/csound_midi.h`, `src/engine/csound/csound_engine.cpp:283,347` — the Csound M4 template.
