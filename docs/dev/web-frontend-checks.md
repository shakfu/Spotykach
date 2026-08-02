# Web front-end: the browser pass

The `web/` suite runs in bun and covers the logic. It cannot cover the four browser APIs the app is
built on — File System Access, `decodeAudioData`, WebSerial, service workers — because a DOM shim can
only assert that the code *calls* them. This is the mechanical run that closes that gap: about 30
minutes with a card, a Daisy and both browsers.

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
- Optional, for C8 only: a Daisy flashed with `make ENGINE=<engine> TERMINAL=1`.

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

### C9 — offline

**Known limitation before you start:** `js/ui/main.js` registers the service worker only when
`location.protocol === 'https:'`, so it does **not** register on `http://localhost`. That keeps a
cache-first worker from serving stale files during development, and it means C9 cannot be run against
`make web-serve` — it needs a real HTTPS deploy. Either deploy to Pages first, or serve `web/` over
local HTTPS with a self-signed certificate.

Then:

1. Load the page, confirm the worker is active in DevTools → Application → Service Workers.
2. Go offline. Reload. The page and all five tabs must still work.
3. Confirm `card_layout.json` came from the cache and not a stale copy of an older release: the header
   line must still read the current bank count.

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
| C8 terminal | | n/a | |
| C9 offline | | | |

Record failures as issues against `web/`, and add a regression test to `web/test/` for anything the
node suite could have caught but did not — that is the more valuable half of the outcome.
