#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "engine/istreamdeck.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

// Dual virtual RadioMusic. Each deck (A/B) is an INDEPENDENT virtual radio browsing one shared SD
// library of banks ("/radio/0".."/radio/15", each a folder of headerless 16-bit-mono ".raw" stations).
// The two radios are blended by the platform crossfader and placed by the routing switch, so it plays
// like a pair of radios you tune and mix.
//
// The defining RadioMusic behaviour is the FREE-RUNNING VIRTUAL PLAYHEAD: every station sounds as
// though it kept broadcasting while you were tuned elsewhere. A single monotonic frame clock advances
// each audio block; tuning to a station seeks its file to (clock + START) mod length and streams
// forward, so each station lands at its "live" position. Raw 16-bit mono makes this cheap - a station's
// length is filesize/2 (a pure f_stat, no header), and a seek is f_lseek(frame*2).
//
// Per-deck control map:
//   PITCH (Speed)      -> STATION select (tuning dial), summed with the V/oct CV jack (cv_voct).
//   POS   (Pos)        -> START offset into the station, summed with the size/pos CV jack (cv_size_pos).
//   SIZE  (Size)       -> varispeed 0.5x..2x (center = unity). RadioMusic is fixed-rate; this is a bonus.
//   ENV   (Env)        -> inter-station STATIC amount (tuning hiss crossfaded in on a station change).
//   MIX   (Mix)        -> deck volume.   Mix fader (Crossfade) -> A/B blend.   Routing switch -> topology.
//   Alt+PITCH (Aux)    -> BANK select (held selector, ring dots).
//   Play pad / gate-in -> RESET (re-tune the current station to its live position).
//
// The engine is thin in the audio ISR: it pulls int16 bytes from the platform's per-deck play ring
// (int16->float), runs a varispeed resampler, crossfades the static, and mixes to the stereo bus. All
// the slow FatFs work (directory scan, file open + seek, the bank index) happens off the audio path in
// prepare() (main loop) via the StreamDeck service.
class RadioEngine : public IEngine {
public:
    RadioEngine() = default;
    ~RadioEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override;
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override { return CapOwnDisplay | CapDualDeck | CapAux; }

#if SPK_TERMINAL
    // Engine-specific state (target B, docs/dev/terminal-target-b.md). `station` is the one that
    // matters for testing: the knob selects a station, but this reports which one is ACTUALLY
    // streaming (-1 = none), so a test can assert the seek completed rather than that the knob moved.
    enum EQ : uint8_t { EQ_STATION, EQ_STATIONS, EQ_BANK, EQ_COUNT };
    static constexpr EngineQuery kEngineQueries[] = {
        { "station",  QueryScope::Deck, ValueKind::Int, nullptr, true },   // -1 = none open
        { "stations", QueryScope::Deck, ValueKind::Int, nullptr, true },   // count found in the bank
        { "bank",     QueryScope::Deck, ValueKind::Int, nullptr, true },
    };
    static_assert(sizeof(kEngineQueries) / sizeof(kEngineQueries[0]) == EQ_COUNT,
                  "kEngineQueries out of sync with the EQ enum");

    EngineQueryTable engine_queries() const override { return { kEngineQueries, EQ_COUNT }; }
    void read_engine_query(uint8_t i, DeckRef::Ref d, TextSink& r) override {
        const int k = (d == DeckRef::B) ? 1 : 0;
        switch (i) {
            case EQ_STATION:  r.append_i32(_open_station[k]); break;
            case EQ_STATIONS: r.append_i32(_nst[k]); break;
            case EQ_BANK:     r.append_i32(_bank[k]); break;
            default: break;
        }
    }

    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md): the ids this engine actually
    // consumes, so a host sweep exercises only real parameters instead of the whole ParamId enum.
    // Derived from the engine's own ParamId/ConfigId use; NOT yet verified on hardware.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::Env))
             | (1u << static_cast<uint32_t>(ParamId::Aux))
             | (1u << static_cast<uint32_t>(ParamId::Crossfade));
    }
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>(1u << static_cast<uint32_t>(ConfigId::Route));
    }
#endif

    void  set_param(ParamId id, DeckRef::Ref d, float v) override;
    float param(ParamId id, DeckRef::Ref d) const override;
    void  set_aux_active(DeckRef::Ref d, bool held) override;   // Alt held -> show the bank selector
    bool  set_config(ConfigId id, DeckRef::Ref, int value) override; // routing switch -> stereo topology
    Route route() const override { return _route; }

    void  cv_voct(DeckRef::Ref d, float value) override;        // STATION CV
    void  cv_size_pos(DeckRef::Ref d, float value) override;    // START CV

    bool  on_play_pad(DeckRef::Ref d, bool reverse) override;   // RESET (re-tune)
    void  on_gate_trigger(DeckRef::Ref d) override;             // RESET (re-tune)

    void  render(DisplayModel& m) override;

