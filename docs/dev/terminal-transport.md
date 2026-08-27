# Terminal transport spec (phase 1)

Status: **built and hardware-verified (2026-07-31)** - but see the port correction below. This specifies layer [1] of the terminal channel (see [`terminal-control.md`](terminal-control.md)) - the bidirectional byte pipe over USB-C CDC: the RX producer (interrupt callback), the lock-free ring, the non-blocking TX path, and how the service coexists with the Daisy Logger on the one shared CDC device. It stops at raw bytes; the line codec and command dispatch (layers [2]/[3]) are a separate spec and are only stubbed here as the `LineSink` seam.

> **CORRECTION (2026-07-31, verified on hardware).** This spec's founding premise - that the channel > lives on the Daisy Seed's internal USB (OTG_FS, PA11/PA12) and must therefore share it with the > Logger - is **wrong for the Spotykach**. That board's panel USB-C is wired to **OTG_HS on PB14/PB15** > (Seed pins D29/D30), and it runs libDaisy's `extdfu` bootloader, which serves DFU over that same > core. Nothing is connected to PA11/PA12. The terminal now defaults to `FS_EXTERNAL` > (`SPK_TERMINAL_PORT_EXTERNAL`, `TERMPORT=int` for a bare Seed or Pod). > > The evidence was in the tree from the start and was misread twice: the Makefile routes the logger to > `LOGGER_EXTERNAL` unconditionally, which says which jack this hardware actually exposes. > `terminal-impl.md` deviation #1 saw that override and concluded "nothing owns `FS_INTERNAL`, so the > terminal must init it" - the opposite of what it implied. Read the rest of this spec with the port > substituted; the layering, ring, TX discipline and Logger-coexistence reasoning are all unaffected, > since OTG_HS runs in the same embedded full-speed device mode.

## libDaisy facts that constrain the design

Read from the vendored fork; cite before trusting.

- **`UsbHandle` is stateless.** `sizeof(UsbHandle) == 1`, enforced by `static_assert` in `lib/libDaisy/src/hid/logger_impl.h` (the `LoggerImpl<LOGGER_INTERNAL>` init). All state lives in file-scope globals (`hUsbDeviceFS`, `rx_callback`) and the ST USB device stack. **Consequence:** any `UsbHandle` instance is a view onto the *same* CDC device. The Logger's handle and a terminal-owned handle are interchangeable; there is exactly one device and exactly one `Init(FS_INTERNAL)`.

- **The Logger owns USB bring-up only when logging is compiled in.** `Log::StartLog` (`src/app.cpp:213`) reaches `LoggerImpl<LOGGER_INTERNAL>::Init()` -> `usb_handle_.Init(FS_INTERNAL)` **only if** `INFS_LOG` is set, because `common.h:46-50` selects `Logger<LOGGER_NONE>` otherwise and its `StartLog` is a no-op. So the "log routing" decision is exactly the existing `INFS_LOG` flag, with no libDaisy edits. This spec handles both cases.

- **RX re-arms before the callback, into a shared buffer.** `CDC_Receive_FS` (`lib/libDaisy/src/usbd/usbd_cdc_if.c:274-280`):

  ```c
  USBD_CDC_ReceivePacket(&hUsbDeviceFS);   // re-arm reception (into UserRxBufferFS, 2048B, set once)
  rx_callback_fs(Buf, Len);                // Buf aliases UserRxBufferFS; *Len = bytes this packet
  ```

  The callback runs in **USB IRQ context** (`OTG_FS_IRQHandler`) and **must copy `Buf`/`*Len` out immediately** - the buffer is reused by the next packet. This is the SPSC producer.

- **`SetReceiveCallback` takes a plain C function pointer** `void(*)(uint8_t*, uint32_t*)` (`hid/usb.h:38`). No context arg -> the callback is a `static` function pushing into a file-scope ring; it cannot be a capturing lambda or a bound member.

- **TX can block.** `CDC_Transmit_FS` returns busy while an IN transfer is in flight; the Logger's `TransmitSync` *spins* on it and switches to fully blocking after 2 packets (`hid/logger.h:104-114`). The `METER` path documents that the Logger "spins after its first 2 packets" when the host is not draining (`src/app.cpp:278-280`). **The terminal must never route replies through the Logger** - replies use an independent non-blocking transmit that drops/queues, so a disconnected host can never hang the main loop.

## Ownership and initialization

One CDC device, shared. The terminal owns the **RX callback** and an **independent non-blocking TX**; the Logger (when present) keeps doing its own TX. Init depends on who brings up the device:

| `INFS_LOG` | Who calls `Init(FS_INTERNAL)` | Terminal init does |
|-----------|-------------------------------|--------------------|
| `1` (unified console) | Logger, via `Log::StartLog` (`app.cpp:213`) | `SetReceiveCallback` only - must NOT re-init |
| `0` (reply-only) | nobody (Logger is `LOGGER_NONE`) | `Init(FS_INTERNAL)` then `SetReceiveCallback` |

```cpp
// src/terminal/terminal.cpp
void Terminal::init() {
#if !INFS_LOG
    _usb.Init(daisy::UsbHandle::FS_INTERNAL);   // no Logger to bring the device up; we own it
#endif
    _usb.SetReceiveCallback(&Terminal::RxTrampoline, daisy::UsbHandle::FS_INTERNAL);
}
```

**Ordering:** `Terminal::init()` must run *after* `Log::StartLog(false)` (`app.cpp:213`) so that, in the unified case, the device is already up before the callback is attached. Place the call immediately after the boot banner in `AppImpl::Init()` (see integration below).

Port note: phase-1 terminal is `FS_INTERNAL` (USB-C) only. It is independent of the `METER` build, which uses `FS_EXTERNAL`; they touch different peripherals. Do not enable both as "the console."

## RX producer - the interrupt callback

A single file-scope ring and a static trampoline. Copy out, publish, return - nothing else in IRQ context.

```cpp
// src/terminal/terminal.cpp
static RxRing g_rx;   // file-scope: the callback has no context pointer

void Terminal::RxTrampoline(uint8_t* buf, uint32_t* len) {   // USB IRQ context
    g_rx.push(buf, static_cast<size_t>(*len));               // memcpy into ring; drop on overflow
}
```

## The SPSC ring

Single producer (IRQ), single consumer (main loop) -> a lock-free byte ring; no critical sections, no disabling interrupts. Free-running `uint32_t` head/tail masked to a power-of-two capacity (the kfifo discipline: `size = head - tail`, unambiguous full/empty).

```cpp
// src/terminal/rx_ring.h
class RxRing {
  public:
    // Producer (IRQ). Copies as many bytes as fit; drops the remainder and latches _overflow.
    void push(const uint8_t* src, size_t n) {
        uint32_t head = _head;                       // producer owns _head
        uint32_t free = kCap - (head - _tail);       // _tail is volatile (written by consumer)
        if (n > free) { _overflow = true; n = free; }
        for (size_t i = 0; i < n; ++i)
            _buf[(head + i) & kMask] = src[i];
        __DMB();                                      // data lands before the index is published
        _head = head + n;                             // publish
    }
    // Consumer (main loop). Returns bytes copied out, up to max.
    size_t pop(uint8_t* dst, size_t max) {
        uint32_t tail = _tail;                        // consumer owns _tail
        uint32_t avail = _head - tail;                // _head is volatile (written by producer)
        __DMB();                                      // read index before reading data
        size_t n = avail < max ? avail : max;
        for (size_t i = 0; i < n; ++i)
            dst[i] = _buf[(tail + i) & kMask];
        _tail = tail + n;                             // publish
        return n;
    }
    bool take_overflow() { bool o = _overflow; _overflow = false; return o; }

  private:
    static constexpr uint32_t kCap  = 512;            // power of two; line commands are short
    static constexpr uint32_t kMask = kCap - 1;
    uint8_t           _buf[kCap];
    volatile uint32_t _head = 0;                      // producer -> consumer
    volatile uint32_t _tail = 0;                      // consumer -> producer
    volatile bool     _overflow = false;
};
```

- **Correctness model.** Single core, single cache: no cache-coherency problem between IRQ and main loop (same core sees its own cache). The only hazards are compiler/CPU **reordering**, handled by `volatile` indices plus a `__DMB()` between the data copy and the index publish (producer) and between the index read and the data read (consumer). This is a standard M7 SPSC ring, not a multi-core lock-free structure.

- **Placement.** Default internal SRAM/DTCM (the array is 512 B). It is not DMA memory and needs no cache maintenance; keep it out of SDRAM.

- **Overflow policy.** Drop the overflowing tail of the packet and latch `_overflow`; the consumer reports `err overflow` on the next drained line so a test never silently loses input. Lines are short and the consumer drains every `Loop()` iteration, so overflow means the host flooded, not normal use.

## TX - replies, non-blocking

Replies never use the Logger. A small TX FIFO is flushed non-blocking each `process()`; if `CDC_Transmit_FS` is busy it is retried next iteration, and only a full TX FIFO drops (latched, so the next reply can note it). A connected test host always drains, so this path is effectively lossless in the case that matters.

```cpp
void Terminal::write(const char* s, size_t n) { _tx.enqueue(s, n); }   // called by dispatch layer

void Terminal::flush_tx() {                                            // main loop
    size_t n = _tx.peek(_scratch, sizeof(_scratch));                   // contiguous chunk
    if (n && _usb.TransmitInternal((uint8_t*)_scratch, n) == daisy::UsbHandle::Result::OK)
        _tx.commit(n);                                                 // drop only on success
}
```

