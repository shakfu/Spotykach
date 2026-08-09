# Terminal OSC codec spec (`SPK_TERMINAL_OSC`)

Status: **built and VERIFIED ON HARDWARE (2026-08-09).** Specified 2026-08-07; shipped as an optional
firmware variant, `make ENGINE=<e> TERMINAL=1 OSC=1`. Off-target: `make -C host test-terminal-osc`,
`make -C host test-osc-labels`, `pytest tools/test_osc_codec.py`. On target: the cross-codec parity
sweep passed on a cased Spotykach running `tape` - **63/63 identical against both codecs**, which is the
acceptance criterion this document set for itself. See **Testing**.

Specifies the OSC address space and the SLIP framing for layer [2] of the terminal channel - the opt-in
alternate codec named in [`terminal-control.md`](terminal-control.md). It replaces *only* the codec:
layer [1] (transport, SPSC ring, TX FIFO) and layer [3] (verb table, `IEngine` binding, `mode test`,
`describe`) are unchanged and shared byte-for-byte with the line-ASCII build. Everything here is
`#if SPK_TERMINAL_OSC`, which implies `SPK_TERMINAL`.

**What the implementation changed about this document.** Four things the design got wrong or left open,
corrected in place below and listed here so they are not rediscovered:

1. **The footprint estimate was low by roughly 4x** - measured `~9 KB flash, ~12.4 KB SRAM`, not
   `~3.7 KB / ~2.5 KB`. See **Flash and RAM delta**.
2. **The descriptor bundle is bigger than projected** (5392 B unmasked, not 2-3 KB), because the
   projection was taken from a masked engine. The TX FIFO goes to **8 KB**, not 4 KB.
3. **`TextSink` needed two more methods than "make it virtual"** - `ok_begin()`/`ok_end()`, because the
   `query` path types its value at a call site the sink cannot see. See **Implementation shape**.
4. **`pad play` and `fx gritmode` reply even though they are writes**, because layer [3] returns a value
   through them that a host has no other way to obtain. See **Errors**.

Read [`terminal-dispatch.md`](terminal-dispatch.md) first - this document maps its verb catalog onto an
address space, and where the two disagree, dispatch wins.

## What OSC is for

Line-ASCII stays the default and the floor: it is testable, works with a dumb terminal, and costs a
tokenizer. OSC buys one thing - **the device becomes a node in a Max/Pd/TouchOSC rig**, where a fader is
bound to an address once and then just sends floats. Every decision below is made for that client, not
for the pytest harness, which is already well served by lines.

The design is **two tiers, and only one of them is firmware**. The device speaks a generic, layer-2,
engine-independent address space (`/sk/a/param/speed`). A host-side translator generated from `describe`
offers an engine-specific, human-readable namespace on top of it (`/radio/a/station`). Sections up to
**The semantic tier** specify the firmware half; that section specifies the host half and why the split
falls there.

## The three layers, and why the address sits on the middle one

The panel is fixed hardware and every engine reinterprets it. That produces three distinct namings, and
conflating them is the trap this design exists to avoid. `docs/engines/radio.md:21` tables all three in
one row: **PITCH** (`Speed`) → *station select*.

| Layer | Example | Varies per engine? | Machine-readable today |
|-------|---------|--------------------|------------------------|
| 1 - physical control | `PITCH`, `SIZE`, `ENV` | no (silkscreen) | no |
| 2 - `ParamId` slot | `speed`, `size`, `env` | no (one shared enum) | yes - `names.cpp` |
| 3 - engine meaning | *station select*, *character*, *brightness* | **yes** | no - `docs/engines/*.md` prose |

Layer 3 varies harder than it looks. `tape_engine.cpp:128` maps `ParamId::Size` to `_fx_n[i][1]`,
commented **"character"** - not a size at all. `AltPos` is documented as "engine-interpreted, e.g. tape
uses it as pan"; `Aux` as "edrums model select / tape slot". The enum header states the origin plainly:
it "mirrors the granular engine's MValue-backed set." So the layer-2 vocabulary is *granular's words*,
inherited by every other engine as generic slots.

**The address is layer 2. The engine's meaning travels as a label in `describe`, not in the path.**

This is the conventional OSC split - stable address, cosmetic label - and here it buys something
specific: because layers 1 and 2 are constant across builds, **one control-surface layout drives every
engine**. `/sk/a/param/speed` is always the PITCH knob. A layout generated from `describe` prints
"station" on that fader against a radio build and "pitch" against csound, while the wire never changes.

Putting layer 3 in the path would invert that: the address vocabulary would change per build, forcing a
per-engine layout, and *that* is what would have required an engine-name segment. See **Rejected
alternatives**.

## Design rules

1. **The address is the noun; the argument is the value.** Everything identifying *what is addressed* -
   deck, kind, slot - is a path segment. The value is always an OSC argument, never a segment. An address
   that varies with its value is not an address anything can bind to: a fader would mint a new address
   per frame, `[routeOSC]` would route on a float leaf, and the type tags get discarded in favour of
   hand-rolled decimal formatting.

2. **Slots are named, never numbered.** Names survive an enum reorder - the reason
   `terminal-dispatch.md:102` chose them - and OSC raises the stakes, because a saved layout outlives the
   build that produced it. Insert one `ParamId` and every numeric layout silently drives the param one
   slot over: no error, no rejection, just wrong values into plausible controls.

3. **Addresses are all-lowercase and compared literally**, segment by segment. Decks are `a`/`b`. No case
   folding, no pattern matching (see **Pattern matching**). The line codec's `A`/`B` deck token is a
   codec-local spelling the decoder normalizes when it synthesizes argv (`parse_deck` accepts either).

   This applies to *address text only*. OSC **type tags are protocol literals** and keep the case the
   spec gives them: `f` float, `i` int32, `s` string, `d` double, and `T`/`F` true/false - uppercase, and
   necessarily so, since a lowercase `f` is already the float tag.

4. **Uniform kind segment.** `param/`, `cfg/`, `state/` - always present, never elided. This is not
   decoration: `mix` is a `ParamId` *and* a `query`, and `route` is a `ConfigId` *and* a `query`, both
   true of `names.cpp` today. A flat leaf namespace is ambiguous on real names, and making params the one
   unprefixed kind would fix it only by convention, needing a `static_assert` to keep a future `ParamId`
   from shadowing a reserved word. One segment removes the whole class of problem.

