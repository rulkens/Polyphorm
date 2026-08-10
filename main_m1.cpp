#include "platform.h"
#include "input.h"
#include "gpu/gpu_context.h"
#include <cmath>
#include <cstdio>

int main() {
    Window window = platform::get_window((char *)"Polyphorm [M1]", 1280, 720);
    if (!IS_WINDOW_VALID(window)) { std::fprintf(stderr, "window creation failed\n"); return 1; }
    GpuContext ctx = gpu::init(&window);

    Timer frame_timer = timer::get();
    timer::start(&frame_timer);
    float t = 0.0f, frame_ms = 0.0f;
    bool running = true;
    while (running) {
        input::reset();
        Event event;
        while (platform::get_event(&event)) {
            input::register_event(&event);
            if (event.type == EXIT) running = false;
        }
        if (input::key_pressed(ESC)) running = false;

        float dt = timer::checkpoint(&frame_timer);
        t += dt;
        frame_ms = 0.9f * frame_ms + 0.1f * dt * 1000.0f;  // the original's EMA style
        char title[128];
        std::snprintf(title, sizeof(title), "Polyphorm [M1] [%.1f ms]", frame_ms);
        platform::set_window_title(window, title);

        // Slow teal<->purple pulse proves per-frame present + timing.
        gpu::clear_and_present(&ctx, 0.1f + 0.1f * std::sin(t), 0.2f, 0.35f);
    }
    return 0;
}
