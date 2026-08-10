// Off-target tests for the SPK_TERMINAL_OSC codec (docs/dev/terminal-osc.md).
//
// The OSC codec replaces layer [2] only, so everything it does is hardware-free and testable here:
// SLIP framing, the OSC wire format, address -> verb synthesis, the typed reply sink, and the describe
// bundle. Only src/terminal/terminal.cpp (which owns daisy::UsbHandle) is excluded.
//
//   1. SLIP: escape round-trip, leading/trailing END, overflow RESYNC (not truncation).
//   2. OSC wire: big-endian scalars, padding, the no-typetag read form, rejected tags.
//   3. Type coercion: the spec's permissive table, and what it still refuses.
//   4. Addresses: every family, against the exact IEngine call it must produce.
//   5. Reads: arity not a verb, and the reply's TYPE TAG (,i / ,f / ,s) per kind.
//   6. Writes: silent by default; /sk/dev/mode/ack turns acks on.
//   7. Errors: the fixed token set, echoed against the request address.
//   8. describe: one bundle, addresses composed as the spec predicts, labels, and its real SIZE.
//
// The parity claim the spec makes its acceptance criterion - same sweep, both codecs, identical
// results - needs a device and so cannot be checked here; what IS checked here is the structural half
// (every address `describe` advertises is composed exactly as the rules predict, and is readable as
// composed). The end-to-end half is `make test-hw` vs `make test-hw CODEC=osc`, running tools/'s suites
// against a line build and an OSC build of the same engine.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/iengine.h"
#include "terminal/dispatch.h"
#include "terminal/names.h"
#include "terminal/osc.h"
#include "terminal/osc_addr.h"
#include "terminal/osc_sink.h"
#include "terminal/slip.h"
#include "terminal/term_state.h"
#include "osc_test_util.h"
#include "terminal/tx_fifo.h"

using namespace spotykach;
using osctest::StringOut;
using osctest::Reply;
using osctest::slip_frames;
using osctest::decode_replies;
using osctest::decode_rows;

namespace {

int g_failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); g_failures++; }
}
void check_eq(const std::string& got, const char* want, const char* msg) {
    if (got != want) {
        std::printf("  FAIL: %s\n        want %s\n        got  %s\n", msg, want, got.c_str());
        g_failures++;
    }
}

// --- test doubles ------------------------------------------------------------------------------------

// An ITextOut with a real capacity, for the one behaviour that needs one: a describe bundle that will
// not fit must be refused whole rather than written as a corrupt half-frame.
struct BoundedOut : ITextOut {
    std::string s;
    size_t cap;
    explicit BoundedOut(size_t c) : cap(c) {}
    void write(const char* p, size_t n) override { s.append(p, n); }
    size_t writable() const override { return s.size() >= cap ? 0 : cap - s.size(); }
};

struct MockEngine : IEngine {
    void init(const EngineContext&) override {}
    void prepare() override {}
    void process(const float* const*, float**, size_t) override {}

    std::vector<std::string> log;
    void rec(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[160];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        log.emplace_back(buf);
    }
    std::string last() const { return log.empty() ? std::string("<none>") : log.back(); }
    void clear() { log.clear(); }

    float params[static_cast<size_t>(ParamId::Count)][2] = {};
    void set_param(ParamId id, DeckRef::Ref d, float v) override {
        params[static_cast<size_t>(id)][d == DeckRef::B ? 1 : 0] = v;
        rec("set_param %s %d %.4f", param_name(id), int(d), v);
    }
    float param(ParamId id, DeckRef::Ref d) const override {
        return params[static_cast<size_t>(id)][d == DeckRef::B ? 1 : 0];
    }
    float param_default(ParamId) const override { return 0.25f; }
    void set_mod_speed(DeckRef::Ref d, float v, bool s) override {
        rec("set_mod_speed %d %.4f %d", int(d), v, s ? 1 : 0);
    }
    bool set_config(ConfigId id, DeckRef::Ref d, int v) override {
        rec("set_config %s %d %d", config_name(id), int(d), v);
        return true;
    }
    void cv_voct(DeckRef::Ref d, float v) override { rec("cv_voct %d %.4f", int(d), v); }
    void cv_mix(DeckRef::Ref d, float v) override { rec("cv_mix %d %.4f", int(d), v); }
    void cv_size_pos(DeckRef::Ref d, float v) override { rec("cv_size_pos %d %.4f", int(d), v); }
    void cv_crossfade(float v) override { rec("cv_crossfade %.4f", v); }
    void on_gate_trigger(DeckRef::Ref d) override { rec("on_gate_trigger %d", int(d)); }
    bool gate_out_triggered(DeckRef::Ref d) override { return d == DeckRef::A; }
    DeckRef::Ref handle_midi_note(uint8_t c, uint8_t n) override {
        rec("midi_note %u %u", c, n); return DeckRef::A;
    }
    void handle_midi_message(uint8_t s, uint8_t a, uint8_t b) override { rec("midi_msg %u %u %u", s, a, b); }
    void handle_midi_transport(bool s) override { rec("midi_transport %d", s ? 1 : 0); }
    bool play_pad_empty = true;
    bool on_play_pad(DeckRef::Ref d, bool r) override {
        rec("on_play_pad %d %d", int(d), r ? 1 : 0); return play_pad_empty;
    }
    void on_record_pad(DeckRef::Ref d, bool r) override { rec("on_record_pad %d %d", int(d), r ? 1 : 0); }
    void on_seq_trigger(DeckRef::Ref d) override { rec("on_seq_trigger %d", int(d)); }
    void on_seq_toggle_arm(DeckRef::Ref d) override { rec("on_seq_toggle_arm %d", int(d)); }
    void clear_sequence(DeckRef::Ref d) override { rec("clear_sequence %d", int(d)); }
    void disarm_track(DeckRef::Ref d) override { rec("disarm_track %d", int(d)); }
    void stop_if_generating(DeckRef::Ref d) override { rec("stop_if_generating %d", int(d)); }
    void clear_buffer(DeckRef::Ref d) override { rec("clear_buffer %d", int(d)); }
    void set_fx(DeckRef::Ref d, FxKind k, bool on) override {
        rec("set_fx %d %s %d", int(d), k == FxKind::Flux ? "flux" : "grit", on ? 1 : 0);
    }
    void toggle_fx_lock(DeckRef::Ref d, FxKind k) override {
        rec("toggle_fx_lock %d %s", int(d), k == FxKind::Flux ? "flux" : "grit");
    }
    GritReseed toggle_grit_mode(DeckRef::Ref d) override { rec("toggle_grit_mode %d", int(d)); return { 0.25f, 0.75f }; }
    size_t audio_recorded_bytes(DeckRef::Ref) override { return 4096; }
    size_t audio_capacity_bytes(DeckRef::Ref) override { return 65536; }
    DeckLayout deck_layout(DeckRef::Ref) override { return DeckLayout::slice; }
    bool size_sets_tempo(DeckRef::Ref) override { return true; }
    bool audio_is_empty(DeckRef::Ref d) override { return d != DeckRef::A; }
    float mix() const override { return 0.25f; }
    Route route() const override { return Route::GenerativeStereo; }
    Capabilities capabilities() const override { return 0x133u; }

