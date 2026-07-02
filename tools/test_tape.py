"""test_tape.py - example per-engine test (tape build).

Illustrates a targeted, engine-aware test on top of the generic sweep: put the
engine in a recording mode, clear the buffer, inject deterministic stimulus, and
assert the observable L1 state changed. Runs inside ``test_mode`` so only the
terminal-injected commands drive the engine.

Guarded to skip on non-tape builds so the harness stays safe across engines.
"""

import pytest


def test_tape_records_and_reports_nonempty(test_mode, descriptor):
    if descriptor.engine and descriptor.engine != "tape":
        pytest.skip("not a tape build (engine={})".format(descriptor.engine))
    dev = test_mode
    dev.set_config("mode", "A", 1)             # a mode that records
    dev.pad("clear", "A")
    assert dev.query("empty", "A") == "1"
    dev.pad("rec", "A")
    dev.midi_note(1, 60)                        # inject stimulus deterministically
    assert dev.query("empty", "A") == "0"
