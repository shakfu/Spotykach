// Compiled into the firmware only for SPK_USE_STREAM engines (tape, shuttle) - lives in the src/hw
// wildcard; the guard keeps every non-streaming engine byte-identical.
#if defined(SPK_USE_STREAM)

#include "stream_deck.h"

// Size-optimize: main-loop SD glue, never the audio path.
#pragma GCC optimize("Os")

using namespace spotykach;

// Minimum size for a real radio station: drops macOS AppleDouble metadata stubs (`._*.raw`, ~4 KB) and
// any empty/truncated file. No real station is a fraction of a second; 32 KB ~= 0.34 s of mono int16 @48k.
static constexpr uint32_t kMinStationBytes = 32u * 1024u;

// Build "dir/name" into buf; false if it would not fit. Used to probe a .wav station's header in scan_bank.
static bool join_path(const char* dir, const char* name, char* buf, int cap) {
    int n = 0;
    for (const char* p = dir;  *p; ++p) { if (n >= cap - 1) return false; buf[n++] = *p; }
    if (n >= cap - 1) return false;
    buf[n++] = '/';
    for (const char* p = name; *p; ++p) { if (n >= cap - 1) return false; buf[n++] = *p; }
    buf[n] = '\0';
    return true;
}

void StreamDeck::init(const Mem& m) {
    _d[0].ring.init(m.ring_a, m.ring_a_bytes);
    _d[1].ring.init(m.ring_b, m.ring_b_bytes);
    for (auto& d : _d) {
        d.play.init(&d.ring, m.scratch, m.scratch_bytes);     // play XOR record per deck -> one ring,
        d.record.init(&d.ring, m.scratch, m.scratch_bytes);   // and one scratch shared across decks
    }
}

bool StreamDeck::start_play(DeckRef::Ref deck, const char* path) {
    Deck& d = _d[deck];
    if (d.mode.load(std::memory_order_acquire) != Mode::idle || d.finalizing) return false;
    if (!d.file.open_read(path)) return false;
    if (!d.reader.begin(&d.file)) { d.file.close(); return false; }   // not a valid WAV
    d.raw_src = false;
    d.play.start(&d.reader);
    d.mode.store(Mode::play, std::memory_order_release);             // ISR may now consume
    return true;
}

bool StreamDeck::start_record(DeckRef::Ref deck, const char* path) {
    Deck& d = _d[deck];
    if (d.mode.load(std::memory_order_acquire) != Mode::idle || d.finalizing) return false;
    if (!d.file.open_write(path)) return false;
    if (!d.writer.begin(&d.file, 1)) { d.file.close(); return false; }  // mono header; placeholder failed
    d.record.start(&d.writer);
    d.mode.store(Mode::record, std::memory_order_release);          // ISR may now produce
    return true;
}

void StreamDeck::stop(DeckRef::Ref deck) {
    Deck& d = _d[deck];
    const Mode m = d.mode.load(std::memory_order_acquire);
    if (m == Mode::play) {
        d.mode.store(Mode::idle, std::memory_order_release);       // ISR stops consuming
        d.file.close();
    } else if (m == Mode::record) {
        d.record.stop();                                          // request end-of-record (flush + patch)
        d.mode.store(Mode::idle, std::memory_order_release);       // ISR stops producing immediately...
        d.finalizing = true;                                      // ...main loop flushes the tail below
    }
}

void StreamDeck::set_loop(DeckRef::Ref deck, bool loop) {
    _d[deck].play.set_loop(loop);   // honored on the next play EOF (pump rewinds instead of finishing)
}

uint32_t StreamDeck::loop_frames(DeckRef::Ref deck) const {
    const Deck& d = _d[deck];
    if (d.mode.load(std::memory_order_acquire) != Mode::play) return 0;
    return d.reader.data_bytes() / static_cast<uint32_t>(sizeof(float));  // mono float: 4 bytes/frame
}

bool StreamDeck::exists(const char* path) const {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;   // f_stat uses no FIL handle - safe alongside an open deck file
}

