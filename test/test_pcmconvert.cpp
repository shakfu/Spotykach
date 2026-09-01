// Host tests for convert_pcm_block (src/core/pcm_convert.h), the loop-buffer load shim.
#include "pcm_convert.h"
#include "check.h"
#include <cstring>
#include <vector>

using namespace spotykach;

void run_pcmconvert_tests() {
  std::printf("pcm_convert:\n");

  // f32 -> i16: float input bytes convert to clamped 16-bit, half the byte count.
  {
    float in[4] = {0.f, 1.f, -1.f, 2.f};  // last is out of range -> clamps
    int16_t out[4] = {0, 0, 0, 0};
    convert_pcm_block(reinterpret_cast<const uint8_t*>(in), 4, 4,
                      reinterpret_cast<uint8_t*>(out), 2);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 32767);
    CHECK_EQ(out[2], -32767);
    CHECK_EQ(out[3], 32767);  // clamped
  }

  // i16 -> f32: 16-bit input expands to float, double the byte count.
  {
    int16_t in[3] = {0, 32767, -32767};
    float out[3] = {0, 0, 0};
    convert_pcm_block(reinterpret_cast<const uint8_t*>(in), 3, 2,
                      reinterpret_cast<uint8_t*>(out), 4);
    CHECK_NEAR(out[0], 0.f, 1e-6);
    CHECK_NEAR(out[1], 1.f, 1e-6);
    CHECK_NEAR(out[2], -1.f, 1e-6);
  }

  // f32 -> i16 -> f32 round-trips within one quantization step (the legacy-tape path,
  // mimicking save-as-float then load-into-int16-buffer, or the reverse).
  {
    const int N = 2001;
    std::vector<float> orig(N), back(N);
    std::vector<int16_t> mid(N);
    for (int i = 0; i < N; i++) orig[i] = (i - 1000) / 1000.f;  // -1.0 .. 1.0

    convert_pcm_block(reinterpret_cast<const uint8_t*>(orig.data()), N, 4,
                      reinterpret_cast<uint8_t*>(mid.data()), 2);
    convert_pcm_block(reinterpret_cast<const uint8_t*>(mid.data()), N, 2,
                      reinterpret_cast<uint8_t*>(back.data()), 4);

    bool within = true;
    for (int i = 0; i < N; i++) {
      float d = back[i] - orig[i];
      if (d < 0) d = -d;
      if (d > 2e-5f) within = false;
    }
    CHECK(within);
  }

  // --- the wider format set (u8 / i24 / i32), added for the unified WAV read path ------------------

  // pcm_format_of: the WAV header's (AudioFormat, BitsPerSample) pairs this firmware handles, and the
  // ones it must refuse rather than misread.
  {
    PcmFormat f = PcmFormat::f32;
    CHECK(pcm_format_of(1, 8, f)  && f == PcmFormat::u8);
    CHECK(pcm_format_of(1, 16, f) && f == PcmFormat::i16);
    CHECK(pcm_format_of(1, 24, f) && f == PcmFormat::i24);
    CHECK(pcm_format_of(1, 32, f) && f == PcmFormat::i32);   // 32-bit INTEGER, not float
    CHECK(pcm_format_of(3, 32, f) && f == PcmFormat::f32);
    CHECK(!pcm_format_of(3, 64, f));    // f64: legal WAV, not handled
    CHECK(!pcm_format_of(1, 12, f));    // no such PCM depth here
    CHECK(!pcm_format_of(2, 4, f));     // MS ADPCM
    CHECK(!pcm_format_of(3, 16, f));    // float tag with an impossible depth
  }

  CHECK_EQ((int)pcm_bytes(PcmFormat::u8), 1);
  CHECK_EQ((int)pcm_bytes(PcmFormat::i16), 2);
  CHECK_EQ((int)pcm_bytes(PcmFormat::i24), 3);
  CHECK_EQ((int)pcm_bytes(PcmFormat::i32), 4);
  CHECK_EQ((int)pcm_bytes(PcmFormat::f32), 4);

  // Endpoints and silence for every format. Integer endpoints are the positive maxima, so +/-1.0 maps
  // exactly; 8-bit is unsigned offset-binary, where silence is 128 rather than 0.
  {
    struct { PcmFormat f; } cases[] = { {PcmFormat::u8}, {PcmFormat::i16}, {PcmFormat::i24},
                                        {PcmFormat::i32}, {PcmFormat::f32} };
    for (auto c : cases) {
      uint8_t buf[4] = {0, 0, 0, 0};
      pcm_write1(buf, c.f, 0.f);   CHECK_NEAR(pcm_read1(buf, c.f),  0.f, 1e-2);
      pcm_write1(buf, c.f, 1.f);   CHECK_NEAR(pcm_read1(buf, c.f),  1.f, 1e-6);
      pcm_write1(buf, c.f, -1.f);  CHECK_NEAR(pcm_read1(buf, c.f), -1.f, 1e-6);
      if (c.f != PcmFormat::f32) {   // integer targets hard-clip; f32 passes over-range values through
        pcm_write1(buf, c.f, 3.f);   CHECK_NEAR(pcm_read1(buf, c.f),  1.f, 1e-6);   // clamped, not wrapped
        pcm_write1(buf, c.f, -3.f);  CHECK_NEAR(pcm_read1(buf, c.f), -1.f, 1e-6);
      }
    }
    uint8_t z = 0;
    pcm_write1(&z, PcmFormat::u8, 0.f);
    CHECK_EQ((int)z, 128);   // 8-bit silence is 128, not 0 - the whole reason u8 needs its own tag
  }

  // i24 is read from 3 little-endian bytes with a real sign extension (the byte pattern below is
  // -8388607, i.e. full-scale negative; a naive unsigned read would give a large positive).
  {
    const uint8_t neg_full[3] = {0x01, 0x00, 0x80};
    CHECK_NEAR(pcm_read1(neg_full, PcmFormat::i24), -1.f, 1e-6);
    const uint8_t pos_full[3] = {0xff, 0xff, 0x7f};
    CHECK_NEAR(pcm_read1(pos_full, PcmFormat::i24), 1.f, 1e-6);
    const uint8_t minus_one[3] = {0xff, 0xff, 0xff};   // -1 LSB, just below zero
    CHECK(pcm_read1(minus_one, PcmFormat::i24) < 0.f);
  }

  // i32 full-scale must not wrap. 2147483647.0 is not representable as a float, so a float-domain
  // scale would round +1.0 up past INT32_MAX and land on INT32_MIN - a full-scale peak inverting.
  {
    uint8_t buf[4];
    pcm_write1(buf, PcmFormat::i32, 1.f);
    int32_t s; std::memcpy(&s, buf, 4);
    CHECK(s > 2147000000);   // positive and near full scale, NOT negative
  }

  // Block conversion across widths: a ramp survives i24 -> f32 -> i24 (24-bit is lossless through
  // float32's 24-bit mantissa) and the byte counts are what the widths say.
  {
    const int N = 64;
    std::vector<uint8_t> src(N * 3), back(N * 3);
    std::vector<float> mid(N);
    for (int i = 0; i < N; i++) {
      const float v = (i - 32) / 32.f * 0.9f;
      pcm_write1(src.data() + i * 3, PcmFormat::i24, v);
    }
    convert_pcm_block(src.data(), N, PcmFormat::i24, reinterpret_cast<uint8_t*>(mid.data()), PcmFormat::f32);
    convert_pcm_block(reinterpret_cast<const uint8_t*>(mid.data()), N, PcmFormat::f32, back.data(), PcmFormat::i24);
    CHECK(std::memcmp(src.data(), back.data(), N * 3) == 0);
  }

  // u8 -> f32: the three points that define offset-binary (0 = full negative, 128 = silence, 255 = full
  // positive). This is the format most likely to be misread as signed.
  {
    const uint8_t in[3] = {1, 128, 255};
    float out[3] = {9, 9, 9};
    convert_pcm_block(in, 3, PcmFormat::u8, reinterpret_cast<uint8_t*>(out), PcmFormat::f32);
    CHECK_NEAR(out[0], -1.f, 1e-6);
    CHECK_NEAR(out[1], 0.f, 1e-6);
    CHECK_NEAR(out[2], 1.f, 1e-6);
  }

  // Zero samples is a no-op (does not write or read).
  {
    uint8_t dst[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    convert_pcm_block(nullptr, 0, 4, dst, 2);
    CHECK_EQ((int)dst[0], 0xAA);  // untouched
  }
}
