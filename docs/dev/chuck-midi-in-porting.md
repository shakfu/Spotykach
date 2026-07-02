# Porting the ChucK `MidiIn` re-introduction to another Daisy/ChucK firmware

This is a replication guide for the change implemented in this repo (`daisy-apps`) so it can be applied to a sibling project that also runs a cross-compiled ChucK on a Daisy device - in particular `~/projects/personal/sk-engines` (the "spotykach" firmware). It records *what was done*, *why each piece is needed*, and *what differs* in the target project. For the design rationale and the feasibility analysis, read [`chuck-midi-in.md`](./chuck-midi-in.md) first; this document is the how-to.

## What it gives you

ChucK's in-language MIDI device classes (`MidiIn` / `MidiOut`) are compiled out of the bare-metal build (`__DISABLE_MIDI__`), so patches cannot do the desktop `MidiIn min; min.recv(msg);`. This change rebuilds `libchuck.a` with `MidiIn` enabled but with RtMidi (the OS backend, needs a callback thread) replaced by a thread-free `MidiInManager::inject()` hook the host calls. A `.ck` patch can then use real `MidiIn` unchanged, which is the only way to get the full MIDI vocabulary (CC, pitch-bend, clock, real velocity) and to run unmodified desktop MIDI patches.

## The key insight (why this is safe)

The one genuinely uncertain part was the **threadless event wake**: ChucK's `MidiIn` is a `Chuck_Event`, and a shred blocks on `min => now`; with `__DISABLE_THREADS__` there is no RtMidi callback thread to wake it. This turned out to be already-proven in production: any ChucK firmware that delivers MIDI by broadcasting a `Chuck_Event` from the audio/run() thread (e.g. a "global bridge" that does `broadcastGlobalEvent` and a `.ck` doing `evt => now`) is direct evidence that a `Chuck_Event` resumes a waiting shred with no threads. `MidiIn` reuses the *same* per-VM event-buffer machinery (`CBufferAdvance::put` -> the VM's event buffer, serviced by `compute()` each block). So the wake is confirmation, not discovery. (`daisy-apps` had such a bridge; `sk-engines` does not yet - but the mechanism is identical, so it carries over.)

## Applicability check (do this first)

The library surgery ships as a unified diff against the **vendored ChucK source**. Confirm the target uses the same pinned ChucK so the patch applies cleanly:

```sh
# same CHUCK_REF (e.g. chuck-1.5.5.8) in both fetch scripts:
grep CHUCK_REF scripts/fetch_chuck.sh
# the vendored MIDI files match the patch's base (compare to this repo's pristine source):
diff -q <target>/thirdparty/chuck/src/core/midiio_rtmidi.cpp \
        <thisrepo>/thirdparty/chuck/src/core/midiio_rtmidi.cpp   # see note below
```

