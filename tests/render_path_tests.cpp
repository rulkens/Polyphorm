// Headless render-path tests (M3 Task 1). Same init pattern as
// tests/shader_compile_tests.cpp (graphics::init()/release(), no window/
// surface). Exercises: vs/ps compile validation, and a full offscreen
// draw_mesh + pixel readback that kills DESIGN risk #1 (texture/sampler
// 2N/2N+1 bind convention) and risk #4 (pipeline validation surfaces only
// at draw_mesh time) headlessly. See docs/superpowers/research/m3/
// render-path-design.md.
#include "../cpplib/graphics.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>

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

// Reads back an RGBA32_FLOAT render target into `out` (width*height*4 floats).
// CopyTextureToBuffer requires bytesPerRow padded to 256.
static void readback_rgba32f(graphics::RenderTarget *rt, uint32_t width, uint32_t height,
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
    src.texture = rt->texture;
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
    readback_rgba32f(&rt, RT_W, RT_H, pixels);

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

int main() {
    bool ok = graphics::init();   // headless: no init_swap_chain
    assert(ok);

    test_shader_compile_validation();
    test_offscreen_draw_and_readback();

    graphics::release();
    printf("All render path tests passed\n");
    return 0;
}
