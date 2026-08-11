# Web front-end: the browser pass

The `web/` suite runs in bun and covers the logic. It cannot cover the four browser APIs the app is
built on — File System Access, `decodeAudioData`, WebSerial, service workers — because a DOM shim can
only assert that the code *calls* them. This is the mechanical run that closes that gap: about 40
minutes with a card, a Daisy and both browsers. Budget the extra ten for C8b, which needs its own
flash: the terminal now speaks two codecs and they share only the port handling.

Work top to bottom and record the result. A check that is skipped is not a check that passed, so write
`skipped` and why. Everything here corresponds to "Remaining verification" in
[`web-frontend.md`](web-frontend.md).

## Before starting

```
make test-web                 # must be green before a browser is involved
make web-data                 # only if scripts/card_layout.py changed since the last export
make web-serve                # builds web/dist/app.js, then http://localhost:8000
```

The page runs a bundle built from `web/src/`, so **anything you change in `src/` needs
`make web-build`** before the browser sees it. `make web-serve` does that for you; a reload alone will
not. (`make test-web` fails if the committed bundle is older than the sources, which is the same
mistake caught earlier.)

You need:

- **Chrome or Edge** and **Safari** (Firefox instead of Safari is fine — the point is a browser with no
  File System Access API).
- A **FAT32 SD card**, ideally one that has been in a Mac (so it has `System Volume Information` and
  `.DS_Store` on it — that is the interesting case, not a clean one).
- One **mp3** and one **flac**, ideally a few minutes long and known-good.
- Optional, for C8: a Daisy flashed with `make ENGINE=<engine> TERMINAL=1`.
- Optional, for C8b: the **same engine** flashed again with `make ENGINE=<engine> TERMINAL=1 OSC=1`.
  Same engine, because C8b's central assertion is that the generated control surface is identical
  across the two codecs — a different engine makes that comparison meaningless.

`file://` does not work and is not a bug: ES modules will not load over it and the browser APIs are
offered only over HTTPS or localhost.

## The checks

### C1 — the page loads at all (Chrome)

Open `http://localhost:8000`. Expect the Build tab, and in the header a line reading
`10 banks, scan floor 32 KB, name limit 12`.

Open DevTools and check the console is clean. **A blank panel with a console error is the failure this
check exists for**: every view builds its UI imperatively in one `mount()` call, so one bad property
name takes out a whole tab.

Click each of the five tabs. Each must render content, and the URL fragment must follow
(`#build` … `#terminal`). Reload on `#verify` and confirm it comes back on Verify.

### C2 — the page degrades, not breaks (Safari)

Same URL, same five tabs. Everything must render. Specifically:

- **Verify** — "Choose card folder" is *disabled*, with the note "In-place card access needs Chrome or
  Edge; dropping a folder works here." "Browse for folder" stays enabled.
- **Build** — "Download a starter card (.zip)" enabled, "Write onto a card" disabled.
- **Terminal** — leads with the released-firmware caveat *and* says there is no WebSerial here and no
  zip-shaped substitute.
- **Reference** — fully functional. It needs no browser API at all, so anything disabled on this tab is
  a bug.

This is the graceful-degradation path the whole design was shaped around, and C2 is the check that has
never been run against a real File API.

### C3 — reading a real card

With a card that has content on it, in **Chrome**:

1. Verify → "Choose card folder" → pick the card's **root**.
2. Expect a file count and total size, plus "this card can be edited in place".
3. Findings must match the CLI exactly. Confirm it:

   ```
   python3 scripts/sk_card.py verify /Volumes/<CARD>
   ```

   Same errors, same warnings, same fix text. A difference here is a real defect in `web/src/core/verify.ts`
   — the checker is pinned against a fixture, not against a real card.

4. `System Volume Information`, `.Spotlight-V100`, `.fseventsd` and `.DS_Store` must not appear in the
   findings, and must not produce a permission error either.

Then in **Safari**: Verify → "Browse for folder" → same card. The findings must be identical. (Paths
arrive via `webkitRelativePath` here and via a handle walk in Chrome, and the first path segment is
stripped in one case and not the other — if a folder name leaks into the paths, everything resolves to
the wrong bank and the findings will differ wildly.)

### C4 — dropping a folder

Drag the card's folder onto the Verify dropzone, in **both** browsers. Expect the same findings as C3.

