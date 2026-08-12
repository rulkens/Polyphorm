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
    enum class Kind { NONE, STORAGE_BUFFER, STORAGE_TEX, SAMPLED_TEX } kind = Kind::NONE;
    wgpu::Buffer buffer;
    uint64_t buffer_size = 0;
    wgpu::TextureView view;
};
static const uint32_t MAX_SLOTS = 16;
static BoundSlot g_compute_slots[MAX_SLOTS];
// Independent per-slot compute-sampler storage (DESIGN §6.3): a sampled
// texture (g_compute_slots[slot], Kind::SAMPLED_TEX) and its paired sampler
// must coexist at the same slot number — cs_volpath's WGSL binds both a
// texture_2d and a sampler at the same logical slot. Storing the sampler in
// BoundSlot itself (the pre-fix shape) meant set_texture_sampler_compute's
// `g_compute_slots[slot] = {}` reset erased whichever resource had just been
// written to that slot, and vice versa. Compute samplers bind at
// @group(1) @binding(MAX_SLOTS + slot) = 16 + N: resources keep binding==slot
// so all pre-M4 shaders are unchanged; cs_volpath's WGSL (M4b) declares
// @binding(17/18/19/20) for its s1/s2/s3/s4 samplers.
static wgpu::Sampler g_compute_samplers[MAX_SLOTS];
static wgpu::Buffer g_uniform_buffer;            // group 0 binding 0
static uint64_t g_uniform_size = 0;
static ComputeShader *g_compute_shader = nullptr;

// Render-path shadow state (Task 1 / M3, render-path-design.md §2/§5).
// Render slots are independent of g_compute_slots and are a {view, sampler}
// PAIR per slot (not a tagged union) — a texture write and a sampler write
// to the same slot must both survive, unlike the compute path's single-`kind`
// BoundSlot (design §5's documented divergence from the compute convention).
static VertexShader *g_vertex_shader = nullptr;
static PixelShader  *g_pixel_shader  = nullptr;
static RenderTarget  g_render_target = {};
struct RenderSlot { wgpu::TextureView view; wgpu::Sampler sampler; };
static RenderSlot g_render_slots[MAX_SLOTS];

// Lazy render-pipeline cache (design §2). Cache key deliberately omits
// depth-attachment presence: depth is never bound for the whole M3/M4
// lifetime (§7). Design §2 also proposed omitting target FORMAT from the
// key, reasoning main.cpp always draws to the window's fixed sRGB BGRA8
// view — but that assumption holds only for main.cpp, not for this task's
// own headless tests: render_path_tests draws into an offscreen
// RGBA32_FLOAT RenderTarget (via get_render_target) to make draw_mesh
// verifiable without a window. A hardcoded BGRA8UnormSrgb target format
// would make that draw fail Dawn's render-pass/pipeline attachment-
// compatibility validation outright. DEVIATION: target format IS included
// in the key and in the pipeline descriptor (derived from g_render_target
// at draw time — window => BGRA8UnormSrgb matching window_view(), offscreen
// => RenderTarget::format) — this is exactly the widening design §2's own
// risk register anticipated ("a future milestone that adds an offscreen
// target... knows to widen the key rather than silently reuse a wrong-
// format cached pipeline"), done now because Task 1 introduces that
// offscreen target itself. main.cpp's real M3/M4 draws are unaffected: they
// only ever target the window, so this always resolves to the same
// BGRA8UnormSrgb entry design §2 assumed.
struct PipelineCacheEntry {
    WGPUShaderModule vs = nullptr, ps = nullptr;
    BlendType blend = BlendType::OPAQUE;
    uint32_t stride = 0;
    wgpu::TextureFormat target_format = wgpu::TextureFormat::Undefined;
    // DESIGN §6.4: folded into the key (previously omitted, a comment-only
    // caveat) — TRIANGLESTRIP is currently unused by any shipped shader so
    // this was latent, but a cache hit that silently reused a TRIANGLELIST
    // pipeline for a TRIANGLESTRIP mesh (same vs/ps/blend/stride/format)
    // would be a real, hard-to-diagnose bug once TRIANGLESTRIP is exercised.
    Topology topology = Topology::TRIANGLELIST;
    wgpu::RenderPipeline pipeline;
};
static std::vector<PipelineCacheEntry> g_pipeline_cache;

