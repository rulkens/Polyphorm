# M4b — Volume Rendering + Path Tracing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The volume trace view AND the volumetric path tracer are real — `vs_3d` + all five `ps_volume_*` shaders and `cs_volpath`/`ps_volpath` are ported to WGSL, palettes load from real TGA files, and the user can orbit teal filaments in VM_VOLUME and watch VM_PATH_TRACING converge, reset on camera moves, and survive window resizes.

**Architecture:** Follow `docs/superpowers/research/m4/m4b-volume-pt-design.md` (**DESIGN**) — it resolves every translation and integration question with file:line evidence, and `docs/superpowers/research/m3/wgsl-drafts/translation-notes.md` (**NOTES**) carries the house translation conventions (full 336-byte shared struct, QUIRK markers, OOB-guard rationale, RNG porting). The hard problem is DESIGN §2.6: `cs_volpath`'s `RWTexture2D<float4>` accumulator has no WGSL `read_write` equivalent, so it becomes a `vec4<f32>` storage buffer plus a new tiny blit shader — the exact buffer+blit shape M3 already built, tested, and gated. Two independent tracks (volume: Tasks 3–7; PT: Tasks 8–11) fan out after the shared prerequisites (Tasks 1–2).

**Tech Stack:** Dawn WebGPU / WGSL (pinned via FetchContent), stb_image v2.30 (vendored single header, TGA decode), C++17, CMake/ctest, Dear ImGui (already landed, untouched here).

## Global Constraints

- Build incrementally ONLY: `cmake -B build` (config already exists), `nice -n 19 cmake --build build -j 8`. NEVER wipe `build/` (Dawn re-download), NEVER bare `-j`.
- ALL 7 ctest suites green before EVERY commit: `ctest --test-dir build` runs `cpplib_tests`, `file_system_tests`, `graphics_tests`, `shader_compile_tests`, `render_path_tests`, `sim_kernel_tests`, `energy_smoke`. energy_smoke must not regress; the headless path is unaffected by everything in this plan (no vis-mode block runs headless — `main.cpp:1252` gates on `!headless`).
- Binding conventions (pinned, load-bearing, already implemented in `cpplib/graphics.cpp`): compute non-sampler resources at `@group(1) @binding(slot)`; compute samplers at `@group(1) @binding(16 + slot)` (`graphics.cpp:1072-1080`); render textures at `@group(1) @binding(2*slot)`, render samplers at `@binding(2*slot + 1)` (`graphics.cpp:821-838`); cbuffers at `@group(0) @binding(0)`. `cs_volpath` declares FOUR samplers: s1/s2/s3/s4 → `@binding(17/18/19/20)` (m4a-carryovers.md binding contract — s2 IS used, at `cs_volpath.hlsl:183`).
- Quirk discipline: mechanical bug-for-bug ports, no behavior "fixes". The ONLY sanctioned deviations from upstream behavior in M4b are these five adjudications (user-decided 2026-08-12; do NOT reopen):
  1. **PT dispatch rounds UP** (ceil division) with an explicit OOB pixel guard in `cs_volpath.wgsl`. Rationale: bit-identical to upstream at the original fixed window size (upstream dims were exact multiples of 10); resizable windows are this port's own extension, so there is no upstream behavior to preserve at other sizes. The guard is QUIRK-marked (it restores D3D11 discard-on-OOB semantics that the buffer conversion would otherwise break — NOTES T2 class).
  2. **PT accumulator is a `vec4<f32>` storage buffer + blit shader** (DESIGN §2.6 option a) — no faithful single-resource WGSL port exists; exact numeric match (one thread per pixel, no races).
  3. **Palette textures decode to `RGBA8_UNORM`** (not `_SRGB`); correctness verified by screenshot-compare at the human gate — palette hue/brightness deviations there are gate FINDINGS, not automatic failures.
  4. **`ps_volpath`'s `dummy2`/`dummy3` cbuffer tail** is declared with the canonical `RenderingConfig` names/types (`guiding_strength`/`scattering_anisotropy`, f32) — same byte offsets, unused by the shader (DESIGN §2.7).
  5. **`ps_volume_halocolor` / `ps_volume_velocity` are file-parity ports only** — committed, compile-suite covered, NOT wired into main.cpp, NOT part of the done bar (dead code in `REGIME_SDSS`, DESIGN §1).
