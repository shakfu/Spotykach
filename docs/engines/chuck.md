# chuck engine (`ENGINE=chuck`)

The `chuck` engine runs the [ChucK](https://chuck.stanford.edu) language and virtual machine as a synth on the spotykach. A program (a `.ck` text file) is compiled and run at runtime, so the **patch — not the firmware — defines the sound**. You load programs from the SD card, switch between them live, and play them from the panel knobs.

ChucK is *strongly-timed*: a program sporks concurrent "shreds" that the VM schedules sample-accurately against a unit-generator graph. So a single patch can run several self-playing voices in polyrhythm, not just one signal path.

It behaves like any other engine on the panel, but is built and flashed a little differently: ChucK's code is ~1.1 MB (too big for SRAM), so this is a **QSPI build** that executes from flash, with ChucK's heap in SDRAM. Internals (memory model, allocator, the live-swap design, bring-up notes, roadmap) are in [`docs/dev/chuck-impl.md`](../dev/chuck-impl.md).

## Build & flash

One-time: build the ChucK library (needs the `arm-none-eabi` GCC toolchain). The script fetches a pinned ChucK release and cross-builds it for the Daisy — there is no official ChucK Daisy port, so the script *is* the port:

```text
scripts/fetch_chuck.sh           # fetch ChucK + cross-build libchuck.a (gitignored sources)
```

Then build + flash the engine (put the board in DFU first):

```text
make engine-chuck                # clean + build the QSPI image + flash
make program-chuck               # re-flash the last build without rebuilding
```

Recover the board at any time by flashing any normal engine.

## Controls

With the built-in program (used when no SD patch is loaded — a `SawOsc => LPF => dac` drone):

| Control | Effect |
|---|---|
| **PITCH** | pitch |
| **SIZE** | brightness (filter cutoff) |
| **MIX** | level |
| **Alt + PITCH** | patch selector (see below) |
| **centre mode LED** | white = built-in program, cyan = an SD patch is loaded |

What each knob actually does is up to the patch (it reads named global variables) — the table above is the convention the bundled patches follow. Patches can also be played from a **MIDI keyboard**: MIDI *input* is supported through ChucK's standard `MidiIn` (see [Limitations → MIDI](#limitations)).

## Loading patches from the SD card

Put `.ck` programs in a `chuck/` folder at the card root, named `0.ck` … `7.ck`:

```text
<card>/chuck/0.ck
<card>/chuck/1.ck
```

Ready-to-copy examples are in [`examples/chuck/`](../../examples/chuck/) — a clean two-saw drone, a fat super-saw, a concurrent generative pad, and an STK Rhodey arpeggio, with a README.

- At boot the engine **auto-loads** the lowest-numbered patch (or the built-in if the card has none).

- **Hold Alt and turn PITCH** to scroll the selector (a dot per patch around the rings); **release** to switch to it live. The centre LED turns cyan for an SD patch, white for the built-in.

- Anything missing, empty, or that fails to compile falls back to the built-in, so the unit always makes sound.

### How live switching works

The engine keeps **one** ChucK VM running for the whole session and swaps the patch's shreds inside it. The first time you select a given patch it is compiled (a brief silence while that happens); every later switch back to it is **instant** (the compiled program is cached and re-run, not recompiled). Memory use therefore rises a little the first time each distinct patch is visited and then stays flat — you can cycle patches indefinitely without the unit degrading. (The why, and the ChucK-internals story behind it, is in [`docs/dev/chuck-impl.md`](../dev/chuck-impl.md).)

## Writing a patch

A patch is an ordinary `.ck` program — no required header. To be driven by the panel, declare and read these globals (deck A / deck B):

| Knob | Global |
|---|---|
| PITCH | `speedA` / `speedB` |
| MIX | `mixA` / `mixB` |
| SIZE | `sizeA` / `sizeB` |
| ENV | `envA` / `envB` |
| modifier layer | `fbA`, `modspA`, `modampA` (+ `B`) |

A program reads whichever globals it declares; writes to globals a patch doesn't declare are ignored. Keep to the convention the bundled patches follow — PITCH = pitch/tempo, SIZE = brightness, MIX = level — so the knobs feel consistent when you switch patches. A minimal patch:

```chuck
global float speedA;   // PITCH
global float sizeA;    // SIZE
global float mixA;     // MIX

SawOsc s => LPF f => dac;
while( true )
{
    110.0 + speedA * 770.0   => s.freq;    // pitch
    1500.0 + sizeA * 6000.0  => f.freq;    // brightness
    (0.15 + mixA * 0.85) * 0.3 => s.gain;  // level
    10::ms => now;                          // re-poll the knobs
}
```

**Format notes** — a UTF-8 BOM and CRLF (Windows) line endings are tolerated (the engine strips them on load); an empty or whitespace-only file falls back to the built-in. There is no structural validation beyond that — the real check is whether the program compiles.

**Two caveats from the one-VM design** (fine for a curated bank, worth knowing if you author your own):

- A patch's compiled code is cached for the session, so **editing a slot's `.ck` file on the card while the unit is running won't take effect until you power-cycle**.

- All loaded patches share one namespace, so **two patches that define the same top-level `class` name** will collide when the second is compiled. Give your classes distinct names if you use them across slots.

## CPU and the meter

Every concurrent shred and every UGen costs time inside one audio block, and code runs from QSPI flash (slower than SRAM), so heavy or dense patches can run out of headroom. If a patch genuinely can't keep up, the engine **mutes it and lights both rings solid red** (a safeguard that keeps the controls alive) — switch to another patch with Alt+PITCH to recover. Lighter voices and a capped shred/voice count are the fix.

Build with `METER=1` to see the headroom on the panel instead of guessing:

```text
make engine-chuck METER=1
```

- **Ring A** = CPU load (green → amber → red; red ≈ at/over budget).

- **Ring B** = SDRAM pool usage (how much of ChucK's heap the loaded patches occupy).

## Limitations

- **UGens:** this is a bare-metal, **core-only** ChucK build. *Available:* the oscillators (`SinOsc`/`SawOsc`/`SqrOsc`/`TriOsc`/`PulseOsc`/`Noise`), filters (`LPF`/`HPF`/`BPF`/`ResonZ`/…), envelopes (`ADSR`/`Envelope`), `Gain`, delays, the reverbs (`JCRev`/`NRev`/`PRCRev`), the FFT/analysis UAnae, and the **STK instruments** (`Rhodey`, `Wurley`, `TubeBell`, `Mandolin`, `Moog`, …), plus `Math`/`Std`, sporked shreds, events, and global variables. *Excluded:* **chugins** (no dynamic loading), **sound-file I/O** (`SndBuf`/`WvIn`/`WvOut` — no filesystem audio), and the **OSC/HID/serial** device UGens (the host owns I/O). **`MidiIn` is the exception** — it has been re-introduced over the UART (the host feeds it; see MIDI below), though **`MidiOut` is inert**.

- **MIDI:** MIDI **input** is supported. A patch uses ChucK's standard API unchanged — `MidiIn min; min.open(0); MidiMsg msg; min => now; while( min.recv(msg) ) { /* msg.data1/2/3 */ }` — and receives the **full channel-voice stream** (NoteOn/NoteOff with real velocity, control change, pitch-bend, aftertouch, program change) plus system realtime (clock/start/continue/stop), exactly as a desktop ChucK patch would. The host owns the UART and feeds bytes into ChucK's own MIDI buffer (there is no RtMidi backend on bare metal), so the language semantics are unchanged: `min => now` wakes on a message, `recv()` drains the queue, and `data1` is the status byte (incl. channel), `data2`/`data3` the data bytes. A ready example is [`examples/chuck/midi_in.ck`](../../examples/chuck/midi_in.ck); the design and how-to are in [`docs/dev/chuck-midi-in-porting.md`](../dev/chuck-midi-in-porting.md).

  **Remaining incompatibilities with desktop ChucK** (all are limitations of the bare-metal port, not changes to the language API):
  - **No MIDI output.** `MidiOut` is registered so patches that reference it still compile, but `open()` always fails — `send()`/`noteOn()`/etc. are silent no-ops. Nothing is sent out the UART.

  - **One virtual device, no enumeration.** There is a single input device (device 0). `MidiIn.open(n)` opens that one UART regardless of `n`, `open("name")` maps to device 0, and `MidiIn.name()` returns `"UART (virtual)"` — patches that enumerate or pick devices by name won't find a list.

  - **3-byte messages only.** `MidiMsg` carries `data1`/`data2`/`data3` as usual, but **SysEx and MIDI system-common messages are not delivered** (they don't fit the 3-byte model).

  - **Wake path pending on-hardware sign-off.** The re-introduction is build-verified (library + firmware link with matching ABI); confirming that a shred blocked on `min => now` actually resumes on the threadless build is the one step still to be checked on a device.

- **CPU:** code executes from QSPI flash, so heavy or dense patches can glitch or trip the mute-and-recover safeguard. Watch ring A on a `METER=1` build.

- **Memory:** ChucK gets a 40 MB SDRAM pool for its VM and patches. Live switching is memory-stable (see "How live switching works") — patches are compiled once and cached, so cycling them does not grow memory without bound.
