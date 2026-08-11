#include "graphics.h"
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

// One lazily-built pipeline + scratch uniform per builtin clear kernel
// (declared here, ahead of release(), so release() can reset them; the WGSL
// sources and ensure_clear_kernel()/run_clear() stay further down near the
// clear_texture* entry points).
struct ClearKernel {
    wgpu::ComputePipeline pipeline;
    wgpu::Buffer uniform;   // 16 bytes
};
static ClearKernel g_clear3d, g_clear2d_f, g_clear2d_u;

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
    g_compute_shader = nullptr;
    g_blend = BlendType::OPAQUE;
    g_clear3d = {};
    g_clear2d_f = {};
    g_clear2d_u = {};
    graphics_context = nullptr;
    // wgpu C++ handles are refcounted; dropping them tears down the device.
    g_ctx = {};
    g_gpu = {};
}

// ---- M2a Task 4/5/6 implement these ----

static void fatal(const char *what) {
    fprintf(stderr, "[graphics] FATAL: %s\n", what);
    exit(1);
}

static void warn_once(const char *what) {
    // one stderr line per distinct stub, first call only (`what` is always a
    // string literal, so pointer identity is a valid key)
    static const char *seen[8] = {};
    for (int i = 0; i < 8; i++) {
        if (seen[i] == what) return;
        if (!seen[i]) {
            seen[i] = what;
            fprintf(stderr, "[graphics] %s: stub until M3/M5\n", what);
            return;
        }
    }
}

static wgpu::TextureFormat to_wgpu(Format f) {
    switch (f) {
        case Format::R32_FLOAT:        return wgpu::TextureFormat::R32Float;
        case Format::RGBA32_FLOAT:     return wgpu::TextureFormat::RGBA32Float;
        case Format::R32_UINT:         return wgpu::TextureFormat::R32Uint;
        case Format::RGBA8_UNORM:      return wgpu::TextureFormat::RGBA8Unorm;
        case Format::RGBA8_UNORM_SRGB: return wgpu::TextureFormat::RGBA8UnormSrgb;
        default:                       return wgpu::TextureFormat::Undefined;
    }
}

static uint32_t bytes_per_pixel(Format f) {
    switch (f) {
        case Format::R32_FLOAT: case Format::R32_UINT: return 4;
        case Format::RGBA32_FLOAT: return 16;
        case Format::RGBA8_UNORM: case Format::RGBA8_UNORM_SRGB: return 4;
        default: return 0;
    }
}

ConstantBuffer get_constant_buffer(uint32_t size) {
    ConstantBuffer cb = {};
    wgpu::BufferDescriptor desc = {};
    desc.size = (size + 15u) & ~15u;   // round to 16: uniform binding size floor
    desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    cb.buffer = g_ctx.device.CreateBuffer(&desc);
    cb.size = size;
    return cb;
}

void update_constant_buffer(ConstantBuffer *buffer, void *data) {
    // queue.WriteBuffer executes in queue order, i.e. BEFORE any command
    // buffer submitted later — including dispatches already recorded into
    // g_encoder but not yet flushed. Flush first so those dispatches see the
    // buffer contents as they stood when they were recorded (D3D11
    // Map(WRITE_DISCARD) call-order semantics).
    if (g_encoder) flush_commands();
    // Whole-buffer overwrite, like the D3D11 Map(WRITE_DISCARD)+memcpy.
    g_ctx.queue.WriteBuffer(buffer->buffer, 0, data, buffer->size);
}

void set_constant_buffer(ConstantBuffer *buffer, uint32_t slot) {
    // The original bound to all 4 stages at `slot`; main.cpp only ever uses
    // slot 0 (inventory §1). Fork contract: slot 0 == @group(0) @binding(0).
    assert(slot == 0 && "fork supports constant buffer slot 0 only");
    g_uniform_buffer = buffer->buffer;
    g_uniform_size = (buffer->size + 15u) & ~15u;
}

StructuredBuffer get_structured_buffer(int element_stride, int num_elements) {
    StructuredBuffer sb = {};
    wgpu::BufferDescriptor desc = {};
    desc.size = (uint64_t)element_stride * (uint64_t)num_elements;
    desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst |
                 wgpu::BufferUsage::CopySrc;   // CopySrc: readback path
    sb.buffer = g_ctx.device.CreateBuffer(&desc);
    sb.element_stride = (uint32_t)element_stride;
    sb.num_elements = (uint32_t)num_elements;
    sb.size = (uint32_t)desc.size;
    return sb;
}

