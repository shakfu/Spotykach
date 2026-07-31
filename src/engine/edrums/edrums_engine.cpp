// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "engine/edrums/edrums_engine.h"

#include <cmath>
#include <algorithm>
#include <functional>

#ifndef TEST
#include "daisy_seed.h"   // target only: daisy::QSPIHandle / PersistentStorage for the kit preset
#endif

#include "engine/indicators.h"   // shared indicator toolkit (docs/dev/indicator-comparison.md §7)

using namespace spotykach;

constexpr uint8_t EdrumsEngine::kDivTable[3];
constexpr char    EdrumsEngine::kKitSlug[4];

#ifndef TEST
namespace {
// QSPI kit-preset store. Offset 0x10000 (64 KB) is a clean, separate erase block from the calibration
// Settings (QSPI offset 0) and sits well below the SRAM-boot app image (QSPI offset 0x40000). The
// template Version is pinned at 1 FOREVER -- bumping it triggers libDaisy's bkpt-on-version-mismatch;
// layout changes are versioned by KitData::version instead (apply() rejects a mismatch -> defaults).
constexpr uint32_t kKitQspiOffset = 0x10000;
using KitStore = daisy::PersistentStorage<EdrumsEngine::KitData, EdrumsEngine::kKitSlug, 1>;
KitStore g_kit_store;

void kit_load(void* qspi_ptr, EdrumsEngine& eng) {
    if (!qspi_ptr) return;
    EdrumsEngine::KitData defaults;   // FACTORY seed; only applied if a USER save already exists in flash
    g_kit_store.Init(*static_cast<daisy::QSPIHandle*>(qspi_ptr), defaults, kKitQspiOffset);
    if (g_kit_store.GetState() == KitStore::State::USER)
        eng.apply(g_kit_store.GetSettings());
}
void kit_save(const EdrumsEngine::KitData& kd) { g_kit_store.Save(kd); }
} // namespace
#endif

// The 5 drum models (Alt+PITCH selects). Each sets the voice character; PITCH still drives body freq +
// noise centre and SOS the decay, within the model. Fields:
//   tone        body <-> noise balance (0 body .. 1 noise)
//   sweep       pitch-drop amount (start freq = base*(1+sweep))      pitch_ms  pitch-envelope time
//   base_scale  per-model frequency offset on the shared PITCH knob (so each drum tunes to its range)
//   decay_scale body decay multiplier        noise_decay  noise decay relative to the body (snap vs ring)
//   noise_mult  noise centre/corner = base x this    noise_q  filter Q    noise_hp  high-pass (hats) vs band-pass
//   partial_ratio/partial_mix  a 2nd detuned body sine (snare/tom richness; 0 = off)
//   drive       gentle body saturation (0 = clean)   click  attack-click amount (kick beater / tom mallet)
//   bursts/burst_ms  the Clap's extra noise bursts
namespace {
struct ModelSpec {
    float tone; float sweep; float pitch_ms; float base_scale;
    float decay_scale; float noise_decay; float noise_mult; float noise_q; bool noise_hp;
    float partial_ratio; float partial_mix; float drive; float click;
    int bursts; float burst_ms;
};
const ModelSpec kModels[] = {
    //  tone  sweep p_ms  base   dscl  ndcy  nmult  nq    hp     prat  pmix  drive click  burst
    { 0.00f, 3.0f, 55.f, 0.50f, 1.30f, 1.0f,  8.f, 1.2f, false, 0.0f, 0.00f, 0.35f, 0.5f, 0,  0.f }, // 0 Kick  - deep body, long pitch drop, beater click + drive
    { 0.50f, 0.8f, 18.f, 1.30f, 0.55f, 1.6f, 18.f, 0.8f, false, 1.6f, 0.50f, 0.10f, 0.0f, 0,  0.f }, // 1 Snare - 2-partial body + bright noise that rings past the body
    { 0.92f, 0.0f, 10.f, 1.00f, 0.40f, 1.5f,  9.f, 1.0f, false, 0.0f, 0.00f, 0.00f, 0.0f, 3, 11.f }, // 2 Clap  - 3 extra noise bursts ~11 ms apart, tail rings
    { 1.00f, 0.0f,  5.f, 1.00f, 0.22f, 1.0f, 30.f, 0.9f, true,  0.0f, 0.00f, 0.00f, 0.0f, 0,  0.f }, // 3 Hat   - high-passed noise (metallic "tss"), very short
    { 0.18f, 1.8f, 40.f, 0.90f, 1.00f, 0.8f,  6.f, 1.2f, false, 1.5f, 0.35f, 0.15f, 0.2f, 0,  0.f }, // 4 Tom   - 2-partial pitched body, mild drop, mallet click
};

// The factory kit: per [deck][slot] the boot model + RNG seed + default POS(density)/PITCH/decay. Backs
// both init() and clear_sequence() (reset-to-defaults), so a reset reproduces the exact shipped kit.
// Deck A = kick/tom (low spine), deck B = snare/hat (backbeat); pos=0 -> boots silent.
struct BootDrum { int model; uint32_t seed; float pos, pitch, decay; };
const BootDrum kBootKit[2][2] = {
    { { 0 /*kick */, 0x9e3779b9u, 0.f, 0.50f, 0.50f }, { 4 /*tom */, 0x85ebca6bu, 0.f, 0.40f, 0.55f } },
    { { 1 /*snare*/, 0x6d2b79f5u, 0.f, 0.50f, 0.50f }, { 3 /*hat */, 0xc2b2ae35u, 0.f, 0.82f, 0.30f } },
};
}