    // Layer-3 labels, the one new engine-owned virtual: two slots named the way an engine would.
    bool labels = false;
    const char* param_label(ParamId id) const override {
        if (!labels) return nullptr;
        if (id == ParamId::Speed) return "station";
        if (id == ParamId::Size)  return "character";
        return nullptr;
    }

    ParamMask  pmask = ~ParamMask{0};
    ConfigMask cmask = static_cast<ConfigMask>(~ConfigMask{0});
    ParamMask  live_params() const override { return pmask; }
    ConfigMask live_configs() const override { return cmask; }
};

// --- OSC packet construction (the host half of the wire) -----------------------------------------------

void pad4(std::string& s) { while (s.size() & 3u) s.push_back('\0'); }

void put_be32(std::string& s, uint32_t v) {
    s.push_back(char(v >> 24)); s.push_back(char(v >> 16));
    s.push_back(char(v >> 8));  s.push_back(char(v));
}

struct Msg {
    std::string bytes;
    // `tags` without the leading comma. An EMPTY tags string still emits ",\0\0\0"; pass nullptr for
    // the no-typetag form, which is legal OSC 1.0 and is what a bare read looks like from some hosts.
    explicit Msg(const char* addr, const char* tags = "") {
        bytes = addr; bytes.push_back('\0'); pad4(bytes);
        if (tags) { bytes += ","; bytes += tags; bytes.push_back('\0'); pad4(bytes); }
    }
    Msg& f(float v) {
        uint32_t b; std::memcpy(&b, &v, 4); put_be32(bytes, b); return *this;
    }
    Msg& i(int32_t v) { put_be32(bytes, uint32_t(v)); return *this; }
    Msg& d(double v) {
        uint64_t b; std::memcpy(&b, &v, 8);
        put_be32(bytes, uint32_t(b >> 32)); put_be32(bytes, uint32_t(b));
        return *this;
    }
    Msg& s(const char* v) { bytes += v; bytes.push_back('\0'); pad4(bytes); return *this; }
    const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(bytes.data()); }
    size_t size() const { return bytes.size(); }
};

// Send one OSC message and return the single reply it produced ("" when the write was silent).
std::string send(MockEngine& e, TermState& st, const Msg& m) {
    StringOut out;
    OscSink   sink(out);
    osc_dispatch_packet(m.data(), m.size(), e, sink, st);
    auto r = decode_replies(out.s);
    if (r.empty()) return "";
    if (r.size() > 1) return "<multiple>";
    return r[0].all();
}

std::string send(MockEngine& e, const Msg& m) {
    TermState st;
    return send(e, st, m);
}

// --- 1. SLIP -------------------------------------------------------------------------------------------