5. **Decode into the existing `Command`.** The decoder synthesizes `argv[]` into a scratch buffer and
   calls the *same* `dispatch()`. No second verb table, no second error taxonomy, no engine-visible
   change.

6. **No new capability.** Every OSC address has a line equivalent; not every line has an address (see
   **What has no address**).

## Address space

Two roots: deck-or-global control, and the platform.

```
/sk/<deck>/<kind>/<name>     deck-scoped control      <deck> = a | b | ab
/sk/<kind>/<name>            global control           <kind> = param | cfg | state
/sk/dev/...                  platform: mode, describe, cpu, usb, presets
```

The first segment is a deck, a kind, or `dev` - disjoint sets, so the parse is unambiguous with no
lookahead. **Global params carry no deck segment at all** (`/sk/param/crossfade`), because deck-scope is
a property of the `ParamId` that the platform table already knows and `describe` already reports. The
line codec has to accept a deck token for globals and discard it; the address space encodes the
distinction structurally instead.

### Deck-scoped

| Address | Type tags | Line equivalent |
|---------|-----------|-----------------|
| `/sk/<deck>/param/<slot>` | `,f` | `set param <slot> <deck> <f>` |
| `/sk/<deck>/param/<slot>` | *(none)* | `get param <slot> <deck>` - see **Reads** |
| `/sk/<deck>/cfg/<name>` | `,i` | `config <name> <deck> <int>` |
| `/sk/<deck>/state/<name>` | *(none)* | `query <name> <deck>` |
| `/sk/<deck>/cv/voct\|mix\|size\|xfade` | `,f` | `cv <kind> <deck> <f>` |
| `/sk/<deck>/gate` | *(none)* / `,T` | `gate <deck>` |
| `/sk/<deck>/pad/play\|rec` | *(none)* / `,T`=reverse | `pad play\|rec <deck> [rev]` |
| `/sk/<deck>/pad/stop\|clear` | *(none)* | `pad <kind> <deck>` |
| `/sk/<deck>/seq/trig\|arm\|clear\|disarm` | *(none)* | `seq <kind> <deck>` |
| `/sk/<deck>/fx/flux\|grit` | `,T`/`,F`/`,i` | `fx <kind> <deck> on\|off` |
| `/sk/<deck>/fx/lock/flux\|grit` | *(none)* | `fx lock <kind> <deck>` |
| `/sk/<deck>/fx/gritmode` | *(none)* | `fx gritmode <deck>` |
| `/sk/<deck>/modspeed` | `,f` / `,fT` / `,fF` | `set modspeed <deck> <f> [sync]` |

`cv`, `gate`, `pad`, `seq`, `fx`, `modspeed` are stimulus verbs rather than addressable state, so they
sit beside the kind segments rather than under one. They are a closed, reserved set; a `ParamId` can
never collide with them because params live under `param/`.

### Global

| Address | Type tags | Line equivalent |
|---------|-----------|-----------------|
| `/sk/param/<slot>` | `,f` | `set param <slot> a <f>` |
| `/sk/cfg/route` | `,i` | `config route a <int>` |
| `/sk/state/mix\|route` | *(none)* | `query mix` / `query route` |
| `/sk/midi/note` | `,ii` | `midi note <ch> <note>` |
| `/sk/midi/msg` | `,iii` | `midi msg <status> <d1> <d2>` |
| `/sk/midi/transport` | `,T`/`,F` | `midi transport start\|stop` |

Global params per the platform scope table: `tempo`, `clickmix`, `panspeed`, `panrange`, `keyinterval`,
`crossfade`. (`tempo`, `keyinterval` and `modspeed` are platform-owned and absent from `describe` -
`terminal-dispatch.md` cut them because `set_param` never sees them - so they are not addressable as
params either. `modspeed` keeps its own deck-scoped address, which routes to `set_mod_speed`.)

### Platform

| Address | Type tags | Line equivalent |
|---------|-----------|-----------------|
| `/sk/dev/mode/test\|run` | *(none)* | `mode test\|run` |
| `/sk/dev/mode/ack` | `,T`/`,F` | *(OSC-only, see Errors)* |
| `/sk/dev/describe` | *(none)* | `describe` |
| `/sk/dev/caps` | *(none)* | `caps` |
| `/sk/dev/help` | *(none)* | `help` |
| `/sk/dev/cpu\|cpumin\|cpumax` | *(none)* | `query cpu\|cpumin\|cpumax` |
| `/sk/dev/usb` | *(none)* | `query usb` |
| `/sk/dev/reset` | *(none)* / `,s` deck | `reset [deck]` |
| `/sk/dev/reset/cpu` | *(none)* | `reset cpu` |
| `/sk/dev/preset/save\|load` | `,i` slot | `preset save\|load <slot>` |

`reset` and `preset` are platform composites that walk the descriptor - they belong to the channel, not
to the engine's control surface, so they sit under `dev` rather than on a deck.

### The `ab` deck alias

`ab` in the deck position applies to both decks: `/sk/ab/param/size ,f 0.5`. Bounded at two, a `strcmp`,
and it covers the only fan-out anyone actually asks for. It is a **write-only** alias - a read on `ab`
is `bad-arg`, since one request cannot have two answers on one reply address.

## Complete layer-2 address reference

Every address the platform tables can produce, enumerated. This is the **whole space, not any one
build's**: `live_params()`/`live_configs()` mask it per engine, and an address for a masked-out slot is
`unknown-address`. The "slot meaning" column is the `engine_params.h` comment - granular's reading, which
is what the name means and *not* what any given engine does with it. That is layer 3, and it arrives as
the `describe` label.

Traceable to `src/engine/engine_params.h` (`ParamId`/`ConfigId`) and `src/terminal/names.cpp`
(`kParamNames`/`kConfigNames`), which the decoder reuses rather than duplicating.

### Params, deck-scoped - `/sk/<deck>/param/<slot>`, `<deck>` = `a` | `b` | `ab`

