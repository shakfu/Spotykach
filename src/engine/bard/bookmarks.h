#pragma once

#include <cstdint>

// Bookmark model for the `bard` engine: the plain-text sidecar grammar, the deterministic auto-marks
// used when a book has no sidecar, and the tiny /bard/bard.cfg reader.
//
// Deliberately self-contained (only <cstdint>) and allocation-free so it compiles into the host test on
// its own and runs on the main loop of the device with no heap, no <algorithm>, and no strtod/sscanf.
// Everything here is pure: text in, fixed-size structs out.
//
// User-facing reference: docs/engines/bard.md. Grammar summary:
//
//   #!bard order=file loop=off      <- optional directive line
//   # any other '#' line is a comment
//   0:00              Prologue      <- TIME [ - TIME ] [ whitespace LABEL ]
//   14:32             Chapter 1
//   1:02:11-1:04:00   a passage
//   2841              a bare integer is seconds
//
// TIME is [[HH:]MM:]SS[.mmm] - the LAST colon-separated field is always seconds. A LABEL is parsed and
// discarded (there is no text display; labels are for whoever edits the file). An unparseable line is
// skipped, never fatal.

namespace spotykach {
namespace bard {

// Play order over the marks. `File` is the default and the whole point of the engine: the sidecar's LINE
// order is the recitation order, so a list that is not in time order is a re-ordering of the book.
enum class MarkOrder : uint8_t { File, Time, Shuffle };

// What happens when a segment (Recite) or the mark list (Wander) runs out. `Book` applies to linear Read.
enum class LoopMode : uint8_t { Off, Segment, Book };

// One bookmark: a half-open frame span [start, end). `end` is 0 while parsing ("open-ended") and is
// always resolved to a real frame by resolve() before the engine sees it.
struct Mark {
    uint32_t start;
    uint32_t end;
};

struct MarkList {
    static constexpr int kMax = 64;      // 64 x 8 bytes of spans + 64 order bytes, per deck

    Mark      mark[kMax] = {};
    uint8_t   order[kMax] = {};          // order[k] = index into mark[] of the k-th entry to play
    int       count = 0;
    MarkOrder ordering = MarkOrder::File;
    LoopMode  loop = LoopMode::Off;
    bool      loop_set = false;          // an explicit loop= was present (else the engine's own default)
    bool      generated = false;         // true = auto-marks (no sidecar on the card)