For `sk-engines` this was verified: same `CHUCK_REF=chuck-1.5.5.8`, and its `thirdparty/chuck/src/core/midiio_rtmidi.{h,cpp}` are **byte-identical** to the unpatched source here, so `scripts/patches/midi_daisy.patch` applies as-is. If a target pins a different ChucK release, regenerate the patch (see [Regenerating the patch](#regenerating-the-patch)).

> `thirdparty/chuck` is gitignored and regenerated from upstream by `fetch_chuck.sh`, so any hand-edit to > the vendored source is non-durable - it MUST be carried as a patch the fetch script re-applies. That is > the whole reason for `scripts/patches/midi_daisy.patch` plus the apply step below.

---

## Part A - library surgery (reusable, transfers verbatim)

This is the bulk of the work and is identical across projects. It rebuilds `libchuck.a` with `MidiIn`.

### A1. Copy the patch

Copy `scripts/patches/midi_daisy.patch` from this repo into the target's `scripts/patches/`. The patch edits the two vendored files only:

- **`midiio_rtmidi.h`**: stop including `rtmidi.h` (all uses are stubbed); reduce `RtMidiIn`/`RtMidiOut` to minimal stand-ins (only `getPortName()` is ever referenced, by `MidiIn.name()`); declare `static void MidiInManager::inject(t_CKINT device, t_CKBYTE status, t_CKBYTE d1, t_CKBYTE d2)`.

- **`midiio_rtmidi.cpp`**: remove every `new RtMidiIn/RtMidiOut` + `openPort`/`setCallback`/ `getPortCount`/`getPortName`/`sendMessage`/`ignoreTypes`/`RtMidiError` call. `MidiInManager::open` registers a single virtual UART device backed by a static stand-in instead of an RtMidi port; the named-`open` overload maps to device 0; `MidiOutManager::open` always returns FALSE (MidiOut stays in the type system but is inert - `MidiOut.send()` is guarded by `m_valid`, so its dead `mout->...` calls are removed to compile without `rtmidi.h`); `probeMidiIn/Out` print a fixed line. The new `inject()` builds a 3-byte message and routes it through the existing `cb_midi_input` -> `CBufferAdvance::put`, which both fills the buffer `MidiIn::recv()` drains AND queues the shred wake.

### A2. Drop `__DISABLE_MIDI__` from EVERY ChucK define site

The ChucK feature defines set the class layouts; the archive and every TU that includes `chuck.h` MUST agree, or vtables/member offsets silently corrupt. So remove `-D__DISABLE_MIDI__` from **all** places the target passes the ChucK defines - not just the library build. In `daisy-apps` there were two; in `sk-engines` there are **three**:

| Site | `daisy-apps` | `sk-engines` |
|---|---|---|
| Library build (builds `libchuck.a`) | `scripts/fetch_chuck.sh` (DEFS) | `scripts/fetch_chuck.sh:274` |
| Pod harness | `pod/Makefile.chuck` | `pod/Makefile.chuck:47` |
| **Full firmware** (the real device build) | n/a | **`Makefile:220`** (the `ENGINE=chuck` branch) |

Miss one and you get a corrupted-ABI runtime crash, not a compile error. Grep to be sure you caught them all: `grep -rn "__DISABLE_MIDI__" Makefile* pod/Makefile.chuck scripts/fetch_chuck.sh`.

### A3. Add `midiio_rtmidi` to the compiled source set

In `scripts/fetch_chuck.sh`, add `midiio_rtmidi` to the `CPP_SRCS` list (it sits naturally after `chuck_io`). Do **not** add `rtmidi` - the 166 KB OS backend stays out; only the ~57 KB ChucK-facing `midiio_rtmidi.cpp` is built, with its RtMidi calls stubbed by the patch.

### A4. Make the patch durable (apply it from `fetch_chuck.sh`)

Add an idempotent apply step right after the source-present check in `fetch_chuck.sh` (search for `chuck.h missing`). It must skip cleanly when re-run on already-patched source:

```sh
midi_patch="$repo_root/scripts/patches/midi_daisy.patch"
[ -f "$midi_patch" ] || die "missing $midi_patch (needed to re-enable ChucK MidiIn on the Daisy build)"
if grep -q "MidiInManager::inject" "$core/midiio_rtmidi.cpp" 2>/dev/null; then
    echo "==> Daisy MIDI patch already applied (skipping)"
else
    echo "==> applying Daisy MIDI patch -> $core"
    patch -p1 -d "$core" < "$midi_patch" \
        || die "failed to apply $midi_patch (CHUCK_REF may have drifted from the patch)"
fi
```

### A5. Rebuild and verify the library

```sh
scripts/fetch_chuck.sh          # skips fetch if source present, applies patch, recompiles libchuck.a
```

Success looks like: `midiio_rtmidi.o` in the archive (`arm-none-eabi-ar t .../libchuck.a | grep midiio`)
and the script's own link-test (`new ChucK() + compileCode() + run()`) passing. The `MidiIn` classes add ~38 KB of QSPI text.

---

## Part B - host wiring (project-specific)

Part A makes `MidiIn` *available*; Part B *feeds* it. The contract is one call:

```cpp
// from the SAME thread as ck->run() (the audio ISR), ideally just before run() each block:
MidiInManager::inject(/*device*/0, /*status*/ statusByte, /*data1*/ d1, /*data2*/ d2);
```

`status`/`d1`/`d2` are the raw MIDI bytes (e.g. NoteOn ch1 = `0x90`, note, velocity). `inject()` reaches every VM that opened device 0 via `MidiIn min; min.open(0);`. Two rules:

- **Inject on the run() thread, before `ck->run()`.** A shred's `min.open(0)` runs inside `run()` and mutates the device's buffer map; injecting from a different thread races it. Injecting on the run() thread, right before `run()`, also means the queued wake is serviced *this* block.

- **First block after a patch loads drops its notes** (open() hasn't registered the buffer yet) - benign.

### `daisy-apps` did the minimal version

It already had a "global bridge" delivering NoteOns. In `ChuckEngine::process()`, right before
`ck->run()`, alongside the bridge it added one `inject()` per popped note (status `NoteOn|deck`, note,
fixed velocity 100, since the note ring carried no velocity). No new MIDI input path was needed.

### `sk-engines` needs a small input path (it has no bridge)

Differences in `sk-engines` to account for:

- Its `ChuckEngine` does **not** override `handle_midi_note` (it inherits the `IEngine` no-op at `src/engine/iengine.h:88`), so ChucK currently ignores MIDI entirely. There is no `midi_note.h` / note-ring in the chuck engine.

- MIDI arrives on the **main loop** at `src/ui/core.ui.midi.cpp` (`_process_midi()` -> `_engine.handle_midi_note(channel, note)` around line 49), where full event info is available (`PopEvent()` gives type/channel/data).

- `ChuckEngine::process()` is at `src/engine/chuck/chuck_engine.cpp:448` and calls `ck->run()` at line ~478, with **no** pre-run MIDI/globals block to piggyback on - you add one.

Recommended wiring (gives the real payoff - full MIDI, not just notes):

1. Add a tiny lock-free SPSC ring of raw 3-byte messages to `ChuckEngine` (mirror this repo's `src/engine/midi_note.h` `NoteQueue`, but store `{status,d1,d2}` instead of `{note,deck}`).

2. Override `ChuckEngine::handle_midi_note` (and/or extend `core.ui.midi.cpp` to forward *all* channel messages, not only NoteOn) to **push** the raw bytes onto that ring from the main loop.

3. In `ChuckEngine::process()`, **before** `ck->run()` (line ~478), drain the ring and `MidiInManager::inject(0, status, d1, d2)` for each.

4. `#include "chuck_globals.h"` is already pulled; `MidiInManager` and `MIDI_NOTEON` come from `midiio_rtmidi.h` via `chuck.h` once MIDI is enabled - no extra include needed.

If you only want to match `daisy-apps` (NoteOn-only) first, push just NoteOns and inject
`(0, 0x90|channelDeck, note, velocity)`.

### The example patch

Copy `examples/chuck/midi_in.ck` from this repo (opens `MidiIn`, blocks on `min => now`, drains with `min.recv(msg)`, sporks a voice per NoteOn). Put it on the SD card as a numbered slot to test.

---

## Verification

1. `scripts/fetch_chuck.sh` - library rebuilds, link-test passes (Part A5).

2. Build the pod harness AND the full firmware (`make ENGINE=chuck APP_TYPE=BOOT_QSPI ...`). Both must link with **zero** undefined references - that is the matching-ABI proof. Confirm the symbol resolves:
   `arm-none-eabi-nm <elf> | grep MidiInManager.*inject`.

3. On hardware: flash, load `midi_in.ck`, play NoteOns. Voices sounding = `min => now` woke and `min.recv()` drained. This is the only step that needs the device; everything above is build-time.

## Gotchas

- **ABI sync is the #1 failure mode.** Every TU that includes `chuck.h` must drop `__DISABLE_MIDI__` (Part A2). A mismatch is a silent vtable/offset corruption at runtime, not a build error.

- **`thirdparty/chuck` is gitignored.** Hand-edits vanish on re-fetch; the patch + the `fetch_chuck.sh` apply step (A4) are what make it durable. The patch file itself lives under `scripts/` (tracked).

- **Inject on the run() thread, before `run()`** (Part B) - else you race `min.open()` on the device map.

- **`MidiOut` is inert** on this build (registered but `open()` fails). Patches referencing it compile and run; `send()` is a no-op. Wire a UART writer into `MidiOutManager::open`/`MidiOut::send` if you ever need MIDI output.

- **Flash budget**: enabling the classes adds ~38 KB QSPI text. Check against the QSPI region.

## Regenerating the patch

If the target pins a different ChucK release (the patch won't apply), regenerate it:

```sh
cp -r thirdparty/chuck/src/core/midiio_rtmidi.h   /tmp/pristine/   # before any edits (fresh fetch)
cp -r thirdparty/chuck/src/core/midiio_rtmidi.cpp /tmp/pristine/
# ... re-apply the Part A1 edits by hand to the new source ...
for f in midiio_rtmidi.h midiio_rtmidi.cpp; do
  diff -u /tmp/pristine/$f thirdparty/chuck/src/core/$f \
    | sed -e "1s|.*|--- a/$f|" -e "2s|.*|+++ b/$f|"
done > scripts/patches/midi_daisy.patch
patch -p1 --dry-run -d /tmp/pristine < scripts/patches/midi_daisy.patch   # must apply clean
```

## Reference: the files touched in `daisy-apps`

- `scripts/fetch_chuck.sh` - dropped `__DISABLE_MIDI__`, added `midiio_rtmidi` to `CPP_SRCS`, added the idempotent patch-apply step.

- `pod/Makefile.chuck` - dropped `__DISABLE_MIDI__` (ABI sync).

- `scripts/patches/midi_daisy.patch` - the vendored-source surgery (RtMidi stub + `inject()`).

- `src/engine/chuck/chuck_engine.cpp` - `process()` injects each NoteOn into device 0 before `run()`.

- `examples/chuck/midi_in.ck` - desktop-portable `MidiIn` example.

- `docs/dev/chuck-midi-in.md` - status + design; this file - the porting guide. </content> </invoke>
