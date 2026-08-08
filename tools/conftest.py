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
    except OSError as e:
        # The port EXISTS but will not open - a different situation from "nothing attached", and one
        # the Timeout-only guard used to turn into a hard error. Commonly: permission denied (the port
        # is root:dialout; `sudo usermod -aG dialout $USER`, then re-login), or the port already held
        # by skterm.py / screen. pyserial's SerialException subclasses OSError. Skip rather than fail,
        # since none of these say anything about the firmware - but name the reason, because a silent
        # skip here looks identical to "no hardware" and that cost a debugging cycle once already.
        pytest.skip(f"sk-engines device found but not usable: {e}")
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