    void clear() {
        count = 0; ordering = MarkOrder::File; loop = LoopMode::Off;
        loop_set = false; generated = false;
    }
    bool full() const { return count >= kMax; }
};

// ---- small integer scanners -------------------------------------------------------------------------

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

inline void skip_space(const char*& p) { while (*p && is_space(*p)) ++p; }

// Read an unsigned decimal run at p (advancing it). false if there was no digit. Saturates rather than
// overflowing on an absurd run of digits.
inline bool scan_uint(const char*& p, uint32_t& out) {
    if (!is_digit(*p)) return false;
    uint32_t v = 0;
    while (is_digit(*p)) {
        if (v < 100000000u) v = v * 10u + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    out = v;
    return true;
}

// Parse "[[HH:]MM:]SS[.mmm]" (or a bare integer of seconds) at p, advancing it past the timestamp.
// The last colon-separated field is seconds, so "14:32" is 14 min 32 s and "2841" is 2841 s.
// Returns false (leaving p where it started) if there is no timestamp here.
inline bool scan_time(const char*& p, uint32_t rate, uint32_t& frames) {
    const char* save = p;
    uint32_t field[3] = { 0, 0, 0 };
    int nf = 0;
    for (;;) {
        uint32_t v = 0;
        if (!scan_uint(p, v)) { p = save; return false; }
        if (nf >= 3) { p = save; return false; }             // more than HH:MM:SS - not a timestamp
        field[nf++] = v;
        if (*p == ':') { ++p; continue; }
        break;
    }
    uint32_t ms = 0;
    if (*p == '.') {
        ++p;
        uint32_t scale = 100;                                 // first digit = hundreds of ms
        while (is_digit(*p)) {
            if (scale >= 1) ms += static_cast<uint32_t>(*p - '0') * scale;
            scale /= 10;
            ++p;
        }
    }
    uint32_t h = 0, m = 0, s = 0;
    if (nf == 1)      { s = field[0]; }
    else if (nf == 2) { m = field[0]; s = field[1]; }
    else              { h = field[0]; m = field[1]; s = field[2]; }
    const unsigned long long total_ms =
        (static_cast<unsigned long long>((h * 60ull + m) * 60ull + s) * 1000ull) + ms;
    const unsigned long long f = total_ms * static_cast<unsigned long long>(rate) / 1000ull;
    frames = (f > 0xffffffffull) ? 0xffffffffu : static_cast<uint32_t>(f);
    return true;
}

// ---- resolve: close open-ended segments and build the play order ------------------------------------

// Fill every unset `end`, clamp spans into the book, and build order[] from `ordering`.
//
// An open-ended segment runs to the next CHRONOLOGICALLY later mark, not the next line - which is what
// lets a deliberately scrambled sidecar still describe well-defined segments. `seed` only matters for
// MarkOrder::Shuffle (deterministic, so a shuffled book plays the same list every boot).
inline void resolve(MarkList& l, uint32_t book_frames, uint32_t seed) {
    if (book_frames == 0) book_frames = 0xffffffffu;

    for (int i = 0; i < l.count; i++) {
        Mark& m = l.mark[i];
        if (m.start >= book_frames) m.start = book_frames - 1;
        if (m.end == 0 || m.end <= m.start) {
            uint32_t next = book_frames;                      // default: run to the end of the book
            for (int j = 0; j < l.count; j++) {
                const uint32_t s = l.mark[j].start;
                if (s > m.start && s < next) next = s;        // nearest later start, in ANY line position
            }
            m.end = next;
        }
        if (m.end > book_frames) m.end = book_frames;
    }

    for (int i = 0; i < l.count; i++) l.order[i] = static_cast<uint8_t>(i);

    if (l.ordering == MarkOrder::Time) {
        // Insertion sort of the order indices by start frame (n <= 64, no <algorithm>, stable).
        for (int i = 1; i < l.count; i++) {
            const uint8_t k = l.order[i];
            int j = i - 1;
            while (j >= 0 && l.mark[l.order[j]].start > l.mark[k].start) { l.order[j + 1] = l.order[j]; --j; }
            l.order[j + 1] = k;
        }
    } else if (l.ordering == MarkOrder::Shuffle) {
        uint32_t rng = seed ? seed : 0x9e3779b9u;
        for (int i = l.count - 1; i > 0; i--) {               // Fisher-Yates over the order indices
            rng = rng * 1664525u + 1013904223u;
            const int j = static_cast<int>((rng >> 8) % static_cast<uint32_t>(i + 1));
            const uint8_t t = l.order[i]; l.order[i] = l.order[j]; l.order[j] = t;
        }
    }
}

// ---- the sidecar ------------------------------------------------------------------------------------

// Parse one `#!bard` directive line (order= / loop=). Unknown keys are ignored. Stops at the newline, so
// it can be handed a pointer into the middle of a multi-line buffer.
inline void parse_directives(const char* p, MarkList& l) {
    auto word_is = [](const char*& q, const char* lit) {
        const char* a = q; const char* b = lit;
        while (*b && *a == *b) { ++a; ++b; }
        if (*b) return false;
        q = a;
        return true;
    };
    while (*p && *p != '\n') {
        skip_space(p);
        if (!*p || *p == '\n') break;
        if (word_is(p, "order=")) {
            if      (word_is(p, "time"))    l.ordering = MarkOrder::Time;
            else if (word_is(p, "shuffle")) l.ordering = MarkOrder::Shuffle;
            else if (word_is(p, "file"))    l.ordering = MarkOrder::File;
        } else if (word_is(p, "loop=")) {
            if      (word_is(p, "segment")) { l.loop = LoopMode::Segment; l.loop_set = true; }
            else if (word_is(p, "book"))    { l.loop = LoopMode::Book;    l.loop_set = true; }
            else if (word_is(p, "off"))     { l.loop = LoopMode::Off;     l.loop_set = true; }
        } else {
            while (*p && *p != '\n' && !is_space(*p)) ++p;     // unknown token - skip it whole
        }
    }
}

// Parse a whole sidecar into `out`. `rate` is the book's own sample rate (so timestamps convert to
// frames) and `book_frames` its length (0 = unknown, no clamping). Returns the number of marks accepted.
//
// Robustness rules, all deliberate: a line whose timestamp will not parse is skipped rather than
// failing the file; a mark past the end of the book is dropped; and a failed END parse (very likely,
// since "0:00 - Prologue" is a natural thing for a human to write) leaves the segment open-ended and
// treats the rest of the line as a label.
inline int parse_sidecar(const char* text, uint32_t rate, uint32_t book_frames, MarkList& out) {
    out.clear();
    if (!text || rate == 0) return 0;

    const char* p = text;
    while (*p) {
        const char* line = p;
        while (*p && *p != '\n') ++p;                          // find the line end
        const char* eol = p;
        if (*p == '\n') ++p;

        const char* q = line;
        skip_space(q);
        if (q >= eol) continue;                                // blank

        if (*q == '#') {
            ++q;
            if (*q == '!' ) {
                ++q;
                const char* r = q;
                bool bard = (r[0] == 'b' && r[1] == 'a' && r[2] == 'r' && r[3] == 'd');
                if (bard) parse_directives(r + 4, out);        // parse_directives stops at the NUL/newline
            }
            continue;                                          // every other '#' line is a comment
        }

        if (out.full()) continue;

        uint32_t start = 0;
        if (!scan_time(q, rate, start)) continue;              // not a timestamp line - skip it
        if (book_frames && start >= book_frames) continue;     // a mark past the end is meaningless

        uint32_t end = 0;
        const char* save = q;
        skip_space(q);
        if (*q == '-') {
            ++q;
            skip_space(q);
            uint32_t e = 0;
            if (scan_time(q, rate, e) && e > start) end = e;
            else q = save;                                     // "- Prologue" -> a label, not an end
        }

        out.mark[out.count].start = start;
        out.mark[out.count].end   = end;                       // 0 = open, closed by resolve()
        out.count++;
    }
    return out.count;
}

// ---- auto-marks (no sidecar) ------------------------------------------------------------------------

// FNV-1a over a NUL-terminated name, mixed with the book's length. Seeding from the FILE rather than
// from the clock is the point: the same book gets the same marks on every boot, so they can be learned
// and performed. `reroll` (bumped by Alt+Rev) re-scatters them for the session only.
inline uint32_t book_seed(const char* name, uint32_t book_frames, uint32_t reroll) {
    uint32_t h = 2166136261u;
    for (const char* p = name; p && *p; ++p) { h ^= static_cast<uint8_t>(*p); h *= 16777619u; }
    h ^= book_frames * 2654435761u;
    h += reroll * 0x9e3779b9u;
    return h ? h : 0x9e3779b9u;
}

// Generate deterministic marks for a book with no sidecar: one at the very start plus evenly spaced,
// jittered marks roughly every 5 minutes (4..32 of them).
inline void auto_marks(const char* name, uint32_t book_frames, uint32_t rate,
                       uint32_t reroll, MarkList& out) {
    out.clear();
    out.generated = true;
    if (book_frames == 0 || rate == 0) return;

    const uint32_t minutes = book_frames / rate / 60u;
    int n = static_cast<int>(minutes / 5u);
    if (n < 4) n = 4;
    if (n > 32) n = 32;

    uint32_t rng = book_seed(name, book_frames, reroll);
    const uint32_t spacing = book_frames / static_cast<uint32_t>(n);

    out.mark[out.count].start = 0;                             // always a mark at the start of the book
    out.mark[out.count].end   = 0;
    out.count++;
    for (int i = 1; i < n; i++) {
        rng = rng * 1664525u + 1013904223u;
        // jitter in [-0.4, +0.4) of the spacing, so marks stay in order and never collide
        const int32_t j = static_cast<int32_t>((rng >> 8) % (spacing ? spacing : 1u)) - static_cast<int32_t>(spacing / 2);
        int64_t pos = static_cast<int64_t>(spacing) * i + (static_cast<int64_t>(j) * 4) / 5;
        if (pos < 1) pos = 1;
        if (pos >= static_cast<int64_t>(book_frames)) pos = book_frames - 1;
        out.mark[out.count].start = static_cast<uint32_t>(pos);
        out.mark[out.count].end   = 0;
        out.count++;
    }
}

// ---- writing a sidecar back out ---------------------------------------------------------------------

// Format `frames` as "H:MM:SS.mmm" into `out` (not NUL-terminated); returns bytes written. Always emits
// the hours field so the output is unambiguous and column-aligned for the human who edits it next.
inline int format_time(uint32_t frames, uint32_t rate, char* out) {
    if (rate == 0) rate = 48000;
    const unsigned long long ms = static_cast<unsigned long long>(frames) * 1000ull / rate;
    const uint32_t total_s = static_cast<uint32_t>(ms / 1000ull);
    const uint32_t rem_ms  = static_cast<uint32_t>(ms % 1000ull);
    const uint32_t h = total_s / 3600u, m = (total_s / 60u) % 60u, s = total_s % 60u;
    int n = 0;
    auto d2 = [&](uint32_t v) { out[n++] = static_cast<char>('0' + (v / 10u) % 10u);
                                out[n++] = static_cast<char>('0' + v % 10u); };
    // hours can exceed 99 only for an absurd file; emit as many digits as needed
    if (h >= 10u) { char tmp[8]; int t = 0; uint32_t x = h;
                    do { tmp[t++] = static_cast<char>('0' + x % 10u); x /= 10u; } while (x);
                    for (int i = t - 1; i >= 0; i--) out[n++] = tmp[i]; }
    else out[n++] = static_cast<char>('0' + h);
    out[n++] = ':'; d2(m);
    out[n++] = ':'; d2(s);
    out[n++] = '.';
    out[n++] = static_cast<char>('0' + (rem_ms / 100u) % 10u);
    out[n++] = static_cast<char>('0' + (rem_ms / 10u) % 10u);
    out[n++] = static_cast<char>('0' + rem_ms % 10u);
    return n;
}

// Serialize a mark list back to sidecar text (NUL-terminated); returns bytes written excluding the NUL.
// Round-trips through parse_sidecar: marks are emitted in LINE order (which is the play order), and a
// segment whose end is not simply the next chronological mark is written as an explicit "start-end" range
// so the re-read reproduces the same spans. Writes the directive line only when it carries information.
inline int serialize_marks(const MarkList& l, uint32_t rate, uint32_t book_frames, char* out, int max) {
    if (!out || max <= 1) return 0;
    int n = 0;
    auto put = [&](const char* s) { while (*s && n < max - 1) out[n++] = *s++; };

    if (l.ordering != MarkOrder::File || l.loop_set) {
        put("#!bard");
        if (l.ordering == MarkOrder::Time)         put(" order=time");
        else if (l.ordering == MarkOrder::Shuffle) put(" order=shuffle");
        if (l.loop_set) {
            if (l.loop == LoopMode::Segment)  put(" loop=segment");
            else if (l.loop == LoopMode::Book) put(" loop=book");
            else                               put(" loop=off");
        }
        put("\n");
    }
    for (int i = 0; i < l.count; i++) {
        char line[48];
        int  m = format_time(l.mark[i].start, rate, line);
        // Would a re-read infer this end anyway? It infers the nearest later start, else the book end.
        uint32_t implied = book_frames ? book_frames : 0xffffffffu;
        for (int j = 0; j < l.count; j++) {
            const uint32_t st = l.mark[j].start;
            if (st > l.mark[i].start && st < implied) implied = st;
        }
        if (l.mark[i].end != implied) { line[m++] = '-'; m += format_time(l.mark[i].end, rate, line + m); }
        line[m++] = '\n';
        if (n + m >= max - 1) break;
        for (int k = 0; k < m; k++) out[n++] = line[k];
    }
    out[n] = '\0';
    return n;
}

// ---- lookups the engine needs (here so they are host-testable) --------------------------------------

// Index of the segment containing `frame`, or -1. Segments can overlap in a hand-written sidecar; the
// first match in line order wins, which keeps the answer stable.
inline int mark_at(const MarkList& l, uint32_t frame) {
    for (int i = 0; i < l.count; i++)
        if (frame >= l.mark[i].start && frame < l.mark[i].end) return i;
    return -1;
}

// Where `mark_idx` sits in the play order (so "next" can step from wherever the playhead is), or -1.
inline int order_slot(const MarkList& l, int mark_idx) {
    for (int k = 0; k < l.count; k++) if (static_cast<int>(l.order[k]) == mark_idx) return k;
    return -1;
}

// ---- /bard/bard.cfg ---------------------------------------------------------------------------------

// The card-side config. Both keys are optional; the defaults are what you get with no file at all.
struct Config {
    bool     resume = true;      // resume=off -> the engine never opens a file for writing
    uint32_t rate   = 48000;     // rate=<hz>  -> source rate for headerless .raw books (a .wav carries its own)
};

// Parse "key=value" lines ('#' comments, blank lines ignored). Unknown keys and unparseable values are
// left at their defaults rather than rejecting the file.
inline void parse_config(const char* text, Config& out) {
    if (!text) return;
    const char* p = text;
    while (*p) {
        const char* line = p;
        while (*p && *p != '\n') ++p;
        const char* eol = p;
        if (*p == '\n') ++p;

        const char* q = line;
        skip_space(q);
        if (q >= eol || *q == '#') continue;

        auto word_is = [](const char*& r, const char* lit) {
            const char* a = r; const char* b = lit;
            while (*b && *a == *b) { ++a; ++b; }
            if (*b) return false;
            r = a;
            return true;
        };
        if (word_is(q, "resume=")) {
            if (word_is(q, "off") || word_is(q, "0") || word_is(q, "false") || word_is(q, "no"))
                out.resume = false;
            else out.resume = true;
        } else if (word_is(q, "rate=")) {
            uint32_t v = 0;
            if (scan_uint(q, v) && v >= 8000u && v <= 192000u) out.rate = v;
        }
    }
}

} // namespace bard
} // namespace spotykach
