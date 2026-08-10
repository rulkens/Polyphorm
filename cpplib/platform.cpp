#include "platform.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstring>
#include <deque>

// GLFW callbacks feed this queue; platform::get_event drains it once per
// frame, preserving the original Win32-message-pump contract main.cpp
// is written against.
static std::deque<Event> g_events;
static GLFWwindow *g_glfw_window = nullptr;  // the single live window; set in get_window
static bool g_exit_pushed = false;           // EXIT is delivered exactly once

template <typename T>
static void push_event(EventType type, const T &payload) {
    Event e; e.type = type;
    static_assert(sizeof(T) <= sizeof(e.data), "Event payload too large");
    std::memcpy(e.data, &payload, sizeof(T));
    g_events.push_back(e);
}
static void push_event(EventType type) { Event e; e.type = type; g_events.push_back(e); }

static KeyCode map_key(int key) {
    switch (key) {
        case GLFW_KEY_ESCAPE: return ESC;
        case GLFW_KEY_F1: return F1;   case GLFW_KEY_F2: return F2;
        case GLFW_KEY_F3: return F3;   case GLFW_KEY_F4: return F4;
        case GLFW_KEY_F5: return F5;   case GLFW_KEY_F6: return F6;
        case GLFW_KEY_F7: return F7;   case GLFW_KEY_F8: return F8;
        case GLFW_KEY_F9: return F9;   case GLFW_KEY_F10: return F10;
        case GLFW_KEY_1: return NUM1;  case GLFW_KEY_2: return NUM2;
        case GLFW_KEY_3: return NUM3;  case GLFW_KEY_4: return NUM4;
        case GLFW_KEY_5: return NUM5;  case GLFW_KEY_6: return NUM6;
        default: return OTHER;
    }
}

namespace platform {

Window get_window(char *window_name, uint32_t window_width, uint32_t window_height) {
    Window window = {};
    if (!glfwInit()) return window;
    // No GL context — the surface belongs to WebGPU (Task 4).
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);  // original window is fixed-size
    GLFWwindow *handle = glfwCreateWindow((int)window_width, (int)window_height,
                                          window_name, nullptr, nullptr);
    if (!handle) { glfwTerminate(); return window; }
    window.window_handle = handle;
    window.window_width = window_width;
    window.window_height = window_height;

    glfwSetKeyCallback(handle, [](GLFWwindow *, int key, int, int action, int) {
        if (action == GLFW_PRESS)  push_event(KEY_DOWN, KeyPressedData{map_key(key)});
        if (action == GLFW_RELEASE) push_event(KEY_UP,  KeyPressedData{map_key(key)});
    });
    glfwSetCursorPosCallback(handle, [](GLFWwindow *, double x, double y) {
        push_event(MOUSE_MOVE, MouseMoveData{(float)x, (float)y});
    });
    glfwSetMouseButtonCallback(handle, [](GLFWwindow *w, int button, int action, int) {
        double x, y; glfwGetCursorPos(w, &x, &y);
        MouseMoveData at{(float)x, (float)y};
        if (button == GLFW_MOUSE_BUTTON_LEFT)
            push_event(action == GLFW_PRESS ? MOUSE_LBUTTON_DOWN : MOUSE_LBUTTON_UP, at);
        if (button == GLFW_MOUSE_BUTTON_RIGHT)
            push_event(action == GLFW_PRESS ? MOUSE_RBUTTON_DOWN : MOUSE_RBUTTON_UP, at);
    });
    glfwSetScrollCallback(handle, [](GLFWwindow *, double, double yoff) {
        push_event(MOUSE_WHEEL, MouseWheelData{(float)yoff});
    });

    g_glfw_window = handle;
    return window;
}

bool set_window_title(Window &window, const char *window_title) {
    if (!window.window_handle) return false;
    glfwSetWindowTitle(window.window_handle, window_title);
    return true;
}

bool is_window_valid(Window *window) {
    return window && window->window_handle != nullptr;
}

bool get_event(Event *event) {
    // First call each frame pumps the OS queue; subsequent calls drain ours —
    // preserving the original drain-per-frame Win32-message-pump contract.
    if (g_events.empty()) {
        glfwPollEvents();
        if (g_glfw_window && glfwWindowShouldClose(g_glfw_window) && !g_exit_pushed) {
            push_event(EXIT);
            g_exit_pushed = true;
        }
    }
    if (g_events.empty()) return false;
    *event = g_events.front();
    g_events.pop_front();
    return true;
}

void show_cursor() {
    if (g_glfw_window) glfwSetInputMode(g_glfw_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}
void hide_cursor() {
    if (g_glfw_window) glfwSetInputMode(g_glfw_window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
}

Ticks get_ticks() {
    return (Ticks)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
Ticks get_tick_frequency() { return 1000000000ull; }

float get_dt_from_tick_difference(Ticks t1, Ticks t2, Ticks frequency) {
    return (float)((double)(t2 - t1) / (double)frequency);
}

}  // namespace platform

namespace timer {
Timer get() { Timer t; t.frequency = platform::get_tick_frequency(); t.start = 0; return t; }
void start(Timer *t) { t->start = platform::get_ticks(); }
float end(Timer *t) {
    return platform::get_dt_from_tick_difference(t->start, platform::get_ticks(), t->frequency);
}
float checkpoint(Timer *t) {
    Ticks now = platform::get_ticks();
    float dt = platform::get_dt_from_tick_difference(t->start, now, t->frequency);
    t->start = now;
    return dt;
}
}  // namespace timer
