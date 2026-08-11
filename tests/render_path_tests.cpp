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

int main() {
    bool ok = graphics::init();   // headless: no init_swap_chain
    assert(ok);

    test_shader_compile_validation();
    test_offscreen_draw_and_readback();
    test_particle_chain_pixels_exist();

    graphics::release();
    printf("All render path tests passed\n");
    return 0;
}
