// Headless render-path tests (M3 Task 1). Same init pattern as
// tests/shader_compile_tests.cpp (graphics::init()/release(), no window/
// surface). Exercises: vs/ps compile validation, and a full offscreen
// draw_mesh + pixel readback that kills DESIGN risk #1 (texture/sampler
// 2N/2N+1 bind convention) and risk #4 (pipeline validation surfaces only
// at draw_mesh time) headlessly. See docs/superpowers/research/m3/
// render-path-design.md.
#include "../cpplib/graphics.h"
#include "../cpplib/file_system.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

// ---- Test 1 shaders: trivial compile validation ----

static const char *TRIVIAL_VS_WGSL = R"(
@vertex
fn main(@location(0) position : vec4<f32>) -> @builtin(position) vec4<f32> {
    return position;
}
)";

static const char *TRIVIAL_PS_WGSL = R"(
@fragment
fn main() -> @location(0) vec4<f32> {
    return vec4<f32>(1.0, 1.0, 1.0, 1.0);
}
)";

static void test_shader_compile_validation() {
    graphics::VertexShader vs =
        graphics::get_vertex_shader_from_code((char *)TRIVIAL_VS_WGSL, (uint32_t)strlen(TRIVIAL_VS_WGSL));
    assert(graphics::is_ready(&vs));
    assert(!vs.uses_group0);

    graphics::PixelShader ps =
        graphics::get_pixel_shader_from_code((char *)TRIVIAL_PS_WGSL, (uint32_t)strlen(TRIVIAL_PS_WGSL));
    assert(graphics::is_ready(&ps));
    assert(!ps.uses_group0);

    char bad[] = "this is not valid wgsl at all {{{";
    graphics::VertexShader bad_vs = graphics::get_vertex_shader_from_code(bad, sizeof(bad));
    assert(!graphics::is_ready(&bad_vs));

    graphics::PixelShader bad_ps = graphics::get_pixel_shader_from_code(bad, sizeof(bad));
    assert(!graphics::is_ready(&bad_ps));

    graphics::release(&vs);
    graphics::release(&ps);
    graphics::release(&bad_vs);
    graphics::release(&bad_ps);
    printf("render_path_tests: shader compile validation passed\n");
}

// ---- Test 2 shaders: fullscreen-quad passthrough sampling a texture ----
// Vertex input matches main.cpp's quad_vertices 24 B layout exactly:
// @location(0) float32x4 position, @location(1) float32x2 texcoord.

static const char *QUAD_VS_WGSL = R"(
struct VSOut {
    @builtin(position) position : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn main(@location(0) position : vec4<f32>, @location(1) texcoord : vec2<f32>) -> VSOut {
    var out : VSOut;
    out.position = position;
    out.uv = texcoord;
    return out;
}
)";

// Binds via the render 2N/2N+1 convention: texture at slot 0 -> binding 0,
// sampler at slot 0 -> binding 1 (design §5). This is the exact convention
// this test exercises against DESIGN risk #1.
static const char *SAMPLE_PS_WGSL = R"(
@group(1) @binding(0) var tex : texture_2d<f32>;
@group(1) @binding(1) var samp : sampler;

@fragment
fn main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    let v = textureSample(tex, samp, uv).x;
    return vec4<f32>(v, v, v, 1.0);
}
)";

// The 6-vertex 24 B quad layout from main.cpp's quad_vertices (main.cpp:328-345).
static float quad_vertices[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 1.0f, 0.0f, 1.0f,
    1.0f, 1.0f,
    -1.0f, 1.0f, 0.0f, 1.0f,
    0.0f, 1.0f,

    -1.0f, -1.0f, 0.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, -1.0f, 0.0f, 1.0f,
    1.0f, 0.0f,
    1.0f, 1.0f, 0.0f, 1.0f,
    1.0f, 1.0f,
};
static uint32_t quad_vertices_stride = sizeof(float) * 6;
static uint32_t quad_vertices_count = 6;

// Blocking device-instance pump, mirrors graphics.cpp's internal wait_for —
// graphics.h doesn't expose that helper, so the test uses the public
// GraphicsContext/GpuContext handles to drive its own readback.
static void wait_for(bool *done) {
    while (!*done) graphics_context->gpu->instance.ProcessEvents();
}

// Reads back an RGBA32_FLOAT texture into `out` (width*height*4 floats).
// CopyTextureToBuffer requires bytesPerRow padded to 256. Takes a raw
// wgpu::Texture (rather than graphics::RenderTarget*) so it works for both
// RenderTarget-backed textures (call sites pass rt->texture) and plain
// compute-storage-written graphics::Texture2D textures (Task 9, M4b: the
// cs_volpath_blit readback below passes tex->texture) -- mechanical
// generalization, task-9-brief.md step 1.
static void readback_rgba32f(wgpu::Texture texture, uint32_t width, uint32_t height,
                             float *out) {
    const uint32_t unpadded_bytes_per_row = width * 4 * sizeof(float);
    const uint32_t padded_bytes_per_row = (unpadded_bytes_per_row + 255u) & ~255u;
    const uint64_t buffer_size = (uint64_t)padded_bytes_per_row * height;

    wgpu::BufferDescriptor buf_desc = {};
    buf_desc.size = buffer_size;
    buf_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readback = graphics_context->device.CreateBuffer(&buf_desc);

    wgpu::CommandEncoder encoder = graphics_context->device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src = {};
    src.texture = texture;
    wgpu::TexelCopyBufferInfo dst = {};
    dst.buffer = readback;
    dst.layout.bytesPerRow = padded_bytes_per_row;
    dst.layout.rowsPerImage = height;
    wgpu::Extent3D extent = {width, height, 1};
    encoder.CopyTextureToBuffer(&src, &dst, &extent);
    wgpu::CommandBuffer commands = encoder.Finish();
    graphics_context->queue.Submit(1, &commands);

    bool done = false;
    readback.MapAsync(
        wgpu::MapMode::Read, 0, buffer_size, wgpu::CallbackMode::AllowProcessEvents,
        [&done](wgpu::MapAsyncStatus status, wgpu::StringView message) {
            if (status != wgpu::MapAsyncStatus::Success)
                fprintf(stderr, "render_path_tests: readback map failed: %.*s\n",
                        (int)message.length, message.data);
            done = true;
        });
    wait_for(&done);

    const uint8_t *mapped = (const uint8_t *)readback.GetConstMappedRange(0, buffer_size);
    assert(mapped);
    for (uint32_t y = 0; y < height; y++) {
        memcpy(out + y * width * 4, mapped + (uint64_t)y * padded_bytes_per_row,
              unpadded_bytes_per_row);
    }
    readback.Unmap();
}

static void test_offscreen_draw_and_readback() {
    const uint32_t RT_W = 4, RT_H = 4;
    graphics::RenderTarget rt =
        graphics::get_render_target(RT_W, RT_H, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&rt));
    assert(!rt.is_window);

    graphics::set_render_targets_viewport(&rt);
    graphics::clear_render_target(&rt, 0.0f, 0.0f, 0.0f, 1.0f);

    // DEVIATION from the brief's literal "R32_FLOAT Texture2D": graphics.cpp's
    // clear_texture(Texture2D*, float) asserts format == RGBA32_FLOAT (the
    // R32_FLOAT single-value-fill kernel, g_clear3d, only exists for
    // Texture3D — see graphics.cpp's two clear_texture(Texture2D*)/
    // clear_texture(Texture3D*) overloads). An 8x8 R32_FLOAT Texture2D
    // would trip that assert immediately. RGBA32_FLOAT is used instead —
    // it's also what main.cpp's real sampled source (display_tex) uses
    // (inventory §"Textures"), and clear fills all 4 channels with the same
    // value, so `.x` still reads 0.5 either way. Bonus: this is the first
    // test coverage for the Texture2D RGBA32_FLOAT clear kernel
    // (g_clear2d_f) — graphics_tests.cpp only exercises Texture3D R32_FLOAT
    // and Texture2D R32_UINT clears.
    const uint32_t TEX_W = 8, TEX_H = 8;
    graphics::Texture2D tex =
        graphics::get_texture2D(nullptr, TEX_W, TEX_H, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&tex));
    graphics::clear_texture(&tex, 0.5f);   // sampled source

    graphics::TextureSampler samp = graphics::get_texture_sampler();
    assert(graphics::is_ready(&samp));

    graphics::VertexShader vs =
        graphics::get_vertex_shader_from_code((char *)QUAD_VS_WGSL, (uint32_t)strlen(QUAD_VS_WGSL));
    assert(graphics::is_ready(&vs));
    graphics::PixelShader ps =
        graphics::get_pixel_shader_from_code((char *)SAMPLE_PS_WGSL, (uint32_t)strlen(SAMPLE_PS_WGSL));
    assert(graphics::is_ready(&ps));

    graphics::set_vertex_shader(&vs);
    graphics::set_pixel_shader(&ps);

    // Exercises the 2N/2N+1 convention exactly as main.cpp will: texture and
    // sampler both bound at slot 0, landing at WGSL bindings 0 and 1
    // respectively (DESIGN risk #1 — this is the first thing to break if the
    // split isn't wired correctly).
    graphics::set_texture(&tex, 0);
    graphics::set_texture_sampler(&samp, 0);

    graphics::Mesh quad = graphics::get_mesh(quad_vertices, quad_vertices_count,
                                             quad_vertices_stride, NULL, 0, 0);
    assert(graphics::is_ready(&quad));

    // DEVIATION from the brief's literal "set_blend_state(ALPHA)": WebGPU
    // disallows blending on 32-bit float color targets by default (Dawn:
    // "Blending is enabled but color format (TextureFormat::RGBA32Float) is
    // not blendable" — confirmed empirically; would need the unrequested
    // "float32-blendable" device feature, out of this task's file scope,
    // gpu_context.cpp isn't in the Task 1 file list). OPAQUE is used here
    // instead. This does not weaken the assertion: with the background
    // cleared to a=1 and the quad's fragment also writing a=1, ALPHA's
    // src-over-dst blend equation (srcAlpha=1) and OPAQUE's straight
    // overwrite produce an identical result for this test's data, so the
    // readback assertions below are unaffected either way. The ALPHA
    // blend-mapping code itself (build_pipeline's BlendType::ALPHA branch)
    // is unchanged and still compiled into the pipeline cache logic for
    // main.cpp's real (window-target, always-blendable-format) draws; its
    // numerical correctness remains flagged as design risk #6 (unverified
    // alpha-channel formula, deferred to M4 screenshot comparison).
    graphics::set_blend_state(graphics::BlendType::OPAQUE);
    graphics::draw_mesh(&quad);

    // Public flush: swap_frames() flushes the command encoder unconditionally
    // and only touches surface-present logic if a surface texture was
    // acquired (it wasn't — this draw never calls window_view()), so it's a
    // safe headless flush point. graphics.cpp's flush_commands() itself is
    // not part of the public API.
    graphics::swap_frames();

    float pixels[RT_W * RT_H * 4];
    readback_rgba32f(rt.texture, RT_W, RT_H, pixels);

    for (uint32_t i = 0; i < RT_W * RT_H; i++) {
        float r = pixels[i * 4 + 0];
        float g = pixels[i * 4 + 1];
        float b = pixels[i * 4 + 2];
        float a = pixels[i * 4 + 3];
        assert(fabsf(r - 0.5f) < 1e-5f);
        assert(fabsf(g - 0.5f) < 1e-5f);
        assert(fabsf(b - 0.5f) < 1e-5f);
        assert(fabsf(a - 1.0f) < 1e-5f);
    }
    printf("render_path_tests: offscreen draw_mesh + readback passed (every texel == (0.5,0.5,0.5,1.0))\n");

    graphics::unset_texture(0);
    graphics::release(&quad);
    graphics::release(&vs);
    graphics::release(&ps);
    graphics::release(&samp);
    graphics::release(&tex);
    graphics::release(&rt);
}

