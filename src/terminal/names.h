// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Named addressing for params/config (part of SPK_TERMINAL, always on when the channel is): the flat
// id<->name table plus the describe metadata (scope + range + config labels). Names survive an enum
// reorder and are the whole point of a human-typable channel; numeric ids are accepted as a fallback.
// See docs/dev/terminal-dispatch.md.

#include "engine/engine_params.h"
#include "engine/mode.h"          // Route (for route_to_selector)

namespace spotykach {

// Lookups (accept a name, or a numeric id as fallback). Return false on no match.
bool param_from_token (const char* s, ParamId& out);
bool config_from_token(const char* s, ConfigId& out);

const char* param_name (ParamId id);    // "" if out of range
const char* config_name(ConfigId id);

// describe metadata (platform-owned static tables).
bool        param_is_global(ParamId id);            // global vs per-deck (a property of the id)
bool        config_is_global(ConfigId id);          // Route only; the rest are per-deck

// True for ParamIds the PLATFORM owns and never forwards to IEngine::set_param, so `describe` must not
// advertise them: a host sweep would set them and read back whatever the engine happens to store,
// asserting nothing. Verified against every _engine.set_param() call site in src/ui/:
//   Tempo, KeyInterval - live in the Transport service (the UI keeps them normalized via
//                        tempo_abs_to_norm / set_key_tick_interval_norm and drives Transport directly).
//   ModSpeed           - reaches the engine through set_mod_speed(deck, value, sync), not set_param,
//                        so the `set modspeed` verb is its real path.
// They stay ADDRESSABLE by name (an engine may consume them later, and refusing them would be a silent
// protocol change); they are simply not advertised.
bool        param_is_platform_owned(ParamId id);

// Map an IEngine::route() value to the selector int `config route` accepts. The two encodings differ
// (Route{DoubleMono=1,Stereo=2,GenerativeStereo=3} vs the switch selector 0/1/2) and describe publishes
// only the selector one - so query has to speak it too, or a host cannot round-trip route.
int32_t     route_to_selector(Route r);
void        param_range(ParamId id, float& lo, float& hi);
const char* config_labels(ConfigId id);             // "0:slice 1:reel 2:drift" (selector ints)

}  // namespace spotykach
