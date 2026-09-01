// Simulation tests for PcmLoader (src/core/pcm_loader.h) - the exact accounting that
// card.cpp's audio load runs, exercised here without FatFS. Verifies frame counts,
// truncation to capacity, termination, and correct sample placement, for every width combo
// and across many chunk sizes.
#include "pcm_loader.h"
#include "pcm_convert.h"
#include "check.h"
#include <vector>
#include <cstring>
#include <utility>

using namespace spotykach;

// Stream a simulated file of `file_frames` stereo frames (src_bps each) into a buffer of
// `cap_frames` capacity (dst_bps each), feeding `chunk` bytes per step like card.cpp.
// Returns frames loaded.
static size_t run_load(size_t file_frames, int src_bps, size_t cap_frames, int dst_bps,
                       size_t chunk, std::vector<uint8_t>* out_dst = nullptr,
                       std::vector<uint8_t>* out_file = nullptr) {
  std::vector<uint8_t> file(file_frames * 2 * src_bps);
  for (size_t i = 0; i < file.size(); i++) file[i] = (uint8_t)((i * 131 + 7) & 0xFF);
  std::vector<uint8_t> dst(cap_frames * 2 * dst_bps, 0);

  PcmLoader L;
  L.begin(file.size(), src_bps, dst.data(), dst.size(), dst_bps);

  size_t pos = 0;
  while (true) {
    size_t n = (file.size() - pos < chunk) ? (file.size() - pos) : chunk;
    bool full = L.feed(file.data() + pos, n);
    pos += n;
    if (n < chunk || full) break;  // short read (EOF) or buffer full - mirrors card.cpp
  }
  if (out_dst) *out_dst = std::move(dst);
  if (out_file) *out_file = std::move(file);
  return L.frames();
}