// --- Voice: one synthesized drum ----------------------------------------------------------------
void EdrumsEngine::Voice::init(float sample_rate, float default_hz, int default_model, uint32_t seed)
{
    sr = sample_rate;
    osc.init(sr);
    osc2.init(sr);
    base_hz    = default_hz;     // placeholder; _recompute_pitch (via set_model) sets the real value
    pitch_norm = 0.5f;
    rng        = seed;
    decay_norm = 0.5f;           // sensible default until the SOS knob applies
    set_model(default_model);    // sets the model fields + pitch/click coefs + decay + base + noise filter
    amp = namp = pitch_env = click_amp = 0.f;
}

void EdrumsEngine::Voice::set_model(int idx)
{
    idx = std::clamp(idx, 0, EdrumsEngine::kModelCount - 1);
    const ModelSpec& m = kModels[idx];
    tone = m.tone; sweep = m.sweep; base_scale = m.base_scale;
    decay_scale = m.decay_scale; noise_decay = m.noise_decay; noise_mult = m.noise_mult;
    noise_q = m.noise_q; noise_hp = m.noise_hp;
    partial_ratio = m.partial_ratio; partial_mix = m.partial_mix; drive = m.drive; click_level = m.click;
    burst_count = m.bursts;
    burst_gap   = m.burst_ms * 0.001f * sr;
    pitch_coef  = std::exp(-1.f / (std::max(1.f, m.pitch_ms) * 0.001f * sr)); // per-model pitch sweep time
    click_coef  = std::exp(-1.f / (0.002f * sr));                            // ~2 ms attack click
    _recompute_decay();
    _recompute_pitch();   // base_scale may have changed -> rescale base + refresh the noise filter
}

void EdrumsEngine::Voice::_set_noise_filter()
{
    // The brightness macro (flux+SOS) shifts the centre/corner +/- 2 octaves around the model default.
    const float fc = base_hz * noise_mult * std::exp2((bright_macro - 0.5f) * 4.f);
    const float c  = std::clamp(fc, 300.f, sr * 0.45f);
    const auto type = noise_hp ? infrasonic::BiquadSection::FilterType::HighPass
                               : infrasonic::BiquadSection::FilterType::BandPass;
    noise_filt.SetCoefficients(infrasonic::BiquadSection::CalculateCoefficients(type, sr, c, noise_q));
}

void EdrumsEngine::Voice::_recompute_pitch()
{
    base_hz = 30.f * std::exp2(std::clamp(pitch_norm, 0.f, 1.f) * 4.f) * base_scale; // ~30..480 Hz x model offset
    _set_noise_filter();                                                             // noise colour tracks pitch
}

void EdrumsEngine::Voice::_recompute_decay()
{
    const float T = (0.03f + decay_norm * decay_norm * 1.2f) * decay_scale; // body: 30 ms .. ~1.2 s, scaled
    amp_coef  = std::exp(-1.f / (T * sr));
    const float Tn = std::max(0.005f, T * noise_decay);                     // noise: its own time (snap vs ring)
    namp_coef = std::exp(-1.f / (Tn * sr));
}

void EdrumsEngine::Voice::set_pitch(float norm)
{
    pitch_norm = std::clamp(norm, 0.f, 1.f);
    _recompute_pitch();
}

void EdrumsEngine::Voice::set_decay(float norm)
{
    decay_norm = std::clamp(norm, 0.f, 1.f);
    _recompute_decay();
}

// Live performance macros (grit/flux modifier knobs), each a bipolar offset on the model baseline
// (0.5 = neutral). drive/sweep/tone are read in process(); brightness recomputes the noise filter.
void EdrumsEngine::Voice::set_macro(int which, float v)
{
    v = std::clamp(v, 0.f, 1.f);
    switch (which) {
        case 0: drive_macro  = v; break;            // saturation
        case 1: sweep_macro  = v; break;            // pitch-drop amount
        case 2: tone_macro   = v; break;            // body <-> noise balance
        case 3: bright_macro = v; _set_noise_filter(); break; // noise cutoff
        default: break;
    }
}

