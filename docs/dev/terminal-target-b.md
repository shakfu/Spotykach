# Target B - engine-specific commands, redesigned

Status: **B1 built (2026-07-31); B2/B3 unbuilt.** The declared query table, the platform-side matching /
validation / framing / description, the `safe` rule and the host-side `kind` parsing all landed, with
tape and radio as the first two users. `handle_command` remains for side-effecting verbs. What follows
is the design as written; deviations and what is still open are marked at the end.

Context: [`terminal-control.md`](terminal-control.md) (the two-target split), [`terminal-dispatch.md`](terminal-dispatch.md) (how dispatch works today), [`terminal-impl.md`](terminal-impl.md) (what actually landed).

## What exists, and why nobody uses it

```cpp
// src/engine/iengine.h
virtual bool handle_command(const CommandView& cmd, TextSink& reply) { return false; }
```

The dispatcher tries its own verb table first; anything unmatched - and any `query` name it does not know - falls through to this. Return `true` if the verb was *recognized* (including when replying with an error), `false` if it is not yours.

The mechanism is sound and costs nothing. It is unused because of what surrounds it:

1. **It is invisible to `describe`.** Phase 1 emits only the platform query set, so a host cannot discover that an engine added anything. Every engine-specific name has to be hardcoded in a per-engine test - which is exactly the per-engine knowledge the descriptor exists to eliminate. The generic sweep, the one piece of tooling that has actually paid off, cannot see target B at all.

2. **Each engine re-implements dispatch.** `strcmp` chains, arity checks, deck parsing and error replies, in every engine, for every verb. That is 6-10 lines of boilerplate before any real work, and each copy is a place to get the error taxonomy subtly wrong.

3. **There is no type information.** Even if a host knew `loop_ms` existed, nothing says whether it returns a float, a bool, an enum or free text, nor over what range. A generic consumer can only print the string.

4. **`CapTerminal` is hand-set.** A bit an engine must remember to set, that nothing verifies - so it will drift the moment anyone uses it.

The through-line: the platform half of the channel is a *declared* surface (static tables the dispatcher walks, which is what makes `describe` and the generic sweep possible), while the engine half is *imperative* (a function that either handles a string or does not). The two halves cannot be tooled the same way, so the engine half gets no tooling, so nobody writes to it.

## Goals

1. **Discoverable.** Engine state appears in `describe` alongside platform state, in the same shape, so an existing generic host picks it up with no new code.

2. **Near-zero per-engine burden.** Declaring a name and a reader, not writing a parser.

3. **Impossible to desynchronise.** One source of truth for both dispatch and description - the drift hazard is what killed the current hook.

4. **Generically sweepable** where that is safe, and clearly not where it is not.

5. **Free when unused.** Engines that want nothing pay nothing: no vtable growth beyond the existing virtuals, no flash.

6. **Still an escape hatch.** Whatever is proposed must not remove the ability to do something odd.

## The distinction that matters: safe to call, not query vs action

Target B currently lumps together two things a generic host must treat differently. The obvious split is
by category - reads versus side-effecting verbs - and that is most of the story:

- **Queries** - `loop_ms`, `station`, `grain_count`, `bookmark`. Derived state, side-effect free, safe in
  any order. This is the overwhelming majority of what engines want and all of the value for testing.
- **Actions** - `reload`, `seek 12.5`, `save`. A generic host must **not** call these speculatively;
  sweeping them could clear a buffer or write an SD card.

