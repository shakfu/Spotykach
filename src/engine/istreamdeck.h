#pragma once

#include "engine/deck_ref.h"

#include <cstdint>

namespace spotykach {

// Platform streaming service, injected into the engine via EngineContext (like ITransport). Lets an
// engine play arbitrarily long files from SD and record arbitrarily long takes to SD, bypassing the
// in-SDRAM loop-length cap: the audio ISR only ever touches lock-free rings (the *_consume/*_produce
// calls), while the platform does the slow FatFs I/O in the main loop.
//
// Two INDEPENDENT decks (A/B), each its own play-XOR-record state machine, file, and ring - so the
// tape engine can run them like a pair of record decks (e.g. play A while recording B). Every call is
// per-deck.
//
// Threading contract:
//  - play_consume / record_produce are called from the audio ISR (process()). They never block.
//  - start_play / start_record / stop are control ops called from the MAIN LOOP only (e.g. the engine's
//    on_play_pad/on_record_pad handlers, which the UI invokes outside the audio ISR). They touch FatFs.
//  - is_playing / is_recording are cheap state reads, safe from either side.
// One enumerated station in a radio bank (scan_bank below): its 8.3-safe filename (so the path stays
// small/bounded) and its length in frames (signed-16 mono). Files with longer names are skipped by the
// scanner, so `name` is always a re-openable FatFs short name. A station is either a headerless `.raw`
// (is_wav=false) or a 16-bit-mono `.wav` (is_wav=true), opened via start_play_raw / start_play_wav.
struct BankEntry {
    char     name[13];   // up to 12 chars + NUL (8.3)
    uint32_t frames;     // length in source frames (body bytes / 2)
    uint32_t rate;       // source sample rate: 0 for raw (headerless -> use rate.txt), else the wav rate
    bool     is_wav;     // false = headerless .raw, true = 16-bit-mono PCM .wav
};

// Order a freshly-scanned bank by case-insensitive filename so the selector index (radio station /
// pstretch clip) follows a deterministic alphabetical order rather than FAT directory-enumeration order.
// Small n (a few dozen) -> in-place insertion sort, no <algorithm>/allocation; safe to call from the main
// loop. The compare is lexicographic, so zero-pad numeric names (01.wav .. 12.wav) for natural numeric order.
inline void bank_sort(BankEntry* out, int n) {
    for (int i = 1; i < n; ++i) {
        const BankEntry key = out[i];
        int j = i - 1;
        while (j >= 0) {
            const char* a = key.name; const char* b = out[j].name;
            int cmp = 0;
            for (;;) {
                char ca = *a++, cb = *b++;
                if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
                if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
                if (ca != cb) { cmp = (ca < cb) ? -1 : 1; break; }
                if (ca == '\0') break;                       // equal
            }
            if (cmp < 0) { out[j + 1] = out[j]; --j; } else break;   // stable: stop on >=
        }
        out[j + 1] = key;
    }
}

struct IStreamDeck {
    virtual ~IStreamDeck() = default;

    // --- ISR side (process()) --------------------------------------------------------------------
    // Pull `n` bytes of playback audio for `deck` into `dst`; shortfall is zero-filled (silence).
    virtual uint32_t play_consume(DeckRef::Ref deck, uint8_t* dst, uint32_t n) = 0;
    // Push `n` bytes of record audio for `deck`; bytes that don't fit are dropped (never blocks).
    virtual uint32_t record_produce(DeckRef::Ref deck, const uint8_t* src, uint32_t n) = 0;

    // --- state (either side) ---------------------------------------------------------------------
    virtual bool is_playing(DeckRef::Ref deck)   const = 0;
    virtual bool is_recording(DeckRef::Ref deck) const = 0;

    // --- main-loop control -----------------------------------------------------------------------
    virtual bool start_play(DeckRef::Ref deck, const char* path)   = 0;  // false if busy / file missing
    virtual bool start_record(DeckRef::Ref deck, const char* path) = 0;  // false if busy
    virtual void stop(DeckRef::Ref deck)                           = 0;  // stop deck; finalizes a record

    // Looping: when enabled, playback rewinds to the file start at EOF instead of finishing. set_loop is
    // a cheap flag write (safe from the main loop); loop_frames is the playing file's length in frames
    // (0 if the deck is not playing) so the engine can shape fades/decay per loop.
    virtual void     set_loop(DeckRef::Ref deck, bool loop)  = 0;
    virtual uint32_t loop_frames(DeckRef::Ref deck) const    = 0;

    // Main-loop file probe (f_stat): does `path` exist on the card? Used to show recorded-vs-empty
    // slots. Not for the audio path.
    virtual bool     exists(const char* path) const = 0;

    // --- Raw playback (RadioMusic radio engine) --------------------------------------------------
    // Default no-op bodies (Strategy A): only the real StreamDeck overrides them; tape and host stubs
    // need nothing. All main-loop only (they touch FatFs), like start_play.
    //
    // Open a HEADERLESS raw 16-bit-mono file and begin streaming forward from frame `start_frame`
    // (seek-on-open - the free-running-playhead jump). `loop` rewinds at EOF instead of finishing.
    // False if the deck is busy or the file is missing.
    virtual bool     start_play_raw(DeckRef::Ref, const char* /*path*/, uint32_t /*start_frame*/,
                                    bool /*loop*/) { return false; }
    // As start_play_raw, but for a 16-bit-mono PCM .wav: the header is parsed and the seek is past it.
    // False if busy / missing / not a 16-bit-mono PCM WAV.
    virtual bool     start_play_wav(DeckRef::Ref, const char* /*path*/, uint32_t /*start_frame*/,
                                    bool /*loop*/) { return false; }
    // Re-seek a PLAYING deck's ALREADY-OPEN raw/wav file to `frame`: an f_lseek on the live handle plus a
    // ring flush, instead of the f_open + (for a .wav) header re-parse that start_play_* pays. This is the
    // "lighter in-file seek" docs/dev/radio-impl.md asks for; the bard engine's bookmark jumps take it and
    // fall back to stop + start_play_* only when the target is a different file. False if the deck is not
    // playing such a stream. Main-loop only.
    virtual bool     seek_play(DeckRef::Ref, uint32_t /*frame*/) { return false; }
    // Length of a raw file in frames (f_stat filesize / 2; 0 if missing). No file open - used to index a
    // bank's station lengths for the modulo math. Main-loop only.
    virtual uint32_t frames_of(const char* /*path*/) const { return 0; }
    // Enumerate the `.raw` stations in directory `dir` (f_opendir/f_readdir), filling up to `max`
    // BankEntry slots (8.3 name + frame length); returns the count found. Main-loop only.
    virtual int      scan_bank(const char* /*dir*/, BankEntry* /*out*/, int /*max*/) const { return 0; }
    // Read up to max-1 bytes of a small text file into `buf` and NUL-terminate; returns bytes read (0 if
    // missing/empty). Used to read the radio's on-card rate.txt. Main-loop only; not for audio data.
    virtual int      read_text(const char* /*path*/, char* /*buf*/, int /*max*/) const { return 0; }
    // Write `n` bytes of text to `path`, truncating any existing file; true on success. The mirror of
    // read_text and the only WRITE an engine can make outside a record deck - used by the bard engine to
    // persist its small resume table. Main-loop only (FatFs); never for audio data, and never called from
    // process(). A failure (card absent / write-protected / full) is reported, not fatal: the caller is
    // expected to carry on and stop retrying.
    virtual bool     write_text(const char* /*path*/, const char* /*buf*/, int /*n*/) { return false; }
};

} // namespace spotykach
