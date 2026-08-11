# web/ - the sk-engines browser front end

A static page: what sk-engines is, a browsable catalogue of its engines, SD card tools, and a WebSerial
terminal for `TERMINAL=1` builds — in either codec, line-ASCII or `OSC=1`. TypeScript bundled by [bun](https://bun.sh) and CSS built by
[Tailwind](https://tailwindcss.com); both are build-time only, and both artifacts are committed, so
there is no server and nothing to install to *serve* the page. **Re**building it is another matter:
`make web-build` needs the dev dependencies, so run `bun install` in this directory first — without it
the CSS step fails with a bare `tailwindcss: command not found`. Design rationale and the constraints
that shaped it are in [`../docs/dev/web-frontend.md`](../docs/dev/web-frontend.md).

It ships **two themes**, switched from the View menu and remembered in `localStorage`: **Light**
(the default) and **Dark**. A theme is one attribute - `data-theme` on `<html>` - against one built
stylesheet whose colours are custom properties, so the dark theme is nine values in
`src/app.css` and not a second copy of the app.

Dark is a **choice, not a preference**: `prefers-color-scheme` is deliberately not consulted. When it
was, the light theme - the one meant for reading the engine manuals - came up on a dark ground for
anyone whose system was in dark mode, which is the opposite of what it is for. The reader picks, and
it is remembered. The choice is applied by three inline lines in the `<head>`, because it has to be
settled before the first paint or a reader who chose Dark gets a white flash on every load; a test
asserts that copy agrees with `src/ui/theme.ts`.

**Severity is never carried by colour alone.** This began as a constraint - the original theme was
[system.css](https://github.com/sakofchit/system.css), genuinely 1-bit, with no red to reach for - and
it is kept now that colour is available, because it was the better design regardless. Every finding
states its level as a word (ERROR / WARNING / OK) in the markup, and the rule down its side varies in
weight and style: heavy solid for an error, dotted for a warning, hairline for ok. Colour is a fourth
channel on top of those three, not a replacement for them. Drop the word or the weight and the page
stops working for a colour-blind reader, which is exactly the state it was rebuilt out of.

**Utilities in static markup, component classes in generated markup.** `index.html` carries Tailwind
utilities directly. Anything emitted from `src/ui/*.ts` - a finding, a console line, a table row,
written in a loop inside a template literal - keeps a semantic class name defined in
`src/app.css` with `@apply`. Inlining utilities there would bury the styling inside string
concatenation where it cannot be read, diffed or reused, and would repeat it per iteration.

## Navigation

There was a tab row, and it was the right shape while this page was one tool with six screens. It
cannot express the split the page has now — **global** actions that operate on a card or a device, and
**per-engine** actions that only mean anything once an engine is known. A tablist has one dimension.

So: the **menu bar** carries the global actions, grouped by what they act on — **SD Card** (build,
convert, verify, reference) and **Device** (flash, terminal). The **engine page** carries the
per-engine ones. And the **front page** repeats the common global actions as buttons, because a menu
is where an action lives once you know the tool and a front page is where it is discoverable before
you do. Both routes call the same navigation function, so there is one implementation and no second
path to drift.

`src/ui/main.ts`'s `VIEWS` table is the single source for the routes *and* the menus: a menu is
generated from the same table that resolves the route, so a menu item cannot name a view that does not
exist and a view cannot quietly become unreachable. A test asserts the table and the panels agree.

**The front page** answers, in order: what is this, what is in it, what can I do. Its figures are
derived from the catalogue, never typed — an engine count in prose is exactly the thing that goes
stale the first time an engine is added.

**The engines page** is a card per engine: name, whether it needs an SD card at all, and a line of
description. That last one is not free — only a minority of engines have a `summary` in `engines.json`,
because it comes from an em-dash heading most docs do not write. The rest fall back to the opening of
their manual, trimmed to a sentence in `engines_view.ts` rather than at the generator, so
`web_export.py` stays a faithful extractor and the decision about how much fits on a card stays with
the thing drawing the card.

**Engine pages** offer Flash for every engine, plus Convert and its card layout for the ones that read
the card. The rest get Flash and nothing else — they synthesise rather than play back, and a disabled
"Convert audio" would be a dead affordance, which is what the menu bar was pruned of once already.

Each screen opens with its controls. The reasoning behind a rule lives in a folded aside beneath them,
because every rule here has a reason worth keeping and none of them is worth reading before you can
press a button.

## Running it

```
make web-serve          # builds, then http://localhost:8000
make web-build          # rebuild dist/app.js and dist/app.css after editing src/
make test-web           # typecheck (src strict, tests relaxed) + the suite
make web-data           # regenerate card_layout.json, patches.json and the test fixtures
```

`dist/app.js` and `dist/app.css` are **generated and committed**: GitHub Pages serves `web/` as-is, so
a fresh checkout can open the page with no toolchain. The cost is that either can be committed stale,
so a test fails when one is older than `src/`. Edit `src/`, never `dist/`.

The CSS build is the newer half of that bargain and worth stating plainly: it was added when the two
vendored CSS frameworks were dropped for Tailwind. Before, the stylesheets were served as-is and only
the JavaScript had a build step. Serving and deploying still need no toolchain; only *editing* does.

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
index.html  sw.js                  the page and its offline caching
card_layout.json  patches.json     GENERATED - do not edit, run `make web-data`
engines.json                       GENERATED from docs/engines/*.md - the Engines menu + Reference
engines/<name>.html                GENERATED - each engine's manual, rendered from its markdown
engines/media/                     GENERATED - copies of the control diagrams those manuals show
dist/app.js  dist/app.css          GENERATED - do not edit, run `make web-build`
src/
  app.css         the Tailwind source: @theme palette, then the component layer
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
    osc.ts         SLIP framing + the OSC 1.0 wire format, pure
    oscdevice.ts   the same command API over OSC, for TERMINAL=1 OSC=1 builds
    client.ts      one device surface over either codec, so the UI need not choose
    image.ts       what a firmware .bin is, from its bytes - and what must not be flashed
    dfu.ts         the DFU 1.1 + DFuSe download sequence, with no USB API in it
  platform/   the five browser APIs, and only these files may touch them
    cardsource.ts  File System Access / drag-drop / <input webkitdirectory>
    audio.ts       decodeAudioData -> OfflineAudioContext
    serial.ts      WebSerial - lines, and SLIP frames for the OSC codec
    download.ts    saving a file, and CompressionStream
    clock.ts       setInterval
    usb.ts         WebUSB, for DFU
  app/        one view-model per view: all the state, none of the DOM
  ui/         one file per view: render and bind, nothing else
              main.ts holds VIEWS - the one table the routes and the menus share
test/                                bun runs the .ts directly; fixtures GENERATED
```

**Dependencies point inwards**: `ui -> app -> core` and `platform -> core`, never back out. That is
what makes `app/` testable with no DOM at all - every browser capability enters through an interface in
`core/ports.ts`, and the tests implement those with twenty-line fakes. Three tests in
`test/offline.test.ts` enforce the rule, because it is the kind of claim that rots silently: one
convenient `document.` in a view-model and the models need a browser again.

## Browser support

The card screens work everywhere. Where the File System Access API is missing (Firefox, Safari) the app
falls back to drag-and-drop in and a `.zip` out - the same bytes, one extra step. Only in-place card
editing, the Terminal and Flash screens are Chromium-only (the last needs WebUSB), and both of
those say so rather than offering a button that fails.

## Terminal caveat

`scripts/build_release.py` never passes `TERMINAL=1`, so **no released binary has the command
channel**. The Terminal screen works against a build you make yourself (`make ENGINE=<engine>
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