| Address | `ParamId` | Slot meaning (granular's reading) |
|---------|-----------|-----------------------------------|
| `/sk/a/param/pos` | `Pos` | start position |
| `/sk/a/param/fluxfb` | `FluxFb` | flux feedback |
| `/sk/a/param/env` | `Env` | envelope shape |
| `/sk/a/param/envsize` | `EnvSize` | loop size from the env knob (Drift) |
| `/sk/a/param/size` | `Size` | loop size (Reel/Slice) / window spread (Drift) |
| `/sk/a/param/win` | `Win` | window size (Drift) |
| `/sk/a/param/polyslice` | `PolySlice` | mono/poly slice select |
| `/sk/a/param/speed` | `Speed` | playback speed (Reel/Drift) / pitch (Slice) |
| `/sk/a/param/fluxint` | `FluxIntensity` | flux intensity |
| `/sk/a/param/gritint` | `GritIntensity` | grit intensity |
| `/sk/a/param/fluxmix` | `FluxMix` | flux wet/dry |
| `/sk/a/param/gritmix` | `GritMix` | grit wet/dry |
| `/sk/a/param/feedback` | `Feedback` | overdub feedback |
| `/sk/a/param/mix` | `Mix` | deck in/out mix |
| `/sk/a/param/modamp` | `ModAmp` | modulator depth |
| `/sk/a/param/altpos` | `AltPos` | the Alt+POS knob layer (`CapAltPos`) - engine-interpreted |
| `/sk/a/param/aux` | `Aux` | per-deck Alt+PITCH selector (`CapAux`) - engine-interpreted |

Seventeen deck-scoped slots, each also reachable on `b` and `ab`. `altpos` and `aux` are the two the
platform *declares* engine-interpreted, so they are the two whose label is least likely to match the
slot name.

### Params, global - `/sk/param/<slot>` (no deck segment)

| Address | `ParamId` | Slot meaning |
|---------|-----------|--------------|
| `/sk/param/clickmix` | `ClickMix` | metronome level |
| `/sk/param/panspeed` | `PanSpeed` | auto-pan rate |
| `/sk/param/panrange` | `PanRange` | auto-pan width |
| `/sk/param/crossfade` | `Crossfade` | global deck A/B mix |

### Params with no address

| `ParamId` | Why |
|-----------|-----|
| `Tempo` | platform-owned (Transport service); `set_param` never sees it, absent from `describe` |
| `KeyInterval` | platform-owned (Transport service); same |
| `ModSpeed` | arrives via `set_mod_speed()`, not `set_param` - addressed as `/sk/<deck>/modspeed` |

These three are why every addressable param is uniformly `0..1`. `terminal-dispatch.md` cut them from
`describe` after the sweep asserted on values that went nowhere; the old `40..300` and `1..64` ranges
were display units the setter never took.

### Configs - `/sk/<deck>/cfg/<name>`, `/sk/cfg/route`

| Address | `ConfigId` | Accepted ints |
|---------|------------|---------------|
| `/sk/cfg/route` | `Route` | `0:stereo 1:dmono 2:genstereo` - **global** |
| `/sk/a/cfg/modtype` | `ModType` | `0:lfo 1:follow` |
| `/sk/a/cfg/lfoshape` | `LfoShape` | `0:a 1:b` (engine owns the palette) |
| `/sk/a/cfg/mode` | `Mode` | `0:slice 1:reel 2:drift` |
| `/sk/a/cfg/startmodon` | `StartModOn` | `0:off 1:on` |
| `/sk/a/cfg/sizemodon` | `SizeModOn` | `0:off 1:on` |

Labels come from `kConfigLabels` (`names.cpp`), which `describe` already emits. A write replies with
whether the value changed only under `dev/mode/ack`; otherwise it is silent like any other write.

### State - `/sk/<deck>/state/<name>`, `/sk/state/<name>`

| Address | `IEngine` | Reply |
|---------|-----------|-------|
| `/sk/a/state/empty` | `audio_is_empty(deck)` | `,i` 0/1 |
| `/sk/a/state/recorded` | `audio_recorded_bytes(deck)` | `,i` |
| `/sk/a/state/capacity` | `audio_capacity_bytes(deck)` | `,i` |
| `/sk/a/state/layout` | `deck_layout(deck)` | `,i` 0-3 (single/slice/chord/none) |
| `/sk/a/state/sizetempo` | `size_sets_tempo(deck)` | `,i` 0/1 |
| `/sk/a/state/gateout` | `gate_out_triggered(deck)` | `,i` 0/1 |
| `/sk/state/mix` | `mix()` | `,f` - **global** |
| `/sk/state/route` | `route()` | `,i` 0/1/2, selector encoding - **global** |

`query fit` and `query reseed` have no address (see **What has no address**).

### The two collisions, concretely

This is rule 4 made visible - the reason `param`/`cfg`/`state` is a segment rather than a convention:

| Name | As a param | As a config | As state |
|------|-----------|-------------|----------|
| `mix` | `/sk/a/param/mix` - deck in/out mix | - | `/sk/state/mix` - `mix()`, global |
| `route` | - | `/sk/cfg/route` - write the selector | `/sk/state/route` - read it back |

`mix` is the sharper of the two: the param and the state are not the same quantity at different scopes,
they are different quantities. A flat leaf would have had to pick one.

### Stimulus verbs (closed set, no slot name)

`/sk/<deck>/` + `cv/voct`, `cv/mix`, `cv/size`, `cv/xfade`, `gate`, `pad/play`, `pad/rec`, `pad/stop`,
`pad/clear`, `seq/trig`, `seq/arm`, `seq/clear`, `seq/disarm`, `fx/flux`, `fx/grit`, `fx/lock/flux`,
`fx/lock/grit`, `fx/gritmode`, `modspeed`.

Global: `/sk/midi/note`, `/sk/midi/msg`, `/sk/midi/transport`.

`cv/xfade` is the one that is global in the engine (`cv_crossfade(f)` takes no deck) but keeps a deck
segment for uniformity with the rest of the `cv/` family; the decoder passes `a`.

### Platform

`/sk/dev/` + `mode/test`, `mode/run`, `mode/ack`, `describe`, `caps`, `help`, `cpu`, `cpumin`, `cpumax`,
`usb`, `reset`, `reset/cpu`, `preset/save`, `preset/load`.

### Totals

| Kind | Count |
|------|-------|
| Params, deck-scoped | 17 (× 2 decks, + `ab`) |
| Params, global | 4 |
| Configs | 5 deck-scoped + 1 global |
| State | 6 deck-scoped + 2 global |
| Stimulus verbs | 19 deck-scoped + 3 global |
| Platform | 14 |

Before masking, a two-deck build exposes on the order of 110 distinct addresses. `live_params()` cuts
that hard in practice - `tape` and `shuttle` each declare a handful of slots - which is exactly why
`describe` and not this table is what a host should enumerate.

## Reads: arity, not a verb

`get` and `query` do not appear in the address space. **A message with no arguments is a read of the
address it names**; the same address with an argument is a write.

```
/sk/a/param/speed  ,f 0.5   ->  set param speed A 0.5
/sk/a/param/speed           ->  get param speed A   ->  /sk/reply/a/param/speed ,f 0.5
```

This collapses the entire `get`/`query` branch, and it is available only because rule 1 put the value in
the argument - with the value in the address there is no arity left to overload.

Params are the only bidirectional kind, which matches `IEngine`: `param()` reads back, `set_config` is
write-only. So `state/*` and `dev` reads reject arguments as `bad-arg`; `cfg/*`, `cv/*` and every
stimulus verb reject the no-argument form the same way.

Replies mirror the request path under `/sk/reply`, so a Max patch routes a reply on the path it sent and
several outstanding reads need no sequence tag. Reply arguments are typed: float reads `,f`, int and
boolean reads `,i` (0/1, not `T`/`F`, so a sweep treats every numeric reply uniformly), text reads
(`usb`) `,s` with the string the line codec emits.

`nan` from `cpumin`/`cpumax` inside the post-`reset cpu` gap is sent as an IEEE NaN float, not coerced -
the same "no sample yet" ≠ "zero load" distinction dispatch already makes.

## Errors

```
/sk/err ,ss   <request-address> <reason>
```

`reason` is the line codec's fixed token set (`unknown-param`, `unknown-config`, `bad-deck`, `bad-arg`,
`no-arg`, `too-many-args`, plus the transport's `line-too-long`/`overflow`) with four the address space
needs of its own: `unknown-address` (replacing `unknown-verb`, which has no meaning here), `bad-typetag`,
`bad-packet`, `slip-overflow`. Echoing the request address is what makes an error actionable when nothing
else correlates request to reply.

**Two writes reply anyway**, decided during implementation: `/sk/<deck>/pad/play` and
`/sk/<deck>/fx/gritmode`. Both are actions that *return a value* through layer [3] - the deck's
emptiness, and the grit reseed pair the platform uses to re-pick its `MValue` pickup after the switch -
and a host has no other way to obtain either from the gesture itself. Suppressing them would be the one
place the "no new capability" rule ran backwards, removing something the line codec offers. They are
single pad presses, not a fader stream, so the reason for silence does not apply.

There is deliberately **no `/sk/ok`**. A write that succeeds sends nothing - a rig streaming fader moves
at 100 Hz does not want an ack per message, and a harness that needs one can read the value back. This is
the one behavioural difference from the line codec, where every command replies. Hosts that want acks
enable them per-session with `/sk/dev/mode/ack ,T`, which makes every successful write emit
`/sk/reply/ok ,s <address>`; off by default, and the pytest harness turns it on.

An address naming a param the engine does not implement (`live_params()` bit clear) is
`unknown-address`, not a silent no-op. A layout generated from `describe` never sends one; a hand-written
one finds out immediately.

## `describe` - addresses and labels

`describe` matters more here than for the line codec, because it is what tells a host **what the
addresses are and what they mean on this engine**. It emits full addresses rather than bare names, and
carries the layer-3 label alongside:

```
bundle {
  /sk/reply/dev/describe        ,sss   "radio" "0.9.3-radio" "masked=1"
  /sk/reply/dev/describe/param  ,ssffs "/sk/a/param/speed" "station"   0.0 1.0 "deck"
  /sk/reply/dev/describe/param  ,ssffs "/sk/param/crossfade" "crossfade" 0.0 1.0 "global"
  /sk/reply/dev/describe/cfg    ,sss   "/sk/cfg/route" "route" "0:stereo 1:dmono 2:genstereo"
  /sk/reply/dev/describe/state  ,sss   "/sk/a/state/empty" "empty" "int"
  /sk/reply/dev/describe/caps   ,i     0x00000133
}
```

Emitting the address makes the descriptor directly consumable: a host enumerates the bundle and has a
bindable address plus a display label per row, with no need to re-implement this document's composition
rules. Deck expansion happens on-device, where the scope table lives - deck-scoped params emit an `a` row
and a `b` row - so the host never has to know decks exist.

It is sent as **one bundle**, so a host receives it atomically. ~2-3 KB, and it must fit the TX FIFO in
one piece since an OSC bundle cannot be streamed the way lines can. That makes the dispatch doc's "size
the TX FIFO to hold a full descriptor" recommendation a **requirement** here. Budget 4 KB.

### Where the label comes from

Layer 3 exists nowhere machine-readable today - only in `docs/engines/*.md` prose. It becomes a third
engine-owned virtual alongside the liveness masks, defaulting to the layer-2 name so no engine is forced
to care:

```cpp
// src/engine/iengine.h
virtual const char* param_label(ParamId id) const { return nullptr; }   // nullptr -> use kParamNames
```

Only live slots need one, so it is ~6-12 short strings per engine. **`radio` and `tape` implement
this** (`radio_engine.h` / `tape_engine.h`) - 6 and 10 labels respectively, and they are the two the
design was argued from: `radio`'s `speed`→"station" and `aux`→"bank"; `tape`'s `size`→"character",
`pos`→"drive", `altpos`→"pan", and the two grit slots→"filter cutoff"/"filter resonance". Every other
engine keeps the default. A few hundred bytes of flash per build.

`Crossfade` is deliberately left unlabelled on both: it is the platform crossfader and means the same
thing everywhere, so a per-engine label would be rot risk with no legibility gain. The rule that fell
out of writing two tables is **label what the engine reinterprets, not what it merely uses**. The
real cost is that engines currently carry zero naming burden and this adds a table that can rot - which
is why it defaults rather than being required, and why nothing in the protocol depends on it. **The label
is cosmetic *to the device*: no address, reply, or error ever derives from it.** One tier up it is
load-bearing - it is what the host-side semantic namespace is generated from - and that asymmetry is
deliberate. A label that rots misnames a control on a surface; it can never make the device
unreachable, because the generic address it annotates is unaffected.

## The semantic tier - host-side, generated

Everything above is the **generic tier**: one protocol, layer-2 vocabulary, identical in shape on every
build. It is complete and sufficient on its own - a host never needs anything else to drive the device.

It is also not what a musician wants to type. `/sk/a/param/fluxfb` and `/sk/a/param/polyslice` are
granular's internals leaking into a protocol every engine has to speak, and on a radio build
`/sk/a/param/speed` is the station dial. So there is a second, **semantic tier** - engine-specific,
human-readable, and **entirely host-side**:

```
  patch / surface     /radio/a/station          ,f 0.5      semantic tier   (host)
                              |  translate (generated from describe)
  ---------------------------- USB-C CDC / SLIP ----------------------------
  device              /sk/a/param/speed         ,f 0.5      generic tier    (firmware)
```

The device speaks only the generic tier and does not know the semantic tier exists.

### Why host-side, not firmware

Putting the semantic namespace on the device would double the address space in flash, put a
hand-maintained per-engine table into the *wire format* where drift becomes a protocol bug rather than a
display bug, and force an engine-name segment into the generic tier to declare which vocabulary a path
speaks - the segment **Rejected alternatives** removes.

Host-side, it costs the firmware nothing and can be richer than anything that would fit on-device: units,
display curves, control groupings, per-surface layouts. And it is *generated*, not written, so it cannot
drift from a firmware it re-reads at every connect.

### The engine segment lives here

`/radio/a/station` - the semantic namespace **is** engine-specific by definition, so this is where an
engine name is load-bearing rather than decorative. A patch built against radio names radio's controls
and is meaningless against tape; the segment says so. That is the same fact that made it *wrong* in the
generic tier, where the whole point is that one layout binds to every build.

### What the translator consumes

Exactly the `describe` bundle, which already carries everything needed:

| Field | From | Used for |
|-------|------|----------|
| engine name | `descr` row | the semantic root segment |
| generic address | `describe/param` etc. | the translation target |
| label | `param_label()`, defaulting to the slot name | the semantic leaf |
| range | platform scope table | surface widget bounds |
| scope (`deck`/`global`) | platform scope table | whether a deck segment appears |
| kind | row tag (`param`/`cfg`/`state`) | which semantic subtree |

No firmware change is needed beyond `param_label()`. If a future need appears (units, a curve hint), it
is an added column on the descriptor row, not a protocol change - the address spaces stay as they are.

### Translation rules

1. **Slugify the label**: lowercase, trim, spaces and `/` to `-`, drop anything outside `[a-z0-9-]`.
   `"station select"` → `station-select`. Addresses stay lowercase, as in the generic tier.

2. **Compose**: `/<engine>/<deck>/<slug>` for deck-scoped, `/<engine>/<slug>` for global, mirroring the
   generic tier's deck elision. `cfg`/`state` keep their kind segment (`/radio/a/cfg/mode`,
   `/radio/a/state/empty`) - the `mix`/`route` collisions are properties of the *names*, and a label can
   collide the same way.