- Every shader port declares the FULL 336-byte `RenderingConfig` WGSL struct (3 mat4 + 36 scalars) regardless of the HLSL cbuffer's prefix-subset — copy the struct text verbatim from `shaders/cs_particles_transform.wgsl` (NOTES §1 has the byte-offset table; zero layout fixups needed).
- HUD/histogram fixed-anchor fix (m4a-carryovers.md #3) is explicitly NOT in M4b scope — it stays a carryover.
- After ANY manual `./build/polyphorm` run: kill the process if it didn't exit; leave NO instances running (`pgrep polyphorm` must come back empty). NEVER use osascript or macOS accessibility automation for anything.
- End every commit message with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

## File Structure

- New WGSL: `shaders/vs_3d.wgsl`, `shaders/ps_volume_trace.wgsl`, `shaders/ps_volume_highlight.wgsl`, `shaders/ps_volume_overdensity.wgsl`, `shaders/ps_volume_halocolor.wgsl`, `shaders/ps_volume_velocity.wgsl`, `shaders/cs_volpath.wgsl`, `shaders/cs_volpath_blit.wgsl` (NEW infrastructure, no HLSL counterpart), `shaders/ps_volpath.wgsl`
- New vendor: `cpplib/stb_image.h`
- Modified: `main.cpp`, `cpplib/graphics.cpp`, `CMakeLists.txt`, `tests/shader_compile_tests.cpp`, `tests/render_path_tests.cpp`
- Docs: `docs/superpowers/research/m3/m3-carryovers.md`, `docs/superpowers/research/m4/m4a-carryovers.md`, `docs/superpowers/research/m4/m4b-carryovers.md` (NEW, Task 12)

Dependency shape: Task 1 → Task 2 → {volume track: 3 → 4 → 5 → 6, plus 7 anytime after 3} and {PT track: 8 → 9, 10, then 11}. The two tracks are independent and may be executed by parallel workers after Task 2 lands, provided each track commits only its own files.

---

### Task 1: Per-draw constant-buffer binds — four sites (DESIGN §5)

**Files:**
- Modify: `main.cpp` (4 one-line additions)

**Interfaces:**
- Consumes: `graphics::set_constant_buffer(ConstantBuffer *buffer, uint32_t slot)` (`graphics.h:205`), existing `rendering_settings_buffer`.
- Produces: every volume/PT draw site binds group 0 before `draw_mesh`, so `draw_mesh`'s `needs_group0` fatal (`graphics.cpp:810`) can never fire once real shaders load. Tasks 5/6/11 depend on this; Task 3's test positively pins the group0 render-draw path for the first time.

`draw_mesh` computes `needs_group0 = g_vertex_shader->uses_group0 || g_pixel_shader->uses_group0` (`graphics.cpp:807`). `vs_3d` declares a cbuffer, so all three volume draws need the bind; `ps_volpath` declares a cbuffer even though its paired `vs_2d` does not, so the PT quad draw is the fourth site (DESIGN §5 — NOT named in the older carryover text, which said "three").

- [ ] **Step 1: Three volume draw sites.** In the `VM_VOLUME*` block, insert `graphics::set_constant_buffer(&rendering_settings_buffer, 0);` between each `graphics::update_constant_buffer(&rendering_settings_buffer, &rendering_config);` and its `graphics::draw_mesh(&super_quad_mesh);` — the three sites are the branches ending at `main.cpp:1344`, `:1350`, `:1356` (line numbers pre-edit; each subsequent insert shifts the next by one).

- [ ] **Step 2: Fourth site — PT quad draw.** Inside the `if (graphics::is_ready(&ps_volpath))` block (`main.cpp:1400-1407`), insert `graphics::set_constant_buffer(&rendering_settings_buffer, 0);` immediately before `graphics::draw_mesh(&quad_mesh);` (`:1405`). Comment it: `// ps_volpath declares a cbuffer even though vs_2d doesn't — needs_group0 ORs both stages (design §5, 4th site)`.

- [ ] **Step 3: Build + all suites + commit.** These lines are inert today (both draw blocks are gated behind `is_ready` on still-`{}` shaders), so all suites must stay green unchanged:

```bash
nice -n 19 cmake --build build -j 8 && ctest --test-dir build
git add main.cpp
git commit -m "m4b: per-draw constant-buffer binds at all four volume/PT draw sites (m3 carryover + design §5 fourth site)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Real `load_texture2D` — stb_image, DATA_ROOT, RGBA8 palettes (DESIGN §4)

**Files:**
- Create: `cpplib/stb_image.h` (vendored, v2.30)
- Modify: `cpplib/graphics.cpp` (replace the stub at `:464-471`), `CMakeLists.txt` (DATA_ROOT defines), `main.cpp` (`:544-545` call sites)
- Test: `tests/render_path_tests.cpp` (new `test_load_texture2D_palette`)

**Interfaces:**
- Consumes: `graphics::get_texture2D(void *data, uint32_t width, uint32_t height, Format format, uint32_t pixel_byte_count = 4)` (`graphics.h:160`), `Format::RGBA8_UNORM` (already mapped: `graphics.cpp:306` → `RGBA8Unorm`, `:316` → 4 B/px).
- Produces: `Texture2D graphics::load_texture2D(std::string filename)` — real decode, unchanged signature. `main.cpp:544-545` get real palette textures; the M3 gate run recipe (`cd /tmp/polyviz && …`) keeps working because paths become absolute.

All 15 `bin/data/*.tga` are TGA type 2, 24 bpp BGR, bottom-left origin, no RLE (DESIGN §4, verified by header inspection) — `stbi_load` handles all of that in one call, returning top-down RGBA rows.

- [ ] **Step 1: Failing test first.** Add DATA_ROOT to the test target — in `CMakeLists.txt`, extend `target_compile_definitions(render_path_tests PRIVATE SHADER_DIR=...)` (`:176-177`) with `DATA_ROOT="${CMAKE_SOURCE_DIR}/bin"`. Then add `test_load_texture2D_palette()` to `tests/render_path_tests.cpp` (register in `main()` after `test_offscreen_draw_and_readback`):
  - `graphics::Texture2D pal = graphics::load_texture2D(DATA_ROOT "/data/palette_hot.tga");`
  - `assert(pal.width == 130); assert(pal.height == 16); assert(pal.format == graphics::Format::RGBA8_UNORM);`
  - Draw it 1:1: `RenderTarget rt = get_render_target(130, 16, Format::RGBA32_FLOAT)`, clear to (0,0,0,1), default sampler (`get_texture_sampler()` = POINT/CLAMP), the file's existing 24 B `quad_vertices` mesh, `QUAD_VS_WGSL` + a new inline pass-through PS (returns `textureSample(tex, samp, uv)` — bindings `@group(1) @binding(0)`/`(1)`, same shape as `SAMPLE_PS_WGSL` but full RGBA), `set_blend_state(OPAQUE)`, `set_texture(&pal, 0)` + `set_texture_sampler`, `draw_mesh`, `swap_frames()`, `readback_rgba32f`.
  - Pixel mapping (derive holds because the quad maps NDC y=-1→v=0 and RT row 0 is NDC top): RT pixel `(x, 7)` samples uv `((x+0.5)/130, 1 - 7.5/16 = 0.53125)` → texel `(x, 8)` in top-down texture rows → TGA file scanline `15-8 = 7`. Hand-derived expected values (extracted from `bin/data/palette_hot.tga` file scanline 7):
    - pixel (0, 7) == (0.0, 0.0, 0.0, 1.0)
    - pixel (64, 7) == (250/255, 123/255, 0.0, 1.0) = (0.9803922, 0.4823529, 0.0, 1.0)
    - pixel (129, 7) == (254/255, 254/255, 250/255, 1.0) = (0.9960784, 0.9960784, 0.9803922, 1.0)
  - Tolerance `1.5e-3` per channel — tighter than one 8-bit LSB (1/255 ≈ 3.9e-3), so a missed vertical flip fails (file scanline 8 has (251,122,0) at x=64) and a missed BGR→RGBA swizzle fails loudly ((0,123,250) at x=64). Alpha comes from stbi's 3→4 channel expansion (opaque 255 → 1.0).
  - Unset/release everything the test bound (global render state persists across tests in this file).
  - Run: `nice -n 19 cmake --build build -j 8 && ctest --test-dir build -R render_path_tests` — expect **FAIL** (stub returns a 1×1 white RGBA32_FLOAT texture; the `width == 130` assert trips).

- [ ] **Step 2: Vendor stb_image.** Download the single header once (network fetch is allowed for vendoring, not for builds): `curl -fsSL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o cpplib/stb_image.h`. Verify the header self-reports `stb_image - v2.30` (or newer; record the exact version string in the commit message). It is committed to the repo — no build-time fetch.

- [ ] **Step 3: Replace the stub.** In `cpplib/graphics.cpp`, near the top with the other includes add:
  ```cpp
  #define STB_IMAGE_IMPLEMENTATION
  #define STBI_ONLY_TGA   // palettes are the only stbi consumers; keep the object small
  #include "stb_image.h"
  ```
  Replace `load_texture2D` (`:464-471`) with:
  ```cpp
  Texture2D load_texture2D(std::string filename) {
      int w = 0, h = 0, n = 0;
      unsigned char *pixels = stbi_load(filename.c_str(), &w, &h, &n, 4); // force RGBA
      if (!pixels) {
          char msg[512];
          snprintf(msg, sizeof(msg), "load_texture2D: cannot load %s (%s)",
                   filename.c_str(), stbi_failure_reason());
          fatal(msg);   // palette missing == packaging error; fail loud like shader loads
      }
      // RGBA8_UNORM, not _SRGB: adjudicated 2026-08-12 — plain truecolor LUT data,
      // verified by screenshot-compare at the M4b human gate (deviations = gate findings).
      Texture2D t = get_texture2D(pixels, (uint32_t)w, (uint32_t)h, Format::RGBA8_UNORM);
      stbi_image_free(pixels);
      return t;
  }
  ```

- [ ] **Step 4: DATA_ROOT absolute paths.** In `CMakeLists.txt` extend the polyphorm defines (`:95`) to `target_compile_definitions(polyphorm PRIVATE SHADER_ROOT="${CMAKE_SOURCE_DIR}/shaders" DATA_ROOT="${CMAKE_SOURCE_DIR}/bin")`. In `main.cpp:544-545` change the two call sites to string-literal concatenation (the `COLOR_PALETTE_*` macros — all 12 regime-conditional pairs — stay byte-unchanged since they already carry the `data/` prefix):
  ```cpp
  Texture2D palette_trace_tex = graphics::load_texture2D(DATA_ROOT "/" COLOR_PALETTE_TRACE);
  Texture2D palette_data_tex = graphics::load_texture2D(DATA_ROOT "/" COLOR_PALETTE_DATA);
  ```
  Comment: `// DATA_ROOT mirrors SHADER_ROOT's absolute-path fix — the M3 gate recipe runs from /tmp/polyviz with no data/ subdir (design §4)`.

- [ ] **Step 5: Build + all suites + commit.** `nice -n 19 cmake --build build -j 8 && ctest --test-dir build` — expect the Step 1 test **PASS** and all 7 suites green (the loaded 130×16 / 620×81 palettes replace 1×1 whites in the running app; nothing samples them yet outside the test).

```bash
git add cpplib/stb_image.h cpplib/graphics.cpp CMakeLists.txt main.cpp tests/render_path_tests.cpp
git commit -m "graphics: real load_texture2D — vendored stb_image v2.30, TGA palettes as RGBA8_UNORM, DATA_ROOT absolute paths

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: `vs_3d.wgsl` + 28-byte-stride draw test (DESIGN §2.1, §3)

**Files:**
- Create: `shaders/vs_3d.wgsl`
- Test: `tests/shader_compile_tests.cpp` (one entry), `tests/render_path_tests.cpp` (new `test_super_quad_stride_draw`)

**Interfaces:**
- Consumes: full-struct `RenderingConfig` WGSL text from `shaders/cs_particles_transform.wgsl`; `fill_vertex_attributes`'s existing `case 28:` (`graphics.cpp:686-691`: Float32x4 @0 loc 0, Float32x3 @16 loc 1 — already correct, zero draw coverage).
- Produces: `vs_3d.wgsl` — entry `fn main`, `uses_group0 = true`. Interstage contract consumed by ALL `ps_volume_*` ports (Tasks 4/6/7): output `@builtin(position) position_out: vec4<f32>` + `@location(0) texcoord_out: vec3<f32>`. Vertex inputs `@location(0) position: vec4<f32>`, `@location(1) texcoord: vec3<f32>` (matches `super_quad_vertices_stride = 28`, `main.cpp:694`).

- [ ] **Step 1: Failing tests first.**
  - `tests/shader_compile_tests.cpp`: add `check_compiles_vs(SHADER_DIR "/vs_3d.wgsl", 1);` after the `vs_2d.wgsl` line.
  - `tests/render_path_tests.cpp`: add `test_super_quad_stride_draw()`:
    - Inline a 6-vertex, 7-float (28 B) stride array — the single-quad shape of `super_quad_vertices_template`, with texcoords that make layout errors observable:
      ```cpp
      static float super_quad_test_vertices[] = {
          -1.f,-1.f,0.f,1.f,  0.f,0.f,0.25f,
           1.f, 1.f,0.f,1.f,  1.f,1.f,0.25f,
          -1.f, 1.f,0.f,1.f,  0.f,1.f,0.25f,
          -1.f,-1.f,0.f,1.f,  0.f,0.f,0.25f,
           1.f,-1.f,0.f,1.f,  1.f,0.f,0.25f,
           1.f, 1.f,0.f,1.f,  1.f,1.f,0.25f,
      };  // stride = 7 * sizeof(float) = 28
      ```
    - Load `shaders/vs_3d.wgsl` from disk (`SHADER_DIR`, same `file_system::read_file` pattern as `shader_compile_tests`), pair with an inline trivial PS that visualizes the interstage texcoord: `@fragment fn main(@location(0) tc : vec3<f32>) -> @location(0) vec4<f32> { return vec4<f32>(tc, 1.0); }` (no bindings).
    - Uniform: build a 336-byte config as `float cfg[84]` — identity mat4 at float indices 0-15/16-31/32-47; scalar *i* of the 36 lives at index `48+i` (NOTES §1 table). Set `texcoord_map` (int bits at index 48) = 1; everything else 0. `get_constant_buffer(336)`, `update_constant_buffer`, `set_constant_buffer(&cb, 0)` — with identity matrices `vs_3d` passes positions through and `texcoord_map==1` passes texcoords through. This is also the FIRST positive-path coverage of `draw_mesh`'s group-0 branch (`graphics.cpp:807-818`).
    - Draw into a 4×4 RGBA32_FLOAT RT (cleared to 0,0,0,0), OPAQUE blend, `swap_frames()`, `readback_rgba32f`.
    - Assert every pixel `(x, y)` of the 4×4: `r == (x+0.5)/4`, `g == 1.0 - (y+0.5)/4` (RT row 0 is NDC top; v interpolates as `(y_ndc+1)/2`), `b == 0.25`, `a == 1.0`, tolerance `1e-5`. Any attribute offset/stride mistake shifts position lanes (±1.0 / w=1.0) into the texcoord and breaks these values.
    - Unset/release everything.
  - Run: `nice -n 19 cmake --build build -j 8 && ctest --test-dir build -R "shader_compile_tests|render_path_tests"` — expect **FAIL** in both (file missing: `read_file` assert).

- [ ] **Step 2: Port the shader.** Write `shaders/vs_3d.wgsl` per DESIGN §2.1 — a mechanical port of `shaders/vs_3d.hlsl` (42 lines) in the house style of `shaders/vs_2d.wgsl` (header comment citing draw sites `main.cpp:1298-1357`, per-line HLSL references):
  - Full 336-byte `RenderingConfig` struct at `@group(0) @binding(0)` (copy text from `cs_particles_transform.wgsl`; the HLSL cbuffer stops at `texcoord_map` — declare all 36 scalars anyway, §2 intro rule).
  - `result.position_out = cfg.projection * (cfg.view * (cfg.model * input.position));` — no transpose anywhere (NOTES §1 matrix cross-check).
  - The six-branch `texcoord_map` if/else-if permutation switch ported LITERALLY (spec line 103 requires it). Use `var texcoord_out = vec3<f32>(0.0);` before the chain — HLSL leaves it uninitialized when no branch matches (undefined behavior, unreachable in practice since main.cpp only ever sets ±1/±2/±3); WGSL's mandatory zero-init is the closest defined reproduction. One-line comment noting this.
- Run: compile suite + the new draw test → expect **PASS**.

- [ ] **Step 3: Build + all suites + commit.**

```bash
nice -n 19 cmake --build build -j 8 && ctest --test-dir build
git add shaders/vs_3d.wgsl tests/shader_compile_tests.cpp tests/render_path_tests.cpp
git commit -m "shaders: vs_3d.wgsl port + 28-byte-stride draw coverage (closes m3-carryovers #5)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: `ps_volume_trace.wgsl` + volume readback test (DESIGN §2.2, §6)

**Files:**
- Create: `shaders/ps_volume_trace.wgsl`
- Test: `tests/shader_compile_tests.cpp` (one entry), `tests/render_path_tests.cpp` (new `test_volume_trace_readback`)

**Interfaces:**
- Consumes: Task 3's interstage contract (`@location(0) tc: vec3<f32>` in).
- Produces: `ps_volume_trace.wgsl` — entry `fn main`, `uses_group0 = true`. Bindings (render 2N/2N+1, matching `main.cpp:1299-1306`): `tex_trace: texture_3d<f32>` @binding(0), `tex_sampler_trace: sampler` @binding(1), `tex_false_color: texture_2d<f32>` @binding(2), `tex_false_color_sampler: sampler` @binding(3).

- [ ] **Step 1: Failing tests first.**
  - `shader_compile_tests.cpp`: `check_compiles_ps(SHADER_DIR "/ps_volume_trace.wgsl", 1);`
  - `render_path_tests.cpp`: `test_volume_trace_readback()` — same skeleton as Task 3's test (vs_3d from disk, 28 B quad, 336 B cfg buffer) but with ps_volume_trace from disk and real sampled resources:
    - `tex_trace`: 4×4×4 `R32_FLOAT` `Texture3D` via `get_texture3D(data, 4, 4, 4, Format::R32_FLOAT, 4)` with all 64 floats = `1.0f`.
    - `tex_false_color`: 2×2 `RGBA8_UNORM` `Texture2D`, all four texels `(255, 0, 0, 255)` (solid red — makes the palette sample position irrelevant).
    - Samplers: trace slot 0 `get_texture_sampler(CLAMP, Filter::ANISOTROPIC)` (matches `main.cpp:547`; linear-filters r32float — `Float32Filterable` is a required device feature, `gpu_context.cpp:42/69`), palette slot 1 default sampler.
    - cfg scalars (indices per NOTES §1): `texcoord_map`(48)=1; trims min/max (49-54) = 0.2/0.8 on all three axes (tight window so a mis-strided texcoord reading a position/w lane of ±1.0/1.0 lands OUTSIDE the trim and fails); `trim_density`(55)=0.5; `sample_weight`(61)=2.0; `optical_thickness`(62)=0.25. Matrices identity.
    - Draw into an 8×8 RGBA32_FLOAT RT cleared to (0,0,0,0), OPAQUE blend (WebGPU forbids blending on float32 RTs without the unrequested `float32-blendable` feature — same precedent as `test_offscreen_draw_and_readback`'s deviation comment).
    - Hand-derived expectations: `t = (1.0 - 0.5) * 2.0 = 1.0`; `remap(1,1) = 1 - e^-1 = 0.6321206`; rgb = red palette × then `*= 2.0` → **(2.0, 0.0, 0.0)**; `a = 0.25 * 0.6321206 = 0.1580301`.
      - Center pixel (4,4): texcoord (0.5625, 0.4375, 0.25), inside trim → assert `(2.0, 0.0, 0.0, 0.1580301)` tolerance `1e-4`.
      - Corner pixel (0,0): texcoord x = 0.0625 < 0.2 → trimmed → `fragment = vec4(0)` then `rgb *= 2` → assert `(0, 0, 0, 0)` tolerance `1e-6` (pins the trim early-out).
    - Unset/release everything.
  - Run: `ctest --test-dir build -R "shader_compile_tests|render_path_tests"` after build — expect **FAIL** (file missing).

- [ ] **Step 2: Port the shader.** `shaders/ps_volume_trace.wgsl` per DESIGN §2.2 — literal port of the 95-line HLSL:
  - Full 336 B struct at `@group(0) @binding(0)`, using canonical field names (`world_width/height/depth` — the HLSL's `world_X/Y/Z` are naming drift at the same offsets, DESIGN §2 intro).
  - `remap(val, slope) = 1.0 - exp(-slope * val)` helper.
  - `textureSample` on `texture_3d<f32>` called unconditionally at the top of `main` before any branch (matches HLSL `:45`; keeps WGSL derivative-uniformity trivially satisfied). Keep the existing explicit `.r`.
  - Trim early-out and else-branch verbatim. Port the commented-out "Proxy draws config" block (`:57-90`) verbatim AS A COMMENT (1:1 file correspondence, NOTES T11 precedent).
  - Final line `// QUIRK(single_stack_2x_compensation): kept for VAC parity — upstream draws 1 stack instead of 3` on `fragment = vec4<f32>(fragment.rgb * 2.0, fragment.a);`.
  - No OOB texcoord guard — CLAMP samplers behave identically in both APIs and the mesh construction bounds texcoords (DESIGN §2.2 "no divergence" case).
- Run: both suites → expect **PASS**.

- [ ] **Step 3: Build + all suites + commit.**

```bash
nice -n 19 cmake --build build -j 8 && ctest --test-dir build
git add shaders/ps_volume_trace.wgsl tests/shader_compile_tests.cpp tests/render_path_tests.cpp
git commit -m "shaders: ps_volume_trace.wgsl port + hand-derived volume readback test

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: VM_VOLUME wiring — real shader loads (DESIGN §5)

**Files:**
- Modify: `main.cpp` (`:522-523`)

**Interfaces:**
- Consumes: Tasks 1-4; the existing `load_vs`/`load_ps` lambdas (`main.cpp:497-514`).
- Produces: a live VM_VOLUME mode. The `is_ready` gate at `main.cpp:1336` self-activates — no gate edits (DESIGN §5: gates already correct, comments already say "M4b: real shader lands").

- [ ] **Step 1: Replace the stubs.** `main.cpp:522-523`:
  ```cpp
  VertexShader vertex_shader = load_vs(SHADER_ROOT "/vs_3d.wgsl");
  PixelShader pixel_shader = load_ps(SHADER_ROOT "/ps_volume_trace.wgsl"),
  ```
  (`ps_volume_highlight`/`ps_volume_halocolor`/`ps_volume_overdensity`/`ps_volume_velocity`/`ps_volpath` stay `= {}` on that declaration — Tasks 6/11 take them.)

- [ ] **Step 2: Build + all suites.** `nice -n 19 cmake --build build -j 8 && ctest --test-dir build` — all green (energy_smoke runs the app headless, which loads the new shaders at startup: any Dawn compile error in a shipped WGSL fails here loudly).

- [ ] **Step 3: Windowed smoke.** From a dataset dir (M3 gate recipe: `cd /tmp/polyviz && /Users/rulkens/Development/vendor/cpp/Polyphorm/build/polyphorm --dataset ...` — reuse whatever synthetic dataset invocation the M3/M4a gates used, regenerating via `gen_test_dataset` if `/tmp/polyviz` is gone). Verify startup prints `.../vs_3d.wgsl compiled...` and `.../ps_volume_trace.wgsl compiled...`, no Dawn validation errors over ~30 s of uptime (input can't be injected — mode switching is verified at the Task 13 human gate). Exit/kill; confirm `pgrep polyphorm` is empty.

- [ ] **Step 4: Commit.**

```bash
git add main.cpp
git commit -m "m4b: VM_VOLUME wired — real vs_3d/ps_volume_trace loads replace {} stubs

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: `ps_volume_highlight` + `ps_volume_overdensity` ports, wired (DESIGN §2.3, §2.4)

**Files:**
- Create: `shaders/ps_volume_highlight.wgsl`, `shaders/ps_volume_overdensity.wgsl`
- Modify: `main.cpp` (`:523` declaration line)
- Test: `tests/shader_compile_tests.cpp` (two entries)

**Interfaces:**
- Consumes: Task 3's interstage contract; Task 5's wiring pattern.
- Produces: live VM_VOLUME_HIGHLIGHT / VM_VOLUME_OVERDENSITY. Bindings — highlight: `tex_trace`/sampler @(0)/(1), `tex_deposit: texture_3d<f32>`/sampler @(2)/(3) (main.cpp binds `trail_tex_A|B` at slot 1, `:1312-1315`); overdensity: `tex_trace`/sampler @(0)/(1) only.

- [ ] **Step 1: Failing tests first.** `shader_compile_tests.cpp`: `check_compiles_ps(SHADER_DIR "/ps_volume_highlight.wgsl", 1);` and `check_compiles_ps(SHADER_DIR "/ps_volume_overdensity.wgsl", 1);`. Build + `ctest --test-dir build -R shader_compile_tests` — expect **FAIL**.

- [ ] **Step 2: Port `ps_volume_highlight.wgsl`** (DESIGN §2.3 — same shape as trace plus deposit sample + smoothstep highlight band):
  - Full struct; trim early-out; `smoothstep` is a WGSL builtin with identical semantics.
  - HLSL `:51`/`:53` assign a `Sample(...)` float4 to `float` with IMPLICIT truncation — make it explicit `.x` in WGSL (NOTES V6 precedent), with a one-line comment.
  - `0.0.xxxx` → `vec4<f32>(0.0)`. Keep the commented-out alternate color/highlight lines as comments. QUIRK(single_stack_2x_compensation) on the final `*= 2.0`.
- [ ] **Step 3: Port `ps_volume_overdensity.wgsl`** (DESIGN §2.4):
  - The three `#define COLOR_*` macros become module-level consts, values verbatim:
    ```wgsl
    const COLOR_UNDERDENSE = vec4<f32>(0.0, 0.0, 25.0, 0.08);
    const COLOR_MIDDENSE   = vec4<f32>(0.0, 2.0, 0.0, 0.06);
    const COLOR_OVERDENSE  = vec4<f32>(10.0, 0.0, 0.0, 0.15);
    ```
  - `// QUIRK(overdensity_constant_remap): remap(2.71, sample_weight) — 2.71 is a literal (≈e), the brightness term depends only on sample_weight, NOT on trace. Faithful; do not "fix".` (HLSL `:63`.)
  - Implicit float4→float truncation on the trace sample → explicit `.x` + comment. `var fragment = vec4<f32>(0.0);` init (branch chain is exhaustive; init is WGSL-mandatory).
- [ ] **Step 4: Wire.** `main.cpp:523-524`: `ps_volume_highlight = load_ps(SHADER_ROOT "/ps_volume_highlight.wgsl")`, `ps_volume_overdensity = load_ps(SHADER_ROOT "/ps_volume_overdensity.wgsl")` (halocolor/velocity/ps_volpath still `= {}`).
- [ ] **Step 5: Build + all suites + smoke + commit.** Suites green; brief windowed run as in Task 5 Step 3 (startup compiles both, no Dawn errors); kill; `pgrep polyphorm` empty.

```bash
nice -n 19 cmake --build build -j 8 && ctest --test-dir build
git add shaders/ps_volume_highlight.wgsl shaders/ps_volume_overdensity.wgsl main.cpp tests/shader_compile_tests.cpp
git commit -m "shaders: ps_volume_highlight + ps_volume_overdensity ports, wired into VM_VOLUME_* modes

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: `ps_volume_halocolor` + `ps_volume_velocity` — file-parity ports ONLY (DESIGN §2.5, adjudication 5)

**Files:**
- Create: `shaders/ps_volume_halocolor.wgsl`, `shaders/ps_volume_velocity.wgsl`
- Test: `tests/shader_compile_tests.cpp` (two entries — the ONLY test coverage these get)

**Interfaces:**
- Produces: compile-clean file-parity ports. NOT wired (`main.cpp:524` keeps `ps_volume_halocolor = {}` / `ps_volume_velocity = {}` — both are unreachable dead code in `REGIME_SDSS`: their vis-mode toggles are `#ifdef`-compiled out, `main.cpp:1679-1704`). NOT part of the M4b done bar; no draw-path tests.

- [ ] **Step 1: Compile-suite entries first.** `check_compiles_ps(SHADER_DIR "/ps_volume_halocolor.wgsl", 1);` and `check_compiles_ps(SHADER_DIR "/ps_volume_velocity.wgsl", 1);` — build + run `-R shader_compile_tests`, expect **FAIL**.
- [ ] **Step 2: Port both** — literal, same skeleton as Task 6's shaders (both declare the ConfigBuffer prefix → full 336 B struct, `uses_group0=1`). Halocolor: finite-difference gradient of `tex_trace` (deposit at bindings (2)/(3)); velocity: single `tex_trace` at (0)/(1), sampling `.gba`/`.rgba` channels — compiles fine against `texture_3d<f32>` regardless of the bound texture's channel count (R32Float sampled reads return (v,0,0,1)). File-header comment on each: `// FILE-PARITY PORT (M4b adjudication): dead in REGIME_SDSS — VELOCITY_ANALYSIS / HALO_COLOR_ANALYSIS are #ifdef'd out (main.cpp:38-40); not wired, no draw-path tests.` Keep implicit-truncation sites explicit (`.x` etc.) with comments; QUIRK(single_stack_2x_compensation) on both tails.
- [ ] **Step 3: Build + all suites + commit.**

```bash
nice -n 19 cmake --build build -j 8 && ctest --test-dir build
git add shaders/ps_volume_halocolor.wgsl shaders/ps_volume_velocity.wgsl tests/shader_compile_tests.cpp
git commit -m "shaders: ps_volume_halocolor + ps_volume_velocity file-parity ports (not wired — dead in REGIME_SDSS)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: `cs_volpath.wgsl` + `cs_volpath_blit.wgsl` — the accumulator-buffer conversion (DESIGN §2.6)

This is the single largest task in the milestone (DESIGN risk #1). Shader text only — host plumbing lands in Task 11, the dispatch test in Task 9.

**Files:**
- Create: `shaders/cs_volpath.wgsl`, `shaders/cs_volpath_blit.wgsl`
- Test: `tests/shader_compile_tests.cpp` (two entries)

**Interfaces:**
- Produces: `cs_volpath.wgsl` — `@workgroup_size(10, 10, 1)`, `uses_group0 = true`, binding table (compute convention; pinned by m4a-carryovers.md and `graphics.cpp:1072-1080`):

  | resource | WGSL type | binding |
  |---|---|---|
  | `tex_accumulator` | `var<storage, read_write> array<vec4<f32>>` | 0 |
  | `tex_trace` | `texture_3d<f32>` | 1 |
  | `tex_deposit` | `texture_3d<f32>` | 2 |
  | `tex_palette_trace` | `texture_2d<f32>` | 3 |
  | `tex_palette_data` | `texture_2d<f32>` | 4 |
  | `tex_trace_sampler` | `sampler` | 17 |
  | `tex_deposit_sampler` | `sampler` | 18 |
  | `tex_palette_trace_sampler` | `sampler` | 19 |
  | `tex_palette_data_sampler` | `sampler` | 20 |

- Produces: `cs_volpath_blit.wgsl` — NEW infrastructure, no HLSL counterpart (same class as M3's atomic buffer), `uses_group0 = false`, accumulator buffer (read) @binding(0) + `display_tex` storage texture @binding(1).

- [ ] **Step 1: Failing tests first.** `shader_compile_tests.cpp`: `check_compiles(SHADER_DIR "/cs_volpath.wgsl", 1);` and `check_compiles(SHADER_DIR "/cs_volpath_blit.wgsl", 0);`. Build + `-R shader_compile_tests` — expect **FAIL**.

- [ ] **Step 2: Write `cs_volpath_blit.wgsl`** (exact content — small enough to spec inline):
  ```wgsl
  // cs_volpath_blit.wgsl — NEW SHADER, no HLSL counterpart (M4b design §2.6 option a).
  // Copies the vec4 PT accumulator buffer into display_tex each PT frame, mirroring
  // the cs_particles_transform -> cs_particles_blit two-dispatch shape. Exists because
  // WGSL has no texture_storage_2d<rgba32float, read_write> for cs_volpath's original
  // RWTexture2D<float4> read-modify-write (see cs_agents_propagate.wgsl:118's note).
  // Dispatched exactly (window_width, window_height, 1) with 1x1x1 groups, matching
  // cs_particles_blit's shape — gid is always in-bounds, no guard needed.
  // Row stride == textureDimensions(...).x == window_width == the buffer's row stride
  // (host keeps all three consistent; main.cpp recreates buffer + tex together on resize).

  @group(1) @binding(0) var<storage, read> tex_accumulator : array<vec4<f32>>;
  @group(1) @binding(1) var display_out : texture_storage_2d<rgba32float, write>;

  @compute @workgroup_size(1, 1, 1)
  fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
      let dims = textureDimensions(display_out);
      let idx = gid.y * dims.x + gid.x;
      textureStore(display_out, vec2<i32>(i32(gid.x), i32(gid.y)), tex_accumulator[idx]);
  }
  ```

- [ ] **Step 3: Write `cs_volpath.wgsl`** — full port of the 455-line HLSL per DESIGN §2.6, with these pinned decisions:
  - **Accumulator:** `var<storage, read_write> tex_accumulator : array<vec4<f32>>;` at `@group(1) @binding(0)`, row-major index `let pix_idx = pixel_xy.y * u32(cfg.screen_width) + pixel_xy.x;`. Plain non-atomic read-modify-write is exact here — each invocation owns one pixel.
  - **OOB guard, first statement of `main` (adjudication 1):**
    ```wgsl
    // QUIRK(oob_dispatch_guard): NO HLSL counterpart. Host dispatch rounds UP
    // (ceil) to cover partial 10x10 tiles on resizable windows; D3D11 never
    // launched these threads. WGSL storage-buffer OOB indexing CLAMPS into a
    // real slot instead of discarding, so without this guard every tail-band
    // invocation would corrupt a real pixel's accumulator each frame
    // (translation-notes T2 class). Adjudicated 2026-08-12.
    if (pixel_xy.x >= u32(cfg.screen_width) || pixel_xy.y >= u32(cfg.screen_height)) {
        return;
    }
    ```
  - **Feature flags hardcoded** to the shipping set (DESIGN §2.6 recommendation): `TEMPORAL_ACCUMULATION`, `RUSSIAN_ROULETTE`, `HALO_ILLUMINATION`, `TRACE_ILLUMINATION` active in the control flow; `GRADIENT_GUIDING` / `TRACE_SHARPENING` / `WHITESKY_ILLUMINATION` / `POINT_ILLUMINATION` bodies kept as clearly-marked comment blocks (`// #ifdef POINT_ILLUMINATION (disabled in default build) — kept for 1:1 correspondence:`), same treatment as M3's dead RNG.
  - **Constants verbatim** (truncated-PI convention): `const PI = 3.141592;`, `const PI2 = 6.283184;`, `const INV_PI4 = 0.079577;` (unused, keep), `RAY_EPSILON 1e-5`, `INTENSITY_EPSILON 1e-4`, `NUMERICAL_EPSILON 1e-4`. Do NOT substitute a more precise pi.
  - **RNG** (`:75-111`): WGSL has no member functions — `struct RNG { m_w : u32, m_z : u32 }` plus free functions `fn rng_set_seed(rng: ptr<function, RNG>, seed1: u32, seed2: u32)`, `fn rng_random_uint(rng: ptr<function, RNG>) -> u32`, `fn rng_random_float(rng: ptr<function, RNG>) -> f32`, `fn wang_hash(seed: u32) -> u32`. Constants `BAD_W = 0x464fffffu`, `BAD_Z = 0x9068ffffu`. `// QUIRK(set_seed_mw_recheck): the second guard re-checks m_w == 0 (HLSL :85 — likely meant m_z == 0). Preserved verbatim.` Drop `get_seed` only if truly uncalled — it IS uncalled; port it anyway as a comment or dead function for 1:1 correspondence (dead-code precedent, NOTES T11).
  - `// QUIRK(seed_idx_truncation):` HLSL `:351` assigns a `uint3` expression to `uint` — implicit `.x` truncation. Port as `let idx = threadIDInGroup.x + 100u * groupID.x;` (PT_GROUP_SIZE_X * PT_GROUP_SIZE_Y = 100), comment citing the HLSL line.
  - `// QUIRK(sky_scalar_truncation):` `get_sky_L` is declared `float` but returns `float3` (HLSL `:203-209`) — implicit truncation to `.x`. Port as `fn get_sky_L(rd: vec3<f32>) -> f32 { return 0.0; }` (WHITESKY off); call sites broadcast: `L + throughput * get_sky_L(rd)` works as scalar math onto vec3 via `vec3<f32>(...)` where needed, and `path_L = vec3<f32>(get_sky_L(rd));` at `:440`.
  - `ray_sphere_intersection` (`:126-144`) is dead code — port verbatim (drop only the stray `;` after its closing brace, which WGSL rejects; note it).
  - `ray_AABB_intersection`: `var t : array<f32, 8>;` — mechanical. Division by zero rd components yields inf identically in both APIs; no change, one-line comment.
  - do/while loops (`delta_tracking` `:219-222`, `occlusion_tracking` `:232-236` — the latter dead, POINT off, keep as comment or live dead function): WGSL `loop { <body>; continuing { break if !(<condition>); } }`.
  - All `SampleLevel(sampler, uv, 0)` → `textureSampleLevel(tex, samp, uv, 0.0)` (compute-stage safe).
  - `main` signature: `@compute @workgroup_size(10, 10, 1) fn main(@builtin(local_invocation_id) threadIDInGroup : vec3<u32>, @builtin(workgroup_id) groupID : vec3<u32>, @builtin(global_invocation_id) dispatchThreadId : vec3<u32>)`.
  - `pt_iteration == 0` zero-fill (`:354-355`) and the TEMPORAL_ACCUMULATION tail (`:447-451`) operate on `tex_accumulator[pix_idx]` — numerically identical to the UAV RMW.
  - Full 336 B struct with canonical names (`grid_x/grid_y/grid_z` in this HLSL = `world_width/world_height/world_depth` — same offsets, DESIGN §2 intro).
- Run: `-R shader_compile_tests` → expect **PASS** (both entries).

- [ ] **Step 4: Build + all suites + commit.**

```bash
nice -n 19 cmake --build build -j 8 && ctest --test-dir build
git add shaders/cs_volpath.wgsl shaders/cs_volpath_blit.wgsl tests/shader_compile_tests.cpp
git commit -m "shaders: cs_volpath.wgsl — vec4 buffer accumulator + ceil-dispatch OOB guard; new cs_volpath_blit.wgsl (m3 buffer+blit precedent)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: `cs_volpath` headless dispatch test (DESIGN §6 — highest-value new test)

**Files:**
- Test: `tests/render_path_tests.cpp` (new `test_volpath_dispatch`; small refactor of `readback_rgba32f` to also accept a raw `wgpu::Texture`)

**Interfaces:**
- Consumes: Task 8's shaders (loaded from `SHADER_DIR`), `get_structured_buffer`/`update_structured_buffer`/`capture_structured_buffer`, compute bind API incl. `set_texture_sampler_compute` (16+N path, already pinned by `test_compute_sampler_pairing`).
- Produces: end-to-end pin on the accumulator conversion, RMW math, ceil-dispatch full coverage at a non-multiple-of-10 size, the OOB guard, and the blit's row-major agreement.

Screen 25×15 (neither dimension a multiple of 10): dispatch `ceil(25/10)=3 × ceil(15/10)=2` = 600 invocations, 375 in-bounds, 225 guarded out. Buffer: `get_structured_buffer(4 * sizeof(float), 25 * 15)` (375 × 16 B).

- [ ] **Step 1: Write the test** (expected to PASS immediately if Task 8's port is correct — a FAIL here is a translation bug; use superpowers:systematic-debugging before touching expected values):
  - Config (`float cfg[84]`, indices per NOTES §1): identity matrices; `screen_width`(59)=25, `screen_height`(60)=15; `world_width/height/depth`(56-58)=8; trims(49-54)=0/1 per axis; `trim_density`(55)=0; `sample_weight`(61)=1; `galaxy_weight`(64)=0; `camera_x/y/z`(68-70)=(0,-4,0); `sigma_s`(72)=0 (forces the deterministic emission-absorption ray-march branch); `sigma_a`(73)=0.5; `sigma_e`(74)=1; `trace_max`(75)=1; `camera_offset_x/y`(76-77)=0; `exposure`(78)=1; `n_bounces`(79 int)=2; `ambient_trace`(80)=0; `compressive_accumulation`(81 int)=0; `guiding/anisotropy`(82-83)=0.
  - Resources: `tex_trace` + `tex_deposit` 8×8×8 R32_FLOAT; palettes 2×2 RGBA8_UNORM; samplers: slots 1/2 `get_texture_sampler(CLAMP, Filter::ANISOTROPIC)`, slots 3/4 default. Bind exactly like `main.cpp:1370-1385` will: buffer at compute slot 0, sampled textures 1-4, samplers 1-4; `set_constant_buffer` before each dispatch. Dispatch `run_compute(3, 2, 1)`. After each phase: `swap_frames()` (headless flush) then `capture_structured_buffer(&accum, out, 375, 4 * sizeof(float))`.
  - **Phase A — deterministic RMW + guard (pt_iteration = 5):** trace/deposit all ZERO, palettes all BLACK → every ray (hit or miss) yields `path_L = (0,0,0)`. Pre-fill the buffer with 375 × `(6,6,6,6)` via `update_structured_buffer`. Expected after one dispatch, for ALL 375 elements including the last: `6 * (5/6) + (0,0,0,1)/6` = **(5.0, 5.0, 5.0, 5.1666665)**, tolerance `1e-4`. Without the OOB guard, the 225 tail invocations clamp-write extra RMW applications into element 374 and drive it toward (0,0,0,1) — a ≥ 0.8 deviation.
  - **Phase B — zero-fill + full coverage (pt_iteration = 0):** same resources; expected ALL 375 elements exactly **(0, 0, 0, 1.0)** (zero-fill at `:354-355`, then `0*0/1 + vec4(path_L,1)/1`), tolerance `1e-6`. Alpha == 1.0 on every element pins that ceil-dispatch covered every pixel of the 25×15 grid (truncating dispatch would leave rows 10-14 / cols 20-24 at their Phase A values).
  - **Phase C — emission smoke (pt_iteration = 0):** re-upload trace = 2.0 everywhere; `tex_palette_trace` solid red (255,0,0,255); `tex_palette_data` black. Camera (0,-4,0) looks through the volume center. Assert center pixel index `7*25+12 = 187`: `r > 0.001`, `g < 1e-4`, `b < 1e-4`, `|a - 1.0| < 1e-4` (red-only emission through `get_emitted_trace_L`; HALO term contributes black-palette zero).
  - **Phase D — blit:** create a 25×15 RGBA32_FLOAT `Texture2D`; `set_compute_shader(blit)`, `set_structured_buffer(&accum, 0)`, `set_texture_compute(&display, 1)`, `run_compute(25, 15, 1)`, flush. Generalize `readback_rgba32f` to take `(wgpu::Texture, width, height, float *out)` (the existing RenderTarget call sites pass `rt->texture` — mechanical refactor). Assert all 375 texels equal the Phase C buffer contents, tolerance `1e-6` (pins the blit + row-major index agreement).
  - Unset ALL compute slots and samplers 0-4 afterwards (strict-match contract for later tests); release everything.
- [ ] **Step 2: Run.** `nice -n 19 cmake --build build -j 8 && ctest --test-dir build -R render_path_tests` — expect **PASS**. Then the full suite.
- [ ] **Step 3: Commit.**

```bash
ctest --test-dir build
git add tests/render_path_tests.cpp
git commit -m "tests: cs_volpath headless dispatch test — RMW accumulator, ceil-dispatch coverage at 25x15, OOB guard, blit agreement

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: `ps_volpath.wgsl` (DESIGN §2.7)

**Files:**
- Create: `shaders/ps_volpath.wgsl`
- Test: `tests/shader_compile_tests.cpp` (one entry)

**Interfaces:**
- Consumes: interstage contract of the already-shipped `shaders/vs_2d.wgsl` (`@location(0) texcoord_out: vec2<f32>` — no new vertex shader needed).
- Produces: `ps_volpath.wgsl` — entry `fn main`, `uses_group0 = true` (it DOES declare the cbuffer even though vs_2d doesn't — the reason for Task 1's fourth site). Bindings: `tex: texture_2d<f32>` @(0), `tex_sampler: sampler` @(1).

- [ ] **Step 1: Failing test first.** `check_compiles_ps(SHADER_DIR "/ps_volpath.wgsl", 1);` — build + `-R shader_compile_tests`, expect **FAIL**.
- [ ] **Step 2: Port.** Trivial 59-line HLSL: full 336 B struct with canonical names (adjudication 4: the HLSL's `int dummy2; int dummy3;` tail becomes `guiding_strength: f32, scattering_anisotropy: f32` — same offsets, unused; one-line comment citing DESIGN §2.7). `fn tonemap(L: vec3<f32>, exposure: f32) -> vec3<f32> { return vec3<f32>(1.0) - exp(-exposure * L); }`. Body: `let L = textureSample(tex, tex_sampler, input.texcoord_out).rgb; return vec4<f32>(select(tonemap(L, cfg.exposure), L, cfg.compressive_accumulation == 1), 1.0);` (`select` evaluates both arms — both are pure; comment noting the HLSL ternary equivalence).
- [ ] **Step 3: Build + all suites + commit.**

```bash
nice -n 19 cmake --build build -j 8 && ctest --test-dir build
git add shaders/ps_volpath.wgsl tests/shader_compile_tests.cpp
git commit -m "shaders: ps_volpath.wgsl port — pairs with existing vs_2d.wgsl, canonical cbuffer tail

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 11: VM_PATH_TRACING wiring — host plumbing, ceil dispatch, resize reset (DESIGN §5)

**Files:**
- Modify: `main.cpp` (shader loads, `pt_accum_buffer` lifecycle, PT bind block, dispatch shape, resize branch, shutdown releases)

**Interfaces:**
- Consumes: Tasks 1, 2, 8, 9, 10.
- Produces: a live VM_PATH_TRACING mode. `StructuredBuffer pt_accum_buffer` (16 B stride × `window_width*window_height`) is the accumulator; `display_tex` becomes blit-output + `ps_volpath` sample source, unchanged type.

- [ ] **Step 1: Loads.** `main.cpp:521`: `ComputeShader cs_volpath = load_compute(SHADER_ROOT "/cs_volpath.wgsl");` and on the same declaration block add `ComputeShader cs_volpath_blit = load_compute(SHADER_ROOT "/cs_volpath_blit.wgsl");`. `:524`: `ps_volpath = load_ps(SHADER_ROOT "/ps_volpath.wgsl")`. Update the `= {}` stub comment at `:519-520` to note only `sort_shader` (upstream default-off) and the two file-parity PS ports remain unwired.

- [ ] **Step 2: Buffer lifecycle.** After `display_accum_buffer`'s creation (`main.cpp:656`):
  ```cpp
  // PT accumulator: vec4<f32> per pixel. Replaces the D3D11 RWTexture2D<float4>
  // read-modify-write on display_tex, which WGSL storage textures can't express
  // (read_write is r32*-only) — design §2.6 option (a), M3 buffer+blit precedent.
  StructuredBuffer pt_accum_buffer = graphics::get_structured_buffer(4 * sizeof(float), window_width * window_height);
  ```
  Shutdown: `graphics::release(&pt_accum_buffer);` next to `release(&display_accum_buffer)` (`main.cpp:1805`).

- [ ] **Step 3: Resize branch.** In the resize block, after the `display_accum_buffer` recreation (`main.cpp:892-894`):
  ```cpp
  graphics::release(&pt_accum_buffer);
  pt_accum_buffer = graphics::get_structured_buffer(sizeof(float) * 4, window_width * window_height);
  assert(graphics::is_ready(&pt_accum_buffer));

  // m4a-carryovers #1: resize during PT accumulation recreates the accumulator
  // while pt_iteration stays nonzero — restart accumulation instead of
  // averaging against a fresh (zeroed) buffer at a stale iteration count.
  reset_pt = true;
  ```

- [ ] **Step 4: PT bind block rewrite** (`main.cpp:1370-1397`):
  - Replace `graphics::set_texture_compute(&display_tex, 0);` with `graphics::set_structured_buffer(&pt_accum_buffer, 0);` and the matching `graphics::unset_texture_compute(0);` with `graphics::unset_structured_buffer(0);`. Keep sampled slots 1-4 + samplers exactly as-is (including the QUIRK-commented `:1280`-typo fix and the slot-4 unset from a098681).
  - Dispatch (adjudication 1, replaces the truncating division at `:1386-1389`):
    ```cpp
    // QUIRK(pt_ceil_dispatch): upstream truncates (screen/10) — bit-identical at
    // upstream's fixed window sizes (exact multiples of 10). Resizable windows are
    // this port's own extension; round UP so no dead pixel band, with the matching
    // OOB guard inside cs_volpath.wgsl. Adjudicated 2026-08-12.
    graphics::run_compute(
        (int(rendering_config.screen_width) + int(PT_GROUP_SIZE_X) - 1) / int(PT_GROUP_SIZE_X),
        (int(rendering_config.screen_height) + int(PT_GROUP_SIZE_Y) - 1) / int(PT_GROUP_SIZE_Y),
        1);
    ```
  - Immediately after the unsets (still inside the `run_pt && ...` branch, before `pt_iteration++`), the blit dispatch:
    ```cpp
    // Blit accumulator -> display_tex for ps_volpath to sample (design §2.6).
    graphics::set_compute_shader(&cs_volpath_blit);
    graphics::set_structured_buffer(&pt_accum_buffer, 0);
    graphics::set_texture_compute(&display_tex, 1);
    graphics::run_compute(window_width, window_height, 1);
    graphics::unset_structured_buffer(0);
    graphics::unset_texture_compute(1);
    ```
    (When `run_pt` is false, `display_tex` keeps its last blitted contents and the quad draw shows the frozen image — same visible behavior as upstream, where the accumulator WAS `display_tex`.)

- [ ] **Step 5: Build + all suites.** `nice -n 19 cmake --build build -j 8 && ctest --test-dir build` — all 7 green (energy_smoke startup now compiles all nine new WGSL files).

- [ ] **Step 6: Windowed smoke.** Same recipe as Task 5 Step 3: startup logs show `cs_volpath.wgsl` / `cs_volpath_blit.wgsl` / `ps_volpath.wgsl` compiled; no Dawn errors over ~30 s (PT itself needs a mode switch — human gate). Kill; `pgrep polyphorm` empty.

- [ ] **Step 7: Commit.**

```bash
git add main.cpp
git commit -m "m4b: VM_PATH_TRACING wired — pt_accum_buffer plumbing, blit dispatch, ceil dispatch with guard, reset_pt on resize

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 12: Bookkeeping — QUIRK sweep, carryover ledgers

**Files:**
- Modify: `docs/superpowers/research/m3/m3-carryovers.md`, `docs/superpowers/research/m4/m4a-carryovers.md`
- Create: `docs/superpowers/research/m4/m4b-carryovers.md`

- [ ] **Step 1: QUIRK sweep.** `grep -n "QUIRK(" shaders/*.wgsl main.cpp cpplib/graphics.cpp` and verify every preserved quirk from this plan is marked: `single_stack_2x_compensation` (×5 ps_volume files), `overdensity_constant_remap`, `seed_idx_truncation`, `sky_scalar_truncation`, `set_seed_mw_recheck`, `oob_dispatch_guard`, `pt_ceil_dispatch`. Add any that a task missed (fix in the shader/main.cpp, rerun `ctest --test-dir build -R shader_compile_tests`).
- [ ] **Step 2: Close ledger items.** m3-carryovers.md: mark #2 (per-draw set_constant_buffer — note the fourth site), #4 (palette TGA + CWD), #5 (28 B stride coverage) closed with commit refs; #3 (blend alpha) stays open pending a D3D11 reference capture. m4a-carryovers.md: mark must-handle #1 (reset_pt on resize) and #2 (PT dispatch truncation) closed; #3 (HUD fixed anchors) explicitly NOT done — still open.
- [ ] **Step 3: Write `m4b-carryovers.md`** — the still-open items a future milestone inherits: blend-alpha formula unverified (screenshot-compare finding channel, spec says M5 reads the cube not pixels); palette `RGBA8_UNORM` vs `_SRGB` pending gate verdict; HUD/histogram fixed `SCREEN_X/Y` anchors (m4a #3); DPI-only resize miss (m4a #4); `get_panel_rect` latent assert (m4a #5); `ps_volume_halocolor`/`ps_volume_velocity` ported-not-wired (need `#define` flips out of scope); post-validation cleanup ticket list for every QUIRK from Step 1 (per the top-level spec's convention); `save_texture3D` still stubbed (M5-critical). Plus the human-gate outcome placeholder to be filled by the coordinator after Task 13.
- [ ] **Step 4: Full suite + commit.**

```bash
ctest --test-dir build
git add docs/superpowers/research/m3/m3-carryovers.md docs/superpowers/research/m4/m4a-carryovers.md docs/superpowers/research/m4/m4b-carryovers.md shaders/ main.cpp cpplib/graphics.cpp
git commit -m "docs: m4b bookkeeping — quirk register, carryover ledgers updated, m4b-carryovers opened

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 13 (human gate): visual verification

Coordinator asks the human to run windowed from the synthetic-dataset dir (same recipe as the M3/M4a gates) and walk this checklist. Reminder: exit with Esc; verify no leftover process afterwards.

1. **Volume trace view:** toggle VIS: TRACE (panel). Filamentary structure renders over the dark background — the brief's "teal filaments" reference look; coloring follows the `palette_sunset3` ramp. Camera orbit (LMB drag), scroll zoom, RMB pan all work with the volume view tracking. The trim gizmo (bottom-right) + X/Y/Z POS/WIDTH sliders interactively cull the volume box — culled regions vanish cleanly at the trim planes, and the slab stack re-picks its axis as you orbit past 45° boundaries (no popping artifacts beyond upstream's known single-frame re-pick).
2. **Palette screenshot-compare:** capture VM_VOLUME and (later) VM_PATH_TRACING stills; compare against upstream reference renders (README imagery / any available D3D11 captures). **Palette hue/brightness deviations are gate FINDINGS to record in m4b-carryovers.md (the RGBA8_UNORM-vs-SRGB hypothesis), not automatic failures** — adjudication 3.
3. **Highlights / Overdensity:** VIS: HIGHLIGHTS shows the purple/orange two-field render with a green density band that moves as HGLGHT DENSITY slides; VIS: OVERDENSITY shows the blue/green/red three-bucket render responding to the LO/HI threshold sliders.
4. **Path tracing:** VIS: PATH TRACING — image starts noisy and visibly converges/denoises over a few seconds while the camera is still (accumulation working). Orbiting the camera resets accumulation (re-noise, re-converge). SIGMA/EXPOSURE-class sliders in the PT panel section reset accumulation when dragged. F4 turntable mode keeps it perpetually noisy (reset every frame) — expected.
5. **Resize during PT:** while PT is accumulating, resize the window (drag a corner). The image must reset cleanly and re-converge at the new size — NO garbage frame, no stale stretched image, no right/bottom dead band at non-multiple-of-10 sizes (pins reset_pt + ceil dispatch). Repeat once more mid-accumulation.
6. **Blend-alpha compare:** volume slab transparency/contrast vs upstream reference imagery — the alpha-channel blend formula is a documented guess (`graphics.cpp:719-727`). Deviation = recorded finding (does NOT gate M5; validation reads the exported cube, not pixels).
7. **Round-trips:** cycle VIS: PARTICLES → TRACE → HIGHLIGHTS → OVERDENSITY → PATH TRACING → PARTICLES twice — every mode renders after every transition (no stale-binding fatals; pins the strict-match unset discipline). F1 hides/shows the panel; F8 (trace reset) blanks the volume until the sim re-deposits; F2 full reset works from every mode.
8. **Exit:** Esc exits clean; `pgrep polyphorm` returns nothing.

PASS = items 1, 3, 4, 5, 7, 8 clean; items 2 and 6 produce recorded findings (any verdict). On FAIL: use superpowers:systematic-debugging, fix, re-run the gate.

---

## Self-review (performed before finalizing this plan)

- **DESIGN coverage:** §1 inventory → all 8 HLSL files have a port task (3, 4, 6, 7, 8, 10) + the new blit (8); §2.1-§2.7 each mapped to a task with the pinned decisions inlined; §3 → Task 3; §4 → Task 2; §5 → Tasks 1/5/11 (all four cbuffer sites, gates untouched, reset_pt, ceil dispatch); §6 → compile-suite entries in every shader task, 28 B draw test, volume readback test, dispatch test (Task 9), blend-alpha deliberately NOT test-pinned (ships as documented guess per §6); §7 followed with three merges noted in the header of this section; §8 risks 1/2/3 directly mitigated by Tasks 8/9/1.
- **Adjudications:** all five appear in Global Constraints and land in Tasks 11+8 (ceil+guard), 8+11 (buffer+blit), 2+13 (RGBA8 + gate flag), 10 (canonical tail), 7 (file-parity only). reset_pt lands with the PT port (Task 11), 28 B test with the vs_3d task (Task 3), four cbuffer binds in Task 1, HUD fix excluded (Constraints + Task 12).
- **Consistency:** binding numbers cross-checked against `graphics.cpp:821-838`/`:1072-1080` and `cs_volpath.hlsl:22-30`; `pt_accum_buffer` stride 16 B consistent across Tasks 9/11 and the blit's `array<vec4<f32>>`; config float-index map (48+i) consistent across Tasks 3/4/9; expected values hand-derived from the actual `bin/data/palette_hot.tga` bytes and closed-form RMW arithmetic.
- **Placeholder scan:** no TBD/TODO/"similar to Task N"; every step names exact files, lines, values, and commands.
