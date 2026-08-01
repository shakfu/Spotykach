#!/usr/bin/env python3
"""Tests for convert_tape_audio.py.

Run from anywhere with:  pytest scripts/test_convert_tape_audio.py

Most tests are self-contained: they build WAV byte layouts in memory and drive the
orchestrator through a stub Converter, so they need neither ffmpeg/sox nor the deck.
The end-to-end tests that exercise a real converter are skipped when the tool is absent.
"""

import os
import struct
import subprocess
import shutil

import pytest

import convert_tape_audio as m


# --- helpers: build arbitrary WAV byte layouts ------------------------------

def fmt_chunk(af=m.TARGET_FMT, ch=1, sr=m.TARGET_RATE, bits=m.TARGET_BITS):
    block_align = (bits // 8) * ch
    return struct.pack("<HHIIHH", af, ch, sr, sr * block_align, block_align, bits)


def make_wav(chunks, riff=b"RIFF", wave=b"WAVE"):
    """chunks: list of (4-byte id, body bytes). Word-aligns odd bodies like a real RIFF."""
    body = b""
    for cid, data in chunks:
        body += cid + struct.pack("<I", len(data)) + data
        if len(data) & 1:
            body += b"\x00"
    return riff + struct.pack("<I", 4 + len(body)) + wave + body


def target_wav(seconds=1.0):
    """A minimal conformant mono/48k/float32 WAV with `seconds` of (zeroed) audio."""
    n = int(seconds * m.TARGET_RATE) * m.BYTES_PER_SAMPLE
    return m.canonical_header(n) + b"\x00" * n


# --- canonical_header -------------------------------------------------------

def test_canonical_header_layout():
    h = m.canonical_header(384000)
    assert len(h) == 44
    assert h[0:4] == b"RIFF" and h[8:12] == b"WAVE" and h[12:16] == b"fmt "
    assert h[36:40] == b"data"
    assert struct.unpack("<I", h[4:8])[0] == 36 + 384000     # RIFF size
    assert struct.unpack("<I", h[40:44])[0] == 384000        # data size
    af, ch = struct.unpack("<HH", h[20:24])
    sr = struct.unpack("<I", h[24:28])[0]
    bits = struct.unpack("<H", h[34:36])[0]
    assert (af, ch, sr, bits) == m.TARGET_TUPLE


# --- walk_wav (chunk-walker mirroring the firmware) -------------------------

def _walk_bytes(b, tmp_path, name="x.wav"):
    p = tmp_path / name
    p.write_bytes(b)
    with open(p, "rb") as f:
        return m.walk_wav(f)


def test_walk_canonical(tmp_path):
    fmt, off, sz = _walk_bytes(target_wav(2.0), tmp_path)
    assert fmt == m.TARGET_TUPLE
    assert off == 44
    assert sz == 2 * m.TARGET_RATE * m.BYTES_PER_SAMPLE


def test_walk_skips_metadata_before_data(tmp_path):
    """fact + LIST before data must not fool the walker (the externally-authored layout)."""
    data = b"\x00\x00\x00\x00"
    wav = make_wav([
        (b"fmt ", fmt_chunk()),
        (b"fact", struct.pack("<I", 1)),     # 4-byte body
        (b"LIST", b"INFOIART" + b"\x00" * 8),
        (b"data", data),
    ])
    fmt, off, sz = _walk_bytes(wav, tmp_path)
    assert fmt == m.TARGET_TUPLE
    assert off > 44                          # pushed past the canonical offset
    assert sz == len(data)


def test_walk_handles_odd_chunk_padding(tmp_path):
    """An odd-sized chunk carries a pad byte; the next chunk must still be found."""
    wav = make_wav([
        (b"fmt ", fmt_chunk()),
        (b"junk", b"\x01\x02\x03"),          # odd (3) -> 1 pad byte
        (b"data", b"\x00\x00\x00\x00"),
    ])
    fmt, off, sz = _walk_bytes(wav, tmp_path)
    assert fmt == m.TARGET_TUPLE and sz == 4


def test_walk_rejects_non_riff(tmp_path):
    assert _walk_bytes(b"NOPExxxxWAVE", tmp_path) == (None, None, None)


def test_walk_missing_data(tmp_path):
    fmt, off, sz = _walk_bytes(make_wav([(b"fmt ", fmt_chunk())]), tmp_path)
    assert fmt == m.TARGET_TUPLE and off is None and sz is None


def extensible_fmt_chunk(subformat=m.TARGET_FMT, ch=1, sr=m.TARGET_RATE, bits=m.TARGET_BITS):
    """A 40-byte WAVE_FORMAT_EXTENSIBLE `fmt ` body: tag 0xFFFE, real format in the SubFormat GUID.

    This is what ffmpeg emits for float WAVs, so it is the layout the tool meets in practice - not a
    hypothetical. Layout after the 16-byte WAVEFORMAT: cbSize(2) wValidBitsPerSample(2)
    dwChannelMask(4) SubFormat GUID(16), putting the real tag at body+24.
    """
    block_align = (bits // 8) * ch
    guid_tail = b"\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"   # KSDATAFORMAT_SUBTYPE_* tail
    return (struct.pack("<HHIIHH", 0xFFFE, ch, sr, sr * block_align, block_align, bits)
            + struct.pack("<HHI", 22, bits, 0x4)            # cbSize, wValidBitsPerSample, SPEAKER_FRONT_CENTER
            + struct.pack("<H", subformat) + b"\x00\x00" + guid_tail)


def test_walk_resolves_wave_format_extensible(tmp_path):
    """0xFFFE must resolve to the SubFormat GUID's tag, as WavStreamReader::begin does.

    Regression: walk_wav compared the raw tag, so ffmpeg's float output (0xFFFE) was reported as the
    wrong format and rejected - a file the firmware plays fine. Safe direction (never accepts a bad
    file), which is why it hid behind `--canonical` and the sox path.
    """
    wav = make_wav([(b"fmt ", extensible_fmt_chunk()), (b"data", b"\x00\x00\x00\x00")])
    fmt, off, sz = _walk_bytes(wav, tmp_path)
    assert fmt == m.TARGET_TUPLE, "extensible float32/mono/48k must read as the target tuple"
    assert off is not None and sz == 4


def test_walk_extensible_still_rejects_wrong_subformat(tmp_path):
    """Resolving the GUID must not become a rubber stamp: an extensible INT wav is still not float."""
    wav = make_wav([(b"fmt ", extensible_fmt_chunk(subformat=1)),   # PCM int, not IEEE float
                    (b"data", b"\x00\x00\x00\x00")])
    fmt, _, _ = _walk_bytes(wav, tmp_path)
    assert fmt == (1, 1, m.TARGET_RATE, m.TARGET_BITS)
    assert fmt != m.TARGET_TUPLE


# --- verify -----------------------------------------------------------------

def test_verify_ok(tmp_path):
    p = tmp_path / "ok.wav"
    p.write_bytes(target_wav(1.5))
    ok, detail = m.verify(str(p))
    assert ok and "48k" in detail and "1.5s" in detail


@pytest.mark.parametrize("fmt_kwargs", [
    {"ch": 2},          # stereo
    {"sr": 44100},      # wrong rate
    {"bits": 16},       # wrong depth
    {"af": 1},          # PCM int, not float
])
def test_verify_rejects_wrong_format(tmp_path, fmt_kwargs):
    wav = make_wav([(b"fmt ", fmt_chunk(**fmt_kwargs)), (b"data", b"\x00\x00\x00\x00")])
    p = tmp_path / "bad.wav"
    p.write_bytes(wav)
    ok, detail = m.verify(str(p))
    assert not ok and "float/mono/48k" in detail


def test_verify_rejects_missing_chunks(tmp_path):
    p = tmp_path / "nodata.wav"
    p.write_bytes(make_wav([(b"fmt ", fmt_chunk())]))
    ok, _ = m.verify(str(p))
    assert not ok


def test_wav_seconds(tmp_path):
    p = tmp_path / "dur.wav"
    p.write_bytes(target_wav(2.0))
    assert m.wav_seconds(str(p)) == pytest.approx(2.0)


# --- stub converter for orchestration tests ---------------------------------

class StubConverter(m.Converter):
    """Records calls and emits caller-supplied bytes; never shells out."""
    name = "stub"

    def __init__(self, raw=b"", wav_bytes=None, fail=False):
        self.raw = raw
        self.wav_bytes = wav_bytes if wav_bytes is not None else target_wav(1.0)
        self.fail = fail
        self.decode_calls = []
        self.write_calls = []

    def available(self):
        return True

    def decode_raw(self, src):
        self.decode_calls.append(src)
        if self.fail:
            raise subprocess.CalledProcessError(1, "stub")
        return self.raw

    def write_wav(self, src, dst):
        self.write_calls.append((src, dst))
        if self.fail:
            raise subprocess.CalledProcessError(1, "stub")
        with open(dst, "wb") as f:
            f.write(self.wav_bytes)


def opts(conv, **kw):
    return m.ConvertOptions(converter=conv, **kw)


# --- out_dir / engine layout ------------------------------------------------

def test_out_dir_plain():
    b = m.TapeAudioBatch(opts(StubConverter(), out="root"))
    assert b.out_dir == "root"
    assert b.max_seconds is None


def test_out_dir_engine_subdir():
    b = m.TapeAudioBatch(opts(StubConverter(), out="root", engine="shuttle"))
    assert b.out_dir == os.path.join("root", "shuttle")
    assert b.max_seconds == m.SHUTTLE_MAX_SECONDS


# --- target_name / slot logic -----------------------------------------------

def test_target_name_basename():
    b = m.TapeAudioBatch(opts(StubConverter()))
    assert b.target_name("/a/b/kick.aiff") == "kick.wav"


def test_target_name_deck_slots_advance():
    b = m.TapeAudioBatch(opts(StubConverter(), deck="a", start_slot=2))
    assert b.target_name("x.wav") == "tape_a_2.wav"
    assert b.target_name("y.wav") == "tape_a_3.wav"


def test_target_name_slot_exhaustion():
    b = m.TapeAudioBatch(opts(StubConverter(), deck="b", start_slot=m.SLOTS_PER_DECK))
    assert b.target_name("x.wav") == f"tape_b_{m.SLOTS_PER_DECK}.wav"
    assert b.target_name("y.wav") is None        # past slot 8


# --- gather_dir -------------------------------------------------------------

def test_gather_dir_filters_and_sorts(tmp_path):
    for fn in ["b.wav", "a.mp3", "c.flac", "notes.txt", "project.logicx"]:
        (tmp_path / fn).write_bytes(b"x")
    b = m.TapeAudioBatch(opts(StubConverter()))
    got = [os.path.basename(p) for p in b.gather_dir(str(tmp_path))]
    assert got == ["a.mp3", "b.wav", "c.flac"]


def test_gather_dir_non_recursive_skips_subdirs(tmp_path):
    (tmp_path / "top.wav").write_bytes(b"x")
    sub = tmp_path / "sub"
    sub.mkdir()
    (sub / "deep.wav").write_bytes(b"x")
    b = m.TapeAudioBatch(opts(StubConverter()))
    got = [os.path.basename(p) for p in b.gather_dir(str(tmp_path))]
    assert got == ["top.wav"]


def test_gather_dir_recursive(tmp_path):
    (tmp_path / "top.wav").write_bytes(b"x")
    sub = tmp_path / "sub"
    sub.mkdir()
    (sub / "deep.aiff").write_bytes(b"x")
    b = m.TapeAudioBatch(opts(StubConverter(), recursive=True))
    got = sorted(os.path.basename(p) for p in b.gather_dir(str(tmp_path)))
    assert got == ["deep.aiff", "top.wav"]


# --- convert_one: native vs canonical branch --------------------------------

def test_convert_one_native_uses_write_wav(tmp_path):
    conv = StubConverter(wav_bytes=target_wav(3.0))
    b = m.TapeAudioBatch(opts(conv))
    dst = str(tmp_path / "o.wav")
    seconds = b.convert_one("in.mp3", dst)
    assert conv.write_calls == [("in.mp3", dst)]
    assert conv.decode_calls == []
    assert seconds == pytest.approx(3.0)


def test_convert_one_canonical_prepends_header_and_trims_frames(tmp_path):
    # 9 raw bytes -> trimmed to 8 (two float frames); header makes a 44+8 byte file.
    conv = StubConverter(raw=b"\x00" * 9)
    b = m.TapeAudioBatch(opts(conv, canonical=True))
    dst = str(tmp_path / "o.wav")
    seconds = b.convert_one("in.mp3", dst)
    assert conv.decode_calls == ["in.mp3"]
    assert conv.write_calls == []
    raw = open(dst, "rb").read()
    assert len(raw) == 44 + 8                     # 9 trimmed to whole frames
    with open(dst, "rb") as f:
        fmt, off, sz = m.walk_wav(f)
    assert fmt == m.TARGET_TUPLE and off == 44 and sz == 8
    assert seconds == pytest.approx(8 / (m.TARGET_RATE * m.BYTES_PER_SAMPLE))


# --- run(): exit codes, warnings, failures ----------------------------------

def test_run_success(tmp_path):
    conv = StubConverter(wav_bytes=target_wav(1.0))
    b = m.TapeAudioBatch(opts(conv, out=str(tmp_path / "out")))
    rc = b.run(["a.mp3", "b.wav"])
    assert rc == 0
    assert os.path.isfile(tmp_path / "out" / "a.wav")
    assert os.path.isfile(tmp_path / "out" / "b.wav")


def test_run_decode_failure_sets_rc(tmp_path, capsys):
    conv = StubConverter(fail=True)
    b = m.TapeAudioBatch(opts(conv, out=str(tmp_path / "out")))
    rc = b.run(["a.mp3"])
    assert rc == 1
    assert "could not decode" in capsys.readouterr().err


def test_run_verify_failure_sets_rc(tmp_path, capsys):
    # Converter writes a non-conforming (stereo) WAV; verify must catch it.
    bad = make_wav([(b"fmt ", fmt_chunk(ch=2)), (b"data", b"\x00\x00\x00\x00")])
    conv = StubConverter(wav_bytes=bad)
    b = m.TapeAudioBatch(opts(conv, out=str(tmp_path / "out")))
    rc = b.run(["a.mp3"])
    assert rc == 1
    assert "FAIL" in capsys.readouterr().err


def test_run_no_verify_accepts_nonconforming(tmp_path):
    bad = make_wav([(b"fmt ", fmt_chunk(ch=2)), (b"data", b"\x00\x00\x00\x00")])
    conv = StubConverter(wav_bytes=bad)
    b = m.TapeAudioBatch(opts(conv, out=str(tmp_path / "out"), verify=False))
    assert b.run(["a.mp3"]) == 0


def test_run_shuttle_cap_warns_but_succeeds(tmp_path, capsys):
    conv = StubConverter(wav_bytes=target_wav(m.SHUTTLE_MAX_SECONDS + 5))
    b = m.TapeAudioBatch(opts(conv, out=str(tmp_path / "out"), engine="shuttle"))
    rc = b.run(["long.wav"])
    assert rc == 0                                # warning, not failure
    err = capsys.readouterr().err
    assert "WARN" in err and "RAM" in err


def test_run_slot_exhaustion_sets_rc(tmp_path, capsys):
    conv = StubConverter(wav_bytes=target_wav(1.0))
    b = m.TapeAudioBatch(opts(conv, out=str(tmp_path / "out"),
                              deck="a", start_slot=m.SLOTS_PER_DECK))
    rc = b.run(["one.wav", "two.wav"])            # second overflows slot 8
    assert rc == 1
    assert "slots/deck" in capsys.readouterr().err
    assert os.path.isfile(tmp_path / "out" / f"tape_a_{m.SLOTS_PER_DECK}.wav")


# --- end-to-end with a real converter (skipped if the tool is missing) ------

@pytest.mark.parametrize("tool", ["ffmpeg", "sox"])
@pytest.mark.parametrize("canonical", [False, True])
def test_end_to_end_real_tool(tmp_path, tool, canonical):
    if shutil.which(tool) is None:
        pytest.skip(f"{tool} not on PATH")
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg needed to synthesize the input tone")
    src = tmp_path / "tone.wav"
    subprocess.run(
        ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi",
         "-i", "sine=frequency=440:duration=1:sample_rate=44100", "-ac", "2", str(src)],
        check=True)
    conv = m.CONVERTERS[tool]
    b = m.TapeAudioBatch(opts(conv, out=str(tmp_path / "out"), canonical=canonical))
    assert b.run([str(src)]) == 0
    out = tmp_path / "out" / "tone.wav"
    ok, detail = m.verify(str(out))
    assert ok, detail
    with open(out, "rb") as f:
        fmt, off, _ = m.walk_wav(f)
    assert fmt == m.TARGET_TUPLE
    if canonical:
        assert off == 44