**Was a real bug, now fixed - confirm it stayed fixed.** `platform/cardsource.ts` used to read
`dt.items` *after* awaiting `getAsFileSystemHandle()`, by which point the browser has invalidated the
drag data store: a dropped loose file returned nothing at all, silently. Every read of the store now
happens synchronously before the first `await`, and `test/cardsource.test.ts` drives it with a fake
transfer that expires the same way (verified to fail against the old ordering). What is left to check
on real hardware is only that a real browser expires it the way the fake does:

- drop a **single loose file** onto Verify - it must be read, not silently ignored;
- drop **several** folders or files at once;
- drop a folder in Safari, where only the legacy `webkitGetAsEntry` walk exists.

### C5 — converting real audio

Convert tab, in **Chrome** and again in **Safari**:

1. Drop the mp3. Engine `pstretch`, first slot 1.
2. Convert. Expect `CLIP01.WAV`, a byte count, and the source rate/channels read off the decode.
3. Download the zip, unpack it, and confirm the header with the same parser the firmware mirrors:

   ```
   python3 -c "import sys, pathlib; sys.path.insert(0, 'scripts'); import card_audio; \
     print(card_audio.parse_wav(pathlib.Path('CLIP01.WAV')).describe())"
   ```

   Expect `16-bit PCM, mono, <rate> Hz`, with the rate the tab reported. Repeat with the flac.

4. Convert something under a second and confirm the "looped up" note appears and the file clears
   32 KB — the scan floor is silent on the device, so this is the note that prevents a mystery.

Then **listen to it on the device**: copy `pstretch/CLIP01.WAV` to a card, load it, play it. The
fixtures prove the *encoder* matches Python byte for byte; they say nothing about `decodeAudioData`
producing musically correct samples. Wrong-endian or half-rate audio still passes every host test.

### C6 — writing onto a card in place (Chrome only)

1. Format a card FAT32, or empty an existing one.
2. Build → "Write onto a card" → grant write access. Expect "Every folder, config and README is in
   place".
3. Compare against the CLI's own card:

   ```
   python3 scripts/sk_card.py init --no-demo /tmp/ref-card
   diff -r /tmp/ref-card /Volumes/<CARD>
   ```

   These must be identical. Byte-identity with `sk_card.py init` is asserted per file by SHA-256 in the
   test suite, so a difference here means the *writing*, not the content, is wrong.

4. Run Build again on the same card. Expect "N files were already there and were left untouched" and
   **no** overwrite — pointing this at a card whose `SK/config.txt` you tuned must not reset it. Edit
   `SK/config.txt` first and confirm your edit survives.
5. Point Verify at the result: no problems found.

### C7 — the Reference tab

Cheap, and it is new. Reference tab, either browser:

- One section per bank, and the count under the filter box must match the header's bank count.
- Filter box: type `raw` → radio alone. Type `tape` → four (granular, tape, shuttle, softcut), because
  the text box is a search. Clear it → all of them return.
- Click the `tape` chip → exactly one section, chip marked active. A chip is a selection, not a search,
  so this is the difference worth eyeballing. Click again → all return.
- Cross-check any two entries against `python3 scripts/sk_card.py layout`. Same formats, same slot
  names, same firmware citations.

### C8 — the terminal against real hardware (Chrome only)

Needs a `TERMINAL=1` build; skip and say so otherwise.

1. Flash `make ENGINE=<engine> TERMINAL=1`, connect the panel USB jack.
2. Terminal tab → Connect → pick the port. Expect `USB 0x0483:0x____` and a `describe` reply.

   **With the device unplugged**, Connect first and confirm the chooser is empty, then cancel it. The
   console must explain that nothing is reporting vendor id `0x0483` and reveal "List every serial
   port" — WebSerial cannot distinguish an empty chooser from a cancelled one, so this path used to be
   completely silent. Click the unfiltered button and confirm every serial port on the machine is
   offered. This is the escape hatch for a board that puts a different USB bridge in front of the CDC
   endpoint, and the *only* way to know whether the vendor-id filter is right for your hardware is to
   compare the two lists.
3. The control surface must be **generated** — a slider per advertised parameter with its real range,
   buttons only for the pads this engine implements. An engine that advertises fewer parameters must
   show fewer controls. Compare against `python3 tools/skterm.py` on the same build.
4. Move a slider; confirm the audio changes and the reply is `ok`.
5. `query cpu` a few times; the plot must track. Press `reset cpu` and confirm min/max collapse.
6. `query usb` renders as a table, not a flag soup.
7. Confirm a destructive verb (anything that clears a buffer or writes the card) prompts first.
8. **Unplug the device while connected.** The console must report `device disconnected`, the button
   must return to "Connect", the command line must go disabled and the CPU poll must stop. The failure
   this replaces was silent: the read loop ended, the tab went on claiming a connection, and the poll
   kept firing commands that timed out three seconds at a time. Then replug and reconnect — a second
   session in the same tab must work.

