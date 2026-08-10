# TouchOSC layouts (`make tosc`)

Status: **built, opened in the TouchOSC editor, untested against hardware (2026-08-10)** - the
layouts generate, validate, round-trip and load, but the firmware OSC codec they target is itself a
design draft ([`terminal-osc.md`](terminal-osc.md)), so nothing here has yet been pointed at a
device.

`make tosc` writes a TouchOSC layout per engine, plus one universal layout, into `dist/tosc/`. The
layouts are built by [`scripts/gen_tosc.py`](../../scripts/gen_tosc.py) from the address model in
[`scripts/sk_osc.py`](../../scripts/sk_osc.py), using [py2tosc](https://github.com/shakfu/py2tosc)
to write the file. This is the consumer [`terminal-osc.md`](terminal-osc.md) names at the end of
its "Where it lives" section, and the reason it says the semantic tier pays off most here: a fader
can print `station` while binding to `/sk/a/param/speed`, so the surface never has to translate at
run time.

## Why generate them

A `.tosc` is drawn by hand in a GUI editor, and the surface this device wants is around 135
addresses across six pages. That is an afternoon of clicking, and it goes stale the moment a
`ParamId` moves. Worse, it goes stale *silently*: a layout bound to an address the firmware no
longer answers gets `unknown-address` back, and a layout still bound to a slot whose meaning
changed gets no error at all.

Generating removes both. The address space is read out of the same tables the firmware resolves
addresses with, so the layouts cannot advertise an address the device would reject, and cannot
miss one it would answer.

## Where the addresses come from

`sk_osc.py` parses, rather than restates:

| Source | What is read |
|--------|--------------|
| `src/engine/engine_params.h` | the `ParamId` / `ConfigId` members, in order, with their comments |
| `src/terminal/names.cpp` | `kParamNames`, `kConfigNames`, `kConfigLabels`, and the two scope predicates |
| `src/terminal/dispatch.cpp` | `kPlatformQueries` - the readable state, its scope, its type, its `safe` flag |
| `src/engine/<e>/<e>_engine.h` | that engine's `live_params()` / `live_configs()` |
| `Makefile` | which `ENGINE=` values exist |

Only the *shape* rules are written down in Python - which kind segment a leaf takes, when a deck
segment appears, the closed set of stimulus verbs - because those live in `terminal-osc.md` and
nowhere in the source. Everything with a name in it is parsed. The test suite asserts the composed
totals against the document's own totals table, so the two readings are checked against each other
on every run.

The consequence worth stating: **add a `ParamId` and the layouts follow on the next `make tosc`.**
Nothing needs editing, and if the parse ever stops matching the source, the tests fail rather than
the address space quietly shrinking.

## What a layout contains

Six pages, in tab order:

| Page | Contents |
|------|----------|
| `a`, `b` | every deck-scoped param as a radial, the CV inlets and `modspeed` as faders, and the stimulus verbs (gate, pads, seq, fx) as buttons |
| `both` | the global params, which carry no deck segment, and the write-only `ab` fan-out alias |
| `cfg` | the selector-int configs as radios, captioned with the labels `describe` publishes |
| `state` | a read button per state address, with a readout under it |
| `dev` | mode, describe/caps/help, the CPU and USB reads, reset, presets, MIDI, and readouts for `/sk/err` and `/sk/log` |

Three details are load-bearing rather than cosmetic:

- **Reads send nothing.** A message with no arguments is a read; the same address with an argument
  is a write. Every read button and every bare stimulus trigger therefore carries an empty argument
  list and fires on RISE only, so one press is one message rather than a press and a trailing zero.
- **Params listen on the reply mirror.** Each param control has a second, receive-only binding on
  `/sk/reply/<same path>`, so a read - or a write while `dev/mode/ack` is on - moves the control.
  The `ab` knobs do not: a request on `ab` cannot have two answers on one reply address, so they
  send and never follow.
- **Every bound control carries its address in `tag`.** The layout is readable without unpicking
  the message partials, in the editor or from a Lua script.

`/sk/midi/msg ,iii` is the one address with no control. Three fixed bytes, none of which a control
supplies, is a layout per message rather than a control; `/sk/midi/note` gets an octave of buttons
carrying constants instead.

### Sizing

Pages are laid out in **pixels**, not in proportional weights, because the thing being decided is
cell *shape* and a weight system decides that only by accident. Each band declares a wanted cell
height as a fraction of its cell width — a knob 1.0, a fader 0.5, a button 0.55 — the page adds
them up, and if they overflow the canvas every cell scales down together so their shapes stay in
step. A page that comes to less than the canvas is padded with an invisible spacer rather than
having its few bands stretched, so a knob is the same size on the page with six of them as on the
page with eighteen.

The knobs are then inset to a square inside whatever cell they got. TouchOSC draws a RADIAL to
fill its frame, so a wide cell gives an ellipse with the ends of its arc clipped — visible in the
first build. A test asserts every radial resolves square, since an inset is a fraction applied at
resolve time and is easy to write without it ever reaching the frame.

## Universal versus per-engine

The universal layout is the point of the whole design. Because layers 1 and 2 are constant across
builds, `/sk/a/param/speed` is the PITCH knob on every engine, so one surface drives all of them -
and `make tosc` emits it as `sk-universal.tosc`.

A per-engine layout is that same surface narrowed by the engine's `live_params()`: fewer controls,
and none of them dead. An address for a masked-out slot is `unknown-address` on the device, not a
silent no-op, so the narrowing is worth having.

Four engines - `chorus`, `filter`, `voice`, `gigaverb` - compute their masks in a loop over a
binding table (the Faust and gen~ wrappers), which cannot be read out of a header. They get the
universal surface, and the generator says so. Pointing it at a device narrows them:

```console
$ skterm.py describe > /tmp/describe.txt
$ .venv/bin/python scripts/gen_tosc.py --describe /tmp/describe.txt
```

## Labels

The address a control sends to is always the generic layer-2 one. What is *printed* on it is the
layer-3 label, and `terminal-osc.md` proposes carrying that in `describe` via a `param_label()`
hook on `IEngine`. That hook does not exist yet, so the labels live in
[`scripts/tosc_labels.json`](../../scripts/tosc_labels.json), curated from the Controls table in
each `docs/engines/*.md`.

This is deliberately the weakest link in the chain, and deliberately the one that cannot break
anything: a label that rots misnames a fader, and can never make the device unreachable, because
the address it annotates is unaffected. A slot with no label prints its layer-2 name - degraded,
never broken. The tests assert every label names a slot that engine actually implements, so the
file cannot drift into naming something that is not there.

When `param_label()` lands, `--describe` becomes the authority and this file is the offline
fallback.

## Running it

```console
$ make tosc                                   # universal + one per engine -> dist/tosc/
$ make tosc TOSC_ARGS="--engine radio --xml"  # one engine, plus the readable XML export
$ .venv/bin/python scripts/gen_tosc.py --size 1366x1024   # a different canvas
```

Output goes to `dist/tosc/`, which is ignored: a `.tosc` is a build artifact, and the tests assert
what the generator produces rather than a committed copy of it. The `--xml` export is the same
layout in the readable form TouchOSC also writes, which is what to reach for when diffing a change
to the generator.

`make test-scripts` covers all of it and needs no device.

## Known gaps

- **Nothing has touched hardware.** The firmware codec is unbuilt, so the layouts are checked
  against the specification, against py2tosc's validator, and by opening one in the TouchOSC
  editor - not against a device.
- **`/sk/err` shows the request address.** The error carries `,ss` - address then reason - and a
  TouchOSC label holds one string. The reason is bound to a second label on the assumption that
  arguments are applied in order; if that is wrong it shows the address twice, which is harmless.
- **The config radios assume `x` quantises to the step index.** A radio's value is converted to an
  integer over `0..steps-1`. If TouchOSC's quantisation turns out to be off by a boundary, the
  selector will be too, and this is the first thing to check on a bench.
- **Stimulus verbs are never masked.** They are dispatch's rather than the engine's and are
  answered on every build, but an engine that ignores, say, the seq pads still gets seq buttons.
  The `caps` bitmask could narrow this and is not read yet.
