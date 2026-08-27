# Terminal codec + dispatch spec (phase 1)

Status: **built and hardware-verified (2026-07-31).** Corrections found by running it are marked inline. Specifies layers [2] (line codec) and [3] (dispatch / `IEngine` binding) of the terminal channel - what turns a received line into an `IEngine` call and a reply. It sits on top of [`terminal-transport.md`](terminal-transport.md) (which delivers whole lines via `LineSink` and accepts reply bytes via `Terminal::write`) and realizes the stimulus/observation model in [`terminal-control.md`](terminal-control.md). Everything here is `#if SPK_TERMINAL`.

## What phase 1 covers

- Codec: tokenize a line into a `Command{ verb, argv[] }`; in-place, bounded, no allocation.

- Dispatch: a static verb table; platform-reflective verbs bound directly to `IEngine`, unknown verbs forwarded to the engine's own `handle_command`.

- Stimulus (target A): drive the full `IEngine` input surface by verb.

- Observation L0/L1: `get param` (round-trip) and `query` (engine state).

- `mode test`: the input-isolation flag and the exact points the platform must consult.

- `describe`: the introspection descriptor (platform tables + per-engine liveness masks).

Out of scope (later phases): `measure` (L2 audio tap), `stim` (signal source), OSC/SLIP. Named vs numeric addressing is resolved below.

## Layer [2] - the codec

### Tokenizer

The transport hands `process()` a NUL-safe line buffer (`\r` trimmed, `\n` stripped). The codec tokenizes **in place** - overwrite each run of spaces with `\0` and record argv pointers. No copying, no heap.

```cpp
// src/terminal/command.h
struct Command {
    static constexpr uint8_t kMaxArgs = 6;   // verb + up to 5 args covers every phase-1 form
    const char* argv[kMaxArgs];
    uint8_t     argc = 0;
    const char* verb() const { return argc ? argv[0] : ""; }
};

// Returns false if the line has too many tokens (-> "err too-many-args").
bool tokenize(char* line, Command& out);     // splits on ' ' / '\t', in place
```

The line buffer is the transport's bounded 128 B buffer; a longer line was already rejected upstream as `err line-too-long`. `kMaxArgs` overflow is a codec-level `err too-many-args`.

### Value coercion (input) and formatting (output)

Coercion lives at the codec/dispatch boundary. Input parsing may use `strtof`/`strtol` (libc, linked; the `%f` restriction is a *print* concern, not parse). **Output must not use `printf("%f")`** - the firmware does not link `_printf_float` (the `METER` path formats floats by integer decomposition, `src/app.cpp:275-277`). So replies format floats manually:

```cpp
// src/terminal/fmt.h
bool  parse_f32 (const char* s, float& out);        // strtof + finite check
bool  parse_i32 (const char* s, int32_t& out);      // strtol, base 0 (0x.. ok)
bool  parse_deck(const char* s, DeckRef::Ref& out); // "A"/"a"->A, "B"/"b"->B
void  append_f32(TextSink&, float v, int decimals = 4);   // integer decomposition, no %f
```