3. **Disambiguate within an engine.** Two slots may carry the same label (two "level" controls is the
   obvious case). On collision, suffix with the slot name: `level-mix`, `level-gritmix`. Deterministic,
   so a saved patch stays valid across reconnects.

4. **Fall back silently.** A slot with no `param_label()` gets its layer-2 name as the slug, which is
   what `describe` already sends. An engine that implements no labels therefore produces a semantic tier
   identical to the generic one minus the `param/` segment - degraded, never broken.

5. **Translate replies back.** `/sk/reply/a/param/speed ,f 0.5` → `/radio/a/station ,f 0.5` on the
   semantic side, so a patch sees only its own namespace. Errors carry the *semantic* address the patch
   sent, not the generic one it was translated into, or the reason is unactionable.

6. **Pass through unknown addresses untranslated.** A patch may legitimately want to reach a generic
   address the semantic tier has no name for; a `/sk/...` address arriving at the translator goes out
   verbatim.

### Where it lives

`tools/` alongside the existing host client, as a component of `skdev` rather than a separate program:
`OscDevice` already reads `describe` to build its param map, so the translator is that map plus the
slugify/compose rules and a reverse index. A Max abstraction and a TouchOSC layout generator are then
thin consumers of the same JSON the translator builds - and the layout generator is where the semantic
tier pays off most, because it can print `station` on a fader while binding it to `/sk/a/param/speed` and
skip the translator at runtime entirely.

