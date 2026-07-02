#pragma once

#include <daisy_seed.h>
#include <functional>
#include <cstring>
#include <bitset>
#include <array>

#include "../common.h"
#include "config.h"              // shared tempo range helpers (tempo_abs_to_norm) - was via core.h
#include "engine/color.h"
#include "hw/hardware.h"
#include "engine/iengine.h"
#include "engine/display_model.h"
#include "transport/transport.h"   // platform clock the UI drives directly (tap/source/tempo/leds)
#include "memory/storage.h"
#include "nocopy.h"
#include "hold.h"
#include "mvalue.h"
#include "calibrator.h"
#include "time.iterval.h"
#include "engine/led.ring.h"
#include "dsp/lutsinosc.h"          // shared sine LUT (relocated to src/ root in R4; also used by granular lfo.h)

namespace spotykach { 

static constexpr std::array<float, 7> kSpeedSteps = { 
    .125f,          // -24
    .25f,           // -12
    .33371f,        // -7
    .5f,            // 0
    .5830516667f,   // 7 
    .666666667f,    // 12
    1.f             // 24
};

class CoreUI {
public:
    CoreUI(Hardware&, IEngine&, Transport&, Settings&, Storage&);
    ~CoreUI() = default;

    void init();
    void process();
    void tick();
    void read_cv();

    void render_leds();

    void set_lfo(const float a, const float b)  {
        _lfo_a = a;
        _lfo_b = b;
    }

    void calibrate(const bool recalibrate);

    void process_gate_in();

#if SPK_TERMINAL
    // `mode test` input isolation: when set, the platform stops feeding physical input (knobs/CV/gate)
    // to the engine so terminal-injected stimulus is the only driver (determinism). Outputs (LEDs/DAC/
    // audio) keep rendering. app.cpp pushes the Terminal's test_mode() here each main-loop iteration.
    void set_input_frozen(bool f) { _input_frozen = f; }
#endif

private:
    NOCOPY(CoreUI)

    enum class State: uint8_t {
        launching,
        init_values,
        ready
    };

    void _init_values();
    void _reseed_focus(const DeckRef::Ref); // re-seed a deck's knob pickup after an engine drum swap

    void _process_ui_queue();
    void _process_switches();
    void _process_gate_out(const DeckRef::Ref);
    bool _process_midi();
    bool _process_realtime(daisy::MidiEvent&);

    void _on_pad_touch(Hardware::Pad pad);
    void _on_pad_release(Hardware::Pad pad);
    void _on_play_touch(const DeckRef::Ref, const bool reverse);
    void _on_alt_touch();

    void _process_clock_out();
    void _on_quarter(const bool /*is key quarter*/);
    void _set_tempo_by_size(const DeckRef::Ref, const float fraction);


    // LEDs ////////////////////////////////////////
    void _draw_leds();
    void _draw_launching();
    void _blit_display(); // blit an own-display engine's DisplayModel (rings + indicators) - 3b-2a

    void _draw_fx(const DeckRef::Ref);
    void _draw_play(const DeckRef::Ref, const bool blink);
    void _draw_alt(const DeckRef::Ref);

    void _draw_ring(const DeckRef::Ref);
    void _show_pitch(const DeckRef::Ref);
    void _show_slots(const DeckRef::Ref);
    void _show_key_intervals();
    void _show_size_quarters(const DeckRef::Ref, const uint32_t color);
    void _show_error(const DeckRef::Ref);
    
    void _show_empty(const DeckRef::Ref);
    void _show_gate_in(const DeckRef::Ref);

    void _breathe_led();

    struct LED {
        public: 
        void init(int id)
        {
            _id = (Hardware::LedId)id;
        }

        void on(const uint32_t color, const float bright = 1.f) 
        {
            _bright = bright;
            _color = color;
        }

        void off() {
            _bright = 0;
            _color = 0;
        }

        void set(Hardware& hw)
        {
            hw.leds.Set(_id, _color, _bright);
        }

        private:
        Hardware::LedId _id;
        float _bright;
        uint32_t _color;
    };
    std::array<LED, Hardware::LED_LAST> _led;

    //////////////////////////////////////////////////

