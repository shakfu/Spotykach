# TouchOSC layouts (`make tosc`)

Status: **built, opened in the TouchOSC editor, not yet pointed at a device (2026-08-10).** The layouts generate, validate, round-trip and load. The firmware codec they target is real and hardware-verified ([`terminal-osc.md`](terminal-osc.md)), so the remaining step is a bench session rather than a firmware one.

## How a layout reaches the device: run the bridge

**TouchOSC speaks OSC over UDP. The device has no network interface** - it is OSC over SLIP over USB CDC, because a Daisy has neither Ethernet nor WiFi. So a layout cannot address the hardware directly, and until 2026-08-11 this document generated surfaces with nothing to plug them into.

[`tools/skbridge.py`](../../tools/skbridge.py) is that missing piece - a UDP <-> SLIP-serial relay, the same arrangement and for the same reason as monome's `serialosc`. On the machine the device is plugged into:

```text
python3 tools/skbridge.py -v            # auto-discovers the port; listens on 8000, replies to 9000
```

Then in TouchOSC's connection settings: **host** = that machine's address, **send port** = 8000, **receive port** = 9000. The bridge learns where to answer from the first packet it receives, so send something before expecting a reply; `--reply-host` pins it when the surface and the sender are not the same machine.

The relay translates framing only - it never parses or rewrites OSC - so a layout bound to an address this document has not heard of works without a bridge change. Confirm the link with liblo before opening TouchOSC, which isolates a layout problem from a transport one:

```text
oscsend osc.udp://127.0.0.1:8000 /sk/a/param/speed f 0.5
```

`tools/test_liblo_conformance.py` runs exactly that path end to end (liblo -> UDP -> bridge -> SLIP) against a pty, so the transport is tested even where the layouts are not.

