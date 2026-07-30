// Headless test for the bard engine (the bookmark-navigated storyteller). Three layers:
//   A. bookmarks.h  - the sidecar grammar (time formats, open-ended segments, directives, robustness),
//      the deterministic auto-marks, and the bard.cfg reader. Pure functions, no engine.
//   B. resume_table.h - the LRU resume table: set/get/promote/evict and the text round trip.
//   C. BardEngine through its public IEngine surface, driven by a fake IStreamDeck that serves a ramp and
//      records every open/seek, plus a controllable clock so the settle timers are exercised exactly.
//
// The controllable clock matters: bard inherits radio's settle/hysteresis anti-stutter guards, which are
// time-based, so a wall-clock time source would make those paths either untestable or flaky.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <utility>
#include <string>
#include <vector>

#include "engine/bard/bard_engine.h"
#include "engine/bard/bookmarks.h"
#include "engine/bard/resume_table.h"
#include "engine/bard/wsola.h"
#include "engine/istreamdeck.h"
#include "host_setup.h"

using namespace spotykach;

namespace {

int g_failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); g_failures++; }
}

// A clock the test drives, so the 180 ms selector settle and the 30 s resume checkpoint are exercised
// deterministically rather than by sleeping.
struct FakeTime : ITimeSource {
    uint32_t ms = 0;
    uint32_t now_ms() const override { return ms; }
    uint32_t now_us() const override { return ms * 1000u; }
    void advance(uint32_t d) { ms += d; }
};

// Fake stream deck: a shelf of books, an int16 ramp body (so a mis-seek is visible), a small text-file
// map for the sidecars / bard.cfg / resume.txt, and a record of every open + text write.
struct FakeStream : IStreamDeck {
    // shelf -> books
    struct Book { std::string name; uint32_t frames; bool is_wav; uint32_t rate; };
    std::map<int, std::vector<Book>> shelves;
    std::map<std::string, std::string> files;      // path -> text

    bool     playing[2] = { false, false };
    char     last_path[2][64] = {};
    uint32_t last_frame[2] = { 0, 0 };
    bool     last_loop[2] = { false, false };
    int      open_calls[2] = { 0, 0 };            // full close+open (a new file)
    int      seek_calls[2] = { 0, 0 };            // the light in-file f_lseek path
    int      jumps(int i) const { return open_calls[i] + seek_calls[i]; }
    uint32_t cursor[2] = { 0, 0 };                // ramp position, so play_consume mirrors a real seek
    bool     starve[2] = { false, false };        // simulate a ring underrun

    std::vector<std::pair<std::string, std::string>> writes;
    bool write_ok = true;

    uint32_t play_consume(DeckRef::Ref deck, uint8_t* dst, uint32_t n) override {
        const int i = (deck == DeckRef::A) ? 0 : 1;
        if (starve[i]) return 0;
        const uint32_t cnt = n / sizeof(int16_t);
        int16_t* s = reinterpret_cast<int16_t*>(dst);
        for (uint32_t k = 0; k < cnt; k++) {
            // Period 32 divides the 96-frame block, so at unity rate any two equal-length runs produce
            // identical samples; that is what makes the A/B processing comparisons below valid.
            s[k] = static_cast<int16_t>(static_cast<int>((cursor[i] + k) % 32u) * 500 - 8000);
        }
        cursor[i] += cnt;
        return n;
    }
    uint32_t record_produce(DeckRef::Ref, const uint8_t*, uint32_t n) override { return n; }
    bool is_playing(DeckRef::Ref deck)   const override { return playing[(deck == DeckRef::A) ? 0 : 1]; }
    bool is_recording(DeckRef::Ref)      const override { return false; }
    bool start_play(DeckRef::Ref, const char*)   override { return true; }
    bool start_record(DeckRef::Ref, const char*) override { return true; }
    void stop(DeckRef::Ref deck) override { playing[(deck == DeckRef::A) ? 0 : 1] = false; }
    void set_loop(DeckRef::Ref, bool) override {}
    uint32_t loop_frames(DeckRef::Ref) const override { return 0; }
    bool exists(const char* path) const override { return files.count(path) > 0; }

    bool _open(DeckRef::Ref deck, const char* path, uint32_t start_frame, bool loop) {
        const int i = (deck == DeckRef::A) ? 0 : 1;
        std::strncpy(last_path[i], path, sizeof(last_path[i]) - 1);
        last_frame[i] = start_frame; last_loop[i] = loop; open_calls[i]++;
        cursor[i] = start_frame;
        playing[i] = true;
        return true;
    }
    bool start_play_raw(DeckRef::Ref d, const char* p, uint32_t f, bool l) override { return _open(d, p, f, l); }
    bool start_play_wav(DeckRef::Ref d, const char* p, uint32_t f, bool l) override { return _open(d, p, f, l); }
    // The light path: only valid on a deck that is already playing, and it reuses the open handle.
    bool seek_play(DeckRef::Ref deck, uint32_t frame) override {
        const int i = (deck == DeckRef::A) ? 0 : 1;
        if (!playing[i]) return false;
        last_frame[i] = frame; seek_calls[i]++; cursor[i] = frame;
        return true;
    }
    uint32_t frames_of(const char*) const override { return 0; }

    int read_text(const char* path, char* buf, int max) const override {
        if (max <= 0) return 0;
        auto it = files.find(path);
        if (it == files.end()) { buf[0] = '\0'; return 0; }
        int i = 0;
        for (; it->second[i] && i < max - 1; i++) buf[i] = it->second[i];
        buf[i] = '\0';
        return i;
    }
    bool write_text(const char* path, const char* buf, int n) override {
        if (!write_ok) return false;
        writes.emplace_back(path, std::string(buf, buf + n));
        return true;
    }
    int scan_bank(const char* dir, BankEntry* out, int max) const override {
        // dir = "bard/<shelf>"
        const char* slash = std::strrchr(dir, '/');
        const int shelf = slash ? std::atoi(slash + 1) : 0;
        auto it = shelves.find(shelf);
        if (it == shelves.end()) return 0;
        int n = 0;
        for (const auto& b : it->second) {
            if (n >= max) break;
            std::strncpy(out[n].name, b.name.c_str(), sizeof(out[n].name) - 1);
            out[n].name[sizeof(out[n].name) - 1] = '\0';
            out[n].frames = b.frames;
            out[n].is_wav = b.is_wav;
            out[n].rate   = b.rate;
            n++;
        }
        bank_sort(out, n);
        return n;
    }
};

