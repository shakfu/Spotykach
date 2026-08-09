"""osc.py - OSC 1.0 wire format + SLIP framing for the sk-engines terminal channel.

The host half of ``src/terminal/osc_{decode,encode}.cpp`` and ``src/terminal/slip.h``.
See ``docs/dev/terminal-osc.md``.

Deliberately **dependency-free** (no pyserial, no python-osc): it is pure byte
handling, which is what lets the codec tests and the semantic translator run in CI
on a machine with no hardware and no serial stack. The serial-backed client lives in
``oscdevice.py``.

Two things are worth knowing before reading:

  * OSC is big-endian and 4-byte aligned. Strings are NUL-terminated and then padded
    with NULs to the next multiple of four.
  * A message with **no type-tag string at all** is legal, and here it is meaningful:
    it is how a read is spelled. ``encode(addr)`` with no args produces exactly that.
"""

import struct

# --- SLIP (RFC 1055) -------------------------------------------------------------

END = 0xC0
ESC = 0xDB
ESC_END = 0xDC
ESC_ESC = 0xDD


def slip_encode(payload):
    """Wrap ``payload`` (bytes) in one SLIP frame, END-delimited at both ends."""
    out = bytearray([END])
    for b in payload:
        if b == END:
            out += bytes([ESC, ESC_END])
        elif b == ESC:
            out += bytes([ESC, ESC_ESC])
        else:
            out.append(b)
    out.append(END)
    return bytes(out)


class SlipDecoder:
    """Incremental SLIP decoder: feed bytes, get back complete frames.

    Unbounded on purpose. The device's own assembler is capped at 512 B because that
    bounds the INBOUND direction, but the descriptor travelling the other way is an
    order of magnitude larger - a host has to be able to receive what the device can
    send.
    """

    def __init__(self):
        self._buf = bytearray()
        self._escaped = False

    def feed(self, data):
        """Consume bytes; return a list of complete frames (possibly empty)."""
        frames = []
        for b in data:
            if b == END:
                if self._buf:
                    frames.append(bytes(self._buf))
                self._buf.clear()
                self._escaped = False
            elif self._escaped:
                self._buf.append(END if b == ESC_END else ESC if b == ESC_ESC else b)
                self._escaped = False
            elif b == ESC:
                self._escaped = True
            else:
                self._buf.append(b)
        return frames


# --- OSC ---------------------------------------------------------------------------

def _pad(b):
    return b + b"\0" * (-len(b) % 4)


def _ostr(s):
    return _pad(s.encode("ascii") + b"\0")


def encode(address, *args):
    """Build one OSC message.

    Arguments are typed from their Python type: ``float`` -> ``f``, ``int`` -> ``i``,
    ``str`` -> ``s``, ``bool`` -> ``T``/``F``. Call with no arguments to produce the
    **read** form - a message with no type-tag string, which the device reads as
    "report the value at this address".
    """
    if not args:
        return _ostr(address)
    tags = ","
    body = b""
    for a in args:
        if isinstance(a, bool):            # before int: bool IS an int in Python
            tags += "T" if a else "F"
        elif isinstance(a, int):
            tags += "i"
            body += struct.pack(">i", a)
        elif isinstance(a, float):
            tags += "f"
            body += struct.pack(">f", a)
        elif isinstance(a, str):
            tags += "s"
            body += _ostr(a)
        else:
            raise TypeError("unsupported OSC argument type: {!r}".format(type(a)))
    return _ostr(address) + _ostr(tags) + body


def _read_string(buf, off):
    """Read one OSC-string at ``off``; return it and the offset past its NUL padding."""
    end = buf.index(b"\0", off)
    s = buf[off:end].decode("ascii", errors="replace")
    n = end - off                            # length without the NUL
    return s, off + ((n + 4) & ~3)           # +1 for the NUL, then round up to 4


def decode(packet):
    """Decode one OSC message into ``(address, [args])``.

    Bundles are not handled here - use :func:`decode_packet`, which dispatches.
    """
    addr, off = _read_string(packet, 0)
    if off >= len(packet):
        return addr, []                     # no type-tag string: the read form
    tags, off = _read_string(packet, off)
    args = []
    for t in tags[1:]:
        if t == "i":
            args.append(struct.unpack_from(">i", packet, off)[0]); off += 4
        elif t == "f":
            args.append(struct.unpack_from(">f", packet, off)[0]); off += 4
        elif t == "d":
            args.append(struct.unpack_from(">d", packet, off)[0]); off += 8
        elif t in ("s", "S"):
            s, off = _read_string(packet, off)
            args.append(s)
        elif t == "T":
            args.append(True)
        elif t == "F":
            args.append(False)
        else:
            raise ValueError("unsupported OSC type tag {!r}".format(t))
    return addr, args


def is_bundle(packet):
    return packet[:8] == b"#bundle\0"


def decode_packet(packet):
    """Decode a message or a bundle into a list of ``(address, args)`` pairs.

    Bundle timetags are ignored, exactly as the device ignores them: contents are
    taken as immediate and in order.
    """
    if not is_bundle(packet):
        return [decode(packet)]
    out = []
    off = 16                                 # "#bundle\0" + timetag
    while off + 4 <= len(packet):
        (size,) = struct.unpack_from(">i", packet, off)
        off += 4
        out.extend(decode_packet(packet[off:off + size]))
        off += size
    return out
