# Terminal host tooling spec (phase 1)

Status: **built and hardware-verified (2026-07-31).** `make test-hw` runs against a flashed device; the sketches below are the design, not the shipped source - read `tools/` for that. Specifies the host side of the terminal channel: a Python client library, a pytest harness that drives real hardware, and `skterm.py`, an interactive REPL. All three speak the phase-1 line protocol in [`terminal-dispatch.md`](terminal-dispatch.md) over the USB-C CDC port from [`terminal-transport.md`](terminal-transport.md).

This is **on-target** tooling - it talks to a flashed device over serial. It is therefore distinct from `host/` (off-target C++ unit tests compiled for the build machine, run by `make test`) and belongs under a separate `make test-hw` target that is skipped when no device is attached. `make test` stays hardware-free.

## Layout

```
tools/
  skdev/                 # shared client library (importable by harness and REPL)
    __init__.py
    protocol.py          # port discovery, line framing, log filtering, reply/error parsing
    descriptor.py        # DeviceDescriptor dataclasses + parse_describe()
    device.py            # Device: connection + high-level command API + test_mode() ctx
  skterm.py              # interactive REPL (describe-driven completion, macros)
  conftest.py            # pytest fixtures: device, descriptor, test_mode
  test_generic.py        # cross-engine sweep driven by describe (the payoff)
  test_tape.py           # example per-engine test
  requirements.txt       # pyserial>=3.5
  README.md
```

## The protocol invariant both tools rely on

The transport guarantees TX is single-threaded, so lines never corrupt mid-string, but in unified mode logs and replies **share the stream**. The client distinguishes them by one rule:

- **Log lines begin with `[`** (the `LOG_TAGGED` format is `[tag] ...`, `src/common.h:29`).

- **Reply lines never do** - they start with `ok`, `err`, or a `describe` tag (`descr`/`param`/`config`/`query`/`caps`/`end`).

The client discards (or captures) `[`-prefixed lines and treats the next non-log line as the reply. Commands are **synchronous, one outstanding at a time** - send a line, read until the reply - so there is no reply-to-command ambiguity and no need for sequence numbers in phase 1. (A future `seq N` tag is noted under "Later" for pipelined use.)

## `protocol.py` - transport client

```python
import sys, glob, time
import serial
from serial.tools import list_ports

DAISY_VID = 0x0483            # STMicroelectronics (Daisy Seed CDC); PID 0x5740 typical
BAUD      = 115200            # ignored by USB CDC, but pyserial requires a value
LOG_PREFIX = "["             # LOG_TAGGED lines; filtered out of the reply stream

class Timeout(Exception): pass
class CommandError(Exception):
    def __init__(self, reason): super().__init__(reason); self.reason = reason

def find_port(explicit=None):
    if explicit:
        return explicit
    for p in list_ports.comports():        # prefer VID match - robust across OSes
        if p.vid == DAISY_VID:
            return p.device
    pats = {"darwin": "/dev/tty.usbmodem*", "linux": "/dev/ttyACM*"}.get(sys.platform, "")
    hits = sorted(glob.glob(pats)) if pats else []
    if not hits:
        raise Timeout("no device port found")
    return hits[0]

def open_serial(port, timeout=1.0):
    return serial.Serial(port, BAUD, timeout=timeout, dtr=True)   # DTR asserted for CDC

def is_log(line: str) -> bool:
    return line.startswith(LOG_PREFIX)
```

## `descriptor.py` - the parsed introspection model