void EdrumsEngine::Voice::trigger()
{
    amp        = 1.f;
    namp       = 1.f;
    pitch_env  = 1.f;
    click_amp  = click_level;
    osc.reset();
    osc2.reset();
    burst_left  = burst_count;   // schedule the clap's extra bursts (0 for other models)
    burst_timer = burst_gap;
}

float EdrumsEngine::Voice::process()
{
    // Multi-burst (Clap): re-arm body + noise envelopes at each scheduled burst for the stutter.
    if (burst_left > 0) {
        burst_timer -= 1.f;
        if (burst_timer <= 0.f) { amp = 1.f; namp = 1.f; burst_left--; burst_timer += burst_gap; }
    }

    if (amp < 1.0e-4f && namp < 1.0e-4f && click_amp < 1.0e-4f) { amp = namp = click_amp = 0.f; return 0.f; }

    // Performance macros offset the model baseline (0.5 = neutral): sweep x +/-2 octaves, drive +/-1,
    // tone (body<->noise) +/-0.5. Read live here so a knob move reshapes the next sample.
    const float eff_sweep = sweep * std::exp2((sweep_macro - 0.5f) * 4.f);
    const float eff_drive = std::clamp(drive + (drive_macro - 0.5f) * 2.f, 0.f, 1.5f);

    // Body: pitched sine (+ optional 2nd detuned partial) whose frequency starts (1+sweep)x base and
    // decays to base (the drum "thump"); gently saturated for harmonics/punch; own amp env.
    pitch_env *= pitch_coef;
    const float f = base_hz * (1.f + pitch_env * eff_sweep);
    osc.set_freq(f);
    float body = osc.process();
    if (partial_mix > 0.f) { osc2.set_freq(f * partial_ratio); body += osc2.process() * partial_mix; }
    if (eff_drive > 0.f) body = daisysp::SoftLimit(body * (1.f + eff_drive * 4.f));
    body *= amp;

    // Noise: cheap LCG -> -1..1, band/high-passed (the filter attenuates, so compensate), own decay.
    rng = rng * 1664525u + 1013904223u;
    const float n_raw = static_cast<float>(rng >> 8) * (1.f / 8388608.f) - 1.f;
    const float n = noise_filt.Process(n_raw, 0) * 2.f * namp;

    // Attack click: a brief bright transient at onset (kick beater / tom mallet).
    float clk = 0.f;
    if (click_amp > 1.0e-4f) { clk = n_raw * click_amp; click_amp *= click_coef; }
    else click_amp = 0.f;

    amp  *= amp_coef;
    namp *= namp_coef;
    const float eff_tone = std::clamp(tone + (tone_macro - 0.5f), 0.f, 1.f); // body <-> noise balance offset
    return body * (1.f - eff_tone) + n * eff_tone + clk;
}

// --- EdrumsEngine -------------------------------------------------------------------------------
void EdrumsEngine::init(const EngineContext& ctx)
{
    // Subscribe to the platform clock: the sequencer steps off its ticks (tempo comes from there too).
    _transport = ctx.transport;
    _time      = ctx.time;
    _qspi      = ctx.qspi;
    using namespace std::placeholders;
    _transport->set_on_tick(std::bind(&EdrumsEngine::_on_tick, this, _1));

    _sr = ctx.sample_rate;
    // Four drums = two decks x two slots, seeded from kBootKit (the factory kit; see the table near the
    // top). Every drum is fully voiced (model, pitch, decay, length) but with pos=0 (zero onsets) so the
    // kit boots SILENT - the player raises POS on each drum (Rev-swapping to reach the slot-1 pair) to
    // build it up. The same table backs clear_sequence() (hold Alt+Seq) for a reset-to-defaults.
    for (int di = 0; di < DeckRef::Count; di++)
        for (int s = 0; s < kSlots; s++) {
            const auto& b = kBootKit[di][s];
            _init_slot(static_cast<DeckRef::Ref>(di), s, _sr, b.model, b.seed, b.pos, b.pitch, b.decay);
        }

#ifndef TEST
    // Restore the player's last-saved kit from QSPI (if any), overwriting the boot defaults above. This
    // runs before CoreUI::init() seeds the knob pickups, so the recalled values seed cleanly (no jump).
    kit_load(_qspi, *this);
#endif
}

