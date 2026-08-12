#pragma once
#include <stdint.h>
#include <string>
#include <webgpu/webgpu_cpp.h>

#include "gpu/gpu_context.h"

struct Window;

namespace graphics {

// ---- fork-owned enums (replace DXGI/D3D11 enums) ----
enum class Format {
    UNKNOWN,
    R32_FLOAT,        // replaces DXGI_FORMAT_R16_FLOAT for storage textures:
                      // WebGPU's only read_write float storage format. Exact
                      // behavioural match in REGIME_SDSS (research notes §0).
    RGBA32_FLOAT,     // display_tex
    R32_UINT,         // stale-comment fix (DESIGN §6.5, m3-carryovers.md): the
                      // `display_tex_uint` texture this once named was
                      // deleted — particle splat accumulation is now a
                      // StructuredBuffer of atomics (main.cpp:653), not a
                      // texture. R32_UINT itself is still live: the
                      // clear_texture_uint kernel's format (graphics.cpp).
    RGBA8_UNORM,
    RGBA8_UNORM_SRGB, // window view format (preserves the sRGB-over-UNORM quirk)
};
enum SampleMode { CLAMP, WRAP, BORDER };
enum class Filter { POINT, LINEAR, ANISOTROPIC };
enum class BlendType { OPAQUE, ALPHA };
enum class Topology { TRIANGLELIST, TRIANGLESTRIP };

// ---- types ----
struct GraphicsContext {
    wgpu::Device device;
    wgpu::Queue queue;
    GpuContext *gpu = nullptr;
};

struct RenderTarget {
    wgpu::Texture texture;       // null when is_window (acquired per frame)
    wgpu::TextureView rt_view;   // null when is_window
    uint32_t width = 0, height = 0;
    bool is_window = false;
    // UNKNOWN for the window target (its format is the fixed sRGB BGRA8 view
    // hardcoded at draw_mesh time, matching window_view()); set by
    // get_render_target() for offscreen targets, whose format draw_mesh's
    // pipeline-cache key must account for (M3 Task 1: render_path_tests
    // draws into a non-window-format offscreen RT — see graphics.cpp's
    // draw_mesh comment for why the pipeline format axis isn't hardcoded).
    Format format = Format::UNKNOWN;
};

struct DepthBuffer {             // created by main.cpp but never bound (inventory §4)
    wgpu::Texture texture;
    wgpu::TextureView ds_view;
    uint32_t width = 0, height = 0;
};

struct Texture2D {
    wgpu::Texture texture;
    wgpu::TextureView sr_view;   // sampled view
    wgpu::TextureView ua_view;   // storage view (name kept from D3D11 for familiarity)
    uint32_t width = 0, height = 0;
    Format format = Format::UNKNOWN;
};

struct Texture3D {
    wgpu::Texture texture;
    wgpu::TextureView sr_view;
    wgpu::TextureView ua_view;
    uint32_t width = 0, height = 0, depth = 0;
    Format format = Format::UNKNOWN;
};

struct TextureSampler { wgpu::Sampler sampler; };

struct Mesh {
    wgpu::Buffer vertex_buffer;
    uint32_t vertex_stride = 0, vertex_offset = 0, vertex_count = 0;
    Topology topology = Topology::TRIANGLELIST;
};

struct ConstantBuffer { wgpu::Buffer buffer; uint32_t size = 0; };

struct StructuredBuffer {
    wgpu::Buffer buffer;
    wgpu::Buffer readback;       // lazily created by capture_structured_buffer
    uint32_t element_stride = 0, num_elements = 0, size = 0;
};

// M3: real compiled modules. `valid` means "this WGSL module compiled" —
// weaker than ComputeShader::valid ("pipeline built"), since stride/blend/
// target-format compatibility can only be checked at draw_mesh time, once a
// Mesh and the current blend/target state are known (render-path-design.md §1).
struct VertexShader  { wgpu::ShaderModule module; bool valid = false; bool uses_group0 = false; };
struct PixelShader   { wgpu::ShaderModule module; bool valid = false; bool uses_group0 = false; };
struct ComputeShader { wgpu::ComputePipeline pipeline; bool valid = false; bool uses_group0 = false; };

// Named override constants passed at compute-pipeline creation (quirk toggles,
// WG_X/Y/Z workgroup sizes — research notes §1).
struct ShaderConstant { const char *name; double value; };

// ---- lifecycle ----
bool init();                            // device only (headless-safe); sets graphics_context
bool init_swap_chain(Window *window);   // surface config (framebuffer size)
// Re-Configure the window surface at a new framebuffer size (M4a Task 2b:
// window resize support). No-op when either dimension is 0 (minimized/
// degenerate window) — Dawn requires positive Configure extents; main.cpp
// is expected to skip all rendering for that frame too. MUST be called
// between frames — no open command encoder/render pass (top-of-frame,
// before any set_*/run_* calls, is the safe point; graphics.cpp never
// holds an open encoder there since the previous frame's swap_frames()
// already flushed it).
void resize_surface(uint32_t fb_width, uint32_t fb_height);
// M4a fix round 1: generic, ui::-agnostic extension point. If set,
// swap_frames() invokes it once, before flush_commands()/Present — the only
// point in a frame guaranteed to run after every scene draw_mesh call AND
// every ui:: widget call, and before the frame's commands are submitted.
// ui::init() registers ui::flush_frame() here so ImGui's Render() +
// begin_ui_pass()/RenderDrawData/end_ui_pass() happens exactly once per
// frame, regardless of how many times main.cpp calls the now-inert
// ui::end() (see ui.cpp's ui::end()/flush_frame() comments for why: two
// Render() cycles per app frame was consuming ImGui's MouseClicked edge in
// the first cycle, before the panel's widgets were even issued in the
// second). A plain function pointer, not a ui:: type, so graphics:: still
// has no compile-time dependency on ui:: (design §2.2's layering rule).
void set_frame_end_hook(void (*hook)());
void swap_frames();                     // submit + present
void release();                         // teardown, last call

// ---- render targets / clears ----
RenderTarget get_render_target_window();
// The window view's format (BGRA8UnormSrgb) — single source of truth,
// matching window_view()'s hardcoded view format and draw_mesh's pipeline
// cache key (graphics.cpp). Used by ui.cpp for
// ImGui_ImplWGPU_InitInfo::RenderTargetFormat (M4a design §1.3).
wgpu::TextureFormat get_window_surface_format();
// M3: real offscreen render target (RenderAttachment|CopySrc texture + view).
// Never called by main.cpp (inventory: all "offscreen" buffers are compute-
// written textures, not render targets) but is part of the 39-function
// surface and is how render_path_tests exercises draw_mesh headlessly.
RenderTarget get_render_target(uint32_t width, uint32_t height, Format format);
DepthBuffer get_depth_buffer(uint32_t width, uint32_t height);
void set_render_targets_viewport(RenderTarget *buffer);
void clear_render_target(RenderTarget *buffer, float r, float g, float b, float a);

// ---- ui pass (M4a design §2.2) ----
// Opens a LoadOp::Load render pass on the window surface — draws on top of
// whatever the scene pass(es) already wrote this frame. Reuses the existing
// static ensure_encoder()/window_view() machinery; does not introduce a
// second render-pass idiom. Contract: begin_ui_pass/end_ui_pass are invoked
// by the frame-end hook (registered by ui::init, see set_frame_end_hook),
// which swap_frames() runs after all scene passes are recorded and before
// submit/Present. ui::end() is inert and kept only for upstream compatibility.
wgpu::RenderPassEncoder begin_ui_pass();
void end_ui_pass(wgpu::RenderPassEncoder pass);

// ---- textures ----
Texture2D get_texture2D(void *data, uint32_t width, uint32_t height, Format format,
                        uint32_t pixel_byte_count = 4);
Texture3D get_texture3D(void *data, uint32_t width, uint32_t height, uint32_t depth,
                        Format format, uint32_t pixel_byte_count = 4);
Texture2D load_texture2D(std::string filename);                 // M2a: stub (1x1 white)
void save_texture3D(Texture3D *texture, std::string filename);  // M2a: stub (M5)
void save_texture2D_HDR(Texture2D *texture, std::string filename); // M2a: stub (M5)
uint32_t capture_current_frame();                               // M2a: stub (M5)

// Replaces main.cpp's raw ClearUnorderedAccessView* on .ua_view (inventory §2):
void clear_texture(Texture3D *texture, float value);
void clear_texture(Texture2D *texture, float value);
void clear_texture_uint(Texture2D *texture, uint32_t value);
void clear_structured_buffer(StructuredBuffer *buffer);   // zero-fills; GPU-side ClearBuffer

// ---- binding: render stage (M2a: stubs, real in M3) ----
void set_texture(Texture2D *texture, uint32_t slot);
void set_texture(Texture3D *texture, uint32_t slot);
void unset_texture(uint32_t slot);
void set_texture_sampler(TextureSampler *sampler, uint32_t slot);

// ---- binding: compute stage (real) ----
void set_texture_compute(Texture2D *texture, uint32_t slot);          // storage view
void set_texture_compute(Texture3D *texture, uint32_t slot);
void set_texture_sampled_compute(Texture2D *texture, uint32_t slot);  // sampled view
void set_texture_sampled_compute(Texture3D *texture, uint32_t slot);
void set_texture_sampler_compute(TextureSampler *sampler, uint32_t slot);
void unset_texture_compute(uint32_t slot);
void unset_texture_sampled_compute(uint32_t slot);

// ---- samplers ----
TextureSampler get_texture_sampler(SampleMode mode = CLAMP, Filter filter = Filter::POINT);

// ---- blend ----
void set_blend_state(BlendType type);   // M2a: recorded, consumed by M3 draws

// ---- mesh / draw ----
Mesh get_mesh(void *vertices, uint32_t vertex_count, uint32_t vertex_stride,
              void *indices, uint32_t index_count, uint32_t index_byte_size,
              Topology topology = Topology::TRIANGLELIST);
void draw_mesh(Mesh *mesh);             // M2a: stub (warn once)

// ---- constant buffers ----
ConstantBuffer get_constant_buffer(uint32_t size);
void update_constant_buffer(ConstantBuffer *buffer, void *data);
void set_constant_buffer(ConstantBuffer *buffer, uint32_t slot); // slot must be 0 (group 0 binding 0)

// ---- structured buffers ----
StructuredBuffer get_structured_buffer(int element_stride, int num_elements);
void update_structured_buffer(StructuredBuffer *buffer, void *data);
void set_structured_buffer(StructuredBuffer *buffer, uint32_t slot);
void unset_structured_buffer(uint32_t slot);
void capture_structured_buffer(StructuredBuffer *buffer, void *mapped_data,
                               uint32_t num_elements, size_t element_size);

// ---- shaders ----
VertexShader get_vertex_shader_from_code(char *code, uint32_t code_length);   // M2a: stub
PixelShader get_pixel_shader_from_code(char *code, uint32_t code_length);     // M2a: stub
ComputeShader get_compute_shader_from_code(char *code, uint32_t code_length,
                                           const ShaderConstant *constants = nullptr,
                                           uint32_t constant_count = 0);
void set_vertex_shader(VertexShader *shader);   // M2a: stub
void set_pixel_shader(PixelShader *shader);     // M2a: stub
void set_compute_shader(ComputeShader *shader);

// ---- dispatch ----
void run_compute(int group_count_x, int group_count_y, int group_count_z);

// ---- is_ready (assert guards in main.cpp) ----
bool is_ready(RenderTarget *ptr);
bool is_ready(DepthBuffer *ptr);
bool is_ready(Texture2D *ptr);
bool is_ready(Texture3D *ptr);
bool is_ready(Mesh *ptr);
bool is_ready(ConstantBuffer *ptr);
bool is_ready(StructuredBuffer *ptr);
bool is_ready(TextureSampler *ptr);
bool is_ready(VertexShader *ptr);
bool is_ready(PixelShader *ptr);
bool is_ready(ComputeShader *ptr);

// ---- release overloads (fork keeps only the ones main.cpp calls) ----
void release(RenderTarget *ptr);
void release(DepthBuffer *ptr);
void release(Texture2D *ptr);
void release(Texture3D *ptr);
void release(Mesh *ptr);
void release(ConstantBuffer *ptr);
void release(StructuredBuffer *ptr);
void release(TextureSampler *ptr);
void release(VertexShader *ptr);
void release(PixelShader *ptr);
void release(ComputeShader *ptr);

}

extern graphics::GraphicsContext *graphics_context;
