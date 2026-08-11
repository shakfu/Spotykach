#!/usr/bin/env python3
"""skbridge.py - a UDP <-> SLIP-serial bridge, so OSC-over-UDP software can drive the device.

The device speaks OSC over SLIP over USB CDC (``make ENGINE=<e> TERMINAL=1 OSC=1``). It has no
Ethernet and no WiFi, so OSC-over-UDP - which is what TouchOSC, Max, Pd and every ``oscsend``-shaped
tool actually emit - cannot reach it directly. This is the piece that closes that gap, and it is the
same arrangement monome uses for the same reason: ``serialosc`` is a serial device with a network
daemon in front of it.

Without this, ``make tosc``'s generated TouchOSC layouts have nothing to connect to, and the OSC
codec's only reachable consumer is the browser front-end (which talks to the serial port directly and
so needs no bridge). See ``docs/dev/terminal-osc.md`` and ``docs/dev/tosc.md``.

**The bridge does not parse OSC.** It translates FRAMING and nothing else: UDP is already
packet-delimited by the datagram, and SLIP supplies the same delimitation over a byte stream, so a
packet crosses unmodified in both directions. That is what keeps it correct as the address space
grows - a new address, argument type or bundle layout needs no change here. Addresses are decoded
for the ``-v`` log only, and a decode failure never drops a packet.

Usage::

    python3 tools/skbridge.py                      # auto-discover the port, listen on 8000
    python3 tools/skbridge.py --port /dev/ttyACM0 --listen 8000 --reply-port 9000 -v

Then point TouchOSC/Max/Pd at this machine's address, port 8000, and have it listen on 9000::

    oscsend osc.udp://127.0.0.1:8000 /sk/a/param/speed f 0.5

Exits cleanly (skips, does not crash) when no device is attached, like the rest of ``tools/``.
"""

import argparse
import os
import selectors
import socket
import sys
import time

from skdev import osc

# NOTE: `skdev.protocol` is imported inside main(), not here. It pulls in pyserial, and the point of
# keeping `Relay` free of it is the same one `skdev.osc` makes: the part where the bugs live (framing,
# escaping) must be testable on a machine with no serial stack, which is what CI is.

#: What TouchOSC and Max templates conventionally use. The bridge listens on one and answers on the
#: other; they must differ, since both ends are on the same host in the common case.
DEFAULT_LISTEN = 8000
DEFAULT_REPLY = 9000

#: Big enough for the largest thing the device sends - an unmasked describe bundle is ~5.5 KB - and
#: for any datagram a control surface will send. Note a datagram this size fragments at the IP layer;
#: that is the network's problem and it reassembles, but it is why the describe bundle is the one
#: payload worth watching if a link ever proves lossy.
MAX_DATAGRAM = 65535

#: Bytes read from a silent-looking device before warning about the codec. A line-codec build answers
#: in ASCII with no SLIP delimiters at all, so the decoder yields nothing and the bridge looks broken
#: when it is the firmware that is wrong. This is the "wrong codec = silence" failure mode.
CODEC_WARN_BYTES = 256


class Relay:
    """The pure half: framing translation, with no sockets and no serial port.

    Separated for the same reason ``skdev.osc`` is dependency-free - it makes the part where the bugs
    live (chunk boundaries, escaping) testable with no hardware and no network. There is very little
    to it, and that is the design rather than an omission.
    """

    def __init__(self):
        self._dec = osc.SlipDecoder()
        self._seen = 0
        self._framed = False

    def to_device(self, datagram):
        """One UDP datagram -> the bytes to write to the serial port."""
        return osc.slip_encode(datagram)

    def from_device(self, chunk):
        """Serial bytes -> zero or more complete datagrams to send back over UDP."""
        self._seen += len(chunk)
        frames = self._dec.feed(chunk)
        if frames:
            self._framed = True
        return frames

    @property
    def looks_unframed(self):
        """True once enough bytes have arrived with no SLIP frame among them to be suspicious."""
        return not self._framed and self._seen >= CODEC_WARN_BYTES


def describe_packet(packet):
    """A one-line summary of a packet for the verbose log. Never raises - this is only ever a log."""
    try:
        pairs = osc.decode_packet(packet)
    except Exception:                       # noqa: BLE001 - a log line must not break the relay
        return "<{} bytes, undecodable>".format(len(packet))
    if len(pairs) == 1:
        addr, args = pairs[0]
        return "{} {}".format(addr, " ".join(repr(a) for a in args)) if args else "{} (read)".format(addr)
    return "bundle of {} ({} bytes)".format(len(pairs), len(packet))


