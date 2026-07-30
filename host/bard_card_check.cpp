// Validate a prepared bard card against the FIRMWARE's own rules, before you put it in the module.
//
// "Will the device see this card?" is a recurring question for a card-based engine, and the failure mode is
// silent: a filename one character too long simply does not appear, and the shelf reads as empty with no
// diagnostic. This tool answers it definitively by applying the same filters `StreamDeck::scan_bank` does
// and parsing each sidecar with the real `src/engine/bard/bookmarks.h` - so it cannot drift from the
// firmware the way a re-implementation in the prep script could.
//
//   make -C host bard-card-check
//   ./host/build/bard_card_check <card-root-or-shelf-dir> [...]
//
// Checks per shelf: the 8.3 name limit, the .raw/.wav extension filter, the 32 KB minimum, the 16-bit-mono
// WAV header, the per-shelf book cap, and then per book: the sidecar's mark count, ordering, spans, and
// whether any mark falls outside the audio.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include "engine/bard/bookmarks.h"

using namespace spotykach;

namespace {

int g_problems = 0;
int g_warnings = 0;

void problem(const std::string& what) { std::printf("    PROBLEM: %s\n", what.c_str()); g_problems++; }
void warn(const std::string& what)    { std::printf("    warning: %s\n", what.c_str()); g_warnings++; }

// Mirrors the engine's limits (src/engine/bard/bard_engine.h, src/hw/stream_deck.cpp).
constexpr int      kMaxBooks   = 32;
constexpr int      kMaxNameLen = 12;
constexpr uint32_t kMinBytes   = 32u * 1024u;
constexpr int      kSidecarMax = 4096;

bool ends_with_ci(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++) {
        char a = s[s.size() - n + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
        if (a != b) return false;
    }
    return true;
}

// Read a 16-bit-mono PCM WAV's rate and frame count the way RawStreamReader::begin_wav does. Returns false
// if the file is not a shape the engine can play.
bool wav_info(const std::string& path, uint32_t& rate, uint32_t& frames, std::string& why) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { why = "cannot open"; return false; }
    unsigned char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) {
        std::fclose(f); why = "not a RIFF/WAVE file"; return false;
    }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t data = 0;
    bool have_fmt = false, have_data = false;
    while (!(have_fmt && have_data)) {
        unsigned char c[8];
        if (std::fread(c, 1, 8, f) != 8) break;
        const uint32_t sz = c[4] | (c[5] << 8) | (c[6] << 16) | (static_cast<uint32_t>(c[7]) << 24);
        if (!std::memcmp(c, "fmt ", 4)) {
            std::vector<unsigned char> b(sz < 16 ? 16 : sz, 0);
            if (std::fread(b.data(), 1, sz, f) != sz) break;
            fmt  = static_cast<uint16_t>(b[0] | (b[1] << 8));
            ch   = static_cast<uint16_t>(b[2] | (b[3] << 8));
            rate = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24);
            bits = static_cast<uint16_t>(b[14] | (b[15] << 8));
            have_fmt = true;
            if (sz & 1) std::fseek(f, 1, SEEK_CUR);
        } else if (!std::memcmp(c, "data", 4)) {
            data = sz; have_data = true;
        } else {
            std::fseek(f, static_cast<long>(sz + (sz & 1)), SEEK_CUR);
        }
    }
    std::fclose(f);
    if (!have_fmt || !have_data) { why = "no fmt/data chunk"; return false; }
    if (fmt != 1)    { why = "not PCM (fmt " + std::to_string(fmt) + ")"; return false; }
    if (ch != 1)     { why = std::to_string(ch) + " channels - the engine plays MONO only"; return false; }
    if (bits != 16)  { why = std::to_string(bits) + "-bit - the engine plays 16-bit only"; return false; }
    frames = data / 2u;
    return true;
}

std::string read_file(const std::string& path, int cap) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string s;
    s.resize(static_cast<size_t>(cap));
    const size_t got = std::fread(&s[0], 1, static_cast<size_t>(cap), f);
    std::fclose(f);
    s.resize(got);
    return s;
}

bool is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string fmt_hms(uint32_t frames, uint32_t rate) {
    char buf[32];
    const int n = bard::format_time(frames, rate, buf);
    return std::string(buf, static_cast<size_t>(n));
}