`TextSink` is the reply interface backed by `Terminal::write` (the transport's non-blocking TX FIFO):

```cpp
struct TextSink {
    void str(const char* s);                 // raw
    void line(const char* s);                // s + "\r\n"
    void ok();                               // "ok\r\n"
    void ok_f32(float v);                     // "ok " + append_f32 + "\r\n"
    void err(const char* reason);            // "err " + reason + "\r\n"
};
```

## Layer [3] - dispatch

### Table and context

```cpp
// src/terminal/dispatch.cpp
struct Ctx {
    spotykach::IEngine& engine;
    TextSink&           reply;
    TermState&          state;   // holds test_mode (below)
};

struct Verb { const char* name; void (*fn)(const Command&, Ctx&); };

static const Verb kVerbs[] = {
    {"set",   verb_set},    {"get",  verb_get},   {"query", verb_query},
    {"cv",    verb_cv},     {"gate", verb_gate},  {"midi",  verb_midi},
    {"pad",   verb_pad},    {"fx",   verb_fx},    {"config",verb_config},
    {"mode",  verb_mode},   {"caps", verb_caps},  {"help",  verb_help},
    {"describe", verb_describe},
};
```

### Flow

```text
LineSink(line) -> tokenize -> Command
    match verb in kVerbs           -> handler(cmd, ctx)          (platform-reflective, target A/B-L1)
    no match -> engine.handle_command(view, reply)               (engine-specific, target B)
                    returns false  -> reply.err("unknown-verb")
```

Platform verbs are tried first so every engine gets the reflective surface for free; anything unknown falls through to the engine's own handler, then to an error. This is the exact two-tier split from the control doc (target A central, target B per-engine).

### Addressing params/config: names, resolved cheaply

Phase 1 addresses parameters and configs **by name**, not by numeric id - names survive an enum reorder (critical for a test harness) and are the whole point of a human-typable channel. The cost is a flat `const char*` table indexed by the enum, ~24 + 6 short strings (a few hundred bytes of flash) - negligible even for the reverb build.

```cpp
// src/terminal/names.cpp  (part of SPK_TERMINAL, always on)
static const char* const kParamNames[] = {   // index == (uint8_t)ParamId
    "pos","fluxfb","env","envsize","size","win","polyslice","speed",
    "fluxint","gritint","fluxmix","gritmix","feedback","mix","modspeed","modamp",
    "tempo","clickmix","panspeed","panrange","keyinterval","crossfade","altpos","aux",
};
static const char* const kConfigNames[] = {   // index == (uint8_t)ConfigId
    "route","modtype","lfoshape","mode","startmodon","sizemodon",
};
bool param_from_name (const char*, spotykach::ParamId&);   // linear scan
bool config_from_name(const char*, spotykach::ConfigId&);
```

Refinement to the control doc's flag matrix: the **flat id<->name table is part of `SPK_TERMINAL`** (cheap, enables robust named addressing). `SPK_TERMINAL_REFLECT` adds only the *structured descriptor*

- per-engine ranges, deck-scope, which params a build actually uses, and the `describe` dump - which is the genuinely expensive, engine-aware part. (Numeric ids are also accepted as a fallback, so a minimal build can drop the names by compiling the table out.)

### Verb catalog (phase 1)

Deck token is `A`/`B`; global params ignore it (pass `A`). Values are floats unless noted; semantic range is engine-defined (the engine clamps), so dispatch passes finite values through and the harness asserts via `get`.

| Command | IEngine binding (`src/engine/iengine.h`) | Reply |
|---------|------------------------------------------|-------|
| `set param <name> <deck> <f>` | `set_param(id, deck, f)` (:52) | `ok` |
| `set modspeed <deck> <f> [sync]` | `set_mod_speed(deck, f, sync)` (:54) | `ok` |
| `get param <name> <deck>` | `param(id, deck)` (:53) | `ok <f>` |
| `config <name> <deck> <int>` | `set_config(id, deck, int)` -> changed (:75) | `ok <0/1>` |
| `cv voct\|mix\|size\|xfade <deck> <f>` | `cv_voct/cv_mix/cv_size_pos(deck,f)`, `cv_crossfade(f)` (:112-115) | `ok` |
| `gate <deck>` | `on_gate_trigger(deck)` (:118) | `ok` |
| `midi note <ch> <note>` | `handle_midi_note(ch, note)` (:88) | `ok` |
| `midi msg <status> <d1> <d2>` | `handle_midi_message(status,d1,d2)` (:93) | `ok` |
| `midi transport start\|stop` | `handle_midi_transport(bool)` (:89) | `ok` |
| `pad play\|rec <deck> [rev]` | `on_play_pad/on_record_pad(deck, reverse)` (:102-103) | `ok [empty=<0/1>]` |
| `pad seq <deck>` | `on_seq_trigger(deck)` (:107) | `ok` |
| `pad stop\|clear <deck>` | `stop_if_generating/clear_buffer(deck)` (:100-101) | `ok` |
| `fx flux\|grit <deck> on\|off` | `set_fx(deck, kind, on)` (:96) | `ok` |
| `fx lock <flux\|grit> <deck>` | `toggle_fx_lock(deck, kind)` | `ok` |
| `fx gritmode <deck>` | `toggle_grit_mode(deck)` | `ok intensity=<f> mix=<f>` |
| `seq trig\|arm\|clear\|disarm <deck>` | `on_seq_trigger` / `on_seq_toggle_arm` / `clear_sequence` / `disarm_track` | `ok` |
| `query recorded\|capacity <deck>` | `audio_recorded_bytes` / `audio_capacity_bytes` | `ok <int>` |
| `query layout <deck>` | `deck_layout(deck)` | `ok <0-3>` (single/slice/chord/none) |
| `query sizetempo <deck>` | `size_sets_tempo(deck)` | `ok <0/1>` |
| `query fit <deck> <fraction>` | `tempo_to_fit(deck, f)` - **not advertised**, takes an argument | `ok <f>` |
| `query reseed <deck>` | `take_param_reseed(deck)` - **not advertised**, latching | `ok <0/1>` |
| `reset [deck]` | every advertised param -> `param_default(id)` | `ok <count written>` |
| `preset save\|load <slot>` | snapshot/restore the advertised params, in RAM | `ok <count>` |
| `query empty <deck>` | `audio_is_empty(deck)` (:122) | `ok <0/1>` |
| `query mix` | `mix()` (:142) | `ok <f>` |
| `query route` | `route()` (:143), mapped to the **selector** encoding | `ok <0/1/2>` |
| `query gateout <deck>` | `gate_out_triggered(deck)` (:119) | `ok <0/1>` |
| `query usb` | the `UsbDiag` bring-up snapshot (`src/terminal/usb_diag.h`) | `ok boot=<n> region=<n> clkcfg=<0/1> hsi48=<0/1> usbsel=<n> usb33den=<0/1> usb33rdy=<0/1> phy=<0/1> pullup=<0/1>` |
| `query cpu\|cpumin\|cpumax` | the platform `CpuLoadMeter` (`src/terminal/cpu_stat.h`) | `ok <f>` - percent of the block budget |
| `reset cpu` | clears the meter's min/max extremes | `ok` |
| `query <other> <deck>` | forwarded to `handle_command` (:B) | engine-defined |
| `caps` | `capabilities()` (:49) | `ok 0x<hex>` |
| `mode test\|run` | sets `TermState::test_mode` (below) | `ok` |
| `describe` | platform tables + `live_params()`/`live_configs()` (below) | descriptor block |
| `help` | lists verbs | `ok` + lines |

`seq trig` and `pad seq` are synonyms - the latter predates the `seq` verb and is kept.

### Composite verbs

`reset` and `preset` are the two commands that do many things at once. Both operate on exactly the set `describe` advertises - live per the engine's mask, minus the platform-owned ids - so what a host can see is what a composite touches. Both reply with a count, so a harness can assert they did something rather than silently matching nothing.

`reset` exists for **test isolation**: `mode test` stops the panel perturbing the engine but leaves the previous test's writes in place, which is how a suite ends up passing in isolation and failing in sequence. `tools/conftest.py` now resets in the `test_mode` fixture. The per-param default comes from `IEngine::param_default()`, which defaults to 0.5 - deterministic, which is what a baseline needs, but not necessarily musical; engines with real neutral values should override it.

`reset cpu` is the one form that touches no params at all - it clears the CPU meter's min/max instead. It shares the verb because it is the same idea (put a measurable thing back to a known baseline), and it has to be a keyword rather than a deck, checked ahead of the deck parse that would otherwise reject it as `bad-deck`. The sequence a measurement wants is `reset cpu` -> drive the engine -> `query cpumax`; without the reset the peak is whatever the boot transient happened to be. See **CPU load** below.

`preset` is **params only, in RAM, non-persistent**. Non-persistent because a test wants to snapshot and restore many times per run and should not wear flash to do it. Params only because of a genuine gap in the engine contract: `param()` can read a parameter back, but `set_config` is **write-only** - there is no config getter on `IEngine`, so configs cannot be captured at all. Restoring a slot that was never saved replies `ok 0` rather than erroring, which keeps the error taxonomy fixed.

What neither does is apply its writes **atomically**. Commands dispatch on the main loop while the audio ISR runs, so a block can start midway through a reset and render with some params applied. Nothing has been bitten by this, and fixing it would need engine cooperation (a defer/commit flag or double-buffered param sets), so it is noted rather than built.

**Two queries are deliberately absent from `describe`**, though both are reachable by name. `fit` takes an argument and the descriptor cannot express arity, so the generic sweep - which calls every advertised query with a deck alone - would fail it. `reseed` is a *latching* read: it returns true once and self-clears, so asking changes the answer and a sweep would consume the flag the platform is waiting for. These are the two shapes the safe-to-call rule in [`terminal-target-b.md`](terminal-target-b.md) exists to exclude, and they are the reason it is a per-entry property rather than a category.

`IEngine::set_aux_active` is **not** exposed: the UI pushes it every `process()` for a `CapAux` engine (`core.ui.cpp:186-190`), so a terminal write would be overwritten within a millisecond - the same trap as the panel switches, and not worth a fourth freeze point for a display hint.

`config route` is global; the others in that verb are per-deck. `set_config` returns whether the value changed, echoed so a test can assert idempotence.

### CPU load - `query cpu` / `cpumin` / `cpumax`

The platform has always owned a whole-callback `daisy::CpuLoadMeter` (`src/meter.h`), but reading it meant building `METER=1`, which brings up a **second USB device** (`_meter_usb`, `FS_EXTERNAL`) whose only job is to print the numbers. That device claims the same OTG core the terminal needs, so `METER=1 TERMINAL=1` is not a build that can work - and the numbers TODO.md P2 wants are exactly the numbers that second device existed to produce.

The channel makes it unnecessary. Measuring is cheap - two `System::GetTick()` reads per block - so a `TERMINAL=1` build drives the meter itself and reports on request. `app.cpp` therefore gates the
`Init`/`OnBlockStart`/`OnBlockEnd` calls on an internal `SPK_CPU_METER` (`METER || SPK_TERMINAL`) and
keeps the USB-printing block under `METER` alone. With neither flag the macro is undefined and nothing changes; `cpu_stat.cpp` compiles to 0 bytes, like the rest of the terminal.

Three separate `Float` queries rather than one `Text` line of `avg=.. min=.. max=..` (the shape `usb` uses), because these are the numbers a sweep collects: a `Float` query replies as a bare `ok <value>` the existing host tooling already parses, where a `Text` blob would need a parser of its own.

Values are **percent of the block budget**, not the meter's native 0..1 - the unit every consumer already speaks (`METER=1` prints `load%`, and TODO.md's headroom figures are percentages).

