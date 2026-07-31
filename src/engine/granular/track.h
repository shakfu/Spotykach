#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include <inttypes.h>
#include <array>
#include <functional>
#include "event.h"
#include "nocopy.h"

namespace spotykach {

class Track {
public:
    static constexpr uint16_t kSliceLength = 16;
    static constexpr uint16_t kSlicesCount = 8;
    static constexpr uint16_t kLength = kSlicesCount * kSliceLength;

    int ref = -1;

    Track();
    ~Track() = default;

    void init(Event* buf);

    void tick(const bool is_key);
    Event* event() const { return _current_event; }
    
    void add_event(const Event*);
    void add_p1(const float);
    void add_p2(const float);
    void add_p3(const float);
    void add_p4(const float);
    
    void arm(const bool auto_start);
    void disarm(const bool force = false);
    bool is_armed() const { return _is_armed; };
    bool is_recording() const { return _is_recording; }
    
    void set_clearing(const bool value) { _is_clearing = value; }
    bool is_empty() const { return _is_empty; }
    void rewind();
    void clear();

private:
    NOCOPY(Track)

    void _start_recording();
    void _stop_recording();

    void _clear(uint16_t slot) { _slice[slot]->on = false; }
    void _make_slice();
    void _advance_write_slot();
    void _advance_read_slot();

    static constexpr uint16_t kTicksPerSlot = 2;
    static constexpr uint16_t kNone = 0xffff;

    std::array<Event*, kSliceLength> _slice;

    Event* _buffer;
    Event* _current_event;

    uint16_t _tick_counter;

    uint16_t _write_slot;
    uint16_t _read_slot;
    uint16_t _slice_slot;
    uint16_t _last_hit_slot;

    uint16_t _rec_counter;
    uint16_t _rec_start;
    uint16_t _rec_end;

    bool _is_armed;
    bool _is_auto_cutting;
    bool _is_recording;
    bool _is_clearing;
    bool _is_empty;
};

};
