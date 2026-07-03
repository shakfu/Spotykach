#include "core.ui.h"
#include "config.h"
#include "daisy.h"

// Size-optimize this whole TU to reclaim SRAM_EXEC for item 3. MIDI parsing/clocking is
// per-message control glue, not per-sample DSP, so -Os is perf-irrelevant here; the audio
// DSP in core/*.cpp stays -O2 -funroll-loops. (Same idiom as core.ui.{cpp,leds,pads}.cpp.)
#pragma GCC optimize("Os")

using namespace spotykach;
using namespace daisy;

// CLOCK ///////////////////////////////////
static bool clock_state = false;
void CoreUI::tick()
{
    auto new_state = false;
    auto midi_state = _process_midi();
    switch (_transport.source()) {
        case ClockSource::ts4: new_state = _hw.GetClockInputState(); break;
        case ClockSource::midi: new_state = midi_state; break;
        default: break;
    }
    _transport.tick(new_state && !clock_state);
    clock_state = new_state;

    // Modified libDaisy MIDI handlers require explicit call to transmit
    // enqueued messages instead of blocking every time a message is sent
    _hw.midi_uart.TransmitEnqueuedMessages();
#ifdef SPK_USB_MIDI
    _hw.midi_usb.TransmitEnqueuedMessages();
#endif
}

// MIDI /////////////////////////////////////

// Map a daisy channel-voice MidiEvent to its MIDI status byte (status nibble | channel), or 0 for event
// types that aren't a 3-byte channel-voice message (the caller skips those for the MidiIn forward). The
// MidiMessageType enum is ordered NoteOff,NoteOn,PolyKeyPressure,ControlChange,ProgramChange,
// ChannelPressure,PitchBend - i.e. status nibbles 0x80..0xE0. ChannelMode is CC (0xB0); SysEx / system
// common are not representable in a 3-byte MidiMsg and are dropped.
static uint8_t chan_status_byte(const MidiEvent& e)
{
    switch (e.type) {
        case MidiMessageType::NoteOff:
        case MidiMessageType::NoteOn:
        case MidiMessageType::PolyphonicKeyPressure:
        case MidiMessageType::ControlChange:
        case MidiMessageType::ProgramChange:
        case MidiMessageType::ChannelPressure:
        case MidiMessageType::PitchBend:
            return static_cast<uint8_t>((0x80 + (static_cast<int>(e.type) << 4)) | (e.channel & 0x0f));
        case MidiMessageType::ChannelMode:
            return static_cast<uint8_t>(0xB0 | (e.channel & 0x0f));   // channel-mode = CC 120..127
        default:
            return 0;
    }
}

// MIDI status byte for the system-realtime messages worth forwarding to a patch (clock/transport). The
// rest (SPP, active sensing, reset) are not forwarded.
static uint8_t srt_status_byte(SystemRealTimeType t)
{
    switch (t) {
        case SystemRealTimeType::TimingClock: return 0xF8;
        case SystemRealTimeType::Start:       return 0xFA;
        case SystemRealTimeType::Continue:    return 0xFB;
        case SystemRealTimeType::Stop:        return 0xFC;
        default:                              return 0;
    }
}

// Handle one parsed MIDI event, from either transport (UART jack or the rear USB-C). The source is
// irrelevant to the meaning of the message, so both streams funnel through here.
void CoreUI::_handle_midi_event(daisy::MidiEvent& event, bool& has_clock)
{
    // Forward the full channel-voice stream to the engine - ChucK delivers it to a patch's MidiIn
    // (real velocity, NoteOff, CC, pitch-bend, aftertouch, program change). Engines that only use
    // handle_midi_note ignore this (default no-op). data[0]/data[1] are the raw MIDI data bytes.
    if (uint8_t st = chan_status_byte(event))
        _engine.handle_midi_message(st, event.data[0], event.data[1]);

    switch(event.type)
    {
        case MidiMessageType::SystemRealTime: {
            has_clock = _process_realtime(event) || has_clock;
            // Also forward clock/start/continue/stop to MidiIn (single status byte, no data) so a
            // patch can sync to host MIDI clock - independent of the firmware's own transport above.
            if (uint8_t st = srt_status_byte(event.srt_type))
                _engine.handle_midi_message(st, 0, 0);
        }
        break;

        case MidiMessageType::NoteOn: {
            auto e = event.AsNoteOn();
            auto ref = _engine.handle_midi_note(e.channel, e.note);  // deck -> gate-in LED only
            if (ref != DeckRef::Count) _show_gate_in(ref);
        }
        break;

        default: break;
    }
}

bool CoreUI::_process_midi()
{
    bool has_clock = false;

    // Drain both MIDI transports: the TRS/UART jack and USB device MIDI on the rear USB-C
    // (the same port used for DFU flashing). Either can drive any MIDI-aware engine.
    _hw.midi_uart.Listen();
    while(_hw.midi_uart.HasEvents()) {
        auto event = _hw.midi_uart.PopEvent();
        _handle_midi_event(event, has_clock);
    }

#ifdef SPK_USB_MIDI
    _hw.midi_usb.Listen();
    while(_hw.midi_usb.HasEvents()) {
        auto event = _hw.midi_usb.PopEvent();
        _handle_midi_event(event, has_clock);
    }
#endif

    return has_clock;
}
bool CoreUI::_process_realtime(daisy::MidiEvent& event)
{
    switch (event.srt_type) {
        case SystemRealTimeType::TimingClock: return true;
        // The clock reset is a platform/transport action; the engine only reacts (deck play/stop).
        case SystemRealTimeType::Start:
        case SystemRealTimeType::Continue: _transport.reset(); _engine.handle_midi_transport(true); break;
        case SystemRealTimeType::Stop:     _engine.handle_midi_transport(false); break;
        default: break;
    }

    return false;
}

void CoreUI::_process_clock_out()
{
    _hw.midi_uart.EnqueueMessage(MidiTxMessage::SystemRealtimeClock());
#ifdef SPK_USB_MIDI
    _hw.midi_usb.EnqueueMessage(MidiTxMessage::SystemRealtimeClock());
#endif
}
