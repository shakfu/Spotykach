# radio engine

`ENGINE=radio` · `src/engine/radio/radio_engine.{h,cpp}` · class `RadioEngine`

A **dual virtual [RadioMusic](https://github.com/TomWhitwell/RadioMusic)**. Each deck (A/B) is an **independent virtual radio** browsing one shared SD library of banks; the two radios are blended by the platform crossfader and placed by the routing switch, so it plays like a pair of radios you tune and mix.

Its signature is the **free-running virtual playhead**: every station sounds as though it kept broadcasting while you were tuned elsewhere. A per-deck clock runs continuously; tuning to a station lands at the position it "would" be playing now, and leaving and returning finds it advanced - exactly like a radio.

> Implementation, architecture, the file map, and the bug writeups live in > [`docs/dev/radio-impl.md`](../dev/radio-impl.md).

---

## Controls (per deck)

![Radio control surface](../media/radio-controls.svg)

*Generated from [`docs/diagrams/controls/radio.json`](../diagrams/controls/radio.json) via `make diagrams`.*

| Control | `ParamId` / config | Effect |
|---|---|---|
| **PITCH** (`Speed`) | + V/oct CV jack | **STATION** select - the tuning dial. Knob + CV summed, quantized to the nearest station in the bank. |
| **POS** (`Pos`) | + size/pos CV jack | **START** offset into the station (applied on the next switch / RESET). |
| **SIZE** (`Size`) | | **varispeed** 0.5x..2x (center = unity). RadioMusic is fixed-rate; this is a platform bonus. |
| **ENV** (`Env`) | | inter-station **STATIC** amount (tuning hiss crossfaded in on a station change). |
| **MIX** (`Mix`) | | deck **volume**. |
| **Alt+PITCH** (`Aux`) | | **BANK** select (held selector, ring dots) - "hold RESET for bank" on the original. |
| **Mix fader** (`Crossfade`) | | A/B blend of the two radios. |
| **Routing switch** (`Route`) | | stereo topology (below). |
| **Play pad** / **gate-in** | | **RESET** - re-start a stopped deck at the live position (a no-op while it is already streaming). The Rev pad is inert. |

### Routing / stereo image

- **LEFT (DoubleMono):** radio A hard-left, radio B hard-right (two radios across the stereo field).

- **CENTRE (Stereo):** both radios centred.

- **RIGHT (GenerativeStereo):** each radio at a random pan (re-rolled on entering the mode).

### Static (tuning hiss)

A filtered-noise "tuning hiss" crossfades in on each station change; **ENV** sets the level. With ENV high, sweeping PITCH switches stations immediately so you hear the dial-tuning churn + hiss; with ENV low, a sweep only opens the station you land on (no zipper). A non-playing deck (empty bank) outputs dead-air hiss at the ENV level.

### Display

Per deck the ring shows the **station position** (a bright marker at `station/N`) over a faint base, green while playing, amber on a failed open / empty bank. Holding **Alt** shows the **BANK** selector (16 dots, the current bank bright). The routing switch lights the mode L/C/R indicator.

---

## SD-card layout

You can download the original 8GB audio from the original radio music module from [here](https://github.com/TomWhitwell/RadioMusic/wiki/Audio-for-the-SD-Card). Place the files in the spotykach sd card as follows:

A shared library both decks browse independently:

```text
/radio/0/   01.raw 02.raw ...      (bank 0)
/radio/1/   ...                    (bank 1)
 ...
/radio/15/  ...                    (bank 15, up to 48 stations each)
```

- **Banks** = numbered folders `0`..`15` (mirrors RadioMusic's 16 folders).

- **Stations** = `.raw`/`.wav` files in a bank. Use short **8.3 names** (e.g. `01.raw`); longer names are skipped.

- **macOS users:** copying files to a FAT card in Finder leaves hidden `.DS_Store` and `._*` companion files; the engine filters them, but to clean a card you can run `dot_clean /Volumes/CARD` and delete any remaining `._*` / `.DS_Store`.

### Audio format

Stations are **signed 16-bit mono PCM**, in either format (mix freely in a bank):

- **`.raw`** - headerless (the original RadioMusic format). Carries no sample rate, so it assumes 48 kHz unless `rate.txt` overrides (below).

- **`.wav`** - a 16-bit-mono PCM WAV. Self-describing: it carries **its own sample rate**, so a 44.1k WAV plays at correct pitch with **no `rate.txt`**.

The firmware does no sample conversion, so a non-16-bit / stereo / float file is rejected (`.wav`) or plays as garbage (`.raw`). Stations can be arbitrarily long (the originals run ~25 min / 130 MB each) - streaming has no length cap.

Convert sources with [`scripts/convert_radio_audio.py`](../../scripts/convert_radio_audio.py). The original RadioMusic library is itself headerless 16-bit-mono `.raw` at **44.1 kHz**, so the converter resamples it to 48 kHz; `--format wav` writes self-describing WAV stations instead. The whole-card `mirror` command reproduces a stock card under `/radio`, preserving names:

```text
# mirror a RadioMusic card's 0..15 folders into /radio/0..15 at 48 kHz, keeping names
scripts/convert_radio_audio.py mirror --keep-names -o /Volumes/SD ~/Downloads/16gb-Disk

# ...as self-describing WAV (no rate.txt needed)
scripts/convert_radio_audio.py mirror --format wav --keep-names -o /Volumes/SD ~/Downloads/16gb-Disk

# one bank from a folder of your own files
scripts/convert_radio_audio.py from-dir --bank 0 -o /Volumes/SD ./samples

# or a one-liner per file
ffmpeg -i in.wav -ac 1 -ar 48000 -f s16le /Volumes/SD/radio/0/01.raw
```

### Source sample rate (`rate.txt`, `.raw` only)

A headerless `.raw` carries no sample rate, so the engine assumes **48 kHz**. To play an **unconverted original card** (44.1 kHz) at correct pitch without converting, drop a one-line text file **`/radio/rate.txt`** with the source rate:

```text
44100
```

Any rate in 8000..192000 is accepted (RadioMusic's 11025/22050/44100/48000/96000 all work). It rebases playback so SIZE-at-noon gives correct pitch. `rate.txt` applies to `.raw` only - a `.wav` carries its own rate. Without conversion *or* `rate.txt`, an original 44.1 kHz card plays ~9% sharp (also pullable by ear with SIZE).

So three ways to play an original 44.1 kHz library at correct pitch: **convert to 48 kHz `.raw`** with `mirror`, **convert to `.wav`** (`--format wav`, no `rate.txt`), or **drop the `.raw` files in as-is and add `rate.txt`**.

---

## Build / flash

```text
make -j8 ENGINE=radio        # build (~83% SRAM_EXEC)
make engine-radio            # clean + build + DFU flash
make -C host test            # host suites incl. test-radio
```