**Interleave with logs is safe without a lock.** Both Logger TX (from `logDebugInfo`/boot) and terminal TX (from `process()`) execute on the **main loop** - a single thread - so they serialize naturally and cannot corrupt each other mid-line. The RX callback is the only IRQ-context actor and it touches only the RX ring, never TX. In the unified (`INFS_LOG=1`) case the host therefore sees interleaved but line-framed `[tag] ...` log lines and `ok/err ...` replies; the harness filters by prefix.

## Main-loop integration

The consumer runs where control input already lives (`AppImpl::Loop`, `src/app.cpp:243`), beside `_ui.process()` and `_stream.process()`.

```cpp
// app.cpp - member
#if SPK_TERMINAL
    Terminal _terminal;
#endif

// AppImpl::Init(), immediately after the boot banner (app.cpp:218)
#if SPK_TERMINAL
    _terminal.init();          // after Log::StartLog so the device is up in the unified case
#endif

// AppImpl::Loop(), in the while(true) body
#if SPK_TERMINAL
    _terminal.process();       // drain RX -> assemble lines -> dispatch; flush TX
#endif
```

`Terminal::process()` for phase 1:

```cpp
void Terminal::process() {
    if (g_rx.take_overflow()) emit_line("err overflow");
    uint8_t chunk[64];
    size_t  n;
    while ((n = g_rx.pop(chunk, sizeof(chunk))) > 0)
        for (size_t i = 0; i < n; ++i)
            _asm.feed(chunk[i], _line_sink);   // line assembler -> LineSink on '\n'
    flush_tx();
}
```

## The codec seam (stub)

Transport delivers whole lines and accepts reply bytes; it knows nothing about verbs. The line assembler (`_asm`) accumulates bytes into a bounded line buffer, trims `\r`, and on `\n` invokes:

```cpp
// LineSink: implemented by the layer-[3] dispatcher in a later spec. Phase-1 transport ships a
// trivial echo/`ok` sink so the pipe is testable end-to-end before the codec exists.
using LineSink = void(*)(const char* line, size_t n, Terminal& reply);
```

A bounded line buffer (e.g. 128 B) with overflow -> `err line-too-long` prevents an unterminated flood from growing without bound.

## Concurrency model

| Actor | Context | Touches | Rule |
|-------|---------|---------|------|
| `RxTrampoline` | USB IRQ | `g_rx.push` only | copy out, publish head, return; no TX, no alloc |
| `Terminal::process` | main loop | `g_rx.pop`, `_asm`, dispatch, `flush_tx` | the only consumer; drains every iteration |
| `Terminal::write` | main loop (from dispatch) | `_tx.enqueue` | same thread as `flush_tx`; no lock |
| Logger TX | main loop (boot/debug) | its own `tx_buff_` | serializes with terminal TX by being single-threaded |

Single producer, single consumer, single TX thread. No mutex, no interrupt disabling.

## Failure modes and edge cases

- **No host connected.** RX simply never fires; TX `TransmitInternal` returns busy/err and the FIFO holds (bounded) - never spins. Boot logging already tolerates this (the METER path relies on it).

- **Host connects mid-run.** CDC enumeration is handled by the ST stack; the callback starts firing on first RX. No terminal-side action. (DTR/line-state handshakes are not required for CDC RX here.)

- **RX overflow / unterminated line.** Ring overflow -> `err overflow`; line-buffer overflow -> `err line-too-long`. Both latch and report; neither blocks.

- **Reset-to-bootloader.** `Loop()` still services the boot-button DFU path (`app.cpp:247`) every iteration; `process()` adds no blocking call, so it cannot starve it. A `reset boot` *command* is out of scope for phase-1 transport (a later dispatch feature).

- **Double-init guard.** The `#if !INFS_LOG` gate is the guard; do not add an unconditional `Init(FS_INTERNAL)` or the unified case re-inits the live device.

## To verify on hardware

- CDC RX actually re-arms across back-to-back packets under the fork (the code path says yes; confirm a >64-byte paste arrives intact through the ring).

- `TransmitInternal` busy-return cadence with a draining host vs a silent host (confirm no spin).

- Enumeration name/VID-PID as seen by the host (cosmetic; affects the `tools/` port glob).

## Out of scope for phase-1 transport

Line grammar and tokenizing (codec, layer [2]); verb dispatch and `IEngine` binding (layer [3]); `mode test` input isolation; `describe`/`measure`/`stim`; OSC/SLIP. This spec ends at "whole lines in, reply bytes out, safely, on one shared CDC device."