    enum class ValueDisplay: uint8_t {
        Always,
        OnMove,
        OnMoveDiffOnly
    };
    void _show_value(
        const MValue&, 
        LEDRing&,
        const uint32_t default_color = 0xffffff, 
        const ValueDisplay display = ValueDisplay::OnMove);
    bool _is_changing(const MValue&) const;
    void _reset_changing_value_id();

    Hardware& _hw;
    IEngine& _engine; // the platform drives the engine ONLY through IEngine (item 3 complete)
    Transport& _transport; // platform clock: UI drives tick/reset/tap/source/tempo + reads leds()
    bool _engine_owns_display = false; // engine fills DisplayModel via render(); platform blits it (3b-2a)
    bool _aux_select = false;          // engine claims Alt+PITCH as a selector (CapAux -> ParamId::Aux)
    bool _alt_pos = false;             // engine claims the Alt+POS knob layer (CapAltPos -> ParamId::AltPos)
    bool _pitch_pickup = false;        // route PITCH through the pickup-gated path (CapPitchPickup, e.g. shuttle)
#if SPK_TERMINAL
    bool _input_frozen = false;        // `mode test`: freeze physical input -> engine (terminal.h)
#endif
    Settings& _settings;
    Storage& _storage;
    Calibrator _calibrator;

    daisy::UiEventQueue _ui_queue;
    daisy::PotMonitor<Hardware, Hardware::kNumAnalogControls> _pot_monitor;
    std::bitset<Hardware::CTRL_LAST> _init;
    std::bitset<Hardware::CTRL_LAST> _apply;

    std::array<int, DeckRef::Count> _changing_value_id;

    std::array<Hold<1500/*ms*/>, DeckRef::Count> _hold_clear;
    Hold<100/*ms*/> _tap_hold;

    // Per-control pickup state, keyed by ParamId (item 3a-2): one MValue per (param, deck),
    // replacing the ~21 former named members. The platform owns the pickup mechanics; mv(id)
    // returns the param's per-deck row. Global params (Tempo/KeyInterval/ClickMix/Pan*) use the
    // [DeckRef::A] slot. _size_quarters is a platform tempo-fit gesture, not a ParamId, so it stays
    // named.
    std::array<std::array<MValue, DeckRef::Count>, static_cast<size_t>(ParamId::Count)> _mv;
    std::array<MValue, DeckRef::Count>& mv(ParamId id) { return _mv[static_cast<size_t>(id)]; }

    std::array<MValue, DeckRef::Count> _size_quarters;

    // The live LED display the platform renders into and blits. Its two ring canvases ARE the
    // per-deck rings (formerly a standalone std::array<LEDRing>); engine.render(DisplayModel&)
    // will fill these in the LED migration. Indicators are still driven via _led[] for now.
    DisplayModel _display;

    float _led_breathe_phase;
    float _led_breathe_brightness;
    bool _blink_led_on;

    float _lfo_a;
    float _lfo_b;

    daisy::StopwatchTimer _blink_timer;
    daisy::StopwatchTimer _arm_blink_timer;
    daisy::StopwatchTimer _clock_switch_timer;

    std::array<daisy::StopwatchTimer, DeckRef::Count> _gate_out_timer;
    std::array<bool, DeckRef::Count> _gate_out_high;
    std::array<int8_t, DeckRef::Count> _alt_blink_count;
    std::array<int8_t, DeckRef::Count> _gate_in_led_cnt;

    bool _show_rec_a;
    bool _show_rec_b;
    float pitch_seg_a;
    float pitch_seg_b;

    std::bitset<2> _gate_in;
    TimeInterval<8/*ms*/> _gate_a_latency;
    TimeInterval<8/*ms*/> _gate_b_latency;

    TimeInterval<1000/*ms*/> _value_display_timeout;

    enum Touched: uint8_t {
        Shift,
        Alt,
        FluxA,
        GritA,
        FluxB,
        GritB,
        TouchedOptions
    };
    std::bitset<TouchedOptions> _touched;
    std::bitset<DeckRef::Ref::Count> _pitch_quantized;

    State _state;
    bool _show_key_quarter;
    bool _clock_led_on;
    bool _clock_source_changed;
    bool _tap_was_tapped;
};

};