void update_structured_buffer(StructuredBuffer *buffer, void *data) {
    // Same queue-order hazard as update_constant_buffer (see comment there).
    if (g_encoder) flush_commands();
    g_ctx.queue.WriteBuffer(buffer->buffer, 0, data, buffer->size);
}

static wgpu::Texture make_texture(wgpu::TextureDimension dim, uint32_t w, uint32_t h,
                                  uint32_t d, Format format) {
    wgpu::TextureDescriptor desc = {};
    desc.dimension = dim;
    desc.size = {w, h, d};
    desc.format = to_wgpu(format);
    desc.mipLevelCount = 1;
    desc.usage = wgpu::TextureUsage::TextureBinding |     // sampled (sr_view)
                 wgpu::TextureUsage::StorageBinding |     // storage (ua_view)
                 wgpu::TextureUsage::CopySrc |            // export/readback
                 wgpu::TextureUsage::CopyDst;             // initial-data upload
    return g_ctx.device.CreateTexture(&desc);
}

Texture2D get_texture2D(void *data, uint32_t width, uint32_t height, Format format,
                        uint32_t pixel_byte_count) {
    (void)pixel_byte_count; // format determines the real size
    Texture2D t = {};
    t.texture = make_texture(wgpu::TextureDimension::e2D, width, height, 1, format);
    t.sr_view = t.texture.CreateView();
    t.ua_view = t.texture.CreateView();
    t.width = width; t.height = height; t.format = format;
    if (data) {
        wgpu::TexelCopyTextureInfo dst = {};
        dst.texture = t.texture;
        wgpu::TexelCopyBufferLayout layout = {};
        layout.bytesPerRow = width * bytes_per_pixel(format);
        layout.rowsPerImage = height;
        wgpu::Extent3D extent = {width, height, 1};
        g_ctx.queue.WriteTexture(&dst, data, (uint64_t)layout.bytesPerRow * height,
                                 &layout, &extent);
    }
    return t;
}

Texture3D get_texture3D(void *data, uint32_t width, uint32_t height, uint32_t depth,
                        Format format, uint32_t pixel_byte_count) {
    (void)pixel_byte_count;
    Texture3D t = {};
    t.texture = make_texture(wgpu::TextureDimension::e3D, width, height, depth, format);
    t.sr_view = t.texture.CreateView();
    t.ua_view = t.texture.CreateView();
    t.width = width; t.height = height; t.depth = depth; t.format = format;
    if (data) {
        wgpu::TexelCopyTextureInfo dst = {};
        dst.texture = t.texture;
        wgpu::TexelCopyBufferLayout layout = {};
        layout.bytesPerRow = width * bytes_per_pixel(format);
        layout.rowsPerImage = height;
        wgpu::Extent3D extent = {width, height, depth};
        g_ctx.queue.WriteTexture(&dst, data,
                                 (uint64_t)layout.bytesPerRow * height * depth,
                                 &layout, &extent);
    }
    // NOTE the original leaves data==NULL textures uninitialized; WebGPU
    // zero-initializes. That is a (beneficial) difference: the original relied
    // on main.cpp clearing before use anyway (inventory §2). Nothing to do.
    return t;
}

DepthBuffer get_depth_buffer(uint32_t width, uint32_t height) {
    // Created by main.cpp but never bound (inventory §4) — real texture, inert.
    DepthBuffer db = {};
    wgpu::TextureDescriptor desc = {};
    desc.size = {width, height, 1};
    desc.format = wgpu::TextureFormat::Depth24PlusStencil8;
    desc.usage = wgpu::TextureUsage::RenderAttachment;
    db.texture = g_ctx.device.CreateTexture(&desc);
    db.ds_view = db.texture.CreateView();
    db.width = width; db.height = height;
    return db;
}