// ---- Test (Task 2, M4b): load_texture2D palette readback ----
// Loads a real 24 bpp BGR TGA palette (bin/data/palette_hot.tga) via
// stb_image and verifies both the decoded texture shape and specific pixel
// values against hand-derived-from-file-bytes expectations (task-2-brief.md
// step 1). Full-RGBA passthrough PS (unlike SAMPLE_PS_WGSL's single-channel
// broadcast above) so BGR->RGBA swizzle and vertical-flip bugs both show up.
static const char *PASSTHROUGH_PS_WGSL = R"(
@group(1) @binding(0) var tex : texture_2d<f32>;
@group(1) @binding(1) var samp : sampler;

@fragment
fn main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    return textureSample(tex, samp, uv);
}
)";

static void test_load_texture2D_palette() {
    graphics::Texture2D pal = graphics::load_texture2D(DATA_ROOT "/data/palette_hot.tga");
    assert(graphics::is_ready(&pal));
    assert(pal.width == 130);
    assert(pal.height == 16);
    assert(pal.format == graphics::Format::RGBA8_UNORM);

    const uint32_t RT_W = 130, RT_H = 16;
    graphics::RenderTarget rt =
        graphics::get_render_target(RT_W, RT_H, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&rt));

    graphics::set_render_targets_viewport(&rt);
    graphics::clear_render_target(&rt, 0.0f, 0.0f, 0.0f, 1.0f);

    graphics::TextureSampler samp = graphics::get_texture_sampler();   // POINT/CLAMP default
    assert(graphics::is_ready(&samp));

    graphics::VertexShader vs =
        graphics::get_vertex_shader_from_code((char *)QUAD_VS_WGSL, (uint32_t)strlen(QUAD_VS_WGSL));
    assert(graphics::is_ready(&vs));
    graphics::PixelShader ps =
        graphics::get_pixel_shader_from_code((char *)PASSTHROUGH_PS_WGSL, (uint32_t)strlen(PASSTHROUGH_PS_WGSL));
    assert(graphics::is_ready(&ps));

    graphics::set_vertex_shader(&vs);
    graphics::set_pixel_shader(&ps);

    graphics::set_texture(&pal, 0);
    graphics::set_texture_sampler(&samp, 0);

    graphics::Mesh quad = graphics::get_mesh(quad_vertices, quad_vertices_count,
                                             quad_vertices_stride, NULL, 0, 0);
    assert(graphics::is_ready(&quad));

    graphics::set_blend_state(graphics::BlendType::OPAQUE);
    graphics::draw_mesh(&quad);
    graphics::swap_frames();

    std::vector<float> pixels(RT_W * RT_H * 4);
    readback_rgba32f(rt.texture, RT_W, RT_H, pixels.data());

    // RT pixel (x, 7) samples uv ((x+0.5)/130, 1 - 7.5/16) -> texel (x, 8) in
    // the top-down decoded texture -> TGA file scanline 15-8=7 (brief step 1).
    const float tol = 1.5e-3f;
    auto check_pixel = [&](uint32_t x, uint32_t y, float r, float g, float b, float a) {
        uint32_t i = y * RT_W + x;
        float pr = pixels[i * 4 + 0];
        float pg = pixels[i * 4 + 1];
        float pb = pixels[i * 4 + 2];
        float pa = pixels[i * 4 + 3];
        assert(fabsf(pr - r) < tol);
        assert(fabsf(pg - g) < tol);
        assert(fabsf(pb - b) < tol);
        assert(fabsf(pa - a) < tol);
    };
    check_pixel(0, 7, 0.0f, 0.0f, 0.0f, 1.0f);
    check_pixel(64, 7, 250.f/255.f, 123.f/255.f, 0.0f, 1.0f);
    check_pixel(129, 7, 254.f/255.f, 254.f/255.f, 250.f/255.f, 1.0f);
    printf("render_path_tests: load_texture2D palette readback passed (palette_hot.tga scanline 7 pixels match)\n");

    graphics::unset_texture(0);
    graphics::release(&quad);
    graphics::release(&vs);
    graphics::release(&ps);
    graphics::release(&samp);
    graphics::release(&pal);
    graphics::release(&rt);
}

// ---- Test 2b (Task 1 / DESIGN §6.1, I1a): unset_texture must clear BOTH
// the view AND the sampler at that slot ----
//
// Pre-fix, unset_texture(slot) only cleared g_render_slots[slot].view,
// leaving a stale .sampler behind. draw_mesh's bind-group-1 builder emits a
// bind-group entry for ANY slot with a non-null .view OR .sampler
// (graphics.cpp's "Bind group 1" comment) -- so a lingering sampler-only
// entry at an otherwise-unused slot would still land in the bind group at
// binding 2*slot+1, which the slot-0-only SAMPLE_PS_WGSL shader (declaring
// ONLY bindings 0/1) does not declare. That's a hard Dawn bind-group/
// pipeline-layout mismatch -- reproduced here with an explicit
// PushErrorScope/PopErrorScope around the draw, since the device's
// uncaptured-error callback (gpu_context.cpp) only logs by default and would
// not otherwise fail this test.
//
// Constructed to genuinely fail pre-fix per the brief: slot 1 gets BOTH a
// view and a sampler bound, then unset_texture(1) is called, then the draw
// uses the slot-0-only shader -- exactly the shape that leaves a poisoned
// sampler-only entry behind pre-fix.
static void test_unset_texture_clears_sampler() {
    const uint32_t RT_W = 4, RT_H = 4;
    graphics::RenderTarget rt =
        graphics::get_render_target(RT_W, RT_H, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&rt));
    graphics::set_render_targets_viewport(&rt);
    graphics::clear_render_target(&rt, 0.0f, 0.0f, 0.0f, 1.0f);

    const uint32_t TEX_W = 4, TEX_H = 4;
    graphics::Texture2D tex0 =
        graphics::get_texture2D(nullptr, TEX_W, TEX_H, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&tex0));
    graphics::clear_texture(&tex0, 0.25f);
    graphics::Texture2D tex1 =
        graphics::get_texture2D(nullptr, TEX_W, TEX_H, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&tex1));
    graphics::clear_texture(&tex1, 0.75f);

    graphics::TextureSampler samp0 = graphics::get_texture_sampler();
    graphics::TextureSampler samp1 = graphics::get_texture_sampler();
    assert(graphics::is_ready(&samp0));
    assert(graphics::is_ready(&samp1));

    // Fresh shader modules (own pipeline-cache entry from Test 2's) -- same
    // slot-0-only SAMPLE_PS_WGSL source.
    graphics::VertexShader vs =
        graphics::get_vertex_shader_from_code((char *)QUAD_VS_WGSL, (uint32_t)strlen(QUAD_VS_WGSL));
    graphics::PixelShader ps =
        graphics::get_pixel_shader_from_code((char *)SAMPLE_PS_WGSL, (uint32_t)strlen(SAMPLE_PS_WGSL));
    assert(graphics::is_ready(&vs));
    assert(graphics::is_ready(&ps));

    graphics::set_vertex_shader(&vs);
    graphics::set_pixel_shader(&ps);

    // Slot 0: the shader's real, declared texture/sampler pair.
    graphics::set_texture(&tex0, 0);
    graphics::set_texture_sampler(&samp0, 0);
    // Slot 1: an UNUSED slot (SAMPLE_PS_WGSL declares no binding 2/3) --
    // bind BOTH view and sampler, then unset. Pre-fix this leaves the
    // sampler behind.
    graphics::set_texture(&tex1, 1);
    graphics::set_texture_sampler(&samp1, 1);
    graphics::unset_texture(1);

    graphics::Mesh quad = graphics::get_mesh(quad_vertices, quad_vertices_count,
                                             quad_vertices_stride, NULL, 0, 0);
    assert(graphics::is_ready(&quad));
    graphics::set_blend_state(graphics::BlendType::OPAQUE);

    bool had_error = false, popped = false;
    graphics_context->device.PushErrorScope(wgpu::ErrorFilter::Validation);
    graphics::draw_mesh(&quad);
    graphics_context->device.PopErrorScope(
        wgpu::CallbackMode::AllowProcessEvents,
        [&had_error, &popped](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                              wgpu::StringView message) {
            if (type != wgpu::ErrorType::NoError) {
                had_error = true;
                fprintf(stderr, "render_path_tests: unset_texture-clears-sampler test: "
                                "%.*s\n", (int)message.length, message.data);
            }
            popped = true;
        });
    wait_for(&popped);
    graphics::swap_frames();

    assert(!had_error && "unset_texture(1) must clear the sampler too -- a stale "
                         "sampler-only entry at an unused slot must not poison "
                         "the bind group (DESIGN §6.1 / I1a)");
    printf("render_path_tests: unset_texture clears sampler (I1a) passed\n");

    graphics::unset_texture(0);
    graphics::release(&quad);
    graphics::release(&vs);
    graphics::release(&ps);
    graphics::release(&samp0);
    graphics::release(&samp1);
    graphics::release(&tex0);
    graphics::release(&tex1);
    graphics::release(&rt);
}

