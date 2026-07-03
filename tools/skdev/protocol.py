"""protocol.py - transport client for the sk-engines terminal channel.

Handles the low-level concerns that sit under the command API:

  * Port discovery - find the Daisy CDC port by USB VID, with a per-platform
    device-glob fallback (``/dev/ttyACM*`` on Linux, ``/dev/tty.usbmodem*`` on
    macOS).
  * Serial open - a pyserial ``Serial`` with DTR asserted (some hosts gate CDC
    output on DTR; baud is cosmetic over USB CDC but pyserial requires a value).
  * Log filtering - the one protocol invariant: log lines begin with ``[`` (the
    firmware ``LOG_TAGGED`` format ``[tag] ...``); reply lines never do. See
    :func:`is_log`.

The two exception types (:class:`Timeout`, :class:`CommandError`) are the error
surface the rest of the library and the harness assert against.
"""

import sys
import glob
import serial
from serial.tools import list_ports

DAISY_VID = 0x0483          # STMicroelectronics (Daisy Seed CDC); PID 0x5740 typical
BAUD = 115200              # ignored by USB CDC, but pyserial requires a value
LOG_PREFIX = "["           # LOG_TAGGED lines; filtered out of the reply stream


class Timeout(Exception):
    """Raised when a read blocks past the serial timeout, or no port is found."""


class CommandError(Exception):
    """Raised when the device replies ``err <reason>`` (or an unexpected line).

    The ``reason`` attribute holds the bare error token (one of the fixed set:
    unknown-verb, unknown-param, unknown-config, bad-deck, bad-arg, no-arg,
    too-many-args, line-too-long, overflow), so host assertions can match on a
    stable identifier rather than wording.
    """

    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def find_port(explicit=None):
    """Return the serial device path for an attached sk-engines device.

    Prefers a USB VID match (robust across OSes); falls back to a per-platform
    device glob. Raises :class:`Timeout` if nothing is found so the caller can
    skip cleanly when no hardware is attached.
    """
    if explicit:
        return explicit
    for p in list_ports.comports():        # prefer VID match - robust across OSes
        if p.vid == DAISY_VID:
            return p.device
    pats = {
        "darwin": "/dev/tty.usbmodem*",
        "linux": "/dev/ttyACM*",
    }.get(sys.platform, "")
    hits = sorted(glob.glob(pats)) if pats else []
    if not hits:
        raise Timeout("no device port found")
    return hits[0]


def open_serial(port, timeout=1.0):
    """Open the CDC port with DTR asserted. Returns a pyserial ``Serial``."""
    return serial.Serial(port, BAUD, timeout=timeout, dtr=True)


def is_log(line):
    """True if ``line`` is a ``[tag] ...`` log line (not a reply)."""
    return line.startswith(LOG_PREFIX)
