#include "core.ui.h"
#include "Utility/dsp.h"
#include "expose.h"

// Size-optimize this whole TU to reclaim SRAM_EXEC for the LED migration. The only
// audio-adjacent code here is read_cv (per-block 500 Hz control glue, not per-sample DSP),
// so -Os is perf-irrelevant; the audio DSP in core/*.cpp stays -O2 -funroll-loops.
#pragma GCC optimize("Os")

using namespace spotykach;
using namespace infrasonic;
using namespace daisy;

static float snapped_speed(const float speed)
{
    auto s = static_cast<float>(kSpeedSteps.size() - 1);
    auto idx = static_cast<int>(std::clamp(std::round(speed * s), 0.f, s));
    return kSpeedSteps[idx];
}

CoreUI::CoreUI(Hardware& hw, IEngine& engine, Transport& transport, Settings& settings, Storage& storage):
_hw                 { hw },
_engine             { engine },
_transport          { transport },
_settings           { settings },
_storage            { storage },
_calibrator         { Calibrator(hw, settings) },
_state              { State::launching },
_show_key_quarter   { false },
_clock_led_on       { false },
_tap_was_tapped     { false }
{};

void CoreUI::init() {
    _hw.StartAdcs();

    _blink_timer.Init();
    _arm_blink_timer.Init();

    _gate_out_timer[DeckRef::A].Init();
    _gate_out_timer[DeckRef::B].Init();

    _pot_monitor.Init(_ui_queue, _hw, 500, 0.005f, 0.002f);

    using namespace std::placeholders;
    auto on_touch = std::bind(&CoreUI::_on_pad_touch, this, _1);
    auto on_release = std::bind(&CoreUI::_on_pad_release, this, _1);
    _hw.SetOnTouch(on_touch);
    _hw.SetOnRelease(on_release);

    auto on_quarter = std::bind(&CoreUI::_on_quarter, this, _1);
    _transport.set_on_quarter(on_quarter);

    auto on_clock_out = std::bind(&CoreUI::_process_clock_out, this);
    _transport.set_on_clock_out(on_clock_out);

    for (int i = 0; i < Hardware::LED_LAST; i++) _led[i].init(i);

    _engine_owns_display = _engine.capabilities() & CapOwnDisplay;
    _aux_select = _engine.capabilities() & CapAux;
    _alt_pos = _engine.capabilities() & CapAltPos;
    _pitch_pickup = _engine.capabilities() & CapPitchPickup;
};

void CoreUI::_init_values()
{
    for (auto ref: { DeckRef::A, DeckRef::B }) {
        
        // Engine-derived seeds (start position + fx state) come from the engine's pre-seeded
        // param cache; the rest are the platform's UI-default pickup starting points.
        mv(ParamId::Pos)[ref].set(_engine.param(ParamId::Pos, ref));
        // Alt+POS layer (engine-seeded): claimed via CapAltPos; the tape engine reads it as pan, others 0.
        mv(ParamId::AltPos)[ref].set(_engine.param(ParamId::AltPos, ref));
        // Engine-seeded (like Pos/ModSpeed): the SIZE default comes from each engine's cache. reso
        // seeds 0.5 (its damping is very sensitive near the top, so centre the knob); granular and
        // delay seed 1.0 to keep their prior default; edrums seeds its own 1.0 per slot.
        mv(ParamId::Size)[ref].set(_engine.param(ParamId::Size, ref));
        mv(ParamId::Speed)[ref].set(.5f);
        mv(ParamId::Mix)[ref].set(.5f);
        mv(ParamId::Feedback)[ref].set(kDefaultFeedback);
        // Engine-seeded (like Pos/Size/ModSpeed): the ENV default comes from each engine's cache. reso
        // seeds 0.5 - its ENV drives Rings brightness, and at 0 the excitation is filtered to silence, so
        // a 0 default booted the voice silent-on-trigger. granular/edrums/delay return 0.0 (their prior
        // default: granular's amplitude envelope off at fully-CCW, per the manual).
        mv(ParamId::Env)[ref].set(_engine.param(ParamId::Env, ref));
        mv(ParamId::EnvSize)[ref].set(1.f);
        mv(ParamId::Win)[ref].set(.2f);
        mv(ParamId::PolySlice)[ref].set(.51f);

        // Engine-seeded (like Pos/ModAmp/Aux): the MODFREQ/"cycle" default comes from each engine's
        // cache, not a shared literal. reso seeds 0 so its Slice arp / Drift scatter starts OFF (a
        // non-zero default free-ran the arp from boot); granular seeds 0.3 to keep its prior default;
        // edrums seeds its own 0.0 (1/16 div).
        mv(ParamId::ModSpeed)[ref].set(_engine.param(ParamId::ModSpeed, ref));
        // Engine-seeded (like Pos): granular's param(ModAmp) is 0 (unchanged), but edrums seeds it to
        // 1.0 so its MOD_AMT->probability knob defaults to 100%.
        mv(ParamId::ModAmp)[ref].set(_engine.param(ParamId::ModAmp, ref));
        // Alt+PITCH selector (CapAux engines, e.g. edrums model select). Engine-seeded so a deck can
        // start on a chosen item; granular's param(Aux) is 0 and the knob is unused there.
        mv(ParamId::Aux)[ref].set(_engine.param(ParamId::Aux, ref));

        _hold_clear[ref].init();

        mv(ParamId::GritMix)[ref].set(_engine.param(ParamId::GritMix, ref));
        mv(ParamId::GritIntensity)[ref].set(_engine.param(ParamId::GritIntensity, ref));
        mv(ParamId::FluxMix)[ref].set(_engine.param(ParamId::FluxMix, ref));
        mv(ParamId::FluxIntensity)[ref].set(_engine.param(ParamId::FluxIntensity, ref));
        mv(ParamId::FluxFb)[ref].set(_engine.param(ParamId::FluxFb, ref));
    }

    mv(ParamId::PanRange)[DeckRef::A].set(.6f);
    mv(ParamId::PanSpeed)[DeckRef::A].set(1.f);

    mv(ParamId::ClickMix)[DeckRef::A].set(0.f);
    mv(ParamId::KeyInterval)[DeckRef::A].set(.0588f); //Corresponds to 1/4th
    mv(ParamId::Tempo)[DeckRef::A].set(tempo_abs_to_norm(120.f));
}