`min`/`max` are extremes since the last `reset cpu`, not a rolling window, so a measurement must bound its own interval. One narrow caveat: `CpuLoadMeter::Reset()` sets all three to `NAN` and re-seeds on the next `OnBlockEnd()`, so a read landing inside that one-block gap (~1 ms at 48 kHz) reports `nan`. That is left as `nan` rather than coerced to 0 - "no sample yet" and "zero load" are different answers.

### Target B - engine-specific verbs and L1 state

> **Unused as of 2026-07-31, and see [`terminal-target-b.md`](terminal-target-b.md) for why.** No engine > implements this hook. The mechanism below works, but it is invisible to `describe`, makes every engine > re-implement matching/validation/error replies, and carries no type information - so the generic sweep > cannot see it and every use would need per-engine host code. That doc proposes a declared query table > alongside this hook, which keeps the escape hatch while making engine state discoverable and > sweepable.

One new virtual (declared in the control doc), no-op default:

```cpp
// src/engine/iengine.h
virtual bool handle_command(const CommandView& cmd, TextSink& reply) { return false; }
```

`CommandView` is `{ const char* const* argv; uint8_t argc; }` - a read-only view over the tokenized line, so the engine never sees the codec. It is the home for (a) engine-unique verbs and (b) `query` names the platform does not know (e.g. `query grains A`, `query loop_ms A`). An engine that implements any should set the `CapTerminal` bit so `help`/`describe` can note it. Example:

```cpp
bool TapeEngine::handle_command(const CommandView& c, TextSink& r) override {
    if (c.argc == 3 && !strcmp(c.argv[0], "query") && !strcmp(c.argv[1], "loop_ms")) {
        DeckRef::Ref d; if (!parse_deck(c.argv[2], d)) { r.err("bad-deck"); return true; }
        r.ok_f32(loop_length_ms(d)); return true;   // handled (even on error) -> return true
    }
    return false;   // not ours -> dispatcher emits "err unknown-verb"
}
```

Contract: return `true` if the verb was *recognized* (including when replying with an error), `false` only if it is not this engine's verb at all.

## `mode test` - input isolation

The dispatcher owns a single flag; the platform reads it to stop feeding *physical* input to the engine so terminal-injected stimulus is the only driver (determinism). The flag lives in `TermState` and is exposed read-only:

```cpp
bool Terminal::test_mode() const;   // false at boot; set by `mode test`, cleared by `mode run`
```

The platform must consult it at exactly the points where physical input reaches the engine - it does **not** touch output rendering:

| Consult point | File | Skip when `test_mode()` |
|---------------|------|-------------------------|
| knob/pot -> engine params in the UI tick | `src/app.cpp:296` (`_ui.tick()`) | the pot-apply pass that calls `set_param` |
| CV jack reads -> engine | `src/app.cpp:297` (`_ui.read_cv()`) | the `cv_*` forwarding |
| gate-in -> engine | `src/app.cpp:123` (`process_gate_in`) | the `on_gate_trigger` forwarding |
| panel switches -> engine config | `src/ui/core.ui.cpp` (`_process_switches`) | the whole pass |

