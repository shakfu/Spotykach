# web/ - browser SD card tools

A static page that builds, fills and checks an SD card for the spotykach engines, plus a WebSerial
terminal for `TERMINAL=1` builds. TypeScript, bundled by [bun](https://bun.sh) into one committed file;
no server, no JavaScript dependencies. Design rationale and the constraints that shaped it are in
[`../docs/dev/web-frontend.md`](../docs/dev/web-frontend.md).

It ships **two themes**, switched from the View menu and remembered in `localStorage`:
**System 6** (the default, via [system.css](https://github.com/sakofchit/system.css)) and **Plain**
(via [water.css](https://watercss.kognise.dev/), light and dark, ordinary system type - the one for
reading the engine manuals). Both are MIT and vendored in `vendor/`.

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

**Reference** and **Terminal** sit apart, to the right, because neither is a step in that job:
Reference is a lookup (`sk_card.py layout` as a screen, and the only tab needing nothing from the
browser), and Terminal needs a firmware build almost nobody has.

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

The suite covers the logic but cannot cover the four browser APIs the app is built on.
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
vendor/system.css/                 VENDORED - system.css + the fonts and button frames it references
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
  platform/   the four browser APIs, and only these files may touch them
    cardsource.ts  File System Access / drag-drop / <input webkitdirectory>
    audio.ts       decodeAudioData -> OfflineAudioContext
    serial.ts      WebSerial
    download.ts    saving a file, and CompressionStream
    clock.ts       setInterval
  app/        one view-model per tab: all the state, none of the DOM
  ui/         the five tabs: render and bind, nothing else
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
editing and the Terminal tab are Chromium-only, and the Terminal tab says so.

## Terminal caveat

`scripts/build_release.py` never passes `TERMINAL=1`, so **no released binary has the command
channel**. The Terminal tab works against a build you make yourself (`make ENGINE=<engine>
TERMINAL=1`); on the QSPI engines (chuck, csound, mosc) that costs USB MIDI, which claims the same OTG
core. Shipping terminal-enabled releases is an open firmware decision - see `TODO.md` P6.

## Not done here

Firmware compilation (needs the ARM toolchain) and DFU flashing (the
[Daisy Web Programmer](https://electro-smith.github.io/Programmer/) already does it, and a half-written
image is the worst failure in the system). Formatting the card is not possible from a browser - format
FAT32 first. Demo audio is not synthesized: the released `sk-card-<version>.zip` is a complete,
checksummed card, and the page links it rather than producing a second unverifiable copy.
