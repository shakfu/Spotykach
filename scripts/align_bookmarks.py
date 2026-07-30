#!/usr/bin/env python3
"""Derive bard bookmarks by ALIGNING a recording against its known text (a prototype).

Unlike silence detection, which guesses at boundaries from gaps, this locates each *fragment of the text*
in the recording, so a mark is "the start of stanza 12" rather than "a gap at 4:31". For a LibriVox book
that is unusually tractable because the text exists: it is public-domain prose or verse read aloud, so this
is forced alignment, not recognition - we already know what was said and only need to know when.

METHOD (the DTW flavour, as `aeneas` does it; no acoustic model or pronunciation dictionary needed):

  1. split the text into fragments on blank lines (stanzas, or paragraphs for prose);
  2. synthesise each fragment with TTS and concatenate, remembering where each fragment
     begins in the SYNTHETIC timeline - so fragment boundaries are known exactly, not guessed;
  3. reduce both signals to MFCCs, mean/variance normalised per signal (essential: a TTS voice
     and a human narrator differ enormously in absolute spectral shape, but their *trajectories*
     through phonetic space are comparable, which is what DTW matches);
  4. dynamic-time-warp synthetic against real, constrained to a band around the global rate ratio;
  5. read each fragment boundary off the warp path to get its real time.

WHY THIS IS A PROTOTYPE, NOT A DEPENDENCY OF prepare_audiobooks.py: it needs numpy and a TTS voice, and
alignment quality depends on the text actually matching the audio - which for a translated work is a real
question about editions, not a parameter to tune. Kept opt-in, out of `make test-scripts`.

    scripts/align_bookmarks.py <audio.wav> <text.txt> [-o SIDECAR.TXT] [--max-marks 64]

Reports drift and a pause-alignment score so the result can be judged without listening; see --help.
Requires numpy and, on macOS, `say` (or espeak-ng elsewhere). ffmpeg for non-WAV audio.
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import wave

try:
    import numpy as np
except ImportError:
    sys.exit("error: this prototype needs numpy (pip install numpy)")


# ---- audio io ------------------------------------------------------------------------------------

def read_wav_mono(path):
    """16-bit mono PCM WAV -> (float32 in [-1,1), rate). The engine's own format, so no conversion."""
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit(f"error: {path} is not 16-bit PCM")
        rate, n, ch = w.getframerate(), w.getnframes(), w.getnchannels()
        raw = w.readframes(n)
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, rate


def synth(text, path, rate, voice=None, wpm=None):
    """Speak `text` into a 16-bit mono WAV at `rate`. macOS `say`, else espeak-ng."""
    if shutil.which("say"):
        cmd = ["say", "-o", path, "--data-format", f"LEI16@{rate}"]
        if voice:
            cmd += ["-v", voice]
        if wpm:
            cmd += ["-r", str(wpm)]
        subprocess.run(cmd + [text], check=True)
    elif shutil.which("espeak-ng"):
        subprocess.run(["espeak-ng", "-w", path, "-s", str(wpm or 175), text], check=True)
        # espeak-ng writes at its own rate; resample to match.
        tmp = path + ".r.wav"
        subprocess.run(["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-i", path,
                        "-ac", "1", "-ar", str(rate), "-c:a", "pcm_s16le", tmp], check=True)
        os.replace(tmp, path)
    else:
        sys.exit("error: no TTS found (need macOS `say` or espeak-ng)")


# ---- features ------------------------------------------------------------------------------------

