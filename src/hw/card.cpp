#include "card.h"
#include "../memory/wav.h"        // kWavBytesPerSample (the buffer's storage width)
#include "../memory/wav_source.h"  // parse_wav / WavInfo (the shared RIFF chunk walk)
#include <string.h>

// Size-optimize this whole TU to reclaim SRAM_EXEC. SD-card chunked I/O is main-loop only (never
// the audio path), so -Os is perf-irrelevant here.
#pragma GCC optimize("Os")

using namespace spotykach;
using namespace daisy;

// The buffer's own rate, and sanity bounds on a header's stated rate (the resample ratio divides by
// it). Mirrors WavStreamReader::kPlaybackSampleRate / kRateMin / kRateMax on the streaming path.
static constexpr uint32_t kDeviceRate = 48000;
static constexpr uint32_t kMinRate    = 4000;
static constexpr uint32_t kMaxRate    = 192000;

Card::Card():
_state { State::unmounted }
{}

void Card::init(uint8_t* buffer) {
    _buffer = buffer;
}

bool Card::file_exists(const char* path)
{
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK && fno.fsize > 0; 
}

void Card::init_read_audio(const AudioData data) 
{   
    if (_state != State::idle) return;

    _size_read_audio = 0;

    char audio_path[12]; // "/SK/G/1.WAV"
    audio_path[0] = '/';
    strcpy(audio_path + 1, data.root_dir);
    strcat(audio_path, "/");
    strcat(audio_path, data.tape_dir);
    strcat(audio_path, "/");
    strcat(audio_path, data.file_name);

    WavInfo info;
    size_t bytesread = 0;
    
    if (f_open(&_sdfile, audio_path, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
        _state = State::failed;
        return;
    }
    
    // The header is parsed out of the first chunk we already read, through the shared RIFF walk
    // (memory/wav_source.h) that the streaming readers use. Bound the walk by `bytesread`, not
    // kChunk: a file shorter than one chunk leaves the rest of _buffer holding the previous read's
    // bytes, and a walk that stepped into them would be parsing stale data.
    if (f_read(&_sdfile, _buffer, kChunk, &bytesread) != FR_OK
    || !parse_wav(_buffer, (uint32_t)bytesread, info)) {
        _state = State::failed;
        _close_file();
        return;
    }

    // Accept any PCM depth this firmware can decode (u8/i16/i24/i32/f32), 1..8 channels, and any
    // sane rate: whatever the file holds is converted to the buffer's stereo native frames as it
    // loads (PcmLoader), so a mono 24-bit 44.1 kHz file loads as correctly as the stereo float one
    // the engine records. Resampling on LOAD rather than on playback is what keeps the buffer in
    // device frames, so every frame<->tempo<->tick relationship downstream is untouched.
    PcmFormat src_fmt;
    const bool fmt_ok = pcm_format_of(info.audio_format, info.bits_per_sample, src_fmt);
    const bool rate_ok = info.sample_rate >= kMinRate && info.sample_rate <= kMaxRate;
    if (fmt_ok
        && info.channels >= 1 && info.channels <= kPcmMaxChannels
        && rate_ok
        && info.data_size > 0) {
            _loader.begin((size_t)info.data_size, src_fmt, info.channels,
                          data.body, data.body_size, kNativeSampleFormat, 2,
                          info.sample_rate, kDeviceRate);
            _offset = 0;
            _size   = _loader.size_bytes();  // keep progress() in sync
            _state  = State::read_audio;
    }
    else {
        _state = State::failed;
        _close_file();
    }

    if (f_lseek(&_sdfile, info.data_start) != FR_OK) {
        _state = State::failed;
        _close_file();
    }
}
void Card::read_audio()
{
    if (_state != State::read_audio) {
        return;
    }
    
    size_t bytesread;
    if (f_read(&_sdfile, _buffer, kChunk, &bytesread) != FR_OK) {
        _state = State::failed;
        _close_file();
        return;
    }

    // The loader converts (when the file's format differs from the buffer's) and tracks the
    // byte/frame accounting, carrying any frame that straddles a chunk boundary - kChunk does not
    // divide every frame size (a stereo 24-bit frame is 6 bytes).
    bool buffer_full = _loader.feed(_buffer, bytesread);
    _offset = _loader.offset();                  // keep progress() in sync
    _size_read_audio = _loader.frames();

    if (bytesread < kChunk || buffer_full) {
        _notify_finish_processing = true;
        _state = State::idle;
        _close_file();
        return;
    }
}

void Card::init_write_audio(const AudioData data)
{
    if (_state != State::idle) return;

    auto res = f_mkdir(data.root_dir);
    if (res != FR_OK && res != FR_EXIST) {
        _state = State::failed;
        return;
    }

    char tape_dir_path[5]; // "SK/G"
    strcpy(tape_dir_path, data.root_dir);
    strcat(tape_dir_path, "/");
    strcat(tape_dir_path, data.tape_dir);
    res = f_mkdir(tape_dir_path);
    if (res != FR_OK && res != FR_EXIST) {
        _state = State::failed;
        return;
    }

    char audio_path[11]; // "SK/G/1.WAV"
    strcpy(audio_path, tape_dir_path);
    strcat(audio_path, "/");
    strcat(audio_path, data.file_name);
    if (f_open(&_sdfile, audio_path, (FA_CREATE_ALWAYS) | (FA_WRITE)) != FR_OK) {
        _state = State::failed;
        return;
    }

    uint32_t byteswritten;
    if (f_write(&_sdfile, data.header, data.header_size, (UINT*)&byteswritten) == FR_OK) {
        _bytes = (uint8_t*)data.body;
        _size = data.body_size;
        _offset = 0;
        _state = State::write_audio;
    }
    else {
        _state = State::failed;
        _close_file();
    }
}

void Card::write_audio() 
{
    if (_state != State::write_audio) {
        return;
    }
    if (_offset >= _size) {
        _notify_finish_processing = true;
        _state = State::idle;
        _close_file();
        return;
    }
    
    uint32_t byteswritten;
    auto write_len = std::min(_size - _offset, kChunk);
    if (f_write(&_sdfile, &_bytes[_offset], write_len, (UINT*)&byteswritten) != FR_OK) {
        _state = State::failed;
        _close_file();
        return;
    }
    _offset += byteswritten;
}

bool Card::read_file(const char* path, uint8_t*& out_data, size_t* out_size)
{
    if (_state != State::idle) return false;

    _state = State::read_file;
    if (f_open(&_sdfile, path, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
        _state = State::idle;
        return false;
    }
    
    if (f_read(&_sdfile, _buffer, kChunk, out_size) != FR_OK) {
        _state = State::idle;
        _close_file();
        return false;
    }
    
    _close_file();
    _state = State::idle;

    out_data = _buffer;
    
    return true;
}

bool Card::write_file(const char* path, const uint8_t* in_data, const size_t in_size)
{
    if (_state != State::idle) return false;

    _state = State::write_file;
    if (f_open(&_sdfile, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        _state = State::idle;
        return false;
    }
    
    uint32_t byteswritten;
    if (f_write(&_sdfile, in_data, in_size, (UINT*)&byteswritten) != FR_OK) {
        _close_file();
        _state = State::idle;
        return false;
    }

    _close_file();
    _state = State::idle;
    
    return true;
}

void Card::_close_file()
{
    f_close(&_sdfile);
}

void Card::recognize()
{
    /* Init handler */
    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();
    sd_cfg.speed = SdmmcHandler::Speed::MEDIUM_SLOW;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_1;
    _sd.Init(sd_cfg);

    /* Links libdaisy i/o to fatfs driver. */
    _fsi.Init(FatFSInterface::Config::MEDIA_SD);

    _state = State::mounting;
}

bool Card::mount()
{
    /* Mount the card */
    auto path = _fsi.GetSDPath();
    if (f_mount(&_fsi.GetSDFileSystem(), path, 1) != FR_OK) {
        return false;
    }
    _state = State::idle;
    return true;
}

void Card::unmount()
{
    auto path = _fsi.GetSDPath();
    f_mount(NULL, path, 0);
    _fsi.DeInit();
    _state = State::unmounted;
}

bool Card::notify_finish_processing()
{
    auto did_finish = _notify_finish_processing;
    _notify_finish_processing = false;
    return did_finish;
}

void Card::cancel()
{
    _close_file();
    _state = State::idle;
}