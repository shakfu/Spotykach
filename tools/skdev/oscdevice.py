"""oscdevice.py - the command API for a device built with ``TERMINAL=1 OSC=1``.

:class:`OscDevice` exposes the **same method surface** as :class:`skdev.Device`
(``set_param``, ``get_param``, ``query``, ``describe``, ``test_mode`` ...) over the
OSC+SLIP codec instead of the line codec. That is not a convenience - it is the
acceptance criterion: ``tools/test_generic.py``'s cross-engine sweep runs unmodified
against either codec, and since layer [3] is shared byte for byte, anything the OSC
build answers differently is a codec bug by definition.

See ``docs/dev/terminal-osc.md``, "Testing".

Two differences from the line client are protocol, not implementation:

  * **A successful write is silent.** There is no ``/sk/ok``; a rig streaming fader
    moves at 100 Hz does not want an ack per message. This client turns acks on at
    connect (``/sk/dev/mode/ack ,T``) so a write is assertable without a read-back -
    which is exactly the case the spec says the harness is for.
  * **Errors arrive on ``/sk/err``**, carrying the request address alongside the
    reason, rather than as an ``err <reason>`` line.
"""

from contextlib import contextmanager

from . import osc
from .descriptor import ConfigDesc, DeviceDescriptor, ParamDesc, QueryDesc
from .protocol import find_port, open_serial, Timeout, CommandError
from .semantic import build as build_translator


