"""Unit tests for the SD card tooling (card_layout, card_audio, sk_card).

Three kinds of test here, and the third is the point of the file:

1. **Unit** - the layout table, the WAV parser, the individual verify checks.
2. **Integration** - `init` produces a card that `verify` passes. These two are written against the
   same layout table, so this is not tautological only because they use it from opposite ends: init
   writes files, verify re-derives the rules and reads them back.
3. **Firmware parity** - assertions that the constants mirrored into `card_layout` still match the
   C++ they were copied from. Everything here is a hand-copied mirror of firmware behaviour, so the
   real failure mode is not a bug in this code, it is the firmware moving and this code not noticing.
   Those tests read the actual source files.

No hardware, no toolchain, no decoder: all stdlib. `make test-scripts` runs them.
"""

from pathlib import Path

import struct

import pytest

import card_audio as ca
import card_layout as cl
import sk_card

REPO = Path(__file__).resolve().parent.parent


# --- layout -------------------------------------------------------------------------------------


def test_every_bank_has_dirs_and_a_firmware_citation():
    for bank in cl.LAYOUT:
        assert bank.dirs, f"{bank.engine} declares no directories"
        assert bank.source, f"{bank.engine} has no firmware source citation"


def test_all_dirs_includes_parents_and_is_deduplicated():
    dirs = cl.all_dirs()
    assert "radio" in dirs and "radio/0" in dirs and "radio/15" in dirs
    assert len(dirs) == len(set(dirs))
    # A parent must precede its children, since init creates them in order.
    assert dirs.index("radio") < dirs.index("radio/0")


def test_bank_for_path_prefers_the_longest_match():
    # SK/ belongs to the platform entry, but SK/B is a granular tape folder. Longest match wins,
    # otherwise granular audio would be validated as platform config.
    assert cl.bank_for_path("SK").engine == "platform"
    assert cl.bank_for_path("SK/B").engine == "granular"
    assert cl.bank_for_path("radio/3").engine == "radio"
    assert cl.bank_for_path("nonsense") is None


@pytest.mark.parametrize("name,ok", [
    ("01.raw", True),
    ("BOOK01.WAV", True),
    ("track.wav", True),
    ("a" * 9 + ".wav", False),        # 13 chars - one over the limit
    ("a" * 8 + ".wav", True),         # 12 chars - exactly at the limit
    ("._01.raw", False),              # AppleDouble
    (".DS_Store", False),
    ("song.mp3", False),
    ("noextension", False),
])
def test_scan_name_rules(name, ok):
    assert cl.scan_name_ok(name) is ok


def test_raw_format_description_does_not_claim_a_wav_format_tag():
    # A headerless file has no AudioFormat field; saying "(WAV AudioFormat 1)" for one would be
    # actively misleading, since the absence of any self-description is that format's whole hazard.
    assert "AudioFormat" not in cl.TARGET_RADIO_RAW.describe()
    assert "AudioFormat" in cl.TARGET_TAPE.describe()


# --- WAV parsing --------------------------------------------------------------------------------


def test_wav_roundtrip_float(tmp_path):
    p = tmp_path / "a.wav"
    ca.write_wav(p, [0.0, 0.5, -0.5, 1.0], 48000, 1, ca.F32)
    info = ca.parse_wav(p)
    assert (info.encoding, info.channels, info.rate, info.frames) == (cl.F32, 1, 48000, 4)


def test_wav_roundtrip_int16_values_survive(tmp_path):
    p = tmp_path / "a.wav"
    ca.write_wav(p, [0.0, 0.5, -0.5], 24000, 1, ca.INT16)
    samples, info = ca.read_samples(p)
    assert info.encoding == cl.INT16 and info.rate == 24000
    assert samples[0] == 0.0
    assert samples[1] == pytest.approx(0.5, abs=1e-4)
    assert samples[2] == pytest.approx(-0.5, abs=1e-4)


def test_write_wav_emits_the_44_byte_header_the_device_writes(tmp_path):
    # src/memory/wav.h static_asserts sizeof(WavHeader) == 44 with a fixed BlocSize of 16. Matching it
    # keeps cards readable by firmware predating the chunk-walk fix.
    p = tmp_path / "a.wav"
    ca.write_wav(p, [0.0] * 8, 48000, 1, ca.F32)
    assert ca.parse_wav(p).data_offset == 44