### What it must never do

- **Never be required.** Every device function is reachable generically. The translator is a convenience
  layer, and a bug in it must never be able to make the device unreachable.
- **Never be in the test path.** `test_generic.py`'s cross-codec parity sweep uses generic addresses
  only. Testing through the translator would make a translation bug look like a firmware bug, which is
  the exact confusion the two-tier split exists to prevent.
- **Never round-trip through labels.** The translator maps semantic → generic on the way in and generic →
  semantic on the way out, but the *authority* is always the generic address. A saved patch stores the
  semantic address it was built with; if a firmware update changes a label, the translator's rebuilt map
  no longer resolves it and the patch fails loudly at connect rather than silently retargeting.

### Testing

One property, host-only, no device: for every row in a captured `describe` bundle, semantic → generic →
semantic is the identity, and every generated semantic address resolves to exactly one generic address.
Run against the descriptor fixtures the web front-end already keeps, so it costs no bench time.

- **`query fit <deck> <fraction>`** - takes an argument, and an argument on a `state/` address is how the
  codec spells "write", which `fit` is not. Line-only, matching its exclusion from `describe`.
- **`query reseed`** - a latching read that self-clears. In a rig where a patch may poll addresses,
  handing it out is a way to lose the flag the platform is waiting on.
- **Engine `handle_command` verbs (target B)** - no engine implements the hook, and it carries no type
  information, so there is nothing to synthesize an address or type tag from. If
  [`terminal-target-b.md`](terminal-target-b.md)'s declared query table lands, those entries get `state/`
  addresses for free, because they would then be describable.

## Pattern matching - excluded

OSC's `*`/`?`/`[]`/`{}` address wildcards are **not** implemented; addresses are compared literally.

