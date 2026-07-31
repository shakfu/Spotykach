// Off-target tests for the SPK_TERMINAL command channel (docs/dev/terminal-*.md).
//
// Everything below the USB transport is hardware-free: the ring, the TX FIFO, the line assembler, the
// tokenizer, the value coercion, the reply formatter, and the dispatcher all depend on nothing but
// IEngine. Only src/terminal/terminal.cpp (which owns daisy::UsbHandle) is excluded, so this covers
// layers [1-header], [2] and [3] against a recording mock engine - no device, no USB, no serial port.
//
//   1. Tokenizer: splitting, whitespace runs, arg limits.
//   2. LineAssembler: CRLF vs bare LF, and that an over-long line's tail is never re-parsed.
//   3. RxRing: SPSC round-trip, wraparound, overflow latching.
//   4. TxFifo: peek/commit discipline, all-or-nothing enqueue, wraparound.
//   5. Value coercion: parse_f32/i32/deck/onoff, including the rejections.
//   6. Reply formatting: integer decomposition of floats (no %f), hex, rounding carry, framing.
//   7. Dispatch: every phase-1 verb -> the exact IEngine call and the exact reply bytes.
//   8. describe: liveness masks honoured, scope tags, CRLF framing, `end` terminator.
//
// The describe block is also written to build/describe_sample.txt so tools/test_descriptor.py can
// check the host-side parser against the firmware's real output without a device attached.
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/iengine.h"
#include "terminal/command.h"
#include "terminal/dispatch.h"
#include "terminal/fmt.h"
#include "terminal/line_assembler.h"
#include "terminal/names.h"
#include "terminal/rx_ring.h"
#include "terminal/term_state.h"
#include "terminal/tx_fifo.h"

using namespace spotykach;

namespace {

int g_failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); g_failures++; }
}
void check_eq(const std::string& got, const char* want, const char* msg) {
    if (got != want) {
        std::printf("  FAIL: %s\n        want %s\n        got  %s\n", msg,
                    std::string(want).c_str(), got.c_str());
        g_failures++;
    }
}

// --- test doubles ---------------------------------------------------------------------------------

// Collects reply bytes so a whole reply can be compared literally, CRLF included.
struct StringOut : ITextOut {
    std::string s;
    void write(const char* p, size_t n) override { s.append(p, n); }
};

// Records every platform-driven call the dispatcher makes, as a formatted line, so a test can assert
// both "the right method ran" and "with the right arguments" in one comparison.
struct MockEngine : IEngine {
    void init(const EngineContext&) override {}
    void prepare() override {}
    void process(const float* const*, float**, size_t) override {}

    std::vector<std::string> log;
    void rec(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[160];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        log.emplace_back(buf);
    }
    std::string last() const { return log.empty() ? std::string("<none>") : log.back(); }

    // Params: stored verbatim so `get param` round-trips (the delay engine does exactly this).
    float params[static_cast<size_t>(ParamId::Count)][2] = {};
    void set_param(ParamId id, DeckRef::Ref d, float v) override {
        params[static_cast<size_t>(id)][d == DeckRef::B ? 1 : 0] = v;
        rec("set_param %s %d %.4f", param_name(id), int(d), v);
    }
    // A non-0.5 default, so a reset is distinguishable from "everything happened to be 0.5".
    float param_default(ParamId) const override { return 0.25f; }
    float param(ParamId id, DeckRef::Ref d) const override {
        return params[static_cast<size_t>(id)][d == DeckRef::B ? 1 : 0];
    }
    void set_mod_speed(DeckRef::Ref d, float v, bool sync) override {
        rec("set_mod_speed %d %.4f %d", int(d), v, sync ? 1 : 0);
    }

    bool config_changed = true;
    bool set_config(ConfigId id, DeckRef::Ref d, int v) override {
        rec("set_config %s %d %d", config_name(id), int(d), v);
        return config_changed;
    }

    void cv_voct(DeckRef::Ref d, float v) override { rec("cv_voct %d %.4f", int(d), v); }
    void cv_mix(DeckRef::Ref d, float v) override { rec("cv_mix %d %.4f", int(d), v); }
    void cv_size_pos(DeckRef::Ref d, float v) override { rec("cv_size_pos %d %.4f", int(d), v); }
    void cv_crossfade(float v) override { rec("cv_crossfade %.4f", v); }

    void on_gate_trigger(DeckRef::Ref d) override { rec("on_gate_trigger %d", int(d)); }
    bool gate_out = true;
    bool gate_out_triggered(DeckRef::Ref d) override { return gate_out && d == DeckRef::A; }

    DeckRef::Ref handle_midi_note(uint8_t ch, uint8_t n) override {
        rec("midi_note %u %u", ch, n);
        return DeckRef::A;
    }
    void handle_midi_message(uint8_t s, uint8_t a, uint8_t b) override {
        rec("midi_msg %u %u %u", s, a, b);
    }
    void handle_midi_transport(bool start) override { rec("midi_transport %d", start ? 1 : 0); }

