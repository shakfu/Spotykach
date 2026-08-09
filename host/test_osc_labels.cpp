// Off-target check on the per-engine `param_label()` tables, against the REAL engines.
//
// `docs/dev/terminal-osc.md` puts the OSC address on the layer-2 slot (`/sk/a/param/size`) and sends
// the engine's own word for that slot as a cosmetic label in `describe`. That split is only worth
// anything if the labels are actually right, and they are hand-maintained tables - the one part of the
// design the spec admits can rot. This is what keeps them honest.
//
// Radio and tape are the two engines that implement labels, and they are the two the spec names,
// because they are the ones whose slots are most misleadingly inherited from granular's vocabulary:
// radio's PITCH is a tuning dial, tape's `Size` is an FX character control.
//
// Three properties, all read out of a real describe bundle:
//   1. Each labelled slot advertises the word the engine's own documentation uses.
//   2. Every live slot carries SOME label (the fallback works), and every advertised address is
//      composed exactly as the spec's rules predict.
//   3. No two live slots on one engine share a label - a collision is legal (the host translator
//      disambiguates with a slot-name suffix) but it produces uglier semantic addresses, so it should
//      be a deliberate choice rather than an accident.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "engine/iengine.h"
#include "engine/bard/bard_engine.h"
#include "engine/delay/delay_engine.h"
#include "engine/edrums/edrums_engine.h"
#include "engine/glitch/glitch_engine.h"
#include "engine/qdelay/qdelay_engine.h"
#include "engine/radio/radio_engine.h"
#include "engine/reso/reso_engine.h"
#include "engine/reverb/reverb_engine.h"
#include "engine/shuttle/shuttle_engine.h"
#include "engine/softcut/softcut_engine.h"
#include "engine/tape/tape_engine.h"
#include "osc_test_util.h"
#include "terminal/names.h"
#include "terminal/osc_addr.h"
#include "terminal/osc_sink.h"
#include "terminal/term_state.h"

using namespace spotykach;

namespace {

int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); g_failures++; }
}

// Ask an engine to describe itself over OSC and return the param rows as
// { generic address -> label }. describe reads only the liveness masks, the labels and capabilities,
// so the engine needs no init() and no arena - which is what keeps this test cheap.
std::map<std::string, std::string> param_labels(IEngine& e) {
    osctest::StringOut out;
    OscSink   sink(out);
    TermState st;

    // "/sk/dev/describe" with no type-tag string: a read.
    const char* addr = "/sk/dev/describe";
    uint8_t pkt[24] = {};
    size_t n = 0;
    while (*addr) pkt[n++] = static_cast<uint8_t>(*addr++);
    pkt[n++] = 0;
    while (n & 3u) pkt[n++] = 0;
    osc_dispatch_packet(pkt, n, e, sink, st);

    std::map<std::string, std::string> out_map;
    for (const auto& r : osctest::decode_rows(out.s)) {
        if (r.addr != "/sk/reply/dev/describe/param") continue;
        // args render as: ,s <address> ,s <label> ,f lo ,f hi ,s scope
        const size_t a0 = r.args.find(",s ") + 3;
        const size_t a1 = r.args.find(" ,s ", a0);
        const size_t l0 = a1 + 4;
        const size_t l1 = r.args.find(" ,f ", l0);
        if (a1 == std::string::npos || l1 == std::string::npos) continue;
        out_map[r.args.substr(a0, a1 - a0)] = r.args.substr(l0, l1 - l0);
    }
    return out_map;
}

void expect(const std::map<std::string, std::string>& m, const char* addr, const char* label) {
    auto it = m.find(addr);
    if (it == m.end()) {
        check(false, std::string("no describe row for ") + addr);
        return;
    }
    if (it->second != label)
        check(false, std::string(addr) + ": want label \"" + label + "\", got \"" + it->second + "\"");
}

// A label is never empty, and a labelled slot must not silently keep the layer-2 name when the engine
// meant to rename it. Also checks the deck expansion: a deck-scoped slot appears on BOTH decks.
void check_shape(const std::map<std::string, std::string>& m, const char* engine) {
    for (const auto& kv : m) {
        check(!kv.second.empty(), std::string(engine) + ": empty label on " + kv.first);
        check(kv.first.rfind("/sk/", 0) == 0, std::string(engine) + ": odd address " + kv.first);
        if (kv.first.compare(0, 6, "/sk/a/") == 0) {
            std::string b = kv.first;
            b[4] = 'b';
            check(m.count(b) == 1, std::string(engine) + ": " + kv.first + " has no deck-b twin");
            check(m.count(b) && m.at(b) == kv.second,
                  std::string(engine) + ": decks disagree on the label for " + kv.first);
        }
    }
}