TextureSampler get_texture_sampler(SampleMode mode, Filter filter) {
    TextureSampler s = {};
    wgpu::SamplerDescriptor desc = {};
    wgpu::AddressMode am = mode == WRAP ? wgpu::AddressMode::Repeat
                        : wgpu::AddressMode::ClampToEdge; // BORDER: WebGPU has no
    // border color sampler in core; CLAMP is the closest. main.cpp only uses
    // CLAMP (inventory §1), so this is unreachable-in-practice.
    desc.addressModeU = am; desc.addressModeV = am; desc.addressModeW = am;
    if (filter == Filter::POINT) {
        desc.magFilter = wgpu::FilterMode::Nearest;
        desc.minFilter = wgpu::FilterMode::Nearest;
        desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
    } else {
        desc.magFilter = wgpu::FilterMode::Linear;
        desc.minFilter = wgpu::FilterMode::Linear;
        desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
        if (filter == Filter::ANISOTROPIC) desc.maxAnisotropy = 16;
    }
    s.sampler = g_ctx.device.CreateSampler(&desc);
    return s;
}

Texture2D load_texture2D(std::string filename) {
    // Palette TGAs load for real in M4 via stb_image; until then a 1x1 white
    // texture keeps bind groups valid.
    warn_once("load_texture2D");
    (void)filename;
    float white[4] = {1.f, 1.f, 1.f, 1.f};
    return get_texture2D(white, 1, 1, Format::RGBA32_FLOAT);
}

void save_texture3D(Texture3D *texture, std::string filename) {
    (void)texture; (void)filename; warn_once("save_texture3D");
}

void save_texture2D_HDR(Texture2D *texture, std::string filename) {
    (void)texture; (void)filename; warn_once("save_texture2D_HDR");
}

uint32_t capture_current_frame() { warn_once("capture_current_frame"); return 0; }

// NOTE: the storage-texture binding is named `tex_target`, not `target` —
// `target` is a reserved WGSL identifier (Dawn: "'target' is a reserved
// keyword"), which silently failed CreateShaderModule for all three clear
// kernels (found in Task 5 while wiring up assert()-validated GPU tests;
// see task-5-report.md for the deviation writeup).
static const char *CLEAR_TEX3D_WGSL = R"(
@group(0) @binding(0) var<uniform> clear_value : vec4<f32>;
@group(1) @binding(0) var tex_target : texture_storage_3d<r32float, write>;
@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(tex_target);
    if (gid.x >= dims.x || gid.y >= dims.y || gid.z >= dims.z) { return; }
    textureStore(tex_target, gid, clear_value);
}
)";

static const char *CLEAR_TEX2D_F_WGSL = R"(
@group(0) @binding(0) var<uniform> clear_value : vec4<f32>;
@group(1) @binding(0) var tex_target : texture_storage_2d<rgba32float, write>;
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(tex_target);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }
    textureStore(tex_target, gid.xy, clear_value);
}
)";

static const char *CLEAR_TEX2D_U_WGSL = R"(
@group(0) @binding(0) var<uniform> clear_value : vec4<u32>;
@group(1) @binding(0) var tex_target : texture_storage_2d<r32uint, write>;
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(tex_target);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }
    textureStore(tex_target, gid.xy, clear_value);
}
)";

static void ensure_clear_kernel(ClearKernel *k, const char *wgsl) {
    if (k->pipeline) return;
    wgpu::ShaderSourceWGSL src = {};
    src.code = wgsl;
    wgpu::ShaderModuleDescriptor mod_desc = {};
    mod_desc.nextInChain = &src;
    wgpu::ShaderModule module = g_ctx.device.CreateShaderModule(&mod_desc);
    wgpu::ComputePipelineDescriptor desc = {};
    desc.compute.module = module;
    k->pipeline = g_ctx.device.CreateComputePipeline(&desc);
    wgpu::BufferDescriptor bdesc = {};
    bdesc.size = 16;
    bdesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    k->uniform = g_ctx.device.CreateBuffer(&bdesc);
}

static void run_clear(ClearKernel *k, wgpu::TextureView view, const void *value16,
                      uint32_t gx, uint32_t gy, uint32_t gz) {
    // k->uniform is a shared per-kernel scratch buffer: two clears with
    // different values recorded into the same encoder would both read the
    // last WriteBuffer's value at execution time without this flush (same
    // queue-order hazard as update_constant_buffer above).
    if (g_encoder) flush_commands();
    g_ctx.queue.WriteBuffer(k->uniform, 0, value16, 16);
    wgpu::BindGroupEntry e0 = {};
    e0.binding = 0; e0.buffer = k->uniform; e0.size = 16;
    wgpu::BindGroupDescriptor g0 = {};
    g0.layout = k->pipeline.GetBindGroupLayout(0);
    g0.entryCount = 1; g0.entries = &e0;
    wgpu::BindGroup group0 = g_ctx.device.CreateBindGroup(&g0);
    wgpu::BindGroupEntry e1 = {};
    e1.binding = 0; e1.textureView = view;
    wgpu::BindGroupDescriptor g1 = {};
    g1.layout = k->pipeline.GetBindGroupLayout(1);
    g1.entryCount = 1; g1.entries = &e1;
    wgpu::BindGroup group1 = g_ctx.device.CreateBindGroup(&g1);

    ensure_encoder();
    wgpu::ComputePassEncoder pass = g_encoder.BeginComputePass();
    pass.SetPipeline(k->pipeline);
    pass.SetBindGroup(0, group0);
    pass.SetBindGroup(1, group1);
    pass.DispatchWorkgroups(gx, gy, gz);
    pass.End();
}

