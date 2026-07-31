"""descriptor.py - the parsed introspection model for ``describe``.

The device's ``describe`` verb emits a machine-parseable, line-tagged block that
lets a host configure itself: parameter names + ranges + deck scope, config enums,
query names, and a capability bitmask. This module holds the dataclasses that model
that block and :func:`parse_describe`, which turns the (log-filtered) lines into a
:class:`DeviceDescriptor`.

Wire format (one item per line, terminated by a bare ``end`` line, which the caller
strips before handing lines here):

    descr engine=<name> version=<ver> masked=<0|1>
    query  <name> <deck|global> <kind> [int:label ...]
    param <name> <deck|global> <lo>..<hi>
    config <name> <int>:<label> <int>:<label> ...
    query <name> <deck|global>
    caps 0x<hex>
    end
"""

from dataclasses import dataclass, field


@dataclass
class ParamDesc:
    """A settable/gettable parameter: name, scope, and declared numeric range."""
    name: str
    scope: str            # "deck" | "global"
    lo: float
    hi: float


@dataclass
class ConfigDesc:
    """An enumerated config: name plus a {int: label} value map."""
    name: str
    values: dict          # {int: label}, e.g. {0:"slice", 1:"reel", 2:"drift"}


@dataclass
class QueryDesc:
    """A readable state item: platform or engine, indistinguishable on the wire by design."""
    name: str
    scope: str            # "deck" | "global"
    kind: str = "text"    # "bool" | "int" | "float" | "enum" | "text"
    values: dict = field(default_factory=dict)   # Enum only: {int: label}


@dataclass
class DeviceDescriptor:
    """The full parsed control surface reported by ``describe``."""
    engine: str = ""
    version: str = ""
    masked: bool = False   # engine narrowed its live_params/live_configs (see parse_describe)
    params: dict = field(default_factory=dict)    # name -> ParamDesc
    configs: dict = field(default_factory=dict)   # name -> ConfigDesc
    queries: dict = field(default_factory=dict)   # name -> QueryDesc
    caps: int = 0


def parse_describe(lines):
    """Parse a describe block into a :class:`DeviceDescriptor`.

    ``lines`` is the block with log lines already filtered out and ending
    before/at ``end`` (the terminator is not required to be present). Blank
    lines and any unrecognized tags are ignored so a forward-compatible device
    that adds a new tag does not break an older host.
    """
    d = DeviceDescriptor()
    for ln in lines:
        tok = ln.split()
        if not tok:
            continue
        if tok[0] == "descr":
            kv = dict(t.split("=", 1) for t in tok[1:] if "=" in t)
            d.engine, d.version = kv.get("engine", ""), kv.get("version", "")
            # masked=1 means the engine declared which ids it actually implements. With masked=0 the
            # descriptor is the whole ParamId enum and a round-trip sweep proves nothing, so the
            # generic test skips rather than reporting a wall of false failures.
            d.masked = kv.get("masked", "0") == "1"
        elif tok[0] == "param":                   # param <name> <scope> <lo>..<hi>
            name, scope, rng = tok[1], tok[2], tok[3]
            lo, hi = (float(x) for x in rng.split(".."))
            d.params[name] = ParamDesc(name, scope, lo, hi)
        elif tok[0] == "config":                  # config <name> [scope] i:label i:label ...
            # Only ``int:label`` tokens are values; skip any optional scope token
            # (the dispatch doc shows a scope, the tools sketch omits it - accept both).
            vals = {int(k): v for k, v in (p.split(":", 1) for p in tok[2:] if ":" in p)}
            d.configs[tok[1]] = ConfigDesc(tok[1], vals)
        elif tok[0] == "query":       # query <name> <scope> [kind] [i:label ...]
            # Scope tells a caller whether to pass a deck; kind tells it how to parse the reply.
            # Both are optional so older firmware (name+scope, or name alone) still parses.
            scope = tok[2] if len(tok) > 2 else "global"
            kind = tok[3] if len(tok) > 3 else "text"
            vals = {int(k): v for k, v in (p.split(":", 1) for p in tok[4:] if ":" in p)}
            d.queries[tok[1]] = QueryDesc(tok[1], scope, kind, vals)
        elif tok[0] == "caps":                    # caps 0x....
            d.caps = int(tok[1], 16)
    return d
