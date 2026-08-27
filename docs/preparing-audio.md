# Preparing audio for the SD card (tape / shuttle)

> **Start at [`docs/sd-card.md`](sd-card.md) if you just want a working card.** `make sdcard` builds > one, `sk_card.py convert` puts your audio on it in the right format, and `make check-sdcard` explains > anything wrong with a card you already have. This page is the tape/shuttle format reference behind > those tools — read it when you want to know *why* the format is what it is, or to drive > `convert_tape_audio.py` directly.

The `tape` and `shuttle` engines read audio straight off the SD card into the audio path with **no format conversion on-device**. They accept exactly one format; anything else is rejected (the deck's amber error LED **strobes** for a wrong-format file vs a steady amber for an empty slot). So source files must be converted on a computer first.

## Supported format

| Property | Value | ffmpeg | sox |
|---|---|---|---|
| Container | WAV (RIFF) | `.wav` | `.wav` |
| Encoding | 32-bit **IEEE float** (`AudioFormat 3`) | `-c:a pcm_f32le` | `-e floating-point -b 32` |
| Channels | **mono** (stereo is down-mixed) | `-ac 1` | `-c 1` |
| Sample rate | **48000 Hz** | `-ar 48000` | `-r 48000` |

Common mistakes that still play as garbage / get rejected: 16-bit PCM, **32-bit *integer*** PCM (`pcm_s32le` — not the same as float; this is the easy one to get wrong), stereo, and 44.1 kHz (no on-device resampling, so it would otherwise play ~8% flat).

## Card layout

8 slots per deck, selected on the device with **Alt+PITCH**:

```text
tape    -> /tapes/tape_<a|b>_<1..8>.wav
shuttle -> /shuttle/tape_<a|b>_<1..8>.wav
```

The shuttle is **RAM-capped at ~30 s per track** — longer files are truncated on load. The tape engine streams from the card, so its files can be any length.

## Recommended: the batch script

[`scripts/convert_tape_audio.py`](../scripts/convert_tape_audio.py) wraps ffmpeg/sox, converts to the exact format, verifies the output header, and can name files into deck slots:

```sh
# Fill tape deck A's slots 1.. (-> out/tapes/tape_a_1.wav, tape_a_2.wav, ...)
scripts/convert_tape_audio.py --engine tape --deck a -o out a.wav b.wav

# Shuttle deck B starting at slot 3; warns on anything over the ~30 s RAM cap
scripts/convert_tape_audio.py --engine shuttle --deck b --start-slot 3 -o card take*.wav

# Just convert, keep original names, into ./out
scripts/convert_tape_audio.py -o out *.wav

# Use sox instead of ffmpeg
scripts/convert_tape_audio.py --tool sox in.wav
```

It exits non-zero and prints `FAIL` if a converted file doesn't match the required format, so it is safe to script. Copy the resulting `tapes/` or `shuttle/` folder to the card root.

## Manual one-liners

If you'd rather not use the script:

**ffmpeg** — single file, then a whole folder:

```sh
ffmpeg -i in.wav -ac 1 -ar 48000 -c:a pcm_f32le tape_a_1.wav

for f in *.wav; do ffmpeg -i "$f" -ac 1 -ar 48000 -c:a pcm_f32le "out/$f"; done
```

**sox** — single file, then a whole folder:

```sh
sox in.wav -c 1 -r 48000 -e floating-point -b 32 tape_a_1.wav

for f in *.wav; do sox "$f" -c 1 -r 48000 -e floating-point -b 32 "out/$f"; done
```

Both tools add a small `fact`/`LIST` metadata chunk before the audio (pushing the `data` chunk to ~offset 58-92). The firmware tolerates header metadata up to 256 bytes, so this is fine — no need to strip it.

## Verify

Check a converted file's header (the four fields that matter):

```sh
ffprobe -hide_banner out/tape_a_1.wav     # want: pcm_f32le, mono, 48000 Hz
# or, raw header bytes — offset 0x14 should read 0300 (float), not 0100 (int):
xxd -l 24 out/tape_a_1.wav
```

The script does this check automatically for every file it converts.
