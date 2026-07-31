// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
//
// SKETCH - QSPI-target only. Compiles against libcsound.a (csound.h on the include path) in the
// BOOT_QSPI build; not part of the SRAM engine bundle. See csound_engine.h / docs/dev/csound-impl.md.

#include "engine/csound/csound_engine.h"
#include "engine/csound/csound_patch.h"   // patch_path / scan_patches / aux_to_index / read_orchestra
#include "engine/indicators.h"            // shared indicator toolkit (docs/dev/indicator-comparison.md §7)

#include "config.h"                        // Config::dynamic() midi_channel_a/b for the channel->deck map

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "csound.h"   // provided by the QSPI build's -I.../Daisy/include/csound

namespace spotykach {

// Arms the SDRAM pool for Csound's allocations (csound_alloc.cpp). Called after _hw.Init() (so SDRAM
// is live), before csoundCreate. No-op-safe if the --wrap allocator isn't linked.
void csound_heap_arm() noexcept;

// ---------------------------------------------------------------------------------------------
// Built-in fallback orchestra. The engine prefers an SD patch (/csound/<n>.csd via csound_patch.h)
// and uses this when there is no card/file, the file isn't a CSD, or it fails to compile - so a
// card-less unit always makes sound, and the built-in is always selectable as slot 0 of the bank.
// The control channels it reads with chnget are the engine's param vocabulary - keep them in sync
// with channel_for() below; an SD patch names the same channels to be driven by the same knobs.
//
// Instruments are triggered with schedule() IN the orchestra, NOT a score `i` event (a score event
// did not fire on-target). Oscillators are table-less (vco2) to dodge the ftgen-at-init silence.
// ---------------------------------------------------------------------------------------------
static const char* kOrchestra = R"csd(
<CsoundSynthesizer>
<CsInstruments>
sr     = 48000
0dbfs  = 1
nchnls = 2

instr 1
  kspeed chnget "speedA"
  kmix   chnget "mixA"
  ksize  chnget "sizeA"
  kfreq  = 110 + kspeed * 770
  kcut   = 1500 + ksize * 6000   ; default bright; undriven channels read 0
  kamp   = 0.15 + kmix * 0.85    ; floor so there's always sound (Mix=0 on boot)
  asig   vco2 0.3, kfreq         ; verified table-less oscillator
  asig   tone asig, kcut         ; core one-pole lowpass (Size -> brightness)
  outs   asig * kamp, asig * kamp
endin

; MIDI-playable voice. The engine schedules this by name on a NoteOn (csound_midi.h kMidiInstrName):
;   p4 = frequency (Hz), p5 = deck (0/1). No NoteOff is delivered, so the note self-terminates over its
;   fixed duration p3 - an exponentially-decaying pluck. SIZE still colours the tone (shared cutoff).
instr MidiNote
  ifreq  = p4
  ksize  chnget "sizeA"
  kcut   = 1500 + ksize * 6000
  aenv   expon 1, p3, 0.001       ; finite exp decay over p3 -> self-terminating pluck
  asig   vco2 0.5, ifreq          ; table-less, like instr 1
  asig   tone asig, kcut
  outs   asig * aenv, asig * aenv
endin

schedule(1, 0, 100000)
</CsInstruments>
<CsScore>
</CsScore>
</CsoundSynthesizer>
)csd";

// Map a platform ParamId (+ deck) to an orchestra control-channel name + a cache slot, or nullptr for
// params this engine ignores. The 'A'/'B' suffix lets one orchestra carry both decks. Eight slots per
// deck (kSlots=16). A patch reads whichever of these it wants with chnget; unread channels cost
// nothing. Aux is NOT here - it is the patch selector, handled in set_param directly.
static const char* channel_for(ParamId id, DeckRef::Ref d, int& slot)
{
    const bool A = (d == DeckRef::A);
    const int  base = A ? 0 : 8;
    switch (id) {
        case ParamId::Speed:    slot = base + 0; return A ? "speedA"  : "speedB";
        case ParamId::Mix:      slot = base + 1; return A ? "mixA"    : "mixB";
        case ParamId::Size:     slot = base + 2; return A ? "sizeA"   : "sizeB";
        case ParamId::Env:      slot = base + 3; return A ? "envA"    : "envB";
        case ParamId::Feedback: slot = base + 4; return A ? "fbA"     : "fbB";
        case ParamId::ModSpeed: slot = base + 5; return A ? "modspA"  : "modspB";
        case ParamId::ModAmp:   slot = base + 6; return A ? "modampA" : "modampB";
        default:                slot = -1;       return nullptr;
    }
}

