"""conftest.py - pytest fixtures for the on-target terminal harness.

Provides three session/function fixtures that the hardware tests depend on:

  * ``device``     - session-scoped :class:`skdev.device.Device`; skips the whole
                     run cleanly (``pytest.skip``) when no device is attached, so
                     ``make test-hw`` no-ops on CI without hardware.
  * ``descriptor`` - session-scoped parsed ``describe`` output.
  * ``test_mode``  - per-test input isolation: wraps each test in the device's
                     ``mode test`` / ``mode run`` so knobs/CV/gate cannot perturb it.
"""

import pytest

from skdev.device import Device
from skdev.protocol import Timeout


@pytest.fixture(scope="session")
def device():
    try:
        dev = Device()
    except Timeout:
        pytest.skip("no sk-engines device attached")   # make test-hw no-ops without hardware
    yield dev
    dev.close()


@pytest.fixture(scope="session")
def descriptor(device):
    return device.describe()


@pytest.fixture
def test_mode(device):
    """Per-test isolation: frozen physical input AND a known parameter baseline.

    ``mode test`` stops knobs/CV/gate/switches from perturbing the engine, but on its own it leaves
    whatever the previous test wrote in place - which is how a suite ends up passing in isolation and
    failing in sequence. ``reset`` drives every advertised param to the engine's declared default, so
    each test starts from the same state regardless of what ran before it.
    """
    with device.test_mode():
        device.reset()
        yield device
