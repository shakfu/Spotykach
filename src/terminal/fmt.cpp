// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/fmt.h"

#if SPK_TERMINAL

#include <cstdlib>   // strtof, strtol
#include <cmath>     // isfinite
#include <cstring>   // strcmp

#pragma GCC optimize("Os")

namespace spotykach {

bool parse_f32(const char* s, float& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    float v = strtof(s, &end);
    if (end == s || *end != '\0') return false;   // no digits, or trailing garbage
    if (!std::isfinite(v)) return false;           // reject nan/inf
    out = v;
    return true;
}

bool parse_i32(const char* s, int32_t& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    long v = strtol(s, &end, 0);                   // base 0: accepts 0x.., 0.. , decimal
    if (end == s || *end != '\0') return false;
    out = static_cast<int32_t>(v);
    return true;
}

bool parse_deck(const char* s, DeckRef::Ref& out) {
    if (!s || !s[0] || s[1] != '\0') return false;   // exactly one char
    switch (s[0]) {
        case 'A': case 'a': out = DeckRef::A; return true;
        case 'B': case 'b': out = DeckRef::B; return true;
        default: return false;
    }
}

bool parse_onoff(const char* s, bool& out) {
    if (!s) return false;
    if (!strcmp(s, "on")  || !strcmp(s, "1")) { out = true;  return true; }
    if (!strcmp(s, "off") || !strcmp(s, "0")) { out = false; return true; }
    return false;
}

}  // namespace spotykach

#endif  // SPK_TERMINAL