class OscDevice:
    """A connected sk-engines device speaking the OSC codec."""

    DEFAULT_TIMEOUT = 3.0

    def __init__(self, port=None, timeout=DEFAULT_TIMEOUT, ack=True):
        self.port = find_port(port)
        self.ser = open_serial(self.port, timeout)
        self.timeout = timeout
        self._dec = osc.SlipDecoder()
        self._pending = []            # decoded frames not yet consumed; see _recv()
        self._globals = None          # lazily filled from describe(); see _scope()
        if ack:
            self.send("/sk/dev/mode/ack", True)

    def close(self):
        self.ser.close()

    # --- framing -------------------------------------------------------------
    def send(self, address, *args):
        """Send one OSC message. Does not wait for anything."""
        self.ser.write(osc.slip_encode(osc.encode(address, *args)))

    def _recv(self):
        """Read one decoded packet, or raise :class:`Timeout`.

        Pending frames are QUEUED, not discarded. One serial read can easily contain more than one
        frame - replies are small and the device drains its FIFO in 64-byte chunks - and returning the
        first while dropping the rest would silently desynchronize the session: every later request
        would be answered by its predecessor's reply, which reads as wrong values rather than as an
        error, and only when the timing happened to batch two frames together.
        """
        while True:
            if self._pending:
                return self._pending.pop(0)
            chunk = self.ser.read(max(1, self.ser.in_waiting or 1))
            if not chunk:
                raise Timeout("no reply")
            self._pending.extend(self._dec.feed(chunk))

    def request(self, address, *args):
        """Send, then read exactly one reply. Raises on ``/sk/err``.

        Used for reads and for the platform composites that answer with a count.
        """
        self.send(address, *args)
        addr, vals = osc.decode(self._recv())
        if addr == "/sk/err":
            # The request address is echoed, which is what makes an error actionable:
            # nothing else correlates a rejection back to what caused it.
            raise CommandError(vals[1] if len(vals) > 1 else "unknown")
        return vals[0] if len(vals) == 1 else vals

    def write(self, address, *args):
        """Send a write and consume its ack (this client enables acks at connect)."""
        return self.request(address, *args)

    # --- scope ---------------------------------------------------------------
    # The one place the two address spaces genuinely differ. The line codec makes a caller pass a deck
    # for a GLOBAL param and then discards it; the OSC address space encodes scope structurally, so a
    # global carries no deck segment at all and `/sk/a/param/crossfade` is `unknown-address`. Callers
    # written against the line surface pass a deck regardless, so the client drops it - otherwise every
    # global would fail against OSC and pass against lines, for no reason a caller could see.
    def _scope(self):
        if self._globals is None:
            d = self.describe()
            self._globals = ({n for n, p in d.params.items() if p.scope == "global"},
                             {n for n, q in d.queries.items() if q.scope == "global"})
        return self._globals

    def _pfx(self, deck, name=None, kind="param"):
        if name is not None:
            params, queries = self._scope()
            glob = (name in params) if kind == "param" else (name in queries)
            # `route` is the only global config, and it is global for the same reason: it is the
            # instrument's channel topology, not a property of either deck.
            if kind == "cfg" and name == "route":
                glob = True
            if glob:
                return "/sk"
        return "/sk/{}".format(deck.lower()) if deck else "/sk"

    # --- stimulus ------------------------------------------------------------
    def set_param(self, name, deck, value):
        self.write("{}/param/{}".format(self._pfx(deck, name), name), float(value))

    def get_param(self, name, deck):
        return float(self.request("{}/param/{}".format(self._pfx(deck, name), name)))

    def set_config(self, name, deck, v):
        self.write("{}/cfg/{}".format(self._pfx(deck, name, "cfg"), name), int(v))
        return True     # the OSC codec does not report the changed flag; ack means applied

    def cv(self, kind, deck, value):
        self.write("{}/cv/{}".format(self._pfx(deck), kind), float(value))

    def gate(self, deck):
        self.write("{}/gate".format(self._pfx(deck)))

    def midi_note(self, ch, note):
        self.write("/sk/midi/note", int(ch), int(note))

    def pad(self, action, deck, rev=False):
        """Press a pad; returns the reply payload, matching :meth:`skdev.Device.pad`.

        `play` answers with the deck's emptiness. The OSC reply carries the whole line the line codec
        would have sent (``"ok empty=0"``) because that handler composes free-form text, which the sink
        cannot type; the line client strips the ``ok `` framing, so strip it here too. Without this the
        two clients report a different value for a press the two CODECS agree about - a client
        difference masquerading as a firmware one, which is the failure mode the parity sweep exists to
        catch and must not itself introduce.
        """
        addr = "{}/pad/{}".format(self._pfx(deck), action)
        if action == "play":
            r = self.request(addr, bool(rev))
            return r[3:] if isinstance(r, str) and r.startswith("ok ") else r
        self.write(addr, bool(rev)) if rev else self.write(addr)
        return ""       # bare ok, exactly as the line client reports it

    def fx(self, kind, deck, on):
        self.write("{}/fx/{}".format(self._pfx(deck), kind), bool(on))

    # --- composites ----------------------------------------------------------
    def reset(self, deck=""):
        return int(self.request("/sk/dev/reset", deck) if deck else self.request("/sk/dev/reset"))

    def preset_save(self, slot=0):
        return int(self.request("/sk/dev/preset/save", int(slot)))

    def preset_load(self, slot=0):
        return int(self.request("/sk/dev/preset/load", int(slot)))

    # --- observation ---------------------------------------------------------
    def query(self, name, deck=""):
        """Read a state address, returning TEXT - exactly what ``Device.query`` returns.

        OSC replies are typed, so the value arrives as a Python int/float/str rather than a string.
        Rendering it back to text is what keeps the suites codec-agnostic: they assert things like
        ``out in ("0", "1")`` for a bool, and a client that handed back ``0`` would fail every one of
        those against OSC and pass against lines - a difference in the CLIENT masquerading as a
        difference in the firmware.
        """
        if name in ("cpu", "cpumin", "cpumax", "usb"):
            v = self.request("/sk/dev/{}".format(name))
        else:
            v = self.request("{}/state/{}".format(self._pfx(deck, name, "state"), name))
        if isinstance(v, bool):
            return "1" if v else "0"
        if isinstance(v, float):
            return "{:.4f}".format(v)      # the line codec's append_f32 default
        return str(v)

    def caps(self):
        return int(self.request("/sk/dev/caps"))

    # --- introspection -------------------------------------------------------
    def describe_pairs(self):
        """Return the raw decoded ``(address, args)`` rows of the describe bundle.

        One bundle, so one SLIP frame - the descriptor arrives atomically or not at
        all, which is why the device sizes its TX FIFO to hold a whole one.
        """
        self.send("/sk/dev/describe")
        return osc.decode_packet(self._recv())

    def describe(self):
        """Return a :class:`skdev.DeviceDescriptor` - the SAME model the line codec produces.

        This is what makes the parity claim testable rather than rhetorical: ``test_generic.py``
        consumes a descriptor and sweeps it, so producing the identical model here lets that suite run
        **unmodified** against either codec, and any difference in the results is a codec bug.

        The bundle carries full addresses where the line codec carries bare names, and deck-scoped rows
        arrive once per deck (the device expands them so a host need not know decks exist). Both are
        reduced back to the line codec's shape here: the name is the address's last segment, and the two
        deck rows collapse into one entry.
        """
        d = DeviceDescriptor()
        for addr, args in self.describe_pairs():
            if addr == "/sk/reply/dev/describe" and len(args) >= 3:
                d.engine, d.version = args[0], args[1]
                d.masked = args[2] == "masked=1"
            elif addr == "/sk/reply/dev/describe/param" and len(args) >= 5:
                name = args[0].rsplit("/", 1)[-1]
                d.params[name] = ParamDesc(name, args[4], float(args[2]), float(args[3]))
            elif addr == "/sk/reply/dev/describe/cfg" and len(args) >= 3:
                name = args[0].rsplit("/", 1)[-1]
                vals = {int(k): v for k, v in
                        (p.split(":", 1) for p in args[2].split() if ":" in p)}
                d.configs[name] = ConfigDesc(name, vals)
            elif addr == "/sk/reply/dev/describe/state" and len(args) >= 3:
                name = args[0].rsplit("/", 1)[-1]
                scope = "deck" if args[0].split("/")[2] in ("a", "b") else "global"
                # A 4th string carries an Enum query's selector labels, empty for other kinds. Older
                # firmware sent only three, so treat it as optional rather than requiring it.
                labels = args[3] if len(args) >= 4 else ""
                vals = {int(k): v for k, v in (t.split(":", 1) for t in labels.split() if ":" in t)}
                d.queries[name] = QueryDesc(name, scope, args[2], vals)
            elif addr == "/sk/reply/dev/describe/caps" and args:
                d.caps = int(args[0])
        return d

    def translator(self):
        """Build the host-side semantic tier from this device's descriptor."""
        return build_translator(self.describe_pairs())

    # --- determinism ---------------------------------------------------------
    @contextmanager
    def test_mode(self):
        self.write("/sk/dev/mode/test")
        try:
            yield self
        finally:
            self.write("/sk/dev/mode/run")
