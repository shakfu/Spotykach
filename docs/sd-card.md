# The SD card

Ten engines read the SD card, and between them they use **eight different folder layouts and four
incompatible audio formats**. The firmware converts nothing on the audio path — it reads file body
bytes straight into audio frames — so a file in the wrong format is not rejected, it is **reinterpreted
as garbage**. Several rules fail silently in ways a person cannot see from a file manager.

You do not have to learn any of this. One command builds a correct card, and another explains anything
wrong with a card you already have.

## Quick start

```sh
# Build a complete card (folders, configs, example patches, demo audio) onto a mounted card
make sdcard SDCARD_OUT=/media/SK

# Or download the prebuilt one: sk-card-<version>.zip ships with every firmware release.
# Unzip it to the root of a FAT32-formatted card.

# Add your own audio, in the right place and format, converted for you
python3 scripts/sk_card.py convert --engine tape /media/SK ~/Music/loop.mp3

# Check a card - yours, or one that is not behaving
make check-sdcard CARD=/media/SK
```

The card must be **FAT32**, up to 32 GB.

## Why a checker exists

The device's only feedback is an LED — a steady amber for an empty slot, a strobing amber for a wrong
format, a red rather than magenta pad on pstretch. That is enough to tell you something is wrong and
nothing about what. Every rule below is a real way to end up with a card that looks right in Finder or
Explorer and does not work:

| What you did | What the device does | Visible? |
|---|---|---|
| 16-bit or 32-bit **integer** WAV where 32-bit **float** is required | reads the bytes as float — plays as noise | no |
| 44.1 kHz instead of 48 kHz | plays ~8% flat, or is rejected | no |
| stereo where mono is required (or vice versa) | wrong-length frames — noise | no |
| filename longer than 12 characters in a scanned folder | file is **skipped entirely** | no |
| file smaller than 32 KB in a scanned folder | file is **skipped entirely** | no |
| copied an `.mp3`/`.flac` across unconverted | never opened | no |
| macOS wrote `._NAME.wav` companions | used to index as garbage "stations" | no |
| wrote `config.txt` as `key=value` | parses as nothing, silently | no |

`sk_card.py verify` predicts all of these by re-implementing the firmware's own checks — the WAV chunk
walk from `src/memory/raw_stream.h` and the directory-scan rules from `src/hw/stream_deck.cpp`.

```
$ make check-sdcard CARD=/media/SK

WILL NOT WORK (2):

  tapes/tape_a_1.wav
    wrong format (encoding is 16-bit PCM, 44100 Hz) - the firmware reads the bytes as-is,
    so this plays as noise or not at all
    -> Needs: WAV, 32-bit IEEE float (WAV AudioFormat 3), MONO, 48000 Hz.
       Fix with: sk_card.py convert --engine tape CARD tape_a_1.wav

  bard/0/The Hobbit Chapter 3.wav
    filename is 24 characters; the scan skips anything over 12, so this file is INVISIBLE
    -> Rename to 12 characters or fewer including the extension (e.g. THEHOBBI.WAV).
```

It exits non-zero if anything will not work, so it can gate a script.

## What each engine expects

`python3 scripts/sk_card.py layout` prints this from the same table the tools use, with firmware
source citations. In summary:

| Engine | Folder | Format |
|---|---|---|
| granular | `SK/{B,G,P,R,T,Y}/{1..6}.WAV` | 48 kHz **stereo**, float *or* 16-bit |
| tape | `tapes/tape_{a,b}_{1..8}.wav` | 48 kHz **mono 32-bit float** |
| shuttle | `shuttle/tape_{a,b}_{1..8}.wav` | as tape, but ~30 s max (loaded into RAM) |
| radio | `radio/{0..15}/*.raw` | **headerless** 16-bit mono, 48 kHz |
| bard | `bard/{0..15}/NAME.WAV` | 16-bit mono; 24 kHz suits speech |
| pstretch | `pstretch/*.wav` | 16-bit mono, any rate |
| csound | `csound/{0..7}.csd` | text |
| chuck | `chuck/{0..7}.ck` | text |

Plus `SK/config.txt` (settings) and `SK/MEM` (written by the device).

**Slot folders vs scanned folders** is the distinction worth internalising. Slot folders
(granular/tape/shuttle/csound/chuck) open *exact filenames*: a file named anything else is simply never
opened. Scanned folders (radio/bard/pstretch) enumerate the directory and apply extra rules — the
12-character limit, the 32 KB floor, `.raw`/`.wav` only, no leading dots — so a file there can be
correctly encoded and still invisible.

Each folder on a generated card contains a `README.TXT` restating its own rules, because that is where
you are standing when the question comes up.

## Adding your own audio

```sh
# tape/shuttle: --deck a|b and --slot pick where it lands
python3 scripts/sk_card.py convert --engine tape --deck b --slot 3 /media/SK drums.wav

# radio: --bank picks the folder (0..15); files are numbered 01.raw, 02.raw, ...
python3 scripts/sk_card.py convert --engine radio --bank 2 /media/SK *.flac

# bard: 24 kHz is half the bytes per hour, and speech never exceeds 12 kHz of bandwidth
python3 scripts/sk_card.py convert --engine bard --bank 0 --rate 24000 /media/SK chapter1.mp3
```

It needs a decoder, and picks one per file: **cysox** (in-process libsox) when that build of libsox
handles the input format, otherwise **ffmpeg**, otherwise the **sox** binary. Install whichever you
prefer — ffmpeg is the safest single choice, since libsox often ships without mp3/flac support
(`libsox-fmt-*` are separate packages on Debian). Force one with `--tool`.

For bulk or specialised jobs the original scripts remain, and do things the front-end does not:

* [`scripts/prepare_audiobooks.py`](../scripts/prepare_audiobooks.py) — audiobooks for `bard`: joins
  per-chapter LibriVox MP3s into one file, derives **bookmarks** from real chapter boundaries, renames
  to 8.3, and writes the `BOOKS.TXT` title map.
* [`scripts/convert_radio_audio.py`](../scripts/convert_radio_audio.py) — mirrors an entire original
  RadioMusic card (its `0..15` folders, headerless 44.1 kHz) into this device's 48 kHz layout.
* [`scripts/convert_tape_audio.py`](../scripts/convert_tape_audio.py) — the detailed tape/shuttle
  converter; see [`preparing-audio.md`](preparing-audio.md) for the format rationale and ffmpeg/sox
  one-liners.

## The base card

`make sdcard` builds a card and packages it as `dist/<version>/sk-card-<version>.zip`, alongside the
firmware binaries, so it ships with every release.

Its demo audio is **synthesized, not sampled** — tones, sweeps, noise beds, rhythmic patterns, and a
formant-ish babble for the bard shelf. That keeps the download small, means every engine makes a sound
on a fresh card, and avoids any question about what is being distributed. It is placeholder content;
replace it with your own.

The build is stdlib-only Python (no ffmpeg, no venv) and byte-reproducible, so the published checksum
is meaningful. `make sdcard SDCARD_DEMO=0` gives the skeleton, configs and READMEs without audio.

## macOS note

macOS writes `._NAME` companion files and `.DS_Store` onto FAT volumes. The firmware filters them
now — an earlier version indexed them as tiny garbage "stations" — but they still waste space and
clutter the card. Run `dot_clean /Volumes/SK` before ejecting, and `verify` will point them out.
