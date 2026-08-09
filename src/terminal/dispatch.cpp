// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/dispatch.h"

#if SPK_TERMINAL

#include "terminal/command.h"
#include "terminal/cpu_stat.h"
#include "terminal/fmt.h"
#include "terminal/names.h"
#include "terminal/preset.h"
#include "version.h"

#include <cstring>
#include <cmath>

#pragma GCC optimize("Os")

namespace spotykach {

namespace {

struct Ctx {
    IEngine&   engine;
    TextSink&  reply;
    TermState& state;
};

// Append a range endpoint: integer form when whole (0..1, 40..300), else 4-decimal float.
void append_num(TextSink& r, float v) {
    if (v == floorf(v) && v > -1e9f && v < 1e9f) r.append_i32(static_cast<int32_t>(v));
    else                                         r.append_f32(v);
}

// --- target A: stimulus + L0 get ------------------------------------------------------------------

void verb_set(const Command& c, Ctx& x) {
    if (c.argc < 2) { x.reply.err("no-arg"); return; }
    if (!strcmp(c.arg(1), "param")) {
        if (c.argc < 5) { x.reply.err("no-arg"); return; }
        ParamId id;      if (!param_from_token(c.arg(2), id)) { x.reply.err("unknown-param"); return; }
        DeckRef::Ref d;  if (!parse_deck(c.arg(3), d))        { x.reply.err("bad-deck");      return; }
        float f;         if (!parse_f32(c.arg(4), f))         { x.reply.err("bad-arg");       return; }
        x.engine.set_param(id, d, f);
        x.reply.ok();
    } else if (!strcmp(c.arg(1), "modspeed")) {
        if (c.argc < 4) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d;  if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        float f;         if (!parse_f32(c.arg(3), f))  { x.reply.err("bad-arg");  return; }
        bool sync = (c.argc >= 5 && !strcmp(c.arg(4), "sync"));
        x.engine.set_mod_speed(d, f, sync);
        x.reply.ok();
    } else {
        x.reply.err("bad-arg");
    }
}

void verb_get(const Command& c, Ctx& x) {
    if (c.argc < 4 || strcmp(c.arg(1), "param")) { x.reply.err("bad-arg"); return; }
    ParamId id;      if (!param_from_token(c.arg(2), id)) { x.reply.err("unknown-param"); return; }
    DeckRef::Ref d;  if (!parse_deck(c.arg(3), d))        { x.reply.err("bad-deck");      return; }
    x.reply.ok_f32(x.engine.param(id, d));
}

void verb_config(const Command& c, Ctx& x) {
    if (c.argc < 4) { x.reply.err("no-arg"); return; }
    ConfigId id;     if (!config_from_token(c.arg(1), id)) { x.reply.err("unknown-config"); return; }
    DeckRef::Ref d;  if (!parse_deck(c.arg(2), d))         { x.reply.err("bad-deck");       return; }
    int32_t v;       if (!parse_i32(c.arg(3), v))          { x.reply.err("bad-arg");        return; }
    bool changed = x.engine.set_config(id, d, static_cast<int>(v));
    x.reply.ok_i32(changed ? 1 : 0);
}

void verb_cv(const Command& c, Ctx& x) {
    if (c.argc < 4) { x.reply.err("no-arg"); return; }
    DeckRef::Ref d;  if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
    float f;         if (!parse_f32(c.arg(3), f))  { x.reply.err("bad-arg");  return; }
    const char* k = c.arg(1);
    if      (!strcmp(k, "voct"))  x.engine.cv_voct(d, f);
    else if (!strcmp(k, "mix"))   x.engine.cv_mix(d, f);
    else if (!strcmp(k, "size"))  x.engine.cv_size_pos(d, f);
    else if (!strcmp(k, "xfade")) x.engine.cv_crossfade(f);   // global; deck arg ignored
    else { x.reply.err("bad-arg"); return; }
    x.reply.ok();
}

void verb_gate(const Command& c, Ctx& x) {
    if (c.argc < 2) { x.reply.err("no-arg"); return; }
    DeckRef::Ref d;  if (!parse_deck(c.arg(1), d)) { x.reply.err("bad-deck"); return; }
    x.engine.on_gate_trigger(d);
    x.reply.ok();
}

void verb_midi(const Command& c, Ctx& x) {
    if (c.argc < 2) { x.reply.err("no-arg"); return; }
    const char* s = c.arg(1);
    if (!strcmp(s, "note")) {
        if (c.argc < 4) { x.reply.err("no-arg"); return; }
        int32_t ch, note;
        if (!parse_i32(c.arg(2), ch) || !parse_i32(c.arg(3), note)) { x.reply.err("bad-arg"); return; }
        x.engine.handle_midi_note(static_cast<uint8_t>(ch), static_cast<uint8_t>(note));
        x.reply.ok();
    } else if (!strcmp(s, "msg")) {
        if (c.argc < 5) { x.reply.err("no-arg"); return; }
        int32_t st, d1, d2;
        if (!parse_i32(c.arg(2), st) || !parse_i32(c.arg(3), d1) || !parse_i32(c.arg(4), d2)) {
            x.reply.err("bad-arg"); return;
        }
        x.engine.handle_midi_message(static_cast<uint8_t>(st), static_cast<uint8_t>(d1),
                                     static_cast<uint8_t>(d2));
        x.reply.ok();
    } else if (!strcmp(s, "transport")) {
        if (c.argc < 3) { x.reply.err("no-arg"); return; }
        if      (!strcmp(c.arg(2), "start")) x.engine.handle_midi_transport(true);
        else if (!strcmp(c.arg(2), "stop"))  x.engine.handle_midi_transport(false);
        else { x.reply.err("bad-arg"); return; }
        x.reply.ok();
    } else {
        x.reply.err("bad-arg");
    }
}

void verb_pad(const Command& c, Ctx& x) {
    if (c.argc < 3) { x.reply.err("no-arg"); return; }
    DeckRef::Ref d;  if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
    const char* s = c.arg(1);
    bool rev = (c.argc >= 4 && (!strcmp(c.arg(3), "rev") || !strcmp(c.arg(3), "reverse")));
    if (!strcmp(s, "play")) {
        bool empty = x.engine.on_play_pad(d, rev);
        x.reply.str("ok empty=");
        x.reply.append_i32(empty ? 1 : 0);
        x.reply.str("\r\n");
    } else if (!strcmp(s, "rec")) {
        x.engine.on_record_pad(d, rev); x.reply.ok();
    } else if (!strcmp(s, "seq")) {
        x.engine.on_seq_trigger(d);     x.reply.ok();
    } else if (!strcmp(s, "stop")) {
        x.engine.stop_if_generating(d); x.reply.ok();
    } else if (!strcmp(s, "clear")) {
        x.engine.clear_buffer(d);       x.reply.ok();
    } else {
        x.reply.err("bad-arg");
    }
}

// Parse "flux"/"grit" into an FxKind. Returns false for anything else.
bool parse_fx_kind(const char* s, FxKind& out) {
    if      (!strcmp(s, "flux")) { out = FxKind::Flux; return true; }
    else if (!strcmp(s, "grit")) { out = FxKind::Grit; return true; }
    return false;
}

void verb_fx(const Command& c, Ctx& x) {
    if (c.argc < 3) { x.reply.err("no-arg"); return; }

    // `fx lock <flux|grit> <deck>` - toggle the fx lock (the pad-hold gesture on the panel).
    if (!strcmp(c.arg(1), "lock")) {
        if (c.argc < 4) { x.reply.err("no-arg"); return; }
        FxKind fk;       if (!parse_fx_kind(c.arg(2), fk)) { x.reply.err("bad-arg");  return; }
        DeckRef::Ref d;  if (!parse_deck(c.arg(3), d))     { x.reply.err("bad-deck"); return; }
        x.engine.toggle_fx_lock(d, fk);
        x.reply.ok();
        return;
    }

    // `fx gritmode <deck>` - cycle the grit sub-effect. An ACTION that returns values: the platform
    // normally uses them to reseed its MValue pickup after the switch, so a test wants them too.
    if (!strcmp(c.arg(1), "gritmode")) {
        DeckRef::Ref d;  if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        const GritReseed g = x.engine.toggle_grit_mode(d);
        x.reply.str("ok intensity=");
        x.reply.append_f32(g.intensity);
        x.reply.str(" mix=");
        x.reply.append_f32(g.mix);
        x.reply.str("\r\n");
        return;
    }

    // `fx flux|grit <deck> on|off`
    if (c.argc < 4) { x.reply.err("no-arg"); return; }
    FxKind fk;       if (!parse_fx_kind(c.arg(1), fk))  { x.reply.err("bad-arg");  return; }
    DeckRef::Ref d;  if (!parse_deck(c.arg(2), d))      { x.reply.err("bad-deck"); return; }
    bool on;         if (!parse_onoff(c.arg(3), on))    { x.reply.err("bad-arg");  return; }
    x.engine.set_fx(d, fk, on);
    x.reply.ok();
}

// `seq trig|arm|clear|disarm <deck>` - the step-sequencer surface. `pad seq <deck>` predates this and
// remains a synonym for `seq trig`.
void verb_seq(const Command& c, Ctx& x) {
    if (c.argc < 3) { x.reply.err("no-arg"); return; }
    DeckRef::Ref d;  if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
    const char* s = c.arg(1);
    if      (!strcmp(s, "trig"))   x.engine.on_seq_trigger(d);
    else if (!strcmp(s, "arm"))    x.engine.on_seq_toggle_arm(d);
    else if (!strcmp(s, "clear"))  x.engine.clear_sequence(d);
    else if (!strcmp(s, "disarm")) x.engine.disarm_track(d);
    else { x.reply.err("bad-arg"); return; }
    x.reply.ok();
}

// --- observation L1 + forwarding ------------------------------------------------------------------

// --- queries: one declared table per half ----------------------------------------------------------
// Platform queries are a table of exactly the same shape an engine declares (EngineQuery), so dispatch
// and `describe` walk one code path over both halves and cannot drift. See terminal-target-b.md.

enum PQ : uint8_t {
    PQ_EMPTY, PQ_MIX, PQ_ROUTE, PQ_GATEOUT, PQ_RECORDED, PQ_CAPACITY,
    PQ_LAYOUT, PQ_SIZETEMPO, PQ_USB, PQ_CPU, PQ_CPUMIN, PQ_CPUMAX, PQ_RESEED, PQ_COUNT
};

const EngineQuery kPlatformQueries[] = {
    { "empty",     QueryScope::Deck,   ValueKind::Bool,  nullptr, true },
    { "mix",       QueryScope::Global, ValueKind::Float, nullptr, true },
    { "route",     QueryScope::Global, ValueKind::Enum,  "0:stereo 1:dmono 2:genstereo", true },
    { "gateout",   QueryScope::Deck,   ValueKind::Bool,  nullptr, true },
    { "recorded",  QueryScope::Deck,   ValueKind::Int,   nullptr, true },
    { "capacity",  QueryScope::Deck,   ValueKind::Int,   nullptr, true },
    { "layout",    QueryScope::Deck,   ValueKind::Enum,  "0:single 1:slice 2:chord 3:none", true },
    { "sizetempo", QueryScope::Deck,   ValueKind::Bool,  nullptr, true },
    { "usb",       QueryScope::Global, ValueKind::Text,  nullptr, true },
    // CPU load as PERCENT of the block budget. Three separate Float queries rather than one Text line
    // of `avg=.. min=.. max=..` (the shape `usb` uses) because these are the numbers a sweep collects:
    // a Float query comes back as a bare `ok <value>` the existing host tooling already parses, where a
    // Text blob would need its own parser. min/max are since the last `reset cpu` - see cpu_stat.h.
    { "cpu",       QueryScope::Global, ValueKind::Float, nullptr, true },
    { "cpumin",    QueryScope::Global, ValueKind::Float, nullptr, true },
    { "cpumax",    QueryScope::Global, ValueKind::Float, nullptr, true },
    // Latching: take_param_reseed returns true once and self-clears, so asking changes the answer.
    // safe=false keeps it out of describe, and therefore out of any generic sweep.
    { "reseed",    QueryScope::Deck,   ValueKind::Bool,  nullptr, false },
};
static_assert(sizeof(kPlatformQueries) / sizeof(kPlatformQueries[0]) == PQ_COUNT,
              "kPlatformQueries out of sync with the PQ enum");

// Append only the VALUE; the caller frames "ok " ... CRLF.
void read_platform_query(uint8_t i, Ctx& x, DeckRef::Ref d) {
    switch (i) {
        case PQ_EMPTY:     x.reply.append_i32(x.engine.audio_is_empty(d) ? 1 : 0); break;
        case PQ_MIX:       x.reply.append_f32(x.engine.mix()); break;
        case PQ_ROUTE:     x.reply.append_i32(route_to_selector(x.engine.route())); break;
        case PQ_GATEOUT:   x.reply.append_i32(x.engine.gate_out_triggered(d) ? 1 : 0); break;
        case PQ_RECORDED:  x.reply.append_i32(static_cast<int32_t>(x.engine.audio_recorded_bytes(d))); break;
        case PQ_CAPACITY:  x.reply.append_i32(static_cast<int32_t>(x.engine.audio_capacity_bytes(d))); break;
        case PQ_LAYOUT:    x.reply.append_i32(static_cast<int32_t>(x.engine.deck_layout(d))); break;
        case PQ_SIZETEMPO: x.reply.append_i32(x.engine.size_sets_tempo(d) ? 1 : 0); break;
        case PQ_RESEED:    x.reply.append_i32(x.engine.take_param_reseed(d) ? 1 : 0); break;
        case PQ_CPU:
        case PQ_CPUMIN:
        case PQ_CPUMAX: {
            CpuStat s; cpu_stat_read(s);
            x.reply.append_f32(i == PQ_CPU ? s.avg : (i == PQ_CPUMIN ? s.min : s.max));
            break;
        }
        case PQ_USB: {
            usb_diag_refresh(x.state.usb);   // live, not the init snapshot
            const UsbDiag& u = x.state.usb;
            TextSink& r = x.reply;
            r.str("boot=");      r.append_i32(u.boot_version);
            r.str(" region=");   r.append_i32(u.memory_region);
            r.str(" clkcfg=");   r.append_i32(u.clocks_configured ? 1 : 0);
            r.str(" hsi48=");    r.append_i32(u.hsi48_ready ? 1 : 0);
            r.str(" usbsel=");   r.append_i32(u.usb_clk_source);
            r.str(" usb33den="); r.append_i32(u.usb33_detector ? 1 : 0);
            r.str(" usb33rdy="); r.append_i32(u.usb33_ready ? 1 : 0);
            r.str(" phy=");      r.append_i32(u.transceiver_on ? 1 : 0);
            r.str(" pullup=");   r.append_i32(u.pullup_asserted ? 1 : 0);
            r.str(" vbussense=");r.append_i32(u.vbus_sensing ? 1 : 0);
            r.str(" dp=");       r.append_i32(u.dp_af_ok ? 1 : 0);
            r.str(" dm=");       r.append_i32(u.dm_af_ok ? 1 : 0);
            r.str(" rst=");      r.append_i32(u.usb_reset_seen ? 1 : 0);
            r.str(" sof=");      r.append_i32(u.sof_seen ? 1 : 0);
            break;
        }
        default: break;
    }
}

// Resolve the deck a query needs from its declared scope. Engines write no deck handling at all.
bool resolve_query_deck(QueryScope scope, const Command& c, Ctx& x, DeckRef::Ref& d) {
    if (scope == QueryScope::Global) { d = DeckRef::A; return true; }
    if (c.argc < 3)                  { x.reply.err("no-arg");  return false; }
    if (!parse_deck(c.arg(2), d))    { x.reply.err("bad-deck"); return false; }
    return true;
}

void verb_query(const Command& c, Ctx& x) {
    if (c.argc < 2) { x.reply.err("no-arg"); return; }
    const char* name = c.arg(1);

    // `fit` takes an ARGUMENT, which the table shape cannot express (see terminal-target-b.md open
    // question 6), so it is handled here and never advertised.
    if (!strcmp(name, "fit")) {
        if (c.argc < 4) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        float f;        if (!parse_f32(c.arg(3), f))  { x.reply.err("bad-arg");  return; }
        x.reply.ok_f32(x.engine.tempo_to_fit(d, f));
        return;
    }

    for (uint8_t i = 0; i < PQ_COUNT; ++i) {          // platform names win over engine names
        if (strcmp(name, kPlatformQueries[i].name)) continue;
        DeckRef::Ref d;
        if (!resolve_query_deck(kPlatformQueries[i].scope, c, x, d)) return;
        x.reply.ok_begin();
        read_platform_query(i, x, d);
        x.reply.ok_end();
        return;
    }

    const EngineQueryTable t = x.engine.engine_queries();
    for (uint8_t i = 0; i < t.count; ++i) {
        if (strcmp(name, t.items[i].name)) continue;
        DeckRef::Ref d;
        if (!resolve_query_deck(t.items[i].scope, c, x, d)) return;
        x.reply.ok_begin();
        x.engine.read_engine_query(i, d, x.reply);   // engine appends the value only
        x.reply.ok_end();
        return;
    }

    // Neither table: the free-form hook, for actions and anything odd.
    CommandView view{ c.argv, c.argc };
    if (!x.engine.handle_command(view, x.reply)) x.reply.err("unknown-verb");
}

// --- composite verbs: reset / preset ---------------------------------------------------------------

// The params a host may meaningfully write: live per the engine's mask, and not platform-owned. Exactly
// the set `describe` advertises, so `reset` and `preset` operate on what a host can actually see.
template <typename F>
void for_each_live_param(IEngine& e, F&& f) {
    const IEngine::ParamMask m = e.live_params();
    for (uint32_t i = 0; i < static_cast<uint32_t>(ParamId::Count); ++i) {
        if (!(m & (1u << i))) continue;
        const ParamId id = static_cast<ParamId>(i);
        if (param_is_platform_owned(id)) continue;
        f(id);
    }
}

// Apply `fn(id, deck)` over a param's decks, honouring scope: a global param lives in the deck-A slot
// only, so writing it per-deck would double-write it (and, on an engine that stores blindly, leave a
// stale value in the B slot). `only` == DeckRef::Count means "every deck".
template <typename F>
int32_t for_each_deck(ParamId id, DeckRef::Ref only, F&& fn) {
    if (param_is_global(id)) {
        if (only == DeckRef::Count || only == DeckRef::A) { fn(id, DeckRef::A); return 1; }
        return 0;
    }
    if (only != DeckRef::Count) { fn(id, only); return 1; }
    fn(id, DeckRef::A);
    fn(id, DeckRef::B);
    return 2;
}

// `reset [deck]` - drive every advertised param to the engine's declared default. The point is a KNOWN
// BASELINE: without it each test inherits whatever the previous one wrote, which is how suites end up
// passing in isolation and failing in sequence. Replies with the number of params written so a harness
// can assert it did something.
void verb_reset(const Command& c, Ctx& x) {
    // `reset cpu` - clear the CPU meter's min/max instead of touching params. Same verb because it is
    // the same idea (return a measurable thing to a known baseline), and it has to be a distinct
    // keyword rather than a deck: the sequence a measurement needs is `reset cpu` -> drive the engine
    // -> `query cpumax`, and without the reset the peak is whatever the boot transient was. Checked
    // before the deck parse, which would otherwise reject it as `bad-deck`.
    if (c.argc >= 2 && !strcmp(c.arg(1), "cpu")) { cpu_stat_reset(); x.reply.ok(); return; }

    DeckRef::Ref only = DeckRef::Count;   // Count == both decks
    if (c.argc >= 2 && !parse_deck(c.arg(1), only)) { x.reply.err("bad-deck"); return; }

    int32_t n = 0;
    for_each_live_param(x.engine, [&](ParamId id) {
        const float d = x.engine.param_default(id);
        n += for_each_deck(id, only, [&](ParamId p, DeckRef::Ref deck) { x.engine.set_param(p, deck, d); });
    });
    x.reply.ok_i32(n);
}

// `preset save|load <slot>` - snapshot and restore the advertised params, in RAM (see preset.h).
// Params only: there is no config getter on IEngine, so configs cannot be captured.
// `load` of a slot that was never saved replies `ok 0` rather than erroring - "restored nothing" is a
// legitimate answer and keeps the error taxonomy fixed.
void verb_preset(const Command& c, Ctx& x) {
    if (c.argc < 3) { x.reply.err("no-arg"); return; }
    int32_t slot;
    if (!parse_i32(c.arg(2), slot) || slot < 0 || slot >= PresetSlots::kSlots) {
        x.reply.err("bad-arg"); return;
    }
    PresetSlots::Slot& s = x.state.presets.slots[slot];
    int32_t n = 0;

    if (!strcmp(c.arg(1), "save")) {
        for_each_live_param(x.engine, [&](ParamId id) {
            n += for_each_deck(id, DeckRef::Count, [&](ParamId p, DeckRef::Ref deck) {
                s.v[static_cast<size_t>(p)][deck == DeckRef::B ? 1 : 0] = x.engine.param(p, deck);
            });
        });
        s.valid = true;
        x.reply.ok_i32(n);
    } else if (!strcmp(c.arg(1), "load")) {
        if (!s.valid) { x.reply.ok_i32(0); return; }
        for_each_live_param(x.engine, [&](ParamId id) {
            n += for_each_deck(id, DeckRef::Count, [&](ParamId p, DeckRef::Ref deck) {
                x.engine.set_param(p, deck, s.v[static_cast<size_t>(p)][deck == DeckRef::B ? 1 : 0]);
            });
        });
        x.reply.ok_i32(n);
    } else {
        x.reply.err("bad-arg");
    }
}

// --- platform meta ---------------------------------------------------------------------------------

void verb_mode(const Command& c, Ctx& x) {
    if (c.argc < 2) { x.reply.err("no-arg"); return; }
    if      (!strcmp(c.arg(1), "test")) { x.state.test_mode = true;  x.reply.ok(); }
    else if (!strcmp(c.arg(1), "run"))  { x.state.test_mode = false; x.reply.ok(); }
    else                                  x.reply.err("bad-arg");
}

void verb_caps(const Command&, Ctx& x) {
    // CapTerminal is derived, not hand-set: an engine that declares queries advertises the bit whether
    // or not it remembered to. Nothing verifies a hand-set bit, so it would drift on first use.
    Capabilities caps = x.engine.capabilities();
    if (x.engine.engine_queries().count > 0) caps |= CapTerminal;
    x.reply.ok_hex(caps);
}

void verb_help(const Command&, Ctx& x) {
    x.reply.line("ok verbs: set get query config cv gate midi pad fx seq reset preset mode caps describe help");
}

const char* kind_name(ValueKind k) {
    switch (k) {
        case ValueKind::Bool:  return "bool";
        case ValueKind::Int:   return "int";
        case ValueKind::Float: return "float";
        case ValueKind::Enum:  return "enum";
        default:               return "text";
    }
}

// `query <name> <scope> <kind> [labels]`. The kind token is new; older hosts read name+scope and
// ignore the rest, so this is backward compatible.
void emit_queries(TextSink& r, const EngineQuery* q, uint8_t n) {
    for (uint8_t i = 0; i < n; ++i) {
        if (!q[i].safe) continue;
        r.str("query ");
        r.str(q[i].name);
        r.str(q[i].scope == QueryScope::Deck ? " deck " : " global ");
        r.str(kind_name(q[i].kind));
        if (q[i].kind == ValueKind::Enum && q[i].labels) { r.str(" "); r.str(q[i].labels); }
        r.str("\r\n");
    }
}

void verb_describe(const Command&, Ctx& x) {
    TextSink& r = x.reply;

    const IEngine::ParamMask  pmask = x.engine.live_params();
    const IEngine::ConfigMask cmask = x.engine.live_configs();

    // Has this engine actually narrowed its liveness masks, or is it still on the "all live" default?
    // A host sweep must know: with the default, describe lists ids the engine never reads, so a
    // read-back mismatch is descriptor noise rather than a defect. Reported so the harness can skip
    // instead of emitting a wall of false failures.
    const bool masked = (pmask != ~IEngine::ParamMask{0})
                     || (cmask != static_cast<IEngine::ConfigMask>(~IEngine::ConfigMask{0}));

    r.str("descr engine=");
    r.str(infrasonic::engine_name());
    r.str(" version=");
    r.str(infrasonic::firmware_version());
    r.str(" masked=");
    r.append_i32(masked ? 1 : 0);
    r.str("\r\n");

    for (uint32_t i = 0; i < static_cast<uint32_t>(ParamId::Count); ++i) {
        if (!(pmask & (1u << i))) continue;
        ParamId id = static_cast<ParamId>(i);
        // Never advertise a param the platform keeps to itself - see param_is_platform_owned().
        if (param_is_platform_owned(id)) continue;
        r.str("param ");
        r.str(param_name(id));
        r.str(param_is_global(id) ? " global " : " deck ");
        float lo, hi; param_range(id, lo, hi);
        append_num(r, lo); r.str(".."); append_num(r, hi);
        r.str("\r\n");
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(ConfigId::Count); ++i) {
        if (!(cmask & (1u << i))) continue;
        ConfigId id = static_cast<ConfigId>(i);
        r.str("config ");
        r.str(config_name(id));
        r.str(" ");
        r.str(config_labels(id));
        r.str("\r\n");
    }

    // Platform-known query vocabulary (phase 1 lists the platform set; an engine's own queries are
    // reachable via handle_command but not enumerated here).
    // Query vocabulary - platform and engine halves emitted identically, so a host cannot tell them
    // apart and does not need to. ONLY `safe` entries appear: the generic sweep calls everything it can
    // see, so anything it must not call blindly is simply never named. See terminal-target-b.md.
    emit_queries(r, kPlatformQueries, PQ_COUNT);
    const EngineQueryTable et = x.engine.engine_queries();
    emit_queries(r, et.items, et.count);

    r.str("caps ");
    {
        Capabilities caps = x.engine.capabilities();
        if (x.engine.engine_queries().count > 0) caps |= CapTerminal;
        r.append_hex(caps);
    }
    r.str("\r\n");
    r.line("end");
}

struct Verb { const char* name; void (*fn)(const Command&, Ctx&); };
const Verb kVerbs[] = {
    { "set",      verb_set      }, { "get",   verb_get   }, { "query", verb_query    },
    { "config",   verb_config   }, { "cv",    verb_cv    }, { "gate",  verb_gate     },
    { "midi",     verb_midi     }, { "pad",   verb_pad   }, { "fx",    verb_fx       },
    { "mode",     verb_mode     }, { "caps",  verb_caps  }, { "help",  verb_help     },
    { "seq",      verb_seq      }, { "reset", verb_reset }, { "preset", verb_preset },
    { "describe", verb_describe },
};

}  // namespace

#if SPK_TERMINAL_OSC
EngineQueryTable platform_queries() { return { kPlatformQueries, PQ_COUNT }; }
const char*      value_kind_name(ValueKind k) { return kind_name(k); }
#endif

void dispatch_line(char* line, IEngine& engine, TextSink& reply, TermState& state) {
    Command cmd;
    if (!tokenize(line, cmd)) { reply.err("too-many-args"); return; }
    if (cmd.argc == 0) return;   // blank line: no command, no reply

    Ctx ctx{ engine, reply, state };
    for (const Verb& v : kVerbs) {
        if (!strcmp(cmd.verb(), v.name)) { v.fn(cmd, ctx); return; }
    }
    // Not a platform verb -> engine-specific (target B); unrecognized -> fixed error token.
    CommandView view{ cmd.argv, cmd.argc };
    if (!engine.handle_command(view, reply)) reply.err("unknown-verb");
}

}  // namespace spotykach

#endif  // SPK_TERMINAL
