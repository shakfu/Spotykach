// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/dispatch.h"

#if SPK_TERMINAL

#include "terminal/command.h"
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

void verb_query(const Command& c, Ctx& x) {
    if (c.argc < 2) { x.reply.err("no-arg"); return; }
    const char* s = c.arg(1);
    if (!strcmp(s, "empty")) {
        if (c.argc < 3) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        x.reply.ok_i32(x.engine.audio_is_empty(d) ? 1 : 0);
    } else if (!strcmp(s, "mix")) {
        x.reply.ok_f32(x.engine.mix());
    } else if (!strcmp(s, "route")) {
        // Report the SELECTOR encoding `config route` accepts, not the raw Route enum - the two differ,
        // and describe publishes only the selector labels. Without this, `config route A 0` reads back
        // as 2 and a host cannot round-trip it.
        x.reply.ok_i32(route_to_selector(x.engine.route()));
    } else if (!strcmp(s, "gateout")) {
        if (c.argc < 3) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        x.reply.ok_i32(x.engine.gate_out_triggered(d) ? 1 : 0);
    } else if (!strcmp(s, "recorded")) {
        if (c.argc < 3) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        x.reply.ok_i32(static_cast<int32_t>(x.engine.audio_recorded_bytes(d)));
    } else if (!strcmp(s, "capacity")) {
        if (c.argc < 3) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        x.reply.ok_i32(static_cast<int32_t>(x.engine.audio_capacity_bytes(d)));
    } else if (!strcmp(s, "layout")) {
        if (c.argc < 3) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        x.reply.ok_i32(static_cast<int32_t>(x.engine.deck_layout(d)));
    } else if (!strcmp(s, "sizetempo")) {
        if (c.argc < 3) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        x.reply.ok_i32(x.engine.size_sets_tempo(d) ? 1 : 0);
    } else if (!strcmp(s, "fit")) {
        // Takes an ARGUMENT (the loop fraction), so it is deliberately NOT advertised in describe: the
        // descriptor has no way to say "this query needs an extra parameter", and the generic sweep
        // calls every advertised query with a deck alone. Reachable by name; see terminal-target-b.md.
        if (c.argc < 4) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        float f;        if (!parse_f32(c.arg(3), f))  { x.reply.err("bad-arg");  return; }
        x.reply.ok_f32(x.engine.tempo_to_fit(d, f));
    } else if (!strcmp(s, "reseed")) {
        // A LATCHING read: returns true once and self-clears, so asking changes the answer. Not
        // advertised - a generic sweep would silently consume the flag the platform is waiting for.
        // The textbook unsafe query; see the safe-to-call discussion in terminal-target-b.md.
        if (c.argc < 3) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        x.reply.ok_i32(x.engine.take_param_reseed(d) ? 1 : 0);
    } else if (!strcmp(s, "usb")) {
        // The USB bring-up snapshot (usb_diag.h). Reaching this at all means enumeration worked, so it
        // is a confirmation readout rather than a diagnosis - the diagnosis path is the TERM_USBDIAG
        // panel probe. Key=value so the host can parse it without positional assumptions.
        //
        // Refresh first: the live fields (core state, pad ownership) and the sticky host-activity bits
        // are otherwise frozen at what init() captured, which is before the host has enumerated - so
        // sof/rst would always read 0 in a build without TERM_USBDIAG driving the refresh from Loop().
        // We are on the main loop here, same as every other consumer, so this is safe.
        usb_diag_refresh(x.state.usb);
        const UsbDiag& u = x.state.usb;
        TextSink& r = x.reply;
        r.str("ok boot=");     r.append_i32(u.boot_version);
        r.str(" region=");     r.append_i32(u.memory_region);
        r.str(" clkcfg=");     r.append_i32(u.clocks_configured ? 1 : 0);
        r.str(" hsi48=");      r.append_i32(u.hsi48_ready ? 1 : 0);
        r.str(" usbsel=");     r.append_i32(u.usb_clk_source);
        r.str(" usb33den=");   r.append_i32(u.usb33_detector ? 1 : 0);
        r.str(" usb33rdy=");   r.append_i32(u.usb33_ready ? 1 : 0);
        r.str(" phy=");        r.append_i32(u.transceiver_on ? 1 : 0);
        r.str(" pullup=");     r.append_i32(u.pullup_asserted ? 1 : 0);
        r.str(" vbussense=");  r.append_i32(u.vbus_sensing ? 1 : 0);
        r.str(" dp=");         r.append_i32(u.dp_af_ok ? 1 : 0);
        r.str(" dm=");         r.append_i32(u.dm_af_ok ? 1 : 0);
        r.str(" rst=");        r.append_i32(u.usb_reset_seen ? 1 : 0);
        r.str(" sof=");        r.append_i32(u.sof_seen ? 1 : 0);
        r.str("\r\n");
    } else {
        // Unknown query name -> engine-specific (target B). handle_command returns true if recognized.
        CommandView view{ c.argv, c.argc };
        if (!x.engine.handle_command(view, x.reply)) x.reply.err("unknown-verb");
    }
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
    x.reply.ok_hex(x.engine.capabilities());
}

void verb_help(const Command&, Ctx& x) {
    x.reply.line("ok verbs: set get query config cv gate midi pad fx seq reset preset mode caps describe help");
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
    r.line("query empty deck");
    r.line("query mix global");
    r.line("query route global");
    r.line("query gateout deck");
    r.line("query recorded deck");
    r.line("query capacity deck");
    r.line("query layout deck");
    r.line("query sizetempo deck");
    r.line("query usb global");
    // NOT advertised, on purpose (both are reachable by name):
    //   `fit`    - takes an argument, and the descriptor cannot express arity, so the generic sweep
    //              (which calls each advertised query with a deck alone) would fail it.
    //   `reseed` - a latching read: asking changes the answer, so a sweep would consume the flag.
    // These are the two shapes the safe-to-call rule in terminal-target-b.md exists to keep out.

    r.str("caps ");
    r.append_hex(x.engine.capabilities());
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