void test_slip() {
    std::printf("slip\n");
    {
        // Every byte that needs escaping, plus the two that trigger it, round-trips exactly.
        const uint8_t src[] = { 0x01, slip::kEnd, 0x02, slip::kEsc, 0x03, slip::kEnd, slip::kEsc };
        std::string wire;
        slip_encode(src, sizeof src, [&](const char* p, size_t n) { wire.append(p, n); });
        check(wire.front() == char(slip::kEnd), "frame opens with END (flushes line noise)");
        check(wire.back()  == char(slip::kEnd), "frame closes with END");
        check(wire.size() == slip_encoded_size(src, sizeof src), "slip_encoded_size predicts the frame");

        SlipAssembler a;
        int ready = 0;
        for (unsigned char b : wire) if (a.feed(b) == SlipAssembler::Feed::Ready) ready++;
        check(ready == 1, "exactly one packet completes");
        check(a.len() == sizeof src && std::memcmp(a.packet(), src, sizeof src) == 0,
              "escaped bytes survive the round trip");
    }
    {
        // The behaviour that matters: an over-long packet RESYNCHRONIZES rather than truncating, so the
        // next packet is clean. A truncated OSC packet is undetectable garbage.
        SlipAssembler a;
        int overflow = 0, ready = 0;
        const std::string flood(SlipAssembler::kMax + 64, 'x');
        for (unsigned char b : flood) if (a.feed(b) == SlipAssembler::Feed::Overflow) overflow++;
        check(overflow == 0, "overflow is reported at the frame boundary, not mid-flood");
        if (a.feed(slip::kEnd) == SlipAssembler::Feed::Overflow) overflow++;
        check(overflow == 1, "the over-long packet is reported exactly once");
        a.reset();

        Msg m("/sk/dev/caps");
        std::string wire;
        slip_encode(m.data(), m.size(), [&](const char* p, size_t n) { wire.append(p, n); });
        for (unsigned char b : wire) if (a.feed(b) == SlipAssembler::Feed::Ready) ready++;
        check(ready == 1, "the packet after an overflow parses cleanly");
    }
    {
        SlipAssembler a;
        check(a.feed(slip::kEnd) == SlipAssembler::Feed::Pending, "a leading END yields no empty packet");
        check(a.feed(slip::kEnd) == SlipAssembler::Feed::Pending, "back-to-back ENDs are legal padding");
    }
}

// --- 2/3. OSC wire format and coercion --------------------------------------------------------------

void test_wire() {
    std::printf("osc wire format\n");
    {
        Msg m("/sk/a/param/speed", "f");
        m.f(0.5f);
        OscMessage p;
        check(p.parse(m.data(), m.size()), "a float message parses");
        check_eq(p.address(), "/sk/a/param/speed", "address");
        check(p.argc() == 1, "one argument");
        float f = 0;
        check(p.as_f32(0, f) && f == 0.5f, "big-endian float survives");
    }
    {
        // The read form: no type-tag string at all. Legal OSC 1.0, and the whole basis of "arity, not
        // a verb" - so it must parse as zero arguments rather than be rejected as malformed.
        Msg m("/sk/a/param/speed", nullptr);
        OscMessage p;
        check(p.parse(m.data(), m.size()), "a message with NO typetag string parses");
        check(p.argc() == 0, "and carries no arguments");
    }
    {
        Msg m("/sk/midi/msg", "iii");
        m.i(144).i(60).i(100);
        OscMessage p;
        check(p.parse(m.data(), m.size()), "three ints parse");
        int32_t a = 0, b = 0, c = 0;
        check(p.as_i32(0, a) && p.as_i32(1, b) && p.as_i32(2, c) && a == 144 && b == 60 && c == 100,
              "big-endian ints survive in order");
    }
    {
        OscMessage p;
        const uint8_t junk[3] = { 1, 2, 3 };
        check(!p.parse(junk, 3), "a packet that is not a whole number of words is rejected");
        Msg noslash("sk/a", "f"); noslash.f(1.f);
        check(!p.parse(noslash.data(), noslash.size()), "an address without a leading slash is rejected");
        // Blobs and timetag arguments are refused outright rather than skipped: skipping one would
        // silently shift every argument after it.
        Msg blob("/sk/a/param/size", "b"); blob.i(0);
        check(!p.parse(blob.data(), blob.size()), "a blob argument is rejected");
    }

    std::printf("osc type coercion\n");
    {
        // float <- f, i, d; non-finite refused.
        Msg mi("/x", "i"); mi.i(1);
        Msg md("/x", "d"); md.d(0.25);
        OscMessage p; float f = 0;
        check(p.parse(mi.data(), mi.size()) && p.as_f32(0, f) && f == 1.f, "int coerces to float");
        check(p.parse(md.data(), md.size()) && p.as_f32(0, f) && f == 0.25f, "double narrows to float");
        Msg mn("/x", "f"); mn.f(NAN);
        check(p.parse(mn.data(), mn.size()) && !p.as_f32(0, f), "a non-finite float is refused");
    }
    {
        // int <- i, f (truncated toward zero), T/F.
        OscMessage p; int32_t v = 0;
        Msg mf("/x", "f"); mf.f(2.7f);
        check(p.parse(mf.data(), mf.size()) && p.as_i32(0, v) && v == 2, "float truncates toward zero");
        Msg mt("/x", "T");
        check(p.parse(mt.data(), mt.size()) && p.as_i32(0, v) && v == 1, "T is 1");
        Msg mF("/x", "F");
        check(p.parse(mF.data(), mF.size()) && p.as_i32(0, v) && v == 0, "F is 0");
    }
    {
        // bool: absent argument = true (the bare trigger), and a zero float suppresses - which is what
        // makes a TouchOSC button that sends ,f 0.0 on release not fire twice.
        OscMessage p; bool b = false;
        Msg none("/x", nullptr);
        check(p.parse(none.data(), none.size()) && p.as_bool(0, b) && b, "absent argument reads true");
        Msg zero("/x", "f"); zero.f(0.f);
        check(p.parse(zero.data(), zero.size()) && p.as_bool(0, b) && !b, "a zero float is false");
        Msg one("/x", "f"); one.f(1.f);
        check(p.parse(one.data(), one.size()) && p.as_bool(0, b) && b, "a non-zero float is true");
    }
}