// The (ParamId) set channel_for maps - iterated to reseed a freshly compiled instance's channels.
static const ParamId kMappedParams[] = {
    ParamId::Speed, ParamId::Mix, ParamId::Size, ParamId::Env,
    ParamId::Feedback, ParamId::ModSpeed, ParamId::ModAmp,
};

// Scratch buffer size for an SD-loaded orchestra. A CSD patch is a few KB; 64 KB is generous head-
// room. It is malloc'd from the (now armed) SDRAM pool, used only during compile, then freed.
static constexpr int kPatchMax = 64 * 1024;

// ---------------------------------------------------------------------------------------------
// Instance build / publish
// ---------------------------------------------------------------------------------------------
CSOUND* CsoundEngine::build_instance(const char* text, float block_size)
{
    CSOUND* cs = csoundCreate(nullptr, nullptr);
    if (!cs) return nullptr;

    csoundSetHostAudioIO(cs);                  // host owns I/O; we feed spin / drain spout
    csoundSetOption(cs, "-n");                  // no Csound-managed audio device
    csoundSetOption(cs, "-dm0");                // no displays, no messages

    // Tie Csound's k-cycle to the platform block so process() performs exactly once per block. The
    // QSPI build should use a block of >=128 (256 is proven): at ksmps=32 the per-cycle overhead
    // overruns the CPU.
    char opt[24];
    std::snprintf(opt, sizeof(opt), "--ksmps=%d", static_cast<int>(block_size));
    csoundSetOption(cs, opt);

    if (csoundCompileCSD(cs, text, 1, 0) != 0) {
        csoundDestroy(cs);                     // free the partial instance (the pool reclaims it now)
        return nullptr;
    }
    csoundStart(cs);
    return cs;
}

void CsoundEngine::publish_instance(CSOUND* cs)
{
    // Set the derived state BEFORE publishing: the gate's seq_cst publish/begin_use pair makes these
    // plain writes visible to the ISR together with the new pointer (the ISR only reads _ksmps/
    // _midi_instr after begin_use() returns a non-null instance).
    _ksmps = csoundGetKsmps(cs);
    const int n = csoundGetInstrNumber(cs, kMidiInstrName);
    _midi_instr = (n > 0) ? n : 0;
    _gate.publish(cs);
}