// Main loop: debounced QSPI auto-save. A flash erase+write blocks ~tens of ms, so it runs here (never
// the audio ISR) and only once the player has stopped tweaking for kSaveDebounceMs. No-op on host.
void EdrumsEngine::prepare()
{
#ifndef TEST
    if (_dirty && _time && (_time->now_ms() - _dirty_since_ms) >= kSaveDebounceMs) {
        _dirty = false;
        KitData kd;
        serialize(kd);
        kit_save(kd);
    }
#endif
}

// A kit parameter changed -> (re)start the auto-save debounce window. The actual save fires later in
// prepare() once changes settle, so a knob sweep coalesces into a single flash write.
void EdrumsEngine::_mark_dirty()
{
    _dirty = true;
    if (_time) _dirty_since_ms = _time->now_ms();
}

// Fully seed one drum so it sounds independently of the platform's knob apply (which only writes the
// active slot). Mirrors what set_param would do for Pos/Size/Env/Speed/Mix/ModAmp/ModSpeed/Aux, and
// caches each norm so a slot swap can re-seed the platform's knobs from param(). Pos is the value the
// platform reads back at boot (active slot only); the rest the platform overwrites for slot 0 with its
// own UI defaults (Size=1, Speed/Mix=.5, Env=0), which match the values used here -> no boot transient.
void EdrumsEngine::_init_slot(DeckRef::Ref d, int slot, float sr, int model, uint32_t seed,
                              float pos, float pitch, float decay)
{
    using PI = ParamId;
    auto P = [](PI id) { return static_cast<size_t>(id); };
    _track[d][slot].voice.init(sr, 60.f, model, seed); // construct the oscs/filter; _rebuild_slot voices it
    _param[P(PI::Pos)][d][slot]      = pos;
    _param[P(PI::Size)][d][slot]     = 1.0f;
    _param[P(PI::Env)][d][slot]      = 0.0f;
    _param[P(PI::Speed)][d][slot]    = pitch;
    _param[P(PI::Mix)][d][slot]      = 0.8f;  // SOS -> per-drum gain (was decay)
    _param[P(PI::ModAmp)][d][slot]   = 1.0f;  // 100% of onsets fire
    _param[P(PI::ModSpeed)][d][slot] = 0.0f;  // MODFREQ idx 0 -> 1/16 (div 1)
    _param[P(PI::Aux)][d][slot]      = static_cast<float>(model) / (kModelCount - 1); // inverse of set_param's round
    // Sound-shaping modifier channels (seed the platform's knob pickup). Decay -> grit+SOS; the four
    // timbre macros -> grit/flux + PITCH/SOS/POS, all neutral at 0.5 (the model exactly as voiced).
    _param[P(PI::GritMix)][d][slot]       = decay; // grit+SOS   -> decay
    _param[P(PI::GritIntensity)][d][slot] = 0.5f;  // grit+PITCH -> drive
    _param[P(PI::FluxIntensity)][d][slot] = 0.5f;  // flux+PITCH -> pitch-sweep
    _param[P(PI::FluxMix)][d][slot]       = 0.5f;  // flux+SOS   -> body/noise balance
    _param[P(PI::FluxFb)][d][slot]        = 0.5f;  // flux+POS   -> brightness
    _model[d][slot] = static_cast<uint8_t>(model);

    _rebuild_slot(d, slot);   // derive voice + pattern + gain/prob/div from the caches above
}

// Re-derive one drum's voice + pattern from its cached `_param` norms (+ `_model`). Shared by
// _init_slot and apply(), so a loaded preset reconstructs a drum exactly like a fresh seed.
void EdrumsEngine::_rebuild_slot(DeckRef::Ref d, int slot)
{
    using PI = ParamId;
    auto P  = [](PI id) { return static_cast<size_t>(id); };
    auto& t = _track[d][slot];
    auto& v = t.voice;
    const auto pr = [&](PI id) { return _param[P(id)][d][slot]; };

    v.set_model(_model[d][slot]);
    v.set_pitch(pr(PI::Speed));
    v.set_decay(pr(PI::GritMix));          // decay (grit+SOS)
    v.set_macro(0, pr(PI::GritIntensity)); // drive
    v.set_macro(1, pr(PI::FluxIntensity)); // pitch-sweep
    v.set_macro(2, pr(PI::FluxMix));       // body/noise balance
    v.set_macro(3, pr(PI::FluxFb));        // brightness

    _gain[d][slot] = std::clamp(pr(PI::Mix),    0.f, 1.f);
    _prob[d][slot] = std::clamp(pr(PI::ModAmp), 0.f, 1.f);
    const int didx = std::clamp(static_cast<int>(pr(PI::ModSpeed) * 3.f), 0, 2);
    _div[d][slot]  = kDivTable[didx];

    const uint8_t len = static_cast<uint8_t>(2 + std::lround(pr(PI::Size) * 14.f));
    t.pattern.set_length(len);
    t.pattern.set_shift(pr(PI::Env));
    _apply_density(d, slot);               // onsets from _param[Pos] over the length
}