std::vector<float> run(BardEngine& e, int blocks, bool& finite) {
    float il[host::kBlock] = {0}, ir[host::kBlock] = {0}, ol[host::kBlock], orr[host::kBlock];
    const float* in[2] = { il, ir };
    float* out[2] = { ol, orr };
    std::vector<float> v;
    for (int b = 0; b < blocks; b++) {
        e.process(in, out, host::kBlock);
        for (size_t i = 0; i < host::kBlock; i++) {
            if (!std::isfinite(ol[i]) || !std::isfinite(orr[i])) finite = false;
            v.push_back(ol[i]);
        }
    }
    return v;
}
float peak(const std::vector<float>& v) { float p = 0.f; for (float x : v) p = std::fmax(p, std::fabs(x)); return p; }
float sad(const std::vector<float>& a, const std::vector<float>& b) {
    float s = 0.f; size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) s += std::fabs(a[i] - b[i]);
    return s;
}

// Settle a selector change: the guards need kSettleMs of stable target before they act.
void settle(BardEngine& e, FakeTime& t, int rounds = 3) {
    for (int k = 0; k < rounds; k++) { t.advance(200); e.prepare(); }
}

} // namespace

int main() {
    const uint32_t R = 48000;   // source rate used throughout

    // ================= A. the sidecar grammar =====================================================

    // A1. Time formats: SS, MM:SS, HH:MM:SS, a bare integer of seconds, and a fractional part.
    {
        bard::MarkList l;
        const char* txt =
            "0:00\n"
            "14:32\n"
            "1:02:11\n"
            "2841\n"
            "0:01.500\n";
        const int n = bard::parse_sidecar(txt, R, 0, l);
        check(n == 5, "sidecar: five timestamp lines accepted");
        check(l.mark[0].start == 0u, "time: 0:00 -> frame 0");
        check(l.mark[1].start == (14u * 60u + 32u) * R, "time: MM:SS is minutes:seconds");
        check(l.mark[2].start == (1u * 3600u + 2u * 60u + 11u) * R, "time: HH:MM:SS");
        check(l.mark[3].start == 2841u * R, "time: a bare integer is seconds");
        check(l.mark[4].start == R + R / 2u, "time: fractional .500 is milliseconds");
    }

    // A2. An explicit range closes the segment; a dash followed by prose is a LABEL, not a range
    // ("0:00 - Prologue" is the most natural thing for a human to type).
    {
        bard::MarkList l;
        const char* txt =
            "1:00-1:30   an explicit span\n"
            "2:00 - Prologue\n";
        bard::parse_sidecar(txt, R, 10u * 60u * R, l);
        bard::resolve(l, 10u * 60u * R, 1);
        check(l.count == 2, "sidecar: both lines accepted");
        check(l.mark[0].start == 60u * R && l.mark[0].end == 90u * R, "range: start-end closes the segment");
        check(l.mark[1].start == 120u * R && l.mark[1].end == 10u * 60u * R,
              "range: '- Prologue' is a label, so the segment stays open to the end of the book");
    }

    // A3. THE mechanism: a scrambled sidecar is a re-ordering of the book. Open-ended segments still
    // close at the next CHRONOLOGICAL mark, not the next line, so the spans stay well-defined.
    {
        bard::MarkList l;
        const char* txt = "0:30\n0:10\n0:20\n";     // deliberately out of time order
        bard::parse_sidecar(txt, R, 60u * R, l);
        bard::resolve(l, 60u * R, 1);
        check(l.mark[0].start == 30u * R && l.mark[0].end == 60u * R, "scramble: last-in-time runs to the book end");
        check(l.mark[1].start == 10u * R && l.mark[1].end == 20u * R, "scramble: open end closes at the next later mark");
        check(l.mark[2].start == 20u * R && l.mark[2].end == 30u * R, "scramble: ...regardless of line position");
        check(l.order[0] == 0 && l.order[1] == 1 && l.order[2] == 2,
              "scramble: default play order is the FILE's line order");
    }

    // A4. Directives.
    {
        bard::MarkList l;
        bard::parse_sidecar("#!bard order=time loop=segment\n0:30\n0:10\n0:20\n", R, 60u * R, l);
        check(l.ordering == bard::MarkOrder::Time, "directive: order=time parsed");
        check(l.loop == bard::LoopMode::Segment && l.loop_set, "directive: loop=segment parsed and flagged");
        bard::resolve(l, 60u * R, 1);
        check(l.order[0] == 1 && l.order[1] == 2 && l.order[2] == 0, "order=time sorts the play order by start");

        bard::MarkList s;
        bard::parse_sidecar("#!bard order=shuffle\n0:10\n0:20\n0:30\n0:40\n0:50\n", R, 60u * R, s);
        bard::resolve(s, 60u * R, 12345);
        bard::MarkList s2;
        bard::parse_sidecar("#!bard order=shuffle\n0:10\n0:20\n0:30\n0:40\n0:50\n", R, 60u * R, s2);
        bard::resolve(s2, 60u * R, 12345);
        bool same = true;
        for (int i = 0; i < s.count; i++) if (s.order[i] != s2.order[i]) same = false;
        check(same, "order=shuffle is deterministic for a given seed (a shuffled book plays the same list)");
        int sum = 0;
        for (int i = 0; i < s.count; i++) sum += s.order[i];
        check(sum == 0 + 1 + 2 + 3 + 4, "order=shuffle is a permutation (every entry appears once)");
    }

    // A5. Robustness: comments, blanks and garbage are skipped, not fatal; marks past the end of the book
    // are dropped; the 64-mark cap holds.
    {
        bard::MarkList l;
        const char* txt =
            "# a comment\n"
            "\n"
            "not a timestamp at all\n"
            "0:05  fine\n"
            "9:99:99  past the end\n";
        bard::parse_sidecar(txt, R, 60u * R, l);
        check(l.count == 1, "robust: garbage/comment/blank lines skipped, past-the-end mark dropped");
        check(l.mark[0].start == 5u * R, "robust: the one good line survived");

        std::string many;
        for (int i = 0; i < 100; i++) many += std::to_string(i + 1) + "\n";   // 100 marks, 1 s apart
        bard::MarkList c;
        bard::parse_sidecar(many.c_str(), R, 200u * R, c);
        check(c.count == bard::MarkList::kMax, "robust: mark count is capped at 64");
    }

    // A6. Auto-marks: deterministic per book, re-rollable, in range, strictly increasing.
    {
        const uint32_t frames = 40u * 60u * R;            // a 40-minute book
        bard::MarkList a, b, c;
        bard::auto_marks("HOBBIT1.WAV", frames, R, 0, a);
        bard::auto_marks("HOBBIT1.WAV", frames, R, 0, b);
        bard::auto_marks("HOBBIT1.WAV", frames, R, 1, c);
        check(a.count == b.count, "auto-marks: same book -> same count");
        bool identical = a.count == b.count;
        for (int i = 0; i < a.count && identical; i++) if (a.mark[i].start != b.mark[i].start) identical = false;
        check(identical, "auto-marks: deterministic across calls (seeded by the file, not the clock)");
        bool differs = false;
        for (int i = 0; i < a.count && i < c.count; i++) if (a.mark[i].start != c.mark[i].start) differs = true;
        check(differs, "auto-marks: a re-roll produces a different scatter");
        check(a.count == 8, "auto-marks: a 40-minute book gets minutes/5 = 8 marks");
        check(a.generated, "auto-marks: the list is flagged as generated");
        check(a.mark[0].start == 0u, "auto-marks: there is always a mark at the start of the book");
        bool increasing = true;
        for (int i = 1; i < a.count; i++) if (a.mark[i].start <= a.mark[i - 1].start) increasing = false;
        check(increasing, "auto-marks: strictly increasing (jitter never reorders or collides)");

        bard::MarkList tiny;
        bard::auto_marks("SHORT.WAV", 30u * R, R, 0, tiny);      // 30 s -> minutes/5 = 0, clamped to 4
        check(tiny.count == 4, "auto-marks: a very short book still gets the 4-mark floor");
        bard::MarkList huge;
        bard::auto_marks("LONG.WAV", 20u * 3600u * R, R, 0, huge);  // 20 h -> 240, clamped to 32
        check(huge.count == 32, "auto-marks: a very long book is capped at 32 marks");
    }

    // A6b. serialize_marks round-trips through parse_sidecar: an explicit end is written as a range only
    // when a re-read would not infer it, and the directive line appears only when it carries information.
    {
        bard::MarkList l;
        bard::parse_sidecar("#!bard order=time loop=segment\n0:00\n0:10-0:12\n0:20\n", R, 60u * R, l);
        bard::resolve(l, 60u * R, 1);
        char buf[1024];
        const int n = bard::serialize_marks(l, R, 60u * R, buf, sizeof(buf));
        check(n > 0, "serialize: produced text");
        check(std::strstr(buf, "#!bard") != nullptr, "serialize: directives round-trip");
        check(std::strstr(buf, "order=time") && std::strstr(buf, "loop=segment"), "serialize: both directives");

        bard::MarkList back;
        bard::parse_sidecar(buf, R, 60u * R, back);
        bard::resolve(back, 60u * R, 1);
        check(back.count == l.count, "serialize: mark count survives the round trip");
        bool same = back.count == l.count && back.ordering == l.ordering && back.loop == l.loop;
        for (int i = 0; i < back.count && same; i++)
            if (back.mark[i].start != l.mark[i].start || back.mark[i].end != l.mark[i].end) same = false;
        check(same, "serialize: every span survives the round trip exactly");

        // A list with nothing to declare emits no directive line.
        bard::MarkList plain;
        bard::parse_sidecar("0:00\n0:10\n", R, 60u * R, plain);
        bard::resolve(plain, 60u * R, 1);
        bard::serialize_marks(plain, R, 60u * R, buf, sizeof(buf));
        check(std::strstr(buf, "#!bard") == nullptr, "serialize: no directive line when there is nothing to say");

        // Millisecond precision survives (format is H:MM:SS.mmm).
        bard::MarkList ms;
        bard::parse_sidecar("0:01.250\n", R, 60u * R, ms);
        bard::resolve(ms, 60u * R, 1);
        bard::serialize_marks(ms, R, 60u * R, buf, sizeof(buf));
        bard::MarkList ms2;
        bard::parse_sidecar(buf, R, 60u * R, ms2);
        check(ms2.count == 1 && ms2.mark[0].start == ms.mark[0].start, "serialize: milliseconds survive");
    }

    // A6c. Cross-check against what scripts/prepare_audiobooks.py actually writes. Two hazards live here:
    // embedded chapter titles routinely contain a HYPHEN ("Chapter 1 - An Unexpected Party") and the
    // grammar uses '-' for an explicit range, and the script emits H:MM:SS.mmm which must re-read exactly.
    {
        bard::MarkList l;
        const char* written =
            "# hobbit.m4b\n"
            "0:00:00.000   Chapter 1 - An Unexpected Party\n"
            "0:00:10.000   Chapter 2 - Roast Mutton\n"
            "0:00:20.000   Chapter 3 - A Short Rest\n";
        bard::parse_sidecar(written, R, 30u * R, l);
        bard::resolve(l, 30u * R, 1);
        check(l.count == 3, "prep-script: the emitted sidecar parses to one mark per chapter");
        check(l.mark[0].start == 0u, "prep-script: first chapter at 0");
        check(l.mark[1].start == 10u * R, "prep-script: H:MM:SS.mmm re-reads exactly");
        check(l.mark[2].start == 20u * R, "prep-script: ...for every chapter");
        check(l.mark[0].end == 10u * R,
              "prep-script: a hyphen INSIDE a chapter title is a label, not an explicit range");
        check(l.mark[2].end == 30u * R, "prep-script: the last chapter runs to the end of the book");

        // The join path writes bare filenames as labels, which can contain digits and underscores.
        bard::MarkList j;
        bard::parse_sidecar("# hobbit_librivox\n0:00:00.000   hobbit_1_tolkien_64kb\n"
                            "0:00:03.000   hobbit_2_tolkien_64kb\n", R, 10u * R, j);
        bard::resolve(j, 10u * R, 1);
        check(j.count == 2 && j.mark[1].start == 3u * R,
              "prep-script: join labels (filenames with digits) do not disturb the timestamps");
    }

    // A7. Lookups.
    {
        bard::MarkList l;
        bard::parse_sidecar("0:00\n0:10\n0:20\n", R, 30u * R, l);
        bard::resolve(l, 30u * R, 1);
        check(bard::mark_at(l, 5u * R) == 0, "mark_at: inside the first segment");
        check(bard::mark_at(l, 15u * R) == 1, "mark_at: inside the second segment");
        check(bard::mark_at(l, 40u * R) == -1, "mark_at: past every segment -> -1");
        check(bard::order_slot(l, 2) == 2, "order_slot: file order maps index to slot");
        check(bard::order_slot(l, 99) == -1, "order_slot: unknown index -> -1");
    }

    // A8. bard.cfg.
    {
        bard::Config c;
        check(c.resume && c.rate == 48000u, "cfg: defaults are resume=on, rate=48000");
        bard::parse_config("# comment\nresume=off\nrate=24000\n", c);
        check(!c.resume, "cfg: resume=off parsed");
        check(c.rate == 24000u, "cfg: rate=24000 parsed");
        bard::Config d;
        bard::parse_config("resume=on\nrate=999999\nbogus=1\n", d);
        check(d.resume && d.rate == 48000u, "cfg: an out-of-range rate and an unknown key leave defaults");
    }

    // ================= B. the resume table ========================================================
    {
        bard::ResumeTable t;
        t.set("0/A.WAV", 100);
        t.set("0/B.WAV", 200);
        uint32_t f = 0;
        check(t.get("0/A.WAV", f) && f == 100, "resume: set/get round trip");
        check(t.get("0/B.WAV", f) && f == 200, "resume: second entry");
        check(!t.get("0/C.WAV", f), "resume: unknown book has no position");
        t.set("0/A.WAV", 150);
        check(t.get("0/A.WAV", f) && f == 150, "resume: re-set updates in place");
        check(t.count == 2, "resume: re-set does not duplicate");

        // LRU eviction: the oldest untouched book is the one forgotten.
        bard::ResumeTable e;
        for (int i = 0; i < bard::ResumeTable::kMax + 4; i++)
            e.set(("0/BK" + std::to_string(i) + ".WAV").c_str(), static_cast<uint32_t>(i));
        check(e.count == bard::ResumeTable::kMax, "resume: table caps at kMax entries");
        check(!e.get("0/BK0.WAV", f), "resume: the least recently touched entry was evicted");
        check(e.get(("0/BK" + std::to_string(bard::ResumeTable::kMax + 3) + ".WAV").c_str(), f),
              "resume: the newest entry survived");

        // Text round trip, and the size claim the LRU cap exists to guarantee.
        char buf[bard::ResumeTable::kTextMax];
        const int n = e.serialize(buf, sizeof(buf));
        check(n > 0 && n < 2048, "resume: a full 64-entry table serializes to under 2 KB (read_text-safe)");
        bard::ResumeTable p;
        check(p.parse(buf) == e.count, "resume: parse recovers every serialized line");
        uint32_t a = 0, b = 0;
        e.get("0/BK10.WAV", a); p.get("0/BK10.WAV", b);
        check(a == b, "resume: frames survive the round trip");

        // Tolerance: a torn tail (a power cut mid-rewrite) costs only the bad line.
        bard::ResumeTable tt;
        check(tt.parse("0/A.WAV 10\n# note\ngarbage\n0/B.WAV notanumber\n0/C.W") == 1,
              "resume: unparseable and truncated lines are skipped, not fatal");
        check(tt.get("0/A.WAV", f) && f == 10, "resume: the good line before the tear survived");
    }

    // ================= D. WSOLA time-scaling ======================================================
    //
    // The property that matters is that it changes DURATION without changing PITCH - that is the whole
    // reason it is WSOLA and not the pstretch FFT, whose phase randomization would shred intelligibility.
    {
        auto sine_drive = [](bard::Wsola& w, float hz, int out_blocks, uint32_t& consumed,
                             std::vector<float>& out) {
            double ph = 0.0;
            const double inc = 2.0 * M_PI * hz / host::kSampleRate;
            consumed = 0;
            float buf[8192];
            for (int b = 0; b < out_blocks; b++) {
                uint32_t need = w.want(host::kBlock);
                while (need) {
                    const uint32_t chunk = need > 8192u ? 8192u : need;
                    for (uint32_t i = 0; i < chunk; i++) { buf[i] = static_cast<float>(std::sin(ph)); ph += inc; }
                    w.feed(buf, chunk);
                    consumed += chunk;
                    need -= chunk;
                }
                float o[host::kBlock];
                const uint32_t got = w.drain(o, host::kBlock);
                for (uint32_t i = 0; i < got; i++) out.push_back(o[i]);
            }
        };
        auto zero_cross_hz = [](const std::vector<float>& v, size_t skip) {
            int crossings = 0;
            for (size_t i = skip + 1; i < v.size(); i++)
                if ((v[i - 1] <= 0.f && v[i] > 0.f) || (v[i - 1] >= 0.f && v[i] < 0.f)) crossings++;
            const double dur = static_cast<double>(v.size() - skip) / host::kSampleRate;
            return dur > 0.0 ? static_cast<float>(crossings / 2.0 / dur) : 0.f;
        };

        // D1. Bypass at scale 1.0 must be BIT-EXACT, so turning PITCH-KEEP to zero restores the shipped
        // varispeed sound exactly rather than approximately.
        {
            bard::Wsola w; w.init(); w.set_scale(1.f);
            check(w.bypassed(), "wsola: scale 1.0 reports bypass");
            float in[300], out[300];
            for (int i = 0; i < 300; i++) in[i] = static_cast<float>(i) * 0.001f - 0.15f;
            w.feed(in, 300);
            const uint32_t got = w.drain(out, 300);
            check(got == 300, "wsola: bypass delivers every frame");
            bool exact = true;
            for (int i = 0; i < 300; i++) if (in[i] != out[i]) exact = false;
            check(exact, "wsola: bypass is BIT-EXACT (PITCH-KEEP at 0 = the shipped varispeed path)");
        }

        // D2. Duration: at scale 0.5 the engine consumes input twice as fast as it emits output.
        {
            bard::Wsola w; w.init(); w.set_scale(0.5f);
            check(!w.bypassed(), "wsola: a non-unity scale leaves bypass");
            uint32_t consumed = 0; std::vector<float> out;
            sine_drive(w, 200.f, 120, consumed, out);
            const float ratio = static_cast<float>(consumed) / static_cast<float>(out.size());
            check(out.size() > 100, "wsola: scale 0.5 produced output");
            check(ratio > 1.7f && ratio < 2.3f, "wsola: scale 0.5 consumes ~2 input frames per output frame");
        }

        // D3. PITCH: the output frequency must match the input at every scale. This is the claim the whole
        // algorithm choice rests on.
        {
            for (float scale : { 0.4f, 0.7f, 1.4f }) {
                bard::Wsola w; w.init(); w.set_scale(scale);
                uint32_t consumed = 0; std::vector<float> out;
                sine_drive(w, 200.f, 200, consumed, out);
                const float hz = zero_cross_hz(out, 2048);       // skip the priming transient
                const bool ok = hz > 190.f && hz < 210.f;
                if (!ok) std::printf("    (scale %.2f -> %.1f Hz, n=%zu)\n", scale, hz, out.size());
                check(ok, "wsola: pitch is preserved across the time-scale (200 Hz in, 200 Hz out)");
                float pk = 0.f; bool fin = true;
                for (float x : out) { pk = std::fmax(pk, std::fabs(x)); if (!std::isfinite(x)) fin = false; }
                check(fin, "wsola: output finite");
                check(pk < 1.6f, "wsola: overlap-add stays bounded (Hann at 50% sums to unity)");
            }
        }

        // D4. Starvation is graceful: draining without feeding returns short, never garbage.
        {
            bard::Wsola w; w.init(); w.set_scale(0.5f);
            float o[128];
            const uint32_t got = w.drain(o, 128);
            check(got == 0, "wsola: draining an empty engine returns nothing rather than garbage");
        }
    }

    // ================= C. the engine ==============================================================

    FakeTime time;
    FakeStream stream;
    auto reset_card = [&]() {
        stream.shelves.clear();
        stream.files.clear();
        stream.writes.clear();
        stream.write_ok = true;
        for (int i = 0; i < 2; i++) {
            stream.playing[i] = false; stream.open_calls[i] = 0; stream.seek_calls[i] = 0;
            stream.last_frame[i] = 0; stream.cursor[i] = 0; stream.starve[i] = false;
            stream.last_path[i][0] = '\0';
        }
        // shelf 0: two 60 s books; shelf 3: one book (so SHELF select is observable)
        stream.shelves[0] = { { "BOOK1.WAV", 60u * R, true, R }, { "BOOK2.WAV", 60u * R, true, R } };
        stream.shelves[3] = { { "SHELF3.WAV", 60u * R, true, R } };
    };
    // Built by hand rather than via host::make_context: bard sub-allocates nothing from the arena (its
    // whole state is fixed-size members), and the test drives its own clock.
    // Deck B defaults to the same shelf and opens a book too, so it mixes into the bus. Every test that
    // MEASURES deck A silences B first - otherwise "deck A is silent" can never be observed.
    auto solo_a = [&](BardEngine& e) { e.set_param(ParamId::Mix, DeckRef::B, 0.f); };

    // A real arena so the Grit room's delay lines are live (with a null arena the room passes audio
    // through, which is a valid state but would make the room test vacuous).
    static std::vector<uint8_t> arena_mem(1u << 20);
    auto make = [&](BardEngine& e) {
        EngineContext c;
        c.sample_rate = host::kSampleRate;
        c.block_size  = static_cast<float>(host::kBlock);
        c.arena       = { arena_mem.data(), arena_mem.size() };
        c.time        = &time;
        c.transport   = nullptr;
        c.stream      = &stream;
        e.init(c);
    };

    // C1. The card is read and the first book opens at frame 0 with no resume file.
    {
        reset_card(); time.ms = 0;
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        check(std::strcmp(stream.last_path[0], "bard/0/BOOK1.WAV") == 0,
              "engine: BOOK knob 0 opens the first book on shelf 0");
        check(stream.last_frame[0] == 0u, "engine: with no resume file a book opens at frame 0");
        check(!stream.last_loop[0], "engine: books open non-looping (the engine owns the end of the book)");
    }

    // C2. SHELF select (Alt+PITCH) changes the directory.
    {
        reset_card(); time.ms = 0;
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.set_param(ParamId::Aux, DeckRef::A, 3.5f / 16.f);      // shelf 3
        e.prepare();
        check(std::strncmp(stream.last_path[0], "bard/3/", 7) == 0, "engine: Aux picks the shelf directory");
    }

    // C3. Persisted resume: a stored position is honoured on open, and a fresh position is written back.
    {
        reset_card(); time.ms = 0;
        stream.files["bard/resume.txt"] = "0/BOOK1.WAV 96000\n";
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        check(stream.last_frame[0] == 96000u, "resume: a stored position is honoured when the book opens");

        bool fin = true;
        run(e, 40, fin);
        time.advance(40000);                                     // past the 30 s checkpoint
        e.prepare();
        bool wrote = false;
        for (auto& w : stream.writes) if (w.first == "bard/resume.txt") wrote = true;
        check(wrote, "resume: the table is written back on the checkpoint");
        check(fin, "resume: output stayed finite");
    }

    // C4. resume=off means the engine never opens a file for writing at all.
    {
        reset_card(); time.ms = 0;
        stream.files["bard/bard.cfg"] = "resume=off\n";
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        bool fin = true;
        run(e, 40, fin);
        time.advance(120000);
        e.prepare();
        e.prepare();
        check(stream.writes.empty(), "cfg: resume=off performs no card writes");
    }

    // C4b. A failed write disables persistence for the session instead of retrying every loop.
    {
        reset_card(); time.ms = 0;
        stream.write_ok = false;
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        bool fin = true;
        run(e, 10, fin);
        for (int k = 0; k < 5; k++) { time.advance(40000); e.prepare(); }
        stream.write_ok = true;                                  // even if the card comes back...
        for (int k = 0; k < 5; k++) { time.advance(40000); e.prepare(); }
        check(stream.writes.empty(), "resume: a failed write stops retrying for the session");
        check(fin, "resume: a write failure never disturbs the audio");
    }

    // C5. Play / pause. A paused deck is silent; the Play pad toggles it back.
    {
        reset_card(); time.ms = 0;
        BardEngine e; make(e); solo_a(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.prepare();
        bool fin = true;
        const auto playing = run(e, 8, fin);
        check(peak(playing) > 0.01f, "play: an opened book plays");

        e.on_play_pad(DeckRef::A, false);                        // pause
        const auto paused = run(e, 8, fin);
        check(peak(paused) < 1e-6f, "pause: the Play pad silences the deck");

        time.advance(1000);
        e.on_play_pad(DeckRef::A, false);                        // resume
        const auto resumed = run(e, 8, fin);
        check(peak(resumed) > 0.01f, "pause: the Play pad resumes playback");
        check(fin, "play/pause: output stayed finite");
    }

    // C6. Rev pad = JUMP BACK 15 s, clamped at the start of the book.
    {
        reset_card(); time.ms = 0;
        stream.files["bard/resume.txt"] = "0/BOOK1.WAV 1440000\n";   // 30 s in
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        const uint32_t at = stream.last_frame[0];
        time.advance(500);
        e.on_play_pad(DeckRef::A, true);                         // Rev
        e.prepare();
        check(stream.last_frame[0] < at, "jump-back: the Rev pad seeks backwards");
        check(at - stream.last_frame[0] >= 15u * R - 4000u && at - stream.last_frame[0] <= 15u * R + 4000u,
              "jump-back: about 15 s of source frames");

        for (int k = 0; k < 4; k++) { time.advance(500); e.on_play_pad(DeckRef::A, true); e.prepare(); }
        check(stream.last_frame[0] == 0u, "jump-back: repeated steps clamp at the start of the book");
    }

    // C7. The BOOKMARK knob jumps to a mark start - and only after the settle guard, so a sweep does not
    // re-open per main loop (radio's anti-stutter guard, inherited).
    {
        reset_card(); time.ms = 0;
        stream.files["bard/0/BOOK1.txt"] = "0:00\n0:10\n0:20\n0:30\n";
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        const int n0 = stream.jumps(0);

        e.set_param(ParamId::Pos, DeckRef::A, 1.f);              // last mark (0:30)
        e.prepare();
        check(stream.jumps(0) == n0, "bookmark: an unsettled selector move does not seek yet");
        settle(e, time);
        check(stream.last_frame[0] == 30u * R, "bookmark: the settled knob jumps to the mark start");

        // The jump must have taken the LIGHT path - an f_lseek on the live handle, not a reopen. This is
        // what makes rapid bookmark stepping affordable (and pays down radio-impl.md's wanted debt).
        check(stream.seek_calls[0] > 0 && stream.open_calls[0] == 1,
              "bookmark: a jump within the open book uses the light in-file seek, not a reopen");

        // Chatter across a boundary must not seek repeatedly.
        const int n1 = stream.jumps(0);
        bool fin = true;
        for (int k = 0; k < 40; k++) {
            e.cv_size_pos(DeckRef::A, (k & 1) ? 0.10f : -0.10f);
            time.advance(20);
            e.prepare();
            run(e, 1, fin);
        }
        check(stream.jumps(0) == n1, "bookmark: boundary chatter causes no re-seeks (anti-stutter)");
        check(fin, "bookmark: chatter output stayed finite");
    }

    // C8. Recite HOLDS at the segment end by default, and loops once the policy is toggled (Alt+Seq held).
    {
        reset_card(); time.ms = 0;
        stream.files["bard/0/BOOK1.txt"] = "0:00-0:01\n0:10\n";   // a 1 s opening segment
        BardEngine e; make(e); solo_a(e);
        e.set_config(ConfigId::Mode, DeckRef::A, 0);              // 0 = Slice = Recite
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.prepare();
        const int n0 = stream.jumps(0);

        bool fin = true;
        run(e, 700, fin);                                         // 700 * 96 = 67200 frames > 1 s
        const auto tail = run(e, 8, fin);
        check(peak(tail) < 1e-6f, "recite: playback stops dead at the segment end");
        e.prepare();
        check(stream.jumps(0) == n0, "recite: the default policy HOLDS (no re-seek at the end)");

        e.clear_sequence(DeckRef::A);                             // Alt+Seq held -> loop
        e.prepare();
        check(stream.jumps(0) == n0 + 1, "recite: with loop enabled the segment re-seeks to its start");
        check(stream.last_frame[0] == 0u, "recite: the loop lands back on the segment start");
        check(fin, "recite: output stayed finite");
    }

    // C9. Wander auto-advances through the play order at each segment end.
    {
        reset_card(); time.ms = 0;
        stream.files["bard/0/BOOK1.txt"] = "0:00-0:01\n0:30-0:31\n";
        BardEngine e; make(e);
        e.set_config(ConfigId::Mode, DeckRef::A, 2);              // 2 = Drift = Wander
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        bool fin = true;
        run(e, 700, fin);                                         // run past the end of segment 1
        e.prepare();                                              // sees the end, advances
        e.prepare();                                              // applies the queued jump
        check(stream.last_frame[0] == 30u * R, "wander: the segment end advances to the next entry");
        check(fin, "wander: output stayed finite");
    }

    // C10. Gate in advances a bookmark; gate out latches a pulse on the crossing.
    {
        reset_card(); time.ms = 0;
        stream.files["bard/0/BOOK1.txt"] = "0:00\n0:10\n0:20\n";
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        e.gate_out_triggered(DeckRef::A);                         // clear anything from the open
        time.advance(500);
        e.on_gate_trigger(DeckRef::A);
        e.prepare();
        check(stream.last_frame[0] == 10u * R, "gate: a rising edge advances one entry in the play order");
        check(e.gate_out_triggered(DeckRef::A), "gate: the crossing latched a gate-out pulse");
        check(!e.gate_out_triggered(DeckRef::A), "gate: the pulse latch self-clears");

        // An unpatched, chattering jack must not seek per loop.
        const int n0 = stream.jumps(0);
        for (int k = 0; k < 20; k++) { time.advance(5); e.on_gate_trigger(DeckRef::A); e.prepare(); }
        check(stream.jumps(0) - n0 <= 1, "gate: a chattering jack is debounced");
    }

    // C11. The Seq pad steps the play order, and order=time makes that differ from the knob's line order.
    {
        reset_card(); time.ms = 0;
        stream.files["bard/0/BOOK1.txt"] = "#!bard order=time\n0:30\n0:10\n0:20\n";
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        // opened at 0 -> not inside any segment (the first mark is at 0:10), so "next" starts at slot 0
        e.on_seq_trigger(DeckRef::A);
        e.prepare();
        check(stream.last_frame[0] == 10u * R, "seq: Next follows the play order (order=time -> 0:10 first)");
        e.on_seq_trigger(DeckRef::A);
        e.prepare();
        check(stream.last_frame[0] == 20u * R, "seq: Next steps to the following entry in time order");
    }

    // C12. Alt+POS scrub, debounced.
    {
        reset_card(); time.ms = 0;
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        e.set_param(ParamId::AltPos, DeckRef::A, 0.5f);
        settle(e, time);
        const uint32_t mid = 30u * R;
        check(stream.last_frame[0] > mid - R && stream.last_frame[0] < mid + R,
              "scrub: Alt+POS at centre seeks halfway through the book (Read spans the whole recording)");
    }

    // C13. RATE: unity, 0.5x and 2.5x all finite and bounded, and audibly different.
    {
        reset_card(); time.ms = 0;
        BardEngine e; make(e); solo_a(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.prepare();
        bool f1 = true, f2 = true;
        e.set_param(ParamId::Size, DeckRef::A, 0.f);             // 0.5x
        const auto slow = run(e, 30, f1);
        e.set_param(ParamId::Size, DeckRef::A, 1.f);             // 2.5x
        const auto fast = run(e, 30, f2);
        check(f1 && f2, "rate: output finite at both extremes");
        check(peak(slow) < 1.2f && peak(fast) < 1.2f, "rate: output bounded near 0 dBFS");
        check(sad(slow, fast) > 1.f, "rate: 0.5x and 2.5x differ (the resampler is wired)");
        check(e.param(ParamId::Size, DeckRef::A) == 1.f, "rate: the knob reads back");
    }

    // C14. An off-rate book (24 kHz, the format bard recommends for speech) plays at its own rate.
    {
        reset_card(); time.ms = 0;
        stream.shelves[0] = { { "BOOK1.WAV", 60u * 24000u, true, 24000 } };
        BardEngine e; make(e); solo_a(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.prepare();
        bool fin = true;
        const auto at24 = run(e, 30, fin);

        reset_card(); time.ms = 0;
        BardEngine e2; make(e2); solo_a(e2);                       // the 48 kHz shelf from reset_card
        e2.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e2.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e2.prepare();
        const auto at48 = run(e2, 30, fin);
        check(fin, "off-rate: output finite at both source rates");
        check(sad(at24, at48) > 1.f, "off-rate: a 24 kHz book is rebased by its own header rate");
    }

    // C15. Auto-marks are used when a book has no sidecar (the ring has ticks to draw either way).
    {
        reset_card(); time.ms = 0;                                 // no sidecar files at all
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        e.set_param(ParamId::Pos, DeckRef::A, 1.f);                // jump to the LAST auto-mark
        settle(e, time);
        check(stream.last_frame[0] > 0u, "auto-marks: a book with no sidecar still has bookmarks to jump to");
        check(stream.last_frame[0] < 60u * R, "auto-marks: ...and they sit inside the book");
    }

    // C16. Flux voice colour changes the sound; off by default.
    {
        reset_card(); time.ms = 0;
        BardEngine e; make(e); solo_a(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.prepare();
        bool fin = true;
        const auto clean = run(e, 20, fin);
        e.set_param(ParamId::FluxIntensity, DeckRef::A, 1.f);
        e.set_param(ParamId::FluxMix, DeckRef::A, 1.f);
        const auto still_clean = run(e, 20, fin);
        check(sad(clean, still_clean) < 1e-3f * still_clean.size(),
              "colour: the knobs alone do nothing until the Flux pad engages");
        e.set_fx(DeckRef::A, FxKind::Flux, true);
        const auto coloured = run(e, 20, fin);
        check(sad(clean, coloured) > 1.f, "colour: the Flux pad band-limits and drives the voice");
        check(peak(coloured) < 1.2f, "colour: output stays bounded");
        check(fin, "colour: output finite");
    }

    // C17. The ducker: deck A's speech pulls deck B down when A is in Follow with depth.
    {
        reset_card(); time.ms = 0;
        stream.shelves[0] = { { "BOOK1.WAV", 60u * R, true, R } };
        BardEngine e; make(e);
        for (auto d : { DeckRef::A, DeckRef::B }) {
            e.set_param(ParamId::Speed, d, 0.f);
            e.set_param(ParamId::Mix, d, 1.f);
        }
        e.set_config(ConfigId::Route, DeckRef::A, 1);              // DoubleMono: A left, B right
        e.prepare();
        bool fin = true;
        run(e, 4, fin);
        float il[host::kBlock] = {0}, ir[host::kBlock] = {0}, ol[host::kBlock], orr[host::kBlock];
        const float* in[2] = { il, ir };
        float* out[2] = { ol, orr };
        e.process(in, out, host::kBlock);
        float unducked = 0.f;
        for (size_t i = 0; i < host::kBlock; i++) unducked = std::fmax(unducked, std::fabs(orr[i]));

        e.set_config(ConfigId::ModType, DeckRef::A, 1);            // A = Follow
        e.set_param(ParamId::ModAmp, DeckRef::A, 1.f);             // full duck depth
        run(e, 8, fin);
        e.process(in, out, host::kBlock);
        float ducked = 0.f;
        for (size_t i = 0; i < host::kBlock; i++) ducked = std::fmax(ducked, std::fabs(orr[i]));
        check(ducked < unducked * 0.9f, "duck: deck A's speech envelope attenuates deck B");
        check(fin, "duck: output finite");
    }

    // C18. A starved ring must not run the tracked playhead ahead of what was actually heard - otherwise
    // an underrun would silently skip the book forward and corrupt the resume position.
    {
        reset_card(); time.ms = 0;
        BardEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.set_param(ParamId::Speed, DeckRef::B, 1.f);            // deck B on the OTHER book (its own key)
        e.prepare();
        bool fin = true;
        run(e, 4, fin);
        stream.starve[0] = true;
        run(e, 50, fin);
        stream.starve[0] = false;
        time.advance(40000);
        e.prepare();
        uint32_t stored = 0;
        bard::ResumeTable t;
        for (auto& w : stream.writes) if (w.first == "bard/resume.txt") t.parse(w.second.c_str());
        check(t.get("0/BOOK1.WAV", stored), "underrun: a position was still checkpointed");
        check(stored < 4u * host::kBlock + 16u,
              "underrun: the playhead did not advance while the ring was starved");
        check(fin, "underrun: output finite through the starve");
    }

    // C19. An IDLE bookmark knob must not fight the Seq pad / gate. The knob disagrees with the segment
    // the moment anything else advances it, so a selector that re-quantizes on every main loop would drag
    // the playhead straight back - the advance would silently undo itself ~180 ms later.
    {
        reset_card(); time.ms = 0;
        stream.files["bard/0/BOOK1.txt"] = "0:00\n0:10\n0:20\n0:30\n";
        BardEngine e; make(e); solo_a(e);
        e.set_param(ParamId::Pos, DeckRef::A, 0.f);              // knob parked at the FIRST mark
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        time.advance(500);
        e.on_seq_trigger(DeckRef::A);                            // Seq pad -> mark 1
        e.prepare();
        check(stream.last_frame[0] == 10u * R, "idle-knob: the Seq pad advanced to the second mark");
        settle(e, time, 5);                                      // plenty of settle time for a fight
        check(stream.last_frame[0] == 10u * R,
              "idle-knob: an untouched POS knob does not drag the advance back");

        // ...but moving the knob still selects.
        e.set_param(ParamId::Pos, DeckRef::A, 1.f);
        settle(e, time);
        check(stream.last_frame[0] == 30u * R, "idle-knob: moving POS still jumps to its mark");
    }

    // C20. Tap-hold Play commits the mark list to the sidecar, and a dropped mark (Alt+Play) is included -
    // so a performance's worth of marks survives a power cycle and can be edited on a computer.
    {
        reset_card(); time.ms = 0;
        stream.files["bard/0/BOOK1.txt"] = "0:00\n0:10\n";
        BardEngine e; make(e); solo_a(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.prepare();
        bool fin = true;
        run(e, 40, fin);                                          // advance the playhead a little
        e.on_record_pad(DeckRef::A, false);                        // Alt+Play: drop a mark here
        e.stop_if_generating(DeckRef::A);                          // tap-hold Play: commit
        e.prepare();

        std::string written;
        for (auto& w : stream.writes) if (w.first == "bard/0/BOOK1.txt") written = w.second;
        check(!written.empty(), "commit: tap-hold Play wrote the sidecar");
        bard::MarkList back;
        bard::parse_sidecar(written.c_str(), R, 60u * R, back);
        check(back.count == 3, "commit: the dropped mark is included in the written sidecar");
        check(fin, "commit: audio unaffected");
    }

    // C21. The Grit room: inert until the pad engages, audible when it does, bounded, and its three
    // characters differ. Written MIT from the Schroeder/Moorer structure rather than reusing the GPLv3
    // src/dsp/diffuser.h, which would have relicensed the engine (see room.h).
    {
        reset_card(); time.ms = 0;
        BardEngine e; make(e); solo_a(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.f);
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.prepare();
        bool fin = true;
        const auto dry = run(e, 20, fin);
        e.set_param(ParamId::GritIntensity, DeckRef::A, 0.7f);
        e.set_param(ParamId::GritMix, DeckRef::A, 1.f);
        const auto still_dry = run(e, 20, fin);
        check(sad(dry, still_dry) < 1e-3f * still_dry.size(),
              "room: the knobs alone do nothing until the Grit pad engages");

        // Each character is silent for its own pre-delay after a switch (set_character clears the tail, and
        // nothing emerges until the shortest comb / the slap tap has filled). The comparison window must
        // therefore exceed the LONGEST pre-delay - the slap's ~200 ms - or every character reads as silence.
        const int kRoomBlocks = 300;                                  // ~600 ms at 48 kHz
        e.set_fx(DeckRef::A, FxKind::Grit, true);
        const auto plate = run(e, kRoomBlocks, fin);
        check(sad(dry, plate) > 1.f, "room: the Grit pad adds a room");
        check(peak(plate) < 1.2f, "room: output stays bounded");

        const GritReseed r = e.toggle_grit_mode(DeckRef::A);          // plate -> hall
        check(r.mix == 1.f, "room: toggle_grit_mode reports the mix back for the platform's pickup");
        const auto hall = run(e, kRoomBlocks, fin);
        check(sad(plate, hall) > 1.f, "room: the hall character differs from the plate");
        e.toggle_grit_mode(DeckRef::A);                               // hall -> slap
        const auto slap = run(e, kRoomBlocks, fin);
        check(peak(slap) > 1e-4f, "room: the slap character produces its echo within the window");
        check(sad(hall, slap) > 1.f, "room: the slap character differs again");
        check(peak(hall) < 1.2f && peak(slap) < 1.2f, "room: every character stays bounded");
        check(fin, "room: output finite in all three characters");

        // A long decay must not run away: feed it hard, then check the tail settles rather than blowing up.
        e.set_param(ParamId::GritIntensity, DeckRef::A, 1.f);
        e.toggle_grit_mode(DeckRef::A);                               // back to plate (the feedback network)
        const auto loud = run(e, 200, fin);
        check(peak(loud) < 1.2f && fin, "room: a maximum-decay feedback network stays stable");
    }

    // C22. PITCH-KEEP (ENV): the RATE must be the same either way - only the pitch differs. The invariant
    // is that source frames are consumed at `rate` per output frame regardless of the keep amount, because
    // the chain resamples by rate^(1-keep) and time-scales by rate^-keep, which compose to rate.
    {
        auto consumed_over = [&](float keep, int blocks) {
            reset_card(); time.ms = 0;
            static BardEngine e;                       // static: 2 x WSOLA buffers are large for a stack frame
            e.~BardEngine(); new (&e) BardEngine();
            make(e); solo_a(e);
            e.set_param(ParamId::Speed, DeckRef::A, 0.f);
            e.set_param(ParamId::Mix, DeckRef::A, 1.f);
            e.set_param(ParamId::Size, DeckRef::A, 1.f);          // RATE = 2.5x
            e.set_param(ParamId::Env, DeckRef::A, keep);
            e.prepare();
            const uint32_t before = stream.cursor[0];
            bool fin = true;
            const auto out = run(e, blocks, fin);
            return std::make_pair(stream.cursor[0] - before, out);
        };
        const auto vari = consumed_over(0.f, 60);
        const auto kept = consumed_over(1.f, 60);
        const float frames = 60.f * static_cast<float>(host::kBlock);
        const float rv = static_cast<float>(vari.first) / frames;
        const float rk = static_cast<float>(kept.first) / frames;
        check(rv > 2.2f && rv < 2.8f, "pitch-keep: varispeed consumes ~2.5 source frames per output frame");
        check(rk > 2.2f && rk < 2.8f, "pitch-keep: WSOLA consumes the SAME ~2.5 - rate is unchanged by keep");
        check(sad(vari.second, kept.second) > 1.f, "pitch-keep: the two paths sound different (chain wired)");
        check(peak(kept.second) < 1.4f, "pitch-keep: the time-scaled output stays bounded");
    }

    if (g_failures == 0) { std::printf("OK: all bard checks passed\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
