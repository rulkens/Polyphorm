// M4a: real Dear ImGui adapter for ui.h, replacing ui_stub.cpp in the
// polyphorm target (see docs/superpowers/research/m4/imgui-integration-design.md).
// This is a NEW implementation, not a revival of the D3D11-era ui.cpp it
// replaces (that file is gone from the target's source list and no longer
// referenced anywhere).
//
// ui.h stays byte-unchanged (house rule) — every function below implements
// an existing signature. No buffering layer is needed (design §3.1):
// draw_rect/draw_text call directly into ImGui's background draw list.
#include "ui.h"
#include "graphics.h"
#include "platform.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_wgpu.h>

namespace ui {

// Guards ImGui::NewFrame()/Render() pairing across the up-to-two-or-zero
// ui::end() call sites in main.cpp (design §4.2). Set by ensure_frame_open()
// (called lazily from the first per-frame ui:: touch), cleared once end()
// has actually run Render()+submit.
static bool g_frame_open = false;
static bool g_input_responsive = false;

// Lazily opens the ImGui frame on the first ui:: touch of the frame
// (is_registering_input/start_panel/draw_rect/draw_text all call this).
static void ensure_frame_open() {
    if (g_frame_open) return;
    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    g_frame_open = true;
}

void init(float, float) {
    // Defensive: never initialize ImGui without a live graphics context
    // (headless mode never calls ui::init at all — main.cpp's guards — but
    // this is belt-and-suspenders against that invariant breaking).
    if (!graphics_context) return;

    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOther(platform::get_glfw_window(), true);

    ImGui_ImplWGPU_InitInfo info = {};
    info.Device = graphics_context->device.Get();
    info.NumFramesInFlight = 3;
    info.RenderTargetFormat = (WGPUTextureFormat)graphics::get_window_surface_format();
    info.DepthStencilFormat = WGPUTextureFormat_Undefined;
    ImGui_ImplWGPU_Init(&info);
}

void draw_text(const char *text, Font *, float x, float y, Vector4 color, Vector2 origin) {
    ensure_frame_open();
    ImVec2 size = ImGui::CalcTextSize(text);
    ImVec2 pos(x - origin.x * size.x, y - origin.y * size.y);
    ImGui::GetBackgroundDrawList()->AddText(pos, ImColor(color.x, color.y, color.z, color.w), text);
}

void draw_text(const char *text, Font *font, Vector2 pos, Vector4 color, Vector2 origin) {
    draw_text(text, font, pos.x, pos.y, color, origin);
}

void draw_text(const char *text, Vector2 pos, Vector4 color, Vector2 origin) {
    draw_text(text, nullptr, pos.x, pos.y, color, origin);
}

void draw_rect(float x, float y, float width, float height, Vector4 color) {
    ensure_frame_open();
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(x, y), ImVec2(x + width, y + height),
        ImColor(color.x, color.y, color.z, color.w));
}

void draw_rect(Vector2 pos, float width, float height, Vector4 color) {
    draw_rect(pos.x, pos.y, width, height, color);
}

Panel start_panel(char *name, Vector2 pos, float width) {
    ensure_frame_open();
    Panel p = {};
    p.name = name;
    p.pos = pos;
    p.width = width;
    // §3.2.1 quirk note: pos/width are deliberately NOT used to size/position
    // the ImGui window — the old D3D11 adapter's 1px-wide-background-rect
    // quirk is not reproduced. ImGui auto-sizes/auto-positions instead.
    ImGui::Begin(*name ? name : "Polyphorm");
    return p;
}

Panel start_panel(char *name, float x, float y, float width) {
    return start_panel(name, Vector2(x, y), width);
}

void end_panel(Panel *) {
    ImGui::End();
}

Vector4 get_panel_rect(Panel *panel) {
    // No call sites in main.cpp (design §3.2) — best-effort implementation
    // for API completeness.
    if (g_frame_open) {
        ImVec2 p = ImGui::GetWindowPos();
        ImVec2 s = ImGui::GetWindowSize();
        return Vector4(p.x, p.y, s.x, s.y);
    }
    return Vector4(panel->pos.x, panel->pos.y, panel->width, 0);
}

void end() {
    if (!g_frame_open) return;   // nothing pending — idempotent per frame (design §2.3)
    ImGui::Render();
    wgpu::RenderPassEncoder pass = graphics::begin_ui_pass();
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
    graphics::end_ui_pass(pass);
    g_frame_open = false;
}

bool add_toggle(Panel *, char *label, bool *state) {
    ensure_frame_open();
    return ImGui::Checkbox(label, state);
}

bool add_slider(Panel *, char *label, float *pos, float min, float max) {
    ensure_frame_open();
    return ImGui::SliderFloat(label, pos, min, max);
}

void release() {
    ImGui_ImplWGPU_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void set_input_responsive(bool is_responsive) {
    g_input_responsive = is_responsive;
    ImGuiIO &io = ImGui::GetIO();
    if (is_responsive) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    else                io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
}

bool is_input_responsive() {
    return g_input_responsive;
}

bool is_registering_input() {
    ensure_frame_open();
    return ImGui::GetIO().WantCaptureMouse;
}

float get_screen_width() {
    return ImGui::GetIO().DisplaySize.x;
}

Font *get_font() {
    return nullptr;
}

}  // namespace ui
