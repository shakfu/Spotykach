#include "app.h"

#include <functional>

#include "common.h"
#include "version.h"
#include "settings.h"
#include "hw/hardware.h"
#include "hw/buffer.sdram.h"
#include "ui/core.ui.h"
#include "engine/itimesource.h"
#include "engine/engine_select.h"  // ActiveEngine (build-time engine selection, item 3b)
#include "transport/transport.h"   // platform clock/transport service (shared across engines)
#include "memory/storage.h"
#include "expose.h"
#if defined(SPK_USE_STREAM)
#include "hw/stream_deck.h"   // SD streaming service (any SPK_USE_STREAM engine: tape, shuttle, radio)
#endif
#if SPK_TERMINAL
#include "terminal/terminal.h"   // USB-C text/command test channel (docs/dev/terminal-*.md)
#endif
#ifdef METER
#include "hid/usb.h"   // daisy::UsbHandle - direct non-blocking CDC for the CPU-load meter
#include <cstdio>      // snprintf
#endif

#define STORAGE

// #define METER
//
// SPK_CPU_METER - "is the CpuLoadMeter being DRIVEN", which is not the same question as "is METER=1".
// METER=1 both drives the meter and brings up a second USB device (`_meter_usb`, FS_EXTERNAL) purely to
// print the readings; that device claims the same OTG core the terminal channel needs, so METER=1 and
// TERMINAL=1 cannot coexist. A TERMINAL build therefore drives the meter itself and reports it on
// request via `query cpu` (see terminal/cpu_stat.h) - the measurement is two System::GetTick() reads
// per block, while only the printing needs a USB device of its own. Keep the printing block under
// METER alone; everything that only measures uses SPK_CPU_METER.
#if defined(METER) || SPK_TERMINAL
#define SPK_CPU_METER 1
#endif

#ifdef SPK_CPU_METER
#include "meter.h"
#endif

using namespace daisy;
using namespace infrasonic;

namespace spotykach {

// Hardware-backed clock for the DSP core. Off-target builds (host harness) supply their own.
struct DaisyTimeSource : ITimeSource {
    uint32_t now_ms() const override { return daisy::System::GetNow(); }
    uint32_t now_us() const override { return daisy::System::GetUs(); }
};

class AppImpl {
  public:
    AppImpl():
    _ui     { CoreUI(_hw, _engine, _transport, _settings, _storage) }
    {}

    ~AppImpl() = default;