void clear_texture(Texture3D *texture, float value) {
    assert(texture->format == Format::R32_FLOAT);
    ensure_clear_kernel(&g_clear3d, CLEAR_TEX3D_WGSL);
    float v[4] = {value, value, value, value};
    run_clear(&g_clear3d, texture->ua_view, v,
              (texture->width + 3) / 4, (texture->height + 3) / 4,
              (texture->depth + 3) / 4);
}

void clear_texture(Texture2D *texture, float value) {
    assert(texture->format == Format::RGBA32_FLOAT);
    ensure_clear_kernel(&g_clear2d_f, CLEAR_TEX2D_F_WGSL);
    float v[4] = {value, value, value, value};
    run_clear(&g_clear2d_f, texture->ua_view, v,
              (texture->width + 7) / 8, (texture->height + 7) / 8, 1);
}

void clear_texture_uint(Texture2D *texture, uint32_t value) {
    assert(texture->format == Format::R32_UINT);
    ensure_clear_kernel(&g_clear2d_u, CLEAR_TEX2D_U_WGSL);
    uint32_t v[4] = {value, value, value, value};
    run_clear(&g_clear2d_u, texture->ua_view, v,
              (texture->width + 7) / 8, (texture->height + 7) / 8, 1);
}

void set_texture(Texture2D *t, uint32_t slot)  { (void)t; (void)slot; }
void set_texture(Texture3D *t, uint32_t slot)  { (void)t; (void)slot; }
void unset_texture(uint32_t slot)              { (void)slot; }
void set_texture_sampler(TextureSampler *s, uint32_t slot) { (void)s; (void)slot; }

void set_texture_compute(Texture2D *t, uint32_t slot)  { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::STORAGE_TEX; g_compute_slots[slot].view = t->ua_view; }
void set_texture_compute(Texture3D *t, uint32_t slot)  { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::STORAGE_TEX; g_compute_slots[slot].view = t->ua_view; }
void set_texture_sampled_compute(Texture2D *t, uint32_t slot) { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::SAMPLED_TEX; g_compute_slots[slot].view = t->sr_view; }
void set_texture_sampled_compute(Texture3D *t, uint32_t slot) { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::SAMPLED_TEX; g_compute_slots[slot].view = t->sr_view; }
void set_texture_sampler_compute(TextureSampler *s, uint32_t slot) { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::SAMPLER; g_compute_slots[slot].sampler = s->sampler; }
void unset_texture_compute(uint32_t slot)          { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; }
void unset_texture_sampled_compute(uint32_t slot)  { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; }


void set_blend_state(BlendType type) {
    g_blend = type;
}

Mesh get_mesh(void *vertices, uint32_t vertex_count, uint32_t vertex_stride,
              void *indices, uint32_t index_count, uint32_t index_byte_size,
              Topology topology) {
    // Real vertex buffer now (M3 draws it); index path unused by main.cpp
    // (inventory §1: both meshes are non-indexed) — assert it stays that way.
    assert(indices == nullptr && index_count == 0 && "fork: non-indexed meshes only");
    (void)index_byte_size;
    Mesh m = {};
    wgpu::BufferDescriptor desc = {};
    desc.size = ((uint64_t)vertex_count * vertex_stride + 3u) & ~3ull;
    desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    m.vertex_buffer = g_ctx.device.CreateBuffer(&desc);
    g_ctx.queue.WriteBuffer(m.vertex_buffer, 0, vertices,
                            (uint64_t)vertex_count * vertex_stride);
    m.vertex_stride = vertex_stride;
    m.vertex_count = vertex_count;
    m.topology = topology;
    return m;
}