The switch row is not optional, contrary to this spec's original "pads/switches can stay live (harmless)". Switches **assert** their position every iteration rather than reacting to a change, so an ungated pass overwrites any `config` the terminal sets within a millisecond - `config route A 0` followed by `query route` returns the switch's value, and `set_config` keeps reporting `changed=1` because the value really is changing back each pass. Found on hardware, 2026-07-31. Pads remain ungated: they are event-driven, so they only perturb a test if someone physically touches the panel.

These are the only three sites; each is a single early-return guarded by `if (_terminal.test_mode())`. Pads/switches driven from the same UI can stay live (harmless) or be gated too - a follow-up detail, not a phase-1 blocker. This is the whole of the "test mode" mechanism: no output arbitration, no per-pin ownership.

## `describe` - introspection

`describe` emits the device's control surface so a host configures itself. Metadata ownership is split so the per-engine burden is a masks pair (see [`terminal-control.md`](terminal-control.md)):

- **Platform-owned static tables.** Per `ParamId`: name (the `kParamNames` table), deck-scope, and range. Scope and range are properties of the id, not the engine - a fixed table marks the global ids (`Tempo`, `ClickMix`, `PanSpeed`, `PanRange`, `KeyInterval`, `Crossfade`; and `Route` among configs) and the non-normalized ranges (`Tempo`, `KeyInterval`); everything else is per-deck, `0..1`. Per `ConfigId`: name + enum labels (e.g. `mode: 0:slice 1:reel 2:drift`, `route: 1:dmono 2:stereo 3:genstereo`).

- **Engine-owned liveness masks.** Two new `IEngine` virtuals, default "all live", so the descriptor lists only what the engine implements (else a generic round-trip sweep gets false failures on ignored params):

  ```cpp
  // src/engine/iengine.h
  virtual ParamMask  live_params()  const { return ~0u; }   // bitset over ParamId (24 values < 32)
  virtual ConfigMask live_configs() const { return ~0u; }   // bitset over ConfigId  (6 values)
  // e.g. TapeEngine: return (1u<<(int)ParamId::Size)|(1u<<(int)ParamId::Feedback)|... ;
  ```

  `ParamMask = uint32_t`, `ConfigMask = uint8_t`. The dispatcher walks the platform tables and emits a line only where the engine's mask bit is set.

### Output format

A line-per-item block, machine-parseable, terminated by `end` so the host knows the dump is complete:

```text
> describe
descr engine=tape version=0.9.3-tape masked=1
param size deck 0..1
param speed deck 0..1
param crossfade global 0..1
config route 0:stereo 1:dmono 2:genstereo
query empty deck
caps 0x00000133
end
```

Three corrections to the original sketch, all found by running it:

- **`masked=<0|1>`** on the `descr` line reports whether the engine narrowed `live_params()`/
  `live_configs()`. With `masked=0` the descriptor is the entire `ParamId` enum and a round-trip sweep proves nothing, so the host harness skips instead of emitting false failures.

- **No `tempo`.** `Tempo`, `KeyInterval` and `ModSpeed` are platform-owned - the first two live in the Transport service, the third arrives via `set_mod_speed()` - so `set_param` never sees them and advertising them made the sweep assert on values that went nowhere. With them gone, every advertised param is uniformly `0..1`; the old `40..300` and `1..64` ranges were display units the setter never took, which is precisely why they were wrong.