// --- 4/5. addresses, reads, writes ---------------------------------------------------------------------

void test_params() {
    std::printf("addresses: params\n");
    MockEngine e;

    check_eq(send(e, Msg("/sk/a/param/speed", "f").f(0.5f)), "", "a successful write is SILENT");
    check_eq(e.last(), "set_param speed 0 0.5000", "the write reached the engine");

    check_eq(send(e, Msg("/sk/b/param/size", "f").f(0.25f)), "", "deck b write");
    check_eq(e.last(), "set_param size 1 0.2500", "deck comes from the path, never an argument");

    // Reads: arity, not a verb. The reply mirrors the request path under /sk/reply and is TYPED.
    check_eq(send(e, Msg("/sk/a/param/speed", nullptr)), "/sk/reply/a/param/speed ,f 0.5000",
             "no arguments is a read, and the reply is a float");

    // The ab alias: one packet, two dispatches, decks a then b.
    e.clear();
    check_eq(send(e, Msg("/sk/ab/param/pos", "f").f(0.75f)), "", "ab write is silent like any other");
    check(e.log.size() == 2, "ab fans out to exactly two dispatches");
    check_eq(e.log[0], "set_param pos 0 0.7500", "ab writes deck a first");
    check_eq(e.log[1], "set_param pos 1 0.7500", "then deck b");
    // Write-only: one request cannot have two answers on one reply address.
    check_eq(send(e, Msg("/sk/ab/param/pos", nullptr)), "/sk/err ,s /sk/ab/param/pos ,s bad-arg",
             "a read on ab is refused");

    // Globals carry NO deck segment - the address space encodes scope structurally.
    check_eq(send(e, Msg("/sk/param/crossfade", "f").f(0.3f)), "", "global param write");
    check_eq(e.last(), "set_param crossfade 0 0.3000", "a global param lands in the deck-A slot");
    check_eq(send(e, Msg("/sk/a/param/crossfade", "f").f(0.3f)),
             "/sk/err ,s /sk/a/param/crossfade ,s unknown-address",
             "a global param is NOT reachable with a deck segment");
    check_eq(send(e, Msg("/sk/param/speed", "f").f(0.3f)),
             "/sk/err ,s /sk/param/speed ,s unknown-address",
             "a deck param is NOT reachable without one");

    // A masked-out param is unknown-address, not a silent no-op.
    e.pmask = (1u << uint32_t(ParamId::Size));
    check_eq(send(e, Msg("/sk/a/param/pos", "f").f(0.5f)),
             "/sk/err ,s /sk/a/param/pos ,s unknown-address",
             "a param outside live_params() is unknown-address");
    e.pmask = ~IEngine::ParamMask{0};

    // The three platform-owned ids never reach set_param, so they are not addressable as params.
    for (const char* p : { "tempo", "keyinterval", "modspeed" }) {
        char addr[64]; std::snprintf(addr, sizeof addr, "/sk/a/param/%s", p);
        char want[128]; std::snprintf(want, sizeof want, "/sk/err ,s %s ,s unknown-address", addr);
        check_eq(send(e, Msg(addr, "f").f(0.5f)), want, "a platform-owned param has no param address");
    }
}

void test_configs_and_state() {
    std::printf("addresses: configs and state\n");
    MockEngine e;

    check_eq(send(e, Msg("/sk/a/cfg/mode", "i").i(1)), "", "config write is silent");
    check_eq(e.last(), "set_config mode 0 1", "config reaches the engine");
    check_eq(send(e, Msg("/sk/cfg/route", "i").i(2)), "", "route is global - no deck segment");
    check_eq(e.last(), "set_config route 0 2", "global config lands on deck A");
    check_eq(send(e, Msg("/sk/a/cfg/route", "i").i(2)),
             "/sk/err ,s /sk/a/cfg/route ,s unknown-address", "route is not deck-addressable");
    // set_config is write-only on IEngine, so a bare cfg address is a mistake rather than a read.
    check_eq(send(e, Msg("/sk/a/cfg/mode", nullptr)), "/sk/err ,s /sk/a/cfg/mode ,s no-arg",
             "a config address with no argument is refused");

    // State reads are typed by the query's declared ValueKind, reaching the sink through
    // ok_begin/append/ok_end - which is the mechanism that keeps layer [3] untouched.
    check_eq(send(e, Msg("/sk/a/state/empty", nullptr)), "/sk/reply/a/state/empty ,i 0", "bool reads as ,i");
    check_eq(send(e, Msg("/sk/b/state/empty", nullptr)), "/sk/reply/b/state/empty ,i 1", "deck b state");
    check_eq(send(e, Msg("/sk/a/state/recorded", nullptr)), "/sk/reply/a/state/recorded ,i 4096", "int state");
    check_eq(send(e, Msg("/sk/state/mix", nullptr)), "/sk/reply/state/mix ,f 0.2500", "a float read is ,f");
    check_eq(send(e, Msg("/sk/state/route", nullptr)), "/sk/reply/state/route ,i 2",
             "route reads back in the SELECTOR encoding config route accepts");
    // A state address is a read; an argument on one is how the codec spells "write", which it is not.
    check_eq(send(e, Msg("/sk/a/state/empty", "f").f(1.f)),
             "/sk/err ,s /sk/a/state/empty ,s too-many-args", "an argument on a state address is refused");
    check_eq(send(e, Msg("/sk/ab/state/empty", nullptr)),
             "/sk/err ,s /sk/ab/state/empty ,s bad-arg", "a read on ab has no single answer");
}