// ---- Test 3: @group(0)-declaring shader pair with no uniform bound ----
//
// Per the design (§5) and draw_mesh's step 3, a shader pair where either
// stage declares `@group(0)` but no constant buffer has been
// set_constant_buffer'd must fatal() (mirrors run_compute's identical gate
// and message shape). This is death-behavior (calls exit(1)) and there is no
// death-test harness in this test suite (shader_compile_tests.cpp and
// graphics_tests.cpp establish the same precedent: fatal() paths are
// documented, not executed in-process, since an in-process exit(1) would
// abort the whole test binary before later assertions/printf's run). Not
// implemented as a runnable test; Test 2 above already covers the positive
// (uniform correctly omitted) path since QUAD_VS_WGSL/SAMPLE_PS_WGSL declare
// no @group(0) at all.

// ---- Test 4 (M3 Task 3): particle-chain pixels-exist test ----
//
// Mirrors main.cpp's VM_PARTICLES compute chain (cs_particles_transform ->
// cs_particles_blit) headlessly on a 64x64 "screen", exactly the bind
// discipline/order of main.cpp's VM_PARTICLES block, then reads back
// display_tex and asserts:
//   (a) at least one texel has nonzero red (the chain actually produced
//       pixels), pinned further to the known center-particle pixel value;
//   (b) the OOB write guard in cs_particles_transform.wgsl held: a particle
//       deliberately placed at exactly x == screen_width must NOT corrupt
//       the accumulation buffer's last element. Without the shader's
//       explicit bounds guard, WGSL's storage-buffer OOB-index safety
//       clamps (rather than discards) an out-of-range dynamic array index,
//       which would land this particle's atomicAdd on the LAST buffer
//       element instead of being dropped -- see cs_particles_transform.wgsl's
//       "NEW REQUIRED GUARD" comment and translation-notes.md §0.1.
//
// Camera choice: identity view AND identity projection ("simple centered
// ortho" per the M3 Task 3 brief) -- not merely orthographic, the full no-op
// transform. Chosen because it makes the grid-space -> screen-space mapping
// invertible by hand, so particles can be placed at exact, predictable
// screen pixels (worked out below) without needing cpplib/maths.h linked
// into this test binary.

// RenderingConfig, duplicated byte-for-byte (see main.cpp:263-312 and its
// static_assert at :313, and cs_particles_transform.wgsl / cs_particles_blit
// .wgsl's `struct RenderingConfig`) using plain float[16] in place of
// Matrix4x4 (both are 64 B column-major arrays; identity is representation-
// agnostic) so this test doesn't need to link cpplib/maths.cpp.
struct RenderingConfigTest {
    float projection[16];
    float view[16];
    float model[16];

    int32_t texcoord_map;
    float trim_x_min;
    float trim_x_max;
    float trim_y_min;

    float trim_y_max;
    float trim_z_min;
    float trim_z_max;
    float trim_density;

    float world_width;
    float world_height;
    float world_depth;
    float screen_width;

    float screen_height;
    float sample_weight;
    float optical_thickness;
    float highlight_density;

    float galaxy_weight;
    float histogram_base;
    float overdensity_threshold_low;
    float overdensity_threshold_high;

    float camera_x;
    float camera_y;
    float camera_z;
    int32_t pt_iteration;

    float sigma_s;
    float sigma_a;
    float sigma_e;
    float trace_max;

    float camera_offset_x;
    float camera_offset_y;
    float exposure;
    int32_t n_bounces;

    float ambient_trace;
    int32_t compressive_accumulation;
    float guiding_strength;
    float scattering_anisotropy;
};
static_assert(sizeof(RenderingConfigTest) == 3 * 64 + 36 * 4, "RenderingConfigTest layout must match main.cpp's RenderingConfig / the WGSL cfg struct");

