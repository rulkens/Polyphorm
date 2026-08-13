// M4: replace with the Dear ImGui implementation (see docs/superpowers/research/m2/imgui-integration.md).
#include "ui.h"

namespace ui {

static bool g_input_responsive = false;

void init(float, float) {}
void draw_text(const char *, Font *, float, float, Vector4, Vector2) {}
void draw_text(const char *, Font *, Vector2, Vector4, Vector2) {}
void draw_text(const char *, Vector2, Vector4, Vector2) {}
void draw_rect(float, float, float, float, Vector4) {}
void draw_rect(Vector2, float, float, Vector4) {}
Panel start_panel(char *name, Vector2 pos, float width) { Panel p = {}; p.name = name; p.pos = pos; p.width = width; return p; }
Panel start_panel(char *name, float x, float y, float width) { return start_panel(name, Vector2(x, y), width); }
Panel start_panel_collapsed(char *name, Vector2 pos, float width) { return start_panel(name, pos, width); }
void end_panel(Panel *) {}
Vector4 get_panel_rect(Panel *) { return Vector4(0, 0, 0, 0); }
void end() {}
bool add_toggle(Panel *, char *, bool *) { return false; }
bool add_slider(Panel *, char *, float *, float, float) { return false; }
void add_text(Panel *, const char *) {}
bool add_combo(Panel *, char *, int *, const char *const [], int) { return false; }
void release() {}
void set_input_responsive(bool is_responsive) { g_input_responsive = is_responsive; }
bool is_input_responsive() { return g_input_responsive; }
bool is_registering_input() { return false; }
float get_screen_width() { return 0.0f; }
Font *get_font() { return nullptr; }

}
