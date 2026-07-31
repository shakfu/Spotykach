#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include "dev/sdram.h"
#include "daisy_seed.h"
#include "nocopy.h"
#include "card.h"
#include "engine/engine_context.h" // EngineArena

namespace spotykach {

// The SDRAM allocator. Since item "EngineBuffers generalization" (Stage 2) it is engine-agnostic:
// it owns one big SDRAM arena that the active engine sub-allocates (via engine/arena.h), plus the
// platform's SD-card staging buffer. It no longer knows any engine's buffer shape, so the HAL has
// no dependency on the granular DSP.
class SDRAMBuffer {
public:
    static SDRAMBuffer& pool() {
        static SDRAMBuffer instance;
        return instance;
    }

    // Opaque SDRAM the active engine sub-allocates from at init().
    EngineArena engineArena();

    // SD-card chunked-I/O staging buffer (platform-owned; used by Storage).
    uint8_t* card_buffer() const;

#if defined(SPK_USE_STREAM)
    // One SDRAM read-ahead/write-behind ring PER DECK (each ring serves that deck's play OR record,
    // since a deck is play-XOR-record) + a shared chunk scratch for a streaming engine.
    // Power-of-two ring sizes (required by SpscRing). Only allocated for SPK_USE_STREAM builds.
    struct StreamMem { uint8_t* ring_a; uint32_t ring_a_bytes;
                       uint8_t* ring_b; uint32_t ring_b_bytes;
                       uint8_t* scratch; uint32_t scratch_bytes; };
    StreamMem streamMem() const;
#endif

private:
    NOCOPY(SDRAMBuffer)

    SDRAMBuffer() = default;
    ~SDRAMBuffer() = default;
};
};