// Two slots sharing a label is legal - the host translator suffixes with the slot name - but it makes
// the semantic address uglier, so it should never happen by accident.
void check_no_collisions(const std::map<std::string, std::string>& m, const char* engine) {
    std::map<std::string, std::string> seen;   // label -> first address, deck A / globals only
    for (const auto& kv : m) {
        if (kv.first.compare(0, 6, "/sk/b/") == 0) continue;   // the b rows are the same labels again
        auto it = seen.find(kv.second);
        if (it != seen.end())
            check(false, std::string(engine) + ": label \"" + kv.second + "\" is shared by "
                         + it->second + " and " + kv.first);
        else
            seen[kv.second] = kv.first;
    }
}

void test_radio() {
    std::printf("radio param labels\n");
    RadioEngine e;
    const auto m = param_labels(e);

    // The spec's own worked example: PITCH is the tuning dial on this engine, and the address that
    // carries it is still the generic /sk/a/param/speed that every other build uses for PITCH.
    expect(m, "/sk/a/param/speed", "station");
    expect(m, "/sk/b/param/speed", "station");
    expect(m, "/sk/a/param/pos",   "start");
    expect(m, "/sk/a/param/size",  "varispeed");
    expect(m, "/sk/a/param/env",   "static");
    expect(m, "/sk/a/param/mix",   "volume");
    expect(m, "/sk/a/param/aux",   "bank");
    // Crossfade is deliberately unlabelled: the platform crossfader means the same thing on every
    // engine, so a per-engine label would be pure rot risk. Same on every engine below.
    expect(m, "/sk/param/crossfade", "crossfade");
    check(m.size() == 6 * 2 + 1, "radio advertises 6 deck-scoped params x 2 decks + crossfade");
}

void test_tape() {
    std::printf("tape param labels\n");
    TapeEngine e;
    const auto m = param_labels(e);

    // Tape is the sharpest case for the address/label split: six of these slots carry a granular word
    // that says nothing about what tape does with them.
    expect(m, "/sk/a/param/speed",   "varispeed");
    expect(m, "/sk/a/param/altpos",  "pan");
    expect(m, "/sk/a/param/mix",     "volume");
    expect(m, "/sk/a/param/env",     "loop mode");
    expect(m, "/sk/a/param/aux",     "tape slot");
    expect(m, "/sk/a/param/pos",     "drive");             // NOT a position
    expect(m, "/sk/a/param/size",    "character");         // NOT a size - tape_engine.cpp:128
    expect(m, "/sk/a/param/modamp",  "wow/flutter depth");
    expect(m, "/sk/a/param/gritint", "filter cutoff");     // the grit slots are the low-pass
    expect(m, "/sk/a/param/gritmix", "filter resonance");
    expect(m, "/sk/param/crossfade", "crossfade");
    check(m.size() == 10 * 2 + 1, "tape advertises 10 deck-scoped params x 2 decks + crossfade");

    // modspeed is the wow/flutter RATE, but it reaches the engine through set_mod_speed() rather than
    // set_param, so it is platform-owned and has no param address at all - it lives at
    // /sk/<deck>/modspeed instead. Assert that, or a host would look for a label that cannot exist.
    check(m.count("/sk/a/param/modspeed") == 0,
          "modspeed has no param address (set_mod_speed is its path)");
}

