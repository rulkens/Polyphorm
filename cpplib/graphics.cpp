#include "graphics.h"
#include "logging.h"   // check what M1 code uses; if logging.cpp is not yet
                       // compiled, use fprintf(stderr, ...) like gpu_context.cpp
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

graphics::GraphicsContext *graphics_context = nullptr;

namespace graphics {

// ---- internal state ----
static GraphicsContext g_ctx;
static GpuContext g_gpu;
static wgpu::CommandEncoder g_encoder;
static wgpu::SurfaceTexture g_surface_tex;       // acquired lazily per frame
static bool g_surface_tex_acquired = false;
static BlendType g_blend = BlendType::OPAQUE;

// Compute binding shadow state (Task 5 consumes):
struct BoundSlot {
    enum class Kind { NONE, STORAGE_BUFFER, STORAGE_TEX, SAMPLED_TEX, SAMPLER } kind = Kind::NONE;
    wgpu::Buffer buffer;
    uint64_t buffer_size = 0;
    wgpu::TextureView view;
    wgpu::Sampler sampler;
};
static const uint32_t MAX_SLOTS = 16;
static BoundSlot g_compute_slots[MAX_SLOTS];
static wgpu::Buffer g_uniform_buffer;            // group 0 binding 0
static uint64_t g_uniform_size = 0;
static ComputeShader *g_compute_shader = nullptr;

static void ensure_encoder() {
    if (!g_encoder) g_encoder = g_ctx.device.CreateCommandEncoder();
}

static void flush_commands() {
    if (!g_encoder) return;
    wgpu::CommandBuffer commands = g_encoder.Finish();
    g_ctx.queue.Submit(1, &commands);
    g_encoder = nullptr;
}

// Blocking pump: process events until `done` flips. Used by readback and
// pipeline-error scopes. Mirrors gpu_context.cpp's request pumps.
static void wait_for(bool *done) {
    while (!*done) g_gpu.instance.ProcessEvents();
}

bool init() {
    if (!gpu::init_device(&g_gpu)) return false;
    g_ctx.device = g_gpu.device;
    g_ctx.queue = g_gpu.queue;
    g_ctx.gpu = &g_gpu;
    graphics_context = &g_ctx;
    return true;
}

bool init_swap_chain(Window *window) {
    return gpu::init_surface(&g_gpu, window);
}

RenderTarget get_render_target_window() {
    RenderTarget rt = {};
    rt.is_window = true;
    rt.width = g_gpu.width;
    rt.height = g_gpu.height;
    return rt;
}

// Acquire the surface texture for this frame if not already held.
static wgpu::TextureView window_view() {
    if (!g_surface_tex_acquired) {
        g_gpu.surface.GetCurrentTexture(&g_surface_tex);
        if (g_surface_tex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
            g_surface_tex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
            fprintf(stderr, "[graphics] surface texture acquisition failed\n");
            return nullptr;
        }
        g_surface_tex_acquired = true;
    }
    wgpu::TextureViewDescriptor view_desc = {};
    view_desc.format = wgpu::TextureFormat::BGRA8UnormSrgb; // sRGB view over the
    // BGRA8Unorm surface — preserves the original's sRGB-over-UNORM gamma quirk
    // (inventory §1, get_render_target_window). Surface must be configured with
    // this viewFormat in gpu::init_surface — if M1's configure lacks
    // `viewFormats`, add it there (1-line: viewFormatCount=1, viewFormats=&srgb).
    return g_surface_tex.texture.CreateView(&view_desc);
}

void set_render_targets_viewport(RenderTarget *buffer) {
    // D3D11 version set OM targets + viewport. WebGPU render passes carry the
    // target; viewport is full-target by default. Nothing to record until a
    // clear or (M3) a draw — this call is intentionally a no-op that validates
    // the argument shape.
    (void)buffer;
}

void clear_render_target(RenderTarget *buffer, float r, float g, float b, float a) {
    ensure_encoder();
    wgpu::TextureView view = buffer->is_window ? window_view() : buffer->rt_view;
    if (!view) return;
    wgpu::RenderPassColorAttachment att = {};
    att.view = view;
    att.loadOp = wgpu::LoadOp::Clear;
    att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {r, g, b, a};
    wgpu::RenderPassDescriptor pass_desc = {};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &att;
    wgpu::RenderPassEncoder pass = g_encoder.BeginRenderPass(&pass_desc);
    pass.End();
}

void swap_frames() {
    flush_commands();
    if (g_surface_tex_acquired) {
        g_gpu.surface.Present();
        g_surface_tex = {};
        g_surface_tex_acquired = false;
    }
}

void release() {
    flush_commands();
    g_uniform_buffer = nullptr;
    for (uint32_t i = 0; i < MAX_SLOTS; i++) g_compute_slots[i] = {};
    graphics_context = nullptr;
    // wgpu C++ handles are refcounted; dropping them tears down the device.
    g_ctx = {};
    g_gpu = {};
}

// ---- M2a Task 4/5/6 implement these ----

DepthBuffer get_depth_buffer(uint32_t width, uint32_t height) {
    return {};
}

Texture2D get_texture2D(void *data, uint32_t width, uint32_t height, Format format,
                        uint32_t pixel_byte_count) {
    return {};
}

Texture3D get_texture3D(void *data, uint32_t width, uint32_t height, uint32_t depth,
                        Format format, uint32_t pixel_byte_count) {
    return {};
}

Texture2D load_texture2D(std::string filename) {
    return {};
}

void save_texture3D(Texture3D *texture, std::string filename) {
}

void save_texture2D_HDR(Texture2D *texture, std::string filename) {
}

uint32_t capture_current_frame() {
    return 0;
}

void clear_texture(Texture3D *texture, float value) {
}

void clear_texture(Texture2D *texture, float value) {
}

void clear_texture_uint(Texture2D *texture, uint32_t value) {
}

void set_texture(Texture2D *texture, uint32_t slot) {
}

void set_texture(Texture3D *texture, uint32_t slot) {
}

void unset_texture(uint32_t slot) {
}

void set_texture_sampler(TextureSampler *sampler, uint32_t slot) {
}

void set_texture_compute(Texture2D *texture, uint32_t slot) {
}

void set_texture_compute(Texture3D *texture, uint32_t slot) {
}

void set_texture_sampled_compute(Texture2D *texture, uint32_t slot) {
}

void set_texture_sampled_compute(Texture3D *texture, uint32_t slot) {
}

void set_texture_sampler_compute(TextureSampler *sampler, uint32_t slot) {
}

void unset_texture_compute(uint32_t slot) {
}

void unset_texture_sampled_compute(uint32_t slot) {
}

TextureSampler get_texture_sampler(SampleMode mode, Filter filter) {
    return {};
}

void set_blend_state(BlendType type) {
    g_blend = type;
}

Mesh get_mesh(void *vertices, uint32_t vertex_count, uint32_t vertex_stride,
              void *indices, uint32_t index_count, uint32_t index_byte_size,
              Topology topology) {
    return {};
}

void draw_mesh(Mesh *mesh) {
    static bool warned = false;
    if (!warned) {
        fprintf(stderr, "[graphics] draw_mesh: M2a stub\n");
        warned = true;
    }
}

ConstantBuffer get_constant_buffer(uint32_t size) {
    return {};
}

void update_constant_buffer(ConstantBuffer *buffer, void *data) {
}

void set_constant_buffer(ConstantBuffer *buffer, uint32_t slot) {
}

StructuredBuffer get_structured_buffer(int element_stride, int num_elements) {
    return {};
}

void update_structured_buffer(StructuredBuffer *buffer, void *data) {
}

void set_structured_buffer(StructuredBuffer *buffer, uint32_t slot) {
}

void capture_structured_buffer(StructuredBuffer *buffer, void *mapped_data,
                               uint32_t num_elements, size_t element_size) {
}

VertexShader get_vertex_shader_from_code(char *code, uint32_t code_length) {
    return {};
}

PixelShader get_pixel_shader_from_code(char *code, uint32_t code_length) {
    return {};
}

ComputeShader get_compute_shader_from_code(char *code, uint32_t code_length,
                                           const ShaderConstant *constants,
                                           uint32_t constant_count) {
    return {};
}

void set_vertex_shader(VertexShader *shader) {
}

void set_pixel_shader(PixelShader *shader) {
}

void set_compute_shader(ComputeShader *shader) {
    g_compute_shader = shader;
}

void run_compute(int group_count_x, int group_count_y, int group_count_z) {
}

bool is_ready(RenderTarget *ptr) {
    return ptr != nullptr && (ptr->is_window || ptr->rt_view);
}

bool is_ready(DepthBuffer *ptr) {
    return ptr != nullptr && ptr->ds_view;
}

bool is_ready(Texture2D *ptr) {
    return ptr != nullptr && ptr->texture;
}

bool is_ready(Texture3D *ptr) {
    return ptr != nullptr && ptr->texture;
}

bool is_ready(Mesh *ptr) {
    return ptr != nullptr && ptr->vertex_buffer;
}

bool is_ready(ConstantBuffer *ptr) {
    return ptr != nullptr && ptr->buffer;
}

bool is_ready(StructuredBuffer *ptr) {
    return ptr != nullptr && ptr->buffer;
}

bool is_ready(TextureSampler *ptr) {
    return ptr != nullptr && ptr->sampler;
}

bool is_ready(VertexShader *ptr) {
    return ptr != nullptr && ptr->valid;
}

bool is_ready(PixelShader *ptr) {
    return ptr != nullptr && ptr->valid;
}

bool is_ready(ComputeShader *ptr) {
    return ptr != nullptr && ptr->valid;
}

void release(RenderTarget *ptr) {
    if (ptr) *ptr = {};
}

void release(DepthBuffer *ptr) {
    if (ptr) *ptr = {};
}

void release(Texture2D *ptr) {
    if (ptr) *ptr = {};
}

void release(Texture3D *ptr) {
    if (ptr) *ptr = {};
}

void release(Mesh *ptr) {
    if (ptr) *ptr = {};
}

void release(ConstantBuffer *ptr) {
    if (ptr) *ptr = {};
}

void release(StructuredBuffer *ptr) {
    if (ptr) *ptr = {};
}

void release(TextureSampler *ptr) {
    if (ptr) *ptr = {};
}

void release(VertexShader *ptr) {
    if (ptr) *ptr = {};
}

void release(PixelShader *ptr) {
    if (ptr) *ptr = {};
}

void release(ComputeShader *ptr) {
    if (ptr) *ptr = {};
}

}