void draw_mesh(Mesh *mesh) { (void)mesh; warn_once("draw_mesh"); }

void set_structured_buffer(StructuredBuffer *b, uint32_t slot) { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::STORAGE_BUFFER; g_compute_slots[slot].buffer = b->buffer; g_compute_slots[slot].buffer_size = b->size; }

void unset_structured_buffer(uint32_t slot)
{
    assert(slot < MAX_SLOTS);
    g_compute_slots[slot] = {};
}

void capture_structured_buffer(StructuredBuffer *buffer, void *mapped_data,
                               uint32_t num_elements, size_t element_size) {
    uint64_t byte_count = (uint64_t)num_elements * element_size;
    assert(byte_count <= buffer->size);
    if (!buffer->readback) {
        wgpu::BufferDescriptor desc = {};
        desc.size = buffer->size;
        desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        buffer->readback = g_ctx.device.CreateBuffer(&desc);
    }
    // Flush pending compute, copy, submit, block on map — reproducing the
    // D3D11 Map(D3D11_MAP_READ) same-frame stall (inventory §6). Latency is a
    // conscious non-goal here: correctness first, matching original behavior.
    ensure_encoder();
    g_encoder.CopyBufferToBuffer(buffer->buffer, 0, buffer->readback, 0, buffer->size);
    flush_commands();

    bool done = false;
    buffer->readback.MapAsync(
        wgpu::MapMode::Read, 0, buffer->size, wgpu::CallbackMode::AllowProcessEvents,
        [&done](wgpu::MapAsyncStatus status, wgpu::StringView message) {
            if (status != wgpu::MapAsyncStatus::Success)
                fprintf(stderr, "[graphics] readback map failed: %.*s\n",
                        (int)message.length, message.data);
            done = true;
        });
    wait_for(&done);
    const void *src = buffer->readback.GetConstMappedRange(0, buffer->size);
    if (src) memcpy(mapped_data, src, byte_count);
    buffer->readback.Unmap();
}

VertexShader get_vertex_shader_from_code(char *code, uint32_t code_length) {
    (void)code; (void)code_length;
    VertexShader vs = {}; vs.valid = true;   // M3 compiles real WGSL here
    return vs;
}
PixelShader get_pixel_shader_from_code(char *code, uint32_t code_length) {
    (void)code; (void)code_length;
    PixelShader ps = {}; ps.valid = true;
    return ps;
}

ComputeShader get_compute_shader_from_code(char *code, uint32_t code_length,
                                           const ShaderConstant *constants,
                                           uint32_t constant_count) {
    ComputeShader cs = {};
    // file_system::read_file guarantees a trailing NUL (Task 1), so `code` is
    // a valid C string; code_length is unused but kept for signature parity.
    (void)code_length;

    g_ctx.device.PushErrorScope(wgpu::ErrorFilter::Validation);

    wgpu::ShaderSourceWGSL src = {};
    src.code = code;
    wgpu::ShaderModuleDescriptor mod_desc = {};
    mod_desc.nextInChain = &src;
    wgpu::ShaderModule module = g_ctx.device.CreateShaderModule(&mod_desc);

    std::vector<wgpu::ConstantEntry> entries(constant_count);
    for (uint32_t i = 0; i < constant_count; i++) {
        entries[i] = {};
        entries[i].key = constants[i].name;
        entries[i].value = constants[i].value;
    }
    wgpu::ComputePipelineDescriptor desc = {};
    desc.compute.module = module;
    desc.compute.constantCount = constant_count;
    desc.compute.constants = entries.data();
    cs.pipeline = g_ctx.device.CreateComputePipeline(&desc);

    bool done = false, had_error = false;
    g_ctx.device.PopErrorScope(
        wgpu::CallbackMode::AllowProcessEvents,
        [&done, &had_error](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                            wgpu::StringView message) {
            if (type != wgpu::ErrorType::NoError) {
                had_error = true;
                fprintf(stderr, "[graphics] shader/pipeline error: %.*s\n",
                        (int)message.length, message.data);
            }
            done = true;
        });
    wait_for(&done);
    cs.valid = !had_error;

    // Comment-safe enough for our controlled sources: every sim shader that
    // uses a uniform declares it as literally "@group(0)".
    cs.uses_group0 = (strstr(code, "@group(0)") != NULL);
    return cs;
}