def test_parser_walks_past_metadata_chunks(tmp_path):
    """A `LIST` chunk before `data` pushes the body past offset 44. External encoders do this, and the
    firmware now chunk-walks rather than assuming 44 - so the checker must too, or it would report
    perfectly good files as broken."""
    import struct
    body = struct.pack("<4f", 0.0, 0.1, 0.2, 0.3)
    fmt = b"fmt " + struct.pack("<IHHIIHH", 16, 3, 1, 48000, 48000 * 4, 4, 32)
    meta = b"LIST" + struct.pack("<I", 10) + b"INFOhello\x00"
    data = b"data" + struct.pack("<I", len(body)) + body
    payload = b"WAVE" + fmt + meta + data
    (tmp_path / "m.wav").write_bytes(b"RIFF" + struct.pack("<I", len(payload)) + payload)
    info = ca.parse_wav(tmp_path / "m.wav")
    assert info.data_offset > 44 and info.frames == 4 and info.encoding == cl.F32


def test_parser_unwraps_wave_format_extensible(tmp_path):
    """ffmpeg writes WAVE_FORMAT_EXTENSIBLE (0xFFFE) for some inputs, with the real tag inside the
    GUID. The firmware unwraps it (raw_stream.h:57-61); a checker that did not would report a valid
    file as an unknown format."""
    import struct
    body = struct.pack("<4h", 0, 100, -100, 0)
    ext = struct.pack("<HHI", 16, 1, 0) + b"\x01\x00" + b"\x00" * 14  # cbSize=22 payload, tag in GUID
    fmt = b"fmt " + struct.pack("<IHHIIHH", 40, 0xFFFE, 1, 48000, 96000, 2, 16) + ext
    data = b"data" + struct.pack("<I", len(body)) + body
    payload = b"WAVE" + fmt + data
    (tmp_path / "e.wav").write_bytes(b"RIFF" + struct.pack("<I", len(payload)) + payload)
    assert ca.parse_wav(tmp_path / "e.wav").encoding == cl.INT16


@pytest.mark.parametrize("blob,reason", [
    (b"", "too short"),
    (b"NOTARIFF" + b"\x00" * 40, "not RIFF"),
    (b"RIFF" + b"\x00\x00\x00\x00" + b"WAVE", "no data chunk"),
])
def test_parser_rejects_what_the_firmware_rejects(tmp_path, blob, reason):
    p = tmp_path / "bad.wav"
    p.write_bytes(blob)
    with pytest.raises(ca.WavError):
        ca.parse_wav(p)


def test_pad_to_bytes_clears_the_scan_floor():
    short = ca.tone(0.05, 440.0, 48000)
    padded = ca.pad_to_bytes(short, cl.SCAN_MIN_BYTES, 2)
    assert len(padded) * 2 >= cl.SCAN_MIN_BYTES
    assert len(short) * 2 < cl.SCAN_MIN_BYTES  # the fixture is genuinely too short to start with


def test_synthesis_is_deterministic():
    # The release zip's checksum depends on this: unseeded randomness would change the artifact on
    # every build.
    assert ca.noise_bed(0.1, 48000, seed=7) == ca.noise_bed(0.1, 48000, seed=7)
    assert ca.noise_bed(0.1, 48000, seed=7) != ca.noise_bed(0.1, 48000, seed=8)


# --- verify -------------------------------------------------------------------------------------


def _card(tmp_path) -> Path:
    root = tmp_path / "card"
    for d in cl.all_dirs():
        (root / d).mkdir(parents=True, exist_ok=True)
    return root


def _levels(findings, level):
    return [f for f in findings if f.level == level]


def test_verify_accepts_a_correct_tape_file(tmp_path):
    root = _card(tmp_path)
    ca.write_wav(root / "tapes/tape_a_1.wav", ca.tone(1.0, 220, 48000), 48000, 1, ca.F32)
    problems = [f for f in sk_card.verify_card(root) if f.path.startswith("tapes/")]
    assert problems == []


