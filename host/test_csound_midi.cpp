// Host test for the Csound MIDI plumbing (src/engine/csound/csound_midi.h, roadmap #3): the
// note->frequency map, the CS_INSTR_EVENT p-field builder, and the lock-free pending-note ring that
// carries notes from the main loop to the audio ISR. No libcsound dependency, so it runs off-target;
// the csoundEvent/csoundGetInstrNumber calls live in the QSPI-only engine and aren't exercised here.
// Build: `make -C host test-csound-midi`.

#include "engine/csound/csound_midi.h"

#include <cstdio>
#include <cmath>
#include <thread>

using namespace spotykach;

namespace {

int g_failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); g_failures++; }
}
bool approx(float a, float b, float rel = 1e-4f) {
    const float d = std::fabs(a - b), m = std::fabs(b) > 1.f ? std::fabs(b) : 1.f;
    return d <= rel * m;
}

void test_note_to_hz() {
    std::printf("midi note -> Hz (12-TET, A4=440)\n");
    check(approx(midi_note_to_hz(69), 440.0f),       "note 69 = A4 = 440 Hz");
    check(approx(midi_note_to_hz(57), 220.0f),       "note 57 = A3 = 220 Hz");
    check(approx(midi_note_to_hz(81), 880.0f),       "note 81 = A5 = 880 Hz");
    check(approx(midi_note_to_hz(60), 261.6256f),    "note 60 = middle C ~= 261.63 Hz");
    // Octave invariant: +12 semitones doubles the frequency, across the range.
    for (int n = 0; n <= 115; n++)
        check(approx(midi_note_to_hz(n + 12), 2.0f * midi_note_to_hz(n)),
              "note+12 doubles frequency");
    check(midi_note_to_hz(0) > 0.f && std::isfinite(midi_note_to_hz(127)),
          "extremes are finite and positive");
}

void test_pfields() {
    std::printf("CS_INSTR_EVENT p-field builder\n");
    float pf[kMidiNoteFields];
    const int n = csound_note_pfields(pf, 7, kMidiNoteDur, 69, 1);
    check(n == kMidiNoteFields && n == 5, "writes 5 p-fields");
    check(pf[0] == 7.0f,                  "p1 = instrument number");
    check(pf[1] == 0.0f,                  "p2 = 0 (start now)");
    check(approx(pf[2], kMidiNoteDur),    "p3 = note duration");
    check(approx(pf[3], 440.0f),          "p4 = frequency for the note");
    check(pf[4] == 1.0f,                  "p5 = deck (B)");

    csound_note_pfields(pf, 3, 0.25f, 60, 0);
    check(pf[0] == 3.0f && pf[4] == 0.0f && approx(pf[3], midi_note_to_hz(60)),
          "instr/deck/freq pass through for a second note");
}

void test_ring_fifo_and_drop() {
    std::printf("note ring: FIFO, empty, full-drop, capacity\n");
    NoteQueue<8> q;
    MidiNoteEvent e;
    check(!q.pop(e), "empty ring pops nothing");

    for (int i = 0; i < 8; i++) check(q.push({ (uint8_t)(40 + i), (uint8_t)(i & 1) }), "fill to capacity");
    check(!q.push({ 99, 0 }), "the 9th push (over capacity 8) is dropped");

    for (int i = 0; i < 8; i++) {            // drains in push order
        check(q.pop(e), "pop a queued note");
        check(e.note == (uint8_t)(40 + i) && e.deck == (uint8_t)(i & 1), "FIFO order + payload intact");
    }
    check(!q.pop(e), "ring empty again after draining");
}

void test_ring_wraparound() {
    std::printf("note ring: wraparound over many cycles\n");
    NoteQueue<4> q;
    MidiNoteEvent e{};   // value-init: a failing pop() leaves e untouched, and the next line reads e.note
    uint8_t next_push = 0, next_pop = 0;
    for (int cycle = 0; cycle < 1000; cycle++) {
        // push 3, pop 3 each cycle -> head/tail sweep past the 4-slot boundary repeatedly
        for (int k = 0; k < 3; k++) check(q.push({ next_push++, 0 }), "push within capacity");
        for (int k = 0; k < 3; k++) {
            check(q.pop(e), "pop within available");
            check(e.note == next_pop++, "value preserved across wraparound");
        }
    }
}

// Light SPSC concurrency exercise on the host: one producer thread pushes a known sequence, one
// consumer thread drains it; every popped value must be in order with none invented. (On target the
// producer is the main loop and the consumer the audio ISR; the host threads stand in for that.)
void test_ring_spsc_threads() {
    std::printf("note ring: single-producer/single-consumer across threads\n");
    NoteQueue<16> q;
    const int kTotal = 200000;
    int popped = 0, out_of_order = 0;
    std::thread consumer([&] {
        MidiNoteEvent e;
        uint8_t expect = 0;
        while (popped < kTotal) {
            if (q.pop(e)) {
                if (e.note != expect) out_of_order++;
                expect++;
                popped++;
            }
        }
    });
    std::thread producer([&] {
        for (int i = 0; i < kTotal; i++) while (!q.push({ (uint8_t)i, 0 })) { /* spin if full */ }
    });
    producer.join();
    consumer.join();
    check(popped == kTotal, "consumer received every produced note");
    check(out_of_order == 0, "notes arrived in order with none dropped or invented");
}

} // namespace

int main() {
    std::printf("=== Csound MIDI plumbing ===\n");
    test_note_to_hz();
    test_pfields();
    test_ring_fifo_and_drop();
    test_ring_wraparound();
    test_ring_spsc_threads();

    if (g_failures) { std::printf("\nFAILED: %d check(s)\n", g_failures); return 1; }
    std::printf("\nAll Csound MIDI tests passed.\n");
    return 0;
}
