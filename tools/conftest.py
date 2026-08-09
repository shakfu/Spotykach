"""conftest.py - pytest fixtures for the on-target terminal harness.

Provides three session/function fixtures that the hardware tests depend on:

  * ``device``     - session-scoped :class:`skdev.device.Device`; skips the whole
                     run cleanly (``pytest.skip``) when no device is attached, so
                     ``make test-hw`` no-ops on CI without hardware.
  * ``descriptor`` - session-scoped parsed ``describe`` output.
  * ``test_mode``  - per-test input isolation: wraps each test in the device's
                     ``mode test`` / ``mode run`` so knobs/CV/gate cannot perturb it.

The pyserial-backed imports are deferred INTO the ``device`` fixture, for the same
reason ``skdev/__init__.py`` defers them via PEP 562: some tests in this directory
need no device and no serial stack at all (``test_descriptor.py``,
``test_osc_codec.py``), and importing pyserial here would make collecting them fail
on a machine that has only pytest - which is exactly the CI machine.
"""

import pytest


def pytest_addoption(parser):
    parser.addoption(
        "--osc", action="store_true", default=False,
        help="drive the device with the OSC codec (a TERMINAL=1 OSC=1 build) instead of line-ASCII. "
             "The suites are codec-agnostic by design, so this is the cross-codec parity run: the same "
             "sweep against both builds must give identical results.",
    )


def pytest_configure(config):
    # Runs BEFORE test modules are imported, which is what lets the collection-time sweeps in
    # test_generic.py see the choice. Setting it any later would leave them on the line client.
    import harness
    harness.set_codec("osc" if config.getoption("--osc") else "line")


@pytest.fixture(scope="session")
def codec(request):
    """Which codec this session speaks - reported so a failure names the build it came from."""
    return "osc" if request.config.getoption("--osc") else "line"


@pytest.fixture(scope="session")
def device(codec):
    # Both clients expose the same method surface, which is exactly what makes the parity sweep
    # possible: every test below is written against that surface and never against a codec.
    from skdev.protocol import Timeout
    try:
        if codec == "osc":
            from skdev.oscdevice import OscDevice
            dev = OscDevice()
        else:
            from skdev.device import Device
            dev = Device()
    except ImportError as e:
        pytest.skip(f"pyserial not installed, so no device tests: {e}")
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