@pytest.mark.parametrize("rate,channels,encoding", [
    (48000, 2, ca.F32),      # stereo: downmixed to mono as it loads
    (48000, 1, ca.INT16),    # another depth: converted as it loads
    (48000, 2, ca.INT16),    # both at once
])
def test_verify_accepts_any_decodable_depth_or_channel_count(tmp_path, rate, channels, encoding):
    """Since the unified read path, these are correct files rather than findings: the device converts
    depth and channel count on the way to the ring. Only `convert`'s output format stayed narrow."""
    root = _card(tmp_path)
    ca.write_wav(root / "tapes/tape_a_1.wav", [0.0] * 2048 * channels, rate, channels, encoding)
    problems = [f for f in sk_card.verify_card(root) if f.path.startswith("tapes/")]
    assert problems == [], [f.problem for f in problems]


def test_verify_accepts_an_off_rate_file_now_that_the_loader_resamples(tmp_path):
    # 44.1 kHz was the single most common way to get a card wrong. It is resampled on the way in now,
    # to the device rate, so the buffer still holds the same number of frames per second as a take
    # recorded on the device - which is why nothing downstream (loop length, tempo) had to change.
    root = _card(tmp_path)
    ca.write_wav(root / "tapes/tape_a_1.wav", [0.0] * 2048, 44100, 1, ca.F32)
    problems = [f for f in sk_card.verify_card(root) if f.path.startswith("tapes/")]
    assert problems == [], [f.problem for f in problems]


def test_verify_flags_a_rate_outside_the_resampler_bounds(tmp_path):
    root = _card(tmp_path)
    ca.write_wav(root / "tapes/tape_a_1.wav", [0.0] * 2048, 1000, 1, ca.F32)
    errors = _levels(sk_card.verify_card(root), "error")
    assert any("outside the" in f.problem for f in errors), [f.problem for f in errors]


def test_verify_flags_an_encoding_the_firmware_cannot_decode(tmp_path):
    # 64-bit float is a legal WAV the device has no decoder for; it must read as an error, not as
    # something that will merely be converted.
    root = _card(tmp_path)
    body = b"\0" * 4096
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
           + b"fmt " + struct.pack("<IHHIIHH", 16, 3, 1, 48000, 48000 * 8, 8, 64)
           + b"data" + struct.pack("<I", len(body)))
    (root / "tapes/tape_a_1.wav").write_bytes(hdr + body)
    errors = _levels(sk_card.verify_card(root), "error")
    assert any("cannot decode" in f.problem for f in errors), [f.problem for f in errors]


def test_verify_flags_a_name_too_long_for_the_scan(tmp_path):
    root = _card(tmp_path)
    long_name = "The Hobbit Chapter 3.wav"
    ca.write_wav(root / "bard/0" / long_name,
                 ca.pad_to_bytes([0.0] * 100, cl.SCAN_MIN_BYTES, 2), 24000, 1, ca.INT16)
    errors = _levels(sk_card.verify_card(root), "error")
    assert any("INVISIBLE" in f.problem and "characters" in f.problem for f in errors)
    # and the suggested replacement must actually be legal, not naive truncation
    hint = next(f.fix for f in errors if "characters" in f.problem)
    suggestion = hint.split("(e.g. ")[1].split(")")[0]
    assert cl.scan_name_ok(suggestion), suggestion


def test_verify_flags_a_file_under_the_scan_floor(tmp_path):
    root = _card(tmp_path)
    ca.write_wav(root / "pstretch/TINY.WAV", ca.tone(0.05, 220, 48000), 48000, 1, ca.INT16)
    errors = _levels(sk_card.verify_card(root), "error")
    assert any("INVISIBLE" in f.problem and "KB" in f.problem for f in errors)


def test_verify_identifies_appledouble_stubs(tmp_path):
    root = _card(tmp_path)
    (root / "radio/0/._01.raw").write_bytes(b"\x00" * 4096)
    warns = _levels(sk_card.verify_card(root), "warn")
    assert any("dot" in f.problem for f in warns)
    assert any("dot_clean" in f.fix for f in warns)


def test_verify_tells_you_to_convert_an_mp3_rather_than_listing_slot_names(tmp_path):
    root = _card(tmp_path)
    (root / "tapes/song.mp3").write_bytes(b"ID3\x04\x00\x00" + b"\x00" * 40000)
    errors = _levels(sk_card.verify_card(root), "error")
    assert any("no decoder" in f.problem and "convert" in f.fix for f in errors)


def test_verify_flags_key_equals_value_config(tmp_path):
    root = _card(tmp_path)
    (root / "SK/config.txt").write_text("pre_load=1\n")
    errors = _levels(sk_card.verify_card(root), "error")
    assert any("separate lines" in f.problem for f in errors)