// A spot-check per engine on the label that would be most misleading if it were absent - the slot
// whose granular-inherited name says least about what the engine does with it.
void test_other_engines() {
    std::printf("remaining engines: spot-checks\n");
    { DelayEngine e;   const auto m = param_labels(e);
      expect(m, "/sk/a/param/pos",  "feedback");        // POS is the feedback amount
      expect(m, "/sk/a/param/size", "division"); }
    { QdelayEngine e;  const auto m = param_labels(e);
      expect(m, "/sk/a/param/pos",  "feedback");
      expect(m, "/sk/a/param/env",  "tone"); }
    { ReverbEngine e;  const auto m = param_labels(e);
      // The labels are the docs table's ROLE column, which is stable across plate/hall/greyhole; any
      // one algorithm's own parameter name would be wrong for the other two.
      expect(m, "/sk/a/param/speed", "decay");
      expect(m, "/sk/a/param/pos",   "tone"); }
    { ResoEngine e;    const auto m = param_labels(e);
      expect(m, "/sk/a/param/size",   "damping");       // Rings' own patch fields
      expect(m, "/sk/a/param/modamp", "structure"); }
    { EdrumsEngine e;  const auto m = param_labels(e);
      expect(m, "/sk/a/param/pos",  "density");         // a drum sequencer: POS is onset density
      expect(m, "/sk/a/param/size", "pattern length"); }
    { GlitchEngine e;  const auto m = param_labels(e);
      // Named generically ON PURPOSE: the two macros mean something different per algorithm, so a
      // fixed label would be wrong eleven times out of twelve.
      expect(m, "/sk/a/param/size", "param 1");
      expect(m, "/sk/a/param/aux",  "algorithm"); }
    { ShuttleEngine e; const auto m = param_labels(e);
      expect(m, "/sk/a/param/pos",     "loop start");
      expect(m, "/sk/a/param/gritmix", "character"); }
    { SoftcutEngine e; const auto m = param_labels(e);
      expect(m, "/sk/a/param/env",   "overdub feedback");   // ENV is not an envelope here
      expect(m, "/sk/a/param/speed", "rate"); }
    { BardEngine e;    const auto m = param_labels(e);
      expect(m, "/sk/a/param/speed", "book");               // PITCH browses books
      expect(m, "/sk/a/param/pos",   "bookmark");
      expect(m, "/sk/a/param/env",   "pitch keep"); }
}

// The invariants every labelled engine must satisfy, enforced across all of them at once so a new
// table cannot land without meeting them.
void test_all_engines_invariants() {
    std::printf("all engines: shape + collisions\n");
    RadioEngine radio; TapeEngine tape; DelayEngine delay; QdelayEngine qdelay;
    ReverbEngine reverb; ResoEngine reso; EdrumsEngine edrums; GlitchEngine glitch;
    ShuttleEngine shuttle; SoftcutEngine softcut; BardEngine bard;
    struct Row { const char* name; IEngine* e; };
    const Row rows[] = {
        { "radio", &radio }, { "tape", &tape }, { "delay", &delay }, { "qdelay", &qdelay },
        { "reverb", &reverb }, { "reso", &reso }, { "edrums", &edrums }, { "glitch", &glitch },
        { "shuttle", &shuttle }, { "softcut", &softcut }, { "bard", &bard },
    };
    for (const Row& r : rows) {
        const auto m = param_labels(*r.e);
        check(!m.empty(), std::string(r.name) + " advertises no params at all");
        check_shape(m, r.name);
        check_no_collisions(m, r.name);
        // Crossfade is the platform crossfader: it must keep the layer-2 name on every engine, or the
        // "label what the engine reinterprets, not what it merely uses" rule has been broken.
        auto it = m.find("/sk/param/crossfade");
        if (it != m.end())
            check(it->second == "crossfade",
                  std::string(r.name) + " labelled the platform crossfader as \"" + it->second + "\"");
    }
}

// The universal-layout claim: the SAME layer-2 address set appears for the same live slots on every
// build, and only the labels differ. Radio and tape share five live slots; those five addresses must be
// identical strings, with different words on them.
void test_cross_engine_stability() {
    std::printf("cross-engine address stability\n");
    RadioEngine r; TapeEngine t;
    const auto rm = param_labels(r), tm = param_labels(t);

    for (const char* addr : { "/sk/a/param/speed", "/sk/a/param/pos", "/sk/a/param/size",
                              "/sk/a/param/env", "/sk/a/param/mix", "/sk/a/param/aux",
                              "/sk/param/crossfade" }) {
        check(rm.count(addr) == 1, std::string("radio advertises ") + addr);
        check(tm.count(addr) == 1, std::string("tape advertises ") + addr);
    }
    // ...and the point of the whole design: same address, different word.
    check(rm.at("/sk/a/param/size") != tm.at("/sk/a/param/size"),
          "one address, two engine meanings - which is why the meaning is not in the path");
    check(rm.at("/sk/a/param/speed") == "station" && tm.at("/sk/a/param/speed") == "varispeed",
          "PITCH is a tuning dial on radio and a varispeed on tape, at the same address");
}

}  // namespace

int main() {
    std::printf("== per-engine OSC param labels (off-target) ==\n");
    test_radio();
    test_tape();
    test_other_engines();
    test_all_engines_invariants();
    test_cross_engine_stability();

    if (g_failures == 0) { std::printf("OK: all label checks passed\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
