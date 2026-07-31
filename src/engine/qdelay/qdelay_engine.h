// SPDX-License-Identifier: GPL-3.0-only
//
// This engine is GPLv3, NOT MIT like the rest of this repository: it incorporates the GPLv3 diffuser
// (src/dsp/diffuser.h, ported from https://github.com/tiagolr/qdelay), so a build with ENGINE=qdelay
// is a combined work distributed under GPLv3. See src/engine/qdelay/{NOTICE.md,LICENSE}.
#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "dsp/diffuser.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

// QDelay - a second "flavor" of the dual delay, inspired by qdelay (github.com/tiagolr/qdelay). Shares the delay
// engine's control grammar (tempo-synced division, feedback, mix, PITCH transpose, ENV tone, the mod
// LFO, the Reverse Rev-pad gesture and the three stereo topologies) but swaps the CHARACTER palette:
//
//   Mode switch (ConfigId::Mode, per deck) -> character: Clean / Diffuse / Duck
//
//   Clean   - clean digital repeats (identical to the delay engine's Clean).
//   Diffuse - the feedback runs through an 8-stage allpass diffuser (dsp/diffuser.h, a JUCE-free port
//             of qdelay's Diffusor): repeats progressively smear into a dense, reverb-like dub wash.
//   Duck    - the heard wet ducks under the dry input (a per-tap peak follower attenuates the wet send
//             while you play, so the repeats stay out of the way and bloom in the gaps).
//
// Everything else mirrors DelayEngine: SIZE->division, POS->feedback, SOS->mix, PITCH->+/-1 octave
// output transpose, ENV->feedback tone, MODFREQ/MOD_AMT->delay-time mod LFO, Play pad->Freeze, Rev
// pad->Reverse, Route->Stereo/DoubleMono/Ping-pong. Delay lines + the diffuser live in borrowed SDRAM.
class QdelayEngine : public IEngine {
public:
    enum Mode : uint8_t { Clean = 0, Diffuse, Duck, ModeCount };

    QdelayEngine() = default;
    ~QdelayEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override {}
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override { return CapOwnDisplay | CapDualDeck; }

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md): the ids this engine actually
    // consumes, so a host sweep exercises only real parameters instead of the whole ParamId enum.
    // Derived from the engine's own ParamId/ConfigId use; NOT yet verified on hardware.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::ModAmp))
             | (1u << static_cast<uint32_t>(ParamId::Env));
    }
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>((1u << static_cast<uint32_t>(ConfigId::Route))
                                     | (1u << static_cast<uint32_t>(ConfigId::Mode)));
    }
#endif

    void  set_param(ParamId id, DeckRef::Ref deck, float value) override;
    float param(ParamId id, DeckRef::Ref deck) const override;
    void  set_mod_speed(DeckRef::Ref deck, float value, bool sync) override; // MODFREQ -> mod LFO rate
    bool  on_play_pad(DeckRef::Ref deck, bool reverse) override;             // Play -> Freeze, Rev -> Reverse
    bool  set_config(ConfigId id, DeckRef::Ref deck, int value) override;    // Mode -> character; Route -> topology
    Route route() const override { return _route; }
    void  render(DisplayModel& m) override;

private:
    NOCOPY(QdelayEngine)

    // Crossfading two-head delay-line pitch shifter for the PITCH output transpose (transparent at unity).
    struct Shifter {
        static constexpr size_t kWin = 2048;
        float  buf[kWin] = {};
        size_t w = 0;
        float  phase = 0.f;
        float  process(float x, float ratio);
        float  read(float off) const;
        void   clear() { for (size_t i = 0; i < kWin; i++) buf[i] = 0.f; w = 0; phase = 0.f; }
    };

    // One delay line + its smoothed controls. read_color()/write_out() are split so the engine can run
    // the (stereo) diffuser over both taps' feedback between the read and the write.
    struct Tap {
        float*  buf = nullptr;
        size_t  len = 0, w = 0;
        float   sr = 48000.f, min_d = 1.f, max_d = 1.f;
        int     div = 0;
        float   beats = 0.25f, target_d = 1.f;
        float   fb = 0.f, mix = 0.f, ratio = 1.f, tone_coef = 1.f;
        uint8_t mode = Clean;
        float   s_delay = 1.f, s_fb = 0.f, s_mix = 0.f, s_ratio = 1.f;
        float   tone_lp = 0.f;
        float   mod_ph = 0.f, mod_rate = 0.f, mod_depth = 0.f;
        bool    frozen = false;
        bool    reversed = false;
        float   rev_ph = 0.f;
        float   duck_env = 0.f;   // per-tap input peak envelope (drives the Duck attenuation)
        float   peak = 0.f;
        float   _x = 0.f, _wet = 0.f;
        Shifter out_shift;

        void  init(void* mem, float sample_rate, size_t length);
        void  set_div(float norm);
        void  set_tone(float env);
        void  set_target(float bpm);
        float read_buf(float off) const;
        float read_rev(float win);
        float read_color(float x_in);          // smooth + read the tap + tone LP -> the feedback signal
        float duck_gain() const;               // 1 (no duck) .. floor, from duck_env when mode == Duck
        float write_out(float fbsig);          // write x + fb*fbsig; return the (ducked) wet/dry mix
    };

    static DeckRef::Ref _safe(DeckRef::Ref d) { return d < DeckRef::Count ? d : DeckRef::A; }
    void apply_params(Tap& t, DeckRef::Ref src);

    const ITransport* _transport = nullptr;
    Tap      _tap[DeckRef::Count];
    Diffuser _diffuser;                                  // shared stereo feedback diffuser (Diffuse mode)
    uint8_t  _mode[DeckRef::Count]    = { Clean, Clean };
    float    _mod_rate[DeckRef::Count] = { 0.f, 0.f };
    bool     _freeze[DeckRef::Count]   = { false, false };
    bool     _reverse[DeckRef::Count]  = { false, false };
    Route    _route = Route::Stereo;
    float    _param[static_cast<size_t>(ParamId::Count)][DeckRef::Count] = {};
};

};