def test_verify_accepts_the_documented_config(tmp_path):
    root = _card(tmp_path)
    (root / "SK/config.txt").write_text(cl.DEFAULT_CONFIG)
    assert [f for f in sk_card.verify_card(root) if f.path == "SK/config.txt"] == []


def test_verify_flags_out_of_range_config_values(tmp_path):
    root = _card(tmp_path)
    (root / "SK/config.txt").write_text("mid_ch_a\n99\n")
    errors = _levels(sk_card.verify_card(root), "error")
    assert any("1..16" in f.problem for f in errors)


def test_verify_reports_a_missing_card_root(tmp_path):
    findings = sk_card.verify_card(tmp_path / "nope")
    assert findings and findings[0].level == "error"


# --- init / integration -------------------------------------------------------------------------


def test_init_skeleton_passes_verify(tmp_path):
    root = tmp_path / "card"
    sk_card.build_card(root, demo=False, quiet=True)
    errors = _levels(sk_card.verify_card(root), "error")
    assert errors == [], [f.problem for f in errors]


def test_init_with_demo_audio_passes_verify(tmp_path):
    """The one that matters: every synthesized demo file must satisfy the same rules the checker
    enforces. A base card that its own verifier rejects would be worse than shipping nothing."""
    root = tmp_path / "card"
    sk_card.build_card(root, demo=True, quiet=True)
    errors = _levels(sk_card.verify_card(root), "error")
    assert errors == [], [f"{f.path}: {f.problem}" for f in errors]


def test_init_creates_every_folder_with_a_readme(tmp_path):
    root = tmp_path / "card"
    sk_card.build_card(root, demo=False, quiet=True)
    for d in cl.all_dirs():
        assert (root / d).is_dir(), d
    for bank in cl.LAYOUT:
        for d in bank.dirs:
            assert (root / d / "README.TXT").exists(), d


def test_readmes_state_the_scan_rules_where_they_apply(tmp_path):
    root = tmp_path / "card"
    sk_card.build_card(root, demo=False, quiet=True)
    scanned = (root / "radio/0/README.TXT").read_text()
    assert "12 characters" in scanned and "32 KB" in scanned
    # A slot bank has no scan, so it must not claim rules that do not apply to it.
    slots = (root / "tapes/README.TXT").read_text()
    assert "INVISIBLE" not in slots
    assert "tape_a_1.wav" in slots


def test_init_copies_the_repo_example_patches(tmp_path):
    root = tmp_path / "card"
    sk_card.build_card(root, demo=False, quiet=True)
    if (REPO / "examples/chuck").is_dir():
        assert (root / "chuck/0.ck").exists()
    if (REPO / "examples/csound").is_dir():
        assert (root / "csound/0.csd").exists()


def test_demo_audio_matches_each_engines_declared_format(tmp_path):
    root = tmp_path / "card"
    sk_card.build_card(root, demo=True, quiet=True)
    checked = 0
    for bank in cl.LAYOUT:
        if bank.fmt.container != cl.WAV:
            continue
        for d in bank.dirs:
            for f in sorted((root / d).glob("*")):
                if f.suffix.upper() != ".WAV":
                    continue
                info = ca.parse_wav(f)
                assert info.encoding in bank.fmt.encodings, f
                if bank.fmt.channels:
                    assert info.channels == bank.fmt.channels, f
                if bank.fmt.rate:
                    assert info.rate == bank.fmt.rate, f
                checked += 1
    assert checked > 0


# --- convert backends ---------------------------------------------------------------------------


def test_backend_registry_order_prefers_cysox_then_ffmpeg():
    assert sk_card.BACKEND_ORDER[0] == "cysox"
    assert "ffmpeg" in sk_card.BACKEND_ORDER


def test_unknown_backend_is_rejected():
    with pytest.raises(KeyError):
        sk_card.available_backends("madeup")


def test_decode_any_falls_back_when_a_backend_cannot_read_the_format(tmp_path):
    """cysox is preferred but libsox's format support is a build-time property - mp3/flac are commonly
    missing. The chain must fall through silently rather than failing the file."""
    class NoHandler(sk_card.Backend):
        name = "nohandler"

        def available(self):
            return True

        def supports(self, src):
            return False

        def decode(self, src, rate, channels):
            raise AssertionError("must not be called when supports() is False")

    class Works(sk_card.Backend):
        name = "works"

        def available(self):
            return True

        def decode(self, src, rate, channels):
            return [0.25, 0.5]

    samples, used = sk_card.decode_any([NoHandler(), Works()], tmp_path / "x.mp3", 48000, 1)
    assert samples == [0.25, 0.5] and used == "works"