The transport is the unverified part: the client is tested against a scripted fake, so what C8 is
really checking is chunk boundaries from a real CDC endpoint. Paste a long command and watch for a
line split across two reads being mangled.

### C8b — the same terminal over the OSC codec (Chrome only)

Needs a **second flash**: `make ENGINE=<engine> TERMINAL=1 OSC=1`. Added 2026-08-11, when the browser
client gained the OSC codec; never yet run.

Why a separate check rather than a variant of C8: the codec changes the *unit* of the transport, from
a newline-terminated string to a SLIP-delimited byte frame. The two paths share the port handling and
nothing else, and the host suite covers everything except a real CDC endpoint — which is exactly where
the interesting failures are.

1. Before connecting, set the dropdown beside Connect to **OSC codec**. Connect.
   - The status line must say `OSC codec`, and the dropdown must go **disabled** for the session. The
     codec is a property of the firmware, not the connection.
2. **Connect with the wrong codec selected, deliberately, both ways round.** A line build addressed as
   OSC and an OSC build addressed as lines both produce a device that never answers. Confirm this
   surfaces as a timeout and an empty descriptor rather than a hang or a blank tab — this is the
   mistake a user will actually make, and the only signal is silence.
3. The generated control surface must be **identical** to C8's on the same engine: same sliders, same
   ranges, same buttons. It is built from a `Descriptor` that both codecs reduce to the same shape, so
   any visible difference here is a codec bug, and this is the browser equivalent of the 63/63 parity
   sweep. Compare side by side against `python3 tools/skterm.py` if in doubt.
4. Move a slider; confirm the audio changes exactly as it did over lines.
5. The console now takes **addresses**, not commands. Confirm:
   - `/sk/a/param/speed 0.5` writes, and is acknowledged.
   - `/sk/a/param/speed` alone reads it back (a read is a message with no type-tag string at all).
   - `set param speed a 0.5` is refused with a message saying the build speaks OSC. It is deliberately
     not translated.
   - `/sk/cfg/route 2` writes an int and `/sk/cfg/route 2.0` a float — the console types an argument
     from how it was *spelled*, which is the one place that ambiguity has a good answer.
6. `query cpu` equivalents: the CPU plot must track as it does over lines, and `reset cpu` must collapse
   min/max. These go to `/sk/dev/cpu` and `/sk/dev/reset/cpu` rather than through the console.
7. Confirm a destructive address (`/sk/a/pad/clear`) prompts first, and that `/sk/dev/reset/cpu` does
   **not** — it only clears the meter extremes.
8. **The describe bundle is the payload worth watching.** It is ~2 KB in ONE SLIP frame, so it crosses
   many CDC reads. If the control surface renders at all, reassembly worked; if it renders partially,
   it did not. This is the browser-side version of the risk the spec called out as the main one.
9. Unplug while connected, as in C8 step 8. Teardown must behave identically.

### C9 — offline

**Known limitation before you start:** `js/ui/main.js` registers the service worker only when
`location.protocol === 'https:'`, so it does **not** register on `http://localhost`. That keeps a
cache-first worker from serving stale files during development, and it means C9 cannot be run against
`make web-serve` — it needs a real HTTPS deploy. Either deploy to Pages first, or serve `web/` over
local HTTPS with a self-signed certificate.

Then:

1. Load the page, confirm the worker is active in DevTools → Application → Service Workers.
2. Go offline. Reload. The page and every tab must still work.
3. Confirm `card_layout.json` came from the cache and not a stale copy of an older release: the header
   line must still read the current bank count.

### C10 — flashing real firmware (Chrome only)

> **Status, 2026-08-03.** C10a and C10b **pass** on hardware (Daisy + spotykach bootloader, Chrome on
> Linux): the device enumerates and its interface is claimed, images are identified from their banner,
> the bootloader binary and a non-firmware file are both refused, and a real write of `bard`
> (`0.6.1-11-g8871468`, 163 KB) boots and runs. **C10c is OUTSTANDING** — see the warning on it below.
>
> Two findings from that session are now baked into the code and worth not re-discovering:
> - The download loop must re-issue `SET_ADDRESS` before **every** chunk and always write **block 2**.
>   Setting the address once and incrementing the block number is a legal reading of DFuSe, and it
>   does not work here — it produced a device whose first byte did not match what was sent.
> - This bootloader is **not** a full DfuSe implementation: it acknowledges `UPLOAD` and answers with
>   an uninitialised buffer rather than QSPI contents or an honest stall. Read-back verification is
>   therefore gated on a capability probe after the erase, and *unverified* is the normal result.
>   Before that was understood, two good flashes were reported as failures.

