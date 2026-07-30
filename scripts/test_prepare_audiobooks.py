"""Tests for scripts/prepare_audiobooks.py (the bard engine's card-prep companion).

Covers the pure logic - timestamp formatting, chapter-list import, mark thinning, 8.3 naming, and the
sidecar writer - without needing ffmpeg. The round-trip property that matters is that what this script
writes is exactly what src/engine/bard/bookmarks.h parses.
"""

import importlib.util
import os
import pathlib
import sys

_HERE = pathlib.Path(__file__).parent
_spec = importlib.util.spec_from_file_location("prep", _HERE / "prepare_audiobooks.py")
prep = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(prep)


# ---- format_time: must match bookmarks.h's H:MM:SS.mmm ---------------------------------------------

def test_format_time_matches_engine_format():
    assert prep.format_time(0) == "0:00:00.000"
    assert prep.format_time(1.5) == "0:00:01.500"
    assert prep.format_time(872) == "0:14:32.000"
    assert prep.format_time(3731.25) == "1:02:11.250"


def test_format_time_clamps_negative():
    assert prep.format_time(-5) == "0:00:00.000"


def test_format_time_beyond_ten_hours():
    # A 24 kHz book can legitimately run past 10 h, so the hours field must not be fixed at one digit.
    assert prep.format_time(12 * 3600 + 61) == "12:01:01.000"


# ---- chapter-list import ---------------------------------------------------------------------------

def test_parse_mark_file_accepts_every_time_form(tmp_path):
    f = tmp_path / "chapters.txt"
    f.write_text("# a comment\n\n0:00\n14:32\n1:02:11\n2841\n0:01.500 with a label\n")
    marks = prep.parse_mark_file(f)
    assert 0.0 in marks
    assert 872.0 in marks                    # 14:32
    assert 3731.0 in marks                   # 1:02:11
    assert 2841.0 in marks                   # bare seconds
    assert 1.5 in marks


def test_parse_mark_file_skips_garbage_and_dedupes(tmp_path):
    f = tmp_path / "c.txt"
    f.write_text("not a time\n0:10\n0:10\n---\n")
    assert prep.parse_mark_file(f) == [10.0]


# ---- mark thinning -------------------------------------------------------------------------------

def test_thin_marks_enforces_minimum_gap():
    marks = [10.0, 12.0, 14.0, 200.0, 400.0]
    out = prep.thin_marks(marks, duration=600.0, min_gap=60.0)
    assert out[0] == 0.0                     # a book always gets a mark at its start
    gaps = [b - a for a, b in zip(out, out[1:])]
    assert all(g >= 60.0 for g in gaps)


def test_thin_marks_drops_marks_at_the_very_edges():
    out = prep.thin_marks([0.2, 300.0, 599.5], duration=600.0, min_gap=10.0)
    assert 0.2 not in out and 599.5 not in out