`make tosc` writes a TouchOSC layout per engine, plus one universal layout, into `dist/tosc/`. The layouts are built by [`scripts/gen_tosc.py`](../../scripts/gen_tosc.py) from the address model in [`scripts/sk_osc.py`](../../scripts/sk_osc.py), using [py2tosc](https://github.com/shakfu/py2tosc) to write the file. This is the consumer [`terminal-osc.md`](terminal-osc.md) names at the end of its "Where it lives" section, and the reason it says the semantic tier pays off most here: a fader can print `station` while binding to `/sk/a/param/speed`, so the surface never has to translate at run time.

## Why generate them

A `.tosc` is drawn by hand in a GUI editor, and the surface this device wants is around 135 addresses across six pages. That is an afternoon of clicking, and it goes stale the moment a `ParamId` moves. Worse, it goes stale *silently*: a layout bound to an address the firmware no longer answers gets `unknown-address` back, and a layout still bound to a slot whose meaning changed gets no error at all.

Generating removes both. The address space is read out of the same tables the firmware resolves addresses with, so the layouts cannot advertise an address the device would reject, and cannot miss one it would answer.

## Where the addresses come from

`sk_osc.py` parses, rather than restates:

| Source | What is read |
|--------|--------------|
| `src/engine/engine_params.h` | the `ParamId` / `ConfigId` members, in order, with their comments |
| `src/terminal/names.cpp` | `kParamNames`, `kConfigNames`, `kConfigLabels`, and the two scope predicates |
| `src/terminal/dispatch.cpp` | `kPlatformQueries` - the readable state, its scope, its type, its `safe` flag |
| `src/engine/<e>/<e>_engine.h` | that engine's `live_params()` / `live_configs()`, and its `param_label()` table |
| `Makefile` | which `ENGINE=` values exist |

Only the *shape* rules are written down in Python - which kind segment a leaf takes, when a deck segment appears, the closed set of stimulus verbs - because those live in `terminal-osc.md` and nowhere in the source. Everything with a name in it is parsed.

Those shape rules are the part nothing can share, so they are asserted against the firmware that implements them. `sk_osc.py --inventory` exports the whole composed space to [`host/osc_addresses.txt`](../../host/osc_addresses.txt), and [`host/test_osc_addr.cpp`](../../host/test_osc_addr.cpp) puts every line of it through the real `osc_dispatch_packet` - two directions, because each catches a different mistake:

1. **Every address the model composes must resolve.** One the resolver answers `unknown-address` is a rule the model has wrong. This also probes deliberate near-misses (`/sk/a/size` with no kind segment, a global param carrying a deck) to prove the resolver is not simply accepting everything.

2. **Every address `describe` advertises must be in the model.** This is the direction that catches a rule the model is *missing* - a new `ParamId`, a new query - which direction 1 cannot see, since a shorter list still passes it.

It earned its place on the first run, by finding a disagreement inside the firmware rather than in the model: the descriptor advertised `/sk/state/cpu` for the four platform reads the resolver answers at `/sk/dev/cpu`. Both spellings resolved, so nothing was broken and nothing else in the tree compared the two. `describe_state_rows` now shares one `is_platform_read()` predicate with the resolver, and `host/test_terminal_osc.cpp` asserts the spelling rather than only the reachability.

The inventory is committed so the off-target test needs no Python at build time, and `make test-scripts` fails if it has gone stale, the same guard the committed `web/` export carries.

The consequence worth stating: **add a `ParamId` and the layouts follow on the next `make tosc`.** Nothing needs editing, and if the parse ever stops matching the source, the tests fail rather than the address space quietly shrinking.

## What a layout contains

Six pages, in tab order:

| Page | Contents |
|------|----------|
| `a`, `b` | every deck-scoped param as a radial, the CV inlets and `modspeed` as faders, and the stimulus verbs (gate, pads, seq, fx) as buttons |
| `both` | the global params, which carry no deck segment, and the write-only `ab` fan-out alias |
| `cfg` | the selector-int configs as radios, captioned with the labels `describe` publishes |
| `state` | a read button per state address, with a readout under it |
| `dev` | mode, describe/caps/help, the CPU and USB reads, reset, presets, MIDI, and readouts for `/sk/err` and `/sk/log` |

Three details are structural rather than cosmetic:

- **Reads send nothing.** A message with no arguments is a read; the same address with an argument is a write. Every read button and every bare stimulus trigger therefore carries an empty argument list and fires on RISE only, so one press is one message rather than a press and a trailing zero.

- **Params listen on the reply mirror.** Each param control has a second, receive-only binding on `/sk/reply/<same path>`, so a read - or a write while `dev/mode/ack` is on - moves the control. The `ab` knobs do not: a request on `ab` cannot have two answers on one reply address, so they send and never follow.

- **Every bound control carries its address in `tag`.** The layout is readable without unpicking the message partials, in the editor or from a Lua script.

`/sk/midi/msg ,iii` is the one address with no control. Three fixed bytes, none of which a control supplies, is a layout per message rather than a control; `/sk/midi/note` gets an octave of buttons carrying constants instead.

### Sizing

Pages are laid out in **pixels**, not in proportional weights, because the thing being decided is cell *shape* and a weight system decides that only by accident. Each band declares a wanted cell height as a fraction of its cell width — a knob 1.0, a fader 0.5, a button 0.55 — the page adds them up, and if they overflow the canvas every cell scales down together so their shapes stay in step. A page that comes to less than the canvas is padded with an invisible spacer rather than having its few bands stretched, so a knob is the same size on the page with six of them as on the page with eighteen.

The knobs are then inset to a square inside whatever cell they got. TouchOSC draws a RADIAL to fill its frame, so a wide cell gives an ellipse with the ends of its arc clipped — visible in the first build. A test asserts every radial resolves square, since an inset is a fraction applied at resolve time and is easy to write without it ever reaching the frame.

## Universal versus per-engine

The universal layout is the point of the whole design. Because layers 1 and 2 are constant across builds, `/sk/a/param/speed` is the PITCH knob on every engine, so one surface drives all of them - and `make tosc` emits it as `sk-universal.tosc`.

A per-engine layout is that same surface narrowed by the engine's `live_params()`: fewer controls, and none of them dead. An address for a masked-out slot is `unknown-address` on the device, not a silent no-op, so the narrowing is worth having.

Four engines - `chorus`, `filter`, `voice`, `gigaverb` - compute their masks in a loop over a binding table (the Faust and gen~ wrappers), which cannot be read out of a header. They get the universal surface, and the generator says so; the three Faust ones are still *labelled*, because that bind table is a static array in the generated header even though the mask that walks it is not. Pointing the generator at a device narrows any of them:

```console
skterm.py describe > /tmp/describe.txt
.venv/bin/python scripts/gen_tosc.py --describe /tmp/describe.txt
```

## Labels

The address a control sends to is always the generic layer-2 one. What is *printed* on it is the layer-3 label, which the firmware owns: `IEngine::param_label()`, implemented by sixteen engines and derived from the Faust bind table by three more. `sk_osc.py` reads those tables the same way it reads the liveness masks, so there is no second copy of them anywhere in this tooling.

There was one, briefly - a curated `scripts/tosc_labels.json`, written while `param_label()` was still a proposal - and it is worth recording what happened to it, because it is the hazard this whole module is built to avoid. Within a single merge it had already drifted: it called shuttle's `Speed` "capstan" where the firmware calls it "varispeed", and its `Aux` "slot" against the firmware's "tape slot". The file is deleted; the tables are read instead.

A slot with no label prints its layer-2 name, which is what the firmware's own fallback does - `granular` declares none deliberately, since the shared `ParamId` vocabulary is already its own words. The label remains the one part of the chain that cannot break anything: it misnames a fader, and can never make the device unreachable, because the address it annotates is unaffected.

The line codec's `describe` carries no labels; only the OSC codec's bundle does, as `/sk/reply/dev/describe/param ,ssffs <address> <label> <lo> <hi> <scope>`. A host that has decoded one can hand the mapping straight to `sk_osc.space_for(labels=...)` - [`tools/skdev/semantic.py`](../../tools/skdev/semantic.py) already builds it.

## Running it

```console
make tosc                                   # universal + one per engine -> dist/tosc/
make tosc TOSC_ARGS="--engine radio --xml"  # one engine, plus the readable XML export
.venv/bin/python scripts/gen_tosc.py --size 1366x1024   # a different canvas
```

Output goes to `dist/tosc/`, which is ignored: a `.tosc` is a build artifact, and the tests assert what the generator produces rather than a committed copy of it. The `--xml` export is the same layout in the readable form TouchOSC also writes, which is what to reach for when diffing a change to the generator.

`make test-scripts` covers all of it and needs no device.

## Known gaps

- **Nothing has touched a device.** The layouts are checked against the specification, against py2tosc's validator, against the real resolver off-target, and by opening one in the TouchOSC editor. A bench session is the obvious next step.

- **`describe` and the resolver disagree on four addresses, and the resolver is the one following the spec.** `terminal-osc.md` files the CPU and USB reads under the platform section (`/sk/dev/cpu`, "they report on the channel and the board, not on the engine's control surface") and `osc_addr.cpp` resolves them there. `describe_state_rows` walks the whole platform query table and composes every row with the `state` kind, so the descriptor advertises `/sk/state/cpu` for the same read. Nothing is broken - both spellings resolve, which `test_osc_addr.cpp` proves by probing them - but by the document's own acceptance criterion ("every address describe advertises is exactly what this document's rules predict") the descriptor is the side that is wrong. The model composes the documented `/sk/dev/*`; the test pins the divergence by name so it cannot silently grow.

- **`/sk/err` shows the request address.** The error carries `,ss` - address then reason - and a TouchOSC label holds one string. The reason is bound to a second label on the assumption that arguments are applied in order; if that is wrong it shows the address twice, which is harmless.

- **The config radios assume `x` quantises to the step index.** A radio's value is converted to an integer over `0..steps-1`. If TouchOSC's quantisation turns out to be off by a boundary, the selector will be too, and this is the first thing to check on a bench.

- **Stimulus verbs are never masked.** They are dispatch's rather than the engine's and are answered on every build, but an engine that ignores, say, the seq pads still gets seq buttons. The `caps` bitmask could narrow this and is not read yet.
