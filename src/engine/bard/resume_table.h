#pragma once

#include <cstdint>

// The `bard` engine's resume table: "where was I in each book". An LRU of the 64 most recently played
// books, serialized to /bard/resume.txt as one line per book:
//
//   0/HOBBIT1.WAV 84719232
//   0/DUNE.WAV 12006400
//
// The 64-entry cap is load-bearing, not decorative. IStreamDeck::read_text reads only the first max-1
// bytes and truncates silently, so the file must stay small enough to read in one go: 64 lines of about
// 28 bytes is under 2 KB, whereas an uncapped table over 16 shelves x 32 books would be ~14 KB and would
// lose its tail on every read.
//
// Self-contained (only <cstdint>) and allocation-free, like bookmarks.h, so the host test links it alone.

namespace spotykach {
namespace bard {

struct ResumeTable {
    static constexpr int kMax     = 64;   // LRU depth
    static constexpr int kKeyMax  = 20;   // "<shelf>/<NAME.WAV>" = 2 + 1 + 12 + NUL, rounded up
    static constexpr int kTextMax = 2048; // serialize() buffer size the caller should provide

    struct Entry {
        char     key[kKeyMax];
        uint32_t frame;
    };

    Entry entry[kMax] = {};
    int   count = 0;                      // entry[0] is the most recently touched

    void clear() { count = 0; }

    // Record `frame` for `key`, moving it to the front. A new key past kMax evicts the least recently
    // touched entry - the oldest book you have not opened is the right thing to forget.
    void set(const char* key, uint32_t frame) {
        const int at = _find(key);
        if (at >= 0) {
            entry[at].frame = frame;
            _promote(at);
            return;
        }
        if (count < kMax) count++;
        for (int i = count - 1; i > 0; i--) entry[i] = entry[i - 1];   // shift down, drop the tail
        _copy_key(entry[0].key, key);
        entry[0].frame = frame;
    }

    // Look up `key` without reordering (a read is not a use). false if the book has no stored position.
    bool get(const char* key, uint32_t& frame) const {
        const int at = _find(key);
        if (at < 0) return false;
        frame = entry[at].frame;
        return true;
    }

    // Parse "<key> <frame>" lines, most-recent-first (the order serialize() wrote). Blank lines, '#'
    // comments and unparseable lines are skipped rather than rejecting the file: a power cut during a
    // rewrite then costs at most the tail of the table, which is why no atomic rename is needed.
    int parse(const char* text) {
        clear();
        if (!text) return 0;
        const char* p = text;
        while (*p && count < kMax) {
            const char* line = p;
            while (*p && *p != '\n') ++p;
            const char* eol = p;
            if (*p == '\n') ++p;

            const char* q = line;
            while (q < eol && (*q == ' ' || *q == '\t' || *q == '\r')) ++q;
            if (q >= eol || *q == '#') continue;

            // key = up to the first space
            int k = 0;
            char key[kKeyMax];
            while (q < eol && *q != ' ' && *q != '\t' && *q != '\r' && k < kKeyMax - 1) key[k++] = *q++;
            key[k] = '\0';
            if (k == 0) continue;
            while (q < eol && (*q == ' ' || *q == '\t')) ++q;
            if (q >= eol || *q < '0' || *q > '9') continue;

            uint32_t f = 0;
            bool sane = true;
            int digits = 0;
            while (q < eol && *q >= '0' && *q <= '9') {
                if (++digits > 10) { sane = false; break; }            // > uint32 range - drop the line
                f = f * 10u + static_cast<uint32_t>(*q - '0');
                ++q;
            }
            if (!sane) continue;

            _copy_key(entry[count].key, key);                          // append: input is already LRU order
            entry[count].frame = f;
            count++;
        }
        return count;
    }

    // Write the table as text into `out` (NUL-terminated), most-recent-first. Returns the byte count
    // excluding the NUL, or 0 if the buffer is too small for even one line.
    int serialize(char* out, int max) const {
        if (!out || max <= 1) return 0;
        int n = 0;
        for (int i = 0; i < count; i++) {
            char line[kKeyMax + 16];
            int  m = 0;
            for (const char* s = entry[i].key; *s && m < kKeyMax; ++s) line[m++] = *s;
            line[m++] = ' ';
            m += _utoa(entry[i].frame, line + m);
            line[m++] = '\n';
            if (n + m >= max) break;                                   // stop cleanly at the cap
            for (int j = 0; j < m; j++) out[n + j] = line[j];
            n += m;
        }
        out[n] = '\0';
        return n;
    }

private:
    static void _copy_key(char* dst, const char* src) {
        int i = 0;
        for (; src[i] && i < kKeyMax - 1; i++) dst[i] = src[i];
        dst[i] = '\0';
    }
    static bool _key_eq(const char* a, const char* b) {
        for (int i = 0; i < kKeyMax; i++) {
            if (a[i] != b[i]) return false;
            if (a[i] == '\0') return true;
        }
        return true;
    }
    static int _utoa(uint32_t v, char* out) {
        char tmp[12];
        int  n = 0;
        do { tmp[n++] = static_cast<char>('0' + (v % 10u)); v /= 10u; } while (v);
        for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
        return n;
    }
    int _find(const char* key) const {
        for (int i = 0; i < count; i++) if (_key_eq(entry[i].key, key)) return i;
        return -1;
    }
    void _promote(int at) {
        if (at <= 0) return;
        const Entry e = entry[at];
        for (int i = at; i > 0; i--) entry[i] = entry[i - 1];
        entry[0] = e;
    }
};

} // namespace bard
} // namespace spotykach