    void Init();
    void Loop();
    // DAC modulation outputs. One block-rate engine call (no per-sample virtual on the ISR), then
    // convert float CV to the DAC's 12-bit range. set_lfo caches the block's last sample for the
    // cycle LED (read asynchronously at 62 Hz, so last-of-block is equivalent to the old per-sample).
    void process_cv(uint16_t** out, size_t size) {
        float cv0[kDacBufSize];
        float cv1[kDacBufSize];
        _engine.process_cv(cv0, cv1, size);
        for (size_t i = 0; i < size; i++) {
            out[0][i] = __USAT(cv0[i] * (1 << 12), 12);
            out[1][i] = __USAT(cv1[i] * (1 << 12), 12);
        }
        _ui.set_lfo(cv0[size - 1], cv1[size - 1]);
    }
    CoreUI& ui() { return _ui; }

#ifdef CHUCK_BRINGUP
    // ChucK bring-up only (build: make engine-chuck BRINGUP=1). Blink the Daisy onboard LED - which is
    // independent of the panel WS2812s - n times so we can see how far Init() gets when the panel never
    // comes up (solid-white = it died before _ui.init()). Blocking; bring-up builds only.
    void bringup_mark(int n) {
        daisy::System::Delay(500);
        for (int i = 0; i < n; i++) {
            _hw.seed.SetLed(true);  daisy::System::Delay(160);
            _hw.seed.SetLed(false); daisy::System::Delay(160);
        }
    }
    // Slow toggle from the audio ISR so "audio is running" is visible (~1.4 Hz at 256/48k).
    void bringup_audio_tick() {
        static uint32_t c = 0; static bool s = false;
        if (((c++) & 0x7F) == 0) { s = !s; _hw.seed.SetLed(s); }
    }
#endif

#if SPK_TERMINAL && TERM_USBDIAG
    // USB bring-up probe (build: make ENGINE=<e> TERMINAL=1 USBDIAG=1). When the port never enumerates
    // there is no channel to report over, so the verdict goes to the panel LEDs (CoreUI::_draw_usb_diag,
    // readable on a cased unit) and, redundantly, as a blink code on the Daisy onboard LED.
    //
    // NON-BLOCKING and non-exclusive: called once per Loop() iteration, it only drives the onboard LED
    // (which the app otherwise never touches) off wall-clock milliseconds. The app runs normally -
    // panel, audio, and crucially the boot-button DFU escape hatch (the 3s-held check below, and the
    // press-release reset in Hardware::ProcessDigitalControls) all stay live. An earlier version of
    // this parked in Init() instead and took the escape hatch with it; do not reintroduce that.
    //
    // The onboard-LED blink covers the first six checks only (ONE blink = bad, TWO = good, in the order
    // a pullup depends on them: clocks configured, HSI48, USB clock source, VDD33_USB, transceiver
    // powered, D+ pullup asserted). The panel shows all eleven bits at once - see _draw_usb_diag for
    // the full legend, including the pad-ownership and host-activity bits that identified the wrong-port
    // fault on 2026-07-31.
    void usb_diag_tick() {
        static constexpr int kMaxSeg = 6 * (2 * 2) + 6 + 1;   // worst case: 2 blinks/group + gaps
        static uint16_t seg_ms[kMaxSeg];
        static bool     seg_on[kMaxSeg];
        static int      seg_n = 0;
        static int      seg_i = 0;
        static uint32_t seg_start = 0;
        static bool     started = false;

        // The panel readout: the cased Spotykach hides the onboard LED, so push the verdict to the
        // WS2812s too (CoreUI::_draw_usb_diag renders it and suppresses the normal UI). Cheap enough to
        // repush every iteration; the snapshot never changes after init().
        {
            const UsbDiag& u = _terminal.refresh_usb_diag();   // re-read; an init snapshot hides later damage
            _ui.set_usb_diag(static_cast<uint16_t>(
                  (u.clocks_configured    ? 1u : 0u) << 0
                | (u.hsi48_ready          ? 1u : 0u) << 1
                | (u.usb_clk_source == 3  ? 1u : 0u) << 2
                | (u.usb33_ready          ? 1u : 0u) << 3
                | (u.transceiver_on       ? 1u : 0u) << 4
                | (u.pullup_asserted      ? 1u : 0u) << 5
                | (u.vbus_sensing         ? 0u : 1u) << 6   // green when sensing is OFF
                | (u.dp_af_ok             ? 1u : 0u) << 7
                | (u.dm_af_ok             ? 1u : 0u) << 8
                | (u.usb_reset_seen       ? 1u : 0u) << 9
                | (u.sof_seen             ? 1u : 0u) << 10));
        }

        if (!started) {   // build the blink schedule once, from the snapshot init() captured
            const auto& u = _terminal.usb_diag();
            const bool  bits[6] = { u.clocks_configured, u.hsi48_ready, u.usb_clk_source == 3,
                                    u.usb33_ready, u.transceiver_on, u.pullup_asserted };
            for (bool good : bits) {
                for (int i = 0; i < (good ? 2 : 1); i++) {
                    seg_ms[seg_n] = 140; seg_on[seg_n++] = true;    // blink on
                    seg_ms[seg_n] = 220; seg_on[seg_n++] = false;   // blink off
                }
                seg_ms[seg_n] = 700; seg_on[seg_n++] = false;       // gap between groups
            }
            seg_ms[seg_n] = 2500; seg_on[seg_n++] = false;          // gap between repeats
            seg_start = daisy::System::GetNow();
            started = true;
        }

        const uint32_t now = daisy::System::GetNow();
        if (now - seg_start >= seg_ms[seg_i]) {
            seg_start = now;
            seg_i = (seg_i + 1) % seg_n;
        }
        _hw.seed.SetLed(seg_on[seg_i]);
    }
#endif

    void ProcessAudio(AudioHandle::InputBuffer  in,
                      AudioHandle::OutputBuffer out,
                      size_t                    size);

  private:
    NOCOPY(AppImpl)

    #if DEBUG || defined(METER)
    StopwatchTimer _log_timer; // throttles the serial log (debug info and/or the CPU meter)
    #endif
    #ifdef METER
    daisy::UsbHandle _meter_usb; // non-blocking CDC for the load meter (no Logger -> can't spin/hang)
    #endif
    #if DEBUG
    void logDebugInfo();
    #endif
    bool _log_enabled;