void test_stimulus() {
    std::printf("addresses: stimulus verbs\n");
    MockEngine e;

    check_eq(send(e, Msg("/sk/a/cv/voct", "f").f(1.f)), "", "cv voct");
    check_eq(e.last(), "cv_voct 0 1.0000", "cv voct binding");
    check_eq(send(e, Msg("/sk/a/cv/xfade", "f").f(0.7f)), "", "cv xfade");
    check_eq(e.last(), "cv_crossfade 0.7000", "cv xfade is global but keeps a deck segment for uniformity");

    check_eq(send(e, Msg("/sk/b/gate", nullptr)), "", "a bare gate fires");
    check_eq(e.last(), "on_gate_trigger 1", "gate binding");
    e.clear();
    check_eq(send(e, Msg("/sk/b/gate", "F")), "", "gate with ,F is suppressed");
    check(e.log.empty(), "a suppressed trigger reaches the engine not at all");
    e.clear();
    check_eq(send(e, Msg("/sk/b/gate", "f").f(0.f)), "", "gate with a zero float is suppressed too");
    check(e.log.empty(), "TouchOSC's button-release frame does not re-fire the gate");

    // pad play answers with the deck's emptiness - an ACTION that returns a value, which a host has no
    // other way to learn from the press.
    check_eq(send(e, Msg("/sk/a/pad/play", nullptr)), "/sk/reply/a/pad/play ,s ok empty=1",
             "pad play reports emptiness as a string reply");
    check_eq(e.last(), "on_play_pad 0 0", "pad play binding");
    check_eq(send(e, Msg("/sk/a/pad/play", "T")), "/sk/reply/a/pad/play ,s ok empty=1", "pad play reverse");
    check_eq(e.last(), "on_play_pad 0 1", "the boolean on a pad is REVERSE, not a trigger gate");
    check_eq(send(e, Msg("/sk/a/pad/clear", nullptr)), "", "pad clear is a silent write");
    check_eq(e.last(), "clear_buffer 0", "pad clear binding");

    check_eq(send(e, Msg("/sk/a/seq/arm", nullptr)), "", "seq arm");
    check_eq(e.last(), "on_seq_toggle_arm 0", "seq arm binding");

    check_eq(send(e, Msg("/sk/a/fx/flux", "T")), "", "fx on");
    check_eq(e.last(), "set_fx 0 flux 1", "fx flux on");
    check_eq(send(e, Msg("/sk/b/fx/grit", "F")), "", "fx off");
    check_eq(e.last(), "set_fx 1 grit 0", "fx grit off");
    check_eq(send(e, Msg("/sk/a/fx/lock/flux", nullptr)), "", "fx lock");
    check_eq(e.last(), "toggle_fx_lock 0 flux", "fx lock binding");
    check_eq(send(e, Msg("/sk/b/fx/gritmode", nullptr)),
             "/sk/reply/b/fx/gritmode ,s ok intensity=0.2500 mix=0.7500",
             "gritmode returns the reseed pair the platform uses to re-pick its MValues");

    check_eq(send(e, Msg("/sk/a/modspeed", "f").f(0.3f)), "", "modspeed");
    check_eq(e.last(), "set_mod_speed 0 0.3000 0", "modspeed defaults to unsynced");
    check_eq(send(e, Msg("/sk/b/modspeed", "fT").f(0.3f)), "", "modspeed synced");
    check_eq(e.last(), "set_mod_speed 1 0.3000 1", "the sync flag rides as a second argument");

    check_eq(send(e, Msg("/sk/midi/note", "ii").i(1).i(60)), "", "midi note");
    check_eq(e.last(), "midi_note 1 60", "midi note binding");
    check_eq(send(e, Msg("/sk/midi/msg", "iii").i(144).i(60).i(100)), "", "midi msg");
    check_eq(e.last(), "midi_msg 144 60 100", "midi msg binding");
    check_eq(send(e, Msg("/sk/midi/transport", "T")), "", "midi transport start");
    check_eq(e.last(), "midi_transport 1", "transport start");
    check_eq(send(e, Msg("/sk/midi/transport", "F")), "", "midi transport stop");
    check_eq(e.last(), "midi_transport 0", "transport stop");
}

