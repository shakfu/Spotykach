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


def _params():
    """Collection-time: open, describe, close. Returns [] when no hardware."""
    try:
        dev = Device()
    except Timeout:
        return []
    try:
        return list(dev.describe().params.values())
    finally:
        dev.close()


@pytest.mark.parametrize("p", _params(), ids=lambda p: p.name)
def test_param_roundtrip(test_mode, p):
    dev = test_mode
    decks = ["A", "B"] if p.scope == "deck" else ["A"]
    target = p.lo + 0.5 * (p.hi - p.lo)        # mid-range, inside the declared range
    for d in decks:
        dev.set_param(p.name, d, target)
        got = dev.get_param(p.name, d)
        assert abs(got - target) <= 1e-3 * (p.hi - p.lo) + 1e-4, \
            "{}[{}] set {} got {}".format(p.name, d, target, got)