    DaisyTimeSource _time_source;
    Transport       _transport; // platform clock; injected into the engine + driven by CoreUI
    ActiveEngine    _engine;  // concrete engine chosen at build time; platform sees only IEngine
    Hardware    _hw;
    Settings    _settings;
    Storage     _storage;
#if defined(SPK_USE_STREAM)
    StreamDeck  _stream;  // SD play/record streaming for streaming engines (pumped in Loop)
#endif
#if SPK_TERMINAL
    Terminal    _terminal;  // USB-C text/command channel: on-target engine testing + runtime control
#endif
    // DECLARED LAST, deliberately. Members are initialised in DECLARATION order, not in the order the
    // constructor's init list writes them, and _ui is the one member built from other members:
    // `_ui { CoreUI(_hw, _engine, _transport, _settings, _storage) }`. While _ui sat above _hw/
    // _settings/_storage those three were still uninitialised when CoreUI's constructor received them
    // - harmless only because that constructor does nothing but bind the references
    // (core.ui.cpp:21-32), and silent UB the moment anyone reads through one of them there. Keep _ui
    // below every member it is passed.
    CoreUI      _ui;
};
};

using namespace spotykach;

static AppImpl impl;

static int8_t leds_update_counter = 0;
void T5Callback(void* data) 
{
    impl.ui().process_gate_in();
    if (leds_update_counter++ == 3) {
        leds_update_counter = 0;
        impl.ui().render_leds();
    }
};

//According to GetPClk2Freq docs, timers run at the frequency twice faster
//as their peripheral frequency. So call_freq_hz should be twise smaller 
//then synclock period.
TimerHandle tim5_handle;
void StartT5Callback(TimerHandle::PeriodElapsedCallback cb, uint32_t call_freq_hz) {
    TimerHandle::Config timcfg;
    timcfg.periph = TimerHandle::Config::Peripheral::TIM_5;
    timcfg.dir = TimerHandle::Config::CounterDir::UP;
    timcfg.period = System::GetPClk2Freq() / call_freq_hz;
    timcfg.enable_irq = true;
    tim5_handle.Init(timcfg);
    tim5_handle.SetCallback(cb);
    tim5_handle.Start();
};

void DACCallback(uint16_t **out, size_t size)
{
    impl.process_cv(out, size);
};

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
#ifdef CHUCK_BRINGUP
    impl.bringup_audio_tick();   // onboard LED ~1.4 Hz = the audio ISR is alive
#endif
    impl.ProcessAudio(in, out, size);
};

void AppImpl::Init() 
{
    auto sample_rate = 48000;
    auto block_size = 96;
#if defined(SPK_ENGINE_CSOUND) || defined(SPK_ENGINE_CHUCK)
    // Csound/ChucK amortize a fixed per-block VM overhead, so a larger block buys CPU headroom (docs:
    // >=128, 256 proven for csound). Both are dedicated QSPI-only builds, so this changes no other
    // engine. ChucK's run() per-block cost (and double-precision UGen math) wants this just as much -
    // 96 is the small-block case the csound notes warn against. Trade: +~3.3 ms latency (256 vs 96).
    block_size = 256;
#endif
    _hw.Init(sample_rate, block_size);
#ifdef CHUCK_BRINGUP
    bringup_mark(1);   // reached: hardware (FMC/SDRAM) up
#endif

    // Hand the engine the SDRAM arena + clock; the engine sub-allocates whatever buffers it needs
    // (item: EngineBuffers generalization). The platform/HAL no longer knows any engine's layout.
    // The platform clock comes up first: the engine subscribes to its ticks during init().
    _transport.init(sample_rate, block_size, &_time_source);

    EngineContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.block_size = block_size;
    ctx.time = &_time_source;
    ctx.transport = &_transport;
    ctx.arena = SDRAMBuffer::pool().engineArena();
    ctx.qspi = &_hw.seed.qspi;   // QSPI flash handle for engines that persist a kit preset (edrums)
#if defined(SPK_USE_STREAM)
    {
        const auto sm = SDRAMBuffer::pool().streamMem();
        _stream.init({ sm.ring_a, sm.ring_a_bytes, sm.ring_b, sm.ring_b_bytes,
                       sm.scratch, sm.scratch_bytes });
        ctx.stream = &_stream;   // engine reads this in init()
    }
#endif
#ifdef CHUCK_BRINGUP
    bringup_mark(2);   // reached: about to call engine.init() (ChucK create + compileCode)
#endif
    _engine.init(ctx);
#ifdef CHUCK_BRINGUP
    bringup_mark(3);   // reached: engine.init() RETURNED (so ChucK create/compile did not hang/fault)
#endif

    _ui.init();
#ifdef CHUCK_BRINGUP
    bringup_mark(4);   // reached: UI initialised - boot essentially complete
#endif
    #ifdef STORAGE
    _storage.init(_engine);
    _storage.read_settigs();
    #endif

    Log::StartLog(false);
    // Touch the build banner through a volatile so it is retained even in release builds, where the
    // LOG_TAGGED below compiles to nothing (so `strings firmware.bin` can still report the version).
    volatile const char* fw_banner = firmware_banner();
    (void)fw_banner;
    LOG_TAGGED("boot", "%s", firmware_banner());
#if SPK_TERMINAL
    // Bring up the terminal channel after Log::StartLog so, in a build where a logger owns the CDC
    // device, it is already up before the RX callback attaches. Binds ActiveEngine as IEngine& for life.
    _terminal.init(_engine);
#endif
#if DEBUG || defined(METER)
    _log_timer.Init();
#endif

    StartT5Callback(T5Callback, 250);

    _hw.StartDAC(DACCallback);

    auto& audio = _hw.seed.audio_handle;
    audio.SetSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    audio.SetBlockSize(block_size);
    audio.Start(AudioCallback);

    _settings.init(_hw);
    _settings.read();

    _ui.calibrate(false);

    #ifdef SPK_CPU_METER
    Meter::cpu().load.Init(sample_rate, block_size);
    #endif
    #ifdef METER
    _meter_usb.Init(daisy::UsbHandle::FS_EXTERNAL); // CDC for the load meter (LOGGER_EXTERNAL port)
    #endif
}