The tree makes them tempting - `/sk/*/param/size` (both decks) and `/sk/a/param/*` (a whole deck) are
well-formed, meaningful requests. They are still a trap: one packet fans into N engine writes with a
single value and no way to report which subset failed, and a read wildcard fans into N replies that can
overrun the TX FIFO mid-burst. Matching also costs a real parser rather than a `strcmp` loop. The `ab`
alias covers the 80% case at zero cost; wildcards proper wait for a patch that needs them.

## Framing: SLIP (RFC 1055)

OSC over UDP is self-delimiting by datagram; over a serial byte stream it is not - an OSC packet carries
no length prefix, so the receiver cannot find a boundary. SLIP supplies it, and is what OSC-over-serial
implementations (`[oscparse]`, liblo's serial transports, Arduino OSC libs) already speak.

```
END     0xC0   packet delimiter (sent leading and trailing - a leading END flushes line noise)
ESC     0xDB
ESC_END 0xDC   ESC ESC_END  -> literal 0xC0
ESC_ESC 0xDD   ESC ESC_ESC  -> literal 0xDB
```

The decoder replaces the line assembler (`src/terminal/line_assembler.h`) with the same shape - fed from
the same SPSC RX ring, emitting a completed packet into a bounded buffer instead of a completed line.
OSC messages are 4-byte aligned and verbose, so the 128 B line buffer is too small; **512 B** covers
every address above with headroom. Overflow discards to the next `END` and emits `slip-overflow` -
resynchronizing rather than truncating, since a truncated OSC packet is undetectable garbage where a
truncated line is merely wrong.

**Logger coexistence is the sharpest constraint OSC adds.** The transport's shared-CDC arrangement lets
human-readable `[tag]` log lines interleave with replies. Harmless for line-ASCII; *fatal* for SLIP, where
a log line lands inside a packet. So `SPK_TERMINAL_OSC` must either (a) force `INFS_LOG=0`, or (b) wrap
log output as `/sk/log ,s` in its own SLIP frames. **(b)** is right - it keeps `DEBUG=1` usable and gives
a Max patch somewhere to show firmware logs - but (a) is the acceptable phase-1 shortcut. This problem
does not exist in the line build.

## Type coercion

The one place OSC is more permissive than the line codec, because control surfaces are inconsistent about
what a button sends.

| Expected | Accepted tags | Rule |
|----------|---------------|------|
| float | `f`, `i`, `d` | `d` narrowed; non-finite rejected `bad-arg` |
| int | `i`, `f`, `T`/`F` | `f` truncated toward zero; `T`=1, `F`=0 |
| bool | `T`, `F`, `i`, `f` | non-zero true; **absent argument = true** (bare trigger) |
| deck | *(path segment)* | never an argument |
| string | `s` | only `dev/reset` deck, `dev/preset` slot |

TouchOSC buttons send `,f 1.0`/`,f 0.0` in some configurations, so a trigger address accepts `,f` with
zero suppressing the trigger.

Rejected outright: blobs, timetags as arguments, arrays, and any message whose argument count exceeds
what the address expects (`too-many-args`). Extra *trailing* arguments are an error, not ignored -
silently dropping them hides a patch wired to the wrong address.

**Bundle timetags are ignored**, not scheduled. The device has no dispatch queue and no clock synced to
the host; honouring a timetag would mean building both. Bundle contents dispatch immediately, in order -
the behaviour every serial OSC implementation without a scheduler adopts. Documented rather than
silently divergent.

## Implementation shape

```
src/terminal/
  slip.h          SLIP encode/decode, mirrors line_assembler.h's interface
  osc.h           OSC wire format: OscMessage reader, OscWriter, OscBundleWriter
  osc_decode.cpp  packet -> address + typed args; the coercion table; bundle walking
  osc_sink.h      OscSink: the typed reply sink (a TextSink subclass)
  osc_encode.cpp  the writers + OscSink
  osc_addr.h      kOscBundleCap + the codec entry point
  osc_addr.cpp    address -> a line in the existing grammar -> dispatch_line(); describe bundle
```

The line the decoder synthesizes goes through `dispatch_line()` unchanged, which is why there is no
second verb table: an address resolves to `set param speed a 0.5000` and the existing tokenizer takes it
from there. Host side, `tools/skdev/{osc,semantic,oscdevice}.py` mirror the same split - wire format,
semantic tier, device client - with the first two dependency-free so they test without pyserial.

`osc_addr.cpp` carries **no table of addresses**. The leaf segments are already `kParamNames` /
`kConfigNames` from `names.cpp`, so it resolves an address by reusing `param_from_token` /
`config_from_token` on the leaf; what it adds is the shape rules - deck vs global vs `dev`, kind segment,
reserved stimulus verbs - which is a short function. The address space is therefore generated from the
same tables `describe` walks and cannot drift from the descriptor.

```cpp
// /sk/a/param/speed     ,f 0.5   ->  argv = {"set","param","speed","a","0.5000"}
// /sk/a/param/speed     (no args) ->  argv = {"get","param","speed","a"}
// /sk/param/crossfade   ,f 0.25  ->  argv = {"set","param","crossfade","a","0.2500"}
// /sk/a/cfg/mode        ,i 1     ->  argv = {"config","mode","a","1"}
// /sk/a/state/empty               ->  argv = {"query","empty","a"}
// /sk/ab/param/size     ,f 0.5   ->  two dispatches, decks a then b
//
// Numbers are formatted back to text with append_f32 (fmt.h) into a scratch buffer,
// then dispatch() parses them again with parse_f32. Round-tripping a float through
// decimal is the deliberate cost of reusing layer [3] unchanged: ~4 decimal digits,
// well inside every 0..1 param's audible resolution.
```

That round-trip is the design's one real wart. The alternative - a typed `Command` carrying pre-parsed
values - removes it but forks dispatch's argument handling in two, which is exactly the rewrite the
codec-agnostic layering exists to avoid. If the format/parse pair ever mattered (it will not; these are
control-rate messages on the main loop), the fix is a `Command` union, not a second dispatcher.

Reply encoding is the mirror problem: layer [3] writes to a `TextSink` and knows nothing about types. The
typing information already exists at every call site, because the line codec needed it to choose
`append_f32` over raw - `TextSink` *already has* `ok_f32` and `ok`. So the OSC sink overrides those to
emit `,f`/`,i`/`,s`, and only the generic `str()` path degrades to a string. `TextSink` becomes a
compile-time policy: `SPK_SINK_VIRTUAL` expands to `virtual` under `SPK_TERMINAL_OSC` and to nothing
otherwise, so the line build keeps its original vtable-free shape.

**That was not quite the whole change.** One call site does not carry its typing where the sink can see
it: `query`. Dispatch frames a query reply as `str("ok ")`, then lets `read_platform_query()` - or the
engine's own `read_engine_query()` - append the value, then `str("\r\n")`. The type is known (every
query DECLARES a `ValueKind`), but it is known to the table, not to the sink, and the value arrives
through the same `append_*` calls that free-form text uses. Typing state reads correctly therefore
needed two more methods:

```cpp
SPK_SINK_VIRTUAL void ok_begin();   // "ok "     in the line codec; OSC: arm typed capture
SPK_SINK_VIRTUAL void ok_end();     // "\r\n"    in the line codec; OSC: settle the type
```

Byte-for-byte identical to the `str()` calls they replace, so the line codec is unchanged. Between them
the OSC sink watches what happens: exactly one `append_i32` and nothing else means `,i`, exactly one
`append_f32` means `,f`, and anything else - a raw `str()`, or several values, as `query usb` does -
falls back to `,s` carrying the text the line codec would have produced. That makes "only the generic
`str()` path degrades to a string" mechanical rather than a convention someone has to remember.

The alternative was to pass `ValueKind` down into the sink, which would have put a layer-[3] concept
into the reply interface for the benefit of one codec. Watching the calls keeps the knowledge where it
already was. The alternative - wrapping the
text line as `,s "ok 0.7500"` - would make every host parse ASCII inside an OSC string, defeating the
point of using OSC.

## Flash and RAM delta (estimated, unmeasured)

**Measured 2026-08-09**, `ENGINE=delay`, `TERMINAL=1` with and without `OSC=1`. The original estimates
are kept alongside because the gap is instructive, not because it is close.

| Item | Estimated | Measured |
|------|-----------|----------|
| Codec code (SLIP + OSC parse/encode + typed sink + address synthesis) | ~3.5 KB | **~9.0 KB** (`SRAM_EXEC` 183356 -> 192396 B) |
| RX packet buffer | 512 B | 512 B |
| TX FIFO growth (descriptor bundle must fit whole) | 2 KB -> 4 KB | **2 KB -> 8 KB** |
| `describe` bundle scratch (static; not in the original estimate at all) | - | **6 KB** |
| Total SRAM | ~2.5 KB | **~12.4 KB** (98776 -> 111448 B) |

Two things account for the gap. The code estimate was simply optimistic - address parsing, the coercion
table and the reply sink are each about twice the size guessed. The data estimate missed an item
entirely: **the descriptor has to be assembled somewhere before it can be framed**, and a bundle cannot
be built incrementally into a ring buffer the way lines can, because each element is prefixed with its
own length. That scratch is the single largest cost in the whole codec.

The bundle is 5392 B on an engine using the DEFAULT all-live masks - 64 rows: 38 param (17 deck-scoped
x 2 decks + 4 global), 18 state, 6 config, plus `descr` and `caps`. The original 2-3 KB projection was
taken from a masked engine; real engines mask hard (`tape` and `shuttle` declare a handful of slots
each) and land near 1 KB. The unmasked case is what has to fit, though, or `describe` fails on exactly
the engines that have not narrowed yet - so `kOscBundleCap` is 6 KB and
`host/test_terminal_osc.cpp` asserts the unmasked descriptor against it, which keeps the guard from
rotting as params or queries are added.

Affordable on most engines able to host `TERMINAL=1` (per [`terminal-impl.md`](terminal-impl.md)) -
`delay` lands at 72.3% of `SRAM_EXEC`, up from 68.9%, and `tape` at 79.5% - but the margin is thinner
than the spec implied.

**`pstretch` cannot host OSC.** Verified 2026-08-09: `make ENGINE=pstretch TERMINAL=1 OSC=1` fails to
link, `.bss` overflowing `SRAM` by 4352 bytes. It is not close and it is not a regression - the line
build already sits at **97.4% of SRAM** (311168 / 319488 B) because pstretch's FFT working set needs its
own linker split (`linker/alt_sram_pstretch.lds`, 200K/312K instead of 300K/212K). OSC's ~12.4 KB of
data has nowhere to go. This is exactly the case this section always predicted; it now has a name.

If that engine ever needs OSC, the lever is the descriptor buffer rather than the codec: `pstretch`
masks to 9 params, so its descriptor is ~1.5 KB and `kOscBundleCap` could drop from 6 KB to 2 KB with
the TX FIFO following it to 4 KB - about 8 KB back, comfortably more than the shortfall. That would be
a per-build knob, and it is deliberately not implemented until something asks for it. The runtime
already fails safe if a descriptor outgrows the buffer: `describe` answers `/sk/err ... overflow`
rather than emitting a truncated bundle.

The other engines at the `SRAM_EXEC` edge are unaffected - the cost that bit here is data, not code.

## Testing

`skdev` grew an `OscDevice` with the same method surface (`set_param`, `get_param`, `query`,
`describe`) over a dependency-free OSC/SLIP implementation, so `test_generic.py`'s cross-engine sweep
runs unmodified against either codec. That parity is the real acceptance criterion: **the same sweep,
both codecs, identical results.** Anything the OSC build answers differently is a codec bug by
definition, since layer [3] is shared.

`describe()` returns the same `DeviceDescriptor` from both clients - the bundle's full addresses are
reduced back to bare names and the two per-deck rows collapse into one entry - which is what lets the
suite be written against the client surface and never against a codec.

### The bench procedure (RUN 2026-08-09, passed)

Both codecs cannot be present at once; it is a compile-time flag, so parity is a two-flash comparison.

```
make ENGINE=tape TERMINAL=1 -j8 && make ENGINE=tape TERMINAL=1 program-dfu
make test-hw                       > /tmp/line.txt      # line codec, the reference
make ENGINE=tape TERMINAL=1 OSC=1 -j8 && make ENGINE=tape TERMINAL=1 OSC=1 program-dfu
make test-hw CODEC=osc             > /tmp/osc.txt       # same suites, OSC client
diff /tmp/line.txt /tmp/osc.txt
```

