#pragma once
// Shared decoding scaffolding for the off-target OSC tests (test_terminal_osc.cpp, test_osc_labels.cpp).
// The host half of the wire: pull SLIP frames out of a byte stream, then render OSC messages as one
// comparable string.
//
// The SLIP decoder here is deliberately NOT SlipAssembler. That is the device's RX path, bounded at
// 512 B because that bounds the INBOUND direction; the describe bundle travelling the other way is an
// order of magnitude larger, and a host has to be able to receive what the device can send.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/terminal_io.h"
#include "terminal/osc.h"
#include "terminal/slip.h"

namespace osctest {

// Collects reply bytes so a whole exchange can be decoded after the fact.
struct StringOut : spotykach::ITextOut {
    std::string s;
    void write(const char* p, size_t n) override { s.append(p, n); }
};

// Unbounded SLIP frame extraction.
inline std::vector<std::string> slip_frames(const std::string& stream) {
    namespace slip = spotykach::slip;
    std::vector<std::string> frames;
    std::string cur;
    bool escaped = false;
    for (unsigned char b : stream) {
        if (b == slip::kEnd) {
            if (!cur.empty()) frames.push_back(cur);
            cur.clear();
            escaped = false;
        } else if (escaped) {
            cur.push_back(char(b == slip::kEscEnd ? slip::kEnd : b == slip::kEscEsc ? slip::kEsc : b));
            escaped = false;
        } else if (b == slip::kEsc) {
            escaped = true;
        } else {
            cur.push_back(char(b));
        }
    }
    return frames;
}

// One decoded message: address plus a compact rendering of its arguments.
struct Reply {
    std::string addr;
    std::string args;
    std::string all() const { return addr.empty() ? "" : addr + " " + args; }
};

inline std::string render_args(const spotykach::OscMessage& m) {
    using spotykach::OscType;
    std::string r;
    for (uint8_t i = 0; i < m.argc(); ++i) {
        if (i) r += " ";
        const spotykach::OscArg& a = m.arg(i);
        char buf[64];
        switch (a.type) {
            case OscType::Int32:  std::snprintf(buf, sizeof buf, ",i %d", a.i); break;
            case OscType::Float:
                if (std::isnan(a.f)) std::snprintf(buf, sizeof buf, ",f nan");
                else                 std::snprintf(buf, sizeof buf, ",f %.4f", a.f);
                break;
            case OscType::String: std::snprintf(buf, sizeof buf, ",s %s", a.s); break;
            case OscType::True:   std::snprintf(buf, sizeof buf, ",T"); break;
            case OscType::False:  std::snprintf(buf, sizeof buf, ",F"); break;
            default:              std::snprintf(buf, sizeof buf, ",? "); break;
        }
        r += buf;
    }
    return r;
}

// Decode every frame in a stream as one message each (the reply path).
inline std::vector<Reply> decode_replies(const std::string& stream) {
    std::vector<Reply> out;
    for (const auto& f : slip_frames(stream)) {
        spotykach::OscMessage m;
        Reply r;
        if (m.parse(reinterpret_cast<const uint8_t*>(f.data()), f.size())) {
            r.addr = m.address(); r.args = render_args(m);
        } else {
            r.addr = "<unparseable>";
        }
        out.push_back(r);
    }
    return out;
}

// Decode a stream whose frames may be bundles, flattening bundle elements into rows - what a host does
// with `describe`. `bundle_bytes`, when given, receives the size of the last frame seen.
inline std::vector<Reply> decode_rows(const std::string& stream, size_t* bundle_bytes = nullptr) {
    std::vector<Reply> rows;
    for (const auto& f : slip_frames(stream)) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(f.data());
        if (bundle_bytes) *bundle_bytes = f.size();
        if (!spotykach::osc_is_bundle(p, f.size())) {
            spotykach::OscMessage m;
            Reply r;
            if (m.parse(p, f.size())) { r.addr = m.address(); r.args = render_args(m); }
            else                      { r.addr = "<unparseable>"; }
            rows.push_back(r);
            continue;
        }
        size_t cursor = 0;
        const uint8_t* el; size_t el_len;
        while (spotykach::osc_bundle_next(p, f.size(), cursor, el, el_len)) {
            spotykach::OscMessage m;
            Reply r;
            if (m.parse(el, el_len)) { r.addr = m.address(); r.args = render_args(m); }
            else                     { r.addr = "<unparseable>"; }
            rows.push_back(r);
        }
    }
    return rows;
}

}  // namespace osctest