void check_shelf(const std::string& dir) {
    std::printf("shelf %s\n", dir.c_str());
    DIR* d = opendir(dir.c_str());
    if (!d) { problem("cannot open the directory"); return; }

    std::vector<std::string> books;
    int skipped = 0;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        if (is_dir(full)) continue;

        const bool audio = ends_with_ci(name, ".wav") || ends_with_ci(name, ".raw");
        if (name[0] == '.') {
            if (audio) { warn("hidden companion '" + name + "' ignored (run dot_clean on the card)"); }
            continue;
        }
        if (!audio) continue;                                   // sidecars / BOOKS.TXT: not books

        if (name.size() > static_cast<size_t>(kMaxNameLen)) {
            problem("'" + name + "' is " + std::to_string(name.size()) +
                    " chars - the engine's scan skips anything over 12, so this book is INVISIBLE");
            skipped++;
            continue;
        }
        struct stat st;
        if (stat(full.c_str(), &st) != 0) { problem("cannot stat '" + name + "'"); continue; }
        if (static_cast<uint32_t>(st.st_size) < kMinBytes) {
            problem("'" + name + "' is " + std::to_string(st.st_size) +
                    " B - under the 32 KB minimum, so the scan skips it");
            skipped++;
            continue;
        }
        books.push_back(name);
    }
    closedir(d);

    if (books.empty()) {
        problem(skipped ? "no book on this shelf survives the scan (see above) - it will read as EMPTY"
                        : "no .wav/.raw books on this shelf");
        return;
    }
    if (static_cast<int>(books.size()) > kMaxBooks)
        problem("shelf holds " + std::to_string(books.size()) + " books but only the first " +
                std::to_string(kMaxBooks) + " are indexed");

    // The engine sorts case-insensitively (bank_sort), so the BOOK knob follows this order.
    std::sort(books.begin(), books.end(), [](const std::string& a, const std::string& b) {
        std::string x = a, y = b;
        for (auto& c : x) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        for (auto& c : y) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return x < y;
    });

    for (size_t i = 0; i < books.size(); i++) {
        const std::string& name = books[i];
        const std::string full = dir + "/" + name;
        std::printf("  [%zu] %s\n", i, name.c_str());

        uint32_t rate = 48000, frames = 0;
        if (ends_with_ci(name, ".wav")) {
            std::string why;
            if (!wav_info(full, rate, frames, why)) { problem("unplayable WAV: " + why); continue; }
        } else {
            struct stat st; stat(full.c_str(), &st);
            frames = static_cast<uint32_t>(st.st_size) / 2u;      // headerless: rate comes from bard.cfg
            warn("headerless .raw - its rate comes from bard.cfg's rate= (default 48000), not the file");
        }
        std::printf("      %u Hz mono, %s (%.1f MiB)\n", rate, fmt_hms(frames, rate).c_str(),
                    static_cast<double>(frames) * 2.0 / 1048576.0);
        if (static_cast<double>(frames) * 2.0 > 4.0 * 1024 * 1024 * 1024)
            problem("over FAT32's 4 GB per-file limit");

        // The sidecar the engine will look for: NAME.txt beside NAME.wav.
        const std::string stem = name.substr(0, name.rfind('.'));
        std::string side = dir + "/" + stem + ".txt";
        if (!read_file(side, 1).size()) side = dir + "/" + stem + ".TXT";
        const std::string text = read_file(side, kSidecarMax + 1);
        if (text.empty()) {
            std::printf("      no sidecar - the engine will generate deterministic auto-marks\n");
            continue;
        }
        if (static_cast<int>(text.size()) > kSidecarMax)
            problem("sidecar is over 4096 B; read_text truncates silently, so its tail is LOST on device");

        bard::MarkList l;
        const std::string capped = text.substr(0, static_cast<size_t>(kSidecarMax));
        bard::parse_sidecar(capped.c_str(), rate, frames, l);
        if (l.count == 0) {
            problem("sidecar present but no mark parsed - the engine will fall back to auto-marks");
            continue;
        }
        bard::resolve(l, frames, bard::book_seed(name.c_str(), frames, 0));
        std::printf("      %d mark(s) parsed, order=%s loop=%s\n", l.count,
                    l.ordering == bard::MarkOrder::Time ? "time"
                    : l.ordering == bard::MarkOrder::Shuffle ? "shuffle" : "file",
                    !l.loop_set ? "(default hold)"
                    : l.loop == bard::LoopMode::Segment ? "segment"
                    : l.loop == bard::LoopMode::Book ? "book" : "off");
        if (l.count >= bard::MarkList::kMax)
            warn("at the 64-mark cap - any further marks in the file were dropped");

        int out_of_range = 0, zero_len = 0;
        for (int k = 0; k < l.count; k++) {
            if (frames && l.mark[k].start >= frames) out_of_range++;
            if (l.mark[k].end <= l.mark[k].start) zero_len++;
        }
        if (out_of_range) problem(std::to_string(out_of_range) + " mark(s) start past the end of the audio");
        if (zero_len)     problem(std::to_string(zero_len) + " zero-length segment(s)");
        std::printf("      first %s, last %s\n",
                    fmt_hms(l.mark[0].start, rate).c_str(),
                    fmt_hms(l.mark[l.count - 1].start, rate).c_str());
    }
}

} // namespace

#include <algorithm>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <card-root-or-shelf-dir> [...]\n", argv[0]);
        return 2;
    }
    for (int a = 1; a < argc; a++) {
        std::string root = argv[a];
        while (root.size() > 1 && root.back() == '/') root.pop_back();

        // Accept a card root (containing bard/0..15), a /bard dir, or a single shelf dir.
        std::vector<std::string> shelves;
        const std::string bard = is_dir(root + "/bard") ? root + "/bard" : root;
        for (int s = 0; s < 16; s++) {
            const std::string d = bard + "/" + std::to_string(s);
            if (is_dir(d)) shelves.push_back(d);
        }
        if (shelves.empty()) shelves.push_back(root);

        const std::string cfg = read_file(bard + "/bard.cfg", 512);
        if (!cfg.empty()) {
            bard::Config c;
            bard::parse_config(cfg.c_str(), c);
            std::printf("bard.cfg: resume=%s rate=%u\n", c.resume ? "on" : "off", c.rate);
        } else {
            std::printf("no bard.cfg (defaults: resume=on, rate=48000 for headerless .raw)\n");
        }
        for (const auto& s : shelves) check_shelf(s);
    }
    if (g_problems == 0) {
        std::printf("\nOK: the card is playable%s\n",
                    g_warnings ? " (with warnings above)" : "");
        return 0;
    }
    std::printf("\nFAILED: %d problem(s), %d warning(s)\n", g_problems, g_warnings);
    return 1;
}