    bool play_pad_empty = true;
    bool on_play_pad(DeckRef::Ref d, bool rev) override {
        rec("on_play_pad %d %d", int(d), rev ? 1 : 0);
        return play_pad_empty;
    }
    void on_record_pad(DeckRef::Ref d, bool rev) override { rec("on_record_pad %d %d", int(d), rev ? 1 : 0); }
    void on_seq_trigger(DeckRef::Ref d) override { rec("on_seq_trigger %d", int(d)); }
    void stop_if_generating(DeckRef::Ref d) override { rec("stop_if_generating %d", int(d)); }
    void clear_buffer(DeckRef::Ref d) override { rec("clear_buffer %d", int(d)); }
    void set_fx(DeckRef::Ref d, FxKind k, bool on) override {
        rec("set_fx %d %s %d", int(d), k == FxKind::Flux ? "flux" : "grit", on ? 1 : 0);
    }
    void toggle_fx_lock(DeckRef::Ref d, FxKind k) override {
        rec("toggle_fx_lock %d %s", int(d), k == FxKind::Flux ? "flux" : "grit");
    }
    GritReseed toggle_grit_mode(DeckRef::Ref d) override {
        rec("toggle_grit_mode %d", int(d));
        return { 0.25f, 0.75f };
    }
    void on_seq_toggle_arm(DeckRef::Ref d) override { rec("on_seq_toggle_arm %d", int(d)); }
    void clear_sequence(DeckRef::Ref d) override    { rec("clear_sequence %d", int(d)); }
    void disarm_track(DeckRef::Ref d) override      { rec("disarm_track %d", int(d)); }

    size_t audio_recorded_bytes(DeckRef::Ref) override { return 4096; }
    size_t audio_capacity_bytes(DeckRef::Ref) override { return 65536; }
    DeckLayout deck_layout(DeckRef::Ref) override      { return DeckLayout::slice; }   // == 1
    bool size_sets_tempo(DeckRef::Ref) override        { return true; }
    float tempo_to_fit(DeckRef::Ref, float fraction) override { return 120.f * fraction; }
    // Latching, exactly like the real thing: true once, then false.
    bool reseed_pending = true;
    bool take_param_reseed(DeckRef::Ref) override {
        const bool r = reseed_pending; reseed_pending = false; return r;
    }

    bool empty_a = false;
    bool audio_is_empty(DeckRef::Ref d) override { return d == DeckRef::A ? empty_a : true; }
    float mix() const override { return 0.25f; }
    Route route() const override { return Route::GenerativeStereo; }   // == 3
    Capabilities capabilities() const override { return 0x133u; }

    // Target B. `custom` off = the default "not mine" behaviour; on = recognizes one query name.
    bool custom = false;
    bool handle_command(const CommandView& c, TextSink& reply) override {
        if (!custom) return false;
        if (c.argc == 3 && !strcmp(c.arg(0), "query") && !strcmp(c.arg(1), "loop_ms")) {
            DeckRef::Ref d;
            if (!parse_deck(c.arg(2), d)) { reply.err("bad-deck"); return true; }
            reply.ok_f32(123.5f);
            return true;
        }
        return false;
    }

    // Target B: a declared table covering every ValueKind, both scopes, a name that COLLIDES with a
    // platform query (the platform must win), and an unsafe entry that must never be advertised.
    enum EQ : uint8_t { EQ_LOOP, EQ_STATE, EQ_ON, EQ_N, EQ_TXT, EQ_MIX, EQ_LATCH, EQ_COUNT };
    static constexpr EngineQuery kEQ[] = {
        { "loop_ms", QueryScope::Deck,   ValueKind::Float, nullptr, true },
        { "state",   QueryScope::Global, ValueKind::Enum,  "0:stopped 1:playing", true },
        { "armed",   QueryScope::Deck,   ValueKind::Bool,  nullptr, true },
        { "grains",  QueryScope::Global, ValueKind::Int,   nullptr, true },
        { "label",   QueryScope::Global, ValueKind::Text,  nullptr, true },
        { "mix",     QueryScope::Global, ValueKind::Float, nullptr, true },   // collides on purpose
        { "latch",   QueryScope::Deck,   ValueKind::Bool,  nullptr, false },  // never advertised
    };
    static_assert(sizeof(kEQ) / sizeof(kEQ[0]) == EQ_COUNT, "kEQ out of sync");

    bool declare_queries = false;   // off by default: most tests want a bare engine
    EngineQueryTable engine_queries() const override {
        return declare_queries ? EngineQueryTable{ kEQ, EQ_COUNT } : EngineQueryTable{ nullptr, 0 };
    }
    void read_engine_query(uint8_t i, DeckRef::Ref d, TextSink& r) override {
        switch (i) {
            case EQ_LOOP:  r.append_f32(d == DeckRef::B ? 250.f : 125.f); break;
            case EQ_STATE: r.append_i32(1); break;
            case EQ_ON:    r.append_i32(d == DeckRef::A ? 1 : 0); break;
            case EQ_N:     r.append_i32(42); break;
            case EQ_TXT:   r.str("tape-a"); break;
            case EQ_MIX:   r.append_f32(9.9f); break;   // must never be reached: platform wins
            case EQ_LATCH: r.append_i32(1); break;
            default: break;
        }
    }

    ParamMask  pmask = ~ParamMask{0};
    ConfigMask cmask = static_cast<ConfigMask>(~ConfigMask{0});
    ParamMask  live_params() const override { return pmask; }
    ConfigMask live_configs() const override { return cmask; }
};

// Send one line through the dispatcher exactly as Terminal::on_line would, and return the reply bytes.
std::string run(MockEngine& e, TermState& st, const char* cmd) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", cmd);
    StringOut out;
    TextSink  sink(out);
    dispatch_line(buf, e, sink, st);
    return out.s;
}

// Convenience for the common case where the dispatcher state does not matter to the assertion.
std::string run(MockEngine& e, const char* cmd) {
    TermState st;
    return run(e, st, cmd);
}

