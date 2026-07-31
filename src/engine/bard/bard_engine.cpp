#include "engine/bard/bard_engine.h"
#include "engine/indicators.h" // shared indicator toolkit (docs/dev/indicator-comparison.md §7)

#include "daisysp.h"   // daisysp::SoftLimit

#include <cmath>
#include <cstdint>

namespace spotykach {

namespace {

// Selector quantization with a deadband, lifted from the radio engine's _quant_station: hold the
// committed index until the continuous position crosses its boundary by `hyst`, so pot/CV noise sitting
// on a boundary cannot chatter the target (and so trigger a re-open per main loop - the stutter bug).
int quant_sel(float x, int n, int cur, float hyst) {
    if (n <= 0) return -1;
    if (n == 1) return 0;
    x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    const float pos = x * static_cast<float>(n - 1);
    auto nearest = [&](float p) {
        int s = static_cast<int>(std::lround(p));
        return s < 0 ? 0 : (s >= n ? n - 1 : s);
    };
    if (cur < 0 || cur >= n) return nearest(pos);
    if (pos > static_cast<float>(cur) + 0.5f + hyst) return nearest(pos);
    if (pos < static_cast<float>(cur) - 0.5f - hyst) return nearest(pos);
    return cur;
}

inline int idx(DeckRef::Ref d) { return (d == DeckRef::A) ? 0 : 1; }
inline DeckRef::Ref ref_of(int i) { return (i == 0) ? DeckRef::A : DeckRef::B; }

} // namespace

void BardEngine::init(const EngineContext& ctx) {
    _stream    = ctx.stream;
    _time      = ctx.time;
    _transport = ctx.transport;

    for (int i = 0; i < 2; i++) {
        _rescan[i] = true;
        _open_book_i[i] = -1;
        _marks[i].clear();
        _hp[i].Init(ctx.sample_rate);
        _lp[i].Init(ctx.sample_rate);
        _hp[i].SetParams(20.f, 0.707f);
        _lp[i].SetParams(ctx.sample_rate * 0.45f, 0.707f);
        _wsola[i].init();
        _update_rate_chain(i);
    }
    _text[0] = '\0';

    // Sub-allocate the two rooms from the injected SDRAM arena. A null/exhausted arena leaves them
    // inactive (process() passes audio through), which is what the host test runs with.
    {
        Arena arena(ctx.arena);
        const size_t need = bard::Room::capacity_floats(ctx.sample_rate);
        for (int i = 0; i < 2; i++) _room[i].init(arena.alloc<float>(need), ctx.sample_rate);
    }

    // Armed segment advance rides the transport's KEY (bar) boundary. The callback runs in the audio-block
    // context, so it only ever sets a flag; prepare() does the seek.
    if (_transport) {
        _transport->set_on_tick([this](const TransportTick& t) {
            if (!t.key) return;
            for (int i = 0; i < 2; i++) if (_armed[i]) _tick_advance[i] = true;
        });
    }
}

// ---- main loop -------------------------------------------------------------------------------------

void BardEngine::prepare() {
    if (!_stream) return;
    const uint32_t now = _time ? _time->now_ms() : 0;

    _load_config();
    _load_resume();

    for (int i = 0; i < 2; i++) {
        const DeckRef::Ref d = ref_of(i);

        // Boot retry: the card mounts ~1 s after power-up, so keep rescanning an empty shelf for a few
        // seconds rather than showing an empty shelf until the user touches a knob.
        if (_nbooks[i] == 0 && now < kBootScanMs) _rescan[i] = true;

        if (_rescan[i]) {
            _rescan[i] = false;
            _rescan_shelf(i);
            _open_book_i[i]  = -1;                 // force a (re)open in the new shelf
            _pending_book[i] = -1;
        }
        // A successful scan proves the card is mounted, so a still-missing bard.cfg / resume.txt is simply
        // absent rather than not-yet-mounted: stop retrying and take the defaults.
        if (_nbooks[i] > 0) { _cfg_loaded = true; _resume_loaded = true; }

        _apply_selectors(d, now);

        // A pending explicit jump (pads / gate / scrub / selector) - one seek per main loop, at most.
        if (_req[i]) {
            _req[i] = false;
            if (_open_book_i[i] >= 0) {
                _seek(d, _req_frame[i]);
                _paused[i] = false;
            }
        }

        // Armed advance requested by the transport tick.
        if (_tick_advance[i]) {
            _tick_advance[i] = false;
            if (_open_book_i[i] >= 0) _advance(d, 1);
        }

        // Segment / book end. The audio path already stopped at _seg_end, so this is never late enough to
        // bleed into the next chapter; a deck whose stream simply ran out (loop=false at EOF) lands here too.
        if (_open_book_i[i] >= 0 && !_paused[i]) {
            const bool at_end = _pos[i] >= _seg_end[i] || !_stream->is_playing(d);
            if (at_end) {
                switch (_seq[i]) {
                    case Seq::Read:
                        if (_marks[i].loop == bard::LoopMode::Book) { _seek(d, 0); _enter_segment(i, bard::mark_at(_marks[i], 0)); }
                        else                                        { _paused[i] = true; }
                        break;
                    case Seq::Recite:
                        if (_loop_seg[i] && _seg[i] >= 0) { _seek(d, _marks[i].mark[_seg[i]].start); _gate_out[i] = true; }
                        else                              { _paused[i] = true; }
                        break;
                    case Seq::Wander:
                        if (_armed[i]) _paused[i] = true;   // hold silently until the next key boundary
                        else           _advance(d, 1);
                        break;
                }
            }
        }

        // Gate out / mark tracking: latch a pulse whenever the playhead enters a different bookmark,
        // whether by a jump or by playing across the boundary in Read.
        if (_open_book_i[i] >= 0) {
            const int m = bard::mark_at(_marks[i], _pos[i]);
            if (m != _seen_mark[i]) {
                if (m >= 0 && _seen_mark[i] >= 0) _gate_out[i] = true;
                _seen_mark[i] = m;
            }
        }
    }

    _save_resume(now);
}

// Read /bard/bard.cfg once (resume=on|off, rate=<hz>). Absent file -> the struct defaults.
void BardEngine::_load_config() {
    if (_cfg_loaded) return;
    const int n = _stream->read_text("bard/bard.cfg", _text, kTextMax);
    if (n > 0) { bard::parse_config(_text, _cfg); _cfg_loaded = true; }
}

// Read /bard/resume.txt once into the LRU table. Absent file -> an empty table (every book starts at 0).
void BardEngine::_load_resume() {
    if (_resume_loaded) return;
    const int n = _stream->read_text("bard/resume.txt", _text, kTextMax);
    if (n > 0) { _resume.parse(_text); _resume_loaded = true; }
}

// Persist the resume table. Written on a ~30 s checkpoint while playing and whenever a book change or a
// pause marked it dirty. This is the engine's ONLY write to the card; `resume=off` removes it entirely,
// and the first failure (no card / write-protected / full) disables it for the session rather than
// retrying every loop. A torn file is harmless - ResumeTable::parse discards unparseable lines.
void BardEngine::_save_resume(uint32_t now) {
    if (!_cfg.resume || !_resume_writable) return;

    // Both decks can hold the SAME book, and then "where was I" is genuinely ambiguous - one key, two
    // playheads. Rather than let the two fight (last writer wins, so the stored position would depend on
    // loop order), deck A wins deterministically and deck B is skipped for that book.
    char first_key[bard::ResumeTable::kKeyMax] = { 0 };
    for (int i = 0; i < 2; i++) {
        if (_open_book_i[i] < 0) continue;
        const char* key = _resume_key(i, _open_book_i[i]);
        if (i == 1 && first_key[0]) {
            bool same = true;
            for (int k = 0; k < bard::ResumeTable::kKeyMax; k++) {
                if (first_key[k] != key[k]) { same = false; break; }
                if (!key[k]) break;
            }
            if (same) continue;
        }
        if (i == 0) for (int k = 0; k < bard::ResumeTable::kKeyMax - 1 && key[k]; k++) first_key[k] = key[k];
        if (!_paused[i] || _resume_dirty) _resume.set(key, _pos[i]);
    }
    const bool due = _resume_dirty || (now - _resume_ms) >= kCheckpointMs;
    if (!due) return;
    _resume_ms    = now;
    _resume_dirty = false;
    if (_resume.count == 0) return;

    const int n = _resume.serialize(_text, kTextMax);
    if (n <= 0) return;
    if (!_stream->write_text("bard/resume.txt", _text, n)) _resume_writable = false;
}

void BardEngine::_rescan_shelf(int i) {
    _nbooks[i] = _stream->scan_bank(_shelf_dir(i), _books[i], kMaxBooks);
}

// Quantize the BOOK and BOOKMARK selectors (with radio's hysteresis + settle guards) and apply the
// debounced Alt+POS scrub. Everything here can trigger at most one seek per main loop.
void BardEngine::_apply_selectors(DeckRef::Ref d, uint32_t now) {
    const int i = idx(d);

    // --- BOOK (PITCH + V/oct CV) ---
    const int want_book = quant_sel(_book_n[i] + _book_cv[i], _nbooks[i], _open_book_i[i], kSelHyst);
    if (want_book != _pending_book[i]) { _pending_book[i] = want_book; _pending_book_ms[i] = now; }
    // Nothing open yet -> open immediately: the settle timer exists to stop a re-open storm on a LIVE
    // deck, and there is no stutter to avoid before the first book is playing.
    const bool book_settled = !_time || _open_book_i[i] < 0 || (now - _pending_book_ms[i]) >= kSettleMs;
    if (want_book != _open_book_i[i] && book_settled) {
        _resume_dirty = true;                       // remember where we were leaving
        _save_resume(now);
        _open_book(d, want_book, now, /*use_resume=*/true);
        return;                                     // a fresh book: let the next loop settle its bookmark
    }

    if (_open_book_i[i] < 0) return;

    if (_rescan_marks[i]) { _rescan_marks[i] = false; _load_marks(i); }

    // Commit marks to the sidecar (tap-hold Play). Shares _text with the sidecar read and the resume
    // write - all main-loop, never concurrent. A failure only costs the confirmation flash.
    if (_commit_marks[i]) {
        _commit_marks[i] = false;
        const int len = bard::serialize_marks(_marks[i], _src_rate[i], _book_frames[i], _text, kTextMax);
        if (len > 0 && _stream->write_text(_sidecar_path(i, _open_book_i[i]), _text, len)) {
            _marks[i].generated = false;                 // the book has a real sidecar now
            _commit_flash[i] = now + kErrFlashMs;
        } else {
            _err_until[i] = now + kErrFlashMs;
        }
    }

    // --- BOOKMARK (POS + size/pos CV). The knob walks mark[] in the SIDECAR'S LINE ORDER; the Seq pad
    // and the gate walk the play order (which differs when order=time / order=shuffle).
    //
    // It acts on MOVEMENT, not on disagreement (see _mark_x in the header): a parked knob must not undo a
    // Seq-pad / gate / clock advance, and must not override the resumed position when a book opens. ---
    const int   n  = _marks[i].count;
    const float mx = _mark_n[i] + _mark_cv[i];
    if (_mark_x[i] < -0.5f || mx - _mark_x[i] > 0.01f || _mark_x[i] - mx > 0.01f) {
        _mark_moved[i] = true;
        _mark_x[i]     = mx;
    }
    if (_mark_moved[i]) {
        const int want_seg = quant_sel(mx, n, _seg[i], kSelHyst);
        if (want_seg != _pending_seg[i]) { _pending_seg[i] = want_seg; _pending_seg_ms[i] = now; }
        const bool seg_settled = !_time || _seg[i] < 0 || (now - _pending_seg_ms[i]) >= kSettleMs;
        if (seg_settled) {
            _mark_moved[i] = false;                  // the sweep has landed, whether or not it moved us
            if (want_seg >= 0 && want_seg != _seg[i]) {
                _enter_segment(i, want_seg);
                _request_jump(i, _marks[i].mark[want_seg].start);
                return;
            }
        }
    }

    // --- SCRUB (Alt+POS), debounced the same way: a sweep only opens the spot you land on. In Read the
    // "segment" is the whole book, so Alt+POS scrubs the entire recording. ---
    if (_scrub_touched[i] && (!_time || (now - _scrub_ms[i]) >= kSettleMs)) {
        _scrub_touched[i] = false;
        const uint32_t s = (_seg[i] >= 0 && _seq[i] != Seq::Read) ? _marks[i].mark[_seg[i]].start : 0u;
        const uint32_t e = (_seg[i] >= 0 && _seq[i] != Seq::Read) ? _marks[i].mark[_seg[i]].end
                                                                  : _book_frames[i];
        if (e > s) {
            const float x = _scrub_n[i] < 0.f ? 0.f : (_scrub_n[i] > 1.f ? 1.f : _scrub_n[i]);
            _request_jump(i, s + static_cast<uint32_t>(x * static_cast<float>(e - s - 1)));
        }
    }
}

void BardEngine::_open_book(DeckRef::Ref d, int book, uint32_t now, bool use_resume) {
    const int i = idx(d);
    _stream->stop(d);
    _primed[i] = false;
    _seen_mark[i] = -1;

    if (book < 0 || book >= _nbooks[i]) {
        _open_book_i[i] = -1; _book_frames[i] = 0; _seg[i] = -1; _seg_end[i] = 0;
        _marks[i].clear();
        return;
    }

    const BankEntry& b = _books[i][book];
    _open_book_i[i]  = book;                 // commit first: path/sidecar/resume helpers read it
    _book_frames[i]  = b.frames;
    _src_rate[i]     = (b.is_wav && b.rate > 0) ? b.rate : _cfg.rate;
    _rate_ratio[i]   = static_cast<float>(_src_rate[i]) / 48000.f;

    _load_marks(i);

    uint32_t start = 0;
    if (use_resume) {
        uint32_t f = 0;
        if (_resume.get(_resume_key(i, book), f) && f + 1 < b.frames) start = f;
    }
    _enter_segment(i, bard::mark_at(_marks[i], start));
    // Seed the bookmark selector at the knob's CURRENT position so a fresh book honours its resumed
    // position instead of being immediately snapped to wherever POS happens to be parked.
    _mark_x[i]     = _mark_n[i] + _mark_cv[i];
    _mark_moved[i] = false;
    if (!_seek(d, start)) { _err_until[i] = now + kErrFlashMs; _open_book_i[i] = -1; }
    _paused[i] = false;
}

// Read the sidecar for the open book, or generate deterministic auto-marks if there is none. An explicit
// directive line survives a mark-less sidecar (you can set order=/loop= and still take the auto-marks).
void BardEngine::_load_marks(int i) {
    const int b = _open_book_i[i];
    if (b < 0) { _marks[i].clear(); return; }
    const char* name = _books[i][b].name;

    const int n = _stream->read_text(_sidecar_path(i, b), _text, kTextMax);
    bool have = false;
    bard::MarkOrder keep_order = bard::MarkOrder::File;
    bard::LoopMode  keep_loop  = bard::LoopMode::Off;
    bool            keep_set   = false;
    if (n > 0) {
        bard::parse_sidecar(_text, _src_rate[i], _book_frames[i], _marks[i]);
        keep_order = _marks[i].ordering; keep_loop = _marks[i].loop; keep_set = _marks[i].loop_set;
        have = _marks[i].count > 0;
    }
    if (!have) {
        bard::auto_marks(name, _book_frames[i], _src_rate[i], _reroll[i], _marks[i]);
        _marks[i].ordering = keep_order; _marks[i].loop = keep_loop; _marks[i].loop_set = keep_set;
    }
    bard::resolve(_marks[i], _book_frames[i], bard::book_seed(name, _book_frames[i], _reroll[i]));

    // Segment-end policy: the sidecar's loop= if it said anything, else HOLD. A silent stop is recoverable
    // with one knob turn; an unexpected loop of a spoken passage is the worse surprise.
    _loop_seg[i] = _marks[i].loop_set ? (_marks[i].loop == bard::LoopMode::Segment) : false;
}

// Commit a segment: remember its index and where the audio path must stop. In Read the segment is the
// whole book (marks are jump targets only), so nothing bounds playback but the end of the file.
void BardEngine::_enter_segment(int i, int mark_idx) {
    _seg[i] = mark_idx;
    if (_seq[i] == Seq::Read || mark_idx < 0 || mark_idx >= _marks[i].count) {
        _seg_end[i] = _book_frames[i] ? _book_frames[i] : 0xffffffffu;
    } else {
        _seg_end[i] = _marks[i].mark[mark_idx].end;
    }
}

bool BardEngine::_seek(DeckRef::Ref d, uint32_t frame) {
    const int i = idx(d);
    const int b = _open_book_i[i];
    if (b < 0) return false;
    if (_book_frames[i] && frame + 1 >= _book_frames[i]) frame = _book_frames[i] - 1;

    // Prefer the LIGHT path: if this deck is already streaming this book, an f_lseek on the live handle
    // plus a ring flush is much cheaper than close + f_open + (for a .wav) header re-parse. Every jump
    // except the first open of a book takes it, which is what makes rapid bookmark stepping affordable.
    // seek_play returns false on a deck that is not playing, so the initial open falls through naturally
    // (and _open_book always stops the deck first, so a live deck is never on a different book here).
    if (_stream->seek_play(d, frame)) {
        _pos[i]    = frame;
        _primed[i] = false;
        _wsola[i].reset();                                   // drop audio buffered from the old position
        _seam_gain[i] = (_seam_n[i] > 0.001f) ? 0.f : 1.f;
        return true;
    }

    _stream->stop(d);
    const BankEntry& e = _books[i][b];
    const char* path = _book_path(i, b);
    const bool ok = e.is_wav ? _stream->start_play_wav(d, path, frame, /*loop=*/false)
                             : _stream->start_play_raw(d, path, frame, /*loop=*/false);
    if (ok) {
        _pos[i]    = frame;
        _primed[i] = false;
        _wsola[i].reset();
        _seam_gain[i] = (_seam_n[i] > 0.001f) ? 0.f : 1.f;   // SEAM: fade the new stream in (declick)
    }
    return ok;
}

// Step `steps` entries through the PLAY ORDER from wherever the playhead currently is. Running off the end
// wraps when the policy is loop, else holds.
void BardEngine::_advance(DeckRef::Ref d, int steps) {
    const int i = idx(d);
    const int n = _marks[i].count;
    if (n <= 0) return;

    int slot = bard::order_slot(_marks[i], _seg[i]);
    if (slot < 0) slot = 0;
    else          slot += steps;

    if (slot >= n) {
        if (!_loop_seg[i]) { _paused[i] = true; return; }
        slot = 0;
    }
    if (slot < 0) slot = n - 1;

    const int m = static_cast<int>(_marks[i].order[slot]);
    _enter_segment(i, m);
    _request_jump(i, _marks[i].mark[m].start);
    _gate_out[i] = true;
}

void BardEngine::_request_jump(int i, uint32_t frame) {
    _req_frame[i] = frame;
    _req[i]       = true;
}

// ---- audio -----------------------------------------------------------------------------------------

void BardEngine::process(const float* const* /*in*/, float** out, size_t size) {
    const size_t n = size > kMaxFrames ? kMaxFrames : size;
    if (!_stream) { for (int c = 0; c < 2; c++) for (size_t i = 0; i < n; i++) out[c][i] = 0.f; return; }

    float monoA[kMaxFrames], monoB[kMaxFrames];
    _render_deck(DeckRef::A, monoA, n);
    _render_deck(DeckRef::B, monoB, n);

    // Duck (Mod Type = Follow): each deck's block envelope attenuates the OTHER deck, so a narrator opens
    // gaps in an atmosphere bed. One block of latency (~2 ms) is inaudible and keeps this per-block cheap.
    for (int i = 0; i < 2; i++) {
        const float* src = (i == 0) ? monoA : monoB;
        float pk = 0.f;
        for (size_t s = 0; s < n; s++) { const float a = std::fabs(src[s]); if (a > pk) pk = a; }
        // Fast attack, release from the Cycle knob (~30 ms .. ~1.5 s over the block rate).
        const float rel = 0.02f + 0.28f * (1.f - _duck_rel_n[i]);
        const float k   = (pk > _env_follow[i]) ? 0.5f : rel;
        _env_follow[i] += k * (pk - _env_follow[i]);
    }
    for (int i = 0; i < 2; i++) {
        const int other = 1 - i;
        float g = 1.f;
        if (_follow[other] && _duck_n[other] > 0.001f) {
            float e = _env_follow[other] * kDuckKnee;
            if (e > 1.f) e = 1.f;
            g = 1.f - _duck_n[other] * e;
        }
        _duck_gain[i] = g;
    }

    float pLa, pRa, pLb, pRb;
    switch (_route) {
        case Route::DoubleMono:                       // LEFT: a story per ear
            pLa = 1.f; pRa = 0.f; pLb = 0.f; pRb = 1.f; break;
        case Route::GenerativeStereo:                 // RIGHT: random pan per deck
            pLa = _rndL[0]; pRa = _rndR[0]; pLb = _rndL[1]; pRb = _rndR[1]; break;
        case Route::Stereo: default:                  // CENTRE: both centred
            pLa = pRa = pLb = pRb = kCenterGain; break;
    }
    const float ga = _gain_n[0] * _gA * _duck_gain[0];
    const float gb = _gain_n[1] * _gB * _duck_gain[1];
    const float La = ga * pLa, Ra = ga * pRa, Lb = gb * pLb, Rb = gb * pRb;
    for (size_t i = 0; i < n; i++) {
        out[0][i] = daisysp::SoftLimit(monoA[i] * La + monoB[i] * Lb);
        out[1][i] = daisysp::SoftLimit(monoA[i] * Ra + monoB[i] * Rb);
    }
}

void BardEngine::_render_deck(DeckRef::Ref d, float* mono, size_t n) {
    const int i = idx(d);
    const bool live = _stream->is_playing(d) && !_paused[i] && _open_book_i[i] >= 0
                      && _pos[i] < _seg_end[i];
    if (!live) {
        _primed[i] = false;
        for (size_t s = 0; s < n; s++) mono[s] = 0.f;
        return;
    }
    if (!_primed[i]) {
        float a = 0.f, b = 0.f;
        _pull(d, a); _pull(d, b);
        _cur[i] = a; _next[i] = b; _phase[i] = 0.f; _primed[i] = true;
    }

    // Source frames advanced per resampled frame = the rate chain's k x the book's own source-rate rebase
    // (a .wav's header rate, or bard.cfg's rate= for a headerless .raw), so it plays at correct pitch.
    const float step = _res_k[i] * _rate_ratio[i];

    // Stage 1 - feed the time-scaler exactly the resampled frames it needs for this block, stopping dead if
    // the segment boundary falls inside them. At PITCH-KEEP 0 the scaler is a bit-exact passthrough, so this
    // is the same signal path the varispeed-only build had.
    uint32_t need = _wsola[i].want(static_cast<uint32_t>(n));
    while (need--) {
        const float src = _cur[i] + (_next[i] - _cur[i]) * _phase[i];
        _wsola[i].feed1(src);
        _phase[i] += step;
        while (_phase[i] >= 1.f) {
            _phase[i] -= 1.f;
            _cur[i] = _next[i];
            float nx = 0.f;
            _pull(d, nx);
            _next[i] = nx;
        }
        if (_pos[i] >= _seg_end[i]) break;            // boundary reached; prepare() will seek
    }

    // Stage 2 - drain, then colour / room / seam on the FINAL voice. A short drain (starved scaler or a
    // boundary hit) zero-fills the rest of the block.
    float voice[kMaxFrames];
    const uint32_t got = _wsola[i].drain(voice, static_cast<uint32_t>(n));
    const bool fx   = _flux_held[i] || _flux_lock[i];
    const bool room = (_grit_held[i] || _grit_lock[i]) && _grit_mix[i] > 0.001f;
    for (size_t s = 0; s < n; s++) {
        if (s >= got) { mono[s] = 0.f; continue; }
        float x = voice[s];
        if (fx)   x = _colour(i, x);
        if (room) x += (_room[i].process(x) - x) * _grit_mix[i];
        if (_seam_gain[i] < 1.f) { x *= _seam_gain[i]; _seam_gain[i] += _seam_inc[i];
                                   if (_seam_gain[i] > 1.f) _seam_gain[i] = 1.f; }
        mono[s] = x;
    }
}

// RATE (SIZE) and PITCH-KEEP (ENV) -> the resampler factor and the WSOLA time-scale. Recomputed here, on
// the main loop, so the audio ISR never calls powf.
void BardEngine::_update_rate_chain(int i) {
    const float r = _rate[i];
    const float p = _env_n[i] < 0.f ? 0.f : (_env_n[i] > 1.f ? 1.f : _env_n[i]);
    if (p < 0.001f) {                       // pure varispeed: resample by the whole rate, scaler bypassed
        _res_k[i]   = r;
        _wsola_a[i] = 1.f;
    } else {
        _res_k[i]   = std::pow(r, 1.f - p);
        _wsola_a[i] = std::pow(r, -p);
    }
    _wsola[i].set_scale(_wsola_a[i]);
}

// One source frame from the ring. On an underrun the playhead does NOT advance, so the tracked position
// stays honest (a starved ring would otherwise run _pos ahead of what was actually heard).
bool BardEngine::_pull(DeckRef::Ref d, float& out) {
    const int i = idx(d);
    int16_t s = 0;
    const uint32_t got = _stream->play_consume(d, reinterpret_cast<uint8_t*>(&s), sizeof(int16_t));
    if (got < sizeof(int16_t)) { out = 0.f; return false; }
    _pos[i] = _pos[i] + 1;
    out = static_cast<float>(s) * (1.f / 32768.f);
    return true;
}

// Flux VOICE COLOUR: drive into a band-limiting HPF/LPF pair, blended back by FluxMix. The band moves
// with FluxIntensity from clean, through 1930s-wireless, to telephone - the cheapest character-per-cycle
// there is for speech, and the cutoffs only recompute when a knob moves (main loop), never per sample.
float BardEngine::_colour(int i, float x) {
    const float driven = daisysp::SoftLimit(x * _colour_drive[i]);
    const float band   = _lp[i].Process(_hp[i].Process(driven));
    return x + (band - x) * _flux_mix[i];
}

// ---- params / config -------------------------------------------------------------------------------

void BardEngine::set_param(ParamId id, DeckRef::Ref d, float v) {
    const int i = idx(d);
    switch (id) {
        case ParamId::Speed: _book_n[i] = v; break;
        case ParamId::Pos:   _mark_n[i] = v; break;
        case ParamId::Size:
            _size_n[i] = v;
            // Unity at centre, 0.5x at 0 and 2.5x at 1 (exponential each side; the two halves meet at 1.0
            // in value, with a slope change at centre - the range speech wants is not log-symmetric).
            _rate[i] = (v <= 0.5f) ? std::exp2f((v - 0.5f) * 2.f)
                                   : std::exp2f((v - 0.5f) * 2.f * 1.32192809f);
            _update_rate_chain(i);
            break;
        case ParamId::Env:   _env_n[i] = v; _update_rate_chain(i); break;   // PITCH-KEEP
        case ParamId::Mix:   _gain_n[i] = v + _mix_cv[i]; break;
        case ParamId::Aux: {
            int s = static_cast<int>(v * kMaxShelves);
            s = s < 0 ? 0 : (s >= kMaxShelves ? kMaxShelves - 1 : s);
            _shelf_n[i] = v;
            if (s != _shelf[i]) { _shelf[i] = s; _rescan[i] = true; }
            break;
        }
        case ParamId::AltPos: {
            const float dv = v - _scrub_n[i];
            if (dv > 0.01f || dv < -0.01f) {            // ignore sub-1% nudges
                _scrub_n[i] = v;
                _scrub_touched[i] = true;
                _scrub_ms[i] = _time ? _time->now_ms() : 0;
            }
            break;
        }
        case ParamId::Feedback:                          // Alt+SOS: SEAM (jump fade-in), 0..kSeamMaxMs
            _seam_n[i] = v;
            _seam_inc[i] = (v > 0.001f) ? 1.f / (v * kSeamMaxMs * 48.f) : 1.f;
            break;
        case ParamId::ModAmp: _duck_n[i] = v; break;
        case ParamId::FluxIntensity: {
            _flux_int[i] = v;
            // clean (20 Hz .. 20 kHz) -> wireless (300 Hz .. 3.5 kHz) -> telephone (300 Hz .. 3 kHz, driven)
            const float hp = 20.f + v * 280.f;
            const float lp = 20000.f - v * 17000.f;
            _hp[i].SetParams(hp, 0.707f);
            _lp[i].SetParams(lp, 0.707f);
            _colour_drive[i] = 1.f + v * 7.f;
            break;
        }
        case ParamId::FluxMix: _flux_mix[i] = v; break;
        case ParamId::GritIntensity: _grit_int[i] = v; _room[i].set_size(v); break;
        case ParamId::GritMix:       _grit_mix[i] = v; break;
        case ParamId::Crossfade:
            _xfade = v;
            {
                float x = v + _xfade_cv;
                x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
                _gA = x <= 0.5f ? 1.f : 2.f * (1.f - x);
                _gB = x >= 0.5f ? 1.f : 2.f * x;
            }
            break;
        default: break;
    }
}

float BardEngine::param(ParamId id, DeckRef::Ref d) const {
    const int i = idx(d);
    // Crossfade is global (deck-A slot). set_param stores it and derives the A/B blend gains, but
    // param() had no case, so `get param crossfade` reported 0 for a value the engine really holds.
    // Found by the on-target sweep 2026-07-31 (six engines shared this omission); the UI only ever
    // writes this param, so nothing else noticed.
    if (id == ParamId::Crossfade) return _xfade;
    switch (id) {
        case ParamId::Speed:  return _book_n[i];
        case ParamId::Pos:    return _mark_n[i];
        case ParamId::Size:   return _size_n[i];
        case ParamId::Env:    return _env_n[i];
        case ParamId::Mix:    return _gain_n[i];
        case ParamId::AltPos: return _scrub_n[i];
        case ParamId::Feedback: return _seam_n[i];
        case ParamId::ModAmp: return _duck_n[i];
        case ParamId::FluxIntensity: return _flux_int[i];
        case ParamId::FluxMix: return _flux_mix[i];
        case ParamId::GritIntensity: return _grit_int[i];
        case ParamId::GritMix: return _grit_mix[i];
        case ParamId::Aux:
            return (static_cast<float>(_shelf[i]) + 0.5f) / static_cast<float>(kMaxShelves);
        default: return 0.f;
    }
}

void BardEngine::set_mod_speed(DeckRef::Ref d, float value, bool /*sync*/) {
    _duck_rel_n[idx(d)] = value;
}

void BardEngine::set_aux_active(DeckRef::Ref d, bool held) { _aux_held[idx(d)] = held; }

bool BardEngine::set_config(ConfigId id, DeckRef::Ref d, int value) {
    const int i = idx(d);
    if (id == ConfigId::Route) {
        const Route r = (value == 2) ? Route::GenerativeStereo
                      : (value == 1) ? Route::DoubleMono
                                     : Route::Stereo;
        if (r != _route) {
            _route = r;
            if (_route == Route::GenerativeStereo) {
                for (int k = 0; k < 2; k++) {
                    _rng = _rng * 1664525u + 1013904223u;
                    const float p = static_cast<float>(_rng >> 8) * (1.f / 16777216.f);
                    _rndL[k] = std::cos(p * kHalfPi);
                    _rndR[k] = std::sin(p * kHalfPi);
                }
            }
        }
    } else if (id == ConfigId::Mode) {
        // Panel silkscreen Reel / Slice / Drift; the platform's int convention is 0=Slice, 1=Reel, 2=Drift.
        const Seq s = (value == 2) ? Seq::Wander : (value == 1) ? Seq::Read : Seq::Recite;
        if (s != _seq[i]) {
            _seq[i] = s;
            _enter_segment(i, _seg[i]);   // Read unbounds the segment; Recite/Wander re-bound it
        }
    } else if (id == ConfigId::ModType) {
        _follow[i] = (value == 1);        // 0 = LFO (no internal target yet), 1 = Follow (duck the other deck)
    }
    return false;
}

void BardEngine::cv_voct(DeckRef::Ref d, float value)     { _book_cv[idx(d)] = value; }
void BardEngine::cv_size_pos(DeckRef::Ref d, float value) { _mark_cv[idx(d)] = value; }
void BardEngine::cv_mix(DeckRef::Ref d, float value)      { _mix_cv[idx(d)] = value; }
void BardEngine::cv_crossfade(float value) {
    _xfade_cv = value;
    float x = _xfade + value;
    x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    _gA = x <= 0.5f ? 1.f : 2.f * (1.f - x);
    _gB = x >= 0.5f ? 1.f : 2.f * x;
}

// Flux/Grit are momentary while held (the platform calls set_fx on press and release) and latchable with
// Alt+pad (toggle_fx_lock) - the granular engine's idiom, so the pads feel the same across engines.
void BardEngine::set_fx(DeckRef::Ref d, FxKind k, bool on) {
    const int i = idx(d);
    if (k == FxKind::Flux) _flux_held[i] = on;
    else                   _grit_held[i] = on;
}

void BardEngine::toggle_fx_lock(DeckRef::Ref d, FxKind k) {
    const int i = idx(d);
    if (k == FxKind::Flux) _flux_lock[i] = !_flux_lock[i];
    else                   _grit_lock[i] = !_grit_lock[i];
}

// Cycle the room character. The platform reseeds its knob pickup from the returned intensity/mix, which is
// exactly why this hook returns them.
GritReseed BardEngine::toggle_grit_mode(DeckRef::Ref d) {
    const int i = idx(d);
    using C = bard::Room::Character;
    const C c = _room[i].character();
    _room[i].set_character(c == C::Plate ? C::Hall : (c == C::Hall ? C::Slap : C::Plate));
    _room[i].set_size(_grit_int[i]);
    return { _grit_int[i], _grit_mix[i] };
}

// ---- pads / gates ----------------------------------------------------------------------------------

// Play pad = PLAY/PAUSE (the radio inversion: a book must be pausable). Rev pad = JUMP BACK 15 s, and a
// retrigger steps back another 15 s. Both debounced - a capacitive pad can glitch a press.
bool BardEngine::on_play_pad(DeckRef::Ref d, bool reverse) {
    const int i = idx(d);
    const uint32_t now = _time ? _time->now_ms() : 0;
    if (_time && _pad_seen[i] && now - _last_pad_ms[i] < kDebounceMs) return false;
    _last_pad_ms[i] = now; _pad_seen[i] = true;

    if (!reverse) {
        _paused[i] = !_paused[i];
        _resume_dirty = true;                     // checkpoint the position on a pause
    } else if (_open_book_i[i] >= 0) {
        const uint32_t back = kJumpBackSec * _src_rate[i];
        const uint32_t base = (_seq[i] != Seq::Read && _seg[i] >= 0) ? _marks[i].mark[_seg[i]].start : 0u;
        const uint32_t p    = _pos[i];
        uint32_t target = (p > back) ? (p - back) : 0u;
        if (target < base) target = base;         // never step out of the segment you are reciting
        _request_jump(i, target);
    }
    return false;
}

// Alt+Play = DROP MARK at the current position (session-only; committing it to the sidecar is deferred).
// Alt+Rev  = RE-ROLL the auto-marks (only meaningful for a book with no sidecar).
void BardEngine::on_record_pad(DeckRef::Ref d, bool reverse) {
    const int i = idx(d);
    if (_open_book_i[i] < 0) return;

    if (!reverse) {
        if (_marks[i].full()) return;
        const uint32_t p = _pos[i];
        _marks[i].mark[_marks[i].count].start = p;
        _marks[i].mark[_marks[i].count].end   = 0;
        _marks[i].count++;
        bard::resolve(_marks[i], _book_frames[i],
                      bard::book_seed(_books[i][_open_book_i[i]].name, _book_frames[i], _reroll[i]));
        _enter_segment(i, bard::mark_at(_marks[i], p));
    } else {
        _reroll[i]++;
        _rescan_marks[i] = true;                  // prepare() re-reads / re-rolls off the audio path
    }
}

// Tap-hold + Play: COMMIT the current mark list (including anything dropped with Alt+Play) to the book's
// sidecar, so a performance's worth of marks survives a power cycle and can be edited on a computer. Only
// sets a flag - the write itself is FatFs and belongs in prepare().
void BardEngine::stop_if_generating(DeckRef::Ref d) {
    const int i = idx(d);
    if (_open_book_i[i] >= 0 && _marks[i].count > 0) _commit_marks[i] = true;
}

void BardEngine::on_seq_trigger(DeckRef::Ref d)    { if (_open_book_i[idx(d)] >= 0) _advance(d, 1); }
void BardEngine::on_seq_toggle_arm(DeckRef::Ref d) { const int i = idx(d); _armed[i] = !_armed[i]; }

// Alt+Seq held: flip the segment-end policy between LOOP and HOLD. Turning looping on while the deck is
// already parked at the end of a held segment has to start it moving again here - the segment-end handler
// in prepare() only runs on a non-paused deck, so otherwise the toggle would do nothing until the next
// Play press, which reads as a dead control.
void BardEngine::clear_sequence(DeckRef::Ref d) {
    const int i = idx(d);
    _loop_seg[i] = !_loop_seg[i];
    if (!_loop_seg[i] || _open_book_i[i] < 0 || _pos[i] < _seg_end[i]) return;
    if (_seq[i] == Seq::Wander)   _advance(d, 1);                              // wrap to the next entry
    else if (_seg[i] >= 0)        _request_jump(i, _marks[i].mark[_seg[i]].start);
}

void BardEngine::on_gate_trigger(DeckRef::Ref d) {
    const int i = idx(d);
    const uint32_t now = _time ? _time->now_ms() : 0;
    if (_time && _gate_seen[i] && now - _last_gate_ms[i] < kDebounceMs) return;  // a floating jack chatters
    _last_gate_ms[i] = now; _gate_seen[i] = true;
    if (_open_book_i[i] >= 0) _advance(d, 1);
}

bool BardEngine::gate_out_triggered(DeckRef::Ref d) {
    const int i = idx(d);
    const bool t = _gate_out[i];
    _gate_out[i] = false;
    return t;
}

// The speech envelope as a 0..1 CV - the narrator's dynamics, available to the rest of the rack whether
// or not the internal ducker is engaged.
void BardEngine::process_cv(float* cv0, float* cv1, size_t n) {
    const float a = _env_follow[0] > 1.f ? 1.f : _env_follow[0];
    const float b = _env_follow[1] > 1.f ? 1.f : _env_follow[1];
    for (size_t i = 0; i < n; i++) { cv0[i] = a; cv1[i] = b; }
}

// ---- display ---------------------------------------------------------------------------------------

// The ring is the SPINE OF THE BOOK: a progress arc to the playhead, a dim tick at every bookmark with the
// current segment bright, and the shelf dots while Alt is held.
void BardEngine::render(DisplayModel& m) {
    m.clear();
    const uint32_t now = _time ? _time->now_ms() : 0;

    for (int i = 0; i < 2; i++) {
        const DeckRef::Ref d = ref_of(i);
        const bool open    = _open_book_i[i] >= 0;
        const bool err     = _time && now < _err_until[i];
        const bool playing = open && !_paused[i] && _stream && _stream->is_playing(d);

        const bool committed = _time && now < _commit_flash[i];
        const uint32_t c = committed  ? pal::kWhite     // white: marks written to the sidecar
                         : err        ? kErrColor
                         : !open      ? pal::kBlack
                         : _armed[i]  ? pal::kCyan       // cyan: armed to the clock
                         : playing    ? pal::kGreen      // green: reading
                                      : 0xff8000;        // amber: paused (bard-specific hue)
        m.play[i] = { c, (open || err || committed) ? 1.f : 0.f };
        m.gate_in[i] = { pal::kCyan, _armed[i] ? 0.6f : 0.f };

        if (_aux_held[i]) {
            // Alt-held shelf selector (kMaxShelves dots, active bright) — was a hand-rolled loop.
            ring::selector(m.ring[i], kMaxShelves, _shelf[i], pal::kWhite, 0.15f, 0x202020);
        } else {
            m.ring[i].set_hex_color(0x101010); m.ring[i].set_segment(0.f, 0.999f);
            if (open && _book_frames[i] > 0) {
                const float prog = static_cast<float>(static_cast<double>(_pos[i])
                                                    / static_cast<double>(_book_frames[i]));
                m.ring[i].set_hex_color(playing ? 0x004000 : 0x302000);
                m.ring[i].set_segment(0.f, prog > 0.999f ? 0.999f : (prog < 0.02f ? 0.02f : prog));
                // A tick per bookmark; several marks can land on one LED, so the colliding LED just reads
                // brighter rather than pretending 32 LEDs can resolve 64 marks.
                m.ring[i].set_point_hex_color(err ? kErrColor : 0x00ff40);
                for (int k = 0; k < _marks[i].count; k++) {
                    const float pos = static_cast<float>(static_cast<double>(_marks[i].mark[k].start)
                                                       / static_cast<double>(_book_frames[i]));
                    m.ring[i].add_point(pos, (k == _seg[i]) ? 1.f : 0.2f);
                }
            } else if (err || !open) {
                m.ring[i].set_point_hex_color(kErrColor);
                m.ring[i].add_point(0.f, err ? 1.f : 0.25f);
            }
        }
        m.ring[i].set_updated();

        m.flux[i] = { 0xffc040, (_flux_held[i] || _flux_lock[i]) ? 1.f : 0.f };
        // Room character has its own colour so the Grit pad shows which space is selected.
        {
            using C = bard::Room::Character;
            const C rc = _room[i].character();
            const uint32_t rgb = (rc == C::Plate) ? 0x4080ff : (rc == C::Hall) ? 0x8040ff : 0x40ffc0;
            m.grit[i] = { rgb, (_grit_held[i] || _grit_lock[i]) ? 1.f : 0.f };
        }
        led::cycle(m, i, _follow[i] ? (_duck_n[i] > 0.001f ? 1.f : 0.3f) : 0.f, 0x40a0ff);
    }

    led::route_leds(m, _route);
}

// ---- path helpers ----------------------------------------------------------------------------------

// "bard/<shelf>" - relative, like the tape/radio engines; FatFs reads it directly.
const char* BardEngine::_shelf_dir(int i) {
    char* p = _dbuf;
    for (const char* s = "bard/"; *s; ) *p++ = *s++;
    const int b = _shelf[i];
    if (b >= 10) { *p++ = '1'; *p++ = static_cast<char>('0' + (b - 10)); }
    else         { *p++ = static_cast<char>('0' + b); }
    *p = '\0';
    return _dbuf;
}

const char* BardEngine::_book_path(int i, int book) {
    char* p = _pbuf;
    for (const char* s = _shelf_dir(i); *s; ) *p++ = *s++;
    *p++ = '/';
    for (const char* s = _books[i][book].name; *s; ) *p++ = *s++;
    *p = '\0';
    return _pbuf;
}

// "bard/<shelf>/<NAME>.TXT" - the sidecar sitting next to the audio. The audio scan filters to
// .raw/.wav, so the sidecar is never mistaken for a book.
const char* BardEngine::_sidecar_path(int i, int book) {
    char* p = _pbuf;
    for (const char* s = _shelf_dir(i); *s; ) *p++ = *s++;
    *p++ = '/';
    const char* name = _books[i][book].name;
    const char* dot  = nullptr;
    for (const char* s = name; *s; ++s) if (*s == '.') dot = s;
    for (const char* s = name; s != (dot ? dot : s); ) *p++ = *s++;
    if (!dot) for (const char* s = name; *s; ) *p++ = *s++;   // no extension: append .txt to the whole name
    for (const char* s = ".txt"; *s; ) *p++ = *s++;
    *p = '\0';
    return _pbuf;
}

// "<shelf>/<NAME.WAV>" - the resume table key. Shelf-qualified so the same filename on two shelves keeps
// two positions.
const char* BardEngine::_resume_key(int i, int book) {
    char* p = _kbuf;
    const int b = _shelf[i];
    if (b >= 10) { *p++ = '1'; *p++ = static_cast<char>('0' + (b - 10)); }
    else         { *p++ = static_cast<char>('0' + b); }
    *p++ = '/';
    for (const char* s = _books[i][book].name; *s; ) *p++ = *s++;
    *p = '\0';
    return _kbuf;
}

} // namespace spotykach