// One lazily-built pipeline + scratch uniform per builtin clear kernel
// (declared here, ahead of release(), so release() can reset them; the WGSL
// sources and ensure_clear_kernel()/run_clear() stay further down near the
// clear_texture* entry points).
struct ClearKernel {
    wgpu::ComputePipeline pipeline;
    wgpu::Buffer uniform;   // 16 bytes
};
static ClearKernel g_clear3d, g_clear2d_f, g_clear2d_u;

// Forward declarations: get_render_target (below) needs the format
// conversion helper that's otherwise defined further down, next to the
// other texture helpers it belongs with.
static wgpu::TextureFormat to_wgpu(Format f);

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

void resize_surface(uint32_t fb_width, uint32_t fb_height) {
    // Minimized/degenerate window: skip Configure entirely (Dawn requires a
    // positive extent). The caller (main.cpp) is expected to also skip
    // rendering for that frame — see graphics.h's resize_surface comment.
    if (fb_width == 0 || fb_height == 0) return;
    gpu::resize_surface(&g_gpu, fb_width, fb_height);
}

RenderTarget get_render_target_window() {
    RenderTarget rt = {};
    rt.is_window = true;
    rt.width = g_gpu.width;
    rt.height = g_gpu.height;
    return rt;
}

// M3: real offscreen render target. Never called by main.cpp (all
// "offscreen" buffers in the original are compute-written textures, not
// render targets — inventory §3) but implemented for real here because
// render_path_tests needs a headless draw_mesh target with a readback path
// (RenderAttachment for draw_mesh's own pass, CopySrc for the test's
// CopyTextureToBuffer readback).
RenderTarget get_render_target(uint32_t width, uint32_t height, Format format) {
    RenderTarget rt = {};
    wgpu::TextureDescriptor desc = {};
    desc.size = {width, height, 1};
    desc.format = to_wgpu(format);
    desc.mipLevelCount = 1;
    desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    rt.texture = g_ctx.device.CreateTexture(&desc);
    rt.rt_view = rt.texture.CreateView();
    rt.width = width; rt.height = height;
    rt.is_window = false;
    rt.format = format;
    return rt;
}

// Acquire the surface texture for this frame if not already held.
// Format kept in sync with get_window_surface_format() below — single
// source of truth (M4a design §1.3); if this hardcoded format ever changes,
// update that accessor too.
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

wgpu::TextureFormat get_window_surface_format() { return wgpu::TextureFormat::BGRA8UnormSrgb; }

