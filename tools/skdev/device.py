"""device.py - the high-level command API for a connected device.

:class:`Device` wraps an open serial port and exposes the phase-1 terminal
protocol as flat, assertable Python methods. It owns the framing rules:

  * Commands are sent as ``<line>\\r\\n``.
  * Replies are read a line at a time; interleaved ``[tag]`` log lines are
    skipped (and optionally captured via ``log_sink``) until a real reply
    arrives - ``ok`` / ``ok <value>`` / ``err <reason>``.
  * Commands are synchronous, one outstanding at a time, so there is never any
    reply-to-command ambiguity in phase 1.

The stimulus/observation methods map one-to-one onto the verb catalog in
``docs/dev/terminal-dispatch.md``. :meth:`Device.test_mode` is a context manager
around ``mode test`` / ``mode run`` for input-isolated, deterministic tests.
"""

from contextlib import contextmanager

from .protocol import find_port, open_serial, is_log, Timeout, CommandError
from .descriptor import parse_describe


class Device:
    """A connected sk-engines device speaking the phase-1 terminal protocol."""

    # Default read timeout. Generous on purpose: the channel is pumped from the main loop, so its
    # latency is bounded by the SLOWEST main-loop consumer - a streaming engine (tape/radio/bard)
    # scanning the SD card in prepare() can stall replies for a good fraction of a second.
    DEFAULT_TIMEOUT = 3.0

    def __init__(self, port=None, timeout=DEFAULT_TIMEOUT, log_sink=None):
        self.port = find_port(port)
        self.ser = open_serial(self.port, timeout)
        self.log_sink = log_sink          # callable(str) for captured [tag] lines, or None

    def close(self):
        """Close the underlying serial port."""
        self.ser.close()

    # --- framing -------------------------------------------------------------
    def _drain_stale(self):
        """Discard anything already waiting before sending a command.

        The protocol is synchronous - one command in flight - so nothing should ever be pending here.
        Anything that is, is either a late reply from a command that timed out, or an unsolicited
        transport error (`err overflow` / `err tx-overflow`). Without this, a SINGLE timeout offsets
        every subsequent reply for the life of the session: each command reads the previous one's
        answer, and the failures surface far from the cause as nonsense parse errors.

        Discarded text goes to ``log_sink`` when one is set, so it is dropped visibly rather than
        silently.
        """
        pending = self.ser.in_waiting
        if not pending:
            return
        stale = self.ser.read(pending).decode(errors="replace")
        if self.log_sink:
            for ln in stale.splitlines():
                if ln:
                    self.log_sink("[stale] " + ln)

    def _send(self, line):
        self._drain_stale()
        self.ser.write((line + "\r\n").encode())

    def _readline(self):
        raw = self.ser.readline()         # blocks up to serial timeout
        if not raw:
            raise Timeout("no reply")
        return raw.decode(errors="replace").rstrip("\r\n")

    def _read_reply(self):
        while True:                       # skip interleaved log lines
            ln = self._readline()
            if is_log(ln):
                if self.log_sink:
                    self.log_sink(ln)
                continue
            if ln == "ok":
                return ""
            if ln.startswith("ok "):
                return ln[3:]
            if ln.startswith("err "):
                raise CommandError(ln[4:])
            raise CommandError("unexpected: {!r}".format(ln))

    def cmd(self, line):
        """Send one command line and return its reply payload (``""`` for bare ``ok``)."""
        self._send(line)
        return self._read_reply()

    # --- stimulus (target A) -------------------------------------------------
    def set_param(self, name, deck, value):
        self.cmd("set param {} {} {:.6g}".format(name, deck, value))

    def get_param(self, name, deck):
        return float(self.cmd("get param {} {}".format(name, deck)))

    def set_config(self, name, deck, v):
        return self.cmd("config {} {} {}".format(name, deck, v)) == "1"

    def cv(self, kind, deck, value):
        self.cmd("cv {} {} {:.6g}".format(kind, deck, value))

    def gate(self, deck):
        self.cmd("gate {}".format(deck))

    def midi_note(self, ch, note):
        self.cmd("midi note {} {}".format(ch, note))

    def pad(self, action, deck, rev=False):
        """Press a pad. Returns the reply payload.

        `play` is one of only two pads that answer with a value (`empty=<0|1>` - the deck's emptiness,
        which a host has no other way to learn from the press); the rest reply bare `ok`, i.e. "".
        This used to discard the reply and return None, which made it impossible to assert the one
        thing the press reports.
        """
        return self.cmd("pad {} {}{}".format(action, deck, " rev" if rev else ""))

    def fx(self, kind, deck, on):
        self.cmd("fx {} {} {}".format(kind, deck, "on" if on else "off"))

    # --- composites ----------------------------------------------------------
    def reset(self, deck=""):
        """Drive every advertised param to the engine's default. Returns the count written."""
        return int(self.cmd("reset {}".format(deck).rstrip()))

    def preset_save(self, slot=0):
        """Snapshot the advertised params into an in-RAM slot. Returns the count captured."""
        return int(self.cmd("preset save {}".format(slot)))

    def preset_load(self, slot=0):
        """Restore a slot. Returns the count restored; 0 if the slot was never saved."""
        return int(self.cmd("preset load {}".format(slot)))

    # --- observation (L0/L1) -------------------------------------------------
    def query(self, name, deck=""):
        return self.cmd("query {} {}".format(name, deck).rstrip())

    def caps(self):
        return int(self.cmd("caps"), 16)

    # --- introspection -------------------------------------------------------
    def describe(self):
        """Run ``describe`` and return the parsed :class:`DeviceDescriptor`."""
        self._send("describe")
        lines = []
        while True:
            ln = self._readline()
            if is_log(ln):
                if self.log_sink:
                    self.log_sink(ln)
                continue
            if ln == "end":
                break
            lines.append(ln)
        return parse_describe(lines)

    # --- determinism ---------------------------------------------------------
    @contextmanager
    def test_mode(self):
        """Context manager: ``mode test`` on enter, ``mode run`` on exit.

        Physical input (knobs/CV/gate) is frozen while inside, so terminal-
        injected stimulus is the only driver of the engine.
        """
        self.cmd("mode test")
        try:
            yield self
        finally:
            self.cmd("mode run")