// Re-seed one deck's per-knob pickup cache from the engine after it repointed the deck at a different
// underlying target (edrums' Rev-pad drum swap). MValue::set() stores the value WITHOUT marking the
// pot active, so the pot must be moved to within the pickup threshold of the new value before it
// catches - i.e. clean takeover with no jumps. Only the continuous params a sequencer engine reads
// are re-seeded; categorical config and the global params are unaffected by a swap.
void CoreUI::_reseed_focus(const DeckRef::Ref ref)
{
    for (ParamId id : { ParamId::Pos, ParamId::Size, ParamId::Env, ParamId::Speed,
                        ParamId::Mix, ParamId::ModAmp, ParamId::ModSpeed, ParamId::Aux, ParamId::AltPos,
                        // grit/flux modifier channels too, so an engine that maps them per focused
                        // voice (edrums/shuttle macros) re-picks-up cleanly on a swap or reset.
                        ParamId::GritIntensity, ParamId::GritMix, ParamId::FluxIntensity,
                        ParamId::FluxMix, ParamId::FluxFb }) {
        mv(id)[ref].set(_engine.param(id, ref));
    }
}

void CoreUI::process() 
{
    if (_state == State::launching) return;

    _hw.ProcessDigitalControls();
    _pot_monitor.Process();
    _hw.ProcessPads();
    _process_ui_queue();
    _process_switches();
    _process_gate_out(DeckRef::A);
    _process_gate_out(DeckRef::B);

    if (_state == State::init_values) {
        _init_values(); // effectively override the first read of the pots
        _reset_changing_value_id();
        _apply.set();
        _state = State::ready;
    }

    // An engine can repoint a deck's knobs at a different underlying target (edrums' Rev-pad drum
    // swap). When it reports one, re-seed that deck's pickup cache from the engine so the pots catch
    // the new target's values without jumping. Polled after pad handling, before the pot-apply pass.
    for (auto ref : { DeckRef::A, DeckRef::B }) {
        if (_engine.take_param_reseed(ref)) _reseed_focus(ref);
    }

    _tap_hold.process();

    auto layout_a = _engine.deck_layout(DeckRef::A);
    auto layout_b = _engine.deck_layout(DeckRef::B);
    auto is_chord_a = layout_a == DeckLayout::chord;
    auto is_chord_b = layout_b == DeckLayout::chord;

    auto blink = _arm_blink_timer.HasPassedMs(250);
    if (blink) _arm_blink_timer.Restart();

    for (auto ref: { DeckRef::A, DeckRef::B }) {
        if (_hold_clear[ref].process()) {
            _hold_clear[ref].end();
            _engine.clear_sequence(ref);
        }
        // LEDs /////////
        if (_engine_owns_display) continue; // own-display engines fill DisplayModel via render() below
        _draw_ring(ref);
        _draw_fx(ref);
        _draw_alt(ref);
        _draw_play(ref, blink);
    }
    // Push the Alt+PITCH selector-held state so a CapAux own-display engine can show its Aux selector
    // the whole time Alt is held (per deck: Alt down AND that deck's PITCH is not claimed by an fx touch).
    if (_aux_select) {
        const bool alt = _touched.test(Alt);
        _engine.set_aux_active(DeckRef::A, alt && !(_touched.test(FluxA) || _touched.test(GritA)));
        _engine.set_aux_active(DeckRef::B, alt && !(_touched.test(FluxB) || _touched.test(GritB)));
    }
    if (_engine_owns_display) _engine.render(_display);

    if (_apply.test(Hardware::CTRL_POS_A)) {
        if (_touched.test(FluxA)) {
            _engine.set_param(ParamId::FluxFb, DeckRef::A, mv(ParamId::FluxFb)[DeckRef::A].value());
        }
        else if (_alt_pos && _touched.test(Alt)) {
            _engine.set_param(ParamId::AltPos, DeckRef::A, mv(ParamId::AltPos)[DeckRef::A].value());
        }
        else {
            _engine.set_param(ParamId::Pos, DeckRef::A, mv(ParamId::Pos)[DeckRef::A].value());
        }
    }
    if (_apply.test(Hardware::CTRL_POS_B)) {
        if (_touched.test(FluxB)) {
            _engine.set_param(ParamId::FluxFb, DeckRef::B, mv(ParamId::FluxFb)[DeckRef::B].value());
        }
        else if (_alt_pos && _touched.test(Alt)) {
            _engine.set_param(ParamId::AltPos, DeckRef::B, mv(ParamId::AltPos)[DeckRef::B].value());
        }
        else {
            _engine.set_param(ParamId::Pos, DeckRef::B, mv(ParamId::Pos)[DeckRef::B].value());
        }
    }
    if (_apply.test(Hardware::CTRL_ENV_A)) {
        if (is_chord_a) _engine.set_param(ParamId::EnvSize, DeckRef::A, mv(ParamId::EnvSize)[DeckRef::A].value());
        _engine.set_param(ParamId::Env, DeckRef::A, mv(ParamId::Env)[DeckRef::A].value());
    }
    if (_apply.test(Hardware::CTRL_ENV_B)) {
        if (is_chord_b) _engine.set_param(ParamId::EnvSize, DeckRef::B, mv(ParamId::EnvSize)[DeckRef::B].value());
        _engine.set_param(ParamId::Env, DeckRef::B, mv(ParamId::Env)[DeckRef::B].value());
    }
    if (_apply.test(Hardware::CTRL_SIZE_A)) {
        if (is_chord_a) {
            _engine.set_param(ParamId::Win, DeckRef::A, mv(ParamId::Win)[DeckRef::A].value());
            _engine.set_param(ParamId::Size, DeckRef::A, mv(ParamId::Size)[DeckRef::A].value());
        }
        else if (layout_a == DeckLayout::slice && _touched.test(Alt)) {
            _engine.set_param(ParamId::PolySlice, DeckRef::A, mv(ParamId::PolySlice)[DeckRef::A].value());
        }
        else {
            _engine.set_param(ParamId::Size, DeckRef::A, mv(ParamId::Size)[DeckRef::A].value());
        }
    }
    if (_apply.test(Hardware::CTRL_SIZE_B)) {
        if (is_chord_b) {
            _engine.set_param(ParamId::Win, DeckRef::B, mv(ParamId::Win)[DeckRef::B].value());
            _engine.set_param(ParamId::Size, DeckRef::B, mv(ParamId::Size)[DeckRef::B].value());
        }
        else if (layout_b == DeckLayout::slice && _touched.test(Alt)) {
            _engine.set_param(ParamId::PolySlice, DeckRef::B, mv(ParamId::PolySlice)[DeckRef::B].value());
        }
        else {
            _engine.set_param(ParamId::Size, DeckRef::B, mv(ParamId::Size)[DeckRef::B].value());
        }
    }
    if (_apply.test(Hardware::CTRL_PITCH_A)) {
        if (_touched.test(FluxA)) {
            _engine.set_param(ParamId::FluxIntensity, DeckRef::A, mv(ParamId::FluxIntensity)[DeckRef::A].value());
        }
        else if (_touched.test(GritA)) {
            _engine.set_param(ParamId::GritIntensity, DeckRef::A, mv(ParamId::GritIntensity)[DeckRef::A].value());
        }
        else if (_aux_select && _touched.test(Alt)) {
            _engine.set_param(ParamId::Aux, DeckRef::A, mv(ParamId::Aux)[DeckRef::A].value());
        }
        else {
            // PITCH is absolute (in_value = raw pot), NOT pickup-gated (value): a pitch knob should map
            // straight to pitch. With pickup, Speed stayed pinned at its 0.5 seed until the pot was
            // swept exactly through centre, so on a melodic engine the knob appeared dead. The pickup
            // still guards the OTHER PITCH layers (Flux/Grit intensity, Aux) above.
            // CapPitchPickup engines (shuttle) route PITCH through the pickup-gated value so a
            // Play->unity snap + take_param_reseed holds until the pot is swept across it; others map
            // the raw pot straight through (a pitch knob should not go dead until swept through centre).
            auto speed_a = _pitch_pickup ? mv(ParamId::Speed)[DeckRef::A].value()
                                         : mv(ParamId::Speed)[DeckRef::A].in_value();
            if (!_aux_select && _pitch_quantized.test(DeckRef::A)) {
                speed_a = snapped_speed(speed_a);
            }
            _engine.set_param(ParamId::Speed, DeckRef::A, speed_a);
        }
    }
    if (_apply.test(Hardware::CTRL_PITCH_B)) {
        if (_touched.test(FluxB)) {
            _engine.set_param(ParamId::FluxIntensity, DeckRef::B, mv(ParamId::FluxIntensity)[DeckRef::B].value());
        }
        else if (_touched.test(GritB)) {
            _engine.set_param(ParamId::GritIntensity, DeckRef::B, mv(ParamId::GritIntensity)[DeckRef::B].value());
        }
        else if (_aux_select && _touched.test(Alt)) {
            _engine.set_param(ParamId::Aux, DeckRef::B, mv(ParamId::Aux)[DeckRef::B].value());
        }
        else {
            // PITCH is absolute (raw pot), not pickup-gated - see the deck-A note above.
            auto speed_b = _pitch_pickup ? mv(ParamId::Speed)[DeckRef::B].value()
                                         : mv(ParamId::Speed)[DeckRef::B].in_value();
            if (!_aux_select && _pitch_quantized.test(DeckRef::B)) {
                speed_b = snapped_speed(speed_b);
            }
            _engine.set_param(ParamId::Speed, DeckRef::B, speed_b);
        }
    }
    if (_apply.test(Hardware::CTRL_MODFREQ_A)) {
        if (_tap_hold.passed()) _transport.set_tempo_norm(mv(ParamId::Tempo)[DeckRef::A].value());
        else _engine.set_mod_speed(DeckRef::A, mv(ParamId::ModSpeed)[DeckRef::A].value(), _touched.test(Alt));
    }
    if (_apply.test(Hardware::CTRL_MOD_AMT_A)) {
        if (_tap_hold.passed()) _engine.set_param(ParamId::ClickMix, DeckRef::A, mv(ParamId::ClickMix)[DeckRef::A].value());
        else _engine.set_param(ParamId::ModAmp, DeckRef::A, mv(ParamId::ModAmp)[DeckRef::A].value());
    }
    if (_apply.test(Hardware::CTRL_MODFREQ_B)) {
        if (_tap_hold.passed()) _engine.set_param(ParamId::PanSpeed, DeckRef::B, mv(ParamId::PanSpeed)[DeckRef::A].value());
        else _engine.set_mod_speed(DeckRef::B, mv(ParamId::ModSpeed)[DeckRef::B].value(), _touched.test(Alt));
    }
    if (_apply.test(Hardware::CTRL_MOD_AMT_B)) {
        if (_tap_hold.passed()) _engine.set_param(ParamId::PanRange, DeckRef::B, mv(ParamId::PanRange)[DeckRef::A].value());
        else _engine.set_param(ParamId::ModAmp, DeckRef::B, mv(ParamId::ModAmp)[DeckRef::B].value());
    }
    if (_apply.test(Hardware::CTRL_SOS_A)) {
        if (_tap_hold.passed()) {
            _transport.set_key_tick_interval_norm(mv(ParamId::KeyInterval)[DeckRef::A].value());
        }
        else if (_touched.test(FluxA)) {
            _engine.set_param(ParamId::FluxMix, DeckRef::A, mv(ParamId::FluxMix)[DeckRef::A].value());
        }
        else if (_touched.test(GritA)) {
            _engine.set_param(ParamId::GritMix, DeckRef::A, mv(ParamId::GritMix)[DeckRef::A].value());
        }
        else if (_touched.test(Alt)) {
            _engine.set_param(ParamId::Feedback, DeckRef::A, mv(ParamId::Feedback)[DeckRef::A].value());
        }
        else {
            _engine.set_param(ParamId::Mix, DeckRef::A, mv(ParamId::Mix)[DeckRef::A].value());
        }
    }
    if (_apply.test(Hardware::CTRL_SOS_B)) {
        if (_touched.test(FluxB)) {
            _engine.set_param(ParamId::FluxMix, DeckRef::B, mv(ParamId::FluxMix)[DeckRef::B].value());
        }
        else if (_touched.test(GritB)) {
            _engine.set_param(ParamId::GritMix, DeckRef::B, mv(ParamId::GritMix)[DeckRef::B].value());
        }
        else if (_touched.test(Alt)) {
            _engine.set_param(ParamId::Feedback, DeckRef::B, mv(ParamId::Feedback)[DeckRef::B].value());
        }
        else {
            _engine.set_param(ParamId::Mix, DeckRef::B, mv(ParamId::Mix)[DeckRef::B].value());
        }
    }

    //Don't forget to reset flags
    _apply.reset();
    if ((!_tap_hold.passed() && !_touched.test(Alt)) && _value_display_timeout.is_passed()) {
        _reset_changing_value_id();
    }
};