- **`config` lines carry no scope token**, matching the parser (`config <name> <i:label>...`).

Line tags (`descr`/`param`/`config`/`query`/`caps`/`end`) keep it greppable and let the host build its param map, ranges, and autocompletion without positional parsing. The `query` lines list the platform-known state names plus any an engine advertises (a later `describe` hook into `handle_command` can enumerate engine-specific queries; phase 1 lists the platform set).

### Streaming and TX sizing

A full descriptor is ~30-40 lines (~1-1.5 KB) - larger than one CDC packet and larger than a small TX FIFO. Two options; pick one in implementation:

- **Size the TX FIFO to hold a full descriptor** (~2 KB SRAM). Simplest: `verb_describe` enqueues every line and `flush_tx` drains it over successive `process()` iterations. Recommended - 2 KB is trivial on the H750 and keeps `describe` a plain sequence of `write()` calls.

- **Resumable generator.** Keep a small FIFO and have `describe` yield lines as space frees (a cursor in `TermState`). Less RAM, more state machinery. Only needed if 2 KB is contested.

Either way `describe` never blocks: unsent lines wait in the FIFO / generator, drained non-blocking.

## Reply grammar

Deterministic and greppable, one reply per command, newline-framed so it interleaves safely with any `[tag]` log lines (transport guarantees single-threaded TX):

```text
reply   := "ok" [SP result] CRLF
         | "err" SP reason CRLF
result  := float | int | "0x" hex | (name SP value)*      ; float via append_f32, never %f
reason  := token                                          ; e.g. bad-deck, unknown-verb, bad-arg,
                                                          ;      too-many-args, unknown-param, no-arg
```

A command that maps to a `void` `IEngine` method replies bare `ok`. A `get`/`query` replies `ok <value>`. Errors are a fixed, enumerated token set so host asserts are stable.

## Wiring

### Transport binding (the `LineSink`)

The transport spec left `LineSink` as a stub; dispatch fills it. `Terminal` gains the engine reference and routes assembled lines through the dispatcher:

```cpp
// src/terminal/terminal.h
class Terminal {
  public:
    void init(spotykach::IEngine& engine);   // stores &engine; transport init as before
    void process();                          // drain RX -> tokenize -> dispatch -> flush TX
    void write(const char* s, size_t n);     // TX FIFO (used by TextSink)
    bool test_mode() const { return _state.test_mode; }
  private:
    void on_line(char* line, size_t n);      // tokenize + dispatch(cmd, {engine, sink, state})
    spotykach::IEngine* _engine = nullptr;
    TermState           _state;
    // ... rx ring, line assembler, tx fifo from the transport spec ...
};
```

### app.cpp

```cpp
// AppImpl::Init(), after Log::StartLog / boot banner (app.cpp:218)
#if SPK_TERMINAL
    _terminal.init(_engine);       // _engine is ActiveEngine&, binds as IEngine&
#endif
// AppImpl::Loop() body
#if SPK_TERMINAL
    _terminal.process();
#endif
```

The engine is a single active instance (`ActiveEngine _engine`, `src/app.cpp:105`), so the dispatcher holds one `IEngine&` for the life of the program - no per-command lookup.

## Worked example - a delay feedback round-trip test

```text
> mode test
ok
> config mode A 1
ok 1
> set param feedback A 0.75
ok
> get param feedback A
ok 0.7500
> pad rec A
ok
> query empty A
ok 0
> mode run
ok
```

A pytest wrapper (`tools/`) sends these lines and asserts on the `ok ...` replies - deterministic, because `mode test` froze the knobs/CV/gate so only these commands drove the engine.

## Error taxonomy (fixed tokens)

`unknown-verb`, `unknown-param`, `unknown-config`, `bad-deck`, `bad-arg`, `no-arg`, `too-many-args`, `line-too-long` (from transport), `overflow` (from transport). Stable set so host assertions do not chase wording.

## Out of scope

`measure`/`stim` (phases 2/3), OSC/SLIP codec (`SPK_TERMINAL_OSC`), enumeration of engine-specific `query` names inside `describe` (phase 1 lists the platform set), and any output arbitration. Phase 1 is: line in, one `IEngine` call or engine `handle_command`, one deterministic reply out - plus `describe` for introspection.