void run_pcmloader_tests() {
  std::printf("PcmLoader:\n");

  const size_t K = 32768;  // real card kChunk

  // Match (no conversion), both widths: load exactly the file when it fits.
  CHECK_EQ((long long)run_load(100000, 4, 200000, 4, K), 100000);
  CHECK_EQ((long long)run_load(50000, 2, 50000, 2, K), 50000);

  // Truncation to buffer capacity when the file is longer.
  CHECK_EQ((long long)run_load(300000, 4, 200000, 4, K), 200000);

  // Convert float -> int16 (legacy tape into a lo-fi build): same frame count, fits.
  CHECK_EQ((long long)run_load(100000, 4, 200000, 2, K), 100000);
  // ...and an 84s-ish int16 tape truncated into a 42s-ish float buffer.
  CHECK_EQ((long long)run_load(300000, 2, 200000, 4, K), 200000);

  // Convert int16 -> float (lo-fi tape into a stock build): exact fit.
  CHECK_EQ((long long)run_load(123456, 2, 123456, 4, K), 123456);

  // Small file (less than one chunk) loads fully, in both directions.
  CHECK_EQ((long long)run_load(10, 4, 1000, 2, K), 10);
  CHECK_EQ((long long)run_load(3, 2, 1000, 4, K), 3);

  // Robust across many small chunk sizes (more iterations, exact-fill edges).
  for (size_t chunk : {64u, 256u, 4096u}) {
    CHECK_EQ((long long)run_load(5000, 4, 5000, 2, chunk), 5000);   // exact fill, convert
    CHECK_EQ((long long)run_load(7000, 4, 5000, 2, chunk), 5000);   // truncate, convert
    CHECK_EQ((long long)run_load(5000, 4, 5000, 4, chunk), 5000);   // exact fill, match
    CHECK_EQ((long long)run_load(3000, 2, 5000, 4, chunk), 3000);   // short file, convert
  }

  // --- format/channel conversion on load (the widened granular gate) ------------------------------

  // Generalized driver: any source format/channel count -> the buffer's stereo native frames, fed in
  // `chunk`-byte blocks exactly as card.cpp reads them off the card.
  auto run_conv = [](size_t file_frames, PcmFormat sf, uint16_t sch, size_t cap_frames,
                     PcmFormat df, uint16_t dch, size_t chunk,
                     std::vector<uint8_t>* out_dst = nullptr, std::vector<uint8_t>* out_file = nullptr) {
    const size_t src_frame = (size_t)pcm_bytes(sf) * sch, dst_frame = (size_t)pcm_bytes(df) * dch;
    std::vector<uint8_t> file(file_frames * src_frame);
    for (size_t i = 0; i < file.size(); i++) file[i] = (uint8_t)((i * 131 + 7) & 0xFF);
    std::vector<uint8_t> dst(cap_frames * dst_frame, 0);

    PcmLoader L;
    L.begin(file.size(), sf, sch, dst.data(), dst.size(), df, dch);
    size_t pos = 0;
    while (true) {
      size_t n = (file.size() - pos < chunk) ? (file.size() - pos) : chunk;
      bool full = L.feed(file.data() + pos, n);
      pos += n;
      if (n < chunk || full) break;
    }
    if (out_dst) *out_dst = std::move(dst);
    if (out_file) *out_file = std::move(file);
    return L.frames();
  };

  // Frame counts hold for every source shape, including the odd frame sizes (3 and 6 bytes) that no
  // power-of-two chunk divides.
  CHECK_EQ((long long)run_conv(5000, PcmFormat::i24, 2, 8000, PcmFormat::f32, 2, K), 5000);
  CHECK_EQ((long long)run_conv(5000, PcmFormat::i24, 1, 8000, PcmFormat::f32, 2, K), 5000);
  CHECK_EQ((long long)run_conv(5000, PcmFormat::u8,  1, 8000, PcmFormat::f32, 2, K), 5000);
  CHECK_EQ((long long)run_conv(5000, PcmFormat::i32, 2, 8000, PcmFormat::f32, 2, K), 5000);
  CHECK_EQ((long long)run_conv(9000, PcmFormat::i24, 2, 5000, PcmFormat::f32, 2, K), 5000);  // truncated to capacity

  // The straddle case that motivates the carry: a 6-byte frame across chunk sizes that leave a partial
  // frame at nearly every boundary. The result must be byte-identical to converting the file in one go
  // - i.e. no sample is smeared by the leftover bytes of the previous chunk.
  for (size_t chunk : {7u, 64u, 100u, 4096u, 32768u}) {
    std::vector<uint8_t> dst, file;
    const size_t N = 3000;
    const size_t got = run_conv(N, PcmFormat::i24, 2, N, PcmFormat::f32, 2, chunk, &dst, &file);
    CHECK_EQ((long long)got, (long long)N);
    std::vector<uint8_t> expect(N * 2 * 4);
    convert_pcm_frames(file.data(), N, PcmFormat::i24, 2, expect.data(), PcmFormat::f32, 2);
    CHECK(std::memcmp(dst.data(), expect.data(), expect.size()) == 0);
  }

  // Mono -> the stereo loop buffer: both channels carry the same value, and the frame count is the
  // file's frame count (not its sample count).
  {
    std::vector<uint8_t> dst, file;
    const size_t N = 512;
    CHECK_EQ((long long)run_conv(N, PcmFormat::i16, 1, N, PcmFormat::f32, 2, 100, &dst, &file), (long long)N);
    const float* f = reinterpret_cast<const float*>(dst.data());
    bool paired = true;
    for (size_t i = 0; i < N; i++) if (f[i * 2] != f[i * 2 + 1]) paired = false;
    CHECK(paired);
    int16_t s0; std::memcpy(&s0, file.data(), 2);
    CHECK_NEAR(f[0], i16_to_float(s0), 1e-6);
  }

  // --- resampling on load (the granular rate gate) ------------------------------------------------

  // Rate-converting driver: the buffer is always filled at the DEVICE rate, which is the whole point -
  // a 44.1 kHz file becomes the same number of buffer frames as a take recorded on the device, so
  // nothing downstream (loop length, tempo, ticks) sees the difference.
  auto run_rate = [](size_t file_frames, uint32_t src_rate, uint32_t dst_rate, size_t cap_frames,
                     size_t chunk, std::vector<float>* out_samples = nullptr) {
    const size_t src_frame = 4, dst_frame = 8;   // f32 mono in -> f32 stereo buffer out
    std::vector<uint8_t> file(file_frames * src_frame);
    for (size_t i = 0; i < file_frames; i++) {
      const float v = static_cast<float>(i) / static_cast<float>(file_frames);
      std::memcpy(file.data() + i * src_frame, &v, 4);
    }
    std::vector<uint8_t> dst(cap_frames * dst_frame, 0);
    PcmLoader L;
    L.begin(file.size(), PcmFormat::f32, 1, dst.data(), dst.size(), PcmFormat::f32, 2, src_rate, dst_rate);
    size_t pos = 0;
    while (true) {
      size_t n = (file.size() - pos < chunk) ? (file.size() - pos) : chunk;
      bool full = L.feed(file.data() + pos, n);
      pos += n;
      if (n < chunk || full) break;
    }
    if (out_samples) {
      out_samples->resize(L.frames() * 2);
      std::memcpy(out_samples->data(), dst.data(), out_samples->size() * 4);
    }
    return L.frames();
  };

  // 1 s of source at any rate fills 1 s of buffer, to within the interpolator's final partial step.
  for (size_t chunk : {32u, 512u, 32768u}) {
    CHECK((long long)run_rate(44100, 44100, 48000, 96000, chunk) >= 47990);
    CHECK((long long)run_rate(44100, 44100, 48000, 96000, chunk) <= 48000);
    CHECK((long long)run_rate(96000, 96000, 48000, 96000, chunk) >= 47990);
    CHECK((long long)run_rate(96000, 96000, 48000, 96000, chunk) <= 48000);
    CHECK((long long)run_rate(24000, 24000, 48000, 96000, chunk) >= 47990);
  }

  // A ratio above 2 must SKIP source frames, not emit every one (the phase stays >= 1 for several
  // frames in a row, which is how the skipping happens).
  CHECK((long long)run_rate(3000, 144000, 48000, 8000, 512) <= 1001);
  CHECK((long long)run_rate(3000, 144000, 48000, 8000, 512) >= 999);

  // No rate change is exact and untouched, whatever the chunk size.
  CHECK_EQ((long long)run_rate(5000, 48000, 48000, 5000, 512), 5000);

  // The resampled ramp stays monotone across every chunk boundary: a reset interpolator would show up
  // as a step backwards at the seam.
  {
    std::vector<float> got;
    run_rate(4000, 32000, 48000, 8000, 64, &got);
    bool monotone = true;
    for (size_t i = 2; i < got.size(); i += 2) if (got[i] < got[i - 2] - 1e-6f) monotone = false;
    CHECK(monotone);
    // Mono source duplicated into the stereo buffer: both channels of a frame are equal.
    bool paired = true;
    for (size_t i = 0; i + 1 < got.size(); i += 2) if (got[i] != got[i + 1]) paired = false;
    CHECK(paired);
  }

  // Placement / integrity: a converted load must equal a one-shot convert_pcm_block of the
  // whole file (proves chunked offset advancement writes contiguously, no gaps/overlap).
  {
    std::vector<uint8_t> dst, file;
    size_t frames = run_load(20000, 4, 20000, 2, 4096, &dst, &file);
    CHECK_EQ((long long)frames, 20000);

    std::vector<uint8_t> expect(20000 * 2 * 2);
    convert_pcm_block(file.data(), 20000 * 2, 4, expect.data(), 2);
    CHECK(std::memcmp(dst.data(), expect.data(), expect.size()) == 0);
  }
}