void test_platform() {
    std::printf("addresses: platform (/sk/dev)\n");
    MockEngine e;
    TermState  st;

    check_eq(send(e, st, Msg("/sk/dev/mode/test", nullptr)), "", "mode test");
    check(st.test_mode, "mode test sets the isolation flag");
    check_eq(send(e, st, Msg("/sk/dev/mode/run", nullptr)), "", "mode run");
    check(!st.test_mode, "mode run clears it");

    check_eq(send(e, Msg("/sk/dev/caps", nullptr)), "/sk/reply/dev/caps ,i 307",
             "caps is an int on the wire (0x133), not the line codec's hex text");
    check_eq(send(e, Msg("/sk/dev/cpu", nullptr)), "/sk/reply/dev/cpu ,f 0.0000", "cpu reads as a float");
    check(send(e, Msg("/sk/dev/usb", nullptr)).rfind("/sk/reply/dev/usb ,s boot=", 0) == 0,
          "usb degrades to a string - many values, no single tag");
    check(send(e, Msg("/sk/dev/help", nullptr)).rfind("/sk/reply/dev/help ,s ok verbs:", 0) == 0,
          "help is a string reply");

    // reset and preset are platform composites and DO answer: the count is what lets a harness assert
    // the composite actually acted.
    e.pmask = (1u << uint32_t(ParamId::Size)) | (1u << uint32_t(ParamId::Crossfade));
    check_eq(send(e, Msg("/sk/dev/reset", nullptr)), "/sk/reply/dev/reset ,i 3",
             "reset reports how many params it wrote");
    check_eq(send(e, Msg("/sk/dev/reset", "s").s("a")), "/sk/reply/dev/reset ,i 2",
             "the optional deck arrives as a string - a qualifier, not a value");
    check_eq(send(e, Msg("/sk/dev/reset/cpu", nullptr)), "", "reset cpu is a silent write");

    TermState ps;
    check_eq(send(e, ps, Msg("/sk/dev/preset/save", "i").i(0)), "/sk/reply/dev/preset/save ,i 3",
             "preset save reports the captured count");
    check_eq(send(e, ps, Msg("/sk/dev/preset/load", "i").i(0)), "/sk/reply/dev/preset/load ,i 3",
             "preset load reports the restored count");
}

void test_ack_mode() {
    std::printf("ack mode\n");
    MockEngine e;
    TermState  st;

    check_eq(send(e, st, Msg("/sk/a/param/speed", "f").f(0.5f)), "", "silent by default");
    check_eq(send(e, st, Msg("/sk/dev/mode/ack", "T")), "", "turning acks on is itself silent");
    check(st.osc_ack, "ack mode latched on the session");
    check_eq(send(e, st, Msg("/sk/a/param/speed", "f").f(0.5f)),
             "/sk/reply/ok ,s /sk/a/param/speed",
             "with acks on, a successful write echoes the address it acknowledges");
    check_eq(send(e, st, Msg("/sk/a/param/speed", nullptr)), "/sk/reply/a/param/speed ,f 0.5000",
             "a read is unaffected by ack mode");
    check_eq(send(e, st, Msg("/sk/dev/mode/ack", "F")), "", "acks off");
    check(!st.osc_ack, "ack mode cleared");
    check_eq(send(e, st, Msg("/sk/a/param/speed", "f").f(0.5f)), "", "silent again");
}

void test_errors() {
    std::printf("error taxonomy\n");
    MockEngine e;

    check_eq(send(e, Msg("/sk/a/param/nosuch", nullptr)),
             "/sk/err ,s /sk/a/param/nosuch ,s unknown-address", "an unknown slot");
    check_eq(send(e, Msg("/sk/a/nosuch/thing", nullptr)),
             "/sk/err ,s /sk/a/nosuch/thing ,s unknown-address", "an unknown kind segment");
    check_eq(send(e, Msg("/sk/z/param/size", "f").f(0.5f)),
             "/sk/err ,s /sk/z/param/size ,s unknown-address", "an unknown deck");
    check_eq(send(e, Msg("/nope/a/param/size", "f").f(0.5f)),
             "/sk/err ,s /nope/a/param/size ,s unknown-address", "a foreign root");
    check_eq(send(e, Msg("/sk/a/param/size", "ff").f(0.5f).f(0.5f)),
             "/sk/err ,s /sk/a/param/size ,s too-many-args",
             "extra trailing arguments are an error, not ignored");
    check_eq(send(e, Msg("/sk/a/param/size", "s").s("loud")),
             "/sk/err ,s /sk/a/param/size ,s bad-arg", "a string where a float belongs");
    check_eq(send(e, Msg("/sk/a/cv/nosuch", "f").f(1.f)),
             "/sk/err ,s /sk/a/cv/nosuch ,s unknown-address", "an unknown cv channel");

    // The error address is echoed. Without it nothing correlates a request to its rejection, since
    // errors do not mirror the request path the way replies do.
    {
        StringOut out;
        OscSink   sink(out);
        TermState st;
        const uint8_t junk[4] = { 'x', 'y', 'z', 0 };
        osc_dispatch_packet(junk, 4, e, sink, st);
        auto r = decode_replies(out.s);
        check(r.size() == 1 && r[0].all() == "/sk/err ,s /sk ,s bad-packet",
              "an unparseable packet is bad-packet against the root");
    }
}

