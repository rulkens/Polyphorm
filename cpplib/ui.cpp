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
#include "resources.h"

#include <cstring>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_wgpu.h>

namespace ui {

// Guards ImGui::NewFrame()/Render() pairing across the whole app frame. Set
// by ensure_frame_open() (called lazily from the first per-frame ui::
// touch), cleared once flush_frame() has actually run Render()+submit.
static bool g_frame_open = false;
static bool g_input_responsive = false;

// Lazily opens the ImGui frame on the first ui:: touch of the frame
// (is_registering_input/start_panel/draw_rect/draw_text/add_toggle/
// add_slider all call this).
static void ensure_frame_open() {
    if (g_frame_open) return;
    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    g_frame_open = true;
}

// M4a fix round 1 (human gate failure: "cannot interact with the GUI").
// Root cause: main.cpp calls ui::end() up to three times per app frame
// (histogram block, panel block, defensive pre-swap_frames call). The
// original adapter rendered+closed the ImGui frame on the FIRST end() call
// (the histogram block's — background draw lists only, no widgets) before
// the panel's sliders/toggles were even issued; ensure_frame_open() then
// opened a SECOND NewFrame()/Render() cycle for the panel. Both passes are
// LoadOp::Load, so the *visuals* still accumulated correctly (panel and
// histogram render fine) — but ImGui derives the MouseClicked edge inside
// NewFrame() from MouseDownDuration, and the first cycle's NewFrame()
// consumed that edge; by the second cycle (the one with the actual
// widgets), MouseDownDuration was no longer < 0, so MouseClicked was never
// true there and SliderBehavior/ButtonBehavior could never begin a
// drag/press. DESIGN §8 risk #1, materializing as an input bug rather than
// the anticipated NewFrame-without-Render assert.
//
// Fix: ui::end() (below) no longer renders at all — it's now inert, kept
// only because ui.h's signature can't change and main.cpp's three call
// sites are unconditionally still compiled against it. The actual flush
// (Render() + begin_ui_pass()/RenderDrawData/end_ui_pass()) happens exactly
// once per app frame, in flush_frame() below, invoked by
// graphics::swap_frames()'s frame-end hook (registered here in init()) —
// the one point guaranteed to run after every scene draw_mesh call AND
// every ui:: widget call this frame (main.cpp's histogram/panel blocks both
// run before swap_frames()), and before this frame's commands are
// submitted/presented. One NewFrame()/Render() pair per app frame,
// independent of how many (0-3) times main.cpp calls ui::end().
static void flush_frame() {
    if (!g_frame_open) return;
    ImGui::Render();
    wgpu::RenderPassEncoder pass = graphics::begin_ui_pass();
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
    graphics::end_ui_pass(pass);
    g_frame_open = false;
}

void init(float, float) {
    // Defensive: never initialize ImGui without a live graphics context
    // (headless mode never calls ui::init at all — main.cpp's guards — but
    // this is belt-and-suspenders against that invariant breaking).
    if (!graphics_context) return;

    ImGui::CreateContext();
    // Bundle mode: keep imgui.ini out of the signed .app (cwd there is
    // Contents/Resources). Dev mode keeps ImGui's default cwd-relative file.
    if (resources::is_bundle()) {
        static std::string ini_path = resources::output("imgui.ini");
        ImGui::GetIO().IniFilename = ini_path.c_str();
    }
    ImGui_ImplGlfw_InitForOther(platform::get_glfw_window(), true);

    ImGui_ImplWGPU_InitInfo info = {};
    info.Device = graphics_context->device.Get();
    info.NumFramesInFlight = 3;
    info.RenderTargetFormat = (WGPUTextureFormat)graphics::get_window_surface_format();
    info.DepthStencilFormat = WGPUTextureFormat_Undefined;
    ImGui_ImplWGPU_Init(&info);

    graphics::set_frame_end_hook(&flush_frame);
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
    const char *title = *name ? name : "Polyphorm";
    // First-launch layout (no imgui.ini yet): main panel top-left with
    // SHORTCUTS stacked below it, instead of ImGui's default of every
    // window at 60,60 overlapping. Once the user moves a panel, imgui.ini
    // wins (FirstUseEver).
    if (strcmp(title, "Polyphorm") == 0) {
        ImGui::SetNextWindowPos(ImVec2(17, 19), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(474, 657), ImGuiCond_FirstUseEver);
    } else if (strcmp(title, "SHORTCUTS") == 0) {
        ImGui::SetNextWindowPos(ImVec2(17, 692), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(429, 303), ImGuiCond_FirstUseEver);
    }
    ImGui::Begin(title);
    return p;
}

Panel start_panel(char *name, float x, float y, float width) {
    return start_panel(name, Vector2(x, y), width);
}

Panel start_panel_collapsed(char *name, Vector2 pos, float width) {
    ensure_frame_open();
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    return start_panel(name, pos, width);
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
    // M4a fix round 1: intentionally inert — see flush_frame()'s comment
    // above. Kept only so ui.h's signature (and main.cpp's three existing
    // call sites) keep compiling unmodified; the real per-frame flush now
    // happens exactly once, from graphics::swap_frames()'s frame-end hook.
}

bool add_toggle(Panel *, char *label, bool *state) {
    ensure_frame_open();
    return ImGui::Checkbox(label, state);
}

bool add_slider(Panel *, char *label, float *pos, float min, float max) {
    ensure_frame_open();
    return ImGui::SliderFloat(label, pos, min, max);
}

void add_text(Panel *, const char *text) {
    ensure_frame_open();
    ImGui::TextUnformatted(text);
}

bool add_combo(Panel *, char *label, int *index, const char *const items[], int count) {
    ensure_frame_open();
    return ImGui::Combo(label, index, items, count);
}

bool add_play_pause_button(Panel *, bool running, const char *label) {
    ensure_frame_open();
    // The default ImGui font has no media glyphs, so the icon is drawn:
    // a normal full-width button with left padding reserved for it.
    const float h = ImGui::GetFrameHeight();
    const float icon = h * 0.45f;
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec2 pad_saved = style.FramePadding;
    style.FramePadding.x += icon + pad_saved.x;
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    bool clicked = ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f));
    ImGui::PopStyleVar();
    style.FramePadding = pad_saved;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImVec2 r = ImGui::GetItemRectMin();
    float x = r.x + pad_saved.x + 1.0f;
    float y = r.y + (h - icon) * 0.5f;
    if (running) {   // pause: two bars
        float bar = icon * 0.34f;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bar, y + icon), col);
        dl->AddRectFilled(ImVec2(x + icon - bar, y), ImVec2(x + icon, y + icon), col);
    } else {         // play: right-pointing triangle
        dl->AddTriangleFilled(ImVec2(x, y), ImVec2(x, y + icon), ImVec2(x + icon, y + icon * 0.5f), col);
    }
    return clicked;
}

void release() {
    graphics::set_frame_end_hook(nullptr);   // defensive: no dangling callback into torn-down ImGui state
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