private:
    NOCOPY(RadioEngine)

    static constexpr int      kMaxBanks    = 16;        // "/radio/0".."/radio/15"
    static constexpr int      kMaxStations = 48;        // RadioMusic's newer-firmware per-folder cap
    static constexpr size_t   kMaxFrames   = 128;       // platform block is 96
    static constexpr int      kRingLeds    = 32;        // LEDs per ring
    static constexpr uint32_t kErrColor    = 0xff6000;  // amber: a station failed to open / empty bank
    static constexpr uint32_t kErrFlashMs  = 1200;
    static constexpr uint32_t kDebounceMs  = 250;       // ignore a same-deck RESET retrigger within this
    static constexpr float    kStationHyst = 0.25f;     // station-select deadband (fraction of a station
                                                        // width) so CV/pot noise can't chatter the target
                                                        // (oscillation needs > 2*this to flip back)
    static constexpr uint32_t kSettleMs    = 180;       // clean-switch settle when static is low
    static constexpr float    kStaticThresh = 0.15f;    // ENV >= this -> switch immediately (dial tuning)
    static constexpr float    kStaticDec   = 0.0002f;   // per-sample static-burst decay (~100 ms)
    static constexpr float    kNoiseLevel  = 0.30f;     // peak static amplitude
    static constexpr uint32_t kBootScanMs  = 5000;      // retry the bank scan this long (SD mounts ~1 s in)
    static constexpr float    kHalfPi      = 1.57079632679f;
    static constexpr float    kCenterGain  = 0.70710678f;

    void  _render_deck(DeckRef::Ref d, float* mono, size_t n);  // varispeed playback + static, per deck
    bool  _try_load_rate();                 // read /radio/rate.txt -> _src_rate_ratio (true once attempted)
    float _pull(DeckRef::Ref d);            // one int16 source frame -> float (0 on underrun)
    float _static_sample(int i);            // one filtered-noise sample for deck i
    int   _quant_station(int i) const;      // knob+CV -> station index (-1 if the bank is empty)
    void  _do_open(DeckRef::Ref d, int station, uint32_t now);  // (re)open a station at its live offset
    void  _arm_reset(DeckRef::Ref d);       // debounced RESET request (Play pad / gate) -> _retune
    void  _roll_random_pans();              // GenerativeStereo per-deck random pans
    const char* _bank_dir(int i);           // "radio/<bank>" for scan_bank
    const char* _path(int i, int station);  // "radio/<bank>/<name>" for start_play_raw

    IStreamDeck*       _stream = nullptr;
    const ITimeSource* _time   = nullptr;

    Route _route = Route::DoubleMono;        // routing switch position (set each loop via set_config)
    uint64_t _clock = 0;                     // free-running frame clock (advances every block)

    // Source sample rate (RadioMusic .raw files are headerless, so the rate cannot be read from the
    // file). Default 48 kHz -> ratio 1.0; an optional /radio/rate.txt (e.g. "44100") rebases the
    // resampler so an unconverted original card plays at correct pitch. Loaded once at boot.
    float _src_rate_ratio = 1.f;             // global rate.txt ratio (source_rate / 48000) for .raw files
    bool  _rate_loaded    = false;
    // Effective rate ratio of the station currently open on each deck: a .wav carries its own rate in the
    // header (so it plays at correct pitch with no rate.txt), a headerless .raw falls back to rate.txt.
    float _deck_rate_ratio[2] = { 1.f, 1.f };

    // Per-deck bank index (scanned in prepare()) and the currently-open station/bank.
    BankEntry _stations[2][kMaxStations];
    int  _nst[2]          = { 0, 0 };
    int  _bank[2]         = { 0, 0 };
    int  _open_station[2] = { -1, -1 };      // station currently streaming (-1 = none)
    int  _pending[2]      = { -1, -1 };      // desired station awaiting the settle timer
    uint32_t _pending_ms[2] = { 0, 0 };
    bool _rescan[2]       = { true, true };  // (re)scan this deck's bank directory in prepare()
    bool _retune[2]       = { false, false };// RESET: re-seek the current station to its live position

    // Knob + CV caches (0..1). Quantization to station/bank happens in prepare(); varispeed is precomputed.
    float _station_n[2] = { 0.f, 0.f };   float _station_cv[2] = { 0.f, 0.f };
    float _start_n[2]   = { 0.f, 0.f };   float _start_cv[2]   = { 0.f, 0.f };
    float _size_n[2]    = { 0.5f, 0.5f }; // SIZE knob (0.5 = unity varispeed)
    float _env_n[2]     = { 0.f, 0.f };   // static amount
    float _gain[2]      = { 1.f, 1.f };   // MIX volume
    float _bank_n[2]    = { 0.f, 0.f };   // Aux readback

    // Varispeed resampler state (2-frame linear interp), per deck.
    float _speed[2] = { 1.f, 1.f };
    float _phase[2] = { 0.f, 0.f };
    float _cur[2]   = { 0.f, 0.f };
    float _next[2]  = { 0.f, 0.f };
    bool  _primed[2] = { false, false };

    // Static (inter-station hiss): a burst envelope set on a switch, decaying per sample; one filtered
    // noise generator per deck.
    float    _static_env[2] = { 0.f, 0.f };
    float    _noise_lp[2]   = { 0.f, 0.f };
    uint32_t _rng = 0x9e3779b9u;

    // Crossfade (mix fader) A/B blend gains, and the GenerativeStereo random pans.
    float _xfade = 0.5f, _gA = 1.f, _gB = 1.f;
    float _rndL[2] = { kCenterGain, kCenterGain };
    float _rndR[2] = { kCenterGain, kCenterGain };

    bool     _aux_held[2]  = { false, false };
    uint32_t _err_until[2] = { 0, 0 };
    uint32_t _last_reset_ms[2] = { 0, 0 };   // RESET (Play pad / gate) debounce, per deck

    char _dbuf[24];   // scratch: "radio/<bank>"
    char _pbuf[40];   // scratch: "radio/<bank>/<name>"
};

} // namespace spotykach