static void set_identity16(float m[16]) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Reads back an RGBA32_FLOAT Texture2D (not a RenderTarget -- display_tex is
// a compute-storage-written texture, never a render target) into `out`
// (width*height*4 floats). Same CopyTextureToBuffer/256-byte-row-padding
// shape as readback_rgba32f above, operating on tex->texture directly.
static void readback_texture2d_rgba32f(graphics::Texture2D *tex, uint32_t width, uint32_t height,
                                       float *out) {
    const uint32_t unpadded_bytes_per_row = width * 4 * sizeof(float);
    const uint32_t padded_bytes_per_row = (unpadded_bytes_per_row + 255u) & ~255u;
    const uint64_t buffer_size = (uint64_t)padded_bytes_per_row * height;

    wgpu::BufferDescriptor buf_desc = {};
    buf_desc.size = buffer_size;
    buf_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readback = graphics_context->device.CreateBuffer(&buf_desc);

    wgpu::CommandEncoder encoder = graphics_context->device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src = {};
    src.texture = tex->texture;
    wgpu::TexelCopyBufferInfo dst = {};
    dst.buffer = readback;
    dst.layout.bytesPerRow = padded_bytes_per_row;
    dst.layout.rowsPerImage = height;
    wgpu::Extent3D extent = {width, height, 1};
    encoder.CopyTextureToBuffer(&src, &dst, &extent);
    wgpu::CommandBuffer commands = encoder.Finish();
    graphics_context->queue.Submit(1, &commands);

    bool done = false;
    readback.MapAsync(
        wgpu::MapMode::Read, 0, buffer_size, wgpu::CallbackMode::AllowProcessEvents,
        [&done](wgpu::MapAsyncStatus status, wgpu::StringView message) {
            if (status != wgpu::MapAsyncStatus::Success)
                fprintf(stderr, "particle_chain test: readback map failed: %.*s\n",
                        (int)message.length, message.data);
            done = true;
        });
    wait_for(&done);

    const uint8_t *mapped = (const uint8_t *)readback.GetConstMappedRange(0, buffer_size);
    assert(mapped);
    for (uint32_t y = 0; y < height; y++) {
        memcpy(out + y * width * 4, mapped + (uint64_t)y * padded_bytes_per_row,
              unpadded_bytes_per_row);
    }
    readback.Unmap();
}

static void test_particle_chain_pixels_exist() {
    const uint32_t SCREEN_W = 64, SCREEN_H = 64;
    const uint32_t GRID_Z = 1;
    // WG(10,10,10) == 1000 threads/group (cs_particles_transform.wgsl's
    // override WG_X/Y/Z); dispatch(10,10,GRID_Z) covers exactly
    // 10*10*GRID_Z*1000 particle indices with no gaps and no OOB buffer
    // reads -- the same dispatch/index-bijection shape as main.cpp's
    // VM_PARTICLES block (grid_z = NUM_PARTICLES/100/THREAD_GROUP_SIZE).
    const uint32_t N_PARTICLES = 10 * 10 * GRID_Z * 1000;   // 100,000

    // Grid-space particle positions, world_width=height=depth=64 (chosen
    // below). With identity view+projection the shader's math (traced
    // through cs_particles_transform.wgsl by hand):
    //   in_posf = pos/world_size                          (grid -> [0,1])
    //   p = (2*in_posf - 1) * (1, h/w, d/w); p.yz *= -1    ([0,1] -> [-1,1], h/w=d/w=1 here)
    //   world_pos = view*vec4(p,1) = vec4(p,1)             (view = identity)
    //   out_posf  = projection*world_pos = vec4(p,1)       (projection = identity)
    //   out_posf /= out_posf.w                             (w == 1, no-op)
    //   out_posf  = out_posf*0.5 + 0.5                     ([-1,1] -> [0,1])
    //   screen_pos = out_posf.xy * (screen_width, screen_height)
    // So grid pos (32,32,32) (dead center of a 64-wide world) -> p=(0,0,0)
    // -> out_posf.xy=(0.5,0.5) -> screen_pos=(32,32): a clean, known pixel.
    // Grid pos (64,32,32) -> in_posf.x=1.0 -> p.x=1.0 -> out_posf.x=1.0
    // (passes the shader's `> 1.0` rejection, since 1.0 is not > 1.0) ->
    // screen_pos.x = 64 == SCREEN_W: exactly the one-past-the-end case the
    // shader's explicit OOB guard exists for (see header comment above).
    //
    // All other (filler) particles sit at z=1000 grid units, i.e.
    // in_posf.z = 1000/64 = 15.625, far outside the default trim box
    // [0,1] on z -- rejected at the shader's early trim-box return, so they
    // can never touch either pixel/element this test inspects.
    std::vector<float> px(N_PARTICLES, 32.0f), py(N_PARTICLES, 32.0f),
                        pz(N_PARTICLES, 1000.0f), pt(N_PARTICLES, 1.0f);
    px[0] = 32.0f; py[0] = 32.0f; pz[0] = 32.0f; pt[0] = 1.0f;   // -> pixel (32,32), agent-weight (10) splat
    px[1] = 64.0f; py[1] = 32.0f; pz[1] = 32.0f; pt[1] = 1.0f;   // -> screen_pos.x == SCREEN_W, OOB guard must discard

    RenderingConfigTest cfg = {};
    set_identity16(cfg.projection);
    set_identity16(cfg.view);
    set_identity16(cfg.model);
    cfg.trim_x_min = 0.0f; cfg.trim_x_max = 1.0f;
    cfg.trim_y_min = 0.0f; cfg.trim_y_max = 1.0f;
    cfg.trim_z_min = 0.0f; cfg.trim_z_max = 1.0f;
    cfg.world_width = 64.0f; cfg.world_height = 64.0f; cfg.world_depth = 64.0f;
    cfg.screen_width = (float)SCREEN_W; cfg.screen_height = (float)SCREEN_H;
    cfg.sample_weight = 1.0f;    // blit: val_out = val * sample_weight for val < 10000
    cfg.galaxy_weight = 0.25f;   // unused by t>=0 (agent) particles, set for hygiene

    graphics::ConstantBuffer cfg_buf = graphics::get_constant_buffer(sizeof(RenderingConfigTest));
    assert(graphics::is_ready(&cfg_buf));
    graphics::update_constant_buffer(&cfg_buf, &cfg);

    graphics::StructuredBuffer buf_x = graphics::get_structured_buffer(sizeof(float), N_PARTICLES);
    graphics::StructuredBuffer buf_y = graphics::get_structured_buffer(sizeof(float), N_PARTICLES);
    graphics::StructuredBuffer buf_z = graphics::get_structured_buffer(sizeof(float), N_PARTICLES);
    graphics::StructuredBuffer buf_t = graphics::get_structured_buffer(sizeof(float), N_PARTICLES);
    graphics::update_structured_buffer(&buf_x, px.data());
    graphics::update_structured_buffer(&buf_y, py.data());
    graphics::update_structured_buffer(&buf_z, pz.data());
    graphics::update_structured_buffer(&buf_t, pt.data());

    graphics::StructuredBuffer accum_buf = graphics::get_structured_buffer(sizeof(uint32_t), SCREEN_W * SCREEN_H);
    assert(graphics::is_ready(&accum_buf));
    graphics::clear_structured_buffer(&accum_buf);

    graphics::Texture2D display_tex = graphics::get_texture2D(NULL, SCREEN_W, SCREEN_H, graphics::Format::RGBA32_FLOAT, 16);
    assert(graphics::is_ready(&display_tex));

    File transform_f = file_system::read_file(SHADER_DIR "/cs_particles_transform.wgsl");
    assert(transform_f.data != NULL);
    graphics::ComputeShader transform_cs =
        graphics::get_compute_shader_from_code((char *)transform_f.data, transform_f.size);
    file_system::release_file(transform_f);
    assert(graphics::is_ready(&transform_cs));

    File blit_f = file_system::read_file(SHADER_DIR "/cs_particles_blit.wgsl");
    assert(blit_f.data != NULL);
    graphics::ComputeShader blit_cs =
        graphics::get_compute_shader_from_code((char *)blit_f.data, blit_f.size);
    file_system::release_file(blit_f);
    assert(graphics::is_ready(&blit_cs));

    // Splat particles into the accumulation buffer -- same slot layout/order
    // as main.cpp's VM_PARTICLES block (Task 3 brief step 3).
    graphics::set_compute_shader(&transform_cs);
    graphics::set_constant_buffer(&cfg_buf, 0);
    graphics::set_structured_buffer(&accum_buf, 0);
    graphics::set_structured_buffer(&buf_x, 2);
    graphics::set_structured_buffer(&buf_y, 3);
    graphics::set_structured_buffer(&buf_z, 4);
    graphics::set_structured_buffer(&buf_t, 6);
    graphics::run_compute(10, 10, GRID_Z);
    graphics::unset_structured_buffer(0);
    graphics::unset_structured_buffer(2);
    graphics::unset_structured_buffer(3);
    graphics::unset_structured_buffer(4);
    graphics::unset_structured_buffer(6);

    // Blit accumulated counts into display_tex.
    graphics::set_compute_shader(&blit_cs);
    graphics::set_constant_buffer(&cfg_buf, 0);
    graphics::set_structured_buffer(&accum_buf, 0);
    graphics::set_texture_compute(&display_tex, 1);
    graphics::run_compute(SCREEN_W, SCREEN_H, 1);
    graphics::unset_structured_buffer(0);
    graphics::unset_texture_compute(1);

    // (b) OOB guard: the accumulation buffer's LAST element (index
    // SCREEN_W*SCREEN_H-1, the bottom-right screen texel) must be
    // untouched. If cs_particles_transform.wgsl's explicit bounds guard
    // were missing, WGSL's storage-buffer OOB-index safety would CLAMP the
    // deliberately-OOB particle[1]'s atomicAdd to land here instead of
    // discarding it.
    std::vector<uint32_t> accum(SCREEN_W * SCREEN_H);
    graphics::capture_structured_buffer(&accum_buf, accum.data(), SCREEN_W * SCREEN_H, sizeof(uint32_t));
    uint32_t last_elem = accum[SCREEN_W * SCREEN_H - 1];
    assert(last_elem == 0);

    // (a) at least one texel has nonzero red, pinned to the known center
    // pixel's exact expected value: one weight-10 splat * sample_weight(1.0).
    std::vector<float> pixels(SCREEN_W * SCREEN_H * 4);
    readback_texture2d_rgba32f(&display_tex, SCREEN_W, SCREEN_H, pixels.data());
    bool any_nonzero = false;
    for (uint32_t i = 0; i < SCREEN_W * SCREEN_H; i++) {
        if (pixels[i * 4 + 0] > 0.0f) { any_nonzero = true; break; }
    }
    assert(any_nonzero);

    uint32_t center_idx = 32u * SCREEN_W + 32u;
    float center_val = pixels[center_idx * 4 + 0];
    assert(fabsf(center_val - 10.0f) < 1e-3f);

    printf("render_path_tests: particle chain pixels-exist test passed "
           "(center pixel(32,32).x=%.3f, OOB-guard last accum elem=%u, any_nonzero_texel=%s)\n",
           center_val, last_elem, any_nonzero ? "true" : "false");

    graphics::release(&transform_cs);
    graphics::release(&blit_cs);
    graphics::release(&display_tex);
    graphics::release(&accum_buf);
    graphics::release(&buf_x);
    graphics::release(&buf_y);
    graphics::release(&buf_z);
    graphics::release(&buf_t);
    graphics::release(&cfg_buf);
}

// ---- Test (M4b Task 3, DESIGN §3): 28-byte-stride vertex path, first real
// draw coverage for cpplib/graphics.cpp's `case 28:` fill_vertex_attributes
// branch (Float32x4 position @ offset 0 loc 0, Float32x3 texcoord @ offset
// 16 loc 1) -- present since M3 but never exercised by an actual draw_mesh
// call until vs_3d.wgsl existed. Also the first positive-path coverage of
// draw_mesh's group-0 branch (graphics.cpp:807-818).
//
// Single 6-vertex quad shaped like one copy of main.cpp's
// super_quad_vertices_template (main.cpp:660-675), covering NDC [-1,1] x
// [-1,1] at z=0, w=1. Paired with vs_3d.wgsl (identity projection/view/model
// -> position passes through unchanged) and a trivial fragment shader that
// visualizes the interstage texcoord directly.
static float super_quad_test_vertices[] = {
    -1.f,-1.f,0.f,1.f,  0.f,0.f,0.25f,
     1.f, 1.f,0.f,1.f,  1.f,1.f,0.25f,
    -1.f, 1.f,0.f,1.f,  0.f,1.f,0.25f,
    -1.f,-1.f,0.f,1.f,  0.f,0.f,0.25f,
     1.f,-1.f,0.f,1.f,  1.f,0.f,0.25f,
     1.f, 1.f,0.f,1.f,  1.f,1.f,0.25f,
};   // stride = 7 * sizeof(float) = 28, matches super_quad_vertices_stride
static uint32_t super_quad_test_vertices_stride = sizeof(float) * 7;
static uint32_t super_quad_test_vertices_count = 6;

static const char *TEXCOORD_VIS_PS_WGSL = R"(
@fragment fn main(@location(0) tc : vec3<f32>) -> @location(0) vec4<f32> {
    return vec4<f32>(tc, 1.0);
}
)";