// Open a headerless raw 16-bit-mono station and stream forward from `start_frame` (the free-running
// playhead jump). Looping rewinds at EOF so a tuned-in station repeats. f_stat gives the length with no
// header to parse; seek-on-open is a single f_lseek before the first pump.
bool StreamDeck::start_play_raw(DeckRef::Ref deck, const char* path, uint32_t start_frame, bool loop) {
    Deck& d = _d[deck];
    if (d.mode.load(std::memory_order_acquire) != Mode::idle || d.finalizing) return false;
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return false;                  // missing station
    if (!d.file.open_read(path)) return false;
    if (!d.raw.begin(&d.file, static_cast<uint32_t>(fno.fsize))) { d.file.close(); return false; }  // empty
    d.raw.seek_to_frame(start_frame);                               // jump to the virtual position
    d.play.set_loop(loop);
    d.play.start(&d.raw);
    d.raw_src = true;
    d.mode.store(Mode::play, std::memory_order_release);            // ISR may now consume
    return true;
}

// As start_play_raw, but the source is a 16-bit-mono PCM .wav: begin_wav parses the header (the file's
// own rate is captured into the bank index by scan_bank, not here) and the seek lands past it.
bool StreamDeck::start_play_wav(DeckRef::Ref deck, const char* path, uint32_t start_frame, bool loop) {
    Deck& d = _d[deck];
    if (d.mode.load(std::memory_order_acquire) != Mode::idle || d.finalizing) return false;
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return false;
    if (!d.file.open_read(path)) return false;
    uint32_t rate = 0;
    if (!d.raw.begin_wav(&d.file, static_cast<uint32_t>(fno.fsize), rate)) { d.file.close(); return false; }
    d.raw.seek_to_frame(start_frame);
    d.play.set_loop(loop);
    d.play.start(&d.raw);
    d.raw_src = true;
    d.mode.store(Mode::play, std::memory_order_release);
    return true;
}

// The lighter in-file seek: reposition the live handle instead of reopening the file. Sequence matters -
// drop the deck to idle FIRST so the ISR stops consuming (play_consume on an idle deck returns 0, which
// the caller reads as an underrun and handles as silence), then f_lseek, then re-arm. play.start() resets
// the ring and clears the EOF latch, so a deck that had already run out becomes playable again.
bool StreamDeck::seek_play(DeckRef::Ref deck, uint32_t frame) {
    Deck& d = _d[deck];
    if (d.mode.load(std::memory_order_acquire) != Mode::play || d.finalizing) return false;
    if (!d.raw_src || !d.file.is_open()) return false;      // the float-WAV play path is not frame-seekable
    d.mode.store(Mode::idle, std::memory_order_release);    // ISR stops consuming
    if (!d.raw.seek_to_frame(frame)) {
        d.mode.store(Mode::play, std::memory_order_release);  // seek failed: leave the deck as it was
        return false;
    }
    d.play.start(&d.raw);                                   // flush + re-seed the ring, clear eof
    d.mode.store(Mode::play, std::memory_order_release);
    return true;
}

uint32_t StreamDeck::frames_of(const char* path) const {
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return 0;
    return static_cast<uint32_t>(fno.fsize) / RawStreamReader::kBytesPerFrame;
}