```python
from dataclasses import dataclass, field

@dataclass
class ParamDesc:
    name: str
    scope: str            # "deck" | "global"
    lo: float
    hi: float

@dataclass
class ConfigDesc:
    name: str
    values: dict          # {int: label}, e.g. {0:"slice",1:"reel",2:"drift"}

@dataclass
class DeviceDescriptor:
    engine: str = ""
    version: str = ""
    params: dict = field(default_factory=dict)    # name -> ParamDesc
    configs: dict = field(default_factory=dict)   # name -> ConfigDesc
    queries: list = field(default_factory=list)   # ["empty", "mix", ...]
    caps: int = 0

def parse_describe(lines):
    """lines: the describe block, logs already filtered, ending before/at 'end'."""
    d = DeviceDescriptor()
    for ln in lines:
        tok = ln.split()
        if tok[0] == "descr":
            kv = dict(t.split("=", 1) for t in tok[1:] if "=" in t)
            d.engine, d.version = kv.get("engine", ""), kv.get("version", "")
        elif tok[0] == "param":                   # param <name> <scope> <lo>..<hi>
            name, scope, rng = tok[1], tok[2], tok[3]
            lo, hi = (float(x) for x in rng.split(".."))
            d.params[name] = ParamDesc(name, scope, lo, hi)
        elif tok[0] == "config":                  # config <name> i:label i:label ...
            vals = {int(k): v for k, v in (p.split(":", 1) for p in tok[2:])}
            d.configs[tok[1]] = ConfigDesc(tok[1], vals)
        elif tok[0] == "query":                   # query <name> <scope>
            d.queries.append(tok[1])
        elif tok[0] == "caps":                    # caps 0x....
            d.caps = int(tok[1], 16)
    return d
```

## `device.py` - the command API

```python
from contextlib import contextmanager
from .protocol import find_port, open_serial, is_log, Timeout, CommandError
from .descriptor import parse_describe

class Device:
    def __init__(self, port=None, timeout=1.0, log_sink=None):
        self.ser = open_serial(find_port(port), timeout)
        self.log_sink = log_sink          # callable(str) for captured [tag] lines, or None

    def close(self): self.ser.close()

    # --- framing -------------------------------------------------------------
    def _send(self, line: str):
        self.ser.write((line + "\r\n").encode())

    def _readline(self) -> str:
        raw = self.ser.readline()         # blocks up to serial timeout
        if not raw:
            raise Timeout("no reply")
        return raw.decode(errors="replace").rstrip("\r\n")

    def _read_reply(self) -> str:
        while True:                       # skip interleaved log lines
            ln = self._readline()
            if is_log(ln):
                if self.log_sink: self.log_sink(ln)
                continue
            if ln == "ok":            return ""
            if ln.startswith("ok "):  return ln[3:]
            if ln.startswith("err "): raise CommandError(ln[4:])
            raise CommandError(f"unexpected: {ln!r}")

    def cmd(self, line: str) -> str:
        self._send(line); return self._read_reply()

    # --- stimulus (target A) -------------------------------------------------
    def set_param(self, name, deck, value): self.cmd(f"set param {name} {deck} {value:.6g}")
    def get_param(self, name, deck) -> float: return float(self.cmd(f"get param {name} {deck}"))
    def set_config(self, name, deck, v) -> bool: return self.cmd(f"config {name} {deck} {v}") == "1"
    def cv(self, kind, deck, value): self.cmd(f"cv {kind} {deck} {value:.6g}")
    def gate(self, deck):            self.cmd(f"gate {deck}")
    def midi_note(self, ch, note):   self.cmd(f"midi note {ch} {note}")
    def pad(self, action, deck, rev=False): self.cmd(f"pad {action} {deck}{' rev' if rev else ''}")
    def fx(self, kind, deck, on):    self.cmd(f"fx {kind} {deck} {'on' if on else 'off'}")

    # --- observation (L0/L1) -------------------------------------------------
    def query(self, name, deck="") -> str: return self.cmd(f"query {name} {deck}".rstrip())
    def caps(self) -> int:                 return int(self.cmd("caps"), 16)

    # --- introspection -------------------------------------------------------
    def describe(self):
        self._send("describe")
        lines = []
        while True:
            ln = self._readline()
            if is_log(ln):
                if self.log_sink: self.log_sink(ln)
                continue
            if ln == "end": break
            lines.append(ln)
        return parse_describe(lines)

    # --- determinism ---------------------------------------------------------
    @contextmanager
    def test_mode(self):
        self.cmd("mode test")
        try: yield
        finally: self.cmd("mode run")
```

