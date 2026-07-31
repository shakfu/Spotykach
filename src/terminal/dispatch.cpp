// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/dispatch.h"

#if SPK_TERMINAL

#include "terminal/command.h"
#include "terminal/fmt.h"
#include "terminal/names.h"
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

void verb_fx(const Command& c, Ctx& x) {
    if (c.argc < 4) { x.reply.err("no-arg"); return; }
    DeckRef::Ref d;  if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
    bool on;         if (!parse_onoff(c.arg(3), on)) { x.reply.err("bad-arg"); return; }
    FxKind fk;
    if      (!strcmp(c.arg(1), "flux")) fk = FxKind::Flux;
    else if (!strcmp(c.arg(1), "grit")) fk = FxKind::Grit;
    else { x.reply.err("bad-arg"); return; }
    x.engine.set_fx(d, fk, on);
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
        x.reply.ok_i32(static_cast<int32_t>(x.engine.route()));
    } else if (!strcmp(s, "gateout")) {
        if (c.argc < 3) { x.reply.err("no-arg"); return; }
        DeckRef::Ref d; if (!parse_deck(c.arg(2), d)) { x.reply.err("bad-deck"); return; }
        x.reply.ok_i32(x.engine.gate_out_triggered(d) ? 1 : 0);
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
    x.reply.line("ok verbs: set get query config cv gate midi pad fx mode caps describe help");
}

void verb_describe(const Command&, Ctx& x) {
    TextSink& r = x.reply;

    r.str("descr engine=");
    r.str(infrasonic::engine_name());
    r.str(" version=");
    r.str(infrasonic::firmware_version());
    r.str("\r\n");

    const IEngine::ParamMask pmask = x.engine.live_params();
    for (uint32_t i = 0; i < static_cast<uint32_t>(ParamId::Count); ++i) {
        if (!(pmask & (1u << i))) continue;
        ParamId id = static_cast<ParamId>(i);
        r.str("param ");
        r.str(param_name(id));
        r.str(param_is_global(id) ? " global " : " deck ");
        float lo, hi; param_range(id, lo, hi);
        append_num(r, lo); r.str(".."); append_num(r, hi);
        r.str("\r\n");
    }

    const IEngine::ConfigMask cmask = x.engine.live_configs();
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
    r.line("query usb global");

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