// --- Preset persistence (pure; QSPI I/O lives in the platform wiring) --------------------------
void EdrumsEngine::serialize(KitData& kd) const
{
    using PI = ParamId;
    auto P = [](PI id) { return static_cast<size_t>(id); };
    kd.version = KitData::kVersion;
    kd.route   = (_route == Route::GenerativeStereo) ? 2 : (_route == Route::DoubleMono) ? 1 : 0;
    for (int di = 0; di < DeckRef::Count; di++) {
        const auto d = static_cast<DeckRef::Ref>(di);
        kd.active[di] = _active_slot[di];
        for (int s = 0; s < kSlots; s++) {
            auto& dr  = kd.drum[di][s];
            dr.model  = _model[di][s];
            dr.pos    = _param[P(PI::Pos)][d][s];
            dr.size   = _param[P(PI::Size)][d][s];
            dr.env    = _param[P(PI::Env)][d][s];
            dr.pitch  = _param[P(PI::Speed)][d][s];
            dr.gain   = _param[P(PI::Mix)][d][s];
            dr.prob   = _param[P(PI::ModAmp)][d][s];
            dr.div    = _param[P(PI::ModSpeed)][d][s];
            dr.decay  = _param[P(PI::GritMix)][d][s];
            dr.drive  = _param[P(PI::GritIntensity)][d][s];
            dr.sweep  = _param[P(PI::FluxIntensity)][d][s];
            dr.tone   = _param[P(PI::FluxMix)][d][s];
            dr.bright = _param[P(PI::FluxFb)][d][s];
        }
    }
}

void EdrumsEngine::apply(const KitData& kd)
{
    if (kd.version != KitData::kVersion) return;   // unknown layout -> keep current state (defaults)
    using PI = ParamId;
    auto P = [](PI id) { return static_cast<size_t>(id); };
    _route = (kd.route == 2) ? Route::GenerativeStereo : (kd.route == 1) ? Route::DoubleMono : Route::Stereo;
    for (int di = 0; di < DeckRef::Count; di++) {
        const auto d = static_cast<DeckRef::Ref>(di);
        _active_slot[di] = static_cast<uint8_t>(kd.active[di] & 1);
        for (int s = 0; s < kSlots; s++) {
            const auto& dr = kd.drum[di][s];
            _model[di][s] = static_cast<uint8_t>(std::clamp<int>(dr.model, 0, kModelCount - 1));
            _param[P(PI::Pos)][d][s]           = dr.pos;
            _param[P(PI::Size)][d][s]          = dr.size;
            _param[P(PI::Env)][d][s]           = dr.env;
            _param[P(PI::Speed)][d][s]         = dr.pitch;
            _param[P(PI::Mix)][d][s]           = dr.gain;
            _param[P(PI::ModAmp)][d][s]        = dr.prob;
            _param[P(PI::ModSpeed)][d][s]      = dr.div;
            _param[P(PI::GritMix)][d][s]       = dr.decay;
            _param[P(PI::GritIntensity)][d][s] = dr.drive;
            _param[P(PI::FluxIntensity)][d][s] = dr.sweep;
            _param[P(PI::FluxMix)][d][s]       = dr.tone;
            _param[P(PI::FluxFb)][d][s]        = dr.bright;
            _param[P(PI::Aux)][d][s]           = static_cast<float>(_model[di][s]) / (kModelCount - 1);
            _rebuild_slot(d, s);
        }
    }
}

// Re-derive the onset count from the stored POS fraction over the CURRENT pattern length, so density
// stays proportional when SIZE changes the length.
void EdrumsEngine::_apply_density(DeckRef::Ref d, int slot)
{
    auto& p = _track[d][slot].pattern;
    const float frac = _param[static_cast<size_t>(ParamId::Pos)][d][slot];
    p.set_onsets(static_cast<uint8_t>(std::round(frac * p.steps())));
}