def mel_filterbank(n_filters, n_fft, rate, fmin=50.0, fmax=None):
    fmax = fmax or rate / 2.0
    def to_mel(f):   return 2595.0 * math.log10(1.0 + f / 700.0)
    def from_mel(m): return 700.0 * (10.0 ** (m / 2595.0) - 1.0)
    edges = np.linspace(to_mel(fmin), to_mel(fmax), n_filters + 2)
    hz = np.array([from_mel(m) for m in edges])
    bins = np.floor((n_fft + 1) * hz / rate).astype(int)
    fb = np.zeros((n_filters, n_fft // 2 + 1), dtype=np.float32)
    for i in range(n_filters):
        lo, mid, hi = bins[i], bins[i + 1], bins[i + 2]
        if mid == lo: mid = lo + 1
        if hi == mid: hi = mid + 1
        hi = min(hi, fb.shape[1] - 1)
        if lo >= hi: continue
        fb[i, lo:mid] = np.linspace(0, 1, mid - lo, endpoint=False)
        fb[i, mid:hi] = np.linspace(1, 0, hi - mid, endpoint=False)
    return fb


def mfcc(x, rate, hop_ms=20.0, win_ms=40.0, n_mfcc=13, n_filters=26):
    """MFCCs, mean/variance normalised. Also returns per-frame log energy (used for the pause check).

    The normalisation is what makes cross-speaker DTW work at all: a synthetic voice and a human narrator
    have very different absolute spectra, but comparable *relative* motion through phonetic space.
    """
    hop = max(1, int(rate * hop_ms / 1000.0))
    win = max(hop, int(rate * win_ms / 1000.0))
    n_fft = 1 << (win - 1).bit_length()
    if len(x) < win:
        return np.zeros((0, n_mfcc - 1), np.float32), np.zeros(0, np.float32)

    x = np.append(x[0], x[1:] - 0.97 * x[:-1])                 # pre-emphasis
    n_frames = 1 + (len(x) - win) // hop
    idx = np.arange(win)[None, :] + hop * np.arange(n_frames)[:, None]
    frames = x[idx] * np.hamming(win).astype(np.float32)
    spec = np.abs(np.fft.rfft(frames, n=n_fft)) ** 2
    energy = np.log(spec.sum(axis=1) + 1e-10).astype(np.float32)

    fb = mel_filterbank(n_filters, n_fft, rate)
    mel = np.log(spec @ fb.T + 1e-10).astype(np.float32)
    # DCT-II, keep 1..n_mfcc (drop c0: it is overall loudness, which differs most between voices)
    k = np.arange(n_filters)
    dct = np.cos(np.pi / n_filters * (k[None, :] + 0.5) * np.arange(n_mfcc)[:, None]).astype(np.float32)
    c = mel @ dct.T
    c = c[:, 1:]
    c -= c.mean(axis=0, keepdims=True)
    c /= (c.std(axis=0, keepdims=True) + 1e-6)
    return c, energy


# ---- banded DTW ----------------------------------------------------------------------------------

def dtw_map(synth_f, real_f, band_s, hop_ms, report=print):
    """Warp `synth_f` onto `real_f`; return an array mapping each synth frame -> real frame.

    Sakoe-Chiba band centred on the DIAGONAL SCALED BY THE RATE RATIO, not on the raw diagonal: a TTS
    voice and a narrator differ in overall pace by tens of percent, and an unscaled band would exclude the
    true path outright over a long file.
    """
    M, N = len(synth_f), len(real_f)
    if M == 0 or N == 0:
        sys.exit("error: empty feature sequence")
    slope = N / float(M)
    W = max(8, int(band_s * 1000.0 / hop_ms))
    report(f"  DTW: {M} synth x {N} real frames, slope {slope:.3f}, band +/-{band_s:.0f}s ({W} frames)")

    INF = np.float32(1e18)
    centre = (np.arange(M) * slope).astype(np.int64)
    lo = np.maximum(0, centre - W)
    hi = np.minimum(N, centre + W + 1)
    width = int((hi - lo).max())

    prev = np.full(width, INF, dtype=np.float32)
    back = np.zeros((M, width), dtype=np.uint8)          # 0=diag 1=up(real) 2=left(synth)
    prev_lo = 0

    for j in range(M):
        l, h = int(lo[j]), int(hi[j])
        w = h - l
        # cosine distance of this synth frame against the band of real frames
        s = synth_f[j]
        d = 1.0 - (real_f[l:h] @ s) / (np.linalg.norm(s) * np.linalg.norm(real_f[l:h], axis=1) + 1e-9)
        cur = np.full(width, INF, dtype=np.float32)

        if j == 0:
            cur[:w] = np.cumsum(d)                        # first synth frame: consume real frames freely
            back[j, :w] = 1
        else:
            off = l - prev_lo                             # shift between this row's band and the previous
            def shifted(k):
                """prev cost at real index (l + i - k), as a vector over i in [0,w)."""
                out = np.full(w, INF, dtype=np.float32)
                src_start = off - k
                for_i = np.arange(w) + src_start
                ok = (for_i >= 0) & (for_i < width)
                out[ok] = prev[for_i[ok]]
                return out
            diag = shifted(1)                             # (synth-1, real-1)
            left = shifted(0)                             # (synth-1, real)
            best = np.minimum(diag, left)
            choice = np.where(diag <= left, 0, 2).astype(np.uint8)
            # The "up" step (same synth frame, previous real frame) is inherently sequential: cur[i]
            # depends on cur[i-1]. The step LABEL must be recomputed each i - carrying a stale label
            # forward records up-steps as diagonals, and the backtrack then advances the synth frame
            # where it should hold it, which collapses the path onto the scaled diagonal.
            acc = np.empty(w, dtype=np.float32)
            ch = np.empty(w, dtype=np.uint8)
            run = INF                                     # = cur[i-1], the up-step predecessor
            for i in range(w):
                if best[i] <= run:
                    cand, cand_ch = best[i], choice[i]    # diagonal or left
                else:
                    cand, cand_ch = run, np.uint8(1)      # up: hold this synth frame
                acc[i] = cand
                ch[i] = cand_ch
                run = cand + d[i]                         # cur[i], for the next i
            cur[:w] = acc + d
            back[j, :w] = ch
        prev, prev_lo = cur, l

    # Backtrack from the end: the last synth frame must land on the last real frame.
    #
    # The step type decides which index moves, and getting this wrong is subtle but fatal: an "up" step
    # holds the synth frame and consumes a real frame (the narrator is slower here). A loop that advances
    # the synth frame every iteration silently turns every up-step into a diagonal, which pins the path to
    # the scaled diagonal and produces evenly spaced, entirely fictitious marks.
    mapping = np.full(M, -1, dtype=np.int64)
    j, i = M - 1, N - 1
    while j >= 0:
        l, h = int(lo[j]), int(hi[j])
        i = min(max(i, l), h - 1)
        mapping[j] = i            # overwritten while we stay on j, so it ends as the EARLIEST real frame
        step = back[j, i - l]
        if step == 1:             # up: same synth frame, earlier real frame
            if i > l:
                i -= 1
            else:
                j -= 1            # cannot leave the band: fall back to a synth step
        elif step == 0:           # diagonal
            i -= 1
            j -= 1
        else:                     # left: previous synth frame, same real frame
            j -= 1
        if i < 0:
            while j >= 0:
                mapping[j] = 0
                j -= 1
            break
    unset = mapping < 0
    if unset.any():               # any synth frame never visited inherits its successor's position
        for j in range(M - 2, -1, -1):
            if mapping[j] < 0:
                mapping[j] = mapping[j + 1]
        mapping[mapping < 0] = 0
    np.maximum.accumulate(mapping, out=mapping)          # enforce monotonicity
    return mapping


# ---- phonetic keys: "sounds like", computed from spelling rather than from audio ------------------
#
# The goal here is deliberately loose. For an instrument built on jumping between occurrences of a word -
# or of words that merely SOUND like it - a false positive is not an error, it is the point: an accidental
# link is as usable as a semantic one. So this does not try to be a pronunciation dictionary. It reduces a
# word to a coarse consonant-class skeleton (the Soundex idea, without truncation, keeping order) and to a
# rhyme key (from the final vowel onward). Both are crude and both are cheap, which is the right trade when
# recall matters more than precision.

_PHON_CLASS = str.maketrans({
    "B": "F", "P": "F", "V": "F", "F": "F",          # labials
    "C": "S", "G": "S", "J": "S", "K": "S", "Q": "S", "S": "S", "X": "S", "Z": "S",
    "D": "T", "T": "T",
    "L": "L", "M": "N", "N": "N", "R": "R",
    "A": "A", "E": "A", "I": "A", "O": "A", "U": "A", "Y": "A",
    "H": "", "W": "",
})


def phon_code(word):
    """Coarse consonant-class skeleton. Words with the same code 'sound alike' - roughly."""
    w = re.sub(r"[^A-Za-z]", "", word).upper()
    if not w:
        return ""
    w = w.translate(_PHON_CLASS)
    out = []
    for ch in w:                                     # collapse runs (SS -> S), which spelling loves
        if not out or out[-1] != ch:
            out.append(ch)
    return "".join(out)


def syllables(word):
    """Vowel-group count, floored at 1. Used only as a relative duration weight for word timing."""
    return max(1, len(re.findall(r"[aeiouy]+", word.lower())))


def rhyme_key(word):
    """Phonetic code from the LAST vowel group onward - a rough rhyme bucket.

    The trailing silent 'e' has to go first, or it counts as the final vowel group and words ending in
    consonant+e bucket on the 'e' alone - which puts them in a different bucket from the words they
    obviously rhyme with.
    """
    w = re.sub(r"[^A-Za-z]", "", word).lower()
    if len(w) > 2 and w.endswith("e") and w[-2] not in "aeiouy":
        w = w[:-1]
    if not w:
        return ""
    m = list(re.finditer(r"[aeiouy]+", w))
    tail = w[m[-1].start():] if m else w
    # Consonant classes as in phon_code, but the VOWEL is kept: the vowel is what distinguishes -or from
    # -er from -ar, and collapsing it buckets a tenth of the vocabulary together.
    out = []
    for ch in tail.upper():
        if ch in "AEIOUY":
            v = "E" if ch == "Y" else ch
            if not out or out[-1] != v:
                out.append(v)
        else:
            c = ch.translate(_PHON_CLASS)
            if c and (not out or out[-1] != c):
                out.append(c)
    return "".join(out)


def edit_distance(a, b, cap=2):
    """Levenshtein, abandoned once it exceeds `cap` (we only care about near-identical codes)."""
    if abs(len(a) - len(b)) > cap:
        return cap + 1
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        if min(cur) > cap:
            return cap + 1
        prev = cur
    return prev[-1]


def match_words(vocab, query, mode, loose):
    """Which vocabulary words match `query` under `mode`. Returns a set of lowercased words."""
    q = query.lower()
    if mode == "find":
        return {w for w in vocab if w == q}
    if mode == "like":
        qc = phon_code(q)
        hits = {w for w in vocab if phon_code(w) == qc}
        if loose:
            hits |= {w for w in vocab if edit_distance(phon_code(w), qc, 1) <= 1}
        return hits
    if mode == "rhyme":
        qk = rhyme_key(q)
        return {w for w in vocab if qk and rhyme_key(w) == qk}
    return set()


# ---- text ----------------------------------------------------------------------------------------

def strip_gutenberg(raw):
    parts = re.split(r"\*\*\*\s*START OF (?:THE|THIS) PROJECT GUTENBERG.*?\*\*\*", raw, flags=re.S)
    body = parts[1] if len(parts) > 1 else raw
    return re.split(r"\*\*\*\s*END OF (?:THE|THIS) PROJECT GUTENBERG", body, flags=re.S)[0]


def fragments(text, drop_leading=0):
    """Blank-line separated blocks: stanzas in verse, paragraphs in prose."""
    blocks, cur = [], []
    for line in text.splitlines():
        if line.strip():
            cur.append(line.strip())
        elif cur:
            blocks.append(" ".join(cur)); cur = []
    if cur:
        blocks.append(" ".join(cur))
    return blocks[drop_leading:]


# ---- main ----------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("audio")
    ap.add_argument("text")
    ap.add_argument("-o", "--out", help="sidecar to write (default: alongside the audio as NAME.TXT)")
    ap.add_argument("--drop-leading", type=int, default=0,
                    help="skip N leading text blocks (title/author matter the reader did not speak)")
    ap.add_argument("--max-marks", type=int, default=64, help="engine cap (default 64)")
    ap.add_argument("--band", type=float, default=45.0, help="DTW band half-width in seconds")
    ap.add_argument("--hop", type=float, default=20.0, help="feature hop in ms")
    ap.add_argument("--voice", help="TTS voice")
    ap.add_argument("--wpm", type=int, default=170, help="TTS rate (default 170)")
    ap.add_argument("--dry-run", action="store_true", help="report only; write nothing")
    g = ap.add_argument_group("word index (bookmark every occurrence instead of every fragment)")
    g.add_argument("--find", metavar="WORD", help="exact occurrences of WORD")
    g.add_argument("--like", metavar="WORD", help="words with the same coarse phonetic skeleton as WORD")
    g.add_argument("--rhyme", metavar="WORD", help="words sharing WORD's rhyme bucket")
    g.add_argument("--loose", action="store_true", help="with --like, also accept codes within 1 edit")
    g.add_argument("--recurring", type=int, metavar="N",
                   help="just list words occurring >= N times, with their phonetic keys, and exit")
    g.add_argument("--lead", type=float, default=0.6, metavar="S",
                   help="place each mark S seconds BEFORE the estimated word onset, so the word is "
                        "actually heard despite the timing being approximate (default 0.6)")
    g.add_argument("--order", choices=["file", "time", "shuffle"],
                   help="write an order= directive (shuffle makes the engine re-sequence the hits)")
    g.add_argument("--loop", choices=["off", "segment", "book"], help="write a loop= directive")
    ap.add_argument("--self-test", action="store_true",
                    help="align the SYNTHETIC audio against its own text, where the true boundaries are "
                         "known exactly. The only way to tell an aligner bug from an acoustic mismatch.")
    ap.add_argument("--self-test-stretch", type=float, metavar="F", default=0.0,
                    help="stronger self-test: time-stretch the synthetic audio by F (pitch preserved) and "
                         "align against it, so the true boundaries are known AND the path slope is F. A "
                         "slope-1 self-test cannot catch a bug in the up-step (slower-narrator) branch, "
                         "because the true path there is a pure diagonal - which is exactly how one hid.")
    args = ap.parse_args(argv)

    real, rate = read_wav_mono(args.audio)
    dur = len(real) / rate
    raw = open(args.text, errors="replace").read()
    frags = fragments(strip_gutenberg(raw), args.drop_leading)
    if not frags:
        sys.exit("error: no text fragments found")
    print(f"audio {os.path.basename(args.audio)}: {dur / 60:.2f} min at {rate} Hz")
    print(f"text: {len(frags)} fragment(s), {sum(len(f.split()) for f in frags)} words"
          f"{f' (dropped {args.drop_leading} leading block(s))' if args.drop_leading else ''}")
    if len(frags) > args.max_marks:
        print(f"  note: {len(frags)} fragments exceeds the {args.max_marks}-mark cap; "
              f"only the first {args.max_marks} will be written", file=sys.stderr)

    # 1-2. synthesise each fragment, concatenating and recording boundaries in synthetic time
    with tempfile.TemporaryDirectory(prefix="align-") as tmp:
        chunks, bounds = [], []
        for i, frag in enumerate(frags):
            p = os.path.join(tmp, f"f{i:04d}.wav")
            synth(frag, p, rate, args.voice, args.wpm)
            y, _ = read_wav_mono(p)
            bounds.append(sum(len(c) for c in chunks))
            chunks.append(y)
        syn = np.concatenate(chunks)
    truth = None
    if args.self_test_stretch:
        f = args.self_test_stretch
        with tempfile.TemporaryDirectory(prefix="stretch-") as tmp:
            a, b = os.path.join(tmp, "a.wav"), os.path.join(tmp, "b.wav")
            with wave.open(a, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
                w.writeframes((np.clip(syn, -1, 1) * 32767).astype("<i2").tobytes())
            subprocess.run(["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-i", a,
                            "-filter:a", f"atempo={1.0 / f:.6f}", "-ac", "1", "-ar", str(rate),
                            "-c:a", "pcm_s16le", b], check=True)
            real, _ = read_wav_mono(b)
        truth = [b_ / rate * f for b_ in bounds]
        dur = len(real) / rate
        print(f"SELF-TEST: synthetic audio time-stretched x{f} (true path slope {f}, boundaries known)")
    elif args.self_test:
        truth = [b / rate for b in bounds]
        real = syn.copy()                    # the answer is now known exactly
        dur = len(real) / rate
        print("SELF-TEST: aligning the synthetic audio against its own text")
    print(f"synthetic: {len(syn) / rate / 60:.2f} min (pace ratio {len(real) / len(syn):.3f})")

    # 3. features
    sf, _ = mfcc(syn, rate, hop_ms=args.hop)
    rf, energy = mfcc(real, rate, hop_ms=args.hop)

    # 4-5. warp, then read positions off the path
    mapping = dtw_map(sf, rf, args.band, args.hop)
    hop = max(1, int(rate * args.hop / 1000.0))

    def syn_samples_to_real_time(n):
        j = min(len(mapping) - 1, max(0, int(n) // hop))
        return float(mapping[j]) * hop / rate

    marks = [syn_samples_to_real_time(b) for b in bounds]
    labels = [f"fragment {i + 1}" for i in range(len(bounds))]

    # ---- word-level index -----------------------------------------------------------------------
    # The DTW path is dense (one entry per synthetic frame), so any point in synthetic time maps to real
    # time - not just the fragment boundaries. Within a fragment, each word's synthetic position is
    # estimated by distributing the fragment's synthetic duration across its words by syllable count.
    # That is approximate by construction; see --lead.
    word_times = []                                  # (word_lower, real_time)
    for fi, frag in enumerate(frags):
        f_start = bounds[fi]
        f_end = bounds[fi + 1] if fi + 1 < len(bounds) else len(syn)
        f_dur = max(1, f_end - f_start)
        toks = re.findall(r"[A-Za-z][A-Za-z'-]*", frag)
        if not toks:
            continue
        weights = [syllables(t) for t in toks]
        total = float(sum(weights))
        cum = 0.0
        for tok, wt in zip(toks, weights):
            word_times.append((tok.lower(), syn_samples_to_real_time(f_start + f_dur * cum / total)))
            cum += wt

    vocab = {}
    for w, _ in word_times:
        vocab[w] = vocab.get(w, 0) + 1

    if args.recurring:
        print(f"\nwords occurring at least {args.recurring} times "
              f"({len(vocab)} distinct words, {len(word_times)} tokens):")
        common = sorted((c, w) for w, c in vocab.items() if c >= args.recurring)
        for c, w in sorted(common, reverse=True):
            print(f"  {c:3d}  {w:20s} code={phon_code(w):12s} rhyme={rhyme_key(w)}")
        print(f"\n{len(common)} candidate(s). Pick one and re-run with --find/--like/--rhyme.")
        return 0

    query, mode = None, None
    for m, q in (("find", args.find), ("like", args.like), ("rhyme", args.rhyme)):
        if q:
            query, mode = q, m
    if query:
        hits = match_words(set(vocab), query, mode, args.loose)
        if not hits:
            print(f"\nno word matches --{mode} {query!r}", file=sys.stderr)
            return 1
        occ = [(t, w) for w, t in word_times if w in hits]
        print(f"\n--{mode} {query!r} -> {len(hits)} distinct word(s), {len(occ)} occurrence(s)")
        print("  matched: " + ", ".join(f"{w}({vocab[w]})" for w in sorted(hits)))
        if len(occ) > args.max_marks:
            step = len(occ) / float(args.max_marks)
            occ = [occ[int(i * step)] for i in range(args.max_marks)]
            print(f"  thinned evenly to the {args.max_marks}-mark cap", file=sys.stderr)
        marks = [max(0.0, t - args.lead) for t, _ in occ]
        labels = [w for _, w in occ]

    # ---- quality control, so this can be judged without listening -------------------------------
    # (a) monotonic drift: a coverage mismatch (missing/extra text) shows up as a growing offset
    #     between where a fragment SHOULD be under a constant pace and where it landed.
    lin = [dur * b / len(syn) for b in bounds]
    resid = [m - l for m, l in zip(marks, lin)]
    # (b) pause alignment: a correct fragment boundary should sit in a quiet moment. Report the
    #     percentile of frame energy at each mark - low is good. This is the objective proxy for
    #     "did it land in the right place" that needs no listening.
    e_sorted = np.sort(energy)
    pct = []
    for m in marks:
        fi = min(len(energy) - 1, int(m * rate / hop))
        w = energy[max(0, fi - 5): fi + 6]
        pct.append(float(np.searchsorted(e_sorted, w.min()) / len(e_sorted) * 100.0))

    print(f"\nmarks: {len(marks)}   first {marks[0]:.2f}s   last {marks[-1]:.2f}s   (audio {dur:.1f}s)")
    if query:
        # Neither metric applies to a word index: a word mid-line is not in a pause, and "drift vs
        # constant pace" assumes the marks cover the whole text. Printing them would invite reading a
        # correct result as a failure.
        print("  (drift and pause metrics apply to fragment alignment, not to a sparse word index)")
    else:
        print(f"drift vs constant pace: mean {np.mean(resid):+.1f}s  min {min(resid):+.1f}s  "
              f"max {max(resid):+.1f}s")
        print(f"pause score (energy percentile at each mark; lower is better):")
        print(f"  median {np.median(pct):.0f}%   worst {max(pct):.0f}%   "
              f"{sum(1 for p in pct if p < 20)}/{len(pct)} marks in the quietest 20%")
    mono = all(b > a for a, b in zip(marks, marks[1:]))
    print(f"monotonic: {'yes' if mono else 'NO - alignment failed'}")
    if truth is not None:
        err = [abs(m - t) for m, t in zip(marks, truth)]
        print(f"SELF-TEST error vs known boundaries: mean {np.mean(err) * 1000:.0f} ms  "
              f"max {max(err) * 1000:.0f} ms")
        ok = max(err) < 0.100
        print(f"SELF-TEST {'PASS' if ok else 'FAIL'} (expect < 100 ms on identical audio)")
        if not ok:
            return 1

    print("\n  #   time        gap     label")
    for i, m in enumerate(marks):
        gap = m - marks[i - 1] if i else 0.0
        note = "" if query else (f"   {pct[i]:3.0f}%" + ("  <-- suspect" if pct[i] > 50 else ""))
        print(f"  {i + 1:2d}  {int(m // 60):02d}:{m % 60:06.3f}  {gap:6.1f}s  {labels[i]}{note}")

    if args.dry_run:
        return 0
    out = args.out or (os.path.splitext(args.audio)[0] + ".TXT")
    keep, keep_lab = marks[: args.max_marks], labels[: args.max_marks]
    with open(out, "w") as f:
        if args.order or args.loop:
            d = "#!bard"
            if args.order:
                d += f" order={args.order}"
            if args.loop:
                d += f" loop={args.loop}"
            f.write(d + "\n")
        f.write(f"# aligned from {os.path.basename(args.text)} ({len(keep)} marks)\n")
        for m, lab in zip(keep, keep_lab):
            ms = int(round(m * 1000))
            h, rem = divmod(ms // 1000, 3600)
            mi, s = divmod(rem, 60)
            f.write(f"{h}:{mi:02d}:{s:02d}.{ms % 1000:03d}   {lab}\n")
    print(f"\nwrote {out} ({len(keep)} marks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