static void test_super_quad_stride_draw() {
    const uint32_t RT_W = 4, RT_H = 4;
    graphics::RenderTarget rt =
        graphics::get_render_target(RT_W, RT_H, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&rt));

    graphics::set_render_targets_viewport(&rt);
    graphics::clear_render_target(&rt, 0.0f, 0.0f, 0.0f, 0.0f);

    File vs_f = file_system::read_file(SHADER_DIR "/vs_3d.wgsl");
    assert(vs_f.data != NULL);
    graphics::VertexShader vs =
        graphics::get_vertex_shader_from_code((char *)vs_f.data, vs_f.size);
    file_system::release_file(vs_f);
    assert(graphics::is_ready(&vs));
    assert(vs.uses_group0);

    graphics::PixelShader ps =
        graphics::get_pixel_shader_from_code((char *)TEXCOORD_VIS_PS_WGSL, (uint32_t)strlen(TEXCOORD_VIS_PS_WGSL));
    assert(graphics::is_ready(&ps));

    graphics::set_vertex_shader(&vs);
    graphics::set_pixel_shader(&ps);

    // Identity matrices -> vs_3d passes positions through unchanged;
    // texcoord_map == 1 -> vs_3d passes texcoords through unchanged
    // (HLSL:27-28 / vs_3d.wgsl's first branch).
    RenderingConfigTest cfg = {};
    set_identity16(cfg.projection);
    set_identity16(cfg.view);
    set_identity16(cfg.model);
    cfg.texcoord_map = 1;

    graphics::ConstantBuffer cfg_buf = graphics::get_constant_buffer(sizeof(RenderingConfigTest));
    assert(graphics::is_ready(&cfg_buf));
    graphics::update_constant_buffer(&cfg_buf, &cfg);
    graphics::set_constant_buffer(&cfg_buf, 0);

    graphics::Mesh quad = graphics::get_mesh(super_quad_test_vertices, super_quad_test_vertices_count,
                                             super_quad_test_vertices_stride, NULL, 0, 0);
    assert(graphics::is_ready(&quad));

    graphics::set_blend_state(graphics::BlendType::OPAQUE);
    graphics::draw_mesh(&quad);
    graphics::swap_frames();

    float pixels[RT_W * RT_H * 4];
    readback_rgba32f(rt.texture, RT_W, RT_H, pixels);

    // RT row 0 is NDC top (y_ndc = 1 - 2*(y+0.5)/H); v = (y_ndc+1)/2 =
    // 1 - (y+0.5)/H. u = (x_ndc+1)/2 = (x+0.5)/W directly. b is the constant
    // 0.25 texcoord.z passed through unchanged; a == 1.0 from the PS. Any
    // attribute offset/stride mistake shifts position lanes (+-1.0 / w=1.0)
    // into the texcoord and breaks these values.
    const float tol = 1e-5f;
    for (uint32_t y = 0; y < RT_H; y++) {
        for (uint32_t x = 0; x < RT_W; x++) {
            uint32_t i = y * RT_W + x;
            float r = pixels[i * 4 + 0];
            float g = pixels[i * 4 + 1];
            float b = pixels[i * 4 + 2];
            float a = pixels[i * 4 + 3];
            float expect_r = (x + 0.5f) / 4.0f;
            float expect_g = 1.0f - (y + 0.5f) / 4.0f;
            assert(fabsf(r - expect_r) < tol);
            assert(fabsf(g - expect_g) < tol);
            assert(fabsf(b - 0.25f) < tol);
            assert(fabsf(a - 1.0f) < tol);
        }
    }
    printf("render_path_tests: super_quad 28-byte-stride draw passed (4x4 texcoord-visualization RT matches hand-derived values)\n");

    graphics::release(&quad);
    graphics::release(&vs);
    graphics::release(&ps);
    graphics::release(&cfg_buf);
    graphics::release(&rt);
}

// ---- Test 5 (Task 1 / DESIGN §6.3): compute texture+sampler pairing at the
// same slot, end-to-end through the 16+N sampler binding scheme ----
//
// Minimal kernel matching the brief exactly: texture_2d<f32> at
// @group(1) @binding(1) (slot 1, binding == slot, unchanged), sampler at
// @group(1) @binding(17) (MAX_SLOTS + slot = 16 + 1), storage buffer at
// @group(1) @binding(0) (slot 0). Bound via
// set_texture_sampled_compute(&tex, 1) + set_texture_sampler_compute(&samp, 1)
// + set_structured_buffer(&out, 0) -- exactly the call order main.cpp's
// VM_PATH_TRACING block uses for cs_volpath's slots 1/3/4 (M4b).
//
// Pre-fix, set_texture_sampled_compute and set_texture_sampler_compute both
// reset the WHOLE slot (`g_compute_slots[slot] = {}`) before writing their
// own field: calling them back-to-back at slot 1 left only the LAST call's
// write behind, so the sampler call would erase the texture that was just
// bound -- the shader's declared @binding(1) texture would go unbound (and
// pre-fix there was no 16+N split at all, so even the sampler wouldn't land
// at @binding(17)). Either way the sampled value would not be the texture's
// known fill value; this test pins both the pairing fix and the 16+N scheme
// by asserting the exact sampled value round-trips.
static const char *COMPUTE_SAMPLER_PAIR_WGSL = R"(
@group(1) @binding(0) var<storage, read_write> out_buf : array<f32>;
@group(1) @binding(1) var tex : texture_2d<f32>;
@group(1) @binding(17) var samp : sampler;

@compute @workgroup_size(1)
fn main() {
    out_buf[0] = textureSampleLevel(tex, samp, vec2<f32>(0.5, 0.5), 0.0).x;
}
)";

static void test_compute_sampler_pairing() {
    graphics::Texture2D tex =
        graphics::get_texture2D(nullptr, 1, 1, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&tex));
    graphics::clear_texture(&tex, 0.75f);

    graphics::TextureSampler samp = graphics::get_texture_sampler();
    assert(graphics::is_ready(&samp));

    graphics::StructuredBuffer out_buf = graphics::get_structured_buffer(sizeof(float), 1);
    assert(graphics::is_ready(&out_buf));
    graphics::clear_structured_buffer(&out_buf);

    graphics::ComputeShader cs = graphics::get_compute_shader_from_code(
        (char *)COMPUTE_SAMPLER_PAIR_WGSL, (uint32_t)strlen(COMPUTE_SAMPLER_PAIR_WGSL));
    assert(graphics::is_ready(&cs));

    graphics::set_compute_shader(&cs);
    graphics::set_structured_buffer(&out_buf, 0);
    graphics::set_texture_sampled_compute(&tex, 1);
    graphics::set_texture_sampler_compute(&samp, 1);
    graphics::run_compute(1, 1, 1);
    graphics::unset_structured_buffer(0);
    graphics::unset_texture_sampled_compute(1);

    float result[1] = {0.0f};
    graphics::capture_structured_buffer(&out_buf, result, 1, sizeof(float));
    assert(fabsf(result[0] - 0.75f) < 1e-4f &&
          "textureSampleLevel must read the texture bound at slot 1 (binding 1) "
          "through the sampler ALSO bound at slot 1 (binding 17) -- pairing + "
          "16+N scheme, DESIGN §6.3");
    printf("render_path_tests: compute texture+sampler pairing (16+N binding) "
           "passed (sampled=%.4f)\n", result[0]);

    graphics::release(&cs);
    graphics::release(&out_buf);
    graphics::release(&samp);
    graphics::release(&tex);
}

