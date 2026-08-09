"""semantic.py - the host-side semantic tier for the OSC address space.

The device speaks one generic, engine-independent address space
(``/sk/a/param/speed``). That is complete and sufficient on its own: a host never
needs anything else to drive the device. It is also not what a musician wants to
type - on a radio build that address is the station dial.

This module builds the second tier: engine-specific, human-readable, and **entirely
host-side** (``/radio/a/station``). See ``docs/dev/terminal-osc.md``, "The semantic
tier".

It is *generated*, never written - the input is the ``describe`` bundle the device
sends at connect, so it cannot drift from the firmware it re-reads. Three rules keep
it honest, and they are the reason this lives here rather than in flash:

  * **Never required.** Every device function is reachable generically. A bug here
    must never be able to make the device unreachable, so unknown addresses pass
    through untranslated.
  * **Never in the test path.** The cross-codec parity sweep uses generic addresses
    only. Testing through the translator would make a translation bug look like a
    firmware bug - the exact confusion the two-tier split exists to prevent.
  * **Never round-trip through labels.** The authority is always the generic address.
    A saved patch stores the semantic address it was built with; if a firmware update
    changes a label, the rebuilt map no longer resolves it and the patch fails loudly
    at connect rather than silently retargeting a different control.

Dependency-free, like ``descriptor.py``: it consumes already-decoded describe rows.
"""

import re

_SLUG_STRIP = re.compile(r"[^a-z0-9-]")


def slugify(label):
    """``"station select"`` -> ``station-select``.

    Lowercase, trim, spaces and slashes to hyphens, then drop anything outside
    ``[a-z0-9-]``. Addresses stay lowercase, as in the generic tier.
    """
    s = label.strip().lower()
    s = re.sub(r"[\s/]+", "-", s)
    s = _SLUG_STRIP.sub("", s)
    s = re.sub(r"-{2,}", "-", s).strip("-")
    return s


class Row:
    """One describe row: a generic address, its label, and what kind it is."""

    __slots__ = ("kind", "address", "label", "lo", "hi", "scope", "vkind")

    def __init__(self, kind, address, label, lo=None, hi=None, scope=None, vkind=None):
        self.kind = kind            # "param" | "cfg" | "state"
        self.address = address      # generic, e.g. /sk/a/param/speed
        self.label = label          # layer-3 name, or the slot name as fallback
        self.lo = lo
        self.hi = hi
        self.scope = scope          # "deck" | "global" (params only)
        self.vkind = vkind          # value kind (state) or selector labels (cfg)

    @property
    def deck(self):
        """``"a"``/``"b"`` for a deck-scoped address, else ``None``."""
        parts = self.address.split("/")
        return parts[2] if len(parts) > 3 and parts[2] in ("a", "b") else None

    @property
    def slot(self):
        """The layer-2 slot name - the last segment of the generic address."""
        return self.address.rsplit("/", 1)[-1]

    def __repr__(self):
        return "Row({} {} label={})".format(self.kind, self.address, self.label)


def rows_from_bundle(pairs):
    """Turn decoded ``(address, args)`` describe pairs into ``(engine, [Row])``.

    ``pairs`` is what ``skdev.osc.decode_packet`` returns for a describe bundle.
    """
    engine = "sk"
    rows = []
    for addr, args in pairs:
        if addr == "/sk/reply/dev/describe" and args:
            engine = args[0]
        elif addr == "/sk/reply/dev/describe/param" and len(args) >= 5:
            rows.append(Row("param", args[0], args[1], args[2], args[3], args[4]))
        elif addr == "/sk/reply/dev/describe/cfg" and len(args) >= 3:
            rows.append(Row("cfg", args[0], args[1], vkind=args[2]))
        elif addr == "/sk/reply/dev/describe/state" and len(args) >= 3:
            rows.append(Row("state", args[0], args[1], vkind=args[2]))
    return engine, rows


class Translator:
    """Bidirectional map between the semantic and generic address spaces."""

    def __init__(self, engine, rows):
        self.engine = engine
        self.rows = rows
        self.to_generic = {}        # semantic address -> generic address
        self.to_semantic = {}       # generic address  -> semantic address
        self._build()

    # --- construction --------------------------------------------------------
    def _compose(self, row, slug):
        """``/<engine>/<deck>/<slug>`` deck-scoped, ``/<engine>/<slug>`` global.

        ``cfg`` and ``state`` keep their kind segment. The ``mix``/``route``
        collisions that made the kind segment necessary in the generic tier are
        properties of the NAMES, and a label can collide the same way.
        """
        parts = [self.engine]
        if row.deck:
            parts.append(row.deck)
        if row.kind in ("cfg", "state"):
            parts.append(row.kind)
        parts.append(slug)
        return "/" + "/".join(parts)

    def _build(self):
        # Disambiguation has to be decided across the WHOLE row set before any address
        # is minted, or the suffix would depend on iteration order and a saved patch
        # would stop resolving after an unrelated firmware change.
        base = {}
        for row in self.rows:
            slug = slugify(row.label) or row.slot
            base.setdefault((row.kind, row.deck, slug), []).append(row)

        for (kind, deck, slug), group in base.items():
            # Two slots may legitimately carry the same label - two "level" controls is
            # the obvious case. Suffix with the slot name, which is unique by
            # construction, so the result is deterministic across reconnects.
            collide = len({r.slot for r in group}) > 1
            for row in group:
                s = "{}-{}".format(slug, row.slot) if collide else slug
                sem = self._compose(row, s)
                self.to_generic[sem] = row.address
                self.to_semantic[row.address] = sem

    # --- translation ---------------------------------------------------------
    def generic(self, address):
        """Semantic -> generic. Unknown addresses pass through **untranslated**.

        A patch may legitimately want to reach a generic address the semantic tier has
        no name for, so a ``/sk/...`` address goes out verbatim.
        """
        return self.to_generic.get(address, address)

    def semantic(self, address):
        """Generic -> semantic, for translating replies back.

        A reply arrives on ``/sk/reply/<path>``; strip that prefix to recover the
        request address before looking it up, so a patch sees only its own namespace.
        """
        if address.startswith("/sk/reply/"):
            address = "/sk/" + address[len("/sk/reply/"):]
        return self.to_semantic.get(address, address)


def build(pairs):
    """Convenience: decoded describe pairs -> a :class:`Translator`."""
    engine, rows = rows_from_bundle(pairs)
    return Translator(engine, rows)
