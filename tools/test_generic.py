"""test_generic.py - cross-engine parameter sweep driven by ``describe``.

The payoff of a describe-driven client: this one file tests every engine build.
It is parametrized at collection time from a single ``describe`` (open, describe,
close), so each declared parameter becomes its own test case.

Because ``describe`` lists only params the engine's ``live_params()`` mask marks
live, this sweep never sets an ignored param - a read-back mismatch is therefore a
real defect, not descriptor noise. The tolerance accounts for on-device value
quantization; tighten per-engine if a build stores exact floats.
"""

import pytest

from skdev.device import Device
from skdev.protocol import Timeout


_MASKED = [True]   # filled by _params(); a list so the closure can write it at collection time


def _params():
    """Collection-time: open, describe, close. Returns [] when no hardware."""
    try:
        dev = Device()
    except Timeout:
        return []
    try:
        d = dev.describe()
        _MASKED[0] = d.masked
        return list(d.params.values())
    finally:
        dev.close()


@pytest.mark.parametrize("p", _params(), ids=lambda p: p.name)
def test_param_roundtrip(test_mode, p):
    if not _MASKED[0]:
        pytest.skip(
            "engine has not implemented live_params()/live_configs(), so describe lists the whole "
            "ParamId enum - a read-back mismatch here would be descriptor noise, not a defect"
        )
    dev = test_mode
    decks = ["A", "B"] if p.scope == "deck" else ["A"]
    target = p.lo + 0.5 * (p.hi - p.lo)        # mid-range, inside the declared range
    for d in decks:
        dev.set_param(p.name, d, target)
        got = dev.get_param(p.name, d)
        assert abs(got - target) <= 1e-3 * (p.hi - p.lo) + 1e-4, \
            "{}[{}] set {} got {}".format(p.name, d, target, got)


# --- L1 state queries ------------------------------------------------------------------------------
# The param sweep above only proves values go in and come back. These exercise the *observation* half
# of the channel, and they are likewise driven entirely by `describe` - no per-engine knowledge.

_CACHE = {}


def _describe_once():
    """One collection-time describe, shared by the query tests. None when no hardware."""
    if "d" not in _CACHE:
        try:
            dev = Device()
        except Timeout:
            _CACHE["d"] = None
        else:
            try:
                _CACHE["d"] = dev.describe()
            finally:
                dev.close()
    return _CACHE["d"]


def _query_names():
    d = _describe_once()
    return sorted(d.queries) if d else []


def _config_query_pairs():
    """Config names that also have a same-named query - i.e. that can be round-tripped."""
    d = _describe_once()
    return sorted(n for n in d.configs if n in d.queries) if d else []


@pytest.mark.parametrize("name", _query_names(), ids=lambda n: n)
def test_query_answers(test_mode, name):
    """Every advertised query must answer without an error reply, on every deck it claims.

    A `describe` that lists a query the dispatcher does not implement would otherwise only show up
    as `err unknown-verb` the first time somebody typed it by hand.
    """
    dev = test_mode
    scope = _describe_once().queries[name]
    for deck in (["A", "B"] if scope == "deck" else [""]):
        out = dev.query(name, deck)
        assert out != "", "query {} {} answered ok with no value".format(name, deck)


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
    desc = _describe_once()
    scope = desc.queries[name]
    deck = "A" if scope == "deck" else ""
    for value in sorted(desc.configs[name].values):
        dev.set_config(name, "A", value)
        got = dev.query(name, deck)
        assert int(got) == value, (
            "set {} = {} ({}) but query returned {} - config and query disagree on encoding".format(
                name, value, desc.configs[name].values[value], got)
        )