Usage is then flat and assertable:

```python
dev = Device()
with dev.test_mode():
    dev.set_config("mode", "A", 1)
    dev.set_param("feedback", "A", 0.75)
    assert abs(dev.get_param("feedback", "A") - 0.75) < 1e-3
    dev.pad("rec", "A")
    assert dev.query("empty", "A") == "0"
```

## pytest harness

### `conftest.py`

```python
import pytest
from skdev.device import Device
from skdev.protocol import Timeout

@pytest.fixture(scope="session")
def device():
    try:
        dev = Device()
    except Timeout:
        pytest.skip("no sk-engines device attached")   # `make test-hw` no-ops without hardware
    yield dev
    dev.close()

@pytest.fixture(scope="session")
def descriptor(device):
    return device.describe()

@pytest.fixture
def test_mode(device):                                  # per-test input isolation
    with device.test_mode():
        yield device
```

### `test_generic.py` - the cross-engine payoff

Driven entirely by `describe`; the same file tests every engine build. Parametrized at collection time from a describe done once, so each param is its own test case.

```python
import pytest
from skdev.device import Device
from skdev.protocol import Timeout

def _params():                                 # collection-time: open, describe, close
    try: dev = Device()
    except Timeout: return []
    try:    return list(dev.describe().params.values())
    finally: dev.close()

@pytest.mark.parametrize("p", _params(), ids=lambda p: p.name)
def test_param_roundtrip(test_mode, p):
    dev = test_mode
    decks = ["A", "B"] if p.scope == "deck" else ["A"]
    target = p.lo + 0.5 * (p.hi - p.lo)        # mid-range, inside the declared range
    for d in decks:
        dev.set_param(p.name, d, target)
        got = dev.get_param(p.name, d)
        assert abs(got - target) <= 1e-3 * (p.hi - p.lo) + 1e-4, \
            f"{p.name}[{d}] set {target} got {got}"
```

Because `describe` lists only params the engine's `live_params()` mask marks live, this sweep never sets an ignored param, so a read-back mismatch is a real defect - not descriptor noise. Tolerance accounts for on-device value quantization (e.g. the granular MValue grid); tighten per-engine if a build stores exact floats.

### `test_tape.py` - example per-engine test

```python
def test_tape_records_and_reports_nonempty(test_mode):
    dev = test_mode
    dev.set_config("mode", "A", 1)             # a mode that records
    dev.pad("clear", "A"); assert dev.query("empty", "A") == "1"
    dev.pad("rec", "A")
    dev.midi_note(1, 60)                        # inject stimulus deterministically
    assert dev.query("empty", "A") == "0"
```

### `make test-hw`

```make
test-hw:                                       # on-target; requires a flashed, attached device
	cd tools && python -m pytest -q
```

Separate from `test` (off-target `host/`). CI without hardware skips (the `device` fixture calls `pytest.skip`), so it is safe to leave in the default pipeline.

## `skterm.py` - interactive REPL

A thin human front-end over the same `Device`, with `describe`-driven completion and light macros.

### Behaviour

- **Connect and introspect.** On start, open the port and run `describe`; build the completion vocabulary from verbs + `descriptor.params` + `descriptor.configs` + decks (`A`/`B`).

- **Line editing.** `readline` with history persisted to `~/.skterm_history`; a completer that offers the verb set at position 0 and, after `set param`/`get param`/`config`, the relevant names then `A`/`B`.

- **Send and render.** Each entered line is sent verbatim; the reply is printed - `ok`/results in green, `err <reason>` in red. `describe` is rendered as its multi-line block until `end`.

- **Log visibility.** In unified mode, `[tag]` lines are shown dimmed. A background reader thread prints
  async logs while idle; toggle with `!log on|off`. Default off (quiet), on for debugging.

- **Macros.** `@path` sources a file of commands (one per line, `#` comments) - enough for canned test sequences without a macro language. Named macros live in `~/.skterm_macros` as `name: cmd; cmd; ...` and run by `!name`.