void set_render_targets_viewport(RenderTarget *buffer) {
    // D3D11 version set OM targets + viewport. WebGPU render passes carry the
    // target; viewport is full-target by default (design §4 — no SetViewport
    // call needed anywhere: the default viewport already spans the full
    // color-attachment extent, matching "main.cpp never sets a
    // partial/custom viewport"). Recording `g_render_target` here is the only
    // way draw_mesh (which takes no target argument) knows where to render.
    g_render_target = *buffer;
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

// M4a design §2.2: the ImGui render-pass entry point. LoadOp::Load means
// this draws on top of whatever the scene pass(es) already wrote to the
// window view this frame. Contract: begin_ui_pass/end_ui_pass are invoked by
// the frame-end hook (registered by ui::init, see graphics::set_frame_end_hook),
// which swap_frames() runs after all scene passes are recorded and before
// submit/Present. ui::end() is inert and kept only for upstream compatibility.
wgpu::RenderPassEncoder begin_ui_pass() {
    ensure_encoder();
    wgpu::RenderPassColorAttachment att = {};
    att.view = window_view();
    att.loadOp = wgpu::LoadOp::Load;
    att.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor pass_desc = {};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &att;
    return g_encoder.BeginRenderPass(&pass_desc);
}
void end_ui_pass(wgpu::RenderPassEncoder pass) { pass.End(); }

static void (*g_frame_end_hook)() = nullptr;
void set_frame_end_hook(void (*hook)()) { g_frame_end_hook = hook; }

void swap_frames() {
    // M4a fix round 1: run before flush_commands() — this is the one place
    // guaranteed to be after every scene draw_mesh call and every ui::
    // widget call this frame, and before this frame's commands are
    // submitted/presented (graphics.h's set_frame_end_hook comment).
    if (g_frame_end_hook) g_frame_end_hook();
    flush_commands();
    if (g_surface_tex_acquired) {
        g_gpu.surface.Present();
        g_surface_tex = {};
        g_surface_tex_acquired = false;
    }
}

void release() {
    g_frame_end_hook = nullptr;
    flush_commands();
    g_uniform_buffer = nullptr;
    for (uint32_t i = 0; i < MAX_SLOTS; i++) { g_compute_slots[i] = {}; g_compute_samplers[i] = nullptr; }
    g_compute_shader = nullptr;
    g_blend = BlendType::OPAQUE;
    g_clear3d = {};
    g_clear2d_f = {};
    g_clear2d_u = {};
    // Render-path teardown (risk #7: cached wgpu::RenderPipeline handles must
    // not silently outlive device teardown).
    g_pipeline_cache.clear();
    g_vertex_shader = nullptr;
    g_pixel_shader = nullptr;
    g_render_target = {};
    for (uint32_t i = 0; i < MAX_SLOTS; i++) g_render_slots[i] = {};
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

void clear_structured_buffer(StructuredBuffer *buffer) {
    ensure_encoder();
    g_encoder.ClearBuffer(buffer->buffer, 0, buffer->size);
}

// Render bind convention (design §5): texture at slot N -> @group(1)
// @binding(2N), sampler at slot N -> @binding(2N+1). This is the ONE place
// the render path deliberately diverges from the compute path's
// binding==slot(+MAX_SLOTS for samplers, DESIGN §6.3) convention — every
// render call site pairs a texture and its sampler at the SAME slot number,
// so a literal binding==slot scheme would put both at the same WGSL binding
// (a hard Dawn validation error). Each render slot stores an independent
// view AND sampler together in one RenderSlot struct; the compute path
// achieves the same texture/sampler independence via two separate parallel
// arrays (g_compute_slots' single-`kind` tagged union for the resource part,
// g_compute_samplers for the sampler part) rather than a combined struct —
// an implementation-detail difference, not a semantic one.
void set_texture(Texture2D *t, uint32_t slot)  { assert(slot < MAX_SLOTS); g_render_slots[slot].view = t->sr_view; }
void set_texture(Texture3D *t, uint32_t slot)  { assert(slot < MAX_SLOTS); g_render_slots[slot].view = t->sr_view; }
// Clears BOTH .view and .sampler (DESIGN §6.1 / I1a). Previously this only
// cleared .view, leaving a stale sampler-only entry in the slot; the next
// draw_mesh call would still emit a bind-group entry at binding 2N+1 for
// that slot even though no texture is bound there, poisoning bind group 1
// for any pixel shader that doesn't declare a matching sampler binding — a
// hard Dawn validation error. No caller ever wants to unset a texture while
// keeping its sampler bound, so both fields are cleared together.
void unset_texture(uint32_t slot)              { assert(slot < MAX_SLOTS); g_render_slots[slot].view = nullptr; g_render_slots[slot].sampler = nullptr; }
void set_texture_sampler(TextureSampler *s, uint32_t slot) { assert(slot < MAX_SLOTS); g_render_slots[slot].sampler = s->sampler; }

// Resource setters write ONLY the resource part of the slot (kind/buffer/
// view) — the paired sampler (if any) at g_compute_samplers[slot] survives
// untouched, so set_texture_sampled_compute + set_texture_sampler_compute can
// be called in either order without one erasing the other (DESIGN §6.3).
void set_texture_compute(Texture2D *t, uint32_t slot)  { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::STORAGE_TEX; g_compute_slots[slot].view = t->ua_view; }
void set_texture_compute(Texture3D *t, uint32_t slot)  { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::STORAGE_TEX; g_compute_slots[slot].view = t->ua_view; }
void set_texture_sampled_compute(Texture2D *t, uint32_t slot) { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::SAMPLED_TEX; g_compute_slots[slot].view = t->sr_view; }
void set_texture_sampled_compute(Texture3D *t, uint32_t slot) { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::SAMPLED_TEX; g_compute_slots[slot].view = t->sr_view; }
// Writes ONLY the sampler field — no longer zeroes the resource part of the
// slot (the pre-fix bug: this used to reset g_compute_slots[slot], erasing
// whatever set_texture_sampled_compute had just written there).
void set_texture_sampler_compute(TextureSampler *s, uint32_t slot) { assert(slot < MAX_SLOTS); g_compute_samplers[slot] = s->sampler; }
void unset_texture_compute(uint32_t slot)          { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; }
// graphics.h has no dedicated compute-sampler-unset function, so this clears
// BOTH the resource and sampler fields of the slot — mirroring unset_texture's
// render-side rationale (I1a): no caller ever separates unsetting a sampled
// texture from unsetting its paired sampler.
void unset_texture_sampled_compute(uint32_t slot)  { assert(slot < MAX_SLOTS); g_compute_slots[slot] = {}; g_compute_samplers[slot] = nullptr; }


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

static wgpu::PrimitiveTopology to_wgpu_topology(Topology t) {
    switch (t) {
        case Topology::TRIANGLESTRIP: return wgpu::PrimitiveTopology::TriangleStrip;
        case Topology::TRIANGLELIST:
        default:                      return wgpu::PrimitiveTopology::TriangleList;
    }
}

// Vertex layout table (design §3): exactly two vertex shapes exist in the
// whole program. Explicit switch, not a generic format-inference/shader-
// source parser (rejects reviving the D3D11 original's buggy
// get_vertex_input_desc_from_shader tokenizer) — a third stride is a loud
// fatal(), not a silent guess.
static void fill_vertex_attributes(uint32_t stride, wgpu::VertexAttribute *attrs) {
    switch (stride) {
        case 24:
            attrs[0] = {}; attrs[0].format = wgpu::VertexFormat::Float32x4;
            attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
            attrs[1] = {}; attrs[1].format = wgpu::VertexFormat::Float32x2;
            attrs[1].offset = 16; attrs[1].shaderLocation = 1;
            return;
        case 28:
            attrs[0] = {}; attrs[0].format = wgpu::VertexFormat::Float32x4;
            attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
            attrs[1] = {}; attrs[1].format = wgpu::VertexFormat::Float32x3;
            attrs[1].offset = 16; attrs[1].shaderLocation = 1;
            return;
        default: {
            char msg[128];
            snprintf(msg, sizeof(msg), "draw_mesh: no vertex layout for stride %u", stride);
            fatal(msg);
        }
    }
}

static wgpu::RenderPipeline build_pipeline(VertexShader *vs, PixelShader *ps,
                                           BlendType blend, uint32_t stride,
                                           Topology topology,
                                           wgpu::TextureFormat target_format) {
    wgpu::VertexAttribute attrs[2];
    fill_vertex_attributes(stride, attrs);

    wgpu::VertexBufferLayout vbuf_layout = {};
    vbuf_layout.arrayStride = stride;
    vbuf_layout.stepMode = wgpu::VertexStepMode::Vertex;
    vbuf_layout.attributeCount = 2;
    vbuf_layout.attributes = attrs;

    wgpu::VertexState vertex_state = {};
    vertex_state.module = vs->module;
    vertex_state.bufferCount = 1;
    vertex_state.buffers = &vbuf_layout;

    // Blend mapping (design §2b, from the D3D11 original's recorded
    // behaviour): OPAQUE = disabled (blend=nullptr); ALPHA = src-alpha/
    // inv-src-alpha on color, same formula on alpha (documented guess — the
    // alpha-channel op wasn't independently recorded, but is unobservable in
    // M3 since ps_particles_color always writes a=1.0; risk #6).
    wgpu::BlendState blend_state = {};
    blend_state.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha,
                         wgpu::BlendFactor::OneMinusSrcAlpha};
    blend_state.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha,
                         wgpu::BlendFactor::OneMinusSrcAlpha};

    wgpu::ColorTargetState color_target = {};
    color_target.format = target_format;  // window => BGRA8UnormSrgb (window_view()'s
    // format); offscreen => RenderTarget::format (see PipelineCacheEntry's
    // comment for why this isn't hardcoded).
    color_target.blend = (blend == BlendType::ALPHA) ? &blend_state : nullptr;

    wgpu::FragmentState fragment_state = {};
    fragment_state.module = ps->module;
    fragment_state.targetCount = 1;
    fragment_state.targets = &color_target;

    wgpu::RenderPipelineDescriptor desc = {};
    desc.vertex = vertex_state;
    desc.fragment = &fragment_state;
    desc.primitive.topology = to_wgpu_topology(topology);
    desc.depthStencil = nullptr;   // depth out of scope for M3/M4 (design §7)

    g_ctx.device.PushErrorScope(wgpu::ErrorFilter::Validation);
    wgpu::RenderPipeline pipeline = g_ctx.device.CreateRenderPipeline(&desc);
    bool done = false, had_error = false;
    g_ctx.device.PopErrorScope(
        wgpu::CallbackMode::AllowProcessEvents,
        [&done, &had_error](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                            wgpu::StringView message) {
            if (type != wgpu::ErrorType::NoError) {
                had_error = true;
                fprintf(stderr, "[graphics] render pipeline error: %.*s\n",
                        (int)message.length, message.data);
            }
            done = true;
        });
    wait_for(&done);
    if (had_error) {
        fatal("draw_mesh: render pipeline creation failed for the current "
              "vertex/pixel shader pair (see Dawn validation error above)");
    }
    return pipeline;
}

