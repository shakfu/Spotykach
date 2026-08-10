#!/usr/bin/env python3
"""The layer-2 OSC address space, derived from the firmware tables rather than restated.

`docs/dev/terminal-osc.md` specifies an OSC codec for the terminal channel whose address space is
**generated from the same tables `describe` walks** - `kParamNames`/`kConfigNames` in
`src/terminal/names.cpp`, the `kPlatformQueries` table in `src/terminal/dispatch.cpp`, and the
`ParamId`/`ConfigId` enums in `src/engine/engine_params.h`. This module is the host-side half of
that promise: it parses those tables and composes the addresses by the document's rules, so a
generated control surface cannot advertise an address the firmware would answer `unknown-address`
to, and cannot miss one it would answer.

The alternative - transcribing the address table out of the design document into Python - is a
second copy of the vocabulary, free to drift the moment a `ParamId` is added. The only things
hard-coded here are the *shape* rules (which kind segment, deck or global, the closed set of
stimulus verbs), because those live in the document and nowhere else in the source.

Two tiers, per the document:

* the **generic tier** is what this module builds - `/sk/a/param/speed`, engine-independent, the
  same on every build. One control surface drives every engine.
* the **semantic tier** (`/radio/a/station`) is host-side and cosmetic. Here it is only a *label*
  carried alongside the generic address, so a surface can print "station" on a fader that is bound
  to `/sk/a/param/speed`. Labels come from `describe` once the firmware implements the
  `param_label()` hook the document proposes; until then from the curated file this module's
  consumers pass in, defaulting to the layer-2 slot name (document, translation rule 4).

Nothing here imports py2tosc, or anything outside the standard library: the address model is
useful to any host-side consumer (a Max abstraction, the `skdev` translator), and only
`gen_tosc.py` turns it into a TouchOSC layout.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

#: Firmware files parsed for the vocabulary. Named once so a move is one edit and the tests can
#: assert they exist rather than silently producing an empty space.
NAMES_CPP = REPO / "src/terminal/names.cpp"
DISPATCH_CPP = REPO / "src/terminal/dispatch.cpp"
ENGINE_PARAMS_H = REPO / "src/engine/engine_params.h"
ENGINE_DIR = REPO / "src/engine"
MAKEFILE = REPO / "Makefile"

#: The two decks, plus the write-only fan-out alias (document, "The `ab` deck alias").
DECKS = ("a", "b")
BOTH = "ab"


# --- the address model ------------------------------------------------------------------------


@dataclass(frozen=True)
class Address:
    """One layer-2 OSC address, with everything a surface needs to bind a control to it.

    `kind` is the document's kind segment (`param`, `cfg`, `state`) or the pseudo-kinds `stim`
    and `dev` for the two families that sit beside a kind segment rather than under one.
    `argtype` is what the address expects, which is what decides the control type: a `float`
    address gets a fader, a `trigger` an argument-less button, an `enum` a radio.
    """

    address: str                     # "/sk/a/param/speed"
    kind: str                        # param | cfg | state | stim | dev
    slot: str                        # layer-2 name, "" where the leaf is a verb
    deck: str                        # "a" | "b" | "ab" | ""  ("" = global or dev)
    argtype: str                     # float | int | enum | bool | trigger | text | string
    label: str = ""                  # layer-3 label; falls back to `slot` (translation rule 4)
    readable: bool = False
    writable: bool = False
    values: dict[int, str] = field(default_factory=dict)   # enum selectors, {0: "stereo", ...}
    lo: float = 0.0
    hi: float = 1.0
    note: str = ""                   # the slot's meaning, from the enum comment; for tooltips

    @property
    def caption(self) -> str:
        """What a surface prints on the control - the label if there is one, else the slot name."""
        return self.label or self.slot or self.address.rsplit("/", 1)[-1]

    @property
    def reply(self) -> str:
        """The address a reply to this one arrives on (document, "Reads: arity, not a verb")."""
        return "/sk/reply" + self.address[len("/sk"):]


@dataclass
class AddressSpace:
    """Every address one build exposes, grouped the way a surface wants to lay them out."""

    engine: str = "universal"
    params: list[Address] = field(default_factory=list)     # deck-scoped, both decks + ab
    globals: list[Address] = field(default_factory=list)    # global params
    configs: list[Address] = field(default_factory=list)
    states: list[Address] = field(default_factory=list)
    stimulus: list[Address] = field(default_factory=list)
    dev: list[Address] = field(default_factory=list)
    masked: bool = False             # True when an engine's liveness masks were applied

    def all(self) -> list[Address]:
        return [
            *self.params, *self.globals, *self.configs,
            *self.states, *self.stimulus, *self.dev,
        ]

    def deck(self, which: str, kind: str) -> list[Address]:
        """The addresses of one kind on one deck, in table order."""
        pool = {"param": self.params, "cfg": self.configs,
                "state": self.states, "stim": self.stimulus}[kind]
        return [a for a in pool if a.deck == which]

    def slots(self) -> list[str]:
        """The distinct deck-scoped param slots, in `ParamId` order - the knobs of a surface."""
        seen: list[str] = []
        for a in self.params:
            if a.deck == "a" and a.slot not in seen:
                seen.append(a.slot)
        return seen


# --- firmware table parsing -------------------------------------------------------------------


def _enum_members(source: str, name: str) -> list[tuple[str, str]]:
    """The members of a C++ `enum class`, in declaration order, with their trailing comments.

    Returns `(member, comment)` pairs and stops at `Count`, which is the count sentinel every
    enum here ends with rather than a real member.
    """
    body = re.search(rf"enum class {name}\s*:\s*\w+\s*\{{(.*?)\n\}};", source, re.DOTALL)
    if not body:
        raise ValueError(f"no `enum class {name}` in the source given")
    members: list[tuple[str, str]] = []
    for line in body.group(1).splitlines():
        m = re.match(r"\s*(\w+)\s*,?\s*(?://\s*(.*))?$", line)
        if not m or not m.group(1):
            continue
        if m.group(1) == "Count":
            break
        members.append((m.group(1), (m.group(2) or "").strip()))
    return members


def _string_array(source: str, name: str) -> list[str]:
    """The literals of a `static const char* const name[] = { ... };` table, in order."""
    body = re.search(rf"{name}\[\]\s*=\s*\{{(.*?)\}};", source, re.DOTALL)
    if not body:
        raise ValueError(f"no `{name}[]` table in the source given")
    return re.findall(r'"([^"]*)"', body.group(1))


def _switch_cases(source: str, signature: str, enum: str) -> set[str]:
    """The enum members a `bool f(...)` predicate returns true for.

    `param_is_global` and `param_is_platform_owned` are both a run of `case ParamId::X:` labels
    followed by one `return true`, so the members that matter are the ones named before it.
    """
    body = re.search(re.escape(signature) + r"\s*\{(.*?)\n\}", source, re.DOTALL)
    if not body:
        raise ValueError(f"no `{signature}` in the source given")
    head = body.group(1).split("return true", 1)[0]
    return set(re.findall(rf"{enum}::(\w+)", head))


@dataclass
class Vocabulary:
    """The layer-2 vocabulary: what the firmware calls things, and how it scopes them."""

    param_enum: list[str]                  # ParamId members, in order
    param_names: list[str]                 # kParamNames, same order
    param_notes: dict[str, str]            # wire name -> the enum's trailing comment
    global_params: set[str]                # wire names that carry no deck segment
    platform_params: set[str]              # wire names with no param address at all
    config_enum: list[str]
    config_names: list[str]
    config_labels: dict[str, dict[int, str]]
    global_configs: set[str]

    def param_wire(self, member: str) -> str:
        """The wire name of a `ParamId` member (`Speed` -> `speed`)."""
        return self.param_names[self.param_enum.index(member)]

    def config_wire(self, member: str) -> str:
        return self.config_names[self.config_enum.index(member)]


def _selector_labels(text: str) -> dict[int, str]:
    """Parse a `kConfigLabels` string - `"0:slice 1:reel 2:drift"` - into `{0: "slice", ...}`.

    Tolerates the surrounding quotes, since one caller hands over the C++ literal as written.
    """
    return {int(k): v for k, v in re.findall(r'(\d+):([^\s"]+)', text)}


def read_vocabulary(
    names_cpp: Path = NAMES_CPP,
    engine_params_h: Path = ENGINE_PARAMS_H,
) -> Vocabulary:
    """Read the layer-2 vocabulary out of `names.cpp` and `engine_params.h`."""
    names = names_cpp.read_text()
    params_h = engine_params_h.read_text()

    param_members = _enum_members(params_h, "ParamId")
    config_members = _enum_members(params_h, "ConfigId")
    param_names = _string_array(names, "kParamNames")
    config_names = _string_array(names, "kConfigNames")
    config_labels = _string_array(names, "kConfigLabels")

    # The firmware asserts these lengths at compile time; assert them here too, because a parse
    # that silently lost a member would produce a surface missing a control with no other symptom.
    if len(param_members) != len(param_names):
        raise ValueError(
            f"ParamId has {len(param_members)} members but kParamNames has {len(param_names)} entries"
        )
    if len(config_members) != len(config_names) != len(config_labels):
        raise ValueError("ConfigId, kConfigNames and kConfigLabels disagree on length")

    param_enum = [m for m, _ in param_members]
    config_enum = [m for m, _ in config_members]
    wire = dict(zip(param_enum, param_names))

    return Vocabulary(
        param_enum=param_enum,
        param_names=param_names,
        param_notes={param_names[i]: c for i, (_, c) in enumerate(param_members)},
        global_params={wire[m] for m in _switch_cases(names, "bool param_is_global(ParamId id)", "ParamId")},
        platform_params={
            wire[m] for m in _switch_cases(names, "bool param_is_platform_owned(ParamId id)", "ParamId")
        },
        config_enum=config_enum,
        config_names=config_names,
        config_labels={config_names[i]: _selector_labels(t) for i, t in enumerate(config_labels)},
        # ConfigId declares its scope in the enum comment ("global: ..." vs "per-deck: ...") and
        # nowhere else - there is no `config_is_global` to parse, so the comment is the source.
        global_configs={
            config_names[i] for i, (_, c) in enumerate(config_members) if c.lower().startswith("global")
        },
    )


@dataclass(frozen=True)
class Query:
    """One row of `kPlatformQueries` - a readable state item."""

    name: str
    scope: str          # "deck" | "global"
    kind: str           # bool | int | float | enum | text
    values: dict[int, str]
    safe: bool          # false keeps it out of describe, and so out of the address space


def read_queries(dispatch_cpp: Path = DISPATCH_CPP) -> list[Query]:
    """Read the platform query table, which is what the `state/` and `dev` read addresses are."""
    source = dispatch_cpp.read_text()
    body = re.search(r"kPlatformQueries\[\]\s*=\s*\{(.*?)\n\};", source, re.DOTALL)
    if not body:
        raise ValueError("no `kPlatformQueries[]` table in dispatch.cpp")

    rows: list[Query] = []
    row = re.compile(
        r'\{\s*"(\w+)"\s*,\s*QueryScope::(\w+)\s*,\s*ValueKind::(\w+)\s*,\s*'
        r'(nullptr|"[^"]*")\s*,\s*(true|false)\s*\}'
    )
    for name, scope, kind, labels, safe in row.findall(body.group(1)):
        rows.append(Query(
            name=name,
            scope=scope.lower(),
            kind=kind.lower(),
            values=_selector_labels(labels) if labels != "nullptr" else {},
            safe=safe == "true",
        ))
    return rows


#: Query names the document files under `/sk/dev/` rather than `/sk/state/`: they report on the
#: channel and the board, not on the engine's control surface (document, "Platform").
DEV_QUERIES = ("cpu", "cpumin", "cpumax", "usb")


# --- the two closed sets that live only in the document ---------------------------------------

#: Deck-scoped stimulus verbs: the things you *do* to a deck rather than state you address.
#: `(leaf, argtype)`, transcribed from the document's "Stimulus verbs (closed set, no slot name)"
#: because they exist as dispatch verbs, not as a table anything can be read out of. A `ParamId`
#: can never collide with one, since params live under `param/`.
STIMULUS: tuple[tuple[str, str], ...] = (
    ("cv/voct", "float"),
    ("cv/mix", "float"),
    ("cv/size", "float"),
    ("cv/xfade", "float"),        # global in the engine; keeps a deck segment for uniformity
    ("gate", "trigger"),
    ("pad/play", "trigger"),
    ("pad/rec", "trigger"),
    ("pad/stop", "trigger"),
    ("pad/clear", "trigger"),
    ("seq/trig", "trigger"),
    ("seq/arm", "trigger"),
    ("seq/clear", "trigger"),
    ("seq/disarm", "trigger"),
    ("fx/flux", "bool"),
    ("fx/grit", "bool"),
    ("fx/lock/flux", "trigger"),
    ("fx/lock/grit", "trigger"),
    ("fx/gritmode", "trigger"),
    ("modspeed", "float"),        # routes to set_mod_speed, not set_param
)

#: Global stimulus. `midi/msg ,iii` takes three ints from nothing a control can supply, so a
#: surface can only send fixed messages on it; it is here for completeness and skipped by the
#: layout generator.
GLOBAL_STIMULUS: tuple[tuple[str, str], ...] = (
    ("midi/note", "int2"),
    ("midi/msg", "int3"),
    ("midi/transport", "bool"),
)

#: Platform verbs under `/sk/dev/`, minus the four queries, which are derived from the query table.
DEV: tuple[tuple[str, str], ...] = (
    ("mode/test", "trigger"),
    ("mode/run", "trigger"),
    ("mode/ack", "bool"),         # OSC-only: turns per-write acks on for the session
    ("describe", "trigger"),
    ("caps", "trigger"),
    ("help", "trigger"),
    ("reset", "trigger"),         # also accepts ,s <deck>
    ("reset/cpu", "trigger"),
    ("preset/save", "int"),
    ("preset/load", "int"),
)


# --- composition ------------------------------------------------------------------------------


def build_space(
    vocab: Vocabulary | None = None,
    queries: list[Query] | None = None,
    *,
    labels: dict[str, str] | None = None,
    include_both: bool = True,
) -> AddressSpace:
    """Compose the whole layer-2 address space by the document's shape rules.

    Args:
        vocab: The layer-2 vocabulary; read from the firmware if not given.
        queries: The platform query table; read from the firmware if not given.
        labels: Layer-3 labels, keyed by `"param:<slot>"` / `"cfg:<slot>"`. Missing keys fall
            back to the slot name, which is what an engine with no `param_label()` produces.
        include_both: Whether to emit the write-only `ab` fan-out alias.

    Returns:
        Every address the platform tables can produce, before any engine's liveness mask.
    """
    vocab = vocab or read_vocabulary()
    queries = queries if queries is not None else read_queries()
    labels = labels or {}
    space = AddressSpace()

    def label_for(kind: str, slot: str) -> str:
        return labels.get(f"{kind}:{slot}", "")

    # Params. Platform-owned ids have no param address at all: `set_param` never sees them, so
    # `describe` does not advertise them and neither can a surface.
    for slot in vocab.param_names:
        if slot in vocab.platform_params:
            continue
        common = dict(
            kind="param", slot=slot, argtype="float", readable=True, writable=True,
            label=label_for("param", slot), note=vocab.param_notes.get(slot, ""),
        )
        if slot in vocab.global_params:
            space.globals.append(Address(address=f"/sk/param/{slot}", deck="", **common))
            continue
        for deck in DECKS:
            space.params.append(Address(address=f"/sk/{deck}/param/{slot}", deck=deck, **common))
        if include_both:
            # Write-only: one request on `ab` cannot have two answers on one reply address.
            space.params.append(Address(
                address=f"/sk/{BOTH}/param/{slot}", deck=BOTH,
                **{**common, "readable": False},
            ))

    # Configs: write-only enums, the selector ints `config` accepts.
    for slot in vocab.config_names:
        common = dict(
            kind="cfg", slot=slot, argtype="enum", writable=True,
            values=vocab.config_labels.get(slot, {}), label=label_for("cfg", slot),
        )
        if slot in vocab.global_configs:
            space.configs.append(Address(address=f"/sk/cfg/{slot}", deck="", **common))
        else:
            for deck in DECKS:
                space.configs.append(Address(address=f"/sk/{deck}/cfg/{slot}", deck=deck, **common))

    # State and the four platform reads. `safe=false` (reseed) is latching - asking changes the
    # answer - so the firmware keeps it out of describe and it gets no address either.
    for q in queries:
        if not q.safe:
            continue
        common = dict(
            slot=q.name, argtype=q.kind, readable=True, values=q.values, label=q.name,
        )
        if q.name in DEV_QUERIES:
            space.dev.append(Address(address=f"/sk/dev/{q.name}", kind="dev", deck="", **common))
        elif q.scope == "deck":
            for deck in DECKS:
                space.states.append(
                    Address(address=f"/sk/{deck}/state/{q.name}", kind="state", deck=deck, **common)
                )
        else:
            space.states.append(Address(address=f"/sk/state/{q.name}", kind="state", deck="", **common))

    # Stimulus verbs, deck-scoped and global.
    for deck in DECKS:
        for leaf, argtype in STIMULUS:
            space.stimulus.append(Address(
                address=f"/sk/{deck}/{leaf}", kind="stim", slot=leaf, deck=deck,
                argtype=argtype, writable=True, label=leaf,
            ))
    for leaf, argtype in GLOBAL_STIMULUS:
        space.stimulus.append(Address(
            address=f"/sk/{leaf}", kind="stim", slot=leaf, deck="",
            argtype=argtype, writable=True, label=leaf,
        ))

    for leaf, argtype in DEV:
        space.dev.append(Address(
            address=f"/sk/dev/{leaf}", kind="dev", slot=leaf, deck="",
            argtype=argtype, writable=True, label=leaf,
        ))

    return space


# --- per-engine masking -----------------------------------------------------------------------


def _mask_body(source: str, signature: str) -> str | None:
    """The body of a `live_params`/`live_configs` override, or None if it is not there.

    The signature is matched with whitespace-insensitivity, because the declarations are aligned
    by hand and at least one engine writes `ParamMask  live_params()  const override`.
    """
    pattern = re.compile(r"\s+".join(re.escape(t) for t in signature.split()))
    found = pattern.search(source)
    if not found:
        return None
    brace = source.find("{", found.end() - 1)
    depth, i = 0, brace
    while i < len(source):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:i]
        i += 1
    return None


@dataclass(frozen=True)
class LiveMask:
    """What one engine declares it actually implements, read out of its header.

    `params`/`configs` are wire names. `derived` marks a mask a loop computes at run time from a
    binding table (the Faust and gen~ wrappers), which cannot be read statically - those engines
    need a `describe` from a running device, and until then get the whole space.
    """

    engine: str
    params: set[str] | None
    configs: set[str] | None
    derived: bool = False


def read_live_mask(engine: str, vocab: Vocabulary | None = None) -> LiveMask:
    """Read `live_params()`/`live_configs()` out of `src/engine/<engine>/<engine>_engine.h`."""
    vocab = vocab or read_vocabulary()
    header = ENGINE_DIR / engine / f"{engine}_engine.h"
    if not header.exists():
        return LiveMask(engine, None, None, derived=True)
    source = header.read_text()

    def collect(signature: str, enum: str, wire) -> tuple[set[str] | None, bool]:
        body = _mask_body(source, signature)
        if body is None:
            return None, True
        # A loop means the mask is computed from a binding table, not listed.
        if re.search(r"\bfor\s*\(", body):
            return None, True
        return {wire(m) for m in re.findall(rf"{enum}::(\w+)", body)}, False

    params, p_derived = collect("ParamMask live_params() const override", "ParamId", vocab.param_wire)
    configs, c_derived = collect("ConfigMask live_configs() const override", "ConfigId", vocab.config_wire)
    return LiveMask(engine, params, configs, derived=p_derived or c_derived)


def engines() -> list[str]:
    """Every engine the build system accepts, in alphabetical order.

    Read from the Makefile's `ENGINE` branches rather than from the directory listing: `src/engine`
    also holds the shared wrappers (`gen/`, `faust/`) that engines are built *from* and that no
    `ENGINE=` value names.
    """
    text = MAKEFILE.read_text()
    return sorted(set(re.findall(r"^\s*(?:else )?ifeq \(\$\(ENGINE\),\s*(\w+)\)", text, re.MULTILINE)))


def mask_space(space: AddressSpace, mask: LiveMask) -> AddressSpace:
    """Narrow a space to one engine's live slots.

    An address for a masked-out slot is `unknown-address` on the device, not a silent no-op, so a
    surface that offers one is a surface with dead controls. Stimulus verbs and the platform
    section are untouched: they are dispatch's, not the engine's, and are answered on every build.
    """
    if mask.params is None and mask.configs is None:
        return AddressSpace(**{**space.__dict__, "engine": mask.engine, "masked": False})

    keep_p = mask.params if mask.params is not None else {a.slot for a in space.params}
    keep_c = mask.configs if mask.configs is not None else {a.slot for a in space.configs}
    return AddressSpace(
        engine=mask.engine,
        params=[a for a in space.params if a.slot in keep_p],
        globals=[a for a in space.globals if a.slot in keep_p],
        configs=[a for a in space.configs if a.slot in keep_c],
        states=list(space.states),
        stimulus=list(space.stimulus),
        dev=list(space.dev),
        masked=True,
    )


def space_for(
    engine: str | None = None,
    *,
    labels: dict[str, str] | None = None,
    include_both: bool = True,
) -> AddressSpace:
    """The address space for one engine, or the whole universal space when `engine` is None."""
    vocab = read_vocabulary()
    space = build_space(vocab, labels=labels, include_both=include_both)
    if engine is None:
        return space
    return mask_space(space, read_live_mask(engine, vocab))


if __name__ == "__main__":   # a quick look at what the firmware currently exposes
    import sys

    which = sys.argv[1] if len(sys.argv) > 1 else None
    s = space_for(which)
    print(f"{s.engine}: {len(s.all())} addresses" + ("  (masked)" if s.masked else ""))
    for a in s.all():
        print(f"  {a.address:34} {a.kind:6} {a.argtype:8} {a.caption}")
