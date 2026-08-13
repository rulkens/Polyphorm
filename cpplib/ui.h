#pragma once
#include "maths.h"

struct Font;

struct Panel
{
    char *name;
    Vector2 pos;
    float width;

    Vector2 item_pos;
};

namespace ui
{
    void init(float screen_width, float screen_height);
    
    void draw_text(const char *text, Font *font, float x, float y, Vector4 color, Vector2 origin = Vector2(0,0));
    void draw_text(const char *text, Font *font, Vector2 pos, Vector4 color, Vector2 origin = Vector2(0,0));
    void draw_text(const char *text, Vector2 pos, Vector4 color, Vector2 origin = Vector2(0,0));

    void draw_rect(float x, float y, float width, float height, Vector4 color);
    void draw_rect(Vector2 pos, float width, float height, Vector4 color);
    
    Panel start_panel(char *name, Vector2 pos, float width);
    Panel start_panel(char *name, float x, float y, float width);
    // Port addition: same as start_panel, but the window starts collapsed
    // (first launch only — ImGui persists the user's state in imgui.ini).
    Panel start_panel_collapsed(char *name, Vector2 pos, float width);
    void end_panel(Panel *panel);
    Vector4 get_panel_rect(Panel *panel);

    void end();
    
    bool add_toggle(Panel *panel, char *label, bool *state);
    bool add_slider(Panel *panel, char *label, float *pos, float min, float max);
    // Port addition: static text row inside the current panel.
    void add_text(Panel *panel, const char *text);
    // Port addition: dropdown; returns true when the selection changes.
    bool add_combo(Panel *panel, char *label, int *index, const char *const items[], int count);

    void release();

    // These functions enable turning UI on/off as a listener for inputs
    void set_input_responsive(bool is_responsive);
    bool is_input_responsive();

    bool is_registering_input();

    // Getters for values on which UI operates
    float get_screen_width();
    Font *get_font();
}
