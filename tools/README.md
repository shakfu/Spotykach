# sk-engines terminal tooling (`tools/`)

Host-side Python tooling for the **terminal** test channel: a client library, a
pytest harness, and an interactive REPL that talk to a *flashed* sk-engines
device over its USB-C CDC (virtual serial) port.

This is **on-target** tooling - it drives real hardware. It is deliberately
separate from `host/` (off-target C++ unit tests compiled for the build machine,
run by `make test`). The hardware tests live behind `make test-hw`, which is
skipped cleanly when no device is attached, so it is safe in the default pipeline.

Everything here speaks the phase-1 line protocol specified in
[`../docs/dev/terminal-dispatch.md`](../docs/dev/terminal-dispatch.md); the host
design is in [`../docs/dev/terminal-tools.md`](../docs/dev/terminal-tools.md).

## Layout

```
tools/
  skdev/                 # shared client library (importable by harness and REPL)
    protocol.py          # port discovery, line framing, log filtering, reply/error parsing
    descriptor.py        # DeviceDescriptor dataclasses + parse_describe()
    device.py            # Device: connection + command API + test_mode() context
    osc.py               # OSC 1.0 wire format + SLIP framing (the OSC=1 codec)
    semantic.py          # the host-side semantic address tier, generated from describe
    oscdevice.py         # OscDevice: the same command API over the OSC codec
  skterm.py              # interactive REPL (describe-driven completion, macros)
  conftest.py            # pytest fixtures: device, descriptor, test_mode
  test_generic.py        # cross-engine parameter sweep driven by describe
  test_tape.py           # example per-engine test
  test_descriptor.py     # parser check against real firmware output - NO device needed
  test_osc_codec.py      # OSC codec + semantic translator - NO device needed
  requirements.txt       # pyserial>=3.5
```

### Two codecs, one API

A device built `TERMINAL=1` speaks line-ASCII; one built `TERMINAL=1 OSC=1` speaks
OSC over SLIP (see [`../docs/dev/terminal-osc.md`](../docs/dev/terminal-osc.md)).
`OscDevice` deliberately exposes the **same method surface** as `Device`, because
that is the acceptance criterion for the codec rather than a convenience: layer [3]
is shared byte for byte, so `test_generic.py`'s cross-engine sweep must produce
identical results against either, and anything that differs is a codec bug.

`osc.py` and `semantic.py` are dependency-free (no pyserial), which is what lets
`test_osc_codec.py` run in CI on a machine with neither hardware nor a serial stack.

The semantic tier (`/radio/a/station` -> `/sk/a/param/speed`) is **generated** from
the device's own `describe`, never written by hand, and is never required: every
device function is reachable through the generic address space, and an address the
translator does not recognise passes through untranslated. A bug in it must not be
able to make the device unreachable.

## Install

```sh
cd tools
pip install -r requirements.txt      # only dependency is pyserial>=3.5 (Python 3.8+)
```

A virtualenv is recommended:

```sh
python -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt pytest
```

## Run the pytest harness

```sh
make test-hw          # from the repo root; runs: cd tools && python -m pytest -q
```

or directly:

```sh
cd tools && python -m pytest -q
```

* With a device attached, `test_generic.py` reads the device's `describe` output
  and round-trips **every live parameter** the current engine build declares -
  the same file tests every engine. `test_tape.py` shows a targeted per-engine
  test (skipped on non-tape builds).
* With **no device attached**, port discovery raises `Timeout`; the `device`
  fixture turns that into `pytest.skip`, so the run no-ops rather than fails.
  CI without hardware is therefore safe.
* `test_descriptor.py` runs **with or without** a device: it parses the exact
  `describe` block the firmware emits, captured by the off-target C++ test
  (`make -C host test-terminal` writes `host/build/describe_sample.txt`). That
  keeps `parse_describe` honest against real device output instead of a
  hand-written sample. It skips if the sample has never been generated, and it
  does not need pyserial - `skdev.descriptor` imports standalone.
* `test_osc_codec.py` is the same idea for the OSC codec: it reads the describe
  BUNDLE the firmware emits (`make -C host test-terminal-osc` writes
  `host/build/describe_osc_sample.bin`) and checks the wire format plus the one
  property the semantic tier rests on - semantic -> generic -> semantic is the
  identity, and every semantic address resolves to exactly one generic address.
* `make test-tools` from the repo root runs exactly the device-free pair above.
  `make test-hw` runs everything, skipping what needs hardware.

The device port is auto-discovered by USB VID (`0x0483`, STMicroelectronics),
with a per-platform device-glob fallback (`/dev/ttyACM*` on Linux,
`/dev/tty.usbmodem*` on macOS).

## `skterm.py` - interactive REPL

```sh
cd tools
python skterm.py            # auto-discover the port
python skterm.py /dev/ttyACM0   # or name it explicitly
```

On start it opens the port, runs `describe`, and builds tab-completion from the
verb set plus the device's own params, configs, and decks (`A`/`B`). Each line is
sent verbatim; replies render `ok`/results in green, `err <reason>` in red.

Line history persists to `~/.skterm_history`.

Command kinds:

| Kind        | Meaning                                                              |
|-------------|---------------------------------------------------------------------|
| `set param feedback A 0.75` | any protocol line - sent verbatim to the device     |
| `describe`  | re-run introspection and rebuild completion                         |
| `@path`     | source a file of commands (one per line, `#` comments)              |
| `!quit`     | exit (also `!q`, Ctrl-D)                                             |
| `!reconnect`| close and re-open the current port, then re-`describe`              |
| `!port`     | show the current port; `!port /dev/ttyACM1` reconnects to another   |
| `!describe` | re-run `describe` and rebuild completion (like typing `describe`)   |
| `!log on`   | show `[tag]` log lines dimmed (a background reader prints idle logs) |
| `!log off`  | quiet (default)                                                     |
| `!name`     | run a named macro from `~/.skterm_macros`                            |

Named macros live in `~/.skterm_macros`, one per line:

```
# name: cmd; cmd; ...
recA: mode test; config mode A 1; pad clear A; pad rec A
```

Run with `!recA`.

## Protocol notes

* Commands are sent as `<line>\r\n`. Replies are `ok`, `ok <value>`, or
  `err <reason>`, CRLF-terminated. Commands are **synchronous, one outstanding at
  a time** - the client reads to the reply before sending the next.
* **Log lines begin with `[`** (the firmware `LOG_TAGGED` format `[tag] ...`) and
  are filtered out of the reply stream (optionally captured via a `log_sink`).
  Reply lines never start with `[`.
* `describe` emits a line-tagged block (`descr`/`param`/`config`/`query`/`caps`)
  terminated by a bare `end` line. See `descriptor.py` for the parser.
* Error reasons are a fixed token set: `unknown-verb`, `unknown-param`,
  `unknown-config`, `bad-deck`, `bad-arg`, `no-arg`, `too-many-args`,
  `line-too-long`, `overflow`. A `CommandError.reason` holds the bare token so
  host assertions match a stable identifier.
* `Device.test_mode()` maps to `mode test` / `mode run` - it freezes physical
  knobs/CV/gate so terminal-injected stimulus is the only driver. Every hardware
  test should run inside it (the `test_mode` fixture does this per test).

## Library quick start

```python
from skdev.device import Device

dev = Device()                     # auto-discovers the port
with dev.test_mode():
    dev.set_config("mode", "A", 1)
    dev.set_param("feedback", "A", 0.75)
    assert abs(dev.get_param("feedback", "A") - 0.75) < 1e-3
    dev.pad("rec", "A")
    assert dev.query("empty", "A") == "0"
dev.close()
```