But the category is a judgment call and the boundary leaks - see the command model in
[`terminal-control.md`](terminal-control.md#the-command-model-params-queries-and-actions). Actions
already return values here (`pad play A` -> `ok empty=1`), so a reply does not make something a query.
And `IEngine::take_param_reseed()` is a *read* that self-clears, so it would sit in the query column
while being unsafe to sweep.

So the entry is tagged with the property the tooling actually needs rather than the category:

```
safe = idempotent && side-effect-free
```

`query empty` is safe. A latching read is not, whatever we call it. `pad play` is unsafe regardless of
its reply. **Only safe entries are advertised in `describe`**, which makes the sweep correct by
construction: it calls everything it can see, and it can only see things that are safe to call. An
unsafe read stays reachable through the free-form hook for a human or a per-engine test, it is simply
not offered to a generic consumer.

The trade-off, stated honestly: a category is self-documenting, whereas `safe` puts the burden on
whoever declares the entry to get one boolean right. The mitigation is that the default is the
conservative one - an entry is advertised only if it declares itself safe, so forgetting the flag costs
discoverability rather than correctness.

## Proposal

**Queries become a declared table. Actions keep the free-form hook.**

### The engine side

```cpp
// src/engine/terminal_io.h
enum class ValueKind  : uint8_t { Bool, Int, Float, Enum, Text };
enum class QueryScope : uint8_t { Global, Deck };

struct EngineQuery {
    const char* name;    // "loop_ms" - must not collide with a platform query name
    QueryScope  scope;   // Deck -> the platform validates and passes a deck; Global -> DeckRef::A
    ValueKind   kind;    // how a host should parse the value
    const char* labels;  // Enum only: "0:stopped 1:playing 2:winding"; nullptr otherwise
    bool        safe;    // idempotent AND side-effect free -> advertised in describe and swept.
                         // A latching read (see take_param_reseed) must declare false: it stays
                         // callable, but is not offered to a generic host.
};

struct EngineQueryTable { const EngineQuery* items; uint8_t count; };
```

```cpp
// src/engine/iengine.h - two new virtuals, both no-op by default
virtual EngineQueryTable engine_queries() const { return {nullptr, 0}; }
virtual void read_engine_query(uint8_t index, DeckRef::Ref, TextSink&) {}
```

An engine declares metadata once and answers by index:

```cpp
// tape_engine.h
enum Q : uint8_t { Q_LOOP_MS, Q_HEAD, Q_STATE, Q_COUNT };
static constexpr EngineQuery kQueries[] = {
    { "loop_ms", QueryScope::Deck,   ValueKind::Float, nullptr, /*safe*/ true },
    { "head",    QueryScope::Deck,   ValueKind::Float, nullptr, /*safe*/ true },
    { "state",   QueryScope::Global, ValueKind::Enum,  "0:stopped 1:playing 2:winding", true },
};
static_assert(sizeof(kQueries) / sizeof(kQueries[0]) == Q_COUNT, "table/enum drift");

EngineQueryTable engine_queries() const override { return { kQueries, Q_COUNT }; }

void read_engine_query(uint8_t i, DeckRef::Ref d, TextSink& r) override {
    switch (i) {
        case Q_LOOP_MS: r.append_f32(loop_length_ms(d));      break;
        case Q_HEAD:    r.append_f32(head_position(d));       break;
        case Q_STATE:   r.append_i32(static_cast<int>(_st));  break;
    }
}
```

Index-based rather than function pointers: no casts from `IEngine&` back to the concrete type, no relocations, the table stays `constexpr` in flash, and the `static_assert` catches enum/table drift at compile time.

### The platform side

The dispatcher owns everything that is currently boilerplate:

- **Matching.** `query <name>` checks the platform set, then walks `engine_queries()`. Platform names win, so an engine cannot shadow `empty`/`mix`/`route`/`gateout`/`usb`.

- **Arity and deck validation**, from the declared scope. `Deck` requires a valid deck token or `err bad-deck`; `Global` ignores/rejects one. **Engines write no error handling.**

- **Reply framing.** The platform writes `ok `, calls `read_engine_query`, writes CRLF. The engine appends only the value, so it cannot get the grammar wrong.

- **Description.** `describe` walks the same table and emits **only the entries marked `safe`**. One
  source of truth; drift is structurally impossible, and the sweep cannot reach an unsafe entry because
  it never learns the name.

- **`CapTerminal`** becomes derivable: set it when `engine_queries().count > 0`, rather than asking engines to remember.

### Wire format - no new tag needed

Engine queries emit as ordinary `query` lines, with a kind token added to *all* query lines:

```
query empty   deck   bool
query mix     global float
query route   global enum 0:stereo 1:dmono 2:genstereo
query loop_ms deck   float
query state   global enum 0:stopped 1:playing 2:winding
```

This is **backward compatible**: today's `parse_describe` reads `tok[1]` as the name and `tok[2]` as the scope and ignores the rest, so an older host keeps working against newer firmware.

The payoff is that a host cannot tell a platform query from an engine one, and does not need to. The existing `test_query_answers` sweep covers engine queries the day an engine declares one, with **no new host code** - which is the whole point, and the thing the current hook cannot deliver.

### Actions stay as they are

`handle_command` remains, unchanged, for side-effecting verbs and genuinely odd cases. It keeps the escape hatch and avoids designing a second mechanism before there is a real user for one. The cost is that actions stay undiscoverable - accepted deliberately, because a generic host must not invoke them blindly anyway. If a real need appears, an action table is a natural follow-on (sketch below).

## Host side

- `parse_describe` gains `kind` and `labels` per query (`queries` becomes `name -> QueryDesc`).

- `test_query_answers` additionally asserts the reply **parses as its declared kind**, and that an `Enum` value is one of its declared labels - the same class of check as `test_config_query_round_trip`, which caught the route encoding bug.

- `skterm` completion picks up engine query names for free.

## Testing

- Off-target (`host/test_terminal.cpp`): the mock engine declares a table covering every `ValueKind`, both scopes, a name that collides with a platform query (platform must win), an out-of-range index, and an empty table. Assert dispatch, validation, reply framing and `describe` emission.

- On-target: nothing new. The generic sweep picks engine queries up automatically once an engine ships a table - so first real use is also its first test.

## Phasing

- **B1** - the query table, platform validation/framing/description, host `kind` parsing. Land with two real users (tape `loop_ms`, radio `station`) so the design is validated by use rather than by inspection. Everything else stays as it is.

- **B2** - an action table, *only if* B1 shows a real need. Same shape (`name`, arity, arg kinds) plus an explicit `sweepable: false`, so a host can list actions without invoking them.

- **B3** - writable engine-specific values. Deliberately deferred: `set param` already covers writes through the `ParamId` surface, and a parallel writable channel is where scope creep would start.

## Alternatives considered

**A. Keep the hook, add a separate `describe_commands(TextSink&)`.** Smallest change: an engine emits its own descriptor lines. Rejected - two sources of truth that must agree, in every engine, forever. The sync hazard is exactly the failure this document exists to avoid, and this session already spent an afternoon on a different silent-desync bug.

**B. Reuse the `ParamId` space.** Give engines generic "engine slot" ids the way `Aux` and `AltPos` already work, so engine specifics flow through the existing param surface with no new mechanism. Genuinely attractive - fewer concepts, and the sweep already covers it. Rejected because it only carries `float`, is write-oriented rather than read-oriented, and the 24-id enum is shared by every engine: two engines using slot 3 for different things would both describe it under the same name. Worth revisiting if the query table proves heavier than it looks.

**C. Do nothing.** Defensible: target B has no user today, and unused mechanisms are a liability. The argument against is that the *reason* it has no user is the friction described above, so "no demand" is not evidence of no need. B1 is small enough to test that claim cheaply.

## Footprint

Per query: ~12 B of table (two pointers, two bytes, padding) plus the name string, plus one `switch` arm. Five queries on one engine is well under 200 B of flash, all in the `#if SPK_TERMINAL` build. The two new virtuals add two vtable slots to `IEngine` under the flag - the flag already changes the vtable, so the ABI tagging in [`abi_tag.h`](../../src/abi_tag.h) covers it.

## Open questions

1. **Text-valued queries and the reply grammar.** `ok <text>` with spaces breaks the "one token" assumption hosts make. Options: forbid `Text`, quote it, or restrict to a no-whitespace token. Leaning toward the last - it keeps the grammar trivial and covers the realistic case (a station or file name could be slugged).

2. **Cost control.** Queries run on the main loop between `_ui.process()` calls. A query that walks a large buffer would stall the UI. Document a "cheap and side-effect free" contract, or measure and bound it?

3. **Deck argument on a `Global` query** - reject as `bad-arg`, or ignore as the platform verbs do today? Consistency says ignore; strictness catches host bugs earlier.

4. **Name collisions with *future* platform queries.** An engine query named `tempo` today would break the day the platform adds one. Reserve a prefix (`e.`), or accept that platform-wins is enough and the descriptor will show the change?

5. **Does `mode test` need to gate anything here?** Safe queries are pure reads, so no. The latching
   case that would have needed it - a "since last read" counter - is now excluded by declaring
   `safe = false`, which keeps it out of the sweep entirely.

6. **The descriptor cannot express arity.** `query fit <deck> <fraction>` takes an argument, so it
   cannot be advertised at all - the sweep calls every advertised query with a deck alone. Today that
   is handled by omission, which costs discoverability. If parameterized queries become common the
   table needs an arg-count (and probably arg-kind) field.

7. **Who audits the `safe` flag?** Nothing verifies it; a mis-declared entry silently becomes sweepable.
   Options: a naming convention, a review checklist, or accept it as the same class of trust already
   placed in `live_params()`. Worth a decision before the second engine adopts this, not the first.


## What actually landed (B1, 2026-07-31)

- `EngineQuery` / `EngineQueryTable` / `ValueKind` / `QueryScope` in `engine/terminal_io.h`; the two
  virtuals `engine_queries()` and `read_engine_query()` on `IEngine`, both no-op by default.
- **The platform queries became a table of the same shape** (`kPlatformQueries` in `dispatch.cpp`) with
  its own index-based reader. That was not in the original sketch and is the change that made the rest
  clean: `describe` now walks one code path over both halves, so the platform cannot drift from the
  engine either. The `PQ_COUNT` static_assert guards the table/enum pairing on both sides.
- Wire format `query <name> <scope> <kind> [labels]`, emitted for `safe` entries only. Backward
  compatibility was verified in practice, not just claimed: a device running pre-`kind` firmware was
  swept by the new host, which defaults the missing token to `text`.
- `CapTerminal` is derived from a non-empty table rather than hand-set, in both `caps` and `describe`.
- Two real users: **tape** (`slot`, `loopmode`, `speed` - the varispeed actually in effect, as opposed
  to the PITCH knob value `get param speed` already reports) and **radio** (`station` - which station is
  *actually* streaming, `-1` for none, so a test can assert a seek completed rather than that a knob
  moved; plus `stations`, `bank`).
- The platform's own latching read (`reseed`) now declares `safe = false` in the same table, so the rule
  is enforced by one mechanism for both halves rather than by omission.

Off-target coverage exercises every `ValueKind`, both scopes, a deliberate name collision with a
platform query (platform wins), a `safe = false` entry (reachable by name, never advertised), and the
derived `CapTerminal` bit.

## Still open after B1

- **B2 (actions).** Not built. `handle_command` is still the only route for side-effecting verbs, and
  still undiscoverable - deliberately, since a generic host must not invoke them speculatively.
- **Arity.** `query fit <deck> <fraction>` still cannot be advertised, because the table has no
  arg-count field. Handled by omission, which costs discoverability.
- **Nothing audits `safe`.** A mis-declared entry silently becomes sweepable. Same class of trust as
  `live_params()`, but worth a convention before this spreads further.
- **`Text` values with spaces.** `query usb` returns a key=value string, which works because hosts take
  the whole remainder - but it breaks the one-token assumption a stricter parser might make.
