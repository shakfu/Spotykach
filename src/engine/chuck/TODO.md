# TODO - ChucK & Csound engines

Deferred work specific to the script-engine pair (`src/engine/chuck`, `src/engine/csound`). These two engines share a knob vocabulary and a near-identical patch-swap path, so issues here usually apply to both. See `docs/dev/chuck-impl.md` and the root `TODO.md` for wider context.

---

## P1 - Patch swap should keep the patch's own defaults, then soft-takeover the knobs

**Status:** confirmed in ChucK; confirmed identical in Csound.

**Symptom.** When the selected patch changes (Alt+PITCH commit, or boot auto-load), the new patch does **not** start from the default values its own code declares. Instead the engine immediately overwrites every mapped global/channel with the *current knob positions*, so the patch jumps to wherever the pots happen to sit. Loading a fresh `.ck`/`.csd` therefore never sounds the way the patch author intended at its defaults.

**Desired behaviour (soft takeover / "pickup", a.k.a. catch mode).** On a patch swap:

1. The new patch boots at **its own default values** (the value its code assigns to each global/channel, or the language default of 0 if it assigns none) - the host writes nothing.

2. Each knob is then **inactive**: physically moving it does *not* change the parameter yet.

3. A knob only **takes over** once its physical position crosses (comes within a small threshold of) the parameter's current value; from that crossing on, it controls the parameter normally.

This is exactly the behaviour the platform already implements for engine/drum swaps - see Reuse below. The current behaviour is the deliberate *opposite* choice and must be reversed for these engines.

### Root cause

Both engines, on every swap, replay the cached knob state into the freshly compiled patch:

- ChucK: `ChuckEngine::reseed_globals()` - `src/engine/chuck/chuck_engine.cpp:316`, called from `do_reload()` at `chuck_engine.cpp:366`. It pushes `_cache[slot]` (the last knob value the platform sent) into each global via `setGlobalFloat`.

- Csound: `CsoundEngine::reseed_channels(CSOUND*)` - `src/engine/csound/csound_engine.cpp:141`, called from `do_reload()` at `csound_engine.cpp:197`. Same logic via `csoundSetControlChannel`.

Both carry the same comment justifying it: *"replay the knob positions ... so the patch picks up the current panel state instead of jumping on the next pot move."* That is the intent we now want to drop.

A second, related defect blocks the fix: `param(id, deck)` for both engines returns `_cache[slot]` (the knob value), **not** the patch's actual value (ChucK `chuck_engine.cpp` / Csound `csound_engine.cpp:343`). Soft takeover needs `param()` to report the patch's value so the pickup has the right target to catch.

### Reuse - the platform already does this for engine/drum swaps

Do **not** build a new pickup mechanism. The machinery exists:

- `MValue` (`src/ui/mvalue.h`) is the per-control soft-takeover state. `MValue::set(v)` stores a value *without* marking the pot active; the pot must then move within `kThreshold` (0.02) of `v` before it starts tracking. That is precisely the catch behaviour wanted here.

- `IEngine::take_param_reseed(deck)` (`src/engine/iengine.h:61`) is the existing hook: an engine whose deck->value mapping changes underfoot sets it; the platform polls it each loop and re-seeds its `MValue` pickup cache from `param()` for that deck (`CoreUI::_reseed_focus`, `src/ui/core.ui.cpp:124`). Edrums already uses this for its Rev-pad drum swap.

### Proposed fix (both engines, mirror the change)

1. **Stop overwriting patch defaults.** Remove the `reseed_globals()` / `reseed_channels()` call from `do_reload()` (or gate it off). The freshly compiled patch then keeps its declared defaults.

2. **Make `param()` report the patch's value**, so the platform's MValue reseed catches the right target:

   - Csound: easy - `csoundGetControlChannel(cs, name)` is **synchronous**; have `param()` read it back.

   - ChucK: harder - globals are read **asynchronously** (`getGlobalFloat` + callback, serviced on a VM tick). The `do_reload()` path already runs the VM for one frame (the remove-all flush at `chuck_engine.cpp:359`), which can service a getGlobalFloat issued at swap time; cache the result for `param()` to return. If that proves fiddly, the interim target is the language default 0 (correct for any patch that doesn't assign its globals - which is all current example patches).

3. **Signal the reseed.** After a swap, return `true` once from `take_param_reseed(deck)` for each deck so the platform reseeds its MValue pickup from the new `param()` values. The knobs then sit inactive until moved into threshold of the patch's value - the soft takeover.

### Verify

- Host: extend `host/test_chuck_patch.cpp` / the Csound patch tests if the swap logic moves into a host-reachable unit; otherwise this is **flash-gated** (by ear on the Pod).

- By ear: load a patch whose default differs from the current pot position; confirm it boots at the default and that turning a pot does nothing until it passes through the default, then catches.

### Open questions

- Should patch-default behaviour be the new default for both engines, or opt-in per patch? (Recommend: new default - it matches every other engine's swap semantics via `_reseed_focus`.)

- ChucK async readback: is one VM frame in `do_reload` enough to service the getGlobalFloat callbacks for all 7 mapped globals x 2 decks, or does it need a short pump loop? Needs a bench check.