def test_decode_any_falls_back_when_a_backend_raises(tmp_path):
    class Explodes(sk_card.Backend):
        name = "explodes"

        def available(self):
            return True

        def decode(self, src, rate, channels):
            raise RuntimeError("boom")

    class Works(sk_card.Backend):
        name = "works"

        def available(self):
            return True

        def decode(self, src, rate, channels):
            return [1.0]

    samples, used = sk_card.decode_any([Explodes(), Works()], tmp_path / "x.wav", 48000, 1)
    assert used == "works" and samples == [1.0]


def test_decode_any_reports_every_backend_that_failed(tmp_path):
    class Explodes(sk_card.Backend):
        name = "explodes"

        def available(self):
            return True

        def decode(self, src, rate, channels):
            raise RuntimeError("boom")

    with pytest.raises(RuntimeError, match="boom"):
        sk_card.decode_any([Explodes()], tmp_path / "x.wav", 48000, 1)


# --- firmware parity ----------------------------------------------------------------------------
#
# card_layout mirrors constants out of C++ by hand. These tests fail when the firmware moves, which is
# the only way this tooling silently goes wrong: it would keep validating cards against rules the
# device no longer has.


def _src(rel: str) -> str:
    return (REPO / rel).read_text()


@pytest.mark.skipif(not (REPO / "src/hw/stream_deck.cpp").exists(), reason="firmware source absent")
def test_scan_floor_matches_firmware():
    text = _src("src/hw/stream_deck.cpp")
    assert "kMinStationBytes = 32u * 1024u" in text, \
        "kMinStationBytes changed in stream_deck.cpp - update SCAN_MIN_BYTES in card_layout.py"
    assert cl.SCAN_MIN_BYTES == 32 * 1024


@pytest.mark.skipif(not (REPO / "src/hw/stream_deck.cpp").exists(), reason="firmware source absent")
def test_scan_name_limit_matches_firmware():
    text = _src("src/hw/stream_deck.cpp")
    assert f"len > {cl.SCAN_MAX_NAME}" in text, \
        "the scan's filename length limit changed - update SCAN_MAX_NAME in card_layout.py"


@pytest.mark.skipif(not (REPO / "src/memory/raw_stream.h").exists(), reason="firmware source absent")
def test_scanned_banks_accept_any_decodable_pcm_per_firmware():
    """The scanned path's .wav side goes through the shared adapter, so its gate is `pcm_format_of`
    plus the channel bound - not a fixed 16-bit-mono test. Its .raw side is still int16 mono by
    convention, because a headerless file states nothing about itself."""
    text = _src("src/memory/raw_stream.h")
    assert "pcm_format_of(info.audio_format, info.bits_per_sample, fmt)" in text, \
        "raw_stream.h's accepted format changed - update ACCEPT_SCANNED in card_layout.py"
    assert "info.channels > kMaxChannels" in text, \
        "raw_stream.h's channel bound changed - update MAX_CHANNELS in card_layout.py"
    assert "kRawFrameFormat   = PcmFormat::i16" in text, \
        "the raw path's fixed format changed - update TARGET_RADIO_RAW in card_layout.py"
    acc = cl.BANKS["radio"].accepts
    assert acc.rate is cl.ANY_RATE and set(acc.encodings) == set(cl.DECODABLE)
    assert cl.TARGET_SCAN_WAV.channels == 1 and cl.INT16 in cl.TARGET_SCAN_WAV.encodings


@pytest.mark.skipif(not (REPO / "src/memory/wav_source.h").exists(), reason="firmware source absent")
def test_chunk_walk_bound_matches_firmware():
    # One walk for the whole firmware now (src/memory/wav_source.h), so there is one bound to mirror.
    assert "kWavMaxChunks = 64" in _src("src/memory/wav_source.h"), \
        "the WAV chunk-walk bound changed - update MAX_CHUNKS in card_audio.py"
    assert ca.MAX_CHUNKS == 64


