// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Layer [1]/[2] seam: accumulates RX bytes into a bounded, NUL-terminated line buffer, trimming '\r'
// and completing on '\n'. A bounded buffer with overflow -> `err line-too-long` prevents an
// unterminated flood from growing without bound. See docs/dev/terminal-transport.md.
//
// On overflow the assembler swallows the rest of the over-long line (up to and including its '\n')
// so its truncated tail is never re-parsed as a bogus command; it reports TooLong at that '\n'.

#include <cstddef>
#include <cstdint>

namespace spotykach {

class LineAssembler {
  public:
    enum class Feed : uint8_t {
        Pending,   // byte consumed, line not complete
        Ready,     // '\n' seen: line() is a complete NUL-terminated line
        TooLong,   // an over-long line terminated -> caller emits err line-too-long
    };

    Feed feed(uint8_t b) {
        if (_overflowed) {                     // discarding a too-long line until its terminator
            if (b == '\n') { _overflowed = false; _len = 0; return Feed::TooLong; }
            return Feed::Pending;
        }
        if (b == '\r') return Feed::Pending;   // trim CR (accept CRLF and bare LF)
        if (b == '\n') { _buf[_len] = '\0'; return Feed::Ready; }
        if (_len >= kMax) { _overflowed = true; return Feed::Pending; }   // begin swallowing
        _buf[_len++] = static_cast<char>(b);
        return Feed::Pending;
    }

    char*  line() { return _buf; }
    size_t len() const { return _len; }
    void   reset() { _len = 0; _overflowed = false; }

  private:
    static constexpr size_t kMax = 128;   // matches the transport's bounded line budget
    char   _buf[kMax + 1];                // +1 for the NUL terminator
    size_t _len = 0;
    bool   _overflowed = false;
};

}  // namespace spotykach
