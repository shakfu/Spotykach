"""Device-free tests for the OSC codec and the host-side semantic tier.

No hardware, no serial port, no pyserial: this covers ``skdev.osc`` (the OSC wire
format + SLIP framing) and ``skdev.semantic`` (the generated semantic address space)
against a describe bundle produced by the REAL firmware code path.

That bundle is written by ``host/test_terminal_osc.cpp`` to
``host/build/describe_osc_sample.bin``, so these tests read firmware bytes rather
than a hand-written fixture that could drift. Run ``make -C host test-terminal-osc``
first; without the sample the descriptor tests skip cleanly.

The property that matters most here is the one the spec names: for every row in a
captured descriptor, **semantic -> generic -> semantic is the identity**, and every
generated semantic address resolves to exactly one generic address.
"""

import os
import struct

import pytest

from skdev import osc, semantic

SAMPLE = os.path.join(os.path.dirname(__file__), "..", "host", "build",
                      "describe_osc_sample.bin")


# --- SLIP --------------------------------------------------------------------------

def test_slip_roundtrip_escapes_both_special_bytes():
    payload = bytes([0x01, osc.END, 0x02, osc.ESC, 0x03, osc.END, osc.ESC])
    wire = osc.slip_encode(payload)
    assert wire[0] == osc.END and wire[-1] == osc.END
    assert osc.END not in wire[1:-1]        # every interior END was escaped
    assert osc.SlipDecoder().feed(wire) == [payload]


def test_slip_decoder_is_incremental_and_skips_empty_frames():
    dec = osc.SlipDecoder()
    wire = osc.slip_encode(b"abcd") + osc.slip_encode(b"efgh")
    got = []
    for i in range(len(wire)):              # one byte at a time, as the serial port delivers them
        got += dec.feed(wire[i:i + 1])
    assert got == [b"abcd", b"efgh"]
    # Back-to-back ENDs are legal padding, not a zero-length packet.
    assert osc.SlipDecoder().feed(bytes([osc.END, osc.END, osc.END])) == []


# --- OSC wire ----------------------------------------------------------------------

def test_encode_is_big_endian_and_padded():
    pkt = osc.encode("/sk/a/param/speed", 0.5)
    assert len(pkt) % 4 == 0
    assert pkt.startswith(b"/sk/a/param/speed\0")
    assert b",f" in pkt
    assert pkt[-4:] == struct.pack(">f", 0.5)


def test_read_form_has_no_typetag_string():
    """No arguments = a read. It must be the bare address, not an empty tag string."""
    pkt = osc.encode("/sk/a/param/speed")
    assert pkt == b"/sk/a/param/speed\0" + b"\0" * 2
    assert osc.decode(pkt) == ("/sk/a/param/speed", [])


@pytest.mark.parametrize("args", [
    (0.5,), (3,), ("deck-a",), (True,), (False,), (144, 60, 100), (0.25, True),
])
def test_encode_decode_roundtrip(args):
    addr, got = osc.decode(osc.encode("/sk/x", *args))
    assert addr == "/sk/x"
    assert len(got) == len(args)
    for g, a in zip(got, args):
        assert g == pytest.approx(a) if isinstance(a, float) else g == a


def test_bool_is_typed_before_int():
    """bool is a subclass of int in Python; T/F must win or every trigger becomes ,i 1."""
    assert b",T" in osc.encode("/sk/a/gate", True)
    assert b",i" in osc.encode("/sk/x", 1)


# --- the describe bundle, from real firmware bytes ---------------------------------

@pytest.fixture(scope="module")
def bundle():
    if not os.path.exists(SAMPLE):
        pytest.skip("run `make -C host test-terminal-osc` to produce {}".format(SAMPLE))
    with open(SAMPLE, "rb") as f:
        return f.read()


def test_sample_is_one_bundle(bundle):
    """The descriptor is ONE bundle - a host receives it atomically or not at all."""
    assert osc.is_bundle(bundle)


def test_bundle_rows_decode(bundle):
    pairs = osc.decode_packet(bundle)
    addrs = [a for a, _ in pairs]
    assert addrs.count("/sk/reply/dev/describe") == 1
    assert addrs.count("/sk/reply/dev/describe/caps") == 1
    assert any(a == "/sk/reply/dev/describe/param" for a in addrs)
    assert any(a == "/sk/reply/dev/describe/state" for a in addrs)