@pytest.mark.skipif(not (REPO / "src/hw/card.cpp").exists(), reason="firmware source absent")
def test_granular_accepts_any_decodable_pcm_at_48k_per_firmware():
    """granular's load path converts depth, channel count AND rate into its stereo buffer, so its gate
    is `pcm_format_of` plus the channel bound and a sanity range on the rate - nothing is pinned."""
    text = _src("src/hw/card.cpp")
    assert "pcm_format_of(info.audio_format, info.bits_per_sample, src_fmt)" in text
    assert "info.channels >= 1 && info.channels <= kPcmMaxChannels" in text
    assert "info.sample_rate >= kMinRate && info.sample_rate <= kMaxRate" in text
    acc = cl.BANKS["granular"].accepts
    assert acc.rate is cl.ANY_RATE and set(acc.encodings) == set(cl.DECODABLE)
    assert acc.max_channels == cl.MAX_CHANNELS
    # What `convert` still writes: the buffer's own stereo shape, so it loads with no fold.
    assert cl.TARGET_GRANULAR.channels == 2 and cl.TARGET_GRANULAR.rate == 48000


@pytest.mark.skipif(not (REPO / "src/memory/wav.h").exists(), reason="firmware source absent")
def test_device_writes_a_44_byte_header():
    assert 'static_assert(sizeof(header) == 44' in _src("src/memory/wav.h"), \
        "the device's WAV header size changed - revisit write_wav in card_audio.py"


@pytest.mark.parametrize("engine,rel,literal", [
    ("chuck", "src/engine/chuck/chuck_patch.h", '"chuck/%d.ck"'),
    ("csound", "src/engine/csound/csound_patch.h", '"csound/%d.csd"'),
    ("tape", "src/engine/tape/tape_engine.cpp", '"tapes/tape_"'),
    ("shuttle", "src/engine/shuttle/shuttle_engine.cpp", '"shuttle/tape_"'),
    ("softcut", "src/engine/softcut/softcut_engine.cpp", '"softcut/loop_"'),
    ("radio", "src/engine/radio/radio_engine.cpp", '"radio/"'),
])
def test_engine_paths_match_firmware_literals(engine, rel, literal):
    path = REPO / rel
    if not path.exists():
        pytest.skip(f"{rel} absent")
    assert literal in path.read_text(), \
        f"{engine}'s card path changed in {rel} - update LAYOUT in card_layout.py"


@pytest.mark.skipif(not (REPO / "src/engine/softcut/softcut_engine.h").exists(),
                    reason="firmware source absent")
def test_softcut_slot_count_and_buffer_match_firmware():
    """softcut adds no new format - it is TARGET_TAPE in a different folder - so what has to be mirrored
    is the slot count and the RAM cap, both of which are silent when wrong: too many slots and `init`
    writes files no engine opens; a wrong cap and `verify` mis-warns about loop length."""
    text = _src("src/engine/softcut/softcut_engine.h")
    assert "kTapeSlots  = 8" in text, \
        "softcut's slot count changed - update its slots in card_layout.py"
    assert "kBufFrames  = 1u << 19" in text, \
        "softcut's loop buffer changed - update max_seconds in card_layout.py"
    bank = cl.BANKS["softcut"]
    assert len(bank.slots) == 8 * 2, "8 slots per deck, two decks"
    # 1<<19 frames at the platform's 48 kHz, which is what the buffer holds.
    assert bank.max_seconds == pytest.approx((1 << 19) / 48000.0, abs=0.05)


def test_softcut_reuses_the_tape_format_rather_than_declaring_its_own():
    # The three writable streaming banks share one target Fmt on purpose: same 48k mono float WAV through the
    # same StreamDeck path. Only the folder and the filename prefix differ, which is what keeps a
    # softcut loop from overwriting a tape take.
    assert cl.BANKS["softcut"].fmt is cl.TARGET_TAPE is cl.BANKS["tape"].fmt is cl.BANKS["shuttle"].fmt
    dirs = {cl.BANKS[e].dirs[0] for e in ("tape", "shuttle", "softcut")}
    assert len(dirs) == 3, "they must not share a folder, or their slots collide"


# The two ways an engine can reach the card. Both must be searched: matching only the streaming
# service would miss granular and graincloud, which go through the platform's own save/load path.
CARD_ACCESS_PATTERNS = (
    r"ctx\.stream|_stream->|scan_bank|IStreamDeck",          # the streaming service (stream_deck.cpp)
    r"audio_apply_loaded|audio_capacity_bytes|audio_raw_bytes",  # the platform tape/slot path (hw/card.cpp)
)

