"""skdev - host-side client library for the sk-engines terminal channel.

This package speaks the phase-1 line protocol (see ``docs/dev/terminal-dispatch.md``)
over the USB-C CDC serial port exposed by a flashed sk-engines device. It is the
shared library imported by both the pytest harness (``tools/test_*.py``) and the
interactive REPL (``tools/skterm.py``).

Modules:
    protocol   - port discovery, line framing, log filtering, reply/error parsing.
    descriptor - DeviceDescriptor dataclasses + parse_describe().
    device     - Device: connection + high-level command API + test_mode() context.

Nothing here talks to hardware at import time; construct a :class:`Device` to open
a port. When no device is attached, port discovery raises :class:`Timeout`, which
callers (e.g. the pytest ``device`` fixture) turn into a clean skip.
"""

from .protocol import Timeout, CommandError, find_port, open_serial, is_log
from .descriptor import ParamDesc, ConfigDesc, DeviceDescriptor, parse_describe
from .device import Device

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
]
