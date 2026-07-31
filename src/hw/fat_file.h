#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include <daisy_seed.h>          // FatFs FIL / f_* via libDaisy
#include "memory/byte_file.h"

namespace spotykach {

// FatFs-backed IByteFile: the device implementation the streaming-WAV codec (wav_stream.h) reads/writes
// through. Main-loop use only (FatFs is not ISR-safe); the audio ISR only ever touches the ring.
class FatFile : public IByteFile {
public:
    bool open_read(const char* path);
    bool open_write(const char* path);
    void close();
    bool is_open() const { return _open; }

    uint32_t read(void* dst, uint32_t n) override;
    uint32_t write(const void* src, uint32_t n) override;
    bool     seek(uint32_t pos) override;

private:
    FIL  _f;
    bool _open = false;
};

} // namespace spotykach