void test_bundles() {
    std::printf("inbound bundles\n");
    MockEngine e;
    TermState  st;

    // Bundle contents dispatch immediately and IN ORDER; the timetag is ignored, not scheduled.
    Msg a("/sk/a/param/pos", "f");  a.f(0.1f);
    Msg b("/sk/b/param/pos", "f");  b.f(0.2f);
    std::string bundle = "#bundle";
    bundle.push_back('\0');
    for (int i = 0; i < 7; i++) bundle.push_back('\0');
    bundle.push_back('\1');                       // timetag: immediately
    put_be32(bundle, uint32_t(a.size())); bundle += a.bytes;
    put_be32(bundle, uint32_t(b.size())); bundle += b.bytes;

    e.clear();
    StringOut out;
    OscSink   sink(out);
    osc_dispatch_packet(reinterpret_cast<const uint8_t*>(bundle.data()), bundle.size(), e, sink, st);
    check(e.log.size() == 2, "both bundle elements dispatched");
    check_eq(e.log[0], "set_param pos 0 0.1000", "in order: first element");
    check_eq(e.log[1], "set_param pos 1 0.2000", "in order: second element");

    {
        // Nesting recurses, and a small packet can encode a lot of levels. Deeply nested bundles are
        // refused rather than walked - the one place malformed input could cost stack rather than just
        // an error reply.
        std::string deep = a.bytes;
        for (int i = 0; i < 8; i++) {
            std::string outer = "#bundle";
            outer.push_back('\0');
            for (int k = 0; k < 7; k++) outer.push_back('\0');
            outer.push_back('\1');
            put_be32(outer, uint32_t(deep.size()));
            outer += deep;
            deep = outer;
        }
        e.clear();
        StringOut o2;
        OscSink   s2(o2);
        osc_dispatch_packet(reinterpret_cast<const uint8_t*>(deep.data()), deep.size(), e, s2, st);
        check(e.log.empty(), "a deeply nested bundle reaches the engine not at all");
        auto r = decode_replies(o2.s);
        check(r.size() == 1 && r[0].args.find("bad-packet") != std::string::npos,
              "and is refused as bad-packet");
    }
}

// --- 8. describe ---------------------------------------------------------------------------------------

std::vector<Reply> describe_rows(MockEngine& e, size_t* bundle_bytes = nullptr,
                                 const char* dump_to = nullptr) {
    StringOut out;
    OscSink   sink(out);
    TermState st;
    Msg m("/sk/dev/describe", nullptr);
    osc_dispatch_packet(m.data(), m.size(), e, sink, st);

    // Hand the raw bundle to tools/test_osc_codec.py, so the host decoder and the semantic translator
    // are tested against real firmware bytes rather than a hand-written fixture that can drift.
    if (dump_to) {
        for (const auto& f : slip_frames(out.s)) {
            if (FILE* fp = std::fopen(dump_to, "wb")) {
                std::fwrite(f.data(), 1, f.size(), fp);
                std::fclose(fp);
                std::printf("  wrote %s (%zu bytes)\n", dump_to, f.size());
            }
            break;
        }
    }

    // The descriptor is ONE bundle, so what comes back is a single SLIP frame; decode_rows unwraps it
    // and walks the elements - exactly what a host has to do, and the reason it must arrive atomically.
    return decode_rows(out.s, bundle_bytes);
}

int count_rows(const std::vector<Reply>& rows, const char* addr) {
    int n = 0;
    for (const auto& r : rows) if (r.addr == addr) n++;
    return n;
}

bool has_row(const std::vector<Reply>& rows, const char* addr, const char* args) {
    for (const auto& r : rows) if (r.addr == addr && r.args == args) return true;
    return false;
}

// Same, matching only the head of the argument list - for asserting the address a row carries without
// restating the label, range and scope that follow it.
bool has_row_prefix(const std::vector<Reply>& rows, const char* addr, const std::string& args) {
    for (const auto& r : rows) if (r.addr == addr && r.args.rfind(args, 0) == 0) return true;
    return false;
}

