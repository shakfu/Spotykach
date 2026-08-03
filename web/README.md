# web/ - browser SD card tools

A static page that builds, fills and checks an SD card for the spotykach engines, plus a WebSerial
terminal for `TERMINAL=1` builds. TypeScript, bundled by [bun](https://bun.sh) into one committed file;
no server, no JavaScript dependencies. Design rationale and the constraints that shaped it are in
[`../docs/dev/web-frontend.md`](../docs/dev/web-frontend.md).

It ships **three themes**, switched from the View menu and remembered in `localStorage`:
**System 6** (the default, via [system.css](https://github.com/sakofchit/system.css)), **Plain**
(via [water.css](https://watercss.kognise.dev/), ordinary system type on white - the one for reading
the engine manuals) and **Dark** (the same theme on water.css's dark build). Both frameworks are MIT
and vendored in `vendor/`.

Dark is a **choice, not a preference**: the vendored water.css files are its separate `light` and
`dark` builds rather than the `auto` one, which follows `prefers-color-scheme`. With auto, Plain
changed appearance on its own - the plain, white, for-reading theme came up on a dark slate ground
for anyone whose system was in dark mode, which is the opposite of what it is for. Now the reader
picks, in the View menu, and it is remembered. `themes/dark.css` is Plain's skin plus a palette: it
`@import`s `plain.css` and overrides the four things that are actually colour, because a theme is two
`<link>`s and there is no third slot for a shared layer.

A theme is two files: the vendored framework and a skin in `themes/`. Everything structural lives in
`app.css` and is written against tokens, so a theme is a palette plus the places a framework's
defaults have to be overridden - not a second copy of the app. The choice is applied by five inline
lines in the `<head>`, because it has to be settled before the first paint or every load flashes the
wrong theme; a test asserts that copy agrees with `src/ui/theme.ts`.

The System 6 theme is the reason severity is not carried by colour. That is a deliberate choice of character for a hobbyist tool, and it costs one
thing worth knowing about: system.css is genuinely 1-bit, so there is no dark mode and **severity is
not carried by colour**. Every finding states its level as a word (ERROR / WARNING / OK) in the markup,
errors invert to white-on-black, and the rule down the side varies in weight. That is more robust than
the red/amber/green it replaced, which said nothing to a colour-blind reader that the group heading
had not already said.

The tabs run **Build, Convert, Verify** — the order a person needs them, not the order of their value.
Verify is the most valuable screen and the wrong first one: the entry state for someone who just bought
a device is "I have no card yet", and all Verify can say to that is "this is not a card". Build hands
back a complete, valid, minimal card in one click.

**Reference**, **Terminal** and **Flash** sit apart, to the right, because none is a step in that job:
Reference is a lookup (`sk_card.py layout` as a screen, and the only tab needing nothing from the
browser), Terminal needs a firmware build almost nobody has, and Flash is about the device rather than
the card.

Each tab opens with its controls. The reasoning behind a rule lives in a folded aside beneath them,
because every rule here has a reason worth keeping and none of them is worth reading before you can
press a button.

## Running it

```
make web-serve          # builds, then http://localhost:8000
make web-build          # rebuild dist/app.js after editing src/
make test-web           # typecheck (src strict, tests relaxed) + the suite
make web-data           # regenerate card_layout.json, patches.json and the test fixtures
```

`dist/app.js` is **generated and committed**: GitHub Pages serves `web/` as-is, so a fresh checkout can
open the page with no toolchain. The cost is that it can be committed stale, so a test fails when it is
older than `src/`. Edit `src/`, never `dist/`.

Opening `index.html` from the filesystem does **not** work: ES modules will not load over `file://`,
and the browser APIs the page uses are only offered over HTTPS or `localhost`.

The suite covers the logic but cannot cover the browser APIs the app is built on.
[`../docs/dev/web-frontend-checks.md`](../docs/dev/web-frontend-checks.md) is the mechanical pass that
does — about half an hour with a card, a Daisy and two browsers.

## The one rule

**Nothing about the card layout is declared here.** `scripts/card_layout.py` is the single source of
truth, and `make web-data` exports it to `card_layout.json`, which this app reads as data - including
the generated text (per-folder READMEs, the root README, the default config), so the browser writes a
card byte-identical to `sk_card.py init` without owning a line of the wording.

Two things genuinely are reimplemented here, because they are code rather than content:

| Piece | Python original | Pinned by |
|---|---|---|
| WAV writer / parser, raw writer | `scripts/card_audio.py` | byte-equality against fixtures in `test/fixtures/` |
| the `verify` walk | `scripts/sk_card.py` | same verdicts as `verify_card` on a deliberately-broken card |

Both are enforced by tests on both sides. `make test-scripts` fails if the committed export has drifted
from `scripts/`; `make test-web` fails if this side disagrees with the Python.

## Layout

```
index.html  app.css  sw.js         the page, its shared styles, and offline caching
themes/system6.css  plain.css      one skin per theme: palette, chrome, severity reinforcement
       dark.css                    ... and dark.css is plain.css @imported, plus a palette
vendor/system.css/                 VENDORED - system.css + the fonts and button frames it references
vendor/water.css/                  VENDORED - the light and dark builds; see the README.txt beside them
card_layout.json  patches.json     GENERATED - do not edit, run `make web-data`
engines.json                       GENERATED from docs/engines/*.md - the Engines menu + Reference
engines/<name>.html                GENERATED - each engine's manual, rendered from its markdown
engines/media/                     GENERATED - copies of the control diagrams those manuals show
dist/app.js                        GENERATED - do not edit, run `make web-build`
src/
  core/       the rules, and nothing else. No DOM, no browser API, no state.
    types.ts       what card_layout.json declares, given names
    ports.ts       what the core needs from outside, as interfaces
    layout.ts      card_layout.json, wrapped
    wav.ts         WAV/raw read+write, mirroring card_audio.py
    verify.ts      the checker, mirroring sk_card.verify_card
    build.ts       the card skeleton, assembled from the exported text
    convert.ts     encoding for a bank (decoding is a port)
    zip.ts         a dependency-free ZIP writer (compression is a port)
    protocol.ts    line framing + the describe model
    device.ts      the command API, over any transport
    image.ts       what a firmware .bin is, from its bytes - and what must not be flashed
    dfu.ts         the DFU 1.1 + DFuSe download sequence, with no USB API in it
  platform/   the five browser APIs, and only these files may touch them
    cardsource.ts  File System Access / drag-drop / <input webkitdirectory>
    audio.ts       decodeAudioData -> OfflineAudioContext
    serial.ts      WebSerial
    download.ts    saving a file, and CompressionStream
    clock.ts       setInterval
    usb.ts         WebUSB, for DFU
  app/        one view-model per tab: all the state, none of the DOM
  ui/         one file per tab: render and bind, nothing else
test/                                bun runs the .ts directly; fixtures GENERATED
```

**Dependencies point inwards**: `ui -> app -> core` and `platform -> core`, never back out. That is
what makes `app/` testable with no DOM at all - every browser capability enters through an interface in
`core/ports.ts`, and the tests implement those with twenty-line fakes. Three tests in
`test/offline.test.ts` enforce the rule, because it is the kind of claim that rots silently: one
convenient `document.` in a view-model and the models need a browser again.

## Browser support

The card tabs work everywhere. Where the File System Access API is missing (Firefox, Safari) the app
falls back to drag-and-drop in and a `.zip` out - the same bytes, one extra step. Only in-place card
editing, the Terminal tab and the Flash tab are Chromium-only (the last needs WebUSB), and both of
those tabs say so rather than offering a button that fails.

## Terminal caveat

`scripts/build_release.py` never passes `TERMINAL=1`, so **no released binary has the command
channel**. The Terminal tab works against a build you make yourself (`make ENGINE=<engine>
TERMINAL=1`); on the QSPI engines (chuck, csound, mosc) that costs USB MIDI, which claims the same OTG
core. Shipping terminal-enabled releases is an open firmware decision - see `TODO.md` P6.

## Flashing

The **Flash** tab writes an engine image to the device over WebUSB, and the reason it exists after this
README spent a version saying it would not is worth stating, because the original objection was right
about the general case and wrong about this one.

The objection was that a half-written image is the worst failure in the system. That is true of a
flasher that can write anywhere. This one writes **one address**: the application region at
`0x90040000`, in QSPI. The bootloader lives in internal flash at `0x08000000`, and nothing in this app
can name that address - `APP_ADDRESS` is a constant and `assertTarget()` rejects everything else before
a single USB transfer. So the worst case is a device with a corrupt *app* and a working *bootloader*,
which still enters DFU on a 3-second Reset hold and can simply be flashed again. That is a retry, not a
brick, and the page says so on the progress bar rather than in a footnote.

Three things it does that a generic flasher cannot, all of them because a spotykach image identifies
itself in its own bytes (`core/image.ts`):

| Check | How | Why it matters |
|---|---|---|
| names what you are about to install | the linked `spotykach <version> engine=<name>` banner | you confirm "delay 0.6.1", not a filename |
| refuses a bootloader image | its reset vector is in internal flash (`0x08......`) | `bootloader-spotykach-v2.bin` is in the repo root; writing it to the app address is a valid DFU transaction with a useless result |
| refuses anything that is not firmware | reset vector in neither SRAM (`0x24......`) nor QSPI (`0x90......`) | a wrong file is caught before the device is opened |

After writing, the image is read back over DFU `UPLOAD` and compared byte for byte **where the device
supports it** - which the spotykach bootloader does not. It is not a full DfuSe implementation: it
acknowledges `UPLOAD` and answers with an uninitialised buffer rather than QSPI contents or an honest
stall. Comparing against that reported two good flashes of real hardware as *"the flash did not take"*
before it was understood.

So the read-back is gated on a **capability probe** run immediately after the erase, which is the one
moment the right answer is known: an erased region must read as `0xFF`, and anything else means
`UPLOAD` is not reporting memory. A device that fails the probe is flashed anyway and the result says
*unverified*, naming the reason. On current hardware that is the normal outcome, and the confirmation
that a flash worked is that the engine boots.

**Installing a bootloader is not offered and will not be.** That is the write whose failure genuinely
bricks a unit, it is a once-per-device procedure, and `dfu-util` does it.

WebUSB is Chromium-only, like the Terminal tab. The command-line path works everywhere:

```
dfu-util -a 0 -s 0x90040000:leave -D sk-<engine>-<version>.bin -d ,0483:df11
```

## Not done here

Firmware compilation (needs the ARM toolchain) and **bootloader installation**. Formatting the card is
not possible from a browser - format FAT32 first. Demo audio is not synthesized: the released `sk-card-<version>.zip` is a complete,
checksummed card, and the page links it rather than producing a second unverifiable copy.