void CsoundEngine::reseed_channels(CSOUND* cs)
{
    // After a live recompile the new instance's channels are at 0; replay the knob positions the
    // platform last sent so the patch picks up the current panel state instead of jumping on the
    // next pot move. (Safe: the instance isn't published yet, so the ISR can't touch it.)
    for (ParamId id : kMappedParams) {
        for (int di = 0; di < 2; di++) {
            const DeckRef::Ref d = di == 0 ? DeckRef::A : DeckRef::B;
            int slot = -1;
            const char* chan = channel_for(id, d, slot);
            if (chan && slot >= 0 && slot < kSlots)
                csoundSetControlChannel(cs, chan, static_cast<MYFLT>(_cache[slot]));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Patch bank
// ---------------------------------------------------------------------------------------------
void CsoundEngine::rescan_bank()
{
    scan_patches(_stream, _present, kMaxPatchSlots);
    int n = 0;
    _avail_slot[n++] = -1;                       // index 0 is always the built-in
    for (int s = 0; s < kMaxPatchSlots; s++)
        if (_present[s] && n < kMaxAvail) _avail_slot[n++] = s;
    _avail_n = n;
    if (_sel >= _avail_n)         _sel = _avail_n - 1;       // keep the selection in range
    if (_sel_preview >= _avail_n) _sel_preview = _avail_n - 1;
}

const char* CsoundEngine::orchestra_for(int avail_index, char* buf, bool* from_sd)
{
    if (avail_index < 0 || avail_index >= _avail_n) avail_index = 0;
    const int slot = _avail_slot[avail_index];
    if (slot < 0) { if (from_sd) *from_sd = false; return kOrchestra; }   // the built-in
    char path[24];
    patch_path(slot, path, sizeof(path));
    return read_orchestra(_stream, path, buf, buf ? kPatchMax : 0, kOrchestra, from_sd);
}

void CsoundEngine::do_reload(int target)
{
    // Build the NEW instance while the OLD one is still live (still playing): this avoids the audio
    // gap AND avoids a csoundDestroy-then-csoundCreate sequence (which may not re-init cleanly on bare
    // metal). Costs ~2 instances of pool memory briefly.
    char* buf = static_cast<char*>(std::malloc(kPatchMax));
    bool  from_sd = false;
    const char* orc = orchestra_for(target, buf, &from_sd);
    CSOUND* cs = build_instance(orc, _block);
    if (!cs && from_sd) { from_sd = false; target = 0; cs = build_instance(kOrchestra, _block); }
    if (buf) std::free(buf);
    if (!cs) return;                                  // total failure -> keep the old instance running

    // Swap: pull the old instance out of the audio path, publish the new one, then destroy the old
    // (the SDRAM pool reclaims it, roadmap #2). reseed first so the new instance has the knob state.
    reseed_channels(cs);
    CSOUND* old = static_cast<CSOUND*>(_gate.take());
    publish_instance(cs);
    if (old) csoundDestroy(old);
    _sel = target;
    _patch_loaded = from_sd;
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------
void CsoundEngine::init(const EngineContext& ctx)
{
    _sr     = ctx.sample_rate;
    _block  = ctx.block_size;
    _stream = ctx.stream;

    // Route Csound's allocations (MBs at create/compile) to the SDRAM pool in csound_alloc.cpp: the
    // platform's default heap stays in SRAM (global ctors malloc before _hw.Init() powers up the FMC,
    // so a heap in SDRAM would fault there). Arm AFTER _hw.Init() (we're past it) and before
    // csoundCreate. ctx.arena is unused - Csound's heap comes from this pool.
    csound_heap_arm();

    rescan_bank();                               // probe /csound/<n>.csd -> the selectable bank
    _sel = (_avail_n > 1) ? 1 : 0;               // boot into the first SD patch if any, else built-in
    _sel_preview = _sel;

    char* buf = static_cast<char*>(std::malloc(kPatchMax));
    bool  from_sd = false;
    const char* orc = orchestra_for(_sel, buf, &from_sd);
    CSOUND* cs = build_instance(orc, _block);
    if (!cs && from_sd) {                         // a bad SD patch must not cost audio: use the built-in
        from_sd = false; _sel = 0;
        cs = build_instance(kOrchestra, _block);
    }
    if (buf) std::free(buf);
    if (cs) publish_instance(cs);                // boot: _cache is all 0, so no reseed needed
    _patch_loaded = from_sd;                      // gate stays null on total failure -> silence
}

void CsoundEngine::prepare()
{
    // Main-loop housekeeping (off the audio ISR). The heavy patch recompile lives here, gated against
    // the ISR by ReloadGate.
    //
    // Boot auto-load: the SD card mounts ~1 s after power-up (Storage's state machine), after this
    // engine's init() has already booted the built-in. So keep rescanning (throttled - rescan_bank is
    // f_stat I/O) until a slot appears, then load the first SD patch once. After that the user drives
    // patch choice with the Alt selector.
    if (_stream && !_boot_loaded && (_probe_div++ & 0x1FF) == 0) {
        rescan_bank();
        if (_avail_n > 1) {                          // a slot appeared (built-in is index 0)
            _boot_loaded = true;
            _reload_target = 1;                      // the first present SD slot
            _reload_pending = true;
        }
    }

    // While the Alt selector is open, keep rescanning if the bank is still empty so slots appear as
    // soon as the card mounts (radio's boot-retry pattern). Stops once any slot is found.
    if (_aux_held && _avail_n <= 1) _rescan_pending = true;
    if (_rescan_pending) { _rescan_pending = false; rescan_bank(); }
    if (_reload_pending) { _reload_pending = false; do_reload(_reload_target); }
}

void CsoundEngine::process(const float* const* in, float** out, size_t size)
{
    CSOUND* cs = static_cast<CSOUND*>(_gate.begin_use());
    if (!cs || _ksmps <= 0) {                    // no instance (boot fail / mid-reload) -> silence
        for (size_t i = 0; i < size; i++) { out[0][i] = 0.f; out[1][i] = 0.f; }
        _level *= 0.90f;                         // let the meter decay while silent
        _gate.end_use();
        return;
    }

    const size_t n = (size < static_cast<size_t>(_ksmps)) ? size : static_cast<size_t>(_ksmps);
    MYFLT*       spin  = csoundGetSpin(cs);       // ksmps*nchnls, interleaved
    const MYFLT* spout = csoundGetSpout(cs);

    const float* il = in ? in[0] : nullptr;
    const float* ir = in ? in[1] : nullptr;
    for (size_t i = 0; i < n; i++) {              // de-interleaved in -> interleaved spin
        spin[i * 2]     = il ? static_cast<MYFLT>(il[i]) : 0;
        spin[i * 2 + 1] = ir ? static_cast<MYFLT>(ir[i]) : 0;
    }

    // Drain pending MIDI notes HERE (audio ISR), so every csoundEvent + the instrument allocation it
    // triggers happens on the same thread as csoundPerformKsmps - never racing the main loop on the
    // SDRAM pool. async=0 is correct since we are already the performance thread; notes start p2=0,
    // i.e. this k-cycle. (handle_midi_note, on the main loop, only enqueues.)
    MidiNoteEvent ev;
    while (_midi_instr > 0 && _notes.pop(ev)) {
        float pf[kMidiNoteFields];
        csound_note_pfields(pf, _midi_instr, kMidiNoteDur, ev.note, ev.deck);
        MYFLT params[kMidiNoteFields];
        for (int k = 0; k < kMidiNoteFields; k++) params[k] = static_cast<MYFLT>(pf[k]);
        csoundEvent(cs, CS_INSTR_EVENT, params, kMidiNoteFields, 0 /*sync: we are the perf thread*/);
    }

    csoundPerformKsmps(cs);                       // one k-cycle: consumes spin, fills spout

    float peak = 0.f;
    for (size_t i = 0; i < n; i++) {              // interleaved spout -> de-interleaved out
        const float l = static_cast<float>(spout[i * 2]);
        const float r = static_cast<float>(spout[i * 2 + 1]);
        out[0][i] = l;
        out[1][i] = r;
        const float a = (l < 0 ? -l : l), b = (r < 0 ? -r : r);
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    // Peak meter for render(): fast attack, slow decay. Single-float write, read in the main loop.
    _level = (peak > _level) ? peak : _level * 0.90f;
    _gate.end_use();
}

Capabilities CsoundEngine::capabilities() const
{
    // CapAux: claim Alt+PITCH as the patch selector (ParamId::Aux). CapOwnDisplay: the engine draws
    // the rings (level meter, and the selector while Alt is held).
    return CapAux | CapOwnDisplay;
}

void CsoundEngine::set_param(ParamId id, DeckRef::Ref d, float v)
{
    if (id == ParamId::Aux) {                     // Alt+PITCH: preview a patch (deck A drives selection)
        if (d == DeckRef::A) _sel_preview = aux_to_index(v, _avail_n);
        return;                                   // committed on release (set_aux_active), in prepare()
    }

    CSOUND* cs = static_cast<CSOUND*>(_gate.current());
    int slot = -1;
    const char* chan = channel_for(id, d, slot);
    if (slot >= 0 && slot < kSlots) _cache[slot] = v;   // cache even while mid-reload (for reseed)
    if (!cs || !chan) return;
    // NOTE: called from the main loop while process() performs in the audio ISR. A control-channel
    // write racing a chnget read is a benign single-value race (Csound's intended host->orchestra
    // path); no torn structure.
    csoundSetControlChannel(cs, chan, static_cast<MYFLT>(v));
}

float CsoundEngine::param(ParamId id, DeckRef::Ref d) const
{
    if (id == ParamId::Aux)                        // pickup readback: the committed selection's position
        return (_avail_n <= 1) ? 0.f : (static_cast<float>(_sel) + 0.5f) / static_cast<float>(_avail_n);
    int slot = -1;
    channel_for(id, d, slot);
    return (slot >= 0 && slot < kSlots) ? _cache[slot] : 0.f;
}

DeckRef::Ref CsoundEngine::handle_midi_note(uint8_t channel, uint8_t note)
{
    // No instance or no MidiNote instrument -> not playable; tell the UI not to flash.
    if (!_gate.current() || _midi_instr <= 0) return DeckRef::Count;

    // Channel -> deck, matching the platform's configured MIDI channels (same as the other engines).
    const Config& c = Config::dynamic();
    DeckRef::Ref ref = DeckRef::Count;
    if      (channel == c.midi_channel_a()) ref = DeckRef::A;
    else if (channel == c.midi_channel_b()) ref = DeckRef::B;
    if (ref == DeckRef::Count) return DeckRef::Count;

    // Main loop: only enqueue. The audio ISR drains and schedules (see process()). A full ring drops
    // the note, but we still return the deck so the gate-in flashes (the NoteOn was received).
    _notes.push({ note, static_cast<uint8_t>(ref == DeckRef::A ? 0 : 1) });
    return ref;
}

void CsoundEngine::set_aux_active(DeckRef::Ref d, bool held)
{
    if (d != DeckRef::A) return;                  // deck A's Alt+PITCH drives the global patch selection
    if (held && !_aux_held) {                     // press: refresh the bank, start preview at the live one
        _rescan_pending = true;
        _sel_preview = _sel;
    }
    if (!held && _aux_held) {                      // release: commit if the choice changed
        if (_sel_preview != _sel) { _reload_target = _sel_preview; _reload_pending = true; }
    }
    _aux_held = held;
}

void CsoundEngine::render(DisplayModel& m)
{
    m.clear();
    const bool running = (_gate.current() != nullptr);

    // While Alt is held: the patch selector - one dot per selectable orchestra around each ring, the
    // previewed one bright. (The built-in is index 0; SD slots follow.)
    if (_aux_held) {
        for (int i = 0; i < 2; i++) {
            m.play[i] = { pal::kGreen, running ? 1.f : 0.f };
            ring::selector(m.ring[i], _avail_n, _sel_preview, pal::kCyan, 0.18f, pal::kCyan);
            m.ring[i].set_updated();
        }
        m.mode_center = { pal::kCyan, 0.6f };           // selector hue
        return;
    }

    // Otherwise: the output level meter on both rings (green base, amber past -4 dBish, red near clip).
    float lvl = _level;
    if (lvl > 1.f) lvl = 1.f;
    const uint32_t col = (lvl > 0.85f) ? 0xff2000u : (lvl > 0.60f) ? pal::kAmber : pal::kGreen;
    for (int i = 0; i < 2; i++) {
        m.play[i] = { pal::kGreen, running ? 1.f : 0.f };
        m.ring[i].set_hex_color(0x0a0a0a);              // faint full base ring
        m.ring[i].set_segment(0.f, 0.999f);
        if (running && lvl > 0.02f) ring::level(m.ring[i], lvl, col);   // bright arc proportional to level
        m.ring[i].set_updated();
    }
    // Centre mode LED tells the patch source: cyan = an SD slot, white = the built-in.
    m.mode_center = { _patch_loaded ? pal::kCyan : pal::kWhite, 0.5f };
}

} // namespace spotykach
