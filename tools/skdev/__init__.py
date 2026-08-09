"""skdev - host-side client library for the sk-engines terminal channel.

This package speaks the phase-1 line protocol (see ``docs/dev/terminal-dispatch.md``)
over the USB-C CDC serial port exposed by a flashed sk-engines device. It is the
shared library imported by both the pytest harness (``tools/test_*.py``) and the
interactive REPL (``tools/skterm.py``).

Modules:
    protocol   - port discovery, line framing, log filtering, reply/error parsing.
    descriptor - DeviceDescriptor dataclasses + parse_describe().
    device     - Device: connection + high-level command API + test_mode() context.
    osc        - OSC 1.0 wire format + SLIP framing (the SPK_TERMINAL_OSC codec).
    semantic   - the host-side semantic address tier, generated from describe.
    oscdevice  - OscDevice: the same command API over the OSC codec.

Nothing here talks to hardware at import time; construct a :class:`Device` to open
a port. When no device is attached, port discovery raises :class:`Timeout`, which
callers (e.g. the pytest ``device`` fixture) turn into a clean skip.

``descriptor`` is deliberately dependency-free: it parses text and knows nothing
about serial ports. The serial-backed names (``protocol``/``device``) are therefore
re-exported **lazily** via PEP 562, so importing ``skdev.descriptor`` works on a
machine without pyserial installed - which is what lets the device-free parser test
run in CI. Touching a serial-backed name without pyserial still raises ImportError,
at the point of use rather than at collection time.
"""

from .descriptor import ParamDesc, ConfigDesc, DeviceDescriptor, parse_describe
# The OSC wire format and the semantic translator are byte/text handling with no serial
# dependency, so they import eagerly alongside descriptor - which is what lets the
# codec and translator tests run in CI with no pyserial and no hardware.
from . import osc, semantic

_LAZY = {
    "Timeout": "protocol",
    "CommandError": "protocol",
    "find_port": "protocol",
    "open_serial": "protocol",
    "is_log": "protocol",
    "Device": "device",
    "OscDevice": "oscdevice",
}


def __getattr__(name):        # PEP 562: defer the pyserial-dependent imports to first use
    mod = _LAZY.get(name)
    if mod is None:
        raise AttributeError("module {!r} has no attribute {!r}".format(__name__, name))
    import importlib
    return getattr(importlib.import_module("." + mod, __name__), name)


def __dir__():
    return sorted(set(globals()) | set(_LAZY))

__all__ = [
    "Timeout",
    "CommandError",
    "find_port",
    "open_serial",
    "is_log",
    "ParamDesc",
    "ConfigDesc",
    "DeviceDescriptor",
    "parse_describe",
    "Device",
    "OscDevice",
    "osc",
    "semantic",
]