def test_param_rows_carry_address_label_range_scope(bundle):
    rows = [args for a, args in osc.decode_packet(bundle)
            if a == "/sk/reply/dev/describe/param"]
    assert rows
    for addr, label, lo, hi, scope in rows:
        assert addr.startswith("/sk/")
        assert "/param/" in addr
        assert label                        # never empty: falls back to the slot name
        assert (lo, hi) == (0.0, 1.0)       # every addressable param is normalized
        assert scope in ("deck", "global")
        # Scope is encoded STRUCTURALLY: a global param carries no deck segment.
        assert (scope == "deck") == (addr.split("/")[2] in ("a", "b"))


# --- the semantic tier -------------------------------------------------------------

def test_slugify():
    assert semantic.slugify("station select") == "station-select"
    assert semantic.slugify("  Character  ") == "character"
    assert semantic.slugify("wet/dry") == "wet-dry"
    assert semantic.slugify("A+B!") == "ab"


def test_translator_uses_the_engine_label(bundle):
    t = semantic.build(osc.decode_packet(bundle))
    # The mock engine labels Speed as "station" - the radio case from the spec.
    assert t.to_semantic["/sk/a/param/speed"].endswith("/a/station")
    assert t.generic(t.to_semantic["/sk/a/param/speed"]) == "/sk/a/param/speed"


def test_semantic_generic_semantic_is_the_identity(bundle):
    """The property the spec makes the translator's acceptance criterion."""
    t = semantic.build(osc.decode_packet(bundle))
    assert t.to_generic, "the translator produced no addresses at all"
    for sem, gen in t.to_generic.items():
        assert t.generic(sem) == gen
        assert t.semantic(gen) == sem


def test_every_semantic_address_resolves_to_exactly_one_generic(bundle):
    t = semantic.build(osc.decode_packet(bundle))
    assert len(set(t.to_generic.values())) == len(t.to_generic)


def test_replies_translate_back(bundle):
    """A reply arrives on /sk/reply/<path>; the patch must see its own namespace."""
    t = semantic.build(osc.decode_packet(bundle))
    assert t.semantic("/sk/reply/a/param/speed") == t.to_semantic["/sk/a/param/speed"]


def test_unknown_addresses_pass_through_untranslated(bundle):
    """A bug in the translator must never be able to make the device unreachable."""
    t = semantic.build(osc.decode_packet(bundle))
    assert t.generic("/sk/a/param/fluxfb") == "/sk/a/param/fluxfb"
    assert t.generic("/sk/dev/describe") == "/sk/dev/describe"


def test_colliding_labels_are_disambiguated_deterministically():
    """Two slots with the same label get a slot-name suffix, so a saved patch stays valid."""
    pairs = [
        ("/sk/reply/dev/describe", ["tape", "1.0", "masked=1"]),
        ("/sk/reply/dev/describe/param", ["/sk/a/param/mix", "level", 0.0, 1.0, "deck"]),
        ("/sk/reply/dev/describe/param", ["/sk/a/param/gritmix", "level", 0.0, 1.0, "deck"]),
    ]
    t = semantic.build(pairs)
    assert set(t.to_generic) == {"/tape/a/level-mix", "/tape/a/level-gritmix"}
    # Deterministic: rebuilding from the same descriptor yields the same addresses.
    assert semantic.build(pairs).to_generic == t.to_generic


def test_no_labels_degrades_to_the_generic_tier():
    """An engine implementing no labels is degraded, never broken."""
    pairs = [
        ("/sk/reply/dev/describe", ["delay", "1.0", "masked=1"]),
        ("/sk/reply/dev/describe/param", ["/sk/a/param/feedback", "feedback", 0.0, 1.0, "deck"]),
        ("/sk/reply/dev/describe/param", ["/sk/param/crossfade", "crossfade", 0.0, 1.0, "global"]),
    ]
    t = semantic.build(pairs)
    assert t.to_generic["/delay/a/feedback"] == "/sk/a/param/feedback"
    assert t.to_generic["/delay/crossfade"] == "/sk/param/crossfade"


def test_cfg_and_state_keep_their_kind_segment():
    """mix/route collide as NAMES, and a label can collide the same way."""
    pairs = [
        ("/sk/reply/dev/describe", ["tape", "1.0", "masked=1"]),
        ("/sk/reply/dev/describe/cfg", ["/sk/a/cfg/mode", "mode", "0:slice 1:reel"]),
        ("/sk/reply/dev/describe/state", ["/sk/state/mix", "mix", "float"]),
    ]
    t = semantic.build(pairs)
    assert "/tape/a/cfg/mode" in t.to_generic
    assert "/tape/state/mix" in t.to_generic
