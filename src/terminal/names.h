// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Named addressing for params/config (part of SPK_TERMINAL, always on when the channel is): the flat
// id<->name table plus the describe metadata (scope + range + config labels). Names survive an enum
// reorder and are the whole point of a human-typable channel; numeric ids are accepted as a fallback.
// See docs/dev/terminal-dispatch.md.

#include "engine/engine_params.h"

namespace spotykach {

// Lookups (accept a name, or a numeric id as fallback). Return false on no match.
bool param_from_token (const char* s, ParamId& out);
bool config_from_token(const char* s, ConfigId& out);

const char* param_name (ParamId id);    // "" if out of range
const char* config_name(ConfigId id);

// describe metadata (platform-owned static tables).
bool        param_is_global(ParamId id);            // global vs per-deck (a property of the id)
void        param_range(ParamId id, float& lo, float& hi);
const char* config_labels(ConfigId id);             // "0:slice 1:reel 2:drift" (selector ints)

}  // namespace spotykach