// ---- Test 6 (M4a Task 2b): resize cycle on offscreen resources ----
//
// The window-surface Configure path itself isn't headless-testable (no real
// window/surface in this binary — verified manually at the Task 3 human
// gate instead). What IS pinned here, headlessly, is everything main.cpp
// does AROUND that Configure call on a resize: release the old display_tex/
// display_accum_buffer/render-target-sized resources, create fresh ones at
// a DIFFERENT size, and keep drawing/dispatching through the SAME cached
// render pipeline (draw_mesh's g_pipeline_cache, keyed on vs/ps/blend/
// stride/format/topology -- deliberately NOT on texture dimensions) and the
// SAME cached clear-kernel compute pipeline (graphics.cpp's
// ensure_clear_kernel/g_clear2d_f, exercised via clear_texture). Chosen size
// B is both LARGER and a different aspect ratio than size A, so a silently
// stale (wrong-size) resource would surface as a Dawn validation error
// (e.g. CopyTextureToBuffer's extent exceeding the actual texture extent)
// inside the PushErrorScope/PopErrorScope bracket below, not just a wrong
// pixel value.
static void run_resize_cycle_pass(graphics::VertexShader *vs, graphics::PixelShader *ps,
                                  graphics::Mesh *quad, uint32_t w, uint32_t h,
                                  float clear_value) {
    graphics::StructuredBuffer accum = graphics::get_structured_buffer(sizeof(uint32_t), w * h);
    assert(graphics::is_ready(&accum));
    graphics::Texture2D display_tex =
        graphics::get_texture2D(nullptr, w, h, graphics::Format::RGBA32_FLOAT, 16);
    assert(graphics::is_ready(&display_tex));
    graphics::RenderTarget rt = graphics::get_render_target(w, h, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&rt));
    graphics::TextureSampler samp = graphics::get_texture_sampler();
    assert(graphics::is_ready(&samp));

    bool had_error = false, popped = false;
    graphics_context->device.PushErrorScope(wgpu::ErrorFilter::Validation);

    graphics::clear_structured_buffer(&accum);            // mirrors main.cpp's per-frame accum clear
    graphics::clear_texture(&display_tex, clear_value);   // reuses the cached g_clear2d_f pipeline

    graphics::set_render_targets_viewport(&rt);
    graphics::clear_render_target(&rt, 0.0f, 0.0f, 0.0f, 1.0f);
    graphics::set_vertex_shader(vs);
    graphics::set_pixel_shader(ps);
    graphics::set_texture(&display_tex, 0);
    graphics::set_texture_sampler(&samp, 0);
    graphics::set_blend_state(graphics::BlendType::OPAQUE);
    graphics::draw_mesh(quad);                            // reuses the cached render pipeline
    graphics::unset_texture(0);
    graphics::swap_frames();

    std::vector<float> pixels((size_t)w * h * 4);
    readback_rgba32f(rt.texture, w, h, pixels.data());   // OOB CopyTextureToBuffer if rt were stale-sized

    graphics_context->device.PopErrorScope(
        wgpu::CallbackMode::AllowProcessEvents,
        [&had_error, &popped, w, h](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                                    wgpu::StringView message) {
            if (type != wgpu::ErrorType::NoError) {
                had_error = true;
                fprintf(stderr, "render_path_tests: resize cycle (%ux%u pass): %.*s\n",
                        w, h, (int)message.length, message.data);
            }
            popped = true;
        });
    wait_for(&popped);
    assert(!had_error && "resize cycle: recreate-at-new-size produced a Dawn validation error");

    // SAMPLE_PS_WGSL (defined above, Test 2) writes vec4(v,v,v,1.0) where
    // v = tex.x -- r/g/b track the cleared display_tex value, alpha is
    // always hardcoded to 1.0 (same shape asserted by test_offscreen_draw_
    // and_readback above).
    for (uint32_t i = 0; i < w * h; i++) {
        assert(fabsf(pixels[i * 4 + 0] - clear_value) < 1e-5f);
        assert(fabsf(pixels[i * 4 + 1] - clear_value) < 1e-5f);
        assert(fabsf(pixels[i * 4 + 2] - clear_value) < 1e-5f);
        assert(fabsf(pixels[i * 4 + 3] - 1.0f) < 1e-5f);
    }

    graphics::release(&samp);
    graphics::release(&rt);
    graphics::release(&display_tex);
    graphics::release(&accum);
}

static void test_resize_cycle_offscreen() {
    graphics::VertexShader vs =
        graphics::get_vertex_shader_from_code((char *)QUAD_VS_WGSL, (uint32_t)strlen(QUAD_VS_WGSL));
    assert(graphics::is_ready(&vs));
    graphics::PixelShader ps =
        graphics::get_pixel_shader_from_code((char *)SAMPLE_PS_WGSL, (uint32_t)strlen(SAMPLE_PS_WGSL));
    assert(graphics::is_ready(&ps));
    graphics::Mesh quad = graphics::get_mesh(quad_vertices, quad_vertices_count,
                                             quad_vertices_stride, NULL, 0, 0);
    assert(graphics::is_ready(&quad));

    // Size A -> render/dispatch -> release -> size B (larger, different
    // aspect ratio) -> render/dispatch again, reusing the SAME vs/ps/quad
    // mesh (and therefore the SAME cached pipelines) across the resize --
    // exactly mirroring main.cpp's Task 2b resize path, where quad_mesh/
    // vertex_shader_2d/pixel_shader_2d are never recreated, only the
    // screen-sized resources are.
    run_resize_cycle_pass(&vs, &ps, &quad, 16, 12, 0.25f);
    run_resize_cycle_pass(&vs, &ps, &quad, 48, 20, 0.75f);

    printf("render_path_tests: resize cycle (recreate display_tex/accum/RT at a new "
           "size, reusing cached pipelines) passed\n");

    graphics::release(&quad);
    graphics::release(&vs);
    graphics::release(&ps);
}

// ---- Test (M4b Task 4, DESIGN §2.2): ps_volume_trace.wgsl readback ----
//
// Same skeleton as test_super_quad_stride_draw (vs_3d.wgsl from disk, 28 B
// super-quad, 336 B cfg buffer) but with ps_volume_trace.wgsl from disk and
// real sampled resources (a filled tex_trace + a solid-red tex_false_color),
// asserting hand-derived expected values (task-4-brief.md step 1):
//
//   tex_trace: 4x4x4 R32_FLOAT, every texel = 1.0.
//   tex_false_color: 2x2 RGBA8_UNORM, every texel = solid red (255,0,0,255)
//     -- makes the palette SAMPLE POSITION irrelevant, only its color matters.
//   trims = [0.2, 0.8] on all three axes (tight window: a mis-strided
//     texcoord read landing on a position/w lane, +-1.0/1.0, would fall
//     OUTSIDE this window and fail the corner-pixel assertion below).
//   trim_density = 0.5, sample_weight = 2.0, optical_thickness = 0.25.
//
// Hand-derived math (identical to the brief): trace = 1.0 (uniform texture,
// any sample position). t = (1.0 - 0.5) * 2.0 = 1.0. remap(1.0, 1.0) =
// 1 - e^-1 = 0.6321206. fragment.rgb = red palette's .rgb = (1,0,0); then
// the QUIRK(single_stack_2x_compensation) *2.0 at the end of the shader ->
// (2.0, 0.0, 0.0). fragment.a = optical_thickness * remap(t,1.0) =
// 0.25 * 0.6321206 = 0.1580301.
//
// Reuses super_quad_test_vertices (Task 3, above): with texcoord_map == 1,
// vs_3d.wgsl passes texcoords through unchanged, so the same
// u=(x+0.5)/W, v=1-(y+0.5)/H, w=0.25-constant mapping from that test
// applies here too.
//   Center pixel (4,4) of an 8x8 RT: texcoord = (0.5625, 0.4375, 0.25) --
//     inside [0.2,0.8]^3 -> NOT trimmed -> assert (2.0, 0.0, 0.0, 0.1580301).
//   Corner pixel (0,0): texcoord.x = 0.0625 < 0.2 -> trimmed -> fragment =
//     vec4(0) even after the *2.0 (0*2==0) -> assert (0,0,0,0), pins the
//     trim early-out.
static void test_volume_trace_readback() {
    const uint32_t RT_W = 8, RT_H = 8;
    graphics::RenderTarget rt =
        graphics::get_render_target(RT_W, RT_H, graphics::Format::RGBA32_FLOAT);
    assert(graphics::is_ready(&rt));

    graphics::set_render_targets_viewport(&rt);
    graphics::clear_render_target(&rt, 0.0f, 0.0f, 0.0f, 0.0f);

    File vs_f = file_system::read_file(SHADER_DIR "/vs_3d.wgsl");
    assert(vs_f.data != NULL);
    graphics::VertexShader vs =
        graphics::get_vertex_shader_from_code((char *)vs_f.data, vs_f.size);
    file_system::release_file(vs_f);
    assert(graphics::is_ready(&vs));
    assert(vs.uses_group0);

    File ps_f = file_system::read_file(SHADER_DIR "/ps_volume_trace.wgsl");
    assert(ps_f.data != NULL);
    graphics::PixelShader ps =
        graphics::get_pixel_shader_from_code((char *)ps_f.data, ps_f.size);
    file_system::release_file(ps_f);
    assert(graphics::is_ready(&ps));
    assert(ps.uses_group0);

    graphics::set_vertex_shader(&vs);
    graphics::set_pixel_shader(&ps);

    // tex_trace: 4x4x4 R32_FLOAT, every texel == 1.0.
    std::vector<float> trace_data(4 * 4 * 4, 1.0f);
    graphics::Texture3D tex_trace =
        graphics::get_texture3D(trace_data.data(), 4, 4, 4, graphics::Format::R32_FLOAT, 4);
    assert(graphics::is_ready(&tex_trace));

    // tex_false_color: 2x2 RGBA8_UNORM, every texel == solid red.
    uint8_t false_color_data[2 * 2 * 4];
    for (int i = 0; i < 4; i++) {
        false_color_data[i * 4 + 0] = 255;
        false_color_data[i * 4 + 1] = 0;
        false_color_data[i * 4 + 2] = 0;
        false_color_data[i * 4 + 3] = 255;
    }
    graphics::Texture2D tex_false_color =
        graphics::get_texture2D(false_color_data, 2, 2, graphics::Format::RGBA8_UNORM, 4);
    assert(graphics::is_ready(&tex_false_color));

    // Trace slot 0: CLAMP/ANISOTROPIC (matches main.cpp:547 -- linear
    // filtering an r32float texture; Float32Filterable is a required device
    // feature, gpu_context.cpp:42/69, so this is safe).
    graphics::TextureSampler samp_trace =
        graphics::get_texture_sampler(graphics::CLAMP, graphics::Filter::ANISOTROPIC);
    assert(graphics::is_ready(&samp_trace));
    // Palette slot 1: default sampler (CLAMP/POINT).
    graphics::TextureSampler samp_palette = graphics::get_texture_sampler();
    assert(graphics::is_ready(&samp_palette));

    graphics::set_texture(&tex_trace, 0);
    graphics::set_texture_sampler(&samp_trace, 0);
    graphics::set_texture(&tex_false_color, 1);
    graphics::set_texture_sampler(&samp_palette, 1);

    RenderingConfigTest cfg = {};
    set_identity16(cfg.projection);
    set_identity16(cfg.view);
    set_identity16(cfg.model);
    cfg.texcoord_map = 1;
    cfg.trim_x_min = 0.2f; cfg.trim_x_max = 0.8f;
    cfg.trim_y_min = 0.2f; cfg.trim_y_max = 0.8f;
    cfg.trim_z_min = 0.2f; cfg.trim_z_max = 0.8f;
    cfg.trim_density = 0.5f;
    cfg.sample_weight = 2.0f;
    cfg.optical_thickness = 0.25f;

    graphics::ConstantBuffer cfg_buf = graphics::get_constant_buffer(sizeof(RenderingConfigTest));
    assert(graphics::is_ready(&cfg_buf));
    graphics::update_constant_buffer(&cfg_buf, &cfg);
    graphics::set_constant_buffer(&cfg_buf, 0);

    graphics::Mesh quad = graphics::get_mesh(super_quad_test_vertices, super_quad_test_vertices_count,
                                             super_quad_test_vertices_stride, NULL, 0, 0);
    assert(graphics::is_ready(&quad));

    // DEVIATION from a literal "set_blend_state(ALPHA)": WebGPU forbids
    // blending on 32-bit float color targets without the unrequested
    // "float32-blendable" device feature -- same precedent as
    // test_offscreen_draw_and_readback's deviation comment above. OPAQUE is
    // used instead; this test only inspects the RT after a single opaque
    // draw over a zero-cleared background, so the blend equation choice does
    // not affect the assertions below.
    graphics::set_blend_state(graphics::BlendType::OPAQUE);
    graphics::draw_mesh(&quad);
    graphics::swap_frames();

    float pixels[RT_W * RT_H * 4];
    readback_rgba32f(rt.texture, RT_W, RT_H, pixels);

    // Center pixel (4,4): texcoord (0.5625, 0.4375, 0.25), inside the trim
    // box -> assert (2.0, 0.0, 0.0, 0.1580301).
    {
        uint32_t i = 4u * RT_W + 4u;
        float r = pixels[i * 4 + 0];
        float g = pixels[i * 4 + 1];
        float b = pixels[i * 4 + 2];
        float a = pixels[i * 4 + 3];
        const float tol = 1e-4f;
        assert(fabsf(r - 2.0f) < tol);
        assert(fabsf(g - 0.0f) < tol);
        assert(fabsf(b - 0.0f) < tol);
        assert(fabsf(a - 0.1580301f) < tol);
    }

    // Corner pixel (0,0): texcoord.x = 0.0625 < 0.2 -> trimmed -> assert
    // (0,0,0,0), pins the trim early-out.
    {
        uint32_t i = 0u * RT_W + 0u;
        float r = pixels[i * 4 + 0];
        float g = pixels[i * 4 + 1];
        float b = pixels[i * 4 + 2];
        float a = pixels[i * 4 + 3];
        const float tol = 1e-6f;
        assert(fabsf(r - 0.0f) < tol);
        assert(fabsf(g - 0.0f) < tol);
        assert(fabsf(b - 0.0f) < tol);
        assert(fabsf(a - 0.0f) < tol);
    }

    printf("render_path_tests: ps_volume_trace readback passed "
           "(center=(2.0,0.0,0.0,0.1580301), corner trimmed to zero)\n");

    graphics::unset_texture(0);
    graphics::unset_texture(1);
    graphics::release(&quad);
    graphics::release(&vs);
    graphics::release(&ps);
    graphics::release(&samp_trace);
    graphics::release(&samp_palette);
    graphics::release(&tex_trace);
    graphics::release(&tex_false_color);
    graphics::release(&cfg_buf);
    graphics::release(&rt);
}

