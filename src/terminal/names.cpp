// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/names.h"

#if SPK_TERMINAL

#include "terminal/fmt.h"   // parse_i32 (numeric-id fallback)
#include <cstring>

#pragma GCC optimize("Os")

namespace spotykach {

// index == (uint8_t)ParamId (engine/engine_params.h). Keep in lockstep with the enum.
static const char* const kParamNames[] = {
    "pos", "fluxfb", "env", "envsize", "size", "win", "polyslice", "speed",
    "fluxint", "gritint", "fluxmix", "gritmix", "feedback", "mix", "modspeed", "modamp",
    "tempo", "clickmix", "panspeed", "panrange", "keyinterval", "crossfade", "altpos", "aux",
};
static_assert(sizeof(kParamNames) / sizeof(kParamNames[0]) == static_cast<size_t>(ParamId::Count),
              "kParamNames out of sync with ParamId");

// index == (uint8_t)ConfigId.
static const char* const kConfigNames[] = {
    "route", "modtype", "lfoshape", "mode", "startmodon", "sizemodon",
};
static_assert(sizeof(kConfigNames) / sizeof(kConfigNames[0]) == static_cast<size_t>(ConfigId::Count),
              "kConfigNames out of sync with ConfigId");

// Selector-int labels for describe (the ints `config` accepts, per the ConfigId comments in
// engine_params.h - the platform's switch-position selector, which the engine maps to its own enums).
static const char* const kConfigLabels[] = {
    "0:stereo 1:dmono 2:genstereo",   // route (global)
    "0:lfo 1:follow",                 // modtype
    "0:a 1:b",                        // lfoshape (engine owns the palette; generic)
    "0:slice 1:reel 2:drift",         // mode
    "0:off 1:on",                     // startmodon
    "0:off 1:on",                     // sizemodon
};
static_assert(sizeof(kConfigLabels) / sizeof(kConfigLabels[0]) == static_cast<size_t>(ConfigId::Count),
              "kConfigLabels out of sync with ConfigId");

bool param_from_token(const char* s, ParamId& out) {
    if (!s || !*s) return false;
    for (size_t i = 0; i < static_cast<size_t>(ParamId::Count); ++i)
        if (!strcmp(s, kParamNames[i])) { out = static_cast<ParamId>(i); return true; }
    int32_t id;   // numeric-id fallback
    if (parse_i32(s, id) && id >= 0 && id < static_cast<int32_t>(ParamId::Count)) {
        out = static_cast<ParamId>(id);
        return true;
    }
    return false;
}

bool config_from_token(const char* s, ConfigId& out) {
    if (!s || !*s) return false;
    for (size_t i = 0; i < static_cast<size_t>(ConfigId::Count); ++i)
        if (!strcmp(s, kConfigNames[i])) { out = static_cast<ConfigId>(i); return true; }
    int32_t id;
    if (parse_i32(s, id) && id >= 0 && id < static_cast<int32_t>(ConfigId::Count)) {
        out = static_cast<ConfigId>(id);
        return true;
    }
    return false;
}

const char* param_name(ParamId id) {
    size_t i = static_cast<size_t>(id);
    return i < static_cast<size_t>(ParamId::Count) ? kParamNames[i] : "";
}

const char* config_name(ConfigId id) {
    size_t i = static_cast<size_t>(id);
    return i < static_cast<size_t>(ConfigId::Count) ? kConfigNames[i] : "";
}

bool param_is_platform_owned(ParamId id) {
    switch (id) {
        case ParamId::Tempo:
        case ParamId::KeyInterval:
        case ParamId::ModSpeed:
            return true;
        default:
            return false;
    }
}

int32_t route_to_selector(Route r) {
    switch (r) {
        case Route::Stereo:           return 0;
        case Route::DoubleMono:       return 1;
        case Route::GenerativeStereo: return 2;
        default:                      return 0;
    }
}

bool param_is_global(ParamId id) {
    switch (id) {
        case ParamId::Tempo:
        case ParamId::ClickMix:
        case ParamId::PanSpeed:
        case ParamId::PanRange:
        case ParamId::KeyInterval:
        case ParamId::Crossfade:
            return true;
        default:
            return false;
    }
}

// Route is the channel topology for the whole instrument, not a property of either deck. The line
// codec has to accept a deck token here and discard it; the OSC address space encodes the distinction
// structurally instead (`/sk/cfg/route`, no deck segment), which is why it needs to ask.
bool config_is_global(ConfigId id) { return id == ConfigId::Route; }

void param_range(ParamId, float& lo, float& hi) {
    // Everything the engine surface actually carries is normalized 0..1. The two ids that were not
    // (Tempo 40..300, KeyInterval 1..64) are platform-owned and no longer advertised - and those display
    // units were never the units set_param takes, which is precisely why advertising them was wrong.
    lo = 0.f;
    hi = 1.f;
}

const char* config_labels(ConfigId id) {
    size_t i = static_cast<size_t>(id);
    return i < static_cast<size_t>(ConfigId::Count) ? kConfigLabels[i] : "";
}

}  // namespace spotykach

#endif  // SPK_TERMINAL