// CV ///////////////////////////////////////
void CoreUI::read_cv() {
    // Platform reads + calibrates each CV jack; the engine routes by role.
    auto mix_mod_a = _hw.GetControlVoltageValue(Hardware::CV_MIX_A);
    _engine.cv_mix(DeckRef::A, _calibrator.correct(Hardware::CV_MIX_A, mix_mod_a));

    auto pos_size_mod_a = _hw.GetControlVoltageValue(Hardware::CV_SIZE_POS_A);
    auto cor_pos_size_mod_a = _calibrator.correct(Hardware::CV_SIZE_POS_A, pos_size_mod_a);
    cor_pos_size_mod_a = std::round(cor_pos_size_mod_a * 1000.f) / 1000.f;
    _engine.cv_size_pos(DeckRef::A, cor_pos_size_mod_a);

    auto raw_cv_a = _hw.GetControlVoltageValue(Hardware::CV_V_OCT_A);
    _engine.cv_voct(DeckRef::A, _calibrator.correctVOctA(raw_cv_a));

    auto mix_mod_b = _hw.GetControlVoltageValue(Hardware::CV_MIX_B);
    _engine.cv_mix(DeckRef::B, _calibrator.correct(Hardware::CV_MIX_B, mix_mod_b));

    auto pos_size_mod_b = _hw.GetControlVoltageValue(Hardware::CV_SIZE_POS_B);
    auto cor_pos_size_mod_b = _calibrator.correct(Hardware::CV_SIZE_POS_B, pos_size_mod_b);
    cor_pos_size_mod_b = std::round(cor_pos_size_mod_b * 1000.f) / 1000.f;
    _engine.cv_size_pos(DeckRef::B, cor_pos_size_mod_b);

    auto raw_cv_b = _hw.GetControlVoltageValue(Hardware::CV_V_OCT_B);
    _engine.cv_voct(DeckRef::B, _calibrator.correctVOctB(raw_cv_b));

    auto mix_mod = _hw.GetControlVoltageValue(Hardware::CV_CROSSFADE);
    _engine.cv_crossfade(_calibrator.correct(Hardware::CV_CROSSFADE, mix_mod));
}