def test_thin_marks_respects_the_engine_cap():
    # The engine parses at most 64 marks and silently drops the rest, so thinning must be EVEN rather
    # than truncating - losing the end of a book's marks is worse than spreading them out.
    marks = [float(i * 70) for i in range(1, 400)]
    out = prep.thin_marks(marks, duration=400 * 70.0, min_gap=60.0)
    assert len(out) <= prep.MAX_MARKS
    assert out[-1] > marks[len(marks) // 2]  # coverage reaches the far end of the book


# ---- 8.3 naming ----------------------------------------------------------------------------------

def test_book_name_numbers_by_default():
    assert prep.book_name(1, False, "The Hobbit - Chapter 3.mp3") == "01.WAV"
    assert prep.book_name(12, False, "x.mp3") == "12.WAV"


def test_book_name_keep_names_is_8_3_safe():
    # The firmware's scan skips any name longer than 12 chars, so a kept name must still fit 8.3.
    name = prep.book_name(1, True, "The Hobbit - Chapter 3.mp3")
    stem, ext = os.path.splitext(name)
    assert len(stem) <= 8 and ext == ".WAV"
    assert len(name) <= 12
    assert all(c.isalnum() or c in "_-" for c in stem)


def test_book_name_falls_back_to_numbering_when_the_stem_vanishes():
    assert prep.book_name(7, True, "!!!.mp3") == "07.WAV"


# ---- sidecar writing -----------------------------------------------------------------------------

def test_write_sidecar_emits_parseable_text(tmp_path):
    wav = tmp_path / "01.WAV"
    wav.write_bytes(b"\0" * 16)
    path = prep.write_sidecar(str(wav), [0.0, 872.0, 3731.25], order="time", loop="segment", title="Hobbit")
    text = pathlib.Path(path).read_text()
    assert path.endswith("01.TXT")
    assert text.startswith("#!bard order=time loop=segment")
    assert "# Hobbit" in text
    assert "0:14:32.000" in text
    lines = [l for l in text.splitlines() if l and not l.startswith("#")]
    assert len(lines) == 3


def test_write_sidecar_without_directives(tmp_path):
    wav = tmp_path / "02.WAV"
    wav.write_bytes(b"\0" * 16)
    text = pathlib.Path(prep.write_sidecar(str(wav), [0.0, 10.0])).read_text()
    assert "#!bard" not in text


def test_write_sidecar_returns_none_with_no_marks(tmp_path):
    wav = tmp_path / "03.WAV"
    wav.write_bytes(b"\0" * 16)
    assert prep.write_sidecar(str(wav), []) is None


def test_write_sidecar_stays_under_the_read_text_cap(tmp_path):
    # IStreamDeck::read_text reads only the first 4 KB and truncates SILENTLY, so the sidecar must be
    # trimmed here (where we can warn) rather than losing its tail on the device with no diagnostic.
    wav = tmp_path / "04.WAV"
    wav.write_bytes(b"\0" * 16)
    marks = [float(i * 61) for i in range(prep.MAX_MARKS)]
    path = prep.write_sidecar(str(wav), marks, title="a" * 200)
    assert len(pathlib.Path(path).read_bytes()) <= prep.MAX_SIDECAR_BYTES


# ---- natural sort: JOIN order must not scramble a book --------------------------------------------

def test_natural_key_orders_unpadded_chapter_numbers():
    names = ["hobbit_10_x.mp3", "hobbit_2_x.mp3", "hobbit_1_x.mp3", "hobbit_3_x.mp3"]
    assert sorted(names, key=prep.natural_key) == [
        "hobbit_1_x.mp3", "hobbit_2_x.mp3", "hobbit_3_x.mp3", "hobbit_10_x.mp3"]


def test_natural_key_handles_zero_padded_names_too():
    names = ["ch_02.mp3", "ch_01.mp3", "ch_10.mp3"]
    assert sorted(names, key=prep.natural_key) == ["ch_01.mp3", "ch_02.mp3", "ch_10.mp3"]


def test_natural_key_ignores_directory_and_case():
    a = prep.natural_key("/A/Deep/Path/CH_2.MP3")
    b = prep.natural_key("/other/ch_2.mp3")
    assert a == b


# ---- WAV frame counting: JOIN offsets depend on it --------------------------------------------------

def _wav16(frames, extra_chunk=False):
    """Minimal 16-bit mono WAV, optionally with a LIST chunk before `data` (ffmpeg writes these)."""
    data = b"\0\0" * frames
    chunks = b""
    if extra_chunk:
        payload = b"INFOhello!!!"
        chunks += b"LIST" + len(payload).to_bytes(4, "little") + payload
    fmt = (b"fmt " + (16).to_bytes(4, "little") + (1).to_bytes(2, "little") + (1).to_bytes(2, "little")
           + (24000).to_bytes(4, "little") + (48000).to_bytes(4, "little")
           + (2).to_bytes(2, "little") + (16).to_bytes(2, "little"))
    body = b"WAVE" + fmt + chunks + b"data" + len(data).to_bytes(4, "little") + data
    return b"RIFF" + len(body).to_bytes(4, "little") + body


def test_wav_frames_reads_the_data_chunk(tmp_path):
    f = tmp_path / "a.wav"
    f.write_bytes(_wav16(1234))
    assert prep.wav_frames(f) == 1234


def test_wav_frames_survives_an_extra_chunk_before_data(tmp_path):
    # (filesize - 44) / 2 would be WRONG here, and every join mark after the first would drift.
    f = tmp_path / "b.wav"
    f.write_bytes(_wav16(1000, extra_chunk=True))
    assert prep.wav_frames(f) == 1000


def test_wav_frames_rejects_non_wav(tmp_path):
    f = tmp_path / "c.bin"
    f.write_bytes(b"not a wav at all")
    assert prep.wav_frames(f) is None


# ---- exact chapter marks are not gap-filtered -------------------------------------------------------

def test_exact_marks_keep_short_chapters():
    # A 20-second chapter is still a chapter; the min-gap heuristic applies to GUESSES, not boundaries.
    marks = [0.0, 20.0, 40.0, 600.0]
    out = prep.thin_marks(marks, duration=1200.0, min_gap=60.0, exact=True)
    assert out == [0.0, 20.0, 40.0, 600.0]


def test_exact_marks_still_respect_the_engine_cap():
    marks = [float(i * 30) for i in range(200)]
    out = prep.thin_marks(marks, duration=200 * 30.0, min_gap=60.0, exact=True)
    assert len(out) <= prep.MAX_MARKS


def test_inexact_marks_are_still_gap_filtered():
    marks = [0.0, 20.0, 40.0, 600.0]
    out = prep.thin_marks(marks, duration=1200.0, min_gap=60.0, exact=False)
    assert out == [0.0, 600.0]


# ---- sidecar labels ------------------------------------------------------------------------------

def test_write_sidecar_uses_chapter_titles_as_labels(tmp_path):
    wav = tmp_path / "01.WAV"
    wav.write_bytes(b"\0" * 16)
    labels = ["Chapter 1 - An Unexpected Party", "Chapter 2 - Roast Mutton"]
    text = pathlib.Path(prep.write_sidecar(str(wav), [0.0, 10.0], labels=labels)).read_text()
    assert "Chapter 1 - An Unexpected Party" in text
    # A hyphen inside a title must stay on the label side of the line - the grammar uses '-' for ranges,
    # and host/test_bard.cpp asserts the firmware parser reads these lines as open-ended marks.
    assert text.splitlines()[0].startswith("0:00:00.000   Chapter 1")


def test_write_sidecar_collapses_whitespace_in_labels(tmp_path):
    wav = tmp_path / "02.WAV"
    wav.write_bytes(b"\0" * 16)
    text = pathlib.Path(prep.write_sidecar(str(wav), [0.0], labels=["a\n\tb   c"])).read_text()
    assert "a b c" in text


def test_write_sidecar_falls_back_to_numbering_without_labels(tmp_path):
    wav = tmp_path / "03.WAV"
    wav.write_bytes(b"\0" * 16)
    text = pathlib.Path(prep.write_sidecar(str(wav), [0.0, 5.0])).read_text()
    assert "mark 1" in text and "mark 2" in text


# ---- the title map -------------------------------------------------------------------------------

def test_write_titles_maps_8_3_back_to_real_titles(tmp_path):
    prep.write_titles(str(tmp_path), [("01.WAV", "The Hobbit - Chapter 1.mp3")])
    text = (tmp_path / "BOOKS.TXT").read_text()
    assert "01.WAV" in text and "The Hobbit - Chapter 1.mp3" in text


def test_write_titles_skips_an_empty_shelf(tmp_path):
    prep.write_titles(str(tmp_path), [])
    assert not (tmp_path / "BOOKS.TXT").exists()


# ---- random marks snapped to silence ---------------------------------------------------------------

def test_random_marks_are_deterministic_for_a_seed():
    a = prep.random_marks(600.0, 10, [], 5.0, seed=42, report=lambda *_: None)
    b = prep.random_marks(600.0, 10, [], 5.0, seed=42, report=lambda *_: None)
    assert a == b


def test_random_marks_differ_for_a_different_seed():
    a = prep.random_marks(600.0, 10, [], 5.0, seed=1, report=lambda *_: None)
    b = prep.random_marks(600.0, 10, [], 5.0, seed=2, report=lambda *_: None)
    assert a != b


def test_random_marks_start_at_zero_and_stay_in_range():
    out = prep.random_marks(600.0, 12, [], 5.0, seed=7, report=lambda *_: None)
    assert out[0] == 0.0
    assert all(0.0 <= t < 600.0 for t in out)
    assert out == sorted(out)


def test_random_marks_cover_the_whole_book():
    # Stratified, not plain uniform: the point is that marks reach the END of the recording rather than
    # clumping wherever the draws happened to fall.
    out = prep.random_marks(600.0, 10, [], 5.0, seed=3, report=lambda *_: None)
    assert out[-1] > 480.0


def test_random_marks_snap_to_nearby_silences():
    silences = [100.0, 200.0, 300.0, 400.0, 500.0]
    out = prep.random_marks(600.0, 6, silences, 5.0, seed=11, report=lambda *_: None)
    snapped = [t for t in out[1:] if t in silences]
    assert snapped, "expected at least one draw to reach a pause"


def test_random_marks_keep_the_raw_time_when_no_pause_is_near():
    # A stretch of continuous speech must not cost coverage - the draw keeps its raw time.
    out = prep.random_marks(600.0, 6, [1.0], 5.0, seed=5, snap_window=2.0, report=lambda *_: None)
    assert len(out) > 2


def test_random_marks_clamp_an_over_wide_min_gap():
    # min_gap 60s with 20 marks over 600s (30s strata) would reject nearly every draw; it is clamped.
    notes = []
    out = prep.random_marks(600.0, 20, [], 60.0, seed=9, report=notes.append)
    assert len(out) > 10
    assert any("min-gap" in n for n in notes)


def test_random_marks_respect_the_effective_gap():
    out = prep.random_marks(600.0, 10, [], 5.0, seed=13, report=lambda *_: None)
    assert all(b - a >= 5.0 for a, b in zip(out, out[1:]))


def test_seed_for_is_stable_and_filename_derived():
    # Mirrors the engine's book_seed convention: same book -> same scatter on every run. Python's hash()
    # is salted per process and would make the card irreproducible.
    assert prep.seed_for("/a/b/RAVEN.WAV", None) == prep.seed_for("/other/RAVEN.WAV", None)
    assert prep.seed_for("/a/RAVEN.WAV", None) != prep.seed_for("/a/KANT.WAV", None)
    assert prep.seed_for("/a/RAVEN.WAV", 99) == 99


# ---- book titles: BOOKS.TXT is the only place a human can read what 01.WAV is -----------------------

def test_title_from_dir_skips_format_named_containers():
    # A LibriVox download unpacks as <book>/wav/, and the plain basename would label the book "wav".
    assert prep.title_from_dir("content/beyond_good_and_evil/wav") == "beyond good and evil"
    assert prep.title_from_dir("x/the_hobbit/mp3/64kb") == "the hobbit"


def test_title_from_dir_uses_the_folder_when_it_is_not_a_container():
    assert prep.title_from_dir("content/the_raven") == "the raven"
    assert prep.title_from_dir("aesop") == "aesop"


def test_title_from_dir_turns_separators_into_spaces():
    assert prep.title_from_dir("some/principles-metaphysic_morals") == "principles metaphysic morals"


def test_title_from_dir_climbs_to_the_nearest_real_ancestor(tmp_path):
    # A bare wav/ inside a book folder must resolve to the BOOK, not to "wav".
    book = tmp_path / "some_book" / "wav"
    book.mkdir(parents=True)
    assert prep.title_from_dir(str(book)) == "some book"


def test_title_from_dir_falls_back_only_when_every_ancestor_is_a_container():
    assert prep.title_from_dir("/wav") == "book"
    assert prep.title_from_dir("/mp3/64kb") == "book"


# ---- a one-mark sidecar is worse than none ---------------------------------------------------------

def test_thin_marks_returns_only_the_start_when_nothing_is_detected():
    # This is the shape choose_marks() must treat as failure: a lone start-of-book mark would count as a
    # real sidecar on the device and suppress the engine's auto-marks, leaving the book one big segment.
    assert prep.thin_marks([], duration=600.0, min_gap=60.0) == [0.0]
    assert prep.thin_marks([5.0, 10.0], duration=600.0, min_gap=60.0) == [0.0]


# ---- incremental shelf building --------------------------------------------------------------------

def test_write_titles_merges_across_runs(tmp_path):
    # A shelf is filled by several runs (--join takes one book at a time), so truncating would leave the
    # card describing only the last book - and the map is the only place a human can see what 01.WAV is.
    prep.write_titles(str(tmp_path), [("01.WAV", "First Book.mp3")])
    prep.write_titles(str(tmp_path), [("KANT.WAV", "Kant (6 chapters joined)")])
    text = (tmp_path / "BOOKS.TXT").read_text()
    assert "01.WAV" in text and "First Book.mp3" in text
    assert "KANT.WAV" in text and "Kant (6 chapters joined)" in text


def test_write_titles_updates_an_existing_entry(tmp_path):
    prep.write_titles(str(tmp_path), [("01.WAV", "old")])
    prep.write_titles(str(tmp_path), [("01.WAV", "new")])
    text = (tmp_path / "BOOKS.TXT").read_text()
    assert "new" in text and "old" not in text


def test_read_titles_tolerates_a_missing_or_commented_map(tmp_path):
    assert prep.read_titles(str(tmp_path)) == {}
    (tmp_path / "BOOKS.TXT").write_text("# header only\n\n")
    assert prep.read_titles(str(tmp_path)) == {}


def test_next_free_index_skips_existing_numbered_books(tmp_path):
    # Numbering restarted at 01 every run, silently clobbering earlier books on an incremental fill.
    assert prep.next_free_index(str(tmp_path)) == 1
    (tmp_path / "01.WAV").write_bytes(b"x")
    (tmp_path / "02.WAV").write_bytes(b"x")
    assert prep.next_free_index(str(tmp_path)) == 3


def test_next_free_index_ignores_named_books_and_sidecars(tmp_path):
    (tmp_path / "KANT.WAV").write_bytes(b"x")
    (tmp_path / "01.TXT").write_text("x")
    (tmp_path / "BOOKS.TXT").write_text("x")
    assert prep.next_free_index(str(tmp_path)) == 1


def test_next_free_index_fills_a_gap(tmp_path):
    (tmp_path / "02.WAV").write_bytes(b"x")
    assert prep.next_free_index(str(tmp_path)) == 1


# ---- card layout ---------------------------------------------------------------------------------

def test_shelf_dir_layout():
    assert prep.shelf_dir("/Volumes/SD", 3).replace(os.sep, "/") == "/Volumes/SD/bard/3"


def test_engine_limits_match_the_firmware():
    # These mirror src/engine/bard/{bookmarks.h,bard_engine.h}; if the firmware changes, this fails loudly.
    assert prep.MAX_SHELVES == 16
    assert prep.MAX_BOOKS == 32
    assert prep.MAX_MARKS == 64
    assert prep.MAX_SIDECAR_BYTES == 4096
    assert prep.MIN_BOOK_BYTES == 32 * 1024