// Transport sink (audio-block context). Each deck steps every `_div` ticks (its own rate -> polymeter
// with the per-deck length); on an onset it fires, gated by the randomness amount. A grid reset
// realigns both decks (step phase + pattern position) to the bar.
void EdrumsEngine::_on_tick(const TransportTick& e)
{
    // All four drums sequence here, independent of which slot is focused: focus only gates editing and
    // the ring display, never the audio. Each drum steps at its own _div (polymeter) over its own
    // length. A grid reset realigns every drum (step phase + pattern position) to the bar.
    if (e.reset) {
        _step_tick = 0;
        for (DeckRef::Ref d : { DeckRef::A, DeckRef::B })
            for (int s = 0; s < kSlots; s++) _track[d][s].pattern.reset();
    }
    if (!e.tick) return;

    for (DeckRef::Ref d : { DeckRef::A, DeckRef::B }) {
        for (int s = 0; s < kSlots; s++) {
            if (_div[d][s] == 0 || (_step_tick % _div[d][s]) != 0) continue;
            // Always advance the pattern position so a stopped deck stays phase-locked to the grid
            // (its clock keeps running silently); restarting it drops back in in sync, never drifted.
            const bool onset = _track[d][s].pattern.trigger();
            if (!_running[d][s]) continue;   // stopped: position advanced, but fire no voice
            if (onset) {
                const bool fire = _prob[d][s] >= 1.f || _rand01(_track[d][s].prob_rng) < _prob[d][s];
                if (fire) {
                    _pan[d][s] = _rand01(_track[d][s].prob_rng); // random pan for the Generative routing mode
                    _track[d][s].voice.trigger();
                    _flash[d][s] = kFlashFrames;
                }
            }
        }
    }
    _step_tick++;
}

void EdrumsEngine::process(const float* const* /*in*/, float** out, size_t size)
{
    // A drum machine ignores the audio input. The routing switch picks how the two voices reach the
    // two outputs: Stereo = both summed to both (a mono drum bus), DoubleMono = A->left / B->right,
    // Generative = each hit randomly panned. SoftLimit keeps the summed bus in range.
    // Four voices now reach the bus. Stereo = all four summed to both outs (a mono drum bus);
    // DoubleMono = deck A's two slots -> left, deck B's two -> right; Generative = each hit randomly
    // panned. kBusTrim scales before SoftLimit so four simultaneous voices don't sit in constant
    // limiting the way the old two-voice sum avoided.
    for (size_t i = 0; i < size; i++) {
        // Per-deck mute zeros both of a deck's voices (they still process so envelopes stay coherent).
        const float gmA = _muted[DeckRef::A] ? 0.f : 1.f;
        const float gmB = _muted[DeckRef::B] ? 0.f : 1.f;
        const float a0 = _track[DeckRef::A][0].voice.process() * _gain[DeckRef::A][0] * gmA;
        const float a1 = _track[DeckRef::A][1].voice.process() * _gain[DeckRef::A][1] * gmA;
        const float b0 = _track[DeckRef::B][0].voice.process() * _gain[DeckRef::B][0] * gmB;
        const float b1 = _track[DeckRef::B][1].voice.process() * _gain[DeckRef::B][1] * gmB;
        float l, r;
        switch (_route) {
            case Route::DoubleMono:
                l = a0 + a1; r = b0 + b1; break;
            case Route::GenerativeStereo:
                l = a0 * (1.f - _pan[DeckRef::A][0]) + a1 * (1.f - _pan[DeckRef::A][1])
                  + b0 * (1.f - _pan[DeckRef::B][0]) + b1 * (1.f - _pan[DeckRef::B][1]);
                r = a0 * _pan[DeckRef::A][0] + a1 * _pan[DeckRef::A][1]
                  + b0 * _pan[DeckRef::B][0] + b1 * _pan[DeckRef::B][1];
                break;
            case Route::Stereo: default: {
                const float sum = a0 + a1 + b0 + b1; l = sum; r = sum;
            } break;
        }
        out[0][i] = daisysp::SoftLimit(l * kBusTrim);
        out[1][i] = daisysp::SoftLimit(r * kBusTrim);
    }
}

// Routing switch (set_config from _process_switches). Mirrors granular's int mapping so the panel's
// L/C/R positions read the same: 0 = Stereo, 1 = DoubleMono, 2 = GenerativeStereo.
bool EdrumsEngine::set_config(ConfigId id, DeckRef::Ref /*deck*/, int value)
{
    if (id == ConfigId::Route) {
        const Route nr = (value == 2) ? Route::GenerativeStereo
                       : (value == 1) ? Route::DoubleMono
                                      : Route::Stereo;
        if (nr != _route) { _route = nr; _mark_dirty(); }
    }
    return false;
}

// Rev pad (reverse=true): swap which of the deck's two drums is active (editable + shown). The other
// keeps playing - focus never gates audio. Flag a re-seed so the platform repoints that deck's knob
// pickup at the newly-focused drum's stored params (otherwise the next knob touch would jump it). The
// plain Play pad (reverse=false) has no role here. Return false to suppress the platform's "empty"
// flash: a synth drum engine has no buffer to be empty.
bool EdrumsEngine::on_play_pad(DeckRef::Ref deck, bool reverse)
{
    const auto d = _safe(deck);
    if (reverse) {                       // Rev pad: swap which of the deck's two drums is focused
        _active_slot[d] ^= 1;
        _reseed_pending[d] = true;
        _mark_dirty();                   // focus is part of the saved kit
    }
    else {                               // plain Play pad: stop/start the FOCUSED drum's sequencer
        const int s = _active_slot[d];
        _running[d][s] = !_running[d][s]; // not persisted (transport state); tails ring out on stop
    }
    return false;
}