// Gate /////////////////////////////////////
void CoreUI::process_gate_in()
{ 
    if (_storage.of(DeckRef::A).is_idle()) {
        auto a_high = _hw.GetGateInputAState();
        if (a_high && !_gate_in.test(0)) {
            _gate_a_latency.start();
        }
        _gate_in.set(0, a_high);
        if (_gate_a_latency.is_passed()) {
            _engine.on_gate_trigger(DeckRef::A);
            _show_gate_in(DeckRef::A);
        }
    }
    
    if (_storage.of(DeckRef::B).is_idle()) {
        auto b_high = _hw.GetGateInputBState();
        if (b_high && !_gate_in.test(1)) {
            _gate_b_latency.start();
        }
        _gate_in.set(1, b_high);
        if (_gate_b_latency.is_passed()) {
            _engine.on_gate_trigger(DeckRef::B);
            _show_gate_in(DeckRef::B);
        }
    }
}
void CoreUI::_process_gate_out(const DeckRef::Ref ref)
{
    if (_engine.gate_out_triggered(ref)) {
        _gate_out_timer[ref].Restart();
        _gate_out_high[ref] = true;
    }
    if (_gate_out_high[ref] && _gate_out_timer[ref].HasPassedMs(7)) {
        _gate_out_high[ref] = false;
    }
    if (ref == DeckRef::A) {
        _hw.SetGateOutA(_gate_out_high[ref]);
    }
    else {
        _hw.SetGateOutB(_gate_out_high[ref]);
    }
}