// ---- Test (Task 9, M4b, DESIGN §6 -- highest-value new test): cs_volpath
// headless dispatch, four phases (task-9-brief.md) pinning end-to-end: the
// buffer accumulator RMW conversion, ceil-dispatch full coverage at a
// non-multiple-of-10 screen size, the OOB guard, and the blit's row-major
// agreement with the accumulator.
//
// Screen 25x15 (neither dimension a multiple of 10): dispatch
// ceil(25/10)=3 x ceil(15/10)=2 = 600 invocations, 375 in-bounds, 225
// guarded out by cs_volpath.wgsl's QUIRK(oob_dispatch_guard). The buffer is
// sized to exactly the 375 in-bounds pixels
// (get_structured_buffer(4*sizeof(float), 375) == 375 x 16 B) -- if the OOB
// guard were missing, WGSL's storage-buffer OOB-index CLAMP would land the
// 225 tail invocations' writes on the buffer's last real element (index
// 374) instead of discarding them, a >=0.8 deviation from Phase A's
// expected value there (brief step 1).
static void test_volpath_dispatch() {
    const uint32_t SCREEN_W = 25, SCREEN_H = 15;
    const uint32_t N = SCREEN_W * SCREEN_H;   // 375

    File cs_f = file_system::read_file(SHADER_DIR "/cs_volpath.wgsl");
    assert(cs_f.data != NULL);
    graphics::ComputeShader cs_volpath =
        graphics::get_compute_shader_from_code((char *)cs_f.data, cs_f.size);
    file_system::release_file(cs_f);
    assert(graphics::is_ready(&cs_volpath));
    assert(cs_volpath.uses_group0);

    File blit_f = file_system::read_file(SHADER_DIR "/cs_volpath_blit.wgsl");
    assert(blit_f.data != NULL);
    graphics::ComputeShader cs_volpath_blit =
        graphics::get_compute_shader_from_code((char *)blit_f.data, blit_f.size);
    file_system::release_file(blit_f);
    assert(graphics::is_ready(&cs_volpath_blit));
    assert(!cs_volpath_blit.uses_group0);

    // Resources (task-9-brief.md step 1): tex_trace + tex_deposit 8x8x8
    // R32_FLOAT (both start all-zero); palettes 2x2 RGBA8_UNORM (both start
    // solid black).
    std::vector<float> zero_volume(8 * 8 * 8, 0.0f);
    graphics::Texture3D tex_trace =
        graphics::get_texture3D(zero_volume.data(), 8, 8, 8, graphics::Format::R32_FLOAT, 4);
    assert(graphics::is_ready(&tex_trace));
    graphics::Texture3D tex_deposit =
        graphics::get_texture3D(zero_volume.data(), 8, 8, 8, graphics::Format::R32_FLOAT, 4);
    assert(graphics::is_ready(&tex_deposit));

    uint8_t black_data[2 * 2 * 4] = {};   // zero-init == solid black (rgb only is sampled)
    graphics::Texture2D tex_palette_trace =
        graphics::get_texture2D(black_data, 2, 2, graphics::Format::RGBA8_UNORM, 4);
    assert(graphics::is_ready(&tex_palette_trace));
    graphics::Texture2D tex_palette_data =
        graphics::get_texture2D(black_data, 2, 2, graphics::Format::RGBA8_UNORM, 4);
    assert(graphics::is_ready(&tex_palette_data));

    // Samplers: slots 1/2 (tex_trace/tex_deposit) CLAMP/ANISOTROPIC (matches
    // main.cpp's tex_sampler_trace/tex_sampler_deposit); slots 3/4
    // (palettes) default CLAMP/POINT.
    graphics::TextureSampler samp_trace =
        graphics::get_texture_sampler(graphics::CLAMP, graphics::Filter::ANISOTROPIC);
    graphics::TextureSampler samp_deposit =
        graphics::get_texture_sampler(graphics::CLAMP, graphics::Filter::ANISOTROPIC);
    graphics::TextureSampler samp_palette_trace = graphics::get_texture_sampler();
    graphics::TextureSampler samp_palette_data = graphics::get_texture_sampler();
    assert(graphics::is_ready(&samp_trace));
    assert(graphics::is_ready(&samp_deposit));
    assert(graphics::is_ready(&samp_palette_trace));
    assert(graphics::is_ready(&samp_palette_data));

    graphics::StructuredBuffer accum = graphics::get_structured_buffer(4 * sizeof(float), N);
    assert(graphics::is_ready(&accum));

    // Config (task-9-brief.md step 1, cfg indices per NOTES §1).
    RenderingConfigTest cfg = {};
    set_identity16(cfg.projection);
    set_identity16(cfg.view);
    set_identity16(cfg.model);
    cfg.screen_width = (float)SCREEN_W;
    cfg.screen_height = (float)SCREEN_H;
    cfg.world_width = 8.0f; cfg.world_height = 8.0f; cfg.world_depth = 8.0f;
    cfg.trim_x_min = 0.0f; cfg.trim_x_max = 1.0f;
    cfg.trim_y_min = 0.0f; cfg.trim_y_max = 1.0f;
    cfg.trim_z_min = 0.0f; cfg.trim_z_max = 1.0f;
    cfg.trim_density = 0.0f;
    cfg.sample_weight = 1.0f;
    cfg.galaxy_weight = 0.0f;
    cfg.camera_x = 0.0f; cfg.camera_y = -4.0f; cfg.camera_z = 0.0f;
    cfg.sigma_s = 0.0f;    // forces the deterministic emission-absorption ray-march branch
    cfg.sigma_a = 0.5f;
    cfg.sigma_e = 1.0f;
    cfg.trace_max = 1.0f;
    cfg.camera_offset_x = 0.0f; cfg.camera_offset_y = 0.0f;
    cfg.exposure = 1.0f;
    cfg.n_bounces = 2;
    cfg.ambient_trace = 0.0f;
    cfg.compressive_accumulation = 0;
    cfg.guiding_strength = 0.0f;
    cfg.scattering_anisotropy = 0.0f;
    // cfg.pt_iteration set per-phase below.

    graphics::ConstantBuffer cfg_buf = graphics::get_constant_buffer(sizeof(RenderingConfigTest));
    assert(graphics::is_ready(&cfg_buf));

    // Binds exactly like main.cpp:1370-1385 will (task-9-brief.md step 1):
    // buffer at compute slot 0, sampled textures 1-4, samplers 1-4;
    // set_constant_buffer before each dispatch. Unsets all afterwards
    // (strict-match contract).
    auto dispatch_volpath = [&]() {
        graphics::update_constant_buffer(&cfg_buf, &cfg);
        graphics::set_compute_shader(&cs_volpath);
        graphics::set_constant_buffer(&cfg_buf, 0);
        graphics::set_structured_buffer(&accum, 0);
        graphics::set_texture_sampled_compute(&tex_trace, 1);
        graphics::set_texture_sampler_compute(&samp_trace, 1);
        graphics::set_texture_sampled_compute(&tex_deposit, 2);
        graphics::set_texture_sampler_compute(&samp_deposit, 2);
        graphics::set_texture_sampled_compute(&tex_palette_trace, 3);
        graphics::set_texture_sampler_compute(&samp_palette_trace, 3);
        graphics::set_texture_sampled_compute(&tex_palette_data, 4);
        graphics::set_texture_sampler_compute(&samp_palette_data, 4);
        graphics::run_compute(3, 2, 1);
        graphics::unset_structured_buffer(0);
        graphics::unset_texture_sampled_compute(1);
        graphics::unset_texture_sampled_compute(2);
        graphics::unset_texture_sampled_compute(3);
        graphics::unset_texture_sampled_compute(4);
    };

    std::vector<float> out(N * 4);

    // ---- Phase A: deterministic RMW + guard (pt_iteration = 5) ----
    // trace/deposit ZERO, palettes BLACK -> every ray (hit or miss) yields
    // path_L = (0,0,0). Pre-fill the buffer with 375 x (6,6,6,6); expected
    // after one dispatch, for ALL 375 elements including the last:
    // 6*(5/6) + (0,0,0,1)/6 = (5.0, 5.0, 5.0, 5.1666665).
    std::vector<float> prefill(N * 4);
    for (uint32_t i = 0; i < N; i++) {
        prefill[i * 4 + 0] = 6.0f;
        prefill[i * 4 + 1] = 6.0f;
        prefill[i * 4 + 2] = 6.0f;
        prefill[i * 4 + 3] = 6.0f;
    }
    graphics::update_structured_buffer(&accum, prefill.data());
    cfg.pt_iteration = 5;
    dispatch_volpath();
    graphics::swap_frames();
    graphics::capture_structured_buffer(&accum, out.data(), N, 4 * sizeof(float));

    const float tolA = 1e-4f;
    for (uint32_t i = 0; i < N; i++) {
        assert(fabsf(out[i * 4 + 0] - 5.0f) < tolA);
        assert(fabsf(out[i * 4 + 1] - 5.0f) < tolA);
        assert(fabsf(out[i * 4 + 2] - 5.0f) < tolA);
        assert(fabsf(out[i * 4 + 3] - 5.1666665f) < tolA);
    }
    printf("render_path_tests: cs_volpath Phase A (deterministic RMW + guard) passed "
           "(elem[0]=(%.7f,%.7f,%.7f,%.7f), elem[374]=(%.7f,%.7f,%.7f,%.7f))\n",
           out[0], out[1], out[2], out[3],
           out[(N - 1) * 4 + 0], out[(N - 1) * 4 + 1], out[(N - 1) * 4 + 2], out[(N - 1) * 4 + 3]);

    // ---- Phase B: zero-fill + full coverage (pt_iteration = 0) ----
    // Same resources; expected ALL 375 elements exactly (0,0,0,1.0) --
    // zero-fill at cs_volpath.wgsl's pt_iteration==0 branch, then
    // 0*0/1 + vec4(path_L,1)/1. Alpha == 1.0 on every element pins that
    // ceil-dispatch covered every pixel of the 25x15 grid (a truncating
    // dispatch would leave rows 10-14 / cols 20-24 at their Phase A values).
    cfg.pt_iteration = 0;
    dispatch_volpath();
    graphics::swap_frames();
    graphics::capture_structured_buffer(&accum, out.data(), N, 4 * sizeof(float));

    const float tolB = 1e-6f;
    for (uint32_t i = 0; i < N; i++) {
        assert(fabsf(out[i * 4 + 0] - 0.0f) < tolB);
        assert(fabsf(out[i * 4 + 1] - 0.0f) < tolB);
        assert(fabsf(out[i * 4 + 2] - 0.0f) < tolB);
        assert(fabsf(out[i * 4 + 3] - 1.0f) < tolB);
    }
    printf("render_path_tests: cs_volpath Phase B (zero-fill + ceil-dispatch full "
           "coverage) passed (all 375 elements == (0,0,0,1.0))\n");

    // ---- Phase C: emission smoke (pt_iteration = 0) ----
    // Re-upload trace = 2.0 everywhere; tex_palette_trace solid red
    // (255,0,0,255); tex_palette_data stays black. Camera (0,-4,0) looks
    // through the volume center. No texture-update API exists (only
    // structured buffers support update_structured_buffer), so the
    // trace/palette textures are released and recreated with new data.
    graphics::release(&tex_trace);
    std::vector<float> trace_two(8 * 8 * 8, 2.0f);
    tex_trace = graphics::get_texture3D(trace_two.data(), 8, 8, 8, graphics::Format::R32_FLOAT, 4);
    assert(graphics::is_ready(&tex_trace));

    graphics::release(&tex_palette_trace);
    uint8_t red_data[2 * 2 * 4];
    for (int i = 0; i < 4; i++) {
        red_data[i * 4 + 0] = 255;
        red_data[i * 4 + 1] = 0;
        red_data[i * 4 + 2] = 0;
        red_data[i * 4 + 3] = 255;
    }
    tex_palette_trace = graphics::get_texture2D(red_data, 2, 2, graphics::Format::RGBA8_UNORM, 4);
    assert(graphics::is_ready(&tex_palette_trace));

    cfg.pt_iteration = 0;
    dispatch_volpath();
    graphics::swap_frames();
    graphics::capture_structured_buffer(&accum, out.data(), N, 4 * sizeof(float));

    // Center pixel index 7*25+12 = 187: red-only emission through
    // get_emitted_trace_L (HALO term contributes black-palette zero).
    {
        const uint32_t i = 7u * SCREEN_W + 12u;
        assert(i == 187);
        float r = out[i * 4 + 0];
        float g = out[i * 4 + 1];
        float b = out[i * 4 + 2];
        float a = out[i * 4 + 3];
        assert(r > 0.001f);
        assert(g < 1e-4f);
        assert(b < 1e-4f);
        assert(fabsf(a - 1.0f) < 1e-4f);
        printf("render_path_tests: cs_volpath Phase C (emission smoke) passed "
               "(center pixel[187]=(%.6f,%.6f,%.6f,%.6f))\n", r, g, b, a);
    }

    // Save Phase C's buffer contents for Phase D's blit-agreement check.
    std::vector<float> phase_c_out = out;

    // ---- Phase D: blit ----
    // 25x15 RGBA32_FLOAT Texture2D via cs_volpath_blit.wgsl; assert all 375
    // texels equal Phase C's buffer contents (pins the blit + row-major
    // index agreement).
    graphics::Texture2D display =
        graphics::get_texture2D(nullptr, SCREEN_W, SCREEN_H, graphics::Format::RGBA32_FLOAT, 4);
    assert(graphics::is_ready(&display));

    graphics::set_compute_shader(&cs_volpath_blit);
    graphics::set_structured_buffer(&accum, 0);
    graphics::set_texture_compute(&display, 1);
    graphics::run_compute(SCREEN_W, SCREEN_H, 1);
    graphics::swap_frames();
    graphics::unset_structured_buffer(0);
    graphics::unset_texture_compute(1);

    std::vector<float> blit_pixels(N * 4);
    readback_rgba32f(display.texture, SCREEN_W, SCREEN_H, blit_pixels.data());

    const float tolD = 1e-6f;
    for (uint32_t i = 0; i < N * 4; i++) {
        assert(fabsf(blit_pixels[i] - phase_c_out[i]) < tolD);
    }
    printf("render_path_tests: cs_volpath Phase D (blit agreement) passed "
           "(all 375 texels match the accumulator buffer, tol 1e-6)\n");

    graphics::release(&display);
    graphics::release(&cfg_buf);
    graphics::release(&accum);
    graphics::release(&samp_trace);
    graphics::release(&samp_deposit);
    graphics::release(&samp_palette_trace);
    graphics::release(&samp_palette_data);
    graphics::release(&tex_trace);
    graphics::release(&tex_deposit);
    graphics::release(&tex_palette_trace);
    graphics::release(&tex_palette_data);
    graphics::release(&cs_volpath);
    graphics::release(&cs_volpath_blit);
}

int main() {
    bool ok = graphics::init();   // headless: no init_swap_chain
    assert(ok);

    test_shader_compile_validation();
    test_offscreen_draw_and_readback();
    test_load_texture2D_palette();
    test_unset_texture_clears_sampler();
    test_particle_chain_pixels_exist();
    test_super_quad_stride_draw();
    test_compute_sampler_pairing();
    test_resize_cycle_offscreen();
    test_volume_trace_readback();
    test_volpath_dispatch();

    graphics::release();
    printf("All render path tests passed\n");
    return 0;
}