void draw_mesh(Mesh *mesh) {
    assert(g_vertex_shader && g_vertex_shader->valid && g_pixel_shader && g_pixel_shader->valid);

    // Lazy pipeline cache (design §2): linear scan on .Get() pointer
    // equality. Cache key omits depth-attachment presence (constant: never
    // bound, §7) but DOES include target format — see g_pipeline_cache's
    // declaration comment for why that widens design §2's original proposal.
    WGPUShaderModule vs_key = g_vertex_shader->module.Get();
    WGPUShaderModule ps_key = g_pixel_shader->module.Get();
    // Target format: the window's fixed sRGB BGRA8 view (matches
    // window_view()) for the window target, else the offscreen
    // RenderTarget's own format (see PipelineCacheEntry's comment).
    wgpu::TextureFormat target_format = g_render_target.is_window
        ? wgpu::TextureFormat::BGRA8UnormSrgb
        : to_wgpu(g_render_target.format);
    wgpu::RenderPipeline pipeline;
    for (auto &entry : g_pipeline_cache) {
        if (entry.vs == vs_key && entry.ps == ps_key && entry.blend == g_blend &&
            entry.stride == mesh->vertex_stride && entry.target_format == target_format &&
            entry.topology == mesh->topology) {
            pipeline = entry.pipeline;
            break;
        }
    }
    if (!pipeline) {
        pipeline = build_pipeline(g_vertex_shader, g_pixel_shader, g_blend,
                                  mesh->vertex_stride, mesh->topology, target_format);
        PipelineCacheEntry entry;
        entry.vs = vs_key; entry.ps = ps_key; entry.blend = g_blend;
        entry.stride = mesh->vertex_stride; entry.target_format = target_format;
        entry.topology = mesh->topology;
        entry.pipeline = pipeline;
        g_pipeline_cache.push_back(entry);
    }

    // Bind group 0 (uniform), only if either stage declares @group(0) —
    // mirrors run_compute's uses_group0 gate exactly (design §5). Stage
    // visibility comes free from the pipeline's auto bind-group-layout.
    wgpu::BindGroup group0;
    bool needs_group0 = g_vertex_shader->uses_group0 || g_pixel_shader->uses_group0;
    if (needs_group0) {
        if (!g_uniform_buffer) {
            fatal("draw_mesh: shader declares @group(0) uniform but no constant buffer is bound (set_constant_buffer slot 0)");
        }
        wgpu::BindGroupEntry e = {};
        e.binding = 0; e.buffer = g_uniform_buffer; e.size = g_uniform_size;
        wgpu::BindGroupDescriptor d = {};
        d.layout = pipeline.GetBindGroupLayout(0);
        d.entryCount = 1; d.entries = &e;
        group0 = g_ctx.device.CreateBindGroup(&d);
    }

    // Bind group 1: textures/samplers by the 2N/2N+1 convention (design §5).
    std::vector<wgpu::BindGroupEntry> entries;
    for (uint32_t slot = 0; slot < MAX_SLOTS; slot++) {
        const RenderSlot &s = g_render_slots[slot];
        if (s.view) {
            wgpu::BindGroupEntry e = {};
            e.binding = 2 * slot; e.textureView = s.view;
            entries.push_back(e);
        }
        if (s.sampler) {
            wgpu::BindGroupEntry e = {};
            e.binding = 2 * slot + 1; e.sampler = s.sampler;
            entries.push_back(e);
        }
    }
    wgpu::BindGroupDescriptor d1 = {};
    d1.layout = pipeline.GetBindGroupLayout(1);
    d1.entryCount = entries.size(); d1.entries = entries.data();
    wgpu::BindGroup group1 = g_ctx.device.CreateBindGroup(&d1);

    ensure_encoder();
    wgpu::TextureView view = g_render_target.is_window ? window_view() : g_render_target.rt_view;
    wgpu::RenderPassColorAttachment att = {};
    att.view = view;
    att.loadOp = wgpu::LoadOp::Load;    // preserve the prior clear/draw (design §4)
    att.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor pass_desc = {};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &att;
    wgpu::RenderPassEncoder pass = g_encoder.BeginRenderPass(&pass_desc);
    pass.SetPipeline(pipeline);
    if (group0) pass.SetBindGroup(0, group0);
    pass.SetBindGroup(1, group1);
    pass.SetVertexBuffer(0, mesh->vertex_buffer);
    pass.Draw(mesh->vertex_count);
    pass.End();
}

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