std::string fmt_f32(float v, int decimals = 4) {
    StringOut out;
    TextSink(out).append_f32(v, decimals);
    return out.s;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Count occurrences of a line-start tag ("param ", "config ", ...) in a CRLF-framed block.
int count_lines_with(const std::string& block, const char* tag) {
    int n = 0;
    size_t pos = 0;
    const std::string t = tag;
    while (pos < block.size()) {
        size_t eol = block.find("\r\n", pos);
        if (eol == std::string::npos) eol = block.size();
        if (block.compare(pos, t.size(), t) == 0) n++;
        pos = eol + 2;
    }
    return n;
}

// --- 1. tokenizer ---------------------------------------------------------------------------------

void test_tokenizer() {
    std::printf("tokenizer\n");
    {
        char line[] = "set param size A 0.5";
        Command c;
        check(tokenize(line, c), "plain line tokenizes");
        check(c.argc == 5, "argc == 5");
        check_eq(c.verb(), "set", "verb");
        check_eq(c.arg(2), "size", "argv[2]");
        check_eq(c.arg(4), "0.5", "argv[4]");
        check_eq(c.arg(9), "", "out-of-range arg is empty, not UB");
    }
    {
        char line[] = "  gate \t  A   ";   // leading, tab, run, trailing
        Command c;
        check(tokenize(line, c), "whitespace-heavy line tokenizes");
        check(c.argc == 2, "whitespace runs collapse");
        check_eq(c.arg(1), "A", "second token past a tab run");
    }
    {
        char line[] = "";
        Command c;
        check(tokenize(line, c), "empty line is not an error");
        check(c.argc == 0, "empty line has no tokens");
        check_eq(c.verb(), "", "verb of an empty line is empty");
    }
    {
        char line[] = "   \t ";
        Command c;
        check(tokenize(line, c) && c.argc == 0, "all-whitespace line has no tokens");
    }
    {
        char six[] = "a b c d e f";       // exactly kMaxArgs
        char seven[] = "a b c d e f g";   // one too many
        Command c;
        check(tokenize(six, c) && c.argc == 6, "kMaxArgs tokens accepted");
        check(!tokenize(seven, c), "kMaxArgs+1 tokens rejected");
    }
}

// --- 2. line assembler ----------------------------------------------------------------------------

// Feed a whole string; collect completed lines and count TooLong reports, mimicking Terminal::process.
struct Assembled {
    std::vector<std::string> lines;
    int too_long = 0;
};
Assembled feed_all(LineAssembler& a, const std::string& bytes) {
    Assembled r;
    for (unsigned char b : bytes) {
        switch (a.feed(b)) {
            case LineAssembler::Feed::Ready:   r.lines.emplace_back(a.line()); a.reset(); break;
            case LineAssembler::Feed::TooLong: r.too_long++; break;
            case LineAssembler::Feed::Pending: break;
        }
    }
    return r;
}

void test_line_assembler() {
    std::printf("line assembler\n");
    {
        LineAssembler a;
        auto r = feed_all(a, "caps\r\ngate A\n");
        check(r.lines.size() == 2 && r.too_long == 0, "CRLF and bare LF both terminate");
        check_eq(r.lines[0], "caps", "CR trimmed");
        check_eq(r.lines[1], "gate A", "bare LF line intact");
    }
    {
        LineAssembler a;
        auto r = feed_all(a, "\n");
        check(r.lines.size() == 1 && r.lines[0].empty(), "empty line completes as an empty string");
    }
    {
        // The regression that matters: an over-long line must be swallowed to its terminator, so its
        // truncated tail is never re-parsed as a bogus command, and the NEXT line must still work.
        LineAssembler a;
        const std::string flood(400, 'x');
        auto r = feed_all(a, flood + "\ncaps\n");
        check(r.too_long == 1, "over-long line reported exactly once");
        check(r.lines.size() == 1, "the over-long line yields no command");
        check_eq(r.lines[0], "caps", "the following line parses cleanly");
    }
    {
        LineAssembler a;
        const std::string at_limit(128, 'y');   // exactly kMax, must NOT trip the guard
        auto r = feed_all(a, at_limit + "\n");
        check(r.too_long == 0, "a line of exactly kMax bytes is not too long");
        check(r.lines.size() == 1 && r.lines[0].size() == 128, "kMax-byte line arrives whole");
    }
}

// --- 3. RX ring -----------------------------------------------------------------------------------

void test_rx_ring() {
    std::printf("rx ring\n");
    {
        RxRing ring;
        const char* msg = "gate A\n";
        ring.push(reinterpret_cast<const uint8_t*>(msg), 7);
        uint8_t out[16] = {};
        check(ring.pop(out, sizeof out) == 7, "pop returns what was pushed");
        check(std::memcmp(out, msg, 7) == 0, "bytes survive the ring");
        check(ring.pop(out, sizeof out) == 0, "drained ring returns nothing");
        check(!ring.take_overflow(), "no overflow on a normal push");
    }
    {
        // Wrap the 512-byte ring: three 200-byte round trips push head past the capacity boundary.
        RxRing ring;
        std::vector<uint8_t> src(200), dst(200);
        for (int round = 0; round < 3; round++) {
            for (int i = 0; i < 200; i++) src[i] = uint8_t(round * 200 + i);
            ring.push(src.data(), src.size());
            check(ring.pop(dst.data(), dst.size()) == 200, "wrapped pop count");
            check(dst == src, "wrapped bytes are intact");
        }
    }
    {
        // Overflow: the tail of the offending push is dropped, the prefix is kept, the flag latches.
        RxRing ring;
        std::vector<uint8_t> big(600);
        for (size_t i = 0; i < big.size(); i++) big[i] = uint8_t(i);
        ring.push(big.data(), big.size());
        check(ring.take_overflow(), "overflow latched");
        check(!ring.take_overflow(), "take_overflow clears the latch");
        std::vector<uint8_t> got(600);
        const size_t n = ring.pop(got.data(), got.size());
        check(n == 512, "only capacity worth of bytes was kept");
        check(std::memcmp(got.data(), big.data(), n) == 0, "the kept prefix is uncorrupted");
    }
    {
        RxRing ring;
        const uint8_t src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        ring.push(src, 8);
        uint8_t out[3] = {};
        check(ring.pop(out, 3) == 3 && out[0] == 1 && out[2] == 3, "partial pop takes the head");
        check(ring.pop(out, 3) == 3 && out[0] == 4, "the next pop resumes where it stopped");
    }
}

// --- 4. TX FIFO -----------------------------------------------------------------------------------

void test_tx_fifo() {
    std::printf("tx fifo\n");
    {
        TxFifo tx;
        tx.enqueue("ok\r\n", 4);
        uint8_t buf[8] = {};
        check(tx.count() == 4, "count reflects the enqueue");
        check(tx.peek(buf, sizeof buf) == 4, "peek reports the pending bytes");
        check(std::memcmp(buf, "ok\r\n", 4) == 0, "peek copies the right bytes");
        check(tx.count() == 4, "peek does NOT advance (the in-flight discipline)");
        tx.commit(4);
        check(tx.empty(), "commit advances");
    }
    {
        // All-or-nothing: a partial reply would corrupt the line-framed protocol, so a write that does
        // not fit is dropped whole and latched.
        TxFifo tx;
        const std::vector<char> filler(2040, 'x');
        tx.enqueue(filler.data(), filler.size());
        check(!tx.take_overflow(), "a fitting write does not latch");
        tx.enqueue("0123456789abcdef", 16);   // 16 > 8 free
        check(tx.count() == 2040, "the oversized write was dropped whole, not truncated");
        check(tx.take_overflow(), "the drop latched");
        check(!tx.take_overflow(), "take_overflow clears");
    }
    {
        // Wrap the 2048-byte FIFO with repeated 700-byte enqueue/commit cycles.
        TxFifo tx;
        std::vector<char> src(700);
        std::vector<uint8_t> dst(700);
        for (int round = 0; round < 5; round++) {
            for (int i = 0; i < 700; i++) src[i] = char('a' + ((round + i) % 26));
            tx.enqueue(src.data(), src.size());
            check(tx.peek(dst.data(), dst.size()) == 700, "wrapped peek count");
            check(std::memcmp(dst.data(), src.data(), 700) == 0, "wrapped TX bytes intact");
            tx.commit(700);
        }
        check(tx.empty(), "FIFO drains after the wrapping rounds");
    }
}

// --- 5. value coercion ----------------------------------------------------------------------------

void test_coercion() {
    std::printf("coercion\n");
    float f = -1.f;
    check(parse_f32("0.5", f) && f == 0.5f, "parse_f32 decimal");
    check(parse_f32("-2", f) && f == -2.f, "parse_f32 negative");
    check(!parse_f32("0.5x", f), "parse_f32 rejects trailing garbage");
    check(!parse_f32("", f), "parse_f32 rejects empty");
    check(!parse_f32("abc", f), "parse_f32 rejects non-numeric");
    check(!parse_f32("inf", f), "parse_f32 rejects inf");
    check(!parse_f32("nan", f), "parse_f32 rejects nan");

    int32_t i = -1;
    check(parse_i32("42", i) && i == 42, "parse_i32 decimal");
    check(parse_i32("-7", i) && i == -7, "parse_i32 negative");
    check(parse_i32("0x10", i) && i == 16, "parse_i32 hex (base 0)");
    check(!parse_i32("12ab", i), "parse_i32 rejects trailing garbage");
    check(!parse_i32("", i), "parse_i32 rejects empty");

    DeckRef::Ref d = DeckRef::Count;
    check(parse_deck("A", d) && d == DeckRef::A, "parse_deck A");
    check(parse_deck("a", d) && d == DeckRef::A, "parse_deck a");
    check(parse_deck("B", d) && d == DeckRef::B, "parse_deck B");
    check(parse_deck("b", d) && d == DeckRef::B, "parse_deck b");
    check(!parse_deck("AA", d), "parse_deck rejects multi-char");
    check(!parse_deck("C", d), "parse_deck rejects an unknown deck");
    check(!parse_deck("", d), "parse_deck rejects empty");

    bool on = false;
    check(parse_onoff("on", on) && on, "parse_onoff on");
    check(parse_onoff("1", on) && on, "parse_onoff 1");
    check(parse_onoff("off", on) && !on, "parse_onoff off");
    check(parse_onoff("0", on) && !on, "parse_onoff 0");
    check(!parse_onoff("maybe", on), "parse_onoff rejects anything else");

    // Name table + the numeric-id fallback the dispatch spec promises for a minimal build.
    ParamId pid;
    check(param_from_token("size", pid) && pid == ParamId::Size, "param name lookup");
    check(param_from_token("fluxint", pid) && pid == ParamId::FluxIntensity, "abbreviated param name");
    check(param_from_token("4", pid) && pid == ParamId::Size, "numeric param id fallback");
    check(!param_from_token("nosuch", pid), "unknown param name rejected");
    check(!param_from_token("99", pid), "out-of-range numeric param id rejected");
    ConfigId cid;
    check(config_from_token("mode", cid) && cid == ConfigId::Mode, "config name lookup");
    check(config_from_token("0", cid) && cid == ConfigId::Route, "numeric config id fallback");
    check(!config_from_token("nosuch", cid), "unknown config name rejected");
}

// --- 6. reply formatting --------------------------------------------------------------------------

void test_formatting() {
    std::printf("formatting\n");
    check_eq(fmt_f32(0.75f), "0.7500", "float formats to 4 decimals without %f");
    check_eq(fmt_f32(-0.5f), "-0.5000", "negative float");
    check_eq(fmt_f32(0.f), "0.0000", "zero");
    check_eq(fmt_f32(0.0625f), "0.0625", "fraction zero-padding");
    check_eq(fmt_f32(0.99999f), "1.0000", "rounding carries into the integer part");
    check_eq(fmt_f32(300.f), "300.0000", "value above 1 keeps its integer part");
    check_eq(fmt_f32(1.f, 0), "1", "zero decimals emits no point");
    check_eq(fmt_f32(std::nanf("")), "nan", "nan is reported, not garbage");
    check_eq(fmt_f32(INFINITY), "inf", "inf is reported");
    check_eq(fmt_f32(-INFINITY), "-inf", "-inf is reported");

    {
        StringOut o; TextSink(o).append_i32(0);            check_eq(o.s, "0", "int zero"); }
    {
        StringOut o; TextSink(o).append_i32(-1);           check_eq(o.s, "-1", "int negative"); }
    {
        StringOut o; TextSink(o).append_i32(INT32_MIN);    check_eq(o.s, "-2147483648", "INT32_MIN"); }
    {
        StringOut o; TextSink(o).append_hex(0);            check_eq(o.s, "0x0", "hex zero"); }
    {
        StringOut o; TextSink(o).append_hex(0x133);        check_eq(o.s, "0x133", "hex has no leading zeros"); }
    {
        StringOut o; TextSink(o).append_hex(0xdeadbeef);   check_eq(o.s, "0xdeadbeef", "hex is lowercase"); }

    // Framing: the harness greps on these exact shapes.
    { StringOut o; TextSink(o).ok();               check_eq(o.s, "ok\r\n", "bare ok framing"); }
    { StringOut o; TextSink(o).err("bad-deck");    check_eq(o.s, "err bad-deck\r\n", "err framing"); }
    { StringOut o; TextSink(o).ok_i32(1);          check_eq(o.s, "ok 1\r\n", "ok <int> framing"); }
    { StringOut o; TextSink(o).line("end");        check_eq(o.s, "end\r\n", "line framing"); }
}

// --- 7. dispatch ----------------------------------------------------------------------------------

void test_dispatch_stimulus() {
    std::printf("dispatch: stimulus\n");
    MockEngine e;

    check_eq(run(e, "set param size A 0.5"), "ok\r\n", "set param reply");
    check_eq(e.last(), "set_param size 0 0.5000", "set param reaches the engine");
    check_eq(run(e, "get param size A"), "ok 0.5000\r\n", "get param round-trips");
    check_eq(run(e, "set param size B 0.25"), "ok\r\n", "set param deck B");
    check_eq(run(e, "get param size B"), "ok 0.2500\r\n", "decks are independent");
    check_eq(run(e, "set param 4 A 0.125"), "ok\r\n", "numeric param id accepted");
    check_eq(run(e, "get param size A"), "ok 0.1250\r\n", "numeric id addressed the same param");

    check_eq(run(e, "set modspeed A 0.3"), "ok\r\n", "set modspeed reply");
    check_eq(e.last(), "set_mod_speed 0 0.3000 0", "modspeed defaults to unsynced");
    check_eq(run(e, "set modspeed B 0.3 sync"), "ok\r\n", "set modspeed sync reply");
    check_eq(e.last(), "set_mod_speed 1 0.3000 1", "the sync flag is forwarded");

    check_eq(run(e, "config mode A 1"), "ok 1\r\n", "config echoes changed=1");
    check_eq(e.last(), "set_config mode 0 1", "config reaches the engine");
    e.config_changed = false;
    check_eq(run(e, "config mode A 1"), "ok 0\r\n", "config echoes changed=0 (idempotence assertable)");
    e.config_changed = true;

    check_eq(run(e, "cv voct A 1.0"), "ok\r\n", "cv voct reply");
    check_eq(e.last(), "cv_voct 0 1.0000", "cv voct binding");
    check_eq(run(e, "cv mix B 0.2"), "ok\r\n", "cv mix reply");
    check_eq(e.last(), "cv_mix 1 0.2000", "cv mix binding");
    check_eq(run(e, "cv size A 0.4"), "ok\r\n", "cv size reply");
    check_eq(e.last(), "cv_size_pos 0 0.4000", "cv size binding");
    check_eq(run(e, "cv xfade A 0.7"), "ok\r\n", "cv xfade reply");
    check_eq(e.last(), "cv_crossfade 0.7000", "cv xfade is global (deck ignored)");

    check_eq(run(e, "gate B"), "ok\r\n", "gate reply");
    check_eq(e.last(), "on_gate_trigger 1", "gate binding");

    check_eq(run(e, "midi note 1 60"), "ok\r\n", "midi note reply");
    check_eq(e.last(), "midi_note 1 60", "midi note binding");
    check_eq(run(e, "midi msg 144 60 100"), "ok\r\n", "midi msg reply");
    check_eq(e.last(), "midi_msg 144 60 100", "midi msg binding");
    check_eq(run(e, "midi transport start"), "ok\r\n", "midi transport start reply");
    check_eq(e.last(), "midi_transport 1", "midi transport binding");
    check_eq(run(e, "midi transport stop"), "ok\r\n", "midi transport stop reply");
    check_eq(e.last(), "midi_transport 0", "midi transport stop binding");

    check_eq(run(e, "pad play A"), "ok empty=1\r\n", "pad play reports emptiness");
    check_eq(e.last(), "on_play_pad 0 0", "pad play binding");
    e.play_pad_empty = false;
    check_eq(run(e, "pad play A rev"), "ok empty=0\r\n", "pad play rev reply");
    check_eq(e.last(), "on_play_pad 0 1", "the rev flag is forwarded");
    check_eq(run(e, "pad rec B"), "ok\r\n", "pad rec reply");
    check_eq(e.last(), "on_record_pad 1 0", "pad rec binding");
    check_eq(run(e, "pad seq A"), "ok\r\n", "pad seq reply");
    check_eq(e.last(), "on_seq_trigger 0", "pad seq binding");
    check_eq(run(e, "pad stop A"), "ok\r\n", "pad stop reply");
    check_eq(e.last(), "stop_if_generating 0", "pad stop binding");
    check_eq(run(e, "pad clear A"), "ok\r\n", "pad clear reply");
    check_eq(e.last(), "clear_buffer 0", "pad clear binding");

    check_eq(run(e, "fx flux A on"), "ok\r\n", "fx on reply");
    check_eq(e.last(), "set_fx 0 flux 1", "fx flux binding");
    check_eq(run(e, "fx grit B off"), "ok\r\n", "fx off reply");
    check_eq(e.last(), "set_fx 1 grit 0", "fx grit binding");
    check_eq(run(e, "fx lock flux A"), "ok\r\n", "fx lock reply");
    check_eq(e.last(), "toggle_fx_lock 0 flux", "fx lock binding");
    check_eq(run(e, "fx gritmode B"), "ok intensity=0.2500 mix=0.7500\r\n",
             "an action CAN return values - gritmode reports the reseed pair");
    check_eq(e.last(), "toggle_grit_mode 1", "fx gritmode binding");

    check_eq(run(e, "seq trig A"), "ok\r\n", "seq trig reply");
    check_eq(e.last(), "on_seq_trigger 0", "seq trig binding (synonym of pad seq)");
    check_eq(run(e, "seq arm B"), "ok\r\n", "seq arm reply");
    check_eq(e.last(), "on_seq_toggle_arm 1", "seq arm binding");
    check_eq(run(e, "seq clear A"), "ok\r\n", "seq clear reply");
    check_eq(e.last(), "clear_sequence 0", "seq clear binding");
    check_eq(run(e, "seq disarm A"), "ok\r\n", "seq disarm reply");
    check_eq(e.last(), "disarm_track 0", "seq disarm binding");
    check_eq(run(e, "seq nosuch A"), "err bad-arg\r\n", "unknown seq action");
}

// --- composite verbs: reset / preset ---------------------------------------------------------------

void test_dispatch_composites() {
    std::printf("dispatch: composites (reset / preset)\n");
    MockEngine e;
    // Narrow the mask so the counts below are exact and readable: 2 deck params + 1 global.
    e.pmask = (1u << uint32_t(ParamId::Size))
            | (1u << uint32_t(ParamId::Feedback))
            | (1u << uint32_t(ParamId::Crossfade));   // global -> deck A slot only

    // 2 deck params x 2 decks + 1 global x 1 = 5 writes.
    check_eq(run(e, "reset"), "ok 5\r\n", "reset writes every advertised param, globals once");
    check_eq(run(e, "get param size A"), "ok 0.2500\r\n", "reset used the engine's declared default");
    check_eq(run(e, "get param size B"), "ok 0.2500\r\n", "reset covered deck B");

    check_eq(run(e, "reset A"), "ok 3\r\n", "reset <deck> touches one deck (plus globals)");
    check_eq(run(e, "reset Z"), "err bad-deck\r\n", "reset rejects a bad deck");

    // Snapshot, perturb, restore - the whole point of the pair. These MUST share one TermState: the
    // slots live there (one Terminal, one state, for the life of the firmware), so the convenience
    // run() overload - which makes a fresh state per call - would silently lose the saved slot.
    TermState st;
    check_eq(run(e, st, "preset save 0"), "ok 5\r\n", "preset save captures the advertised params");
    check_eq(run(e, st, "set param size A 0.9"), "ok\r\n", "perturb");
    check_eq(run(e, st, "get param size A"), "ok 0.9000\r\n", "perturbed");
    check_eq(run(e, st, "preset load 0"), "ok 5\r\n", "preset load restores");
    check_eq(run(e, st, "get param size A"), "ok 0.2500\r\n", "the perturbation was undone");

    check_eq(run(e, st, "preset load 1"), "ok 0\r\n", "loading a never-saved slot restores nothing");
    check_eq(run(e, st, "preset save 9"), "err bad-arg\r\n", "slot out of range");
    check_eq(run(e, st, "preset nosuch 0"), "err bad-arg\r\n", "unknown preset action");
    check_eq(run(e, st, "preset save"), "err no-arg\r\n", "preset needs a slot");

    // A masked-out param must be untouched by either composite - they operate on what describe shows.
    e.params[static_cast<size_t>(ParamId::Pos)][0] = 0.77f;
    run(e, "reset");
    check_eq(run(e, "get param pos A"), "ok 0.7700\r\n",
             "reset leaves params outside the liveness mask alone");
}

void test_dispatch_observation() {
    std::printf("dispatch: observation\n");
    MockEngine e;
    TermState  st;

    check_eq(run(e, "query empty A"), "ok 0\r\n", "query empty A");
    check_eq(run(e, "query empty B"), "ok 1\r\n", "query empty B");
    check_eq(run(e, "query mix"), "ok 0.2500\r\n", "query mix");
    check_eq(run(e, "query route"), "ok 2\r\n",
             "query route reports the SELECTOR encoding config route accepts, not the Route enum");
    check_eq(run(e, "query gateout A"), "ok 1\r\n", "query gateout A");
    check_eq(run(e, "query gateout B"), "ok 0\r\n", "query gateout B");
    check_eq(run(e, "caps"), "ok 0x133\r\n", "caps reports the capability mask in hex");

    const std::string usb = run(e, "query usb");
    check(usb.rfind("ok ", 0) == 0 && usb.size() > 4, "query usb replies ok with fields");
    for (const char* key : { "boot=", "region=", "clkcfg=", "hsi48=", "usbsel=",
                             "usb33den=", "usb33rdy=", "phy=", "pullup=",
                             "vbussense=", "dp=", "dm=", "rst=", "sof=" })
        check(contains(usb, key), "query usb reports every probe field");

    check_eq(run(e, st, "mode test"), "ok\r\n", "mode test reply");
    check(st.test_mode, "mode test sets the isolation flag");
    check_eq(run(e, st, "mode run"), "ok\r\n", "mode run reply");
    check(!st.test_mode, "mode run clears the isolation flag");
    check_eq(run(e, st, "mode wat"), "err bad-arg\r\n", "unknown mode rejected");

    check(contains(run(e, "help"), "describe"), "help lists the verb set");

    check_eq(run(e, "query recorded A"),  "ok 4096\r\n",  "query recorded");
    check_eq(run(e, "query capacity A"),  "ok 65536\r\n", "query capacity");
    check_eq(run(e, "query layout A"),    "ok 1\r\n",     "query layout reports the DeckLayout enum");
    check_eq(run(e, "query sizetempo A"), "ok 1\r\n",     "query sizetempo");
    check_eq(run(e, "query fit A 0.5"),   "ok 60.0000\r\n", "query fit takes an argument");
    check_eq(run(e, "query fit A"),       "err no-arg\r\n", "query fit without its argument errors");

    // The latching read: asking changes the answer. This is why it must never be advertised.
    check_eq(run(e, "query reseed A"), "ok 1\r\n", "reseed reports a pending reseed");
    check_eq(run(e, "query reseed A"), "ok 0\r\n", "reseed self-cleared - the read had a side effect");
}

void test_dispatch_errors() {
    std::printf("dispatch: error taxonomy\n");
    MockEngine e;
    const size_t before = e.log.size();

    check_eq(run(e, "wat"), "err unknown-verb\r\n", "unknown verb");
    check_eq(run(e, "set param nosuch A 0.5"), "err unknown-param\r\n", "unknown param name");
    check_eq(run(e, "config nosuch A 1"), "err unknown-config\r\n", "unknown config name");
    check_eq(run(e, "set param size C 0.5"), "err bad-deck\r\n", "bad deck token");
    check_eq(run(e, "set param size A xyz"), "err bad-arg\r\n", "unparseable float");
    check_eq(run(e, "set param size A"), "err no-arg\r\n", "missing value");
    check_eq(run(e, "set"), "err no-arg\r\n", "verb with no subcommand");
    check_eq(run(e, "get param size"), "err bad-arg\r\n", "get with too few args");
    check_eq(run(e, "gate"), "err no-arg\r\n", "gate without a deck");
    check_eq(run(e, "cv nosuch A 1.0"), "err bad-arg\r\n", "unknown cv channel");
    check_eq(run(e, "fx flux A maybe"), "err bad-arg\r\n", "unparseable on/off");
    check_eq(run(e, "pad nosuch A"), "err bad-arg\r\n", "unknown pad action");
    check_eq(run(e, "midi nosuch 1 2"), "err bad-arg\r\n", "unknown midi form");
    check_eq(run(e, "a b c d e f g"), "err too-many-args\r\n", "too many tokens");

    check(e.log.size() == before, "no rejected command reached the engine");

    // A blank line is not an error and must not produce a reply, or the synchronous host would fall
    // out of step (one reply per command).
    check_eq(run(e, ""), "", "blank line produces no reply at all");
    check_eq(run(e, "   "), "", "whitespace-only line produces no reply");
}

void test_dispatch_target_b() {
    std::printf("dispatch: target B (declared engine queries)\n");
    MockEngine e;

    // With no table, an engine query name is just an unknown verb.
    check_eq(run(e, "query loop_ms A"), "err unknown-verb\r\n", "no table -> unknown");
    check(!contains(run(e, "describe"), "query loop_ms"), "no table -> nothing advertised");
    check_eq(run(e, "caps"), "ok 0x133\r\n", "no table -> CapTerminal not set");

    e.declare_queries = true;

    check_eq(run(e, "query loop_ms A"), "ok 125.0000\r\n", "deck-scoped engine query");
    check_eq(run(e, "query loop_ms B"), "ok 250.0000\r\n", "the platform passes the right deck");
    check_eq(run(e, "query state"),     "ok 1\r\n",        "global engine query needs no deck");
    check_eq(run(e, "query armed A"),   "ok 1\r\n",        "bool kind");
    check_eq(run(e, "query grains"),    "ok 42\r\n",       "int kind");
    check_eq(run(e, "query label"),     "ok tape-a\r\n",   "text kind");

    // The platform does the validation, so the engine writes none.
    check_eq(run(e, "query loop_ms"),  "err no-arg\r\n",  "deck-scoped query without a deck");
    check_eq(run(e, "query loop_ms Z"), "err bad-deck\r\n", "deck-scoped query with a bad deck");

    // Platform names win, so an engine cannot shadow the reflective surface.
    check_eq(run(e, "query mix"), "ok 0.2500\r\n", "a colliding engine name does NOT shadow the platform");

    // CapTerminal is derived from the table, not hand-set.
    check_eq(run(e, "caps"), "ok 0x533\r\n", "declaring queries advertises CapTerminal (0x400)");

    const std::string d = run(e, "describe");
    check(contains(d, "query loop_ms deck float\r\n"),  "engine query advertised with scope + kind");
    check(contains(d, "query state global enum 0:stopped 1:playing\r\n"), "enum labels advertised");
    check(contains(d, "query empty deck bool\r\n"),     "platform queries now carry a kind too");
    check(!contains(d, "query latch"), "an unsafe entry is never advertised");
    check(!contains(d, "query reseed"), "the platform's own latching read stays unadvertised");
    check(count_lines_with(d, "query ") == 9 + 6,
          "describe emits both halves: 9 safe platform + 6 safe engine");

    // Unsafe entries stay reachable by name - they are simply not offered to a generic host.
    check_eq(run(e, "query latch A"), "ok 1\r\n", "an unsafe query still answers when asked directly");
}

void test_dispatch_engine_verbs() {
    std::printf("dispatch: engine-specific verbs (target B)\n");
    MockEngine e;

    check_eq(run(e, "query loop_ms A"), "err unknown-verb\r\n",
             "an unknown query falls through to handle_command, then errors");
    e.custom = true;
    check_eq(run(e, "query loop_ms A"), "ok 123.5000\r\n", "the engine's own query is used when recognized");
    check_eq(run(e, "query loop_ms Z"), "err bad-deck\r\n",
             "handle_command owning the reply includes its error replies");
    check_eq(run(e, "query nosuch A"), "err unknown-verb\r\n",
             "a query the engine also declines still errors");

    // A platform verb must win over the engine's handler, so every engine keeps the reflective surface.
    check_eq(run(e, "query mix"), "ok 0.2500\r\n", "platform query is not shadowed by the engine");
}

// --- 8. describe ----------------------------------------------------------------------------------

std::string describe_block(MockEngine& e) {
    return run(e, "describe");
}

void test_describe() {
    std::printf("describe\n");
    MockEngine e;

    {
        // A narrow liveness mask must narrow the descriptor: this is the whole point of live_params,
        // and without it a generic host sweep sets params the engine ignores.
        e.pmask = (1u << uint32_t(ParamId::Size)) | (1u << uint32_t(ParamId::Feedback));
        e.cmask = static_cast<IEngine::ConfigMask>(1u << uint32_t(ConfigId::Mode));
        const std::string d = describe_block(e);
        check(contains(d, "masked=1"), "a narrowed mask reports masked=1");
        check(count_lines_with(d, "param ") == 2, "only live params are listed");
        check(count_lines_with(d, "config ") == 1, "only live configs are listed");
        check(contains(d, "param size deck 0..1\r\n"), "param line: name, scope, range");
        check(contains(d, "param feedback deck 0..1\r\n"), "second live param listed");
        check(!contains(d, "param pos "), "a masked-out param is absent");
        check(contains(d, "config mode 0:slice 1:reel 2:drift\r\n"), "config line carries its labels");
    }
    {
        e.pmask = ~uint32_t{0};
        e.cmask = static_cast<IEngine::ConfigMask>(~0u);
        const std::string d = describe_block(e);
        check(d.rfind("descr engine=", 0) == 0, "block opens with the descr line");
        check(contains(d, "version="), "descr line carries the version");
        check(contains(d, "masked=0"), "all-live masks report masked=0 so a host sweep can skip");
        check(d.size() >= 5 && d.compare(d.size() - 5, 5, "end\r\n") == 0,
              "block is terminated by `end` so the host knows it is complete");
        check(contains(d, "caps 0x133\r\n"), "caps line uses the same hex form as the caps verb");
        // Three ids the platform never forwards to set_param must never be advertised, whatever the
        // engine's mask says - otherwise a generic sweep sets them and asserts on a value that went
        // nowhere. See param_is_platform_owned().
        check(count_lines_with(d, "param ") == int(ParamId::Count) - 3,
              "all-live mask lists every param EXCEPT the platform-owned ones");
        check(!contains(d, "param tempo "),       "tempo is not advertised (Transport owns it)");
        check(!contains(d, "param keyinterval "), "keyinterval is not advertised (Transport owns it)");
        check(!contains(d, "param modspeed "),    "modspeed is not advertised (set_mod_speed is its path)");
        check(contains(d, "param crossfade global "), "crossfade IS advertised - it does reach set_param");
        check(count_lines_with(d, "config ") == int(ConfigId::Count), "all-live mask lists every config");
        check(count_lines_with(d, "query ") == 9, "the platform query vocabulary is enumerated");
        for (const char* q : { "query recorded deck", "query capacity deck",
                               "query layout deck", "query sizetempo deck" })
            check(contains(d, q), "the new safe state queries are advertised");
        // The safe-to-call rule, enforced: a parameterized query and a latching one stay out of the
        // descriptor, so the generic sweep - which calls everything it can see - cannot reach them.
        check(!contains(d, "query fit"),    "fit is not advertised (takes an argument)");
        check(!contains(d, "query reseed"), "reseed is not advertised (latching read)");

        // Scope tags: the descriptor is what tells a host whether to sweep one deck or two.
        check(contains(d, "param clickmix global "), "a global param is tagged global");
        check(contains(d, "param pos deck "), "a per-deck param is tagged deck");

        // Every line must be CRLF-framed: the host reads by line and logs share the stream.
        for (size_t i = 0; i < d.size(); i++)
            if (d[i] == '\n')
                check(i > 0 && d[i - 1] == '\r', "every LF in describe is preceded by CR");

        // Hand the exact bytes to tools/test_descriptor.py so the host parser is tested against real
        // firmware output rather than a hand-written sample.
        if (FILE* f = std::fopen("build/describe_sample.txt", "wb")) {
            std::fwrite(d.data(), 1, d.size(), f);
            std::fclose(f);
            std::printf("  wrote build/describe_sample.txt (%zu bytes)\n", d.size());
        } else {
            std::printf("  note: could not write build/describe_sample.txt (run from host/)\n");
        }
    }
}

}  // namespace

int main() {
    std::printf("== terminal channel (off-target) ==\n");
    test_tokenizer();
    test_line_assembler();
    test_rx_ring();
    test_tx_fifo();
    test_coercion();
    test_formatting();
    test_dispatch_stimulus();
    test_dispatch_composites();
    test_dispatch_observation();
    test_dispatch_errors();
    test_dispatch_target_b();
    test_dispatch_engine_verbs();
    test_describe();

    if (g_failures == 0) { std::printf("OK: all terminal checks passed\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