`tape` is the engine to use: it has the richest label table, ten live params, and at 79.5% of
`SRAM_EXEC` it still has headroom. Order matters - flash and pass the LINE build first, because it is
the known-good reference and it is readable in a terminal; a failure there is a hardware or build
problem, not a codec one. Debugging SLIP first would mean debugging a binary protocol with no baseline.

#### What the bench actually answered

The three questions only hardware could settle, all about the parts not shared with the line build:

- **The descriptor bundle survives the 64-byte USB packet path.** ~4 KB on a masked `tape` build,
  arriving as one intact SLIP frame, first try. This was the main risk and it is closed.
- **Nothing else writes ASCII to the CDC.** Every reply decoded cleanly across a full sweep.
- **The semantic tier resolves against a live device.** 46 addresses generated from the device's own
  `describe`; semantic -> generic -> semantic is the identity on all of them; `/tape/a/character` drives
  `/sk/a/param/size` and reads back what it wrote.

Measured on the bench: **0.18 ms** steady-state round trip (the first call after connect costs ~11 ms of
settling, which is easy to mistake for the real latency). A full 63-case sweep runs in about a second,
most of it the one test that makes tape open a WAV.

#### Four defects the parity sweep found, none of which off-target testing could have

Recorded because they are all the same shape - a difference between the two clients masquerading as a
difference between the two codecs, which is exactly what this sweep exists to separate.

1. **OSC `describe` dropped Enum selector labels.** State rows were `,sss`; the line codec's describe
   *does* send them (`query route global enum 0:stereo 1:dmono 2:genstereo`), so the two codecs
   described the same device differently. Now `,ssss`. **The spec's own example row was wrong** and is
   corrected above.
2. **`OscDevice.query()` returned typed values where the line client returns text**, so every `bool` and
   `enum` assertion failed under OSC and passed under lines.
3. **Global params were addressed with a deck segment.** The line codec accepts and discards a deck for
   a global; the OSC space encodes scope structurally, so `/sk/a/param/crossfade` is correctly
   `unknown-address`. The client has to drop what the line codec throws away.
4. **`Device.pad()` discarded its reply entirely** (returned `None`) while `OscDevice.pad()` kept the
   `ok ` framing the line client strips - so the one action that reports a value reported two different
   things.

A fifth was found by reading rather than by the sweep: `OscDevice._recv()` returned the first frame of a
serial chunk and **discarded the rest**, a latent desync that would only surface when timing happened to
batch two replies together.

Two checks the line codec does not need:

- **Address composition parity.** Every address `describe` advertises must be writable and readable if it
  is a param, and must be exactly what this document's rules predict from the slot name and scope. This
  catches drift between `osc_addr.cpp` and the descriptor - the two places that both know how an address
  is spelled.
- **Cross-engine address stability.** The same layer-2 address set appears on every build for the same
  live slots, and only the labels differ. This is the property the universal-layout claim rests on, so it
  should be asserted rather than assumed.

## Rejected alternatives

Recorded so they are not relitigated.

- **Value in the address** (`/sk/a/param/size/0.5`). Reads well, unusable: an address that varies with
  its value cannot be bound to, breaks `[routeOSC]`, and throws away the type tags.

- **Verb-first paths** (`/sk/set/param/<slot>/<deck>`). A transcription of the line grammar. Produces a
  namespace where `/sk/set/*` means everything and no subtree corresponds to anything on the device.

- **An engine-name segment** (`/sk/tape/a/param/size`). Justified at first by wrong-build safety, device
  namespacing, and a future multi-engine binary. None survives: `ParamId` is one shared enum resolved by
  one global name table, so a wrong-build write hits the *correct slot* with different musical meaning,
  not a coincidental index; over CDC each device is its own serial port, so addresses cannot collide and
  an engine name would not disambiguate two tape units anyway; and the multi-engine binary is an
  explicitly declined option in `engine-layout.md` R4. With layer-2 addressing the segment also costs the
  universal layout, which is the main thing OSC is here to provide. It is not discarded, though - it is
  load-bearing one tier up, as the root of the host-side semantic namespace (`/radio/a/station`), which
  is the one place an engine name genuinely identifies something.

- **Layer-1 (silkscreen) addressing on the device** (`/sk/a/pitch`). The panel is the true cross-engine
  invariant, so this is the most tempting alternative - and it is a UI gesture, not a protocol surface.
  `core.ui.cpp:466-524` routes a knob to a `ParamId` through a `DeckLayout` branch *and* `MValue`
  soft-takeover pickup: a write that does not match the stored knob position is deliberately swallowed
  until it catches up, which is correct for a pot and catastrophic for a control message. `alt+size` is
  also `Win` in chord layout and `PolySlice` in slice layout, so one address would resolve to different
  params by mode. Reaching it would mean either duplicating that branch in the terminal - crossing the
  boundary `engine-layout.md` R4 enforces with `check-boundary` - or pushing messages through the
  pot-apply pass that `mode test` exists to disable (`app.cpp:296`).

- **Numeric slots** (`/sk/a/param/7`). Breaks every saved layout on an enum reorder, silently. Retained
  only as the existing `param_from_token` fallback, never as what a host stores.

- **Silkscreen vocabulary** (`/sk/a/param/pitch`). More honest about layer 1, but layer 1 -> layer 2 is
  one-to-many: "One physical control may map to several of these (via modifier layers / mode)". PITCH
  reaches `Speed` and, on the Alt layer, `Aux`; SIZE reaches `Size`, `Win`, `PolySlice` by `DeckLayout`.
  No injective knob name exists, and `AltPos`/`Aux` are named for the modifier layer because no knob owns
  them.

- **Layer-3 names in the path** (`/sk/a/param/station`). Forces a per-engine layout, requires the engine
  segment to declare which vocabulary the path speaks, and makes the wire format depend on a table that
  can rot. Labels in `describe` deliver the same legibility with none of it.

- **Bare leaf, no kind segment** (`/sk/a/size`). Ambiguous on names that exist today: `mix` is a param
  and a query, `route` is a config and a query.

## Out of scope

Pattern matching beyond the `ab` alias; timetag scheduling; OSC-over-UDP (no network interface); bundle
replies for anything but `describe`; `measure`/`stim` addresses (they follow their phases and compose by
the same rules - `/sk/<deck>/measure/<property>`, `/sk/dev/stim/<signal>`); and any output arbitration.
This spec is: SLIP frame in, existing `Command` out, typed OSC reply back.
