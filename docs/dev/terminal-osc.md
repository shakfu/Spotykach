# Terminal OSC codec spec (`SPK_TERMINAL_OSC`)

Status: **design draft, unbuilt (2026-08-07).** Specifies the OSC address space and the SLIP framing for
layer [2] of the terminal channel - the opt-in alternate codec named in
[`terminal-control.md`](terminal-control.md). It replaces *only* the codec: layer [1] (transport, SPSC
ring, TX FIFO) and layer [3] (verb table, `IEngine` binding, `mode test`, `describe`) are unchanged and
shared byte-for-byte with the line-ASCII build. Everything here is `#if SPK_TERMINAL_OSC`, which implies
`SPK_TERMINAL`.

Read [`terminal-dispatch.md`](terminal-dispatch.md) first - this document maps its verb catalog onto an
address space, and where the two disagree, dispatch wins.

## What OSC is for

Line-ASCII stays the default and the floor: it is testable, works with a dumb terminal, and costs a
tokenizer. OSC buys one thing - **the device becomes a node in a Max/Pd/TouchOSC rig**, where a fader is
bound to an address once and then just sends floats. Every decision below is made for that client, not
for the pytest harness, which is already well served by lines.

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

Only live slots need one, so it is ~6-12 short strings per engine (`radio`: `speed`→"station",
`aux`→"bank"; `tape`: `size`→"character", `altpos`→"pan"). A few hundred bytes of flash per build. The
real cost is that engines currently carry zero naming burden and this adds a table that can rot - which
is why it defaults rather than being required, and why nothing in the protocol depends on it. **The label
is cosmetic: no address, reply, or error ever derives from it.**

## What has no address

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
  osc_decode.cpp  packet -> address segments + typed args -> Command{argv[]}
  osc_encode.cpp  typed OSC reply sink
  osc_addr.cpp    address segments -> verb/argv synthesis
```

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
emit `,f`/`,i`/`,s`, and only the generic `str()` path degrades to a string. Making `TextSink` virtual
(or a compile-time policy under `SPK_TERMINAL_OSC`) is the whole change. The alternative - wrapping the
text line as `,s "ok 0.7500"` - would make every host parse ASCII inside an OSC string, defeating the
point of using OSC.

## Flash and RAM delta (estimated, unmeasured)

| Item | Estimate |
|------|----------|
| SLIP encode/decode | ~0.3 KB |
| OSC parse (address split, type tags, arg extraction) | ~1.5 KB |
| OSC encode + typed sink | ~1 KB |
| Address->verb synthesis (rules, not a table - reuses `names.cpp`) | ~0.4 KB |
| Descriptor address + label composition | ~0.3 KB |
| Per-engine `param_label` tables | ~0.2 KB per build |
| RX packet buffer | 512 B SRAM |
| TX FIFO growth (descriptor bundle must fit whole) | 2 KB -> 4 KB SRAM |

~3.7 KB flash, ~2.5 KB additional SRAM over the line build. Affordable on the engines already able to
host `TERMINAL=1` (per [`terminal-impl.md`](terminal-impl.md)); on the ones already at the `SRAM_EXEC`
edge it is not, and OSC is simply not a build they get.

## Testing

`skdev` grows an `OscDevice` with the same method surface (`set_param`, `get_param`, `query`, `describe`)
over `python-osc` plus a SLIP wrapper, so `test_generic.py`'s cross-engine sweep runs unmodified against
either codec. That parity is the real acceptance criterion: **the same sweep, both codecs, identical
results.** Anything the OSC build answers differently is a codec bug by definition, since layer [3] is
shared.

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
  universal layout, which is the main thing OSC is here to provide.

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