The one check on this list whose failure mode is a device that needs recovering rather than a page that
needs reloading, so it is written to be run in an order where nothing is risked until the safe parts
have already passed. `web/test/flash.test.ts` covers the protocol against a scripted device — every
state transition, the poll-timeout wait, the verify mismatch, the cancel path. **None of it proves a
real STM32H750 in the spotykach bootloader accepts the sequence.** That is what this check is for.

Have ready: a released `sk-<engine>-<version>.bin`, `bootloader-spotykach-v2.bin` (as a *negative*
test — it must be refused, not flashed), and `dfu-util` installed as the recovery path.

**C10a — refusals, with no device attached.** Nothing here touches hardware, so do it first.

1. Open the Flash tab with no device connected. The Flash button is disabled and the status line says
   what is missing.
2. Choose `bootloader-spotykach-v2.bin`. It must be **refused** with an ERROR finding naming it as a
   bootloader image. The Flash button stays disabled.
3. Choose any non-firmware file (a `.wav` from the card, say). Refused, reset vector named.
4. Choose a released engine `.bin`. It must be **accepted**, and the finding must name the right engine
   and version — check them against the filename.

**C10b — the real write.** Put the device in bootloader mode: hold Reset ~3s until the pad LEDs breathe
white.

5. Connect device. The picker lists one entry (`0483:df11`); anything else means the filter is wrong.
6. Flash a *small* SRAM engine first (`delay`, ~150 KB) rather than csound at 2.2 MB — a shorter write
   is a shorter window for a first-run bug.
7. Watch the phases: Erasing → Writing → Finishing. **Reading back is expected to be absent** on the
   spotykach bootloader - it does not report memory through `UPLOAD`, so the capability probe after the
   erase disables verification and the result says *unverified*, naming the reason. That is the correct
   outcome, not a fault. If **Reading back** *does* appear and passes, this bootloader is better than
   believed and that is worth recording here.
8. Power-cycle. The engine must boot and its banner must match what the page said it wrote — confirm
   over the Terminal tab, or by ear.

**C10c — the interruption, which is the whole safety argument. OUTSTANDING as of 2026-08-03.** Do this
deliberately, and do it on an engine you are willing to re-flash. Until it passes, the Flash tab's
central claim — that an interrupted write costs a re-flash rather than a device — is reasoned from the
memory map and confirmed by no one.

9. Start a flash of a QSPI engine (`chuck` or `csound` — big enough to leave a window) and hit
   **Cancel** partway through.
10. The page must say the app region is partly written *and* that the bootloader is untouched.
11. Hold Reset ~3s. The device must re-enter DFU. **This is the claim the whole tab rests on** — if the
    device does not come back here, stop, recover with `dfu-util`, and treat the tab as unsafe to ship.
12. Flash again from the page. It must succeed — this also exercises the `dfuERROR` clear path, since
    the previous attempt left the device mid-transfer.

**C10d — the platform notes.** On Linux, confirm the udev rule is needed and that a browser without it
gets a comprehensible failure rather than an empty picker. In Firefox and Safari, the tab must say
WebUSB is unavailable and show the `dfu-util` command, with no enabled buttons.

## Results

| Check | Chrome | Safari | Notes |
|---|---|---|---|
| C1 page loads | | n/a | |
| C2 degrades | n/a | | |
| C3 read a real card | | | |
| C4 drop a folder | | | |
| C5 convert real audio | | | |
| C6 write in place | | n/a | |
| C7 reference | | | |
| C8 terminal (line codec) | | n/a | |
| C8b terminal (OSC codec) | | n/a | needs a second flash, `TERMINAL=1 OSC=1`; added 2026-08-11 |
| C9 offline | | | |
| C10a refusals | PASS 2026-08-03 | | bootloader image and a non-firmware file both refused |
| C10b real write | PASS 2026-08-03 | n/a | bard 0.6.1-11-g8871468; *unverified* by design, boots and runs |
| C10c cancel + recover | **OUTSTANDING** | n/a | **the safety claim — untested** |
| C10d platform notes | | | |

Record failures as issues against `web/`, and add a regression test to `web/test/` for anything the
node suite could have caught but did not — that is the more valuable half of the outcome.
