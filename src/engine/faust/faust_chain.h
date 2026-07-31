// faust_chain.h - generic IEngine wrapper for TWO cyfaust kernels in SERIES (a dual-deck mode).
//
// Sibling of FaustEngine<Traits> for the "deck_mode: series" generated engine: two DISTINCT kernels
// chained A->B, with deck A's knob bank driving stage A and deck B's driving stage B. Covers FX->FX
// (e.g. drive->filter) and instrument->FX (a 0-input generator into an effect - stage A then feeds on
// silence). The chain is mono internally and duplicated to the stereo bus at the output.
//
// Traits must provide:
//   using StageA = <namespaced ::mydsp>;   using StageB = <namespaced ::mydsp>;
//   static const faustgen::Bind* binds_a(); static int nbinds_a();   // ParamId-keyed, deck A -> stage A
//   static const faustgen::Bind* binds_b(); static int nbinds_b();   // ParamId-keyed, deck B -> stage B
//   static constexpr Capabilities caps;     // includes CapDualDeck
//   static constexpr bool soft_limit;       // cubic soft-clip the output
//   static constexpr bool meter;            // own-display: output-peak level ring
//   static constexpr uint32_t color;
//
// set_param is uniform: it writes _role[deck][r], and because each deck's role table was bound to its own
// stage's zones at init(), the wrapper never has to special-case which kernel a knob reaches.

#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)
#include <cstddef>   // size_t (transitive-include hygiene; host build)

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
class FaustChainEngine : public IEngine {
    using StageA = typename Traits::StageA;
    using StageB = typename Traits::StageB;
    static constexpr int    kRoles    = static_cast<int>(ParamId::Count);
    static constexpr size_t kMaxBlock = 128;
    static constexpr int    kMaxCh    = 8;

public:
    FaustChainEngine() = default;
    ~FaustChainEngine() override = default;

    void init(const EngineContext& ctx) override {
        Arena ar(ctx.arena);
        void* ma = ar.alloc<uint8_t>(sizeof(StageA), alignof(StageA));
        void* mb = ar.alloc<uint8_t>(sizeof(StageB), alignof(StageB));
        if (!ma || !mb) return;
        _a = new (ma) StageA();
        _b = new (mb) StageB();
        _a->init(static_cast<int>(ctx.sample_rate));
        _b->init(static_cast<int>(ctx.sample_rate));
        { faustgen::CaptureUI<true> ui; ui.roles = _role[0]; ui.binds = Traits::binds_a(); ui.nbinds = Traits::nbinds_a(); _a->buildUserInterface(&ui); }
        { faustgen::CaptureUI<true> ui; ui.roles = _role[1]; ui.binds = Traits::binds_b(); ui.nbinds = Traits::nbinds_b(); _b->buildUserInterface(&ui); }
        for (int d = 0; d < 2; d++)
            for (int r = 0; r < kRoles; r++) if (_role[d][r].bound()) _v[d][r] = _role[d][r].def01();
        _ain  = _a->getNumInputs();
        _aout = _a->getNumOutputs();
    }

    void prepare() override {}

    void process(const float* const* in, float** out, size_t size) override {
        const size_t n = size > kMaxBlock ? kMaxBlock : size;
        if (!_a || !_b) { std::memset(out[0], 0, size * sizeof(float)); std::memset(out[1], 0, size * sizeof(float)); return; }

        // Stage A -> _mid (mono). An instrument (0 inputs) feeds on silence; an FX takes the left input.
        FAUSTFLOAT* ain[kMaxCh];
        FAUSTFLOAT* amid[kMaxCh];
        FAUSTFLOAT* asrc = const_cast<float*>((_ain > 0 && in && in[0]) ? in[0] : _zero);
        const int anin  = _ain  < kMaxCh ? _ain  : kMaxCh;
        const int anout = _aout < kMaxCh ? _aout : kMaxCh;
        for (int i = 0; i < anin;  i++) ain[i]  = (i == 0) ? asrc : _zero;
        for (int i = 0; i < anout; i++) amid[i] = (i == 0) ? _mid : _discard;
        _a->compute(static_cast<int>(n), ain, amid);
        if (anout <= 0) std::memset(_mid, 0, n * sizeof(float));

        // Stage B: _mid -> out[0] (mono), then dup to the stereo bus.
        FAUSTFLOAT* bin[kMaxCh];
        FAUSTFLOAT* bout[kMaxCh];
        for (int i = 0; i < kMaxCh; i++) bin[i]  = (i == 0) ? _mid   : _zero;
        for (int i = 0; i < kMaxCh; i++) bout[i] = (i == 0) ? out[0] : _discard;
        _b->compute(static_cast<int>(n), bin, bout);
        std::memcpy(out[1], out[0], n * sizeof(float));

        if constexpr (Traits::soft_limit) {
            for (size_t i = 0; i < n; i++) { out[0][i] = daisysp::SoftLimit(out[0][i]); out[1][i] = out[0][i]; }
        }
        if constexpr (Traits::meter) {
            float p = 0.f; for (size_t i = 0; i < n; i++) p = std::fmax(p, std::fabs(out[0][i]));
            _peak = p;
        }
    }

    Capabilities capabilities() const override { return Traits::caps; }

    void set_param(ParamId id, DeckRef::Ref deck, float v) override {
        const int r = static_cast<int>(id);
        if (r < 0 || r >= kRoles) return;
        const int d = (deck < 2) ? static_cast<int>(deck) : 0;
        _v[d][r] = v;
        if (_role[d][r].bound()) _role[d][r].set(v);
    }

    void set_mod_speed(DeckRef::Ref deck, float v, bool /*sync*/) override {
        const int r = static_cast<int>(ParamId::ModSpeed);
        const int d = (deck < 2) ? static_cast<int>(deck) : 0;
        _v[d][r] = v;
        if (_role[d][r].bound()) _role[d][r].set(v);
    }

    float param(ParamId id, DeckRef::Ref deck) const override {
        const int r = static_cast<int>(id);
        if (r < 0 || r >= kRoles) return 0.f;
        const int d = (deck < 2) ? static_cast<int>(deck) : 0;
        return _v[d][r];
    }

    void render(DisplayModel& m) override {
        m.clear();
        for (int d = 0; d < 2; d++) {
            if constexpr (Traits::meter) {
                const float lvl = _peak > 1.f ? 1.f : _peak;
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
    StageA*        _a = nullptr;
    StageB*        _b = nullptr;
    faustgen::Role _role[2][kRoles];
    float          _v[2][kRoles] = {};
    int            _ain = 0, _aout = 0;
    float          _peak = 0.f;
    float          _mid[kMaxBlock]  = {};
    float          _zero[kMaxBlock] = {};
    float          _discard[kMaxBlock];
};

} // namespace spotykach