def test_every_card_reading_engine_has_a_bank():
    """The gap this exists to catch: softcut shipped in every release while `card_layout` had no entry
    for it, so `init` never created its folder and `verify` ignored its files entirely - silently, in
    both front-ends.

    Derived from the source rather than a hardcoded list, so a NEW card-reading engine fails here
    instead of being quietly unsupported. An engine that legitimately reads another bank's folders is
    declared in that bank's `also_read_by`, which makes the sharing an explicit, reviewable claim
    rather than an absence nobody notices.
    """
    import re
    engines = REPO / "src/engine"
    if not engines.is_dir():
        pytest.skip("firmware source absent")
    pattern = re.compile("|".join(CARD_ACCESS_PATTERNS))
    uses_card = set()
    for d in sorted(p for p in engines.iterdir() if p.is_dir()):
        for f in d.rglob("*"):
            if f.suffix not in (".cpp", ".h") or "vendor" in f.parts:
                continue
            if pattern.search(f.read_text(errors="replace")):
                uses_card.add(d.name)
                break
    declared = {e for bank in cl.LAYOUT for e in bank.readers}
    missing = uses_card - declared
    assert not missing, (
        f"these engines read the SD card but appear in no Bank in card_layout.py: {sorted(missing)}. "
        f"Both sk_card.py and web/ are blind to them - add a Bank, or add them to an existing bank's "
        f"`also_read_by` if they reuse its folders.")
    # The sharing claims must be real, or `also_read_by` becomes a way to silence the check.
    for bank in cl.LAYOUT:
        for engine in bank.also_read_by:
            assert engine in uses_card, (
                f"{engine} is listed in {bank.engine}'s also_read_by but no longer reads the card")


def test_the_shared_tape_store_is_recorded_as_shared():
    """The fact that cost a design decision: `SK/{B,G,P,R,T,Y}` looks like granular's folder and is
    actually the PLATFORM's, shared by every engine with CapTapeStorage. Renaming it to `granular/`
    would therefore have broken graincloud - which ships - and every card the stock upstream firmware
    ever wrote. Pinned so the sharing is not rediscovered the hard way."""
    assert cl.BANKS["granular"].readers == ("granular", "graincloud")
    src = REPO / "src/memory/storage.cpp"
    if not src.exists():
        pytest.skip("firmware source absent")
    assert 'kRootDir = "SK"' in src.read_text(), \
        "the platform tape store's root folder moved - update granular's dirs in card_layout.py"
    for engine in cl.BANKS["granular"].readers:
        impl = REPO / "src/engine" / engine
        if impl.is_dir():
            assert any("CapTapeStorage" in f.read_text(errors="replace")
                       for f in impl.rglob("*.cpp")), \
                f"{engine} no longer declares CapTapeStorage, so it no longer uses SK/"


def test_the_card_access_patterns_actually_match_the_known_readers():
    """Guards the guard. If the firmware renames these APIs, the search above silently matches nothing
    and the test above passes while checking nothing at all."""
    import re
    pattern = re.compile("|".join(CARD_ACCESS_PATTERNS))
    for engine in ("tape", "softcut", "radio", "granular", "graincloud"):
        d = REPO / "src/engine" / engine
        if not d.is_dir():
            pytest.skip(f"{engine} source absent")
        hit = any(pattern.search(f.read_text(errors="replace"))
                  for f in d.rglob("*")
                  if f.suffix in (".cpp", ".h") and "vendor" not in f.parts)
        assert hit, f"the card-access search no longer matches {engine} - the patterns have gone stale"


@pytest.mark.parametrize("engine,rel,literal", [
    ("radio", "src/engine/radio/radio_engine.h", "kMaxStations = 48"),
    ("bard", "src/engine/bard/bard_engine.h", "kMaxBooks   = 32"),
    ("pstretch", "src/engine/pstretch/pstretch_engine.h", "kMaxClips = 32"),
])
def test_bank_capacities_match_firmware(engine, rel, literal):
    path = REPO / rel
    if not path.exists():
        pytest.skip(f"{rel} absent")
    assert literal in path.read_text(), \
        f"{engine}'s bank capacity changed - update max_files in card_layout.py"
    assert cl.BANKS[engine].max_files == int(literal.split("=")[1])
