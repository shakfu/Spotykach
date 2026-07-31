// faust_fx.h - generic IEngine wrapper for a SINGLE cyfaust-generated kernel.
//
// One IEngine implementation shared by every simple Faust engine (the analogue of GenEngine<W> for
// gen~). It is parameterized by a per-engine Traits struct (emitted by the generator from the .dsp +
// manifest) that names the kernel type, the ParamId->slider Bind table, the capabilities, and a few
// optional feature flags. The kernel is placement-new'd into the SDRAM arena at init() (its delay-line
// state is far too big for SRAM); set_param writes 0..1 platform values through the captured zones.
//
// Traits must provide:
//   using Kernel = <namespaced ::mydsp>;
//   static const faustgen::Bind* binds();   static int nbinds();   // ParamId-keyed bind table
//   static constexpr Capabilities caps;
//   static constexpr int  decks;          // 1 = single control set; 2 = DoubleMono (deck A=L, B=R)
//   static constexpr int  wet_dry_role;   // -1, or (int)ParamId for a SOFTWARE dry/wet crossfade
//   static constexpr bool soft_limit;     // cubic soft-clip the stereo bus
//   static constexpr bool meter;          // own-display: output-peak level ring
//   static constexpr uint32_t color;      // ring colour when meter
//
// decks==2 (parallel / DoubleMono): two instances of the SAME mono kernel, deck A on the left channel
// and deck B on the right, each with its own knob bank - the reverb DoubleMono shape, generated. The
// two never interact (the crossfader is unused). The kernel must be mono (1-in/1-out). decks==1 keeps the
// v1 path (mono/stereo/0-in marshalling) byte-for-byte: the per-deck arrays are size 1 and collapse away.

#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "engine/indicators.h"   // shared indicator toolkit (docs/dev/indicator-comparison.md §7)
#include "engine/arena.h"
#include "engine/faust/faust_capture.h"
#include "daisysp.h"   // daisysp::SoftLimit (soft_limit feature)

#include <new>
#include <cstring>
#include <cmath>

namespace spotykach {

template <class Traits>
class FaustEngine : public IEngine {
    using Kernel = typename Traits::Kernel;
    static constexpr int    kRoles    = static_cast<int>(ParamId::Count);
    static constexpr int    kDecks    = Traits::decks;   // 1 (single) or 2 (parallel DoubleMono)
    static constexpr size_t kMaxBlock = 128;   // platform block is 96
    static constexpr int    kMaxCh    = 8;     // Faust kernels are small; first 2 map to the stereo bus

public:
    FaustEngine() = default;
    ~FaustEngine() override = default;

    void init(const EngineContext& ctx) override {
        Arena ar(ctx.arena);
        for (int d = 0; d < kDecks; d++) {
            if (void* m = ar.alloc<uint8_t>(sizeof(Kernel), alignof(Kernel))) _k[d] = new (m) Kernel();
            if (!_k[d]) return;
            _k[d]->init(static_cast<int>(ctx.sample_rate));
            faustgen::CaptureUI<true> ui;  // generator path: capture slider defaults to seed the boot cache
            ui.roles = _role[d]; ui.binds = Traits::binds(); ui.nbinds = Traits::nbinds();
            _k[d]->buildUserInterface(&ui);
            // Seed the param cache from each bound slider's own default, so param() reports the kernel's
            // real boot state and the platform's pickup seeds with no jump.
            for (int r = 0; r < kRoles; r++) if (_role[d][r].bound()) _v[d][r] = _role[d][r].def01();
        }
        _nin  = _k[0]->getNumInputs();
        _nout = _k[0]->getNumOutputs();
    }

    void prepare() override {}