// Mirrors get_compute_shader_from_code's error-scope validation pattern
// exactly (PushErrorScope -> CreateShaderModule -> PopErrorScope callback ->
// wait_for -> valid = !had_error). Each compiles ONE file with ONE entry
// point named `main` (@vertex or @fragment) — vs and ps stay in separate
// files/modules (design §1); wgpu::RenderPipelineDescriptor natively accepts
// two distinct ShaderModules, so no call-site restructuring is needed.
//
// NOTE on `is_ready`: for render shaders this means "this WGSL module
// compiled," a WEAKER guarantee than compute's "pipeline built" (design §1).
// CreateShaderModule only proves the WGSL parses/type-checks in isolation —
// it does NOT prove compatibility with a specific vertex-buffer stride,
// blend target format, or its paired stage. Those errors surface only at
// draw_mesh's CreateRenderPipeline call, necessarily later, because stride/
// blend/format aren't known until a Mesh is drawn.
VertexShader get_vertex_shader_from_code(char *code, uint32_t code_length) {
    (void)code_length;
    g_ctx.device.PushErrorScope(wgpu::ErrorFilter::Validation);

    wgpu::ShaderSourceWGSL src = {};
    src.code = code;
    wgpu::ShaderModuleDescriptor mod_desc = {};
    mod_desc.nextInChain = &src;
    wgpu::ShaderModule module = g_ctx.device.CreateShaderModule(&mod_desc);

    bool done = false, had_error = false;
    g_ctx.device.PopErrorScope(
        wgpu::CallbackMode::AllowProcessEvents,
        [&done, &had_error](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                            wgpu::StringView message) {
            if (type != wgpu::ErrorType::NoError) {
                had_error = true;
                fprintf(stderr, "[graphics] vertex shader error: %.*s\n",
                        (int)message.length, message.data);
            }
            done = true;
        });
    wait_for(&done);

    VertexShader vs = {};
    vs.module = module;
    vs.valid = !had_error;
    vs.uses_group0 = (strstr(code, "@group(0)") != NULL);
    return vs;
}