// Alt+Play: toggle the whole DECK's mute (both drums) - they keep stepping (grid-locked), audio
// silenced. A broader scope than the per-drum stop, so the two gestures complement rather than overlap.
// Not persisted. (reverse = Alt+Rev is ignored.)
void EdrumsEngine::on_record_pad(DeckRef::Ref deck, bool reverse)
{
    if (!reverse) { const auto d = _safe(deck); _muted[d] = !_muted[d]; }
}

// Hold Alt+Seq on a deck (the platform's clear-sequence gesture) -> reset that deck's two drums to the
// factory kit (model/pitch/decay/macros/pattern), refocus slot 0, and mark dirty so the auto-save
// overwrites the stored preset. Re-seed the deck's knob pickup so the reset values take over cleanly.
// Per deck: hold on both decks for a full kit reset.
void EdrumsEngine::clear_sequence(DeckRef::Ref deck)
{
    const auto d = _safe(deck);
    for (int s = 0; s < kSlots; s++) {
        const auto& b = kBootKit[d][s];
        _init_slot(d, s, _sr, b.model, b.seed, b.pos, b.pitch, b.decay);
        _running[d][s] = true;     // a reset also restores the transport state (running)
    }
    _muted[d]          = false;    // and unmutes the deck
    _active_slot[d]    = 0;
    _reseed_pending[d] = true;
    _mark_dirty();
}

// One-shot: report (and clear) whether the deck's active drum just changed, so the platform re-seeds
// its MValue cache for that deck exactly once per swap.
bool EdrumsEngine::take_param_reseed(DeckRef::Ref deck)
{
    const auto d = _safe(deck);
    const bool pending = _reseed_pending[d];
    _reseed_pending[d] = false;
    return pending;
}

// All edits target the deck's ACTIVE slot, so the knobs only ever touch the focused drum; the
// backgrounded slot keeps its stored params (and keeps playing).
void EdrumsEngine::set_param(ParamId id, DeckRef::Ref deck, float v)
{
    const auto d = _safe(deck);
    const int  s = _active_slot[d];
    _param[static_cast<size_t>(id)][d][s] = v;
    switch (id) {
        case ParamId::Pos:   _apply_density(d, s); break;                              // density (onsets)
        case ParamId::Size: {                                                          // pattern length
            const uint8_t len = static_cast<uint8_t>(2 + std::lround(v * 14.f));        // 2..16 steps
            _track[d][s].pattern.set_length(len);
            _apply_density(d, s);                                                       // keep density proportional
        } break;
        case ParamId::Env:   _track[d][s].pattern.set_shift(v); break;                 // rotation
        case ParamId::Speed: _track[d][s].voice.set_pitch(v);   break;                 // pitch + noise colour
        case ParamId::Mix:   _gain[d][s] = std::clamp(v, 0.f, 1.f); break;             // SOS -> per-drum gain
        case ParamId::GritMix:       _track[d][s].voice.set_decay(v);    break;        // grit+SOS   -> decay
        case ParamId::GritIntensity: _track[d][s].voice.set_macro(0, v); break;        // grit+PITCH -> drive
        case ParamId::FluxIntensity: _track[d][s].voice.set_macro(1, v); break;        // flux+PITCH -> pitch-sweep
        case ParamId::FluxMix:       _track[d][s].voice.set_macro(2, v); break;        // flux+SOS   -> body/noise
        case ParamId::FluxFb:        _track[d][s].voice.set_macro(3, v); break;        // flux+POS   -> brightness
        case ParamId::ModAmp: _prob[d][s] = std::clamp(v, 0.f, 1.f); break;            // probability an onset fires
        case ParamId::Aux: {                                                           // Alt+PITCH -> model
            const int mdl = std::clamp(static_cast<int>(std::lround(v * (kModelCount - 1))), 0, kModelCount - 1);
            _track[d][s].voice.set_model(mdl);
            if (mdl != _model[d][s]) { _model[d][s] = static_cast<uint8_t>(mdl); _model_show[d][s] = kModelShowFrames; }
        } break;
        default: break;                                                                // edrums ignores the rest
    }
    _mark_dirty();   // any knob change is part of the kit -> schedule a save
}

float EdrumsEngine::param(ParamId id, DeckRef::Ref deck) const
{
    const auto d = _safe(deck);
    return _param[static_cast<size_t>(id)][d][_active_slot[d]];
}

