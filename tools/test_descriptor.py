"""test_descriptor.py - check the host `describe` parser against real firmware output.

Unlike the rest of this directory, this test needs **no device**: it parses the
exact byte block the firmware's `verb_describe` produces, captured by the
off-target C++ test (`host/test_terminal.cpp`, which writes
`host/build/describe_sample.txt` when it runs). That closes the loop between the
two halves of the protocol - a firmware change to the descriptor format fails
here rather than silently breaking a host sweep on the bench.

Run `make test` (which runs the C++ side) before `make test-hw` to refresh the
sample; the test skips if it has never been generated.
"""

import pathlib

import pytest

from skdev.descriptor import parse_describe

SAMPLE = pathlib.Path(__file__).resolve().parent.parent / "host" / "build" / "describe_sample.txt"


@pytest.fixture(scope="module")
def descr():
    if not SAMPLE.exists():
        pytest.skip("no describe sample; run `make -C host test-terminal` to generate it")
    raw = SAMPLE.read_text().splitlines()
    # Device.describe() reads until the bare `end` terminator and does not pass it on.
    assert "end" in raw, "the firmware block must be terminated by `end`"
    return parse_describe(raw[: raw.index("end")])


def test_identity(descr):
    assert descr.engine, "descr line yields an engine name"
    assert descr.version, "descr line yields a version"


def test_params_parse_with_scope_and_range(descr):
    assert descr.params, "the block declares parameters"
    for p in descr.params.values():
        assert p.scope in ("deck", "global"), "{} has a valid scope".format(p.name)
        assert p.hi > p.lo, "{} declares an ordered range".format(p.name)

    # A normalized per-deck param and a global one, both of which a generic sweep relies on.
    assert descr.params["size"].scope == "deck"
    assert (descr.params["size"].lo, descr.params["size"].hi) == (0.0, 1.0)
    assert descr.params["crossfade"].scope == "global"


def test_configs_parse_into_label_maps(descr):
    assert descr.configs, "the block declares configs"
    mode = descr.configs["mode"]
    assert mode.values == {0: "slice", 1: "reel", 2: "drift"}
    for c in descr.configs.values():
        assert c.values, "{} carries at least one int:label pair".format(c.name)
        assert all(isinstance(k, int) for k in c.values), "{} keys are ints".format(c.name)


def test_queries_and_caps(descr):
    for name in ("empty", "mix", "route", "gateout"):
        assert name in descr.queries, "the platform query vocabulary includes {}".format(name)
    assert descr.caps > 0, "caps parses as a non-zero bitmask"


def test_round_trip_is_lossless_for_known_tags(descr):
    """Everything the firmware emits should land somewhere in the model.

    Guards against a new line tag being added on the device and silently dropped
    by the parser - the failure mode that makes a host sweep quietly incomplete.
    """
    raw = SAMPLE.read_text().splitlines()
    counted = sum(
        1
        for ln in raw
        if ln.split() and ln.split()[0] in ("descr", "param", "config", "query", "caps", "end")
    )
    assert counted == len([ln for ln in raw if ln.strip()]), (
        "an unrecognized tag appeared in describe output; teach parse_describe about it"
    )


def test_masked_flag_is_parsed(descr):
    """The generic sweep gates on this, so a parse failure would silently disable it.

    The sample is generated from a mock engine on the all-live default, so masked
    is False here - the assertion that matters is that the field parsed at all.
    """
    assert isinstance(descr.masked, bool)


def test_platform_owned_params_are_not_advertised(descr):
    """Tempo/KeyInterval/ModSpeed never reach IEngine::set_param.

    Advertising them made the generic sweep set values that went nowhere and then
    assert on whatever the engine happened to store. See param_is_platform_owned().
    """
    for name in ("tempo", "keyinterval", "modspeed"):
        assert name not in descr.params, "{} must not be advertised".format(name)


def test_every_advertised_param_is_normalized(descr):
    """With the platform-owned ids gone, the engine surface is uniformly 0..1.

    A non-normalized range would mean an id whose set_param units differ from its
    declared ones - exactly the defect that made tempo unusable.
    """
    for p in descr.params.values():
        assert (p.lo, p.hi) == (0.0, 1.0), "{} declares {}..{}".format(p.name, p.lo, p.hi)
