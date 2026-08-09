// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/osc.h"

#if SPK_TERMINAL_OSC

#include "terminal/osc_sink.h"
#include "terminal/slip.h"

#include <cstring>

#pragma GCC optimize("Os")

namespace spotykach {

namespace {

void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// A scratch ITextOut, so OscSink can reuse TextSink's number formatting WITHOUT its output reaching
// the wire. This matters: the base append_* write straight to the byte sink, and on the OSC side that
// sink carries SLIP frames - raw ASCII landing in it mid-frame is exactly the corruption SLIP exists
// to prevent. The text form is kept only in case the reply turns out to degrade to a string.
struct Scratch : ITextOut {
    char   b[48];
    size_t n = 0;
    Scratch() { b[0] = '\0'; }
    void write(const char* s, size_t c) override {
        for (size_t i = 0; i < c && n < sizeof b - 1; ++i) b[n++] = s[i];
        b[n] = '\0';
    }
};

}  // namespace

// --- OscWriter ---------------------------------------------------------------------------------------

void OscWriter::raw(const void* p, size_t n) {
    if (_over) return;
    if (_len + n > _cap) { _over = true; return; }
    std::memcpy(_buf + _len, p, n);
    _len += n;
}

void OscWriter::pad() {
    if (_over) return;
    while (_len & 3u) {
        if (_len >= _cap) { _over = true; return; }
        _buf[_len++] = 0;
    }
}

bool OscWriter::begin(const char* address, const char* tags) {
    _len = 0; _over = false;
    raw(address, std::strlen(address) + 1);
    pad();
    char tagbuf[OscMessage::kMaxArgs + 8];
    tagbuf[0] = ',';
    const size_t n = std::strlen(tags);
    if (n + 2 > sizeof tagbuf) { _over = true; return false; }
    std::memcpy(tagbuf + 1, tags, n + 1);
    raw(tagbuf, n + 2);
    pad();
    return !_over;
}

void OscWriter::i32(int32_t v) {
    uint8_t b[4];
    put_be32(b, static_cast<uint32_t>(v));
    raw(b, 4);
}

void OscWriter::f32(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    uint8_t b[4];
    put_be32(b, bits);
    raw(b, 4);
}

void OscWriter::str(const char* s) {
    raw(s, std::strlen(s) + 1);
    pad();
}

// --- OscBundleWriter ---------------------------------------------------------------------------------

void OscBundleWriter::open() {
    if (_cap < 16) { _over = true; return; }
    std::memcpy(_buf, "#bundle", 8);       // includes the NUL; 8 bytes exactly, already aligned
    std::memset(_buf + 8, 0, 8);
    _buf[15] = 1;                          // timetag "immediately"
    _len = 16;
}

OscWriter OscBundleWriter::element() {
    if (_over || _len + 4 > _cap) { _over = true; return OscWriter(_buf, 0); }
    // The element is written straight into place, past the 4 bytes its size prefix will occupy.
    return OscWriter(_buf + _len + 4, _cap - _len - 4);
}

void OscBundleWriter::close_element(const OscWriter& w) {
    if (_over) return;
    if (!w.ok()) { _over = true; return; }
    put_be32(_buf + _len, static_cast<uint32_t>(w.size()));
    _len += 4 + w.size();
}

// --- OscSink -----------------------------------------------------------------------------------------

void OscSink::begin_request(const char* request_address, bool ack_mode) {
    _request  = request_address;
    _ack      = ack_mode;
    _expect   = true;
    _kind     = Kind::None;
    _i        = 0;
    _f        = 0.f;
    _reason   = "";
    _text_len = 0;
    _armed    = false;
    _values   = 0;
    _raw      = false;
}

void OscSink::put(const char* s, size_t n) {
    // Silent truncation is correct here: the text branch is a fallback for free-form replies, and a
    // clipped diagnostic string is a far better outcome than refusing to answer at all.
    for (size_t i = 0; i < n && _text_len < kTextMax - 1; ++i) _text[_text_len++] = s[i];
    _text[_text_len] = '\0';
}

void OscSink::note_value(Kind k) {
    if (!_armed) {
        // Outside an ok_begin/ok_end frame the handler is composing free-form text - `pad play`'s
        // "ok empty=1", `fx gritmode`'s two-value line. A value appended there must NOT promote the
        // reply to that value's type: the literals around it carry meaning the tag would drop.
        if (_kind == Kind::None) _kind = k;
        else if (_kind != k)     _kind = Kind::Text;
        return;
    }
    ++_values;
    if (_values == 1) _kind = k;
    else              _kind = Kind::Text;   // more than one value: no single tag can carry it
}

void OscSink::str(const char* s) {
    if (_armed) _raw = true;
    else if (_kind == Kind::None) _kind = Kind::Text;
    put(s, std::strlen(s));
}

void OscSink::line(const char* s) {
    if (_kind == Kind::None) _kind = Kind::Text;
    put(s, std::strlen(s));
}

void OscSink::ok()               { if (_kind == Kind::None) _kind = Kind::Ok; }
void OscSink::ok_i32(int32_t v)  { _kind = Kind::Int;   _i = v; }
void OscSink::ok_f32(float v, int) { _kind = Kind::Float; _f = v; }
// caps is a bitmask: an int on the wire, not the "0x..." text the line codec prints, so a host reads it
// with a bitwise AND instead of parsing hex out of a string.
void OscSink::ok_hex(uint32_t v) { _kind = Kind::Int;   _i = static_cast<int32_t>(v); }

void OscSink::err(const char* reason) { _kind = Kind::Err; _reason = reason; }

// Each of these records the typed value AND its text form: the type is what the reply uses when the
// handler produced exactly one value, the text is the fallback when it produced something else.
void OscSink::append_i32(int32_t v) {
    note_value(Kind::Int);
    _i = v;
    Scratch s; TextSink(s).append_i32(v); put(s.b, s.n);
}

void OscSink::append_f32(float v, int decimals) {
    note_value(Kind::Float);
    _f = v;
    Scratch s; TextSink(s).append_f32(v, decimals); put(s.b, s.n);
}

void OscSink::append_hex(uint32_t v) {
    note_value(Kind::Int);
    _i = static_cast<int32_t>(v);
    Scratch s; TextSink(s).append_hex(v); put(s.b, s.n);
}

void OscSink::ok_begin() { _armed = true; _values = 0; _raw = false; }

void OscSink::ok_end() {
    _armed = false;
    // Exactly one typed value and nothing else -> that value's tag. Anything else is a string.
    if (_raw || _values != 1) _kind = Kind::Text;
}

void OscSink::send_packet(const uint8_t* p, size_t n) {
    // A SLIP frame is assembled from many small writes, so a FIFO that fills partway through would put
    // a CORRUPT frame on the wire rather than simply losing a reply. Check the worst case up front -
    // every byte escaped, plus the two ENDs - and refuse the whole frame if it cannot fit.
    if (out().writable() < slip_encoded_size(p, n)) {
        // Not send_packet(): recursing here with a full FIFO would corrupt the error frame too. The
        // error is small enough that the reserve almost always covers it, and if it does not, the
        // transport's own tx-overflow latch is the backstop.
        uint8_t buf[128];
        OscWriter w(buf, sizeof buf);
        w.begin("/sk/err", "ss");
        w.str(_request);
        w.str("overflow");
        if (w.ok() && out().writable() >= slip_encoded_size(w.data(), w.size()))
            slip_encode(w.data(), w.size(), [this](const char* b, size_t c) { out().write(b, c); });
        return;
    }
    slip_encode(p, n, [this](const char* b, size_t c) { out().write(b, c); });
}

void OscSink::emit_error(const char* request_address, const char* reason) {
    uint8_t buf[128];
    OscWriter w(buf, sizeof buf);
    w.begin("/sk/err", "ss");
    w.str(request_address);
    w.str(reason);
    if (w.ok()) send_packet(w.data(), w.size());
}

void OscSink::finish() {
    if (_kind == Kind::Err) { emit_error(_request, _reason); return; }

    // A successful write is silent by default - that is the one behavioural difference from the line
    // codec, and the reason a 100 Hz fader stream costs nothing on the return path.
    if (!_expect || _kind == Kind::None || _kind == Kind::Ok) {
        if (_ack && _kind != Kind::None) {
            uint8_t buf[128];
            OscWriter w(buf, sizeof buf);
            w.begin("/sk/reply/ok", "s");
            w.str(_request);
            if (w.ok()) send_packet(w.data(), w.size());
        }
        return;
    }

    // Replies mirror the request path under /sk/reply, so a patch routes an answer on the address it
    // sent and several outstanding reads need no sequence tag.
    char addr[96];
    const size_t rn = std::strlen(_request);
    static const char kPrefix[] = "/sk/reply";
    const size_t pn = sizeof kPrefix - 1;
    // Every address this codec accepts starts with "/sk"; the reply is that path re-rooted.
    const char* tail    = (rn >= 3 && !std::strncmp(_request, "/sk", 3)) ? _request + 3 : _request;
    const size_t tail_n = std::strlen(tail);
    if (pn + tail_n + 1 > sizeof addr) { emit_error(_request, "bad-packet"); return; }
    std::memcpy(addr, kPrefix, pn);
    std::memcpy(addr + pn, tail, tail_n + 1);

    uint8_t buf[320];
    OscWriter w(buf, sizeof buf);
    switch (_kind) {
        case Kind::Int:   w.begin(addr, "i"); w.i32(_i); break;
        // NaN from cpumin/cpumax inside the post-`reset cpu` gap goes out as an IEEE NaN rather than
        // being coerced to zero: "no sample yet" is not "no load", and the float tag can say so.
        case Kind::Float: w.begin(addr, "f"); w.f32(_f); break;
        default: {
            // Trim the CRLF the line grammar appends; an OSC string carries no framing of its own.
            while (_text_len && (_text[_text_len - 1] == '\n' || _text[_text_len - 1] == '\r'))
                _text[--_text_len] = '\0';
            w.begin(addr, "s");
            w.str(_text);
            break;
        }
    }
    if (w.ok()) send_packet(w.data(), w.size());
    else        emit_error(_request, "bad-packet");
}

}  // namespace spotykach

#endif  // SPK_TERMINAL_OSC