// MODFREQ knob -> clock division (ticks per step). value 0..1 -> {1/16, 1/8, 1/4}. The norm is cached
// into _param[ModSpeed] so a slot swap can re-seed the platform's MODFREQ knob (it is not otherwise
// stored, since set_mod_speed writes the derived division directly).
void EdrumsEngine::set_mod_speed(DeckRef::Ref deck, float value, bool /*sync*/)
{
    const auto d = _safe(deck);
    const int  s = _active_slot[d];
    int idx = static_cast<int>(std::clamp(value, 0.f, 1.f) * 3.f);
    if (idx > 2) idx = 2;
    _div[d][s] = kDivTable[idx];
    _param[static_cast<size_t>(ParamId::ModSpeed)][d][s] = value;
    _mark_dirty();
}

// Platform pushes this each loop for CapAux engines: true while Alt is held and this deck's PITCH isn't
// claimed by an fx touch. It gates the held-Alt model selector drawn in render().
void EdrumsEngine::set_aux_active(DeckRef::Ref deck, bool active)
{
    _aux_held[static_cast<int>(deck)] = active;
}

void EdrumsEngine::render(DisplayModel& m)
{
    m.clear();
    // Per-(deck,slot) colours in two hue families, so deck identity AND which drum is focused both read
    // at a glance: deck A warm (amber / red-orange), deck B cool (cyan / violet). The ring + Play LED
    // show the focused drum; the Rev LED carries the backgrounded drum's colour.
    static const uint32_t kColor[DeckRef::Count][kSlots] = {
        { 0xff5500, 0xff0066 }, // A: slot 0 amber, slot 1 magenta (red-dominant pair, but clearly distinct)
        { 0x00b0ff, 0x7a5cff }, // B: slot 0 cyan,  slot 1 violet  (blue-dominant pair)
    };
    for (int c = 0; c < 2; c++) {
        const int sl  = _active_slot[c];
        const int inv = sl ^ 1;                 // the backgrounded slot
        const uint32_t col = kColor[c][sl];

        // Alt+PITCH held: a persistent model selector for the focused drum - all kModelCount tones spaced
        // around the ring, the current one bright, in the drum's colour - so scrolling tones has live
        // feedback (previously nothing showed unless the value happened to change, and then only briefly).
        if (_aux_held[c]) {
            ring::selector(m.ring[c], kModelCount, _model[c][sl], col);
            m.ring[c].set_updated();
        }
        // Just after releasing Alt (or any model change), briefly confirm the model number (model+1 dots).
        else if (_model_show[c][sl] > 0) {
            _model_show[c][sl]--;
            m.ring[c].set_point_hex_color(pal::kWhite);
            for (uint8_t i = 0; i <= _model[c][sl]; i++) m.ring[c].set_point(static_cast<uint8_t>(i * 3), 1.f);
            m.ring[c].set_updated();
        }
        else {
            auto& tr = _track[c][sl];
            m.ring[c].set_point_hex_color(col);
            const uint8_t steps = tr.pattern.steps();
            const uint8_t pos   = tr.pattern.position();
            for (uint8_t s = 0; s < steps; s++) {
                float b = tr.pattern.step_is_onset(s) ? 0.5f : 0.06f;   // onset lit, rest dim
                if (s == pos) b = 1.f;                                  // playhead
                const uint8_t led = static_cast<uint8_t>(static_cast<uint16_t>(s) * 32u / steps); // length fills the ring
                m.ring[c].set_point(led, b);
            }
            m.ring[c].set_updated();
        }

        // Play LED = focused drum's colour, full on its hits. Rev LED = backgrounded drum's colour, a
        // dim steady presence (so you always see the other drum is there and which it is) that brightens
        // on its own hits. Decrement both flashes here - the backgrounded slot's flash is set in
        // _on_tick regardless of focus, so consuming it here keeps it from stalling.
        // Play LED doubles as the transport indicator: OFF when the deck is stopped; otherwise the hit
        // flash, dimmed when muted (running-but-silent reads differently from running-and-sounding).
        float play_b = 0.f;
        if (_running[c][sl]) {                       // focused drum's run state; deck-level mute dims it
            const float hit = _muted[c] ? 0.4f : 1.f;
            play_b = _flash[c][sl] > 0 ? hit : 0.15f;
        }
        m.play[c] = { col,               play_b };
        m.rev[c]  = { kColor[c][inv],    _flash[c][inv] > 0 ? 1.f : 0.12f };
        if (_flash[c][sl]  > 0) _flash[c][sl]--;
        if (_flash[c][inv] > 0) _flash[c][inv]--;
    }
    // Routing switch (drum-bus topology) on the mode L/C/R LEDs via the shared helper — previously the
    // edrums panel showed no route feedback at all.
    led::route_leds(m, _route);
}
