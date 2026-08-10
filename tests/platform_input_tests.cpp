#include "platform.h"
#include "input.h"
#include <cassert>
#include <cstring>

static Event make_key_event(EventType type, KeyCode code) {
    Event e; e.type = type;
    KeyPressedData d{code};
    std::memcpy(e.data, &d, sizeof(d));
    return e;
}

int main() {
    // Tick math: 2.5e9 ns at 1e9 Hz is 2.5 s.
    assert(platform::get_tick_frequency() == 1000000000ull);
    float dt = platform::get_dt_from_tick_difference(1000000000ull, 3500000000ull,
                                                     platform::get_tick_frequency());
    assert(dt > 2.499f && dt < 2.501f);

    // get_ticks is monotonic non-decreasing.
    Ticks a = platform::get_ticks();
    Ticks b = platform::get_ticks();
    assert(b >= a);

    // Input state machine: key F2 pressed exactly once per down transition.
    input::reset();
    Event down = make_key_event(KEY_DOWN, F2);
    input::register_event(&down);
    assert(input::key_pressed(F2));
    assert(!input::key_pressed(F3));
    input::reset();
    assert(!input::key_pressed(F2));  // pressed is edge-triggered, cleared by reset

    // Mouse: delta accumulates from move events and zeroes on reset.
    Event mv; mv.type = MOUSE_MOVE;
    MouseMoveData md{120.0f, 80.0f};
    std::memcpy(mv.data, &md, sizeof(md));
    input::register_event(&mv);
    assert(input::mouse_position().x == 120.0f);
    Event lb; lb.type = MOUSE_LBUTTON_DOWN; std::memcpy(lb.data, &md, sizeof(md));
    input::register_event(&lb);
    assert(input::mouse_left_button_pressed() && input::mouse_left_button_down());
    input::reset();
    assert(!input::mouse_left_button_pressed());
    assert(input::mouse_left_button_down());  // held state survives reset

    return 0;
}