- **Local commands** (prefix `!`, never sent to the device): `!quit`, `!reconnect`, `!log on|off`,
  `!describe` (re-run and rebuild completion), `!port <dev>`.

### Sketch

```python
#!/usr/bin/env python3
import sys, os, readline, threading
from skdev.device import Device
from skdev.protocol import CommandError, Timeout

GREEN, RED, DIM, RST = "\033[32m", "\033[31m", "\033[2m", "\033[0m"

class Repl:
    def __init__(self, port=None):
        self.dev = Device(port, log_sink=self._on_log)
        self.desc = self.dev.describe()
        self.show_log = False
        self._install_completer()
        readline.read_history_file(os.path.expanduser("~/.skterm_history")) \
            if os.path.exists(os.path.expanduser("~/.skterm_history")) else None

    def _on_log(self, line):
        if self.show_log: print(f"{DIM}{line}{RST}")

    def _vocab(self, text, state):
        words = (["set","get","query","cv","gate","midi","pad","fx","config","mode",
                  "caps","describe","help"]
                 + list(self.desc.params) + list(self.desc.configs) + ["A","B"])
        opts = [w for w in words if w.startswith(text)]
        return opts[state] if state < len(opts) else None

    def _install_completer(self):
        readline.set_completer(self._vocab)
        readline.parse_and_bind("tab: complete")

    def run(self):
        while True:
            try: line = input("sk> ").strip()
            except (EOFError, KeyboardInterrupt): break
            if not line: continue
            if line.startswith("!"):  self._local(line); continue
            if line.startswith("@"):  self._source(line[1:]); continue
            self._send(line)
        readline.write_history_file(os.path.expanduser("~/.skterm_history"))

    def _send(self, line):
        try:
            if line == "describe":
                self.desc = self.dev.describe(); self._install_completer()
                print(f"{GREEN}{self.desc.engine} {self.desc.version}{RST} "
                      f"({len(self.desc.params)} params)")
            else:
                out = self.dev.cmd(line)
                print(f"{GREEN}ok{(' ' + out) if out else ''}{RST}")
        except CommandError as e: print(f"{RED}err {e.reason}{RST}")
        except Timeout:           print(f"{RED}timeout{RST}")

    def _source(self, path):
        with open(os.path.expanduser(path)) as f:
            for ln in f:
                ln = ln.split("#", 1)[0].strip()
                if ln: print(f"{DIM}@ {ln}{RST}"); self._send(ln)

    def _local(self, line):
        cmd, *arg = line[1:].split(None, 1)
        if   cmd == "quit": sys.exit(0)
        elif cmd == "log":  self.show_log = (arg and arg[0] == "on")
        elif cmd == "reconnect": self.dev.close(); self.__init__()
        else: print(f"{RED}unknown local: !{cmd}{RST}")

if __name__ == "__main__":
    Repl(sys.argv[1] if len(sys.argv) > 1 else None).run()
```

## Cross-cutting notes

- **Synchronous only.** One command in flight; the client reads to the reply before sending the next. No pipelining -> no correlation problem in phase 1. Pipelining would need a device-side `seq` echo (Later).

- **Disconnection.** A yanked cable surfaces as `Timeout` on the next `_readline`; `!reconnect` re-opens and re-`describe`s. The harness `device` fixture is session-scoped, so a mid-run disconnect fails the running test rather than corrupting later ones.

- **Baud is cosmetic** (USB CDC), but `dtr=True` matters - some hosts gate CDC output on DTR.

- **Determinism belongs to the test, not the tool.** `test_mode()` (client ctx) maps to `mode test`/`mode run`; every hardware test should run inside it so knobs/CV/gate cannot perturb the run.

## Out of scope

L2 `measure` / phase-3 `stim` helpers (add `Device.measure(...)` when those verbs land), OSC transport, a GUI/Web-Serial front-end (Tier 2), and pipelined/`seq`-tagged command streams. This spec is the phase-1 line-protocol client, the pytest harness, and the REPL.