void set_vertex_shader(VertexShader *shader) { (void)shader; }
void set_pixel_shader(PixelShader *shader)   { (void)shader; }

void set_compute_shader(ComputeShader *shader) {
    g_compute_shader = shader;
}

void run_compute(int gx, int gy, int gz) {
    assert(g_compute_shader && g_compute_shader->valid);
    ensure_encoder();

    // Group 0: uniform at binding 0, if the shader declares one.
    // Group 1: every currently-bound slot. CONTRACT: the bound slot set must
    // exactly match the shader's @group(1) declarations — extra or missing
    // entries are a Dawn validation error (which names the binding; that is
    // the intended failure mode, better than D3D11's silent null reads).
    wgpu::BindGroup group0;
    if (g_compute_shader->uses_group0) {
        if (!g_uniform_buffer) {
            fatal("run_compute: shader declares @group(0) uniform but no constant buffer is bound (set_constant_buffer slot 0)");
        }
        wgpu::BindGroupEntry e = {};
        e.binding = 0; e.buffer = g_uniform_buffer; e.size = g_uniform_size;
        wgpu::BindGroupDescriptor d = {};
        d.layout = g_compute_shader->pipeline.GetBindGroupLayout(0);
        d.entryCount = 1; d.entries = &e;
        group0 = g_ctx.device.CreateBindGroup(&d);
    }
    // Shader without @group(0): never bind group 0, even if shadow state holds a uniform.

    std::vector<wgpu::BindGroupEntry> entries;
    for (uint32_t i = 0; i < MAX_SLOTS; i++) {
        const BoundSlot &s = g_compute_slots[i];
        if (s.kind == BoundSlot::Kind::NONE) continue;
        wgpu::BindGroupEntry e = {};
        e.binding = i;
        switch (s.kind) {
            case BoundSlot::Kind::STORAGE_BUFFER: e.buffer = s.buffer; e.size = s.buffer_size; break;
            case BoundSlot::Kind::STORAGE_TEX:
            case BoundSlot::Kind::SAMPLED_TEX:    e.textureView = s.view; break;
            case BoundSlot::Kind::SAMPLER:        e.sampler = s.sampler; break;
            default: break;
        }
        entries.push_back(e);
    }
    wgpu::BindGroupDescriptor d1 = {};
    d1.layout = g_compute_shader->pipeline.GetBindGroupLayout(1);
    d1.entryCount = entries.size(); d1.entries = entries.data();
    wgpu::BindGroup group1 = g_ctx.device.CreateBindGroup(&d1);

    wgpu::ComputePassEncoder pass = g_encoder.BeginComputePass();
    pass.SetPipeline(g_compute_shader->pipeline);
    if (group0) pass.SetBindGroup(0, group0);
    pass.SetBindGroup(1, group1);
    pass.DispatchWorkgroups((uint32_t)gx, (uint32_t)gy, (uint32_t)gz);
    pass.End();
}

bool is_ready(RenderTarget *p)     { return p->is_window || p->rt_view != nullptr; }
bool is_ready(DepthBuffer *p)      { return p->ds_view != nullptr; }
bool is_ready(Texture2D *p)        { return p->texture != nullptr; }
bool is_ready(Texture3D *p)        { return p->texture != nullptr; }
bool is_ready(Mesh *p)             { return p->vertex_buffer != nullptr; }
bool is_ready(ConstantBuffer *p)   { return p->buffer != nullptr; }
bool is_ready(StructuredBuffer *p) { return p->buffer != nullptr; }
bool is_ready(TextureSampler *p)   { return p->sampler != nullptr; }
bool is_ready(VertexShader *p)     { return p->valid; }
bool is_ready(PixelShader *p)      { return p->valid; }
bool is_ready(ComputeShader *p)    { return p->valid; }

void release(RenderTarget *p)     { *p = {}; }
void release(DepthBuffer *p)      { *p = {}; }
void release(Texture2D *p)        { *p = {}; }
void release(Texture3D *p)        { *p = {}; }
void release(Mesh *p)             { *p = {}; }
void release(ConstantBuffer *p)   { *p = {}; }
void release(StructuredBuffer *p) { *p = {}; }
void release(TextureSampler *p)   { *p = {}; }
void release(VertexShader *p)     { *p = {}; }
void release(PixelShader *p)      { *p = {}; }
void release(ComputeShader *p)    { *p = {}; }

}
