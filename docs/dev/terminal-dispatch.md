# Terminal codec + dispatch spec (phase 1)

Status: **implementation-ready spec, unbuilt.** Specifies layers [2] (line codec) and [3] (dispatch / `IEngine` binding) of the terminal channel - what turns a received line into an `IEngine` call and a reply. It sits on top of [`terminal-transport.md`](terminal-transport.md) (which delivers whole lines via `LineSink` and accepts reply bytes via `Terminal::write`) and realizes the stimulus/observation model in [`terminal-control.md`](terminal-control.md). Everything here is `#if SPK_TERMINAL`.

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

```
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
| `query empty <deck>` | `audio_is_empty(deck)` (:122) | `ok <0/1>` |
| `query mix` | `mix()` (:142) | `ok <f>` |
| `query route` | `route()` (:143), mapped to the **selector** encoding | `ok <0/1/2>` |
| `query gateout <deck>` | `gate_out_triggered(deck)` (:119) | `ok <0/1>` |
| `query usb` | the `UsbDiag` bring-up snapshot (`src/terminal/usb_diag.h`) | `ok boot=<n> region=<n> clkcfg=<0/1> hsi48=<0/1> usbsel=<n> usb33den=<0/1> usb33rdy=<0/1> phy=<0/1> pullup=<0/1>` |
| `query <other> <deck>` | forwarded to `handle_command` (:B) | engine-defined |
| `caps` | `capabilities()` (:49) | `ok 0x<hex>` |
| `mode test\|run` | sets `TermState::test_mode` (below) | `ok` |
| `describe` | platform tables + `live_params()`/`live_configs()` (below) | descriptor block |
| `help` | lists verbs | `ok` + lines |

`config route` is global; the others in that verb are per-deck. `set_config` returns whether the value changed, echoed so a test can assert idempotence.

### Target B - engine-specific verbs and L1 state

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

The switch row is not optional, contrary to this spec's original "pads/switches can stay live
(harmless)". Switches **assert** their position every iteration rather than reacting to a change, so an
ungated pass overwrites any `config` the terminal sets within a millisecond - `config route A 0`
followed by `query route` returns the switch's value, and `set_config` keeps reporting `changed=1`
because the value really is changing back each pass. Found on hardware, 2026-07-31. Pads remain
ungated: they are event-driven, so they only perturb a test if someone physically touches the panel.

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

```
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
  `live_configs()`. With `masked=0` the descriptor is the entire `ParamId` enum and a round-trip sweep
  proves nothing, so the host harness skips instead of emitting false failures.
- **No `tempo`.** `Tempo`, `KeyInterval` and `ModSpeed` are platform-owned - the first two live in the
  Transport service, the third arrives via `set_mod_speed()` - so `set_param` never sees them and
  advertising them made the sweep assert on values that went nowhere. With them gone, every advertised
  param is uniformly `0..1`; the old `40..300` and `1..64` ranges were display units the setter never
  took, which is precisely why they were wrong.
- **`config` lines carry no scope token**, matching the parser (`config <name> <i:label>...`).

Line tags (`descr`/`param`/`config`/`query`/`caps`/`end`) keep it greppable and let the host build its param map, ranges, and autocompletion without positional parsing. The `query` lines list the platform-known state names plus any an engine advertises (a later `describe` hook into `handle_command` can enumerate engine-specific queries; phase 1 lists the platform set).

### Streaming and TX sizing

A full descriptor is ~30-40 lines (~1-1.5 KB) - larger than one CDC packet and larger than a small TX FIFO. Two options; pick one in implementation:

- **Size the TX FIFO to hold a full descriptor** (~2 KB SRAM). Simplest: `verb_describe` enqueues every line and `flush_tx` drains it over successive `process()` iterations. Recommended - 2 KB is trivial on the H750 and keeps `describe` a plain sequence of `write()` calls.

- **Resumable generator.** Keep a small FIFO and have `describe` yield lines as space frees (a cursor in `TermState`). Less RAM, more state machinery. Only needed if 2 KB is contested.

Either way `describe` never blocks: unsent lines wait in the FIFO / generator, drained non-blocking.

## Reply grammar

Deterministic and greppable, one reply per command, newline-framed so it interleaves safely with any `[tag]` log lines (transport guarantees single-threaded TX):

```
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

```
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
