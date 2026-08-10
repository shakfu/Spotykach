// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/osc_addr.h"

#if SPK_TERMINAL_OSC

#include "terminal/dispatch.h"
#include "terminal/names.h"
#include "terminal/osc.h"
#include "version.h"

#include <cstring>

#pragma GCC optimize("Os")

namespace spotykach {

namespace {

// --- text plumbing ----------------------------------------------------------------------------------

// An ITextOut that fills a caller buffer, so the OSC side can reuse TextSink's float formatting rather
// than growing a second one. Numbers arriving as typed OSC arguments are formatted back to decimal here
// and re-parsed by dispatch; that round-trip is the deliberate cost of reusing layer [3] unchanged
// (~4 decimal digits, well inside every 0..1 param's audible resolution). See the spec's note on it.
struct BufOut : ITextOut {
    char*  p;
    size_t cap;
    size_t len = 0;
    BufOut(char* b, size_t c) : p(b), cap(c) { if (cap) p[0] = '\0'; }
    void write(const char* s, size_t n) override {
        for (size_t i = 0; i < n && len < cap - 1; ++i) p[len++] = s[i];
        if (cap) p[len] = '\0';
    }
};

void fmt_f32(float v, char* out, size_t cap) {
    BufOut b(out, cap);
    TextSink s(b);
    s.append_f32(v);
}

void fmt_i32(int32_t v, char* out, size_t cap) {
    BufOut b(out, cap);
    TextSink s(b);
    s.append_i32(v);
}

// Assembles a command line in the EXISTING grammar, token by token. Bounded; a line that does not fit
// simply never dispatches (and cannot, since every synthesized form is far shorter than the budget).
struct Line {
    char   buf[144];
    size_t len = 0;
    bool   over = false;

    void tok(const char* s) {
        const size_t n = std::strlen(s);
        if (over || len + n + 2 > sizeof buf) { over = true; return; }
        if (len) buf[len++] = ' ';
        std::memcpy(buf + len, s, n);
        len += n;
        buf[len] = '\0';
    }
};

// --- address splitting ------------------------------------------------------------------------------

// The deepest address in the space is `/sk/a/fx/lock/flux` (5). Six leaves room to detect "too deep"
// as a plain unknown-address rather than a silent truncation to a shallower, VALID address.
constexpr uint8_t kMaxSeg = 6;

struct Segments {
    const char* s[kMaxSeg];
    uint8_t     n = 0;
    // Segments are compared literally, all-lowercase, with no case folding and no pattern matching.
    bool is(uint8_t i, const char* lit) const { return i < n && !std::strcmp(s[i], lit); }
    const char* at(uint8_t i) const { return i < n ? s[i] : ""; }
};

// Split "/sk/a/param/speed" into {"sk","a","param","speed"} inside `scratch`. Returns false if the
// address is malformed or deeper than kMaxSeg.
bool split(const char* addr, char* scratch, size_t cap, Segments& out) {
    if (!addr || addr[0] != '/') return false;
    const size_t n = std::strlen(addr);
    if (n + 1 > cap) return false;
    std::memcpy(scratch, addr, n + 1);

    char* p = scratch;
    while (*p == '/') ++p;
    while (*p) {
        if (out.n >= kMaxSeg) return false;
        out.s[out.n++] = p;
        while (*p && *p != '/') ++p;
        if (*p) *p++ = '\0';
        while (*p == '/') ++p;   // tolerate a trailing or doubled slash
    }
    return out.n > 0;
}

// --- deck handling ----------------------------------------------------------------------------------

enum class Deck : uint8_t { A, B, Both, None };

Deck deck_of(const char* s) {
    if (!std::strcmp(s, "a"))  return Deck::A;
    if (!std::strcmp(s, "b"))  return Deck::B;
    if (!std::strcmp(s, "ab")) return Deck::Both;
    return Deck::None;
}

// --- platform reads ---------------------------------------------------------------------------------

// The four entries of the platform query table that the address space files under `/sk/dev/` rather
// than behind a `state/` kind segment: they report on the channel and the board, not on the engine's
// control surface (docs/dev/terminal-osc.md, "Platform").
//
// ONE predicate, consulted by both the resolver and `describe`. Those are the two places that know how
// an address is spelled, and while this list lived only inside do_dev() they disagreed: the descriptor
// advertised `/sk/state/cpu` for a read the resolver answers at `/sk/dev/cpu`. Both spellings resolve,
// so nothing broke - which is exactly why it went unnoticed until host/test_osc_addr.cpp compared the
// descriptor against the resolver address by address.
bool is_platform_read(const char* name) {
    return !std::strcmp(name, "cpu")    || !std::strcmp(name, "cpumin")
        || !std::strcmp(name, "cpumax") || !std::strcmp(name, "usb");
}

// --- one request ------------------------------------------------------------------------------------

struct Req {
    IEngine&          engine;
    OscSink&          sink;
    TermState&        state;
    const OscMessage& msg;