class Bridge:
    """The I/O half: a UDP socket and a serial port, multiplexed in one loop.

    One loop rather than a thread per direction, so there is no shared state to lock and no question
    about writing to the port from two threads. Both ends are injected, which is what lets the tests
    drive the real loop against a pty and an ephemeral socket instead of hardware.
    """

    def __init__(self, ser, sock, reply_port=DEFAULT_REPLY, reply_host=None, verbose=False,
                 log=sys.stderr):
        self.ser = ser
        self.sock = sock
        self.reply_port = reply_port
        self.reply_host = reply_host
        self.verbose = verbose
        self.log = log
        self.relay = Relay()
        self.to_device_count = 0
        self.to_network_count = 0
        self._learned_host = None
        self._warned = False
        self._running = False

    # --- reply addressing ---------------------------------------------------
    #
    # An OSC client sends from an EPHEMERAL port but listens on a fixed one - TouchOSC calls them the
    # send and receive ports, Max spells it [udpsend]/[udpreceive]. So replying to the source address
    # of the datagram would answer a port nothing is listening on. Instead: the source IP is learned,
    # and the reply goes to `reply_port` on it. `--reply-host` overrides when the surface is on a
    # different machine from the sender, which is unusual but happens with a phone plus a patch.

    @property
    def reply_target(self):
        host = self.reply_host or self._learned_host
        return (host, self.reply_port) if host else None

    def _note(self, msg):
        print(msg, file=self.log, flush=True)

    # --- the loop -----------------------------------------------------------

    def serve_forever(self, timeout=0.2):
        """Relay until `stop()`. Returns when stopped."""
        self._running = True
        sel = selectors.DefaultSelector()
        sel.register(self.sock, selectors.EVENT_READ, self._on_network)
        sel.register(self.ser, selectors.EVENT_READ, self._on_device)
        try:
            while self._running:
                for key, _mask in sel.select(timeout):
                    key.data()
        finally:
            sel.close()

    def stop(self):
        self._running = False

    def _on_network(self):
        datagram, addr = self.sock.recvfrom(MAX_DATAGRAM)
        if not datagram:
            return
        self._learned_host = addr[0]
        self.ser.write(self.relay.to_device(datagram))
        self.to_device_count += 1
        if self.verbose:
            self._note("-> {}".format(describe_packet(datagram)))

    def _on_device(self):
        # Non-blocking: the selector already said there is something, and `in_waiting` keeps a burst
        # in one read rather than one syscall per byte.
        chunk = self.ser.read(max(1, getattr(self.ser, "in_waiting", 0) or 1))
        if not chunk:
            return
        for packet in self.relay.from_device(chunk):
            target = self.reply_target
            if target is None:
                # Nothing has ever sent to us, so there is nowhere to answer. Say so once rather than
                # dropping in silence - it is the state a user lands in by starting the bridge and
                # then wondering why replies vanish.
                self._note("device sent {} bytes with no client to answer - send something first, "
                           "or pass --reply-host".format(len(packet)))
                continue
            self.sock.sendto(packet, target)
            self.to_network_count += 1
            if self.verbose:
                self._note("<- {}".format(describe_packet(packet)))
        if self.relay.looks_unframed and not self._warned:
            self._warned = True
            self._note("warning: bytes are arriving but none are SLIP-framed. This is what a "
                       "LINE-codec build looks like from here - rebuild with `TERMINAL=1 OSC=1`.")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="serial device (default: auto-discover by USB vendor id)")
    ap.add_argument("--listen", type=int, default=DEFAULT_LISTEN,
                    help="UDP port to receive on (default: %(default)s)")
    ap.add_argument("--bind", default="0.0.0.0",
                    help="address to bind the listener to (default: %(default)s; use 127.0.0.1 to "
                         "refuse anything off this machine)")
    ap.add_argument("--reply-port", type=int, default=DEFAULT_REPLY,
                    help="UDP port to send replies to (default: %(default)s)")
    ap.add_argument("--reply-host", default=None,
                    help="host to send replies to (default: learned from the first packet received)")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every packet in both directions")
    args = ap.parse_args(argv)

    from skdev.protocol import find_port, open_serial, Timeout

    try:
        port = find_port(args.port)
    except Timeout as e:
        print("no device attached: {}".format(e), file=sys.stderr)
        return 1

    # timeout=0 so reads never block: the selector decides when there is something to read.
    ser = open_serial(port, timeout=0)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.listen))

    bridge = Bridge(ser, sock, reply_port=args.reply_port, reply_host=args.reply_host,
                    verbose=args.verbose)
    print("bridging {} <-> udp {}:{} (replies to {}:{})".format(
        port, args.bind, args.listen, args.reply_host or "<first sender>", args.reply_port),
        file=sys.stderr)
    try:
        bridge.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        ser.close()
        print("\n{} packets to the device, {} back".format(
            bridge.to_device_count, bridge.to_network_count), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
