# bard engine

`ENGINE=bard` - `src/engine/bard/bard_engine.{h,cpp}` - class `BardEngine`

> **Status: complete in code, host-tested, and confirmed working on hardware.** Every phase of the plan is implemented, including the WSOLA pitch-keep that decision #3 deferred and the Grit room: `make -C host test-bard` runs 144 checks green and `make test-scripts` covers the card-prep companion. The firmware links at **62.8% SRAM_EXEC** (167,080 B of 260 KB, built `-Os`, as `reso`/`reverb` are). What remains is the *measured* pass the other engines are also waiting on (TODO P2): no `Meter::cpu` number has been taken, and the feel constants have not been tuned by ear. The one open question is unchanged and is a measurement: [the jump rate](#still-open). Implementation notes, the file map and the risks live in [`docs/dev/bard-impl.md`](../dev/bard-impl.md).

---

## What it is

A **storyteller**. Each deck (A/B) reads a **spoken-word recording** - an audiobook, a lecture, a field recording, a radio play - from a shelf on the SD card, and navigates it by **bookmarks**: a list of timestamps in a plain-text sidecar file next to the audio. One knob jumps from **book to book**, another jumps from **bookmark to bookmark**, and the reading itself can be sped up, slowed down, pitch-shifted or pitch-preserved, coloured, and placed in a room.

The name is the design brief. A bard recites from a fixed repertoire in an order they choose, at a pace they choose, and the recitation is never quite the same twice. The bookmark file **is** the repertoire and the order; the panel is the pace and the colour. Where the [radio](radio.md) engine is about surrendering control to something that plays on without you, bard is about **imposing an order on long-form material** - either the author's order, an order you wrote down, or an order a clock is imposing on you.

## Why this is not just the radio engine with different files

bard borrows radio's whole *structure* (SD streaming, numbered folders, a bank scan, a quantized selector knob, `.raw`/`.wav` 16-bit mono) and **inverts its defining behaviour**. That inversion is the design, so it is worth stating explicitly:

| | [radio](radio.md) | bard |
|---|---|---|
| Playhead | **free-running virtual** - a station plays on whether you are listening or not; you cannot hold a position | **owned** - a position is a place you can return to, leave, and resume |
| Position control | START offset, applied only on the next switch (bounded SD I/O) | bookmark jump + fine scrub, both immediate and repeatable |
| Play pad | RESET (re-tune to the live position); **no pause exists** - a radio cannot pause | **PLAY / PAUSE**; a book must be pausable |
| Rev pad | inert | **JUMP BACK** ("what did they just say?") |
| Determinism | none, by design - that is the instrument | **the point** - the sidecar file is a written-down order |
| Content | music beds, noise, anything | speech, mostly; long-form, narrative, semantically dense |
| Two decks | two radios you mix - naturally musical | **two simultaneous narrators are unintelligible** - see [Two decks](#two-decks-the-real-design-problem) |

So bard is not a re-skin. It reuses radio's plumbing and rejects radio's premise. The parts that are actually shared - `StreamDeck`, `RawStreamReader`, `scan_bank`, `bank_sort`, the anti-stutter guards - are listed under [Feasibility](#feasibility).

---

## The card: books and their sidecars

### Layout

```text
/bard/      BARD.CFG                     (optional: resume=on|off, rate=<hz>)
            RESUME.TXT                   (written by the engine: last position per book)
/bard/0/    HOBBIT1.WAV  HOBBIT1.TXT     (shelf 0)
            HOBBIT2.WAV  HOBBIT2.TXT
            DUNE.WAV                     (no sidecar -> auto-marks)
            BOOKS.TXT                    (human-readable 8.3 -> real title map; firmware ignores it)
/bard/1/    ...                          (shelf 1)
 ...
/bard/15/   ...                          (shelf 15, up to 32 books each)
```

- **Shelves** = numbered folders `0`..`15`, exactly as radio's banks. Selected by **Alt+PITCH**.

- **Books** = `.wav`/`.raw` files in a shelf, sorted alphabetically (`bank_sort`), so the BOOK knob's position N is always the Nth book by name. Zero-pad numeric names.

- **Sidecars** = `NAME.TXT` next to `NAME.WAV`. Optional. Ignored by the audio scan (it filters to `.raw`/`.wav`), read separately when the book is opened. **Tap-hold + Play** writes one back, so marks dropped live can be kept - at the cost of any labels in the file (below).

- **8.3 names are mandatory.** `src/hw/stream_deck.cpp:scan_bank` skips any name longer than 12 characters, so `The Hobbit - Chapter 3.wav` is invisible to the firmware. This is a real ergonomic cost for audiobooks, whose filenames are always long, and it is why the proposed prep script writes a `BOOKS.TXT` title map: the firmware cannot display text on a 32-LED ring anyway, so the mapping is for the human at the card reader, not for the device.

### Audio format - and why 24 kHz is the right choice here

Same codec as radio: **signed 16-bit mono PCM**, `.wav` (self-describing, rate read from the header) or `.raw` (headerless, assumed 48 kHz unless the `rate=` key in `/bard/bard.cfg` overrides it - see [Resume](#resume---persisted-and-switchable-off)).

The recommendation differs from radio's, though. Speech needs nothing like 48 kHz, and books are long:

| Format | Bytes/hour | 32 GB card | Notes |
|---|---|---|---|
| 48 kHz 16-bit mono | 345 MB | ~92 h | radio's default; overkill for speech |
| **24 kHz 16-bit mono `.wav`** | **173 MB** | **~185 h** | 12 kHz audio bandwidth - ample for voice; halves SD bandwidth per deck |
| 22.05 kHz 16-bit mono `.wav` | 159 MB | ~200 h | same idea, if the source is 44.1 kHz |

A `.wav` carries its own rate and the existing playback path already folds it into the resampler stride (radio's `_deck_rate_ratio`), so a 24 kHz book plays at correct pitch with no configuration. The cost is that the 2-frame linear-interpolating resampler is now upsampling 2x, which is slightly soft and less alias-free than at 48 kHz; for speech this is very unlikely to matter, but it is an unverified claim about audible quality, not a measurement.

**Per-file length cap:** FAT32 tops out at 4 GB per file, so a single file holds about **12.4 h** at 48 kHz or **24.8 h** at 24 kHz (16-bit mono). Longer works must be split into parts.

**Parts are independent books - a sidecar never spans files.** `HOBBIT1.WAV`/`HOBBIT2.WAV` are two adjacent books with two sidecars, and the BOOK knob steps between them. This is the deliberately simple answer: a spanning sidecar (`part=2 of=...`) would drag in cross-file mark numbering, cross-file segment ends, a virtual concatenated timeline, and a resume entry that means "part 2 at 03:14" rather than "this file at 03:14" - a lot of machinery for an ergonomic gain the BOOK knob mostly already provides. The cost of the simple rule is honest and small: an auto-advance at the end of part 1 stops rather than rolling into part 2, and a segment cannot straddle a part boundary. Because resume is per book, returning to a multi-part work still lands in the right part at the right position.

### The bookmark sidecar

A plain text file, one bookmark per line, human-authored or generated. The grammar is deliberately tiny so the parser can be a hand-rolled integer scan on the main loop - no `strtod`, no allocation:

```text
#!bard order=file loop=off
# Any line starting with '#' is a comment (the '#!bard' directive line is parsed).
# TIME [ - TIME ] [ whitespace LABEL ]

0:00              Prologue
14:32             Chapter 1
1:02:11           Chapter 2
1:02:11-1:04:00   the bit about the box
2841              a bare integer is seconds
```

- **TIME** is `[[HH:]MM:]SS[.mmm]`, or a bare integer meaning seconds. Anything unparseable is skipped with the line, not treated as fatal.

- **An explicit end** (`start-end`) defines a closed segment. Without one, the segment runs to the next **chronologically later** bookmark in the file, or to the end of the book. Defining it chronologically rather than by line order is what lets a *scrambled* list still describe well-defined segments.

- **Line order is the play order.** This is the mechanism the whole engine exists for: a sidecar whose lines are not in time order is a re-ordering of the book, and `Wander` mode (below) walks it in that order. Determinism lives in the text file, authored by a human, which is why no panel control duplicates it.

- **LABEL is parsed and discarded** by the firmware. There is no text display - just two 32-LED rings - so there is nowhere for a label to go; the device only ever knows a mark as a position in frames. Labels exist for whoever edits the file and for the offline tools, and being honest about that keeps the parser trivial. They matter most if you ever **reorder** the lines, since line order is the play order and you can only author that meaningfully if you can read what you are scrambling.

  **Consequence: committing marks from the device erases the labels.** Tap-hold + Play rewrites the sidecar from what the engine holds in memory, which is timestamps only, so any titles you hand-wrote are replaced by nothing. Keep a copy before using the on-device commit, or re-label afterwards. Retaining labels purely to round-trip them would cost ~2.5 KB of SRAM for 64 of them plus parser complexity, for text the module can never display - so this is a deliberate trade, not an oversight.

  **One syntax hazard when hand-editing:** do not start a label with a hyphen followed by something time-like. `0:01:00.000  - 2:00 intro` parses `2:00` as an explicit segment *end*, because `-` is the range separator. A hyphen anywhere else in a label is safe - `The Wolf - and the Kid` and `Chapter 1 - An Unexpected Party` both read correctly, and `host/test_bard.cpp` pins that, since embedded `.m4b` chapter titles routinely take the second form.

- **Directives** (`#!bard` on the first line): `order=file|time|shuffle` and `loop=off|segment|book`.
  Two directives, no more - anything richer belongs in the file's line order, not in a config language.

- **Limits:** up to **64 bookmarks** per book (64 x 12 bytes of state per deck) and a **4 KB** sidecar. The 4 KB cap is not arbitrary: `IStreamDeck::read_text` reads only the first `max-1` bytes and silently truncates, so either the cap is documented and enforced by the prep script, or the contract needs a chunked text read. Documenting the cap is the cheaper answer.

### When there is no sidecar

The book still gets bookmarks - generated, not read. Proposed: `n = clamp(duration_minutes / 5, 4, 32)` marks, evenly spaced and jittered, from a small LCG **seeded by a hash of the filename and frame count**.

Seeding from the file rather than from the clock is the important part. It makes the marks **deterministic per book**: the same book has the same marks on every boot, so they can be learned and performed, which is exactly the "little determinism" the sidecar provides for books that have one. A clock-seeded roll would make every session different and the marks unlearnable. **Alt+Rev** re-rolls the seed for a deck when a fresh scatter is wanted, and the roll is session-only.

Mark placement belongs **offline**, and that is [`scripts/prepare_audiobooks.py`](../../scripts/prepare_audiobooks.py), modelled on [`scripts/convert_radio_audio.py`](../../scripts/convert_radio_audio.py). Detecting boundaries in firmware would mean reading the whole book off the card before playing a note.

It is written against the two libraries that actually matter, because **both already carry exact chapter boundaries** - which makes silence detection the last resort rather than the first:

| Source | What you download | How marks are derived |
|---|---|---|
| [LibriVox](https://librivox.org) | **one 64 kbps MP3 per chapter** (dozens of files) | `--join` concatenates them into ONE book with a mark at **every join** - exact, because each boundary is a file boundary |
| [LoyalBooks](http://www.loyalbooks.com) | a single **`.m4b`** | its **embedded chapter list** is read straight out of the container, titles and all - nothing to pass |
| anything else | one opaque file | silence detection (`--noise-db`, `--min-silence`, `--min-gap`), or `--marks-from` a chapter list |

```text
# LibriVox: a folder of per-chapter MP3s -> ONE book on shelf 0, one mark per chapter
scripts/prepare_audiobooks.py from-dir --join --shelf 0 -o /Volumes/SD ~/Downloads/hobbit_librivox

# LoyalBooks: a single .m4b -> marks (and labels) from its embedded chapter list
scripts/prepare_audiobooks.py convert --shelf 0 -o /Volumes/SD ~/Downloads/hobbit.m4b

# several INDEPENDENT books onto one shelf (not chapters of one), marks detected from silence
scripts/prepare_audiobooks.py from-dir --shelf 1 -o /Volumes/SD ./lectures

# author a sidecar for a book already on the card, without touching the audio
scripts/prepare_audiobooks.py marks /Volumes/SD/bard/0/BOOK1.WAV
```

`--join` is the important one: a 40-chapter LibriVox book left unjoined would eat 40 of a shelf's 32 slots as 40 separate "books", and you would lose the chapter list entirely. Joined, it is one book whose bookmarks *are* its chapters. Join offsets are accumulated from each converted part's actual frame count rather than from duration estimates, so the marks cannot drift over a ten-hour book, and files are ordered by a natural sort so `chapter_2` precedes `chapter_10` even when the names are not zero-padded.

The script also converts to 24 kHz 16-bit mono `.wav`, renames to 8.3, writes the `BOOKS.TXT` title map, and emits `H:MM:SS.mmm` - the exact format the firmware parses, so a mark list committed on the device round-trips. It enforces the limits that would otherwise fail silently: marks are thinned to 64 (evenly, and loudly when real chapters are being dropped - splitting the work into parts keeps them all), and the sidecar is trimmed under 4 KB because `read_text` truncates past that with no diagnostic. It also probes each source's sample rate and says so if `--rate` is *higher*, since upsampling only spends card space. Tested by `make test-scripts`, and `host/test_bard.cpp` re-parses the script's exact output with the firmware parser - including chapter titles containing a hyphen, which the grammar also uses for explicit ranges.

### Marks from the text: alignment, and the word index

[`scripts/align_bookmarks.py`](../../scripts/align_bookmarks.py) is a **prototype** that places marks by aligning the recording against its known text, rather than guessing at boundaries from silence. For a LibriVox book that is unusually tractable: these are public-domain works read aloud, so the text exists and this is *forced alignment* - what was said is given, only *when* has to be found.

It is deliberately **not** wired into `prepare_audiobooks.py` and **not** in `make test-scripts`: it needs numpy and a TTS voice, and its quality depends on the text genuinely matching the audio, which for a translated work is a question about editions rather than a parameter to tune.

```text
.venv/bin/python -m pip install numpy        # plus macOS `say` (built in) or espeak-ng, and ffmpeg
```

It does two quite different jobs.

**1. Structural marks** - one bookmark per text fragment (blank-line separated: stanzas in verse, paragraphs in prose):

```text
.venv/bin/python scripts/align_bookmarks.py BOOK.WAV book.txt --drop-leading 3 --dry-run
```

`--drop-leading N` skips leading blocks the reader did not speak (title, author). `--dry-run` reports without writing; drop it or pass `-o` to write the sidecar. A Gutenberg header/footer is stripped automatically.

**2. A word index** - one bookmark per *occurrence*, which is what makes the cut-up in [The clock, and the cut-up](#the-clock-and-the-cut-up) authorable:

```text
# what is worth indexing? lists words by frequency with their phonetic keys, then exits
... book.txt --drop-leading 3 --recurring 6 --dry-run

# every occurrence of one word
... book.txt --drop-leading 3 --find lenore --dry-run

# every word in a rhyme bucket, written as a Wander-mode score
... book.txt --drop-leading 3 --rhyme door --order file --loop segment -o BOOK.TXT

# same coarse phonetic skeleton; --loose widens it by one edit
... book.txt --drop-leading 3 --like never --loose --dry-run
```

Start with `--recurring N` to see which buckets exist before committing to one.

**"Sounds alike" is computed from spelling, not from audio.** Because the text is known, a word is reduced to a coarse consonant-class skeleton (`--like`) and to a rhyme key taken from its final vowel onward (`--rhyme`) - no embeddings, no acoustic matching, no query-by-example. Both keys are crude on purpose: when the aim is an *accidental* linkage as much as a semantic one, recall matters more than precision. The dial is the rhyme key's vowel handling - keeping the vowel distinguishes `-or` from `-er` from `-air`; collapsing it buckets roughly a tenth of a vocabulary together, which is "everything" rather than a linkage.

Set `--order file` (or `shuffle`) and the sidecar becomes a score: armed to the clock in **Wander** mode, the deck cuts between rhyme-linked fragments on the bar. Zero firmware involvement - the engine already consumes exactly this.

#### Reading the output, and verifying it

For **structural** runs two numbers say whether to trust the result:

- **drift** - if `max` equals the `--band` value *exactly*, the warp path is pinned against its constraint and the marks are fiction. Healthy output sits well inside the band.

- **pause score** - fragment boundaries should land in real pauses; a low median with most marks in the quietest 20% is good.

Both are **suppressed for word-index runs**, because neither applies there: a word mid-line is not in a pause, and drift assumes the marks cover the whole text. Printing them would invite reading a correct result as a failure.

```text
... --self-test --dry-run                  # align synthetic audio against its own text
... --self-test-stretch 1.576 --dry-run    # same, time-stretched so the true path slope is 1.576
```

**Use the stretch variant.** A slope-1 self-test passes on identical audio even when the up-step (slower-narrator) branch is broken, because the true path there is a pure diagonal - which is exactly how a bug survived the first self-test during bring-up. Expect under ~100 ms against known boundaries; the feature hop is 20 ms.

#### Limits

- **Word timing is approximate by construction.** Fragment boundaries come from the alignment, then each fragment's duration is split across its words by syllable count - so expect a second or two of error inside a long stanza. `--lead S` (default 0.6) places each mark that far *before* the estimated onset so the word is heard rather than missed.

- **Prose needs sentence-level fragmenting**, not implemented: blank-line paragraphs in a full-length book will exceed the 64-mark cap.

- **The text must match the recording.** For a translated work, confirm the reader used the same translation as your text source - a mismatched translation fails outright rather than degrading. Trimming spoken boilerplate from the audio matters more here than it might seem: a DTW aligner cannot absorb unmatched material locally, so anything extra at either end skews the whole path.

- Runtime scales with duration x band width; a 9-minute file is well under a minute.

### Resume - persisted, and switchable off

An audiobook player that forgets where you were is broken, so resume is **persisted from phase 1** and survives a power cycle. It is also **the one thing this engine writes to the card**, which is why it can be turned off.

**In RAM:** a bounded **LRU table of the 64 most recently played books**, each entry `{shelf, name, frame}`. 64 entries keeps the state at roughly 1.8 KB, which matters for the file too (below). Leaving a book and returning always lands where you left it, within the session, regardless of the persistence setting.

**On card:** `/bard/resume.txt`, the same table as text, one line per book:

```text
0/HOBBIT1.WAV 84719232
0/DUNE.WAV 12006400
```

The whole file is rewritten (not patched) on **book change**, on **pause**, and on a **~30 s checkpoint while playing**. At ~28 bytes a line, 64 entries is under 2 KB, so a full rewrite is trivial and the file stays comfortably inside `read_text`'s small-buffer pattern - the LRU cap exists to guarantee that, since an uncapped table over 16 shelves x 32 books would be ~14 KB and would silently truncate on read.

**Turning it off:** a card-side config file, `/bard/bard.cfg`, in the same spirit as radio's `rate.txt`:

```text
resume=on
rate=48000
```

`resume=off` means the engine never opens a file for writing - the in-RAM table still works for the session, and nothing is persisted. Missing file or missing key means `resume=on`, `rate=48000`. Folding `rate=` in here subsumes the need for a separate `rate.txt` (which applies to headerless `.raw` only in any case).

**Failure behaviour:** if a write fails - card write-protected, full, or absent - the engine logs nothing, stops retrying for the session, and carries on playing. A resume table is a convenience; losing it must never interrupt audio. Reading is equally forgiving: the parser discards unparseable lines rather than rejecting the file, so a power cut mid-rewrite costs at most the tail of the table. That tolerance is why no atomic write-and-rename dance is proposed - `FatFile` does not expose `f_rename`, and adding it to buy protection against a failure mode whose worst outcome is "one book forgot its position" is not a good trade.

**Contract cost:** this needs **one new method on `IStreamDeck`** - `write_text(const char* path, const char* buf, int n)`, main-loop only, backed by `FatFile`'s existing `open_write`/`write` (`src/hw/fat_file.h`). [`docs/engines/README.md`](README.md) warns that "the contract grows reluctantly", so this is a deliberate cost rather than a free move; it is also the smallest useful addition available, it is the exact mirror of the `read_text` that already exists, and every other engine gets a default no-op body.

---

## Controls (per deck)

![Bard control surface](../media/bard-controls.svg)

*Generated from [`docs/diagrams/controls/bard.json`](../diagrams/controls/bard.json) via `make diagrams`.*

The default column stays entirely on **navigation and pace** - the things you touch while listening - and all the effects live on the Flux/Grit modifier layers, which is what those pads are for.

| Control | `ParamId` / config | Effect |
|---|---|---|
| **PITCH** (`Speed`) | + V/oct CV jack | **BOOK** select - the shelf-browsing knob. Quantized to the books in the shelf, with radio's hysteresis + settle guards. |
| **POS** (`Pos`) | + size/pos CV jack | **BOOKMARK** select - the jump-to-mark knob. Quantized to the current book's mark list. |
| **SIZE** (`Size`) | | **RATE** 0.5x..2.5x, unity at centre. The audiobook speed knob. |
| **ENV** (`Env`) | | **PITCH-KEEP** 0..1. At 0 the rate change is plain varispeed (pitch follows speed, tape-like); at 1 the rate changes with the pitch **held** (WSOLA time-scaling). Continuous, so intermediate settings give "faster and a little deeper". |
| **MIX** (`Mix`) | + mix CV | deck **volume**. |
| **Cycle** (`ModSpeed`) | | mod **rate**, or the **duck release** time when Mod Type is Follow. **Alt+Cycle** locks it to the transport. |
| **Glow** (`ModAmp`) | | mod **depth** / **duck depth**. 0 = the whole modulation layer is off. |
| **Alt+PITCH** (`Aux`) | | **SHELF** select (0..15), held selector with ring dots - exactly radio's bank gesture. |
| **Alt+POS** (`AltPos`) | | **SCRUB** - fine seek within the current segment. POS is coarse (marks), Alt+POS is fine (inside one). Debounced, as in [pstretch](pstretch.md). |
| **Alt+SOS** (`Feedback`) | | **SEAM** - the crossfade time applied at every jump, ~0 to ~500 ms. Hard cuts for rhythmic work, soft dissolves for listening. |
| **Play pad** (`on_play_pad`, `reverse=false`) | | **PLAY / PAUSE**. |
| **Alt+Play pad** (`on_record_pad`, `reverse=false`) | | **DROP MARK** at the current position (session-only; committing it to the sidecar is deferred). |
| **Rev pad** (`on_play_pad`, `reverse=true`) | | **JUMP BACK** 15 s. Retriggering steps back another 15 s each time. |
| **Alt+Rev pad** (`on_record_pad`, `reverse=true`) | | **RE-ROLL** the auto-marks (only meaningful for a book with no sidecar). |
| **Seq pad** (`on_seq_trigger`) | | **NEXT** - advance one entry in sequence order now (the hands-on counterpart of gate-in; POS selects a mark by index, this follows the sidecar's order). |
| **Alt+Seq pad** (`on_seq_toggle_arm`) | | **ARM TO CLOCK** - advance on the transport's key (bar) boundary. This turns the mark list into a sequencer of speech. |
| **Alt+Seq held** (`clear_sequence`) | | **LOOP / HOLD** - toggle this deck's segment-end policy (below). |
| **Gate in** | | **NEXT BOOKMARK** - one jump per rising edge. |
| **Gate out** | | a pulse on every **bookmark crossing** - the story clocks the rack. |
| **Mod CV out** (`process_cv`) | | the deck's **speech envelope** (Follow) or **LFO** (LFO) as a 0..1 CV. |
| **Mode switch** (`Mode`) | | **SEQUENCE** mode (below). |
| **Mod Type switch** (`ModType`/`LfoShape`) | | **LFO** (rate wobble) or **FOLLOW** (this deck's speech envelope ducks the *other* deck). |
| **Size/Pos mod switches** | | mod **target**: Pos = rate, Size = colour, both = room. |
| **Mix fader** (`Crossfade`) | + crossfade CV | A/B blend. |
| **Routing switch** (`Route`) | | stereo topology (below). |
| **Flux pad + PITCH/SOS/POS** | `FluxIntensity`/`FluxMix`/`FluxFb` | **VOICE COLOUR** - drive and band-limit (wireless / gramophone / telephone), its mix, and a short slap regeneration. |
| **Grit pad + PITCH/SOS** | `GritIntensity`/`GritMix` | **ROOM** - decay and wet. **Alt+Grit** latches it; a second Grit press cycles the character (plate / hall / slap-echo), shown by the pad colour. |
| **Tap-hold + Play pad** | | **COMMIT MARKS** - write the current mark list (including anything dropped with Alt+Play) to the book's sidecar. The play LED flashes white on success. Note this **replaces** the file and writes timestamps only, so hand-written labels are lost - see [the sidecar](#the-bookmark-sidecar). |

Capabilities: `CapOwnDisplay | CapDualDeck | CapAux | CapAltPos | CapTransport | CapStepSequencer`.

The pad gestures above are the ones the platform actually offers, read off `src/ui/core.ui.pads.cpp`: Play/Rev are one handler (`on_play_pad(ref, reverse)`), Alt+Play/Alt+Rev are another (`on_record_pad(ref, reverse)`), and the Seq pad gives three - tap, Alt+tap, and Alt+hold, the last arriving as `clear_sequence` via the platform's hold timer. Reinterpreting `clear_sequence` as something other than "clear" has precedent: [edrums](edrums.md) uses it for "reset this deck's drums to defaults".

### Sequence modes (the mode switch)

The 3-position switch is silkscreened **Reel / Slice / Drift**. As with [reso](reso.md), those legacy labels happen to be literally apt here - they name what the playhead does with the mark list:

| Position | Label | Mode | Behaviour |
|---|---|---|---|
| top | Reel | **Read** | Linear. The book plays straight through; marks are jump targets only. This is "listen to the audiobook". |
| middle | Slice | **Recite** | Segment-locked. The selected segment plays and then either holds or loops (below). Turning POS is how you move on. This is "study this passage". |
| bottom | Drift | **Wander** | Auto-advance. At each segment end, jump to the next entry **in the sidecar's line order** (or per `order=`). This is where a scrambled sidecar becomes a composition. |

### Segment-end policy: both hold and loop, reachable two ways

What `Recite` does when a segment runs out is a per-deck **policy**, not a mode, because both behaviours are wanted for different material - a paragraph you are studying wants to loop; a passage you are auditioning before moving on wants to stop and stay put. So it is settable from the card and from the panel:

- **Authored:** the sidecar's `loop=segment` (loop) or `loop=off` (hold) sets the initial policy when the book opens. `loop=book` is a third value that applies to `Read`, not `Recite` - restart the whole book at the end.

- **Live:** **Alt+Seq held** toggles the policy for that deck, overriding the directive until the book changes. The Seq LED shows the current state.

- **Default when the sidecar says nothing: hold.** A silent stop is recoverable by turning one knob; an unexpected loop of a spoken passage is the more annoying surprise, and it is not obvious the audio has ended rather than repeated.

The policy also governs `Wander`'s last entry: hold stops at the end of the list, loop wraps to the first entry - which is what makes an armed, clocked deck run indefinitely off a short mark list.

### The clock, and the cut-up

`Wander` plus **Alt+Seq arm** is the feature that makes bard an instrument rather than a player: segment advance quantized to the transport's key interval, so a story cuts on the bar. Two decks armed to the same clock with different sidecars interleave two narrations deterministically - which is the Burroughs/Gysin cut-up applied to a fixed score rather than to scissors. Gate-in gives the same thing from an external clock; gate-out lets the story drive the rest of the rack.

This is also the mode with a real feasibility limit: every jump is a seek plus a ring re-prime. See [the jump-rate problem](#the-jump-rate-problem-the-one-that-gates-the-clock-modes).

### Two decks: the real design problem

`docs/engine-ideas.md`'s own test is whether an idea leans into the platform's shape or fights it. bard fights it in exactly one place: **two simultaneous narrators cannot both be understood.** Speech is semantically dense and masks itself. A dual-deck spoken-word engine has to answer this rather than assume the crossfader will sort it out. Three answers, all available from the same symmetric code:

1. **Voice and bed (recommended default).** Deck A is the narrator; deck B is a slow, quiet second recording used as atmosphere - a field recording, a lecture at low level, the same book far away. The **ducker** (Mod Type = Follow) makes it work: A's speech envelope pulls B down, so the bed breathes in the gaps. This is the "audiobook with a score" reading, and it is the one most people will want.

2. **Dichotic (routing LEFT / DoubleMono).** A story per ear. This is a genuine listening form, not a compromise - the two narrations compete for attention and the listener's focus becomes the mix.

3. **Cut-up collage (both decks armed to the clock).** Intelligibility is explicitly not the goal; the rhythm of speech fragments is. Short segments, hard seams (Alt+SOS near 0), fast rate.

The decks stay symmetric in code - both are full book players. The asymmetry is a performance choice.

### Routing / stereo image

- **LEFT (DoubleMono):** deck A hard-left, deck B hard-right (a story per ear).

- **CENTRE (Stereo):** both centred.

- **RIGHT (GenerativeStereo):** each deck at a random pan, re-rolled on entering the mode.

### Display

The ring is **the spine of the book**. Per deck:

- a **progress arc** filled to the playhead's position through the whole book;

- a **dim tick** at each bookmark, quantized to the 32 LED positions; the current segment's tick bright;

- **green** playing, **amber** paused, **cyan** armed to the clock, **red** on a failed open or an empty shelf (radio's amber-error convention, extended);

- the **Play LED** pulses on each bookmark crossing;

- **Alt held** replaces the arc with the **shelf** dots (16), as radio does for banks.

With 32 LEDs and up to 64 marks, several marks can collide on one LED. Show a colliding LED at higher brightness rather than pretending precision.

---

## Effects

Two continuous transforms of the reading in the default column (rate and pitch-keep) and two effect layers on the Flux/Grit pads.

### Rate and pitch-keep

**SIZE** sets the rate; **ENV** sets how much of the pitch shift that implies is cancelled. Output pitch is `rate^(1 - pitchkeep)`: at ENV=0 pure varispeed (a resampler stride change - what a tape machine does, and what [radio](radio.md)'s SIZE and [tape](tape.md)'s PITCH already do), at ENV=1 the rate changes with the pitch held, and intermediate settings give "faster and a little deeper".

Mechanically the chain resamples by `rate^(1-keep)` and then time-scales by `rate^-keep`, which compose to a speed change of exactly `rate` at any keep amount - so **turning ENV never changes how fast the book reads, only how the narrator sounds**. At ENV=0 the time-scaler is a **bit-exact passthrough**, so zero is not "nearly varispeed", it is the varispeed path unchanged.

### Why the pitch-preserved path is WSOLA and not PaulStretch

The pitch-preserved path is a **time-domain overlap-add with a similarity search (WSOLA)**, not the [pstretch](pstretch.md) FFT stretcher. This is worth stating because "we already have a time-stretch engine, reuse it" is the obvious wrong move: PaulStretch's mechanism *is* phase randomization, which destroys the phase coherence that makes consonants intelligible. It is the right algorithm for turning a voice into an ambient wash and precisely the wrong one for playing a book at 1.5x. WSOLA, conversely, is cheap, works over the 0.7x..2.5x range speech actually wants, and its artifacts (a slight doubling below ~0.8x, roughness above ~1.8x) are the familiar artifacts of every audiobook app.

Rough cost, on-target, per deck - **an estimate to be measured on the host harness, not a result**:

- naive search, 1024-sample frame, 480-lag search, ~94 hops/s: ~46 MMAC/s, order **10% of one core**;

- search on a 4x-decimated signal then refine: ~3 MMAC/s, order **1%**.

Decimated WSOLA is what shipped (1024-sample frame, +/-256 search, decimated by 4 then refined). The estimate above is still an estimate: it was validated for **correctness** in `host/` per the project's "validate headless first" rule - bit-exact bypass, the duration ratio, and pitch preservation at several scales - but its **CPU cost has not been measured on the device**.

**The ambient wash is out of scope entirely.** If you want an audiobook phase-randomized into an hour-long drone, [pstretch](pstretch.md) already streams a clip from `/pstretch` through exactly that - put the book there. Duplicating an 8192-point FFT inside bard to re-create a shipped engine's signature is scope creep with a memory cost.

### Colour (Flux) and room (Grit)

- **Flux - voice colour.** A drive stage plus a band-limiting biquad pair over `src/dsp/biquad.h`: opening from clean, through 1930s-wireless (300 Hz-3.5 kHz, driven), to telephone (300 Hz-3 kHz, harder). `FluxFb` adds a short slap with regeneration. Cheap, per-sample, and it is the single most character-per-cycle effect available for speech.

- **Grit - room.** Decay and wet, with `toggle_grit_mode` cycling plate / hall / slap-echo. Deliberately *one cheap algorithmic reverb*, not a Faust plate from [reverb](reverb.md). It is **not** the existing `src/dsp/diffuser.h`: that file is **GPLv3** (a port of qdelay's Diffusor) and linking it would relicense the whole engine away from the repository's MIT. `src/engine/bard/room.h` is instead written from the classic published structure - Schroeder's parallel comb bank into series allpasses, with a damping one-pole in each comb - so bard stays MIT. Delay lines come from the SDRAM arena.

---

## Feasibility

### What was reused (the reason this was cheap)

| Piece | Where | Used for |
|---|---|---|
| `StreamDeck` - lock-free SDRAM rings + main-loop FatFs pump | `src/hw/stream_deck.{h,cpp}`, `SPK_USE_STREAM` | all playback |
| `RawStreamReader` - int16 mono, `.raw` + `.wav`, `seek_to_frame` | `src/memory/raw_stream.h` | the codec and every jump |
| `scan_bank` + `bank_sort` - 8.3 filter, macOS AppleDouble filter, alphabetical order | `src/hw/stream_deck.cpp` | shelf enumeration |
| `read_text` | `IStreamDeck` | reading the sidecar, `bard.cfg`, and `resume.txt` |
| `FatFile::open_write`/`write` | `src/hw/fat_file.h` | backs the one new contract method (`write_text`) |
| `start_play_wav/raw(deck, path, start_frame, loop)` - seek-on-open | `IStreamDeck` | opening a book at a mark |
| Station-select hysteresis + settle-every-prepare anti-stutter guards | `src/engine/radio/radio_engine.cpp` | BOOK and BOOKMARK knobs |
| Debounced Alt+POS scrub against a streaming deck | `src/engine/pstretch/pstretch_engine.cpp` | SCRUB |
| Envelope follower, biquads, diffuser, smoothers | `src/dsp/` | ducker, colour, room |

pstretch is the important precedent for the budget question: it links the streaming stack *and* an 8192-point FFT *and* dual decks at ~80-82% SRAM_EXEC. bard needs the streaming stack and something far cheaper than that FFT, so the envelope is plausible - though "plausible by analogy" is not a link result.

### What was written

1. The **sidecar parser** (integer time scan, directive line, segment-end resolution) and the small `bard.cfg` key-value reader. Pure, host-testable in isolation - the first thing to write.

2. The **mark model**: 64 marks per deck, chronological ordering alongside file ordering, auto-mark generation from a name/length hash.

3. **Resume**: the 64-entry LRU table, its text serialization, the checkpoint policy, and **`IStreamDeck::write_text`** - the one contract addition, with a default no-op body for every other engine.

4. **Sequence state**: Read/Recite/Wander, segment-end detection and its loop/hold policy, clock-quantized advance, seam crossfade.

5. `scripts/prepare_audiobooks.py` and `host/test_bard.cpp`.

6. **Decimated WSOLA** (`src/engine/bard/wsola.h`) plus the `rate^(1-pitchkeep)` chain - deferred by decision #3, then built once the rest was green. Nothing else depends on it, which is what made deferring it safe.

7. **The room** (`src/engine/bard/room.h`) - MIT, from the Schroeder/Moorer structure, because the obvious candidate was GPLv3.

8. **`IStreamDeck::seek_play`** - the light in-file seek, which pays down a debt `docs/dev/radio-impl.md` had already recorded.

9. **`scripts/prepare_audiobooks.py`** + `scripts/test_prepare_audiobooks.py` - convert to 24 kHz mono, rename to 8.3, detect chapter marks from silence, write the sidecar and the `BOOKS.TXT` title map.

10. **`scripts/align_bookmarks.py`** (prototype, opt-in) - marks derived by aligning the recording against its known text, plus the word/rhyme index that makes a cut-up authorable. See [Marks from the text](#marks-from-the-text-alignment-and-the-word-index).

### The jump-rate problem (the one that gates the clock modes)

Every bookmark jump today means a re-open plus a ring flush plus a prime. `docs/dev/radio-impl.md` records that anything re-opening at audio rate stutters, and pstretch's scrub costs ~170 ms of soft output per seek. Armed to a clock at, say, four jumps per second, a full 1 MB-ring flush per jump will not sustain - the `Wander`-on-clock mode is the part of this design most likely to fail on hardware.

Three fixes, cheapest first:

1. **Prime shallow.** Resume audio after a bounded number of milliseconds of ring data rather than a full refill. Smallest change; probably sufficient for jumps up to roughly 1-2 per second.

2. **Seek without re-open.** An `f_lseek` on the already-open file plus a ring flush, avoiding the `f_open`. `docs/dev/radio-impl.md` already lists this as a wanted addition ("a lighter in-file seek"), so bard would be paying down a known debt rather than inventing scope.

3. **Segment-head cache.** Hold the first ~200 ms of each mark in SDRAM and play from the cache while the streamer catches up behind it - jumps become instant and glitch-free. 200 ms of 24 kHz 16-bit mono is 9.6 KB, so 64 marks is ~615 KB per deck (double that at 48 kHz). Filling it costs ~64 seeks and reads at book-open time, which must be chunked across `prepare()` calls or it stalls the UI for a second.

Fix 3 is what would make rhythmic speech cutting actually work, and it is also the piece with no precedent in the codebase. **Benchmark first**: measure achievable jumps-per-second under fix 1 in `host/` before committing to the cache, exactly as `docs/engine-ideas.md` prescribes for the grain cloud's scattered-read question.

### Other risks

- **SD bandwidth with both decks navigating.** radio already lists simultaneous sweeping as its unmeasured case; bard makes jumping the primary gesture rather than an occasional one. 24 kHz sources halve the steady-state bandwidth, which helps.

- **8.3 names** are a persistent authoring irritation for audiobooks; `scripts/prepare_audiobooks.py` hides it, but the card contents become unreadable to a human without the `BOOKS.TXT` map it writes.

- **`read_text` truncates** past `max-1` bytes with no error, so an over-long sidecar silently loses its tail. Enforce the 4 KB cap in the prep script and warn.

- **Card wear** from resume checkpointing. A ~30 s interval on a single sub-2 KB file is negligible; a per-block write would not be. This is the only write path in the engine, and `resume=off` removes it entirely.

- **Cost:** low-to-medium as built. Most of it is file parsing, a mark model and sequence state - main-loop and host-testable. The DSP added is a similarity search, four combs and two allpasses per deck. SRAM_EXEC needed `-Os` to keep working headroom (~88%; ~94% at `-O2`), and the WSOLA buffers put SRAM at ~41%. **No CPU measurement has been taken on the device.**

### What shipped, in the order it was built

1. **Player.** Shelf/book scan, sidecar parse, `bard.cfg`, auto-marks, BOOK + BOOKMARK + SCRUB, play/pause, jump-back, varispeed rate, persisted resume (`write_text`), the ring display.

2. **Sequence.** Read/Recite/Wander, the loop/hold policy, the seam fade, the three Seq-pad gestures, gate in/out, the speech-envelope CV out.

3. **Colour and space.** Flux voice colour, the ducker, then the Grit room.

4. **The light seek** (`seek_play`) and **committing marks** to the sidecar - both small, both enabled by work already done.

5. **The WSOLA experiment**, validated for correctness in `host/` before it was wired to ENV.

6. **`scripts/prepare_audiobooks.py`**, the card-prep companion.

7. **`scripts/align_bookmarks.py`**, a prototype text-alignment and word-index tool - deliberately outside the test path and not a dependency of the prep script.

What remains is not code: put real books on a card, listen, and tune the feel constants. See [`docs/dev/bard-impl.md`](../dev/bard-impl.md) for the list.

---

## Alternative framings

The proposal above should be argued against. Three alternatives, in descending order of how seriously they deserve consideration:

1. **bard as a mode of the radio engine, not a new engine.** **Considered and rejected (2026-07-30): bard is a standalone engine.** radio already has the streaming, the bank scan, the selector knob, and the anti-stutter guards; a "bookmarks" mode could swap the free-running clock for an owned playhead and read a sidecar - cheaper to build and one less firmware variant. *Against, and decisive:* the two engines disagree about the meaning of nearly every control - Play pad, POS, the playhead, the display, the pads, the Rev pad, the Seq pad. A mode flag that inverts the engine's defining behaviour and remaps most of the panel is two engines sharing a translation unit, and it would make radio's code harder to reason about in exchange for saving a `Makefile` entry. The platform's whole premise is that swapping the engine is cheap; take it at its word. Sharing happens at the *service* level (`StreamDeck`, `RawStreamReader`, `scan_bank`) where it already does, not by conditionals inside one engine.

2. **Skip the firmware; prep offline and use the tape engine.** Split books into per-segment files with a script and play them with [tape](tape.md)'s slot selector. Zero new firmware. *Against:* it loses the whole point - jumping *within* a continuous recording, resuming, and a mark list you can re-order in a text editor without re-cutting audio. It is a good fallback if the jump-rate benchmark comes back badly, and a good way to prototype the *sound* of clock-cut speech this week.

3. **A pstretch source mode.** pstretch already streams SD clips and has a scrub. Add bookmarks there. *Against:* pstretch's identity is the wash; bookmarks want intelligibility. Wrong host. The reverse direction is already available and worth stating in the docs - put a book in `/pstretch` if you want it smeared.

## Decisions (2026-07-30)

All seven were taken before implementation and all seven are reflected in the shipped code.

The seven questions this document opened, and their answers. The body of the document reflects them; they are recorded here so the reasoning is not lost.

| # | Question | Decision | Consequence in the design |
|---|---|---|---|
| 1 | Standalone engine, or a radio mode? | **Standalone.** | `ENGINE=bard`, its own `src/engine/bard/`. Sharing stays at the service level. See [Alternative framings](#alternative-framings) 1, now marked rejected. |
| 2 | Session-only resume, or persisted? | **Persisted, with an off switch.** | `/bard/resume.txt` (64-entry LRU) + `/bard/bard.cfg` `resume=on|off`; `IStreamDeck::write_text` is a phase-1 contract addition. See [Resume](#resume---persisted-and-switchable-off). |
| 3 | Is intelligible speed-up required? | **Not initially.** Varispeed first; WSOLA as a later experiment. | Followed, then the experiment landed: ENV is now PITCH-KEEP, backed by decimated WSOLA whose bypass at 0 is bit-exact, so the varispeed-only behaviour is still exactly what you get with the knob down. |
| 4 | Segment-head cache, or a shallow prime? | **The benchmark decides.** | Still the open question - but the cheapest of the three fixes shipped first: bookmark jumps now take a **light in-file seek** (an `f_lseek` on the live handle, no reopen) rather than close + open, so whatever the measurement says, it is measuring a cheaper jump. |
| 5 | Should a sidecar span multi-file books? | **No spanning.** | Parts are independent adjacent books. Auto-advance stops at a part boundary; resume is per book so returning still works. |
| 6 | Auto-marks deterministic or re-rolled? | **Deterministic per book.** | LCG seeded by a hash of filename + frame count, so marks are learnable across boots; Alt+Rev re-rolls for a session. |
| 7 | Does `Recite` hold or loop at a segment end? | **Both, per deck.** | The sidecar's `loop=` sets it, **Alt+Seq held** toggles it live, and the default when the sidecar is silent is **hold**. See [Segment-end policy](#segment-end-policy-both-hold-and-loop-reachable-two-ways). |

### Still open

**Only the jump rate**, and it is now the one thing standing between the engine and its clocked cut-up modes. Every bookmark jump is still a full re-open plus ring flush, so armed segment advance at more than roughly one jump per second is unproven and likely to stutter. Measure it on hardware (or with a ring-timing harness) before deciding whether the segment-head cache is worth ~615 KB per deck. If it comes back badly, [alternative framing 2](#alternative-framings) - pre-cut segments through the tape engine - is the honest fallback for that use case. Everything else here is decided and built.

---

## Appendix: the control-surface diagram spec

Now checked in as [`docs/diagrams/controls/bard.json`](../diagrams/controls/bard.json); `make diagrams` renders it to `docs/media/bard-controls.svg` (shown at the top of the control map). Reproduced here for reference:

```json
{
  "engine": "bard",
  "title": "Bard - bookmarked audiobook player",
  "subtitle": "two SD storytellers navigated by a text-file mark list",

  "knobs": {
    "Pitch": "BOOK select (+V/Oct CV); Alt = SHELF",
    "Position": "BOOKMARK select (+CV); Alt = SCRUB",
    "Size": "RATE 0.5-2.5x",
    "Envelope": "PITCH-KEEP (varispeed <-> pitch held)",
    "Mix (SOS)": "deck volume; Alt = SEAM (jump crossfade)",
    "Cycle": "mod rate / duck release (Alt = clock-sync)",
    "Glow": "mod depth / duck depth"
  },

  "pads": {
    "Play": "play/pause; Alt = drop mark; hold = commit",
    "Reverse": "jump back 15 s; Alt = re-roll auto-marks",
    "Grit": "ROOM (plate / hall / slap)",
    "Flux": "VOICE COLOUR (drive + band-limit)",
    "Seq": "next segment; Alt = arm to clock, Alt-hold = loop/hold"
  },

  "ring": "book progress + bookmark ticks; shelf dots (Alt)",

  "crossfade": "A/B blend of the two storytellers",

  "switches": {
    "Mode (L/C/R)": "sequence: Read / Recite / Wander",
    "Routing (L/C/R)": "stereo image: a story per ear / mono / random",
    "CV target (U/C/D)": "-",
    "Out trims A/B": "output trims"
  },

  "transport": {
    "Tap": "tempo (quantizes armed segment advance)",
    "Spot": "-"
  },

  "cv": {
    "Size/Pos A/B": "BOOKMARK CV",
    "Mix A/B": "deck volume CV",
    "V/Oct A/B": "BOOK CV",
    "Crossfade CV": "A/B blend"
  },

  "gates": {
    "Gate in A/B": "next bookmark",
    "Gate out A/B": "pulse per bookmark crossing"
  },

  "mod_midi": {
    "Mod CV out A/B": "speech envelope (Follow) or LFO"
  }
}
```

## Build / flash

```text
make -j8 ENGINE=bard         # build (~91% SRAM_EXEC, ~78% SDRAM)
make ENGINE=bard program-dfu
make engine-bard             # one-shot: clean + build + flash (device in DFU mode)
make -C host test            # host suites incl. test-bard
```

Note the shared `build/` directory: switching `ENGINE=` without a `make clean` first mixes object files and fails at link. The `engine-bard` target cleans for you.
