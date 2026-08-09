"""harness.py - collection-time support for the parametrized hardware sweeps.

``test_generic.py`` turns each declared param and query into its own test case, which
means it needs a ``describe`` **at collection time**, before any fixture exists. That
has two consequences this module exists to handle.

**1. Collection-time failures abort the whole session.** Anything raised here is a
pytest collection error, which takes down every other test in the directory - including
the ones that need no device at all. So :func:`collect_descriptor` degrades to ``None``
on *any* failure and the sweeps parametrize to zero cases; the ``device`` fixture in
conftest then reports the real reason as a clean skip. The previous version of this
logic guarded only the ``Device()`` constructor and not the ``describe()`` call after
it, so a device that opened but did not answer - exactly what a line client sees when
it is pointed at an OSC build - aborted the run with a collection error instead.

**2. The codec is chosen on the command line, before test modules import.** conftest's
``pytest_configure`` calls :func:`set_codec` early enough that collection sees it, which
is what lets ``--osc`` reach the parametrized sweeps and not just the fixtures. Without
it the sweep would silently keep using the line client on an OSC build.
"""

_state = {"codec": "line", "descriptor": None, "attempted": False}


def set_codec(name):
    """Called from conftest's ``pytest_configure``; ``"line"`` or ``"osc"``."""
    _state["codec"] = name


def codec():
    return _state["codec"]


def client_class():
    """The client for the selected codec. Both expose the same method surface."""
    if _state["codec"] == "osc":
        from skdev.oscdevice import OscDevice
        return OscDevice
    from skdev.device import Device
    return Device


def collect_descriptor():
    """One ``describe`` for the whole collection phase, or ``None`` if unavailable.

    Cached including the failure, so a missing device costs one open attempt rather
    than one per parametrized sweep.
    """
    if _state["attempted"]:
        return _state["descriptor"]
    _state["attempted"] = True
    try:
        dev = client_class()()
        try:
            _state["descriptor"] = dev.describe()
        finally:
            dev.close()
    except Exception:
        # Deliberately broad. At collection time the ONLY acceptable outcome of a
        # hardware problem is "no test cases"; anything else takes the session with it.
        # The distinctions that matter (nothing attached / port unusable / attached but
        # silent) are reported by the device fixture, where they can be a clean skip.
        _state["descriptor"] = None
    return _state["descriptor"]
