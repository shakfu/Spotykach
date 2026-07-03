// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Layer [2] codec: tokenize a received line into a Command{ verb, argv[] }, in place, bounded, no
// allocation. The transport hands us a NUL-terminated line buffer ('\r' trimmed, '\n' stripped); we
// overwrite each run of whitespace with '\0' and record argv pointers. See docs/dev/terminal-dispatch.md.

#include <cstddef>
#include <cstdint>

namespace spotykach {

struct Command {
    static constexpr uint8_t kMaxArgs = 6;   // verb + up to 5 args covers every phase-1 form
    const char* argv[kMaxArgs];
    uint8_t     argc = 0;
    const char* verb() const { return argc ? argv[0] : ""; }
    const char* arg(uint8_t i) const { return i < argc ? argv[i] : ""; }
};

// Split `line` on ' '/'\t' in place. Returns false if the line has more than kMaxArgs tokens
// (-> the dispatcher emits "err too-many-args"). Empty / all-whitespace lines return true with argc 0.
inline bool tokenize(char* line, Command& out) {
    out.argc = 0;
    char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;          // skip leading whitespace
        if (!*p) break;                               // trailing whitespace only
        if (out.argc >= Command::kMaxArgs) return false;  // too many tokens
        out.argv[out.argc++] = p;                     // token starts here
        while (*p && *p != ' ' && *p != '\t') ++p;    // advance to end of token
        if (*p) *p++ = '\0';                          // terminate token, step past the separator
    }
    return true;
}

}  // namespace spotykach
