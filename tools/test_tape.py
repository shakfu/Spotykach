"""test_tape.py - per-engine tests for the tape engine.

Kept deliberately small: anything expressible generically belongs in test_generic.py, which is driven
by `describe` and covers every engine. What lands here is the engine-specific behaviour a descriptor
cannot describe - in tape's case, the relationship between a generic parameter and the engine's own
declared state.

History worth keeping: the original version of this file asserted that `pad rec A` makes a deck
non-empty. It had never been run. Tape does not override `audio_is_empty` at all, so `query empty`
returns the IEngine default (`true`) forever and the assertion could never have passed.
"""

import pytest

pytestmark = pytest.mark.usefixtures("test_mode")


def _is_tape(descriptor):
    return descriptor.engine == "tape"


def test_declares_its_own_state(device, descriptor):
    """tape's target-B queries should be indistinguishable from platform ones to a host."""
    if not _is_tape(descriptor):
        pytest.skip("not a tape build")
    for name in ("slot", "loopmode", "speed"):
        assert name in descriptor.queries, "tape should advertise `{}`".format(name)
    assert descriptor.queries["loopmode"].kind == "enum"
    assert descriptor.queries["loopmode"].values == {
        0: "none", 1: "plain", 2: "faded", 3: "fripp"
    }


def test_aux_param_selects_the_slot(device, descriptor):
    """The generic AUX param and the engine-specific `slot` query must agree.

    This is the pairing a descriptor cannot express: AUX is a normalized 0..1 knob to the platform,
    but tape maps it onto 8 discrete SD slots. Walking every slot proves the mapping end to end and
    is exactly the quantization that makes an exact-equality param sweep the wrong test.
    """
    if not _is_tape(descriptor):
        pytest.skip("not a tape build")
    slots = 8
    for n in range(slots):
        device.set_param("aux", "A", (n + 0.5) / slots)   # centre of slot n
        assert int(device.query("slot", "A")) == n, "aux centre of slot {} should select it".format(n)


def test_speed_query_reports_the_running_varispeed(device, descriptor):
    """`get param speed` is the PITCH knob; `query speed` is the rate actually in effect."""
    if not _is_tape(descriptor):
        pytest.skip("not a tape build")
    speed = float(device.query("speed", "A"))
    assert speed > 0.0, "a stopped deck should still report a positive nominal rate"