PixelShader get_pixel_shader_from_code(char *code, uint32_t code_length) {
    (void)code_length;
    g_ctx.device.PushErrorScope(wgpu::ErrorFilter::Validation);

    wgpu::ShaderSourceWGSL src = {};
    src.code = code;
    wgpu::ShaderModuleDescriptor mod_desc = {};
    mod_desc.nextInChain = &src;
    wgpu::ShaderModule module = g_ctx.device.CreateShaderModule(&mod_desc);

    bool done = false, had_error = false;
    g_ctx.device.PopErrorScope(
        wgpu::CallbackMode::AllowProcessEvents,
        [&done, &had_error](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                            wgpu::StringView message) {
            if (type != wgpu::ErrorType::NoError) {
                had_error = true;
                fprintf(stderr, "[graphics] pixel shader error: %.*s\n",
                        (int)message.length, message.data);
            }
            done = true;
        });
    wait_for(&done);

    PixelShader ps = {};
    ps.module = module;
    ps.valid = !had_error;
    ps.uses_group0 = (strstr(code, "@group(0)") != NULL);
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

void set_vertex_shader(VertexShader *shader) { g_vertex_shader = shader; }
void set_pixel_shader(PixelShader *shader)   { g_pixel_shader = shader; }

void set_compute_shader(ComputeShader *shader) {
    g_compute_shader = shader;
}

void run_compute(int gx, int gy, int gz) {
    assert(g_compute_shader && g_compute_shader->valid);
    ensure_encoder();

    // Group 0: uniform at binding 0, if the shader declares one.
    // Group 1: every currently-bound resource slot (g_compute_slots, binding
    // == slot) PLUS every currently-bound sampler slot (g_compute_samplers,
    // binding == MAX_SLOTS + slot = 16 + N — DESIGN §6.3, set alongside
    // resource entries in the two loops below). CONTRACT: the union of both
    // bound sets must exactly match the shader's @group(1) declarations —
    // extra or missing entries in either array are a Dawn validation error
    // (which names the binding; that is the intended failure mode, better
    // than D3D11's silent null reads).
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
            default: break;
        }
        entries.push_back(e);
    }
    // Compute samplers bind at MAX_SLOTS + slot: resources keep binding==slot
    // so all pre-M4 shaders are unchanged; cs_volpath's WGSL (M4b) declares
    // @binding(17/18/19/20) for its s1/s2/s3/s4 samplers.
    for (uint32_t i = 0; i < MAX_SLOTS; i++) {
        if (!g_compute_samplers[i]) continue;
        wgpu::BindGroupEntry e = {};
        e.binding = MAX_SLOTS + i;
        e.sampler = g_compute_samplers[i];
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