    void fail(const char* reason) { sink.err(reason); }

    // Run a synthesized line through the shared dispatcher. `reply` says whether a successful outcome
    // should produce an OSC message at all - false for the writes that must stay silent.
    void run(Line& l, bool reply) {
        if (l.over) { fail("bad-packet"); return; }
        sink.expect_reply(reply);
        dispatch_line(l.buf, engine, sink, state);
    }
};

// Reject an argument count the address cannot use. Returns true when the count is acceptable.
bool arity(Req& r, uint8_t want) {
    const uint8_t got = r.msg.argc();
    if (got == want) return true;
    // Extra TRAILING arguments are an error rather than ignored: silently dropping them hides a patch
    // wired to the wrong address.
    r.fail(got > want ? "too-many-args" : "no-arg");
    return false;
}

// --- params -----------------------------------------------------------------------------------------

// A param the engine does not implement is unknown-address, not a silent no-op: a layout generated
// from describe never sends one, and a hand-written layout finds out immediately.
bool param_is_live(IEngine& e, ParamId id) {
    return (e.live_params() & (1u << static_cast<uint32_t>(id))) != 0;
}

void do_param(Req& r, Deck deck, const char* slot, bool global) {
    ParamId id;
    if (!param_from_token(slot, id))          { r.fail("unknown-address"); return; }
    // The three platform-owned ids never reach set_param, so they are not addressable as params -
    // modspeed keeps its own deck-scoped address, which routes to set_mod_speed().
    if (param_is_platform_owned(id))          { r.fail("unknown-address"); return; }
    if (!param_is_live(r.engine, id))         { r.fail("unknown-address"); return; }
    // Deck-scope is a property of the ParamId, and the address space encodes it structurally: a global
    // param carries no deck segment, a deck-scoped one must carry one.
    if (param_is_global(id) != global)        { r.fail("unknown-address"); return; }

    const uint8_t argc = r.msg.argc();
    if (argc == 0) {
        // Arity, not a verb: no arguments is a READ of the address named.
        if (deck == Deck::Both) { r.fail("bad-arg"); return; }   // one request cannot have two answers
        Line l;
        l.tok("get"); l.tok("param"); l.tok(param_name(id));
        l.tok(deck == Deck::B ? "b" : "a");
        r.run(l, true);
        return;
    }
    if (argc > 1) { r.fail("too-many-args"); return; }

    float v;
    if (!r.msg.as_f32(0, v)) { r.fail("bad-arg"); return; }
    char val[24];
    fmt_f32(v, val, sizeof val);

    // `ab` fans out to two dispatches, decks a then b. Bounded at two, and it covers the only fan-out
    // anyone actually asks for - which is why wildcards proper are excluded.
    const bool both = (deck == Deck::Both);
    for (int i = 0; i < (both ? 2 : 1); ++i) {
        Line l;
        l.tok("set"); l.tok("param"); l.tok(param_name(id));
        l.tok(both ? (i == 0 ? "a" : "b") : (deck == Deck::B ? "b" : "a"));
        l.tok(val);
        r.run(l, false);
        if (r.sink.errored()) return;   // report the first failure rather than both
    }
}

// --- configs ----------------------------------------------------------------------------------------

void do_config(Req& r, Deck deck, const char* name, bool global) {
    ConfigId id;
    if (!config_from_token(name, id)) { r.fail("unknown-address"); return; }
    if (config_is_global(id) != global) { r.fail("unknown-address"); return; }
    if (!(r.engine.live_configs() & (1u << static_cast<uint32_t>(id)))) {
        r.fail("unknown-address"); return;
    }
    // set_config is write-only on IEngine, so there is no read form to overload the no-argument case
    // against; a bare cfg address is a mistake, not a query.
    if (!arity(r, 1)) return;

    int32_t v;
    if (!r.msg.as_i32(0, v)) { r.fail("bad-arg"); return; }
    char val[16];
    fmt_i32(v, val, sizeof val);

    const bool both = (deck == Deck::Both);
    for (int i = 0; i < (both ? 2 : 1); ++i) {
        Line l;
        l.tok("config"); l.tok(config_name(id));
        l.tok(both ? (i == 0 ? "a" : "b") : (deck == Deck::B ? "b" : "a"));
        l.tok(val);
        r.run(l, false);
        if (r.sink.errored()) return;
    }
}

// --- stimulus verbs ----------------------------------------------------------------------------------

const char* deck_tok(Deck d) { return d == Deck::B ? "b" : "a"; }

// A trigger with a boolean argument: `,F` (or a zero float, which is what a TouchOSC button sends in
// some configurations) suppresses it rather than firing it. Returns false if the trigger is suppressed
// or the argument is unusable.
bool trigger_fires(Req& r, bool& ok_arg) {
    ok_arg = true;
    if (r.msg.argc() == 0) return true;                 // absent argument = true (bare trigger)
    if (r.msg.argc() > 1)  { r.fail("too-many-args"); ok_arg = false; return false; }
    bool on;
    if (!r.msg.as_bool(0, on)) { r.fail("bad-arg"); ok_arg = false; return false; }
    return on;
}

// Returns true if the segment was recognized as a stimulus verb (whether or not it succeeded).
bool do_stimulus(Req& r, Deck deck, const Segments& g, uint8_t k) {
    const char* kind = g.at(k);

    if (!std::strcmp(kind, "cv")) {
        const char* ch = g.at(k + 1);
        if (std::strcmp(ch, "voct") && std::strcmp(ch, "mix") &&
            std::strcmp(ch, "size") && std::strcmp(ch, "xfade")) { r.fail("unknown-address"); return true; }
        if (!arity(r, 1)) return true;
        float v;
        if (!r.msg.as_f32(0, v)) { r.fail("bad-arg"); return true; }
        char val[24]; fmt_f32(v, val, sizeof val);
        Line l; l.tok("cv"); l.tok(ch); l.tok(deck_tok(deck)); l.tok(val);
        r.run(l, false);
        return true;
    }

    if (!std::strcmp(kind, "gate")) {
        bool usable; if (!trigger_fires(r, usable)) return true;
        Line l; l.tok("gate"); l.tok(deck_tok(deck));
        r.run(l, false);
        return true;
    }

    if (!std::strcmp(kind, "pad")) {
        const char* what = g.at(k + 1);
        const bool playrec = !std::strcmp(what, "play") || !std::strcmp(what, "rec");
        if (!playrec && std::strcmp(what, "stop") && std::strcmp(what, "clear")) {
            r.fail("unknown-address"); return true;
        }
        Line l; l.tok("pad"); l.tok(what); l.tok(deck_tok(deck));
        if (playrec) {
            // The boolean here is REVERSE, not a trigger gate - a pad press with ,F is still a press.
            if (r.msg.argc() > 1) { r.fail("too-many-args"); return true; }
            if (r.msg.argc() == 1) {
                bool rev;
                if (!r.msg.as_bool(0, rev)) { r.fail("bad-arg"); return true; }
                if (rev) l.tok("rev");
            }
        } else if (!arity(r, 0)) {
            return true;
        }
        // `pad play` answers with the deck's emptiness - an action that returns a value, which the line
        // codec reports and a host has no other way to learn from the press itself.
        r.run(l, !std::strcmp(what, "play"));
        return true;
    }

    if (!std::strcmp(kind, "seq")) {
        const char* what = g.at(k + 1);
        if (std::strcmp(what, "trig") && std::strcmp(what, "arm") &&
            std::strcmp(what, "clear") && std::strcmp(what, "disarm")) {
            r.fail("unknown-address"); return true;
        }
        bool usable; if (!trigger_fires(r, usable)) return true;
        Line l; l.tok("seq"); l.tok(what); l.tok(deck_tok(deck));
        r.run(l, false);
        return true;
    }

    if (!std::strcmp(kind, "fx")) {
        const char* what = g.at(k + 1);
        if (!std::strcmp(what, "lock")) {
            const char* fx = g.at(k + 2);
            if (std::strcmp(fx, "flux") && std::strcmp(fx, "grit")) { r.fail("unknown-address"); return true; }
            bool usable; if (!trigger_fires(r, usable)) return true;
            Line l; l.tok("fx"); l.tok("lock"); l.tok(fx); l.tok(deck_tok(deck));
            r.run(l, false);
            return true;
        }
        if (!std::strcmp(what, "gritmode")) {
            bool usable; if (!trigger_fires(r, usable)) return true;
            Line l; l.tok("fx"); l.tok("gritmode"); l.tok(deck_tok(deck));
            // Like `pad play`: the platform uses the returned reseed pair to re-pick its MValues after
            // the switch, so a host driving the same gesture needs them too.
            r.run(l, true);
            return true;
        }
        if (std::strcmp(what, "flux") && std::strcmp(what, "grit")) { r.fail("unknown-address"); return true; }
        if (!arity(r, 1)) return true;
        bool on;
        if (!r.msg.as_bool(0, on)) { r.fail("bad-arg"); return true; }
        Line l; l.tok("fx"); l.tok(what); l.tok(deck_tok(deck)); l.tok(on ? "on" : "off");
        r.run(l, false);
        return true;
    }

    if (!std::strcmp(kind, "modspeed")) {
        if (r.msg.argc() < 1) { r.fail("no-arg"); return true; }
        if (r.msg.argc() > 2) { r.fail("too-many-args"); return true; }
        float v;
        if (!r.msg.as_f32(0, v)) { r.fail("bad-arg"); return true; }
        bool sync = false;
        if (r.msg.argc() == 2 && !r.msg.as_bool(1, sync)) { r.fail("bad-arg"); return true; }
        char val[24]; fmt_f32(v, val, sizeof val);
        Line l; l.tok("set"); l.tok("modspeed"); l.tok(deck_tok(deck)); l.tok(val);
        if (sync) l.tok("sync");
        r.run(l, false);
        return true;
    }

    return false;
}

// --- describe ----------------------------------------------------------------------------------------

// Compose "/sk/<deck>/<kind>/<name>", or "/sk/<kind>/<name>" when `deck` is null.
void compose(char* out, size_t cap, const char* deck, const char* kind, const char* name) {
    BufOut b(out, cap);
    b.write("/sk/", 4);
    if (deck) { b.write(deck, std::strlen(deck)); b.write("/", 1); }
    b.write(kind, std::strlen(kind));
    b.write("/", 1);
    b.write(name, std::strlen(name));
}

void describe_param_row(OscBundleWriter& bw, IEngine& e, ParamId id) {
    const char* name  = param_name(id);
    // A label that is absent falls back to the layer-2 name, so an engine that implements none produces
    // a descriptor identical to the generic tier - degraded, never broken.
    const char* label = e.param_label(id);
    if (!label || !*label) label = name;
    float lo, hi; param_range(id, lo, hi);

    // Deck expansion happens HERE, on the device, where the scope table lives - so a host consuming the
    // descriptor never has to know decks exist.
    const bool global = param_is_global(id);
    const char* decks[2] = { global ? nullptr : "a", global ? nullptr : "b" };
    for (int i = 0; i < (global ? 1 : 2); ++i) {
        char addr[64];
        compose(addr, sizeof addr, decks[i], "param", name);
        OscWriter w = bw.element();
        w.begin("/sk/reply/dev/describe/param", "ssffs");
        w.str(addr); w.str(label); w.f32(lo); w.f32(hi); w.str(global ? "global" : "deck");
        bw.close_element(w);
    }
}

// `platform` says whether this is the platform's own table. It gates the `/sk/dev/` spelling rather
// than the name alone doing it: an engine that happened to declare a query called `cpu` owns a
// different thing entirely, and advertising it under `/sk/dev/` would point a host at the platform's
// read instead of at the engine's.
void describe_state_rows(OscBundleWriter& bw, const EngineQuery* q, uint8_t n, bool platform) {
    for (uint8_t i = 0; i < n; ++i) {
        if (!q[i].safe) continue;   // the sweep can only see what is safe to call, by construction
        // Advertised where the resolver answers it - see is_platform_read().
        const bool dev  = platform && is_platform_read(q[i].name);
        const bool deck = !dev && (q[i].scope == QueryScope::Deck);
        const char* decks[2] = { deck ? "a" : nullptr, deck ? "b" : nullptr };
        for (int k = 0; k < (deck ? 2 : 1); ++k) {
            char addr[64];
            compose(addr, sizeof addr, decks[k], dev ? "dev" : "state", q[i].name);
            OscWriter w = bw.element();
            // Four strings, not three: the selector labels an Enum query declares have to travel, or a
            // host cannot check that a reply is one of the declared values - and the line codec's
            // describe DOES send them (`query route global enum 0:stereo 1:dmono 2:genstereo`).
            // Dropping them here made the two codecs describe the same device differently, which is
            // precisely the parity break this format exists to avoid. Empty for non-enum kinds.
            w.begin("/sk/reply/dev/describe/state", "ssss");
            w.str(addr); w.str(q[i].name); w.str(value_kind_name(q[i].kind));
            w.str(q[i].labels ? q[i].labels : "");
            bw.close_element(w);
        }
    }
}

// The descriptor is sent as ONE bundle so a host receives it atomically, which is what forces both this
// buffer and the TX FIFO to hold a whole one (an OSC bundle cannot be streamed the way lines can).
//
// SIZING is in osc_addr.h (kOscBundleCap) and is MEASURED rather than estimated: a row costs ~85 bytes,
// and an engine on the DEFAULT all-live masks advertises 38 param rows (17 deck-scoped x 2 decks + 4
// global), 18 state rows and 6 config rows - 5532 bytes, not the ~2-3 KB the spec projected from a
// masked engine. The unmasked default has to fit, or `describe` fails on exactly the engines that have
// not narrowed their masks yet. Overflow is an explicit error, never a truncated bundle.
uint8_t g_bundle[kOscBundleCap];

void do_describe(Req& r) {
    OscBundleWriter bw(g_bundle, kOscBundleCap);

    const IEngine::ParamMask  pmask = r.engine.live_params();
    const IEngine::ConfigMask cmask = r.engine.live_configs();
    const bool masked = (pmask != ~IEngine::ParamMask{0})
                     || (cmask != static_cast<IEngine::ConfigMask>(~IEngine::ConfigMask{0}));

    {
        OscWriter w = bw.element();
        w.begin("/sk/reply/dev/describe", "sss");
        w.str(infrasonic::engine_name());
        w.str(infrasonic::firmware_version());
        w.str(masked ? "masked=1" : "masked=0");
        bw.close_element(w);
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(ParamId::Count); ++i) {
        if (!(pmask & (1u << i))) continue;
        const ParamId id = static_cast<ParamId>(i);
        if (param_is_platform_owned(id)) continue;
        describe_param_row(bw, r.engine, id);
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(ConfigId::Count); ++i) {
        if (!(cmask & (1u << i))) continue;
        const ConfigId id = static_cast<ConfigId>(i);
        char addr[64];
        compose(addr, sizeof addr, config_is_global(id) ? nullptr : "a", "cfg", config_name(id));
        OscWriter w = bw.element();
        w.begin("/sk/reply/dev/describe/cfg", "sss");
        w.str(addr); w.str(config_name(id)); w.str(config_labels(id));
        bw.close_element(w);
    }

    const EngineQueryTable pq = platform_queries();
    describe_state_rows(bw, pq.items, pq.count, true);
    const EngineQueryTable eq = r.engine.engine_queries();
    describe_state_rows(bw, eq.items, eq.count, false);

    {
        Capabilities caps = r.engine.capabilities();
        if (r.engine.engine_queries().count > 0) caps |= CapTerminal;
        OscWriter w = bw.element();
        w.begin("/sk/reply/dev/describe/caps", "i");
        w.i32(static_cast<int32_t>(caps));
        bw.close_element(w);
    }

    if (!bw.ok()) { r.fail("overflow"); return; }
    r.sink.send_packet(bw.data(), bw.size());
    r.sink.expect_reply(false);   // the bundle IS the reply; finish() must not add a second message
}

// --- platform (/sk/dev/...) ----------------------------------------------------------------------------

void do_dev(Req& r, const Segments& g) {
    const char* what = g.at(2);

    if (!std::strcmp(what, "describe")) {
        if (!arity(r, 0)) return;
        do_describe(r);
        return;
    }

    if (!std::strcmp(what, "mode")) {
        const char* m = g.at(3);
        if (!std::strcmp(m, "ack")) {
            // OSC-only: opt into an ack per successful write for this session. The pytest harness turns
            // it on; a rig streaming faders leaves it off.
            bool on;
            if (r.msg.argc() == 0) on = true;
            else if (r.msg.argc() > 1) { r.fail("too-many-args"); return; }
            else if (!r.msg.as_bool(0, on)) { r.fail("bad-arg"); return; }
            r.state.osc_ack = on;
            // Deliberately no reply, not even under ack: the message that TURNS acking off would
            // otherwise be acknowledged by the setting it just cleared, which reads as a stuck flag.
            r.sink.expect_reply(false);
            return;
        }
        if (std::strcmp(m, "test") && std::strcmp(m, "run")) { r.fail("unknown-address"); return; }
        bool usable; if (!trigger_fires(r, usable)) return;
        Line l; l.tok("mode"); l.tok(m);
        r.run(l, false);
        return;
    }

    if (!std::strcmp(what, "caps") || !std::strcmp(what, "help")) {
        if (!arity(r, 0)) return;
        Line l; l.tok(what);
        r.run(l, true);
        return;
    }

    if (is_platform_read(what)) {
        if (!arity(r, 0)) return;
        Line l; l.tok("query"); l.tok(what);
        r.run(l, true);
        return;
    }

    if (!std::strcmp(what, "reset")) {
        Line l; l.tok("reset");
        if (!std::strcmp(g.at(3), "cpu")) {
            if (!arity(r, 0)) return;
            l.tok("cpu");
            r.run(l, false);
            return;
        }
        if (g.n > 3) { r.fail("unknown-address"); return; }
        // The optional deck arrives as a STRING here, the one place the codec takes one: it is a
        // qualifier on a platform composite, not a value, and `reset` with no deck means both.
        if (r.msg.argc() > 1) { r.fail("too-many-args"); return; }
        if (r.msg.argc() == 1) {
            const char* d = r.msg.as_str(0);
            if (!d) { r.fail("bad-arg"); return; }
            l.tok(d);
        }
        r.run(l, true);   // replies with the number of params written, so a harness can assert it acted
        return;
    }

    if (!std::strcmp(what, "preset")) {
        const char* action = g.at(3);
        if (std::strcmp(action, "save") && std::strcmp(action, "load")) { r.fail("unknown-address"); return; }
        if (!arity(r, 1)) return;
        int32_t slot;
        if (!r.msg.as_i32(0, slot)) { r.fail("bad-arg"); return; }
        char val[16]; fmt_i32(slot, val, sizeof val);
        Line l; l.tok("preset"); l.tok(action); l.tok(val);
        r.run(l, true);
        return;
    }

    r.fail("unknown-address");
}

// --- one message ---------------------------------------------------------------------------------------

void dispatch_message(const OscMessage& msg, IEngine& engine, OscSink& sink, TermState& state) {
    sink.begin_request(msg.address(), state.osc_ack);

    Req r{ engine, sink, state, msg };

    char     scratch[128];
    Segments g;
    if (!split(msg.address(), scratch, sizeof scratch, g) || !g.is(0, "sk")) {
        r.fail("unknown-address");
        sink.finish();
        return;
    }

    if (g.is(1, "dev")) {
        do_dev(r, g);
        sink.finish();
        return;
    }

    const Deck deck = deck_of(g.at(1));
    if (deck != Deck::None) {
        // Deck-scoped. The first segment after /sk is a deck, a kind, or `dev` - disjoint sets, so the
        // parse is unambiguous with no lookahead.
        const char* kind = g.at(2);
        if (!std::strcmp(kind, "param")) {
            if (g.n != 4) r.fail("unknown-address");
            else          do_param(r, deck, g.at(3), false);
        } else if (!std::strcmp(kind, "cfg")) {
            if (g.n != 4) r.fail("unknown-address");
            else          do_config(r, deck, g.at(3), false);
        } else if (!std::strcmp(kind, "state")) {
            if (g.n != 4)            { r.fail("unknown-address"); }
            else if (deck == Deck::Both) { r.fail("bad-arg"); }   // one request, one answer
            else if (!arity(r, 0))   { /* reported */ }
            else {
                Line l; l.tok("query"); l.tok(g.at(3)); l.tok(deck_tok(deck));
                r.run(l, true);
            }
        } else if (!do_stimulus(r, deck, g, 2)) {
            r.fail("unknown-address");
        }
        sink.finish();
        return;
    }

    // Global: no deck segment at all, because deck-scope is a property of the id and the address space
    // encodes that structurally rather than making a host send a token the device discards.
    if (g.is(1, "param")) {
        if (g.n != 3) r.fail("unknown-address");
        else          do_param(r, Deck::A, g.at(2), true);
    } else if (g.is(1, "cfg")) {
        if (g.n != 3) r.fail("unknown-address");
        else          do_config(r, Deck::A, g.at(2), true);
    } else if (g.is(1, "state")) {
        if (g.n != 3)          { r.fail("unknown-address"); }
        else if (!arity(r, 0)) { /* reported */ }
        else {
            Line l; l.tok("query"); l.tok(g.at(2));
            r.run(l, true);
        }
    } else if (g.is(1, "midi")) {
        const char* what = g.at(2);
        if (!std::strcmp(what, "note")) {
            if (!arity(r, 2)) { sink.finish(); return; }
            int32_t ch, note;
            if (!msg.as_i32(0, ch) || !msg.as_i32(1, note)) { r.fail("bad-arg"); sink.finish(); return; }
            char a[16], b[16]; fmt_i32(ch, a, sizeof a); fmt_i32(note, b, sizeof b);
            Line l; l.tok("midi"); l.tok("note"); l.tok(a); l.tok(b);
            r.run(l, false);
        } else if (!std::strcmp(what, "msg")) {
            if (!arity(r, 3)) { sink.finish(); return; }
            int32_t st, d1, d2;
            if (!msg.as_i32(0, st) || !msg.as_i32(1, d1) || !msg.as_i32(2, d2)) {
                r.fail("bad-arg"); sink.finish(); return;
            }
            char a[16], b[16], c[16];
            fmt_i32(st, a, sizeof a); fmt_i32(d1, b, sizeof b); fmt_i32(d2, c, sizeof c);
            Line l; l.tok("midi"); l.tok("msg"); l.tok(a); l.tok(b); l.tok(c);
            r.run(l, false);
        } else if (!std::strcmp(what, "transport")) {
            bool start;
            if (msg.argc() == 0) start = true;
            else if (msg.argc() > 1) { r.fail("too-many-args"); sink.finish(); return; }
            else if (!msg.as_bool(0, start)) { r.fail("bad-arg"); sink.finish(); return; }
            Line l; l.tok("midi"); l.tok("transport"); l.tok(start ? "start" : "stop");
            r.run(l, false);
        } else {
            r.fail("unknown-address");
        }
    } else {
        r.fail("unknown-address");
    }
    sink.finish();
}

}  // namespace

namespace {

// Nesting is legal OSC but has no use in this address space, and recursion on attacker-shaped input is
// the one place this codec could hurt the device rather than just answer badly: each level costs an
// OscMessage plus the segment scratch, and a 512 B packet can encode ~25 levels of empty bundle - low
// thousands of bytes of stack, on a main loop that also runs the UI. Two levels covers "a bundle a host
// nested by accident"; deeper is bad-packet.
constexpr uint8_t kMaxBundleDepth = 2;

void dispatch_packet(const uint8_t* p, size_t n, IEngine& engine, OscSink& sink, TermState& state,
                     uint8_t depth) {
    if (osc_is_bundle(p, n)) {
        if (depth >= kMaxBundleDepth) { sink.emit_error("/sk", "bad-packet"); return; }
        // Bundle contents dispatch immediately and in order; the timetag is ignored, not scheduled.
        size_t cursor = 0;
        const uint8_t* elem; size_t elem_len;
        bool any = false;
        while (osc_bundle_next(p, n, cursor, elem, elem_len)) {
            any = true;
            dispatch_packet(elem, elem_len, engine, sink, state, static_cast<uint8_t>(depth + 1));
        }
        if (!any) sink.emit_error("/sk", "bad-packet");
        return;
    }

    OscMessage msg;
    if (!msg.parse(p, n)) { sink.emit_error("/sk", "bad-packet"); return; }
    dispatch_message(msg, engine, sink, state);
}

}  // namespace

void osc_dispatch_packet(const uint8_t* p, size_t n, IEngine& engine, OscSink& sink, TermState& state) {
    dispatch_packet(p, n, engine, sink, state, 0);
}

}  // namespace spotykach

#endif  // SPK_TERMINAL_OSC
