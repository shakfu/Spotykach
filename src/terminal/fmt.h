// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Value coercion at the codec/dispatch boundary. Input parsing may use strtof/strtol (libc, linked;
// the "%f" restriction is a *print* concern, not parse). Output formatting lives on TextSink
// (engine/terminal_io.h) and never uses printf("%f"). See docs/dev/terminal-dispatch.md.

#include <cstdint>
#include "engine/deck_ref.h"

namespace spotykach {

bool parse_f32(const char* s, float& out);            // strtof + finite check, whole-token
bool parse_i32(const char* s, int32_t& out);          // strtol base 0 (0x.. ok), whole-token
bool parse_deck(const char* s, DeckRef::Ref& out);    // "A"/"a"->A, "B"/"b"->B
bool parse_onoff(const char* s, bool& out);           // "on"/"1"->true, "off"/"0"->false

}  // namespace spotykach