// Knobs ////////////////////////////////////
void CoreUI::_process_ui_queue()
{
    auto layout_a = _engine.deck_layout(DeckRef::A);
    auto layout_b = _engine.deck_layout(DeckRef::B);

    auto is_alt_touched = _touched.test(Alt);
    auto fx_a_touched = _touched.test(FluxA) || _touched.test(GritA);
    auto fx_b_touched = _touched.test(FluxB) || _touched.test(GritB);

    int* changing_id_a = &(_changing_value_id[DeckRef::A]);
    int* changing_id_b = &(_changing_value_id[DeckRef::B]);

    while(!_ui_queue.IsQueueEmpty())
    {
        auto event = _ui_queue.GetAndRemoveNextEvent();
        if (event.type == UiEventQueue::Event::EventType::potMoved) {
            auto val = event.asPotMoved.newPosition;
            _apply.set(event.asPotMoved.id);
            switch (event.asPotMoved.id) {
                // DECK A //////////////////////////////////////
                case Hardware::CTRL_SOS_A: {
                    mv(ParamId::FluxMix)[DeckRef::A].process(val, _touched.test(FluxA), changing_id_a);
                    mv(ParamId::GritMix)[DeckRef::A].process(val, _touched.test(GritA), changing_id_a);
                    mv(ParamId::KeyInterval)[DeckRef::A].process(val, !fx_a_touched && !is_alt_touched && _tap_hold.is_holding(), changing_id_a);
                    mv(ParamId::Mix)[DeckRef::A].process(val, !fx_a_touched && !is_alt_touched && !_tap_hold.is_holding(), changing_id_a);
                    mv(ParamId::Feedback)[DeckRef::A].process(val, !fx_a_touched && is_alt_touched && !_tap_hold.is_holding(), changing_id_a);
                    break;
                }

                case Hardware::CTRL_SIZE_A:
                    switch (layout_a) {
                        case DeckLayout::single: {
                            mv(ParamId::Size)[DeckRef::A].process(val, true, changing_id_a);
                        }
                        break;
                        case DeckLayout::slice: {
                            if (_tap_hold.passed() && _engine.size_sets_tempo(DeckRef::A)) {
                                _size_quarters[DeckRef::A].process(val, true, changing_id_a);
                                _set_tempo_by_size(DeckRef::A, val);
                            }
                            else {
                                mv(ParamId::Size)[DeckRef::A].process(val, !is_alt_touched, changing_id_a);
                                mv(ParamId::PolySlice)[DeckRef::A].process(val, is_alt_touched, changing_id_a);
                                _size_quarters[DeckRef::A].set(val);
                            }
                        }
                        break;
                        case DeckLayout::chord: {
                            mv(ParamId::Size)[DeckRef::A].process(val, !is_alt_touched, changing_id_a);
                            mv(ParamId::Win)[DeckRef::A].process(val, is_alt_touched, changing_id_a);

                        }
                        break;
                        case DeckLayout::none: break;
                    }
                    break;

                case Hardware::CTRL_POS_A:
                    // Pos keeps its original condition unless this engine opts into Alt+POS=pan; then Alt
                    // steers the knob to Pan instead. Non-opt-in engines are byte-identical here.
                    mv(ParamId::Pos)[DeckRef::A].process(val, !fx_a_touched && !(_alt_pos && is_alt_touched), changing_id_a);
                    if (_alt_pos) mv(ParamId::AltPos)[DeckRef::A].process(val, !fx_a_touched && is_alt_touched, changing_id_a);
                    mv(ParamId::FluxFb)[DeckRef::A].process(val, _touched.test(FluxA), changing_id_a);
                    break;

                case Hardware::CTRL_ENV_A:
                    if (layout_a == DeckLayout::chord) {
                        mv(ParamId::Env)[DeckRef::A].process(val, !is_alt_touched, changing_id_a);
                        mv(ParamId::EnvSize)[DeckRef::A].process(val, is_alt_touched, changing_id_a);
                    }
                    else {
                        mv(ParamId::Env)[DeckRef::A].process(val, true, changing_id_a);
                    }
                    break;

                case Hardware::CTRL_PITCH_A: {
                    if (_storage.of(DeckRef::A).state() == DeckStorage::State::selecting) {
                        // round(val * (N-1)): map the full 0..1 knob range across all N slots. The
                        // old val*N only reached the last slot in a razor-thin band near val==1
                        // (select_slot_at rejects idx>=N). Matches upstream v1.1.0's fix.
                        _storage.of(DeckRef::A).select_slot_at(std::round(val * (kStorageSlotCount - 1)));
                        break;
                    }

                    mv(ParamId::FluxIntensity)[DeckRef::A].process(val, _touched.test(FluxA), changing_id_a);
                    mv(ParamId::GritIntensity)[DeckRef::A].process(val, _touched.test(GritA), changing_id_a);
                    if (_aux_select) {
                        // Alt+PITCH is the engine's selector (e.g. edrums model); PITCH alone is pitch.
                        mv(ParamId::Speed)[DeckRef::A].process(val, !fx_a_touched && !is_alt_touched, changing_id_a);
                        mv(ParamId::Aux)[DeckRef::A].process(val, !fx_a_touched && is_alt_touched, changing_id_a);
                    }
                    else {
                        mv(ParamId::Speed)[DeckRef::A].process(val, !fx_a_touched, changing_id_a);
                        if (!fx_a_touched) {
                            _pitch_quantized.set(DeckRef::A, _touched.test(Alt));
                        }
                    }
                    break;
                }

                case Hardware::CTRL_MODFREQ_A: 
                    mv(ParamId::Tempo)[DeckRef::A].process(val, _tap_hold.passed(), changing_id_a);
                    mv(ParamId::ModSpeed)[DeckRef::A].process(val, !_tap_hold.passed(), changing_id_a);
                    break;

                case Hardware::CTRL_MOD_AMT_A: 
                    mv(ParamId::ClickMix)[DeckRef::A].process(val, _tap_hold.passed(), changing_id_a);
                    mv(ParamId::ModAmp)[DeckRef::A].process(val, !_tap_hold.passed(), changing_id_a);
                    break;

                // DECK B //////////////////////////////////////
                case Hardware::CTRL_SOS_B: {
                    mv(ParamId::FluxMix)[DeckRef::B].process(val, _touched.test(FluxB), changing_id_b);
                    mv(ParamId::GritMix)[DeckRef::B].process(val, _touched.test(GritB), changing_id_b);
                    mv(ParamId::Mix)[DeckRef::B].process(val, !fx_b_touched && !is_alt_touched, changing_id_b);
                    mv(ParamId::Feedback)[DeckRef::B].process(val, !fx_b_touched && is_alt_touched, changing_id_b);
                    break;
                }

                case Hardware::CTRL_SIZE_B:
                    switch (layout_b) {
                        case DeckLayout::single: {
                            mv(ParamId::Size)[DeckRef::B].process(val, true, changing_id_b);
                        }
                        break;
                        case DeckLayout::slice: {
                            if (_tap_hold.passed() && _engine.size_sets_tempo(DeckRef::B)) {
                                _size_quarters[DeckRef::B].process(val, true, changing_id_b);
                                _set_tempo_by_size(DeckRef::B, val);
                            }
                            else {
                                mv(ParamId::Size)[DeckRef::B].process(val, !is_alt_touched, changing_id_b);
                                mv(ParamId::PolySlice)[DeckRef::B].process(val, is_alt_touched, changing_id_b);
                                _size_quarters[DeckRef::B].set(val);
                            }
                        }
                        break;
                        case DeckLayout::chord: {
                            mv(ParamId::Size)[DeckRef::B].process(val, !is_alt_touched, changing_id_b);
                            mv(ParamId::Win)[DeckRef::B].process(val, is_alt_touched, changing_id_b);
                        }
                        break;
                        case DeckLayout::none: break;
                    }
                    break;

                case Hardware::CTRL_POS_B:
                    mv(ParamId::Pos)[DeckRef::B].process(val, !fx_b_touched && !(_alt_pos && is_alt_touched), changing_id_b);
                    if (_alt_pos) mv(ParamId::AltPos)[DeckRef::B].process(val, !fx_b_touched && is_alt_touched, changing_id_b);
                    mv(ParamId::FluxFb)[DeckRef::B].process(val, _touched.test(FluxB), changing_id_b);
                    break;

                case Hardware::CTRL_ENV_B:
                    if (layout_b == DeckLayout::chord) {
                        mv(ParamId::Env)[DeckRef::B].process(val, !is_alt_touched, changing_id_b);
                        mv(ParamId::EnvSize)[DeckRef::B].process(val, is_alt_touched, changing_id_b);
                    }
                    else {
                        mv(ParamId::Env)[DeckRef::B].process(val, true, changing_id_b);
                    }
                    break;
                
                case Hardware::CTRL_PITCH_B: {                    
                    if (_storage.of(DeckRef::B).state() == DeckStorage::State::selecting) {
                        // round(val * (N-1)): see the deck-A note above. Matches upstream v1.1.0.
                        _storage.of(DeckRef::B).select_slot_at(std::round(val * (kStorageSlotCount - 1)));
                        break;
                    }

                    mv(ParamId::FluxIntensity)[DeckRef::B].process(val, _touched.test(FluxB), changing_id_b);
                    mv(ParamId::GritIntensity)[DeckRef::B].process(val, _touched.test(GritB), changing_id_b);

                    auto no_fx_b_touched = !_touched.test(FluxB) && !_touched.test(GritB);
                    if (_aux_select) {
                        mv(ParamId::Speed)[DeckRef::B].process(val, no_fx_b_touched && !is_alt_touched, changing_id_b);
                        mv(ParamId::Aux)[DeckRef::B].process(val, no_fx_b_touched && is_alt_touched, changing_id_b);
                    }
                    else {
                        mv(ParamId::Speed)[DeckRef::B].process(val, no_fx_b_touched, changing_id_b);
                        if (no_fx_b_touched) {
                            _pitch_quantized.set(DeckRef::B, _touched.test(Alt));
                        }
                    }
                    break;
                }

                case Hardware::CTRL_MODFREQ_B: 
                    mv(ParamId::PanSpeed)[DeckRef::A].process(val, _tap_hold.passed(), changing_id_b);
                    mv(ParamId::ModSpeed)[DeckRef::B].process(val, !_tap_hold.passed(), changing_id_b);
                    break;

                case Hardware::CTRL_MOD_AMT_B:
                    mv(ParamId::PanRange)[DeckRef::A].process(val, _tap_hold.passed(), changing_id_b);
                    mv(ParamId::ModAmp)[DeckRef::B].process(val, !_tap_hold.passed(), changing_id_b);
                    break;

                case Hardware::CTRL_CROSSFADE:
                    _engine.set_param(ParamId::Crossfade, DeckRef::A, val);
                    break;
            }

            _value_display_timeout.start();
        }
    }
}

