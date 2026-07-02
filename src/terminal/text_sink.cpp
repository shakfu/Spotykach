// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "engine/terminal_io.h"

#if SPK_TERMINAL

#include <cstring>   // strlen
#include <cmath>     // isnan, isinf

// Reply formatting for the terminal channel. The firmware does not link _printf_float (the METER path
// formats floats by integer decomposition too), so floats are formatted manually here - never "%f".
#pragma GCC optimize("Os")

namespace spotykach {

namespace {

// Append an unsigned integer's decimal digits to a caller buffer, return the count written.
size_t u32_to_dec(uint32_t v, char* buf) {
    char tmp[10];
    int n = 0;
    do { tmp[n++] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v);
    for (int i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];   // reverse
    return static_cast<size_t>(n);
}

}  // namespace

void TextSink::str(const char* s) { _out.write(s, std::strlen(s)); }

void TextSink::line(const char* s) { _out.write(s, std::strlen(s)); _out.write("\r\n", 2); }

void TextSink::ok() { _out.write("ok\r\n", 4); }

void TextSink::append_i32(int32_t v) {
    char buf[12];
    size_t n = 0;
    uint32_t mag;
    if (v < 0) { buf[n++] = '-'; mag = static_cast<uint32_t>(-(int64_t)v); }
    else       { mag = static_cast<uint32_t>(v); }
    n += u32_to_dec(mag, buf + n);
    _out.write(buf, n);
}

void TextSink::append_hex(uint32_t v) {
    static const char kHex[] = "0123456789abcdef";
    char buf[10];
    buf[0] = '0'; buf[1] = 'x';
    size_t n = 2;
    bool started = false;
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t nib = (v >> shift) & 0xF;
        if (nib || started || shift == 0) { buf[n++] = kHex[nib]; started = true; }
    }
    _out.write(buf, n);
}

void TextSink::append_f32(float v, int decimals) {
    if (std::isnan(v)) { str("nan"); return; }
    if (std::isinf(v)) { str(v < 0 ? "-inf" : "inf"); return; }
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;

    char buf[24];
    size_t n = 0;
    if (v < 0.f) { buf[n++] = '-'; v = -v; }

    // Split into integer and fractional parts; derive the integer part first so magnitude never
    // overflows the uint32 scaling (params are 0..1, tempo <= 300, but stay robust for any value).
    uint32_t ipart = static_cast<uint32_t>(v);
    float    frac  = v - static_cast<float>(ipart);

    uint32_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    uint32_t fpart = static_cast<uint32_t>(frac * static_cast<float>(scale) + 0.5f);
    if (fpart >= scale) { fpart -= scale; ipart += 1; }   // rounding carry into the integer part

    n += u32_to_dec(ipart, buf + n);
    if (decimals > 0) {
        buf[n++] = '.';
        // zero-pad the fractional part to exactly `decimals` digits
        uint32_t p = scale / 10;
        while (p > 1 && fpart < p) { buf[n++] = '0'; p /= 10; }   // leading zeros of the fraction
        n += u32_to_dec(fpart, buf + n);                          // always >=1 digit (fpart may be 0)
    }
    _out.write(buf, n);
}

void TextSink::ok_i32(int32_t v)   { _out.write("ok ", 3); append_i32(v); _out.write("\r\n", 2); }
void TextSink::ok_f32(float v, int d) { _out.write("ok ", 3); append_f32(v, d); _out.write("\r\n", 2); }
void TextSink::ok_hex(uint32_t v)  { _out.write("ok ", 3); append_hex(v); _out.write("\r\n", 2); }

void TextSink::err(const char* reason) {
    _out.write("err ", 4);
    _out.write(reason, std::strlen(reason));
    _out.write("\r\n", 2);
}

}  // namespace spotykach

#endif  // SPK_TERMINAL