void test_describe() {
    std::printf("describe (bundle)\n");
    {
        MockEngine e;
        e.labels = true;
        e.pmask  = (1u << uint32_t(ParamId::Speed)) | (1u << uint32_t(ParamId::Crossfade));
        e.cmask  = static_cast<IEngine::ConfigMask>(1u << uint32_t(ConfigId::Mode));
        auto rows = describe_rows(e, nullptr, "build/describe_osc_sample.bin");

        check(count_rows(rows, "/sk/reply/dev/describe") == 1, "exactly one descr row");
        {
            // Engine name and version come from version.cpp and differ between host and firmware
            // builds, so assert the SHAPE - three strings, the last being the masked flag a host sweep
            // keys off - rather than pinning values this build does not own.
            std::string descr;
            for (const auto& r : rows) if (r.addr == "/sk/reply/dev/describe") descr = r.args;
            check(descr.find(",s masked=1") != std::string::npos,
                  "the descr row ends with the masked flag");
            check(std::count(descr.begin(), descr.end(), ',') == 3,
                  "the descr row carries exactly three strings: engine, version, masked");
        }

        // Deck expansion happens ON DEVICE, so a host consuming the descriptor never learns decks exist.
        check(has_row(rows, "/sk/reply/dev/describe/param",
                      ",s /sk/a/param/speed ,s station ,f 0.0000 ,f 1.0000 ,s deck"),
              "a deck param emits an `a` row carrying the engine's LABEL, not the slot name");
        check(has_row(rows, "/sk/reply/dev/describe/param",
                      ",s /sk/b/param/speed ,s station ,f 0.0000 ,f 1.0000 ,s deck"),
              "and a `b` row");
        check(has_row(rows, "/sk/reply/dev/describe/param",
                      ",s /sk/param/crossfade ,s crossfade ,f 0.0000 ,f 1.0000 ,s global"),
              "a global param emits ONE row, with no deck segment and the slot name as its fallback label");
        check(count_rows(rows, "/sk/reply/dev/describe/param") == 3, "two deck rows + one global row");

        check(has_row(rows, "/sk/reply/dev/describe/cfg",
                      ",s /sk/a/cfg/mode ,s mode ,s 0:slice 1:reel 2:drift"),
              "a config row carries its address and selector labels");
        check(has_row(rows, "/sk/reply/dev/describe/state", ",s /sk/a/state/empty ,s empty ,s bool ,s "),
              "a deck state row (empty label string for a non-enum kind)");
        check(has_row(rows, "/sk/reply/dev/describe/state", ",s /sk/state/mix ,s mix ,s float ,s "),
              "a global state row has no deck segment");
        // An Enum query must carry its selector labels, or a host cannot check a reply against them -
        // and the line codec DOES send them, so omitting them here was a parity break.
        check(has_row(rows, "/sk/reply/dev/describe/state",
                      ",s /sk/state/route ,s route ,s enum ,s 0:stereo 1:dmono 2:genstereo"),
              "an enum state row carries its selector labels");
        check(!has_row(rows, "/sk/reply/dev/describe/state", ",s /sk/a/state/reseed ,s reseed ,s bool ,s "),
              "the latching read is never advertised");
        check(count_rows(rows, "/sk/reply/dev/describe/caps") == 1, "one caps row");

        // The property the universal-layout claim rests on: EVERY address describe advertises must be
        // one the codec actually accepts. This is the drift check between osc_addr.cpp and the
        // descriptor - the two places that both know how an address is spelled.
        //
        // State rows are checked alongside param rows, and were not always: while this loop looked at
        // params only, describe advertised `/sk/state/cpu` for the four platform reads the resolver
        // answers at `/sk/dev/cpu`, and nothing said so. Both spellings resolve, so a reachability
        // check alone would still have missed it - hence the exact-address assertion below.
        for (const auto& r : rows) {
            const bool param = r.addr == "/sk/reply/dev/describe/param";
            const bool state = r.addr == "/sk/reply/dev/describe/state";
            if (!param && !state) continue;   // cfg is write-only: a bare read of one is bad-arg
            const size_t b = r.args.find(",s ") + 3, en = r.args.find(' ', b);
            const std::string addr = r.args.substr(b, en - b);
            MockEngine probe;
            probe.labels = e.labels; probe.pmask = e.pmask; probe.cmask = e.cmask;
            const std::string reply = send(probe, Msg(addr.c_str(), nullptr));
            check(reply.rfind("/sk/reply", 0) == 0,
                  ("advertised address " + addr + " is readable exactly as composed").c_str());
        }

        // ...and spelled where the resolver answers it. `cpu`/`cpumin`/`cpumax`/`usb` report on the
        // channel and the board rather than on the engine's control surface, so the address space puts
        // them under /sk/dev - which the descriptor has to agree with, not merely be reachable through.
        for (const char* name : { "cpu", "cpumin", "cpumax", "usb" }) {
            const std::string dev   = std::string(",s /sk/dev/") + name + " ,s " + name;
            const std::string state = std::string(",s /sk/state/") + name + " ,s " + name;
            check(has_row_prefix(rows, "/sk/reply/dev/describe/state", dev),
                  (std::string("the platform read ") + name + " is advertised under /sk/dev").c_str());
            check(!has_row_prefix(rows, "/sk/reply/dev/describe/state", state),
                  (std::string("the platform read ") + name
                   + " is not also advertised under /sk/state").c_str());
        }
    }
    {
        // An engine with no labels at all degrades to the generic tier, never breaks.
        MockEngine e;
        e.pmask = (1u << uint32_t(ParamId::Speed));
        e.cmask = 0;
        auto rows = describe_rows(e);
        check(has_row(rows, "/sk/reply/dev/describe/param",
                      ",s /sk/a/param/speed ,s speed ,f 0.0000 ,f 1.0000 ,s deck"),
              "with no param_label the layer-2 name is the label");
    }
    {
        // SIZING. The unmasked default is the worst case and it has to fit, or describe fails on
        // exactly the engines that have not narrowed their masks yet.
        MockEngine e;   // all-live masks
        size_t bytes = 0;
        auto rows = describe_rows(e, &bytes);
        std::printf("  unmasked describe bundle: %zu bytes, %zu rows\n", bytes, rows.size());
        check(bytes > 0, "the unmasked descriptor is produced at all");
        check(bytes <= kOscBundleCap, "the unmasked descriptor fits the bundle buffer");
        // It must also fit the TX FIFO whole, since a bundle cannot be streamed the way lines can.
        TxFifo fifo;
        check(fifo.free_space() >= bytes + 2, "the unmasked descriptor fits the TX FIFO in one piece");
        check(count_rows(rows, "/sk/reply/dev/describe/param") == 2 * 17 + 4,
              "17 deck-scoped params x 2 decks + 4 global, with the platform-owned ids excluded");
    }
    {
        // A FIFO that cannot take the whole frame must refuse it, not write a corrupt half-frame: a
        // truncated SLIP frame is undetectable garbage on the wire.
        MockEngine e;
        BoundedOut out(64);
        OscSink    sink(out);
        TermState  st;
        Msg m("/sk/dev/describe", nullptr);
        osc_dispatch_packet(m.data(), m.size(), e, sink, st);
        auto r = decode_replies(out.s);
        check(r.size() == 1 && r[0].args.find("overflow") != std::string::npos,
              "a descriptor that does not fit is refused with an error, not truncated");
    }
}

}  // namespace

int main() {
    std::printf("== terminal OSC codec (off-target) ==\n");
    test_slip();
    test_wire();
    test_params();
    test_configs_and_state();
    test_stimulus();
    test_platform();
    test_ack_mode();
    test_errors();
    test_bundles();
    test_describe();

    if (g_failures == 0) { std::printf("OK: all OSC checks passed\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
