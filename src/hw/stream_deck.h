#pragma once

#include "engine/istreamdeck.h"
#include "memory/spsc_ring.h"
#include "memory/audio_stream.h"
#include "memory/wav_stream.h"
#include "memory/raw_stream.h"
#include "memory/converting_source.h"
#include "hw/fat_file.h"

#include <atomic>
#include <cstdint>

namespace spotykach {

// Platform streaming service: bridges the audio ISR (lock-free rings) and FatFs (main-loop SD I/O) for
// the `tape` engine. Two INDEPENDENT decks (A/B), each its own play-XOR-record state machine, file, and
// ring; the two share one scratch buffer because the main-loop pump services them sequentially (never
// concurrently). Control (start_*/stop) runs in the main loop; the ISR only calls the *_consume/produce.
class StreamDeck : public IStreamDeck {
public:
    struct Mem {
        uint8_t* ring_a;    uint32_t ring_a_bytes;     // power-of-two SDRAM ring, deck A (play XOR record)
        uint8_t* ring_b;    uint32_t ring_b_bytes;     // power-of-two SDRAM ring, deck B
        uint8_t* scratch;   uint32_t scratch_bytes;    // f_read/f_write staging, shared (sequential pumps)
    };
    void init(const Mem& m);

    // Main-loop pump (AppImpl::Loop): does the slow SD I/O for each deck that is active/finalizing.
    void process();

    // --- IStreamDeck (per-deck) -------------------------------------------------------------------
    uint32_t play_consume(DeckRef::Ref deck, uint8_t* dst, uint32_t n) override;       // ISR
    uint32_t record_produce(DeckRef::Ref deck, const uint8_t* src, uint32_t n) override; // ISR
    bool is_playing(DeckRef::Ref deck)   const override { return _d[deck].mode.load(std::memory_order_acquire) == Mode::play; }
    bool is_recording(DeckRef::Ref deck) const override { return _d[deck].mode.load(std::memory_order_acquire) == Mode::record; }
    bool start_play(DeckRef::Ref deck, const char* path)   override;   // main loop
    bool start_record(DeckRef::Ref deck, const char* path) override;   // main loop
    void stop(DeckRef::Ref deck)                           override;   // main loop
    void set_loop(DeckRef::Ref deck, bool loop)            override;   // main loop
    uint32_t loop_frames(DeckRef::Ref deck) const          override;
    bool exists(const char* path) const                    override;   // main loop (f_stat)

    // Raw 16-bit-mono streaming for the radio engine (main loop). start_play_raw seeks-on-open to the
    // free-running playhead position; frames_of/scan_bank build the bank index off the audio path.
    bool start_play_raw(DeckRef::Ref deck, const char* path, uint32_t start_frame, bool loop) override;
    bool start_play_wav(DeckRef::Ref deck, const char* path, uint32_t start_frame, bool loop) override;
    bool seek_play(DeckRef::Ref deck, uint32_t frame) override;
    uint32_t frames_of(const char* path) const override;
    int  scan_bank(const char* dir, BankEntry* out, int max) const override;
    int  read_text(const char* path, char* buf, int max) const override;
    bool write_text(const char* path, const char* buf, int n) override;

private:
    enum class Mode : uint8_t { idle, play, record };

    // One self-contained streaming unit per deck. The ring is shared between play and record (a deck is
    // only ever doing one); play/record streams bind to it and to the common scratch at init().
    struct Deck {
        std::atomic<Mode> mode{Mode::idle};
        bool            finalizing = false;  // record stopped; main loop flushing the tail + finalizing
        SpscRing        ring;
        PlayStream      play;
        RecordStream    record;
        WavStreamReader reader;
        RawStreamReader raw;                 // headerless 16-bit-mono source (radio/bard engines)
        ConvertingSource conv;               // depth/channel adapter, inserted only for a non-native file
        bool            raw_src = false;     // the live play source is `raw` (frame-seekable), not `reader`
        bool            conv_on = false;     // the live play source is routed through `conv`
        WavStreamWriter writer;
        FatFile         file;                // one file handle per deck (play XOR record)
    };
    Deck _d[2];

    void _pump(Deck& d);
    static IChunkSource* _play_src(Deck& d);
};

} // namespace spotykach