    void process(const float* const* in, float** out, size_t size) override {
        const size_t n = size > kMaxBlock ? kMaxBlock : size;
        if (!_k[0]) { std::memset(out[0], 0, size * sizeof(float)); std::memset(out[1], 0, size * sizeof(float)); return; }

        if constexpr (kDecks >= 2) {
            // DoubleMono: deck d drives channel d through its own kernel instance (mono 1-in/1-out).
            for (int d = 0; d < kDecks && d < 2; d++) {
                FAUSTFLOAT* ci[kMaxCh];
                FAUSTFLOAT* co[kMaxCh];
                const int nin  = _nin  < kMaxCh ? _nin  : kMaxCh;
                const int nout = _nout < kMaxCh ? _nout : kMaxCh;
                FAUSTFLOAT* src = const_cast<float*>((in && in[d]) ? in[d] : _zero);
                for (int i = 0; i < nin;  i++) ci[i] = (i == 0) ? src    : _zero;
                for (int i = 0; i < nout; i++) co[i] = (i == 0) ? out[d] : _discard;
                _k[d]->compute(static_cast<int>(n), ci, co);
                if (nout <= 0) std::memset(out[d], 0, n * sizeof(float));
                if constexpr (Traits::soft_limit)
                    for (size_t i = 0; i < n; i++) out[d][i] = daisysp::SoftLimit(out[d][i]);
                if constexpr (Traits::meter) {
                    float p = 0.f; for (size_t i = 0; i < n; i++) p = std::fmax(p, std::fabs(out[d][i]));
                    _peak[d] = p;
                }
            }
            return;
        }

        // Single control set (v1): general mono/stereo/0-in marshalling.
        FAUSTFLOAT* cin[kMaxCh];
        FAUSTFLOAT* cout[kMaxCh];
        const int nin  = _nin  < kMaxCh ? _nin  : kMaxCh;
        const int nout = _nout < kMaxCh ? _nout : kMaxCh;
        for (int i = 0; i < nin;  i++) cin[i]  = const_cast<float*>((i < 2 && in) ? in[i] : _zero);
        for (int i = 0; i < nout; i++) cout[i] = (i < 2) ? out[i] : _discard;

        _k[0]->compute(static_cast<int>(n), cin, cout);

        if (nout == 1) std::memcpy(out[1], out[0], n * sizeof(float));       // mono kernel -> dup to stereo
        else if (nout <= 0) { std::memset(out[0], 0, n * sizeof(float)); std::memset(out[1], 0, n * sizeof(float)); }

        if constexpr (Traits::wet_dry_role >= 0) {                           // software dry/wet crossfade
            if (in && in[0] && in[1]) {
                const float w = _v[0][Traits::wet_dry_role], d = 1.f - w;
                for (size_t i = 0; i < n; i++) { out[0][i] = w * out[0][i] + d * in[0][i];
                                                 out[1][i] = w * out[1][i] + d * in[1][i]; }
            }
        }
        if constexpr (Traits::soft_limit) {
            for (size_t i = 0; i < n; i++) { out[0][i] = daisysp::SoftLimit(out[0][i]);
                                             out[1][i] = daisysp::SoftLimit(out[1][i]); }
        }
        if constexpr (Traits::meter) {
            float p = 0.f;
            for (size_t i = 0; i < n; i++) { p = std::fmax(p, std::fabs(out[0][i])); p = std::fmax(p, std::fabs(out[1][i])); }
            _peak[0] = p;
        }
    }

    Capabilities capabilities() const override { return Traits::caps; }

    void set_param(ParamId id, DeckRef::Ref deck, float v) override {
        const int r = static_cast<int>(id);
        if (r < 0 || r >= kRoles) return;
        const int d = (kDecks > 1 && deck < kDecks) ? static_cast<int>(deck) : 0;
        _v[d][r] = v;
        if (_role[d][r].bound()) _role[d][r].set(v);
    }

    // The MODFREQ knob is delivered here (not via set_param), so a binding onto ParamId::ModSpeed - the
    // natural home for a Faust LFO-rate slider - works. A no-op if nothing binds ModSpeed.
    void set_mod_speed(DeckRef::Ref deck, float v, bool /*sync*/) override {
        const int r = static_cast<int>(ParamId::ModSpeed);
        const int d = (kDecks > 1 && deck < kDecks) ? static_cast<int>(deck) : 0;
        _v[d][r] = v;
        if (_role[d][r].bound()) _role[d][r].set(v);
    }

    float param(ParamId id, DeckRef::Ref deck) const override {
        const int r = static_cast<int>(id);
        if (r < 0 || r >= kRoles) return 0.f;
        const int d = (kDecks > 1 && deck < kDecks) ? static_cast<int>(deck) : 0;
        return _v[d][r];
    }

    void render(DisplayModel& m) override {
        m.clear();
        for (int d = 0; d < 2; d++) {
            if constexpr (Traits::meter) {
                const float raw = _peak[(kDecks > 1) ? d : 0];
                const float lvl = raw > 1.f ? 1.f : raw;
                ring::level(m.ring[d], lvl, Traits::color);
                m.play[d] = { Traits::color, lvl > 1e-4f ? 1.f : 0.f };
            } else {
                // No meter configured: a dim mode-hued "on, ready" floor + play dot so the panel isn't
                // dark (no ITimeSource in this render, so a static glow rather than a breathe).
                ring::level(m.ring[d], 0.999f, Traits::color, 0.12f);
                m.play[d] = { Traits::color, 0.3f };
            }
            m.ring[d].set_updated();
        }
    }

private:
    Kernel*        _k[kDecks] = {};
    faustgen::Role _role[kDecks][kRoles];
    float          _v[kDecks][kRoles] = {};
    int            _nin = 0, _nout = 0;
    float          _peak[kDecks] = {};
    float          _zero[kMaxBlock] = {};
    float          _discard[kMaxBlock];
};

} // namespace spotykach