// Enumerate the `.raw` files in `dir` (f_opendir/f_readdir), filling up to `max` BankEntry slots with
// each station's short name + frame length. 8.3 names only (longer names are skipped) so the stored
// name is a re-openable FatFs short name and the index stays bounded. Main-loop only.
//
// macOS filter: copying files to a FAT card in Finder writes, next to every `NAME.raw`, a hidden
// AppleDouble companion `._NAME.raw` (~4 KB of metadata, NOT audio) plus `.DS_Store`. The companion ends
// in `.raw`, so without filtering it would be indexed as a bogus tiny "station" that loops a sliver of
// garbage (a fast stutter, pitched by the resampler) - and since there is one per real file, sweeping
// the tuning knob alternates real/garbage. We drop them three ways: the FAT hidden/system attribute
// (macOS sets AM_HID on dot-prefixed files), a leading-dot name, and a minimum size (no real station is
// a fraction of a second; an AppleDouble stub is well under that).
int StreamDeck::scan_bank(const char* dir, BankEntry* out, int max) const {
    DIR dp;
    if (f_opendir(&dp, dir) != FR_OK) return 0;
    FILINFO fno;
    int count = 0;
    while (count < max && f_readdir(&dp, &fno) == FR_OK && fno.fname[0] != 0) {
        if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;      // skip dirs + macOS hidden dotfiles
        const char* name = fno.fname;
        if (name[0] == '.') continue;                               // skip .DS_Store / ._* (when visible)
        // measure name length and locate the last '.'; require a short (<=12 char) name ending in
        // .raw (headerless) or .wav (16-bit-mono PCM).
        int len = 0; const char* dot = nullptr;
        for (const char* p = name; *p; ++p) { if (*p == '.') dot = p; ++len; }
        if (len == 0 || len > 12 || !dot) continue;
        const char* e = dot + 1;
        auto eq = [](char c, char lo) { return c == lo || c == (lo - 32); };  // case-insensitive
        const bool is_raw = eq(e[0], 'r') && eq(e[1], 'a') && eq(e[2], 'w') && e[3] == '\0';
        const bool is_wav = eq(e[0], 'w') && eq(e[1], 'a') && eq(e[2], 'v') && e[3] == '\0';
        if (!is_raw && !is_wav) continue;
        if (fno.fsize < kMinStationBytes) continue;                 // AppleDouble stub / too-short file

        uint32_t frames = 0, rate = 0;
        if (is_wav) {
            // Open + parse the header for the body length and the file's own sample rate; skip a file
            // that is not 16-bit mono PCM. One f_open per WAV at bank load - off the audio path.
            char path[64];
            if (!join_path(dir, name, path, sizeof(path))) continue;
            FatFile wf;
            if (!wf.open_read(path)) continue;
            RawStreamReader probe;
            const bool ok = probe.begin_wav(&wf, static_cast<uint32_t>(fno.fsize), rate);
            wf.close();
            if (!ok) continue;
            frames = probe.frames();
        } else {
            frames = static_cast<uint32_t>(fno.fsize) / RawStreamReader::kBytesPerFrame;
        }

        int j = 0;
        for (; name[j] && j < 12; ++j) out[count].name[j] = name[j];
        out[count].name[j] = '\0';
        out[count].frames = frames;
        out[count].rate   = rate;       // 0 for raw -> the engine uses rate.txt; else the wav's own rate
        out[count].is_wav = is_wav;
        ++count;
    }
    f_closedir(&dp);
    bank_sort(out, count);   // deterministic alphabetical order, not FAT directory-enumeration order
    return count;
}

// Read a small text file (the radio's on-card rate.txt) into `buf`, NUL-terminated. A local FatFile,
// opened/closed here - read at boot before any deck streams, so it never races a deck file. Main-loop only.
int StreamDeck::read_text(const char* path, char* buf, int max) const {
    if (max <= 0) return 0;
    FatFile f;
    if (!f.open_read(path)) { buf[0] = '\0'; return 0; }   // missing -> empty
    const uint32_t got = f.read(buf, static_cast<uint32_t>(max - 1));
    f.close();
    buf[got] = '\0';
    return static_cast<int>(got);
}

// Write a small text file (the bard engine's resume table), truncating any existing content. A local
// FatFile, opened/closed here - it never touches a deck's file handle, so it cannot race a streaming
// deck. Main-loop only. Returns false on any failure (missing tree, write-protected, full, short write);
// the caller treats that as "persistence unavailable" and keeps playing.
bool StreamDeck::write_text(const char* path, const char* buf, int n) {
    if (n < 0) return false;
    FatFile f;
    if (!f.open_write(path)) return false;
    const uint32_t want = static_cast<uint32_t>(n);
    const uint32_t put  = want ? f.write(buf, want) : 0u;
    f.close();
    return put == want;
}

void StreamDeck::process() {
    for (auto& d : _d) _pump(d);   // service each deck's slow SD I/O sequentially (shared scratch)
}

void StreamDeck::_pump(Deck& d) {
    const Mode m = d.mode.load(std::memory_order_acquire);
    if (m == Mode::play) {
        d.play.pump();
        if (d.play.finished()) {                                  // file reached EOF + drained
            d.mode.store(Mode::idle, std::memory_order_release);
            d.file.close();
        }
    } else if (m == Mode::record) {
        d.record.pump();                                          // drain captured audio to SD
    }
    if (d.finalizing) {
        d.record.pump();                                          // flush the tail; finalize() patches header
        if (d.record.finished()) { d.file.close(); d.finalizing = false; }
    }
}

uint32_t StreamDeck::play_consume(DeckRef::Ref deck, uint8_t* dst, uint32_t n) {
    Deck& d = _d[deck];
    if (d.mode.load(std::memory_order_acquire) != Mode::play) return 0;
    return d.play.consume(dst, n);   // zero-fills any shortfall (silence)
}

uint32_t StreamDeck::record_produce(DeckRef::Ref deck, const uint8_t* src, uint32_t n) {
    Deck& d = _d[deck];
    if (d.mode.load(std::memory_order_acquire) != Mode::record) return 0;
    return d.record.produce(src, n);
}

#endif // SPK_USE_STREAM