// Switchess ////////////////////////////////
void CoreUI::_process_switches()
{
    // construct into 8-bit set from inverted bitmask state
    // (all inputs are inverted due to pullups)
    std::bitset<8> sr1 = ~_hw.GetShiftRegState(0);
    std::bitset<8> sr2 = ~_hw.GetShiftRegState(1);

    // The platform reads the switch bits (hardware) and writes them as categorical config; the
    // engine maps each selector to its enums and owns the side effects (item 3a-0).

    // Mode A/B/C switch (mutex) -> route (global; deck arg ignored)
    _engine.set_config(ConfigId::Route, DeckRef::A, sr1.test(2) ? 2 : sr1.test(3) ? 1 : 0);

    // Mod A Type switch
    if (sr1.test(1)) _engine.set_config(ConfigId::ModType, DeckRef::A, 1);
    else {
        _engine.set_config(ConfigId::ModType, DeckRef::A, 0);
        _engine.set_config(ConfigId::LfoShape, DeckRef::A, sr1.test(0) ? 1 : 0);
    }

    // Mode A switch (engine reports whether it changed, so we re-apply size for the new mode)
    if (_engine.set_config(ConfigId::Mode, DeckRef::A, sr1.test(6) ? 2 : sr1.test(7) ? 1 : 0)) {
        _apply.set(Hardware::CTRL_SIZE_A);
    }

    // Size/Pos A switch
    _engine.set_config(ConfigId::StartModOn, DeckRef::A, (sr1.test(5) || !sr1.test(4)) ? 1 : 0);
    _engine.set_config(ConfigId::SizeModOn,  DeckRef::A, (sr1.test(4) || !sr1.test(5)) ? 1 : 0);

    // Mod B Type switch
    if (sr2.test(5)) _engine.set_config(ConfigId::ModType, DeckRef::B, 1);
    else {
        _engine.set_config(ConfigId::ModType, DeckRef::B, 0);
        _engine.set_config(ConfigId::LfoShape, DeckRef::B, sr2.test(4) ? 1 : 0);
    }

    // Mode B switch
    if (_engine.set_config(ConfigId::Mode, DeckRef::B, sr2.test(2) ? 2 : sr2.test(3) ? 1 : 0)) {
        _apply.set(Hardware::CTRL_SIZE_B);
    }

    // Size/Pos B switch
    _engine.set_config(ConfigId::StartModOn, DeckRef::B, (sr2.test(1) || !sr2.test(0)) ? 1 : 0);
    _engine.set_config(ConfigId::SizeModOn,  DeckRef::B, (sr2.test(0) || !sr2.test(1)) ? 1 : 0);

    // Manual tempo tap switch
    // Update no faster than 500Hz
    static auto is_tap_tapped = false;
    static uint32_t last_tap_update = 0;
    uint32_t now = System::GetNow();
    if(now - last_tap_update >= 2)
    {
        last_tap_update = now;
        is_tap_tapped = sr2.test(6);
    }
    if(is_tap_tapped) {
        if (_tap_was_tapped) return;
        _tap_was_tapped = true;
        
        if (_touched.test(Alt)) {
            _transport.toggle_source();
            _clock_source_changed = true;
            _value_display_timeout.start();
        }
        else if (_touched.test(GritA)) {
            auto rs = _engine.toggle_grit_mode(DeckRef::A);
            mv(ParamId::GritIntensity)[DeckRef::A].set(rs.intensity);
            mv(ParamId::GritMix)[DeckRef::A].set(rs.mix);
        }
        else if (_touched.test(GritB)) {
            auto rs = _engine.toggle_grit_mode(DeckRef::B);
            mv(ParamId::GritIntensity)[DeckRef::B].set(rs.intensity);
            mv(ParamId::GritMix)[DeckRef::B].set(rs.mix);
        }
        else {
            if (!_transport.is_external_sync()) {
                _transport.tap_tempo();
                mv(ParamId::Tempo)[DeckRef::A].set(tempo_abs_to_norm(_transport.tempo()));
            }
            if (!_tap_hold.is_holding()) {
                _tap_hold.begin();
            }
        }
    }
    else if (_tap_was_tapped) {
        _reset_changing_value_id();
        _tap_was_tapped = false;
        _tap_hold.end();
    }
}

// Clock /////////////////////////////////////
void CoreUI::_on_quarter(const bool is_key_quarter) 
{
    _clock_led_on = true;
    _show_key_quarter = is_key_quarter;
}

// Calibration //////////////////////////////
void CoreUI::calibrate(const bool recalibrate) {
    _calibrator.init(recalibrate);
}

bool CoreUI::_is_changing(const MValue& value) const
{
    return _changing_value_id[DeckRef::A] == value.id() || _changing_value_id[DeckRef::B] == value.id();
}

void CoreUI::_reset_changing_value_id()
{
   _changing_value_id.fill(0);
   _clock_source_changed = false;
}

void CoreUI::_set_tempo_by_size(const DeckRef::Ref ref, const float fraction)
{
    auto bpm = _engine.tempo_to_fit(ref, fraction);
    auto norm = tempo_abs_to_norm(bpm);
    mv(ParamId::Tempo)[DeckRef::A].set(norm);
    _transport.set_tempo_norm(norm);
}
