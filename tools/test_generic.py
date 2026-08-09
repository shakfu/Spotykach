"""test_generic.py - cross-engine parameter sweep driven by ``describe``.

The payoff of a describe-driven client: this one file tests every engine build.
It is parametrized at collection time from a single ``describe`` (open, describe,
close), so each declared parameter becomes its own test case.

Because ``describe`` lists only params the engine's ``live_params()`` mask marks
live, this sweep never sets an ignored param - a read-back mismatch is therefore a
real defect, not descriptor noise. The tolerance accounts for on-device value
quantization; tighten per-engine if a build stores exact floats.
"""

import re

import pytest

import harness

# One describe for the whole collection phase, codec-aware and failure-tolerant - see harness.py for
# why both of those matter here rather than in a fixture.
_DESCR = harness.collect_descriptor()


def _params():
    return list(_DESCR.params.values()) if _DESCR else []


@pytest.mark.parametrize("p", _params(), ids=lambda p: p.name)
def test_param_roundtrip(test_mode, p):
    if not _DESCR.masked:
        pytest.skip(
            "engine has not implemented live_params()/live_configs(), so describe lists the whole "
            "ParamId enum - a read-back mismatch here would be descriptor noise, not a defect"
        )
    dev = test_mode
    decks = ["A", "B"] if p.scope == "deck" else ["A"]
    span = p.hi - p.lo
    lo_t, hi_t = p.lo + 0.25 * span, p.lo + 0.75 * span

    for d in decks:
        # Exact equality is the wrong assertion: plenty of params are legitimately QUANTIZED (tape's
        # `aux` selects one of 8 SD slots, so 0.5 reads back 0.5625) and the descriptor has no way to
        # declare a step. Instead assert the two properties that hold for continuous and quantized
        # params alike, and that a dead param fails:
        #
        #   1. it TRACKS input   - two different targets must read back differently. A param whose
        #                          getter is missing (returns a constant) fails here.
        #   2. it is STABLE      - writing back what was just read must be a fixed point. Catches a
        #                          getter and setter that disagree about scaling or units.
        dev.set_param(p.name, d, lo_t)
        r1 = dev.get_param(p.name, d)
        dev.set_param(p.name, d, hi_t)
        r2 = dev.get_param(p.name, d)

        assert p.lo - 1e-4 <= r1 <= p.hi + 1e-4, "{}[{}] read {} outside {}..{}".format(
            p.name, d, r1, p.lo, p.hi)
        assert r1 != r2, (
            "{}[{}] returned {} for both {} and {} - it does not track input "
            "(a missing getter reads as a constant)".format(p.name, d, r1, lo_t, hi_t))

        dev.set_param(p.name, d, r2)
        r3 = dev.get_param(p.name, d)
        assert abs(r3 - r2) <= 1e-3 * span + 1e-4, (
            "{}[{}] is not a fixed point: wrote back its own reading {} and got {} - "
            "getter and setter disagree".format(p.name, d, r2, r3))


def test_pad_play_answers(test_mode):
    """`pad play` must answer with the deck's emptiness flag, on both codecs.

    Worth its own test for a narrow reason: `pad play` is one of only TWO writes that reply at all
    (the other is `fx gritmode`). Every other write is silent under OSC by design, so this is one of
    the two paths where a value-returning ACTION crosses the codec boundary - and under OSC it is the
    free-form-text path, the one case the typed reply sink deliberately degrades to a string. Nothing
    else in the suite exercises it.

    What is NOT asserted, deliberately: that playback started. There is nothing to observe it with -
    tape's `on_play_pad` returns a constant and it declares no transport query, so any "the deck is now
    playing" assertion would be unfalsifiable. That is exactly the trap the original test_tape.py fell
    into (see its docstring): it asserted `pad rec A` makes a deck non-empty, had never been run, and
    could never have passed. Assert the reply, which is real, and nothing about state, which is not.

    Called twice: engines that model play as a toggle (tape) are left as they were found, and engines
    that treat it as momentary are unaffected by the repeat. `pad stop` afterwards for the engines that
    implement `stop_if_generating` - a no-op on those that do not.
    """
    dev = test_mode
    for press in ("first", "second"):
        out = dev.pad("play", "A")
        assert re.fullmatch(r"empty=[01]", out or ""), (
            "{} `pad play` answered {!r}, expected empty=<0|1>".format(press, out))
    dev.pad("stop", "A")


# --- L1 state queries ------------------------------------------------------------------------------
# The param sweep above only proves values go in and come back. These exercise the *observation* half
# of the channel, and they are likewise driven entirely by `describe` - no per-engine knowledge.

def _query_names():
    return sorted(_DESCR.queries) if _DESCR else []


def _config_query_pairs():
    """Config names that also have a same-named query - i.e. that can be round-tripped."""
    return sorted(n for n in _DESCR.configs if n in _DESCR.queries) if _DESCR else []


@pytest.mark.parametrize("name", _query_names(), ids=lambda n: n)
def test_query_answers(test_mode, name):
    """Every advertised query must answer without an error reply, on every deck it claims.

    A `describe` that lists a query the dispatcher does not implement would otherwise only show up
    as `err unknown-verb` the first time somebody typed it by hand.
    """
    dev = test_mode
    q = _DESCR.queries[name]
    for deck in (["A", "B"] if q.scope == "deck" else [""]):
        out = dev.query(name, deck)
        assert out != "", "query {} {} answered ok with no value".format(name, deck)

        # The reply must parse as the kind the descriptor declared. A device that advertises `int` and
        # returns a word is a real defect, and without this the sweep would accept any string at all.
        if q.kind == "bool":
            assert out in ("0", "1"), "{} declares bool, returned {!r}".format(name, out)
        elif q.kind == "int":
            int(out)
        elif q.kind == "float":
            float(out)
        elif q.kind == "enum":
            assert int(out) in q.values, \
                "{} returned {} which is not one of its declared labels {}".format(
                    name, out, sorted(q.values))


@pytest.mark.parametrize("name", _config_query_pairs(), ids=lambda n: n)
def test_config_query_round_trip(test_mode, name):
    """Setting a config must be observable through the same-named query, in the SAME encoding.

    This is the regression guard for a real defect: `config route` spoke the platform selector
    encoding (0=Stereo) while `query route` returned the raw Route enum (Stereo=2), so `config route
    A 0` read back as 2 and route could not be round-tripped at all. `describe` publishes only the
    selector labels, so a host had no way to learn the difference. Found by hand at the REPL; this
    would have caught it automatically, for any config/query pair rather than just route.
    """
    dev = test_mode
    desc = _DESCR
    deck = "A" if desc.queries[name].scope == "deck" else ""
    for value in sorted(desc.configs[name].values):
        dev.set_config(name, "A", value)
        got = dev.query(name, deck)
        assert int(got) == value, (
            "set {} = {} ({}) but query returned {} - config and query disagree on encoding".format(
                name, value, desc.configs[name].values[value], got)
        )