void AppImpl::Loop()
{
    while(true) {
        // If boot button held for 3s, reset into bootloader mode for update
        if (_hw.GetBootButtonHeldTime() >= 3000)
        {
            System::ResetToBootloader(System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
        }

#if SPK_TERMINAL
        // Drain the command channel and republish the input-isolation flag: `mode test` freezes the
        // physical input path (knobs/CV/gate) so terminal-injected stimulus is the only engine driver.
        _terminal.process();
        _ui.set_input_frozen(_terminal.test_mode());
#if TERM_USBDIAG
        usb_diag_tick();   // blink the OTG_FS bring-up verdict on the onboard LED; non-blocking
#endif
#endif

        _ui.process();
        _engine.prepare();
        #ifdef STORAGE
        _storage.process();
        #endif
        #if defined(SPK_USE_STREAM)
        _stream.process();   // pump the slow SD play/record I/O for the streaming engine
        #endif
        
        #if DEBUG || defined(METER)
        if(_log_timer.HasPassedMs(250))
        {
            #if DEBUG
            logDebugInfo();
            #endif
            _log_timer.Restart();

            #ifdef METER
            auto& loadMeter = Meter::cpu().load;
            const int mx = (int)(loadMeter.GetMaxCpuLoad() * 10000.f + 0.5f); // hundredths of a percent
            const int av = (int)(loadMeter.GetAvgCpuLoad() * 10000.f + 0.5f);
            const int mn = (int)(loadMeter.GetMinCpuLoad() * 10000.f + 0.5f);
            char line[80];
            const int n = snprintf(line, sizeof(line),
                                   "load%% max=%d.%02d avg=%d.%02d min=%d.%02d\r\n",
                                   mx / 100, mx % 100, av / 100, av % 100, mn / 100, mn % 100);
            // Direct, NON-BLOCKING CDC write: drop the line if the host isn't draining the buffer, so the
            // meter can never hang the main loop (the daisy Logger spins after its first 2 packets).
            if (n > 0) _meter_usb.TransmitExternal((uint8_t*)line, (size_t)n);
            #endif
        }
        #endif
    }
}

void AppImpl::ProcessAudio(AudioHandle::InputBuffer  in,
                           AudioHandle::OutputBuffer out,
                           size_t                    size)
{
    #ifdef SPK_CPU_METER
    Meter::cpu().load.OnBlockStart();
    #endif

    _hw.ProcessAnalogControls();
    _ui.tick();
    _ui.read_cv();
    _engine.process(in, out, size);

    #ifdef SPK_CPU_METER
    Meter::cpu().load.OnBlockEnd();
    #endif
}

#if DEBUG
void AppImpl::logDebugInfo()
{
    Expose::values().print();

    // float val = hw.GetAnalogControlValue(Hardware::CTRL_PITCH_A);
    // float val = hw.GetControlVoltageValue(Hardware::CV_V_OCT_A);
    // Log::PrintLine(FLT_FMT(5), FLT_VAR(5, val));
    // uint16_t touch = hw.GetMpr121TouchStates();
}
#endif

void Application::Init() 
{
    impl.Init();
}

void Application::Loop() 
{
    impl.Loop();
}
