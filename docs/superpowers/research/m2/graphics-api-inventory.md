# Polyphorm graphics:: / file_system:: API inventory (for WebGPU port)

Sources read: `cpplib/graphics.h` (448 lines), `cpplib/graphics.cpp` (1426 lines),
`cpplib/file_system.h` (23 lines), `cpplib/file_system.cpp` (89 lines), `main.cpp` (1670 lines).

Branch: macos-webgpu-port. This file is pure research output — no repo files were modified.

---

## 1. Every `graphics::`/`file_system::` function main.cpp actually calls

Legend: call-site line numbers are exact lines in current `main.cpp`. "D3D11 internals" summarizes
`graphics.cpp`. Overload note added where main.cpp's usage picks one overload out of several declared
in `graphics.h`.

### Init / lifecycle

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::init` | `bool init(LUID *adapter_luid = NULL)` | 438 | Creates `IDXGIFactory`, optionally selects adapter by LUID, calls `D3D11CreateDevice` (feature level 11.0, `D3D11_CREATE_DEVICE_SINGLETHREADED` [+`_DEBUG` in debug]), creates 2 blend states (OPAQUE=zeroed desc, ALPHA=src-alpha/inv-src-alpha) and 2 rasterizer states (SOLID: fill solid/cull none, WIREFRAME: fill wireframe/cull back, both `FrontCounterClockwise=TRUE` i.e. RH winding), sets initial raster state to SOLID. |
| `graphics::init_swap_chain` | `bool init_swap_chain(Window *window)` | 439 | Builds `DXGI_SWAP_CHAIN_DESC` (R8G8B8A8_UNORM, 2 buffers, `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`, `ALLOW_MODE_SWITCH`), creates `IDXGIFactory`, calls `CreateSwapChain`. |
| `graphics::swap_frames` | `void swap_frames()` | 1626 | `swap_chain->Present(1, 0)` — vsync on. |
| `graphics::release` (no-arg global) | `void release()` | 1667 | Releases swap chain, D3D context, D3D device, both blend states, both rasterizer states. Must be called last. |
| `graphics::release(T*)` | 14 overloads (see §3 for full overload set; used ones listed inline below) | 1630-1666 (38 calls) | Each is a thin `RELEASE_DX_RESOURCE` (COM `->Release()` + null) over the object's D3D members — see §2 for exact fields per type. |
| `graphics::show_live_objects` | never called | — | (listed for completeness — not called by main.cpp) |

### Render targets / depth / viewport

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::get_render_target_window` | `RenderTarget get_render_target_window()` | 446 | Gets swap chain back buffer 0 as `ID3D11Texture2D`, creates RTV with format `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` (note: **sRGB view over a UNORM swap chain** — gamma quirk to preserve). `sr_view` left NULL — this RenderTarget cannot be bound as a shader texture. width/height read from swap chain desc. |
| `graphics::get_depth_buffer` | `DepthBuffer get_depth_buffer(uint32_t width, uint32_t height)` | 448 | Creates `ID3D11Texture2D` typeless `R24G8_TYPELESS`, DSV as `D24_UNORM_S8_UINT`, SRV as `R24_UNORM_X8_TYPELESS` (so depth can also be sampled). Bind flags: `SHADER_RESOURCE | DEPTH_STENCIL`. |
| `graphics::clear_render_target` | `void clear_render_target(RenderTarget *buffer, float r,g,b,a)` | 1163 | `ClearRenderTargetView`. |
| `graphics::set_render_targets_viewport` | 3 overloads; main.cpp uses `(RenderTarget*)` only | 450, 1162, 1317, 1457 | `set_viewport(buffer)` (D3D11_VIEWPORT sized to buffer w/h, MaxDepth=1) then `set_render_targets(buffer)` (`OMSetRenderTargets(1,&rt_view,NULL)`, no depth bound — **note: even the 450 call site with `depth_buffer` in scope only calls the single-arg overload; the depth buffer is created but never actually bound via `set_render_targets_viewport(rt, depth)`**). |

Never used: `get_render_target(w,h,format)`, `clear_depth_buffer`, plain `set_render_targets(...)` (all 3 overloads), plain `set_viewport(...)` (all 3 overloads), `set_render_targets_viewport(RenderTarget*, DepthBuffer*)`, `set_render_targets_viewport(RenderTarget*, count, DepthBuffer*)`.

### Textures

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::get_texture2D` | `Texture2D get_texture2D(void*, u32 w, u32 h, DXGI_FORMAT, u32 pixel_byte_count=4)` | 576 (`display_tex`, RGBA32F, data=NULL), 577 (`display_tex_uint`, R32_UINT, data=NULL) | `CreateTexture2D` (Usage DEFAULT, BindFlags `SHADER_RESOURCE\|UNORDERED_ACCESS`, mip=1, array=1), then SRV (Texture2D, 1 mip) and UAV (Texture2D, mip 0). If `data==NULL`, subresource ptr is NULL — texture created uninitialized (must be cleared before first read; main.cpp does this via raw `ClearUnorderedAccessView*` calls, see §6/quirks). |
| `graphics::get_texture3D` | `Texture3D get_texture3D(void*, u32 w,h,depth, DXGI_FORMAT, u32 pixel_byte_count=4)` | 565/566 (HALO_COLOR_ANALYSIS: RG16F) or 568/569 (else: R16F) for `trail_tex_A/B`; 572 (VELOCITY_ANALYSIS: RGBA16F) or 574 (else: R16F) for `trace_tex` | `CreateTexture3D` (Usage DEFAULT, BindFlags `SHADER_RESOURCE\|UNORDERED_ACCESS`, mip=1), SRV (Texture3D), UAV (Texture3D, WSize=depth). data=NULL in all main.cpp call sites → uninitialized GPU memory. |
| `graphics::load_texture2D` | `Texture2D load_texture2D(std::string filename)` | 578 (`palette_trace_tex`), 579 (`palette_data_tex`) | Loads a **TGA** file via DirectXTex (`LoadFromTGAFile` + `CreateTexture`), then builds an SRV. No UAV created (ua_view stays null) — these are read-only palette LUTs. |
| `graphics::save_texture3D` | `void save_texture3D(Texture3D*, std::string filename)` | 1076/1078 (`trail_tex_A` or `trail_tex_B` depending on `is_a`, → `"export/deposit"`), 1079 (`trace_tex` → `"export/trace"`) | `DirectX::CaptureTexture` (GPU readback via staging texture, internal to DirectXTex) then `SaveToDDSFile` (`<name>.dds`) **and** raw binary dump of `image.GetPixels()` to `<name>.bin`. |
| `graphics::save_texture2D_HDR` | `void save_texture2D_HDR(Texture2D*, std::string filename)` | 1448 (`display_tex` → `"capture\\frame<N>"`) | `DirectX::CaptureTexture` + `SaveToHDRFile` → `<name>.hdr`. |
| `graphics::capture_current_frame` | `uint32_t capture_current_frame()` | 1445, 1452 | Gets swap chain buffer 0, creates a matching `ID3D11Texture2D`, `CopyResource` from back buffer into it, calls `save_texture2D` (TGA) internally, releases the temp texture, returns a running frame counter used to name `capture\frameN`. Writes to a hardcoded `capture\` dir (backslash — Windows path). |

Never used: `save_texture2D` directly (only indirectly via `capture_current_frame`).

### Texture binding to shader stages

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::set_texture` | 4 overloads: `(RenderTarget*,slot)`, `(DepthBuffer*,slot)`, `(Texture2D*,slot)`, `(Texture3D*,slot)` — main.cpp uses Texture2D & Texture3D forms | 1189 (display_tex@0), 1200 (trace_tex@0), 1204 (palette_trace_tex@1), 1210/1212 (trail_tex_A/B@1), 1218/1220 (dup, VOLUME_HALOCOLOR branch), 1289 (display_tex@0) | `PSSetShaderResources(slot,1,&sr_view)` — pixel-stage SRV bind only. |
| `graphics::set_texture_compute` | `(Texture2D*,slot)`, `(Texture3D*,slot)` | 991/993 (trail_tex_A/B@0), 995 (trace_tex@1), 1031/1032/1034/1035 (trail A/B@0,1 swapped by `is_a`), 1037 (trace_tex@2), 1056 (trace_tex@0), 1171 (display_tex_uint@0), 1181/1182 (display_tex_uint@0, display_tex@1), 1263 (display_tex@0) | `CSSetUnorderedAccessViews(slot,1,&ua_view,&init_counts=0)` — binds as **UAV** on compute stage (read/write), not SRV. |
| `graphics::set_texture_sampled_compute` | `(Texture2D*,slot)`, `(Texture3D*,slot)` | 1264 (trace_tex@1), 1267/1269 (trail A/B@2), 1272 (palette_trace_tex@3), 1274 (palette_data_tex@4) | `CSSetShaderResources(slot,1,&sr_view)` — binds as **SRV** (read-only) on compute stage, distinct register space from UAVs in D3D11 (`t` vs `u` registers). |
| `graphics::unset_texture` | `(slot)` | 1192, 1252, 1253, 1292 | `PSSetShaderResources(slot,1,{NULL})`. |
| `graphics::unset_texture_compute` | `(slot)` | 1004, 1005 (0,1), 1040-1042 (0,1,2), 1067 (0), 1178 (0), 1184/1185 (0,1), 1280 (0) | `CSSetUnorderedAccessViews(slot,1,{NULL},&init_counts=0)`. |
| `graphics::unset_texture_sampled_compute` | `(slot)` | 1281, 1282, 1283 (slots 1,2,3) | `CSSetShaderResources(slot,1,{NULL})`. |

Never used: `set_texture(RenderTarget*,slot)`, `set_texture(DepthBuffer*,slot)`.

### Samplers

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::get_texture_sampler` | `TextureSampler get_texture_sampler(SampleMode mode=CLAMP, D3D11_FILTER filter=MIN_MAG_MIP_POINT)` | 581 (`tex_sampler_trace`, CLAMP+ANISOTROPIC), 582 (`tex_sampler_deposit`, CLAMP+ANISOTROPIC), 583 (`tex_sampler_display`, defaults=CLAMP+POINT), 584 (`tex_sampler_color_palette`, defaults) | `CreateSamplerState`; `AddressU/V/W` all set to same mode via lookup table `{CLAMP,WRAP,BORDER}`; `ComparisonFunc=NEVER`; Min/MaxLOD = ±FLOAT32_MAX. |
| `graphics::set_texture_sampler` | `(TextureSampler*, slot)` | 1190/1290 (tex_sampler_display@0), 1201 (tex_sampler_trace@0), 1205 (tex_sampler_color_palette@1), 1213/1221 (tex_sampler_deposit@1), 1271 (tex_sampler_deposit@2 — **note: this is the plain PS-stage setter called inside the compute/path-tracing block, likely a bug/quirk — see §6**) | `PSSetSamplers(slot,1,&sampler)`. |
| `graphics::set_texture_sampler_compute` | `(TextureSampler*, slot)` | 1265 (tex_sampler_trace@1), 1273 (tex_sampler_color_palette@3), 1275 (tex_sampler_color_palette@4) | `CSSetSamplers(slot,1,&sampler)`. |

Never used: `get_blend_state` is unrelated; no sampler functions unused beyond what's listed — all declared sampler functions ARE used.

### Blend / rasterizer state

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::set_blend_state` | `void set_blend_state(BlendType type)` | 586 (`ALPHA`, set once at startup, never toggled again) | `OMSetBlendState(blend_states[type], blend_factor={0,0,0,0}, mask=0xffffffff)`. |

Never used: `get_blend_state`, `set_rasterizer_state`, `get_rasterizer_state` — main.cpp never switches raster state, stays on the SOLID state set by `init()`.

### Mesh / drawing

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::get_mesh` | `Mesh get_mesh(void* v, u32 vcount, u32 vstride, void* i, u32 icount, u32 idx_byte_size, D3D11_PRIMITIVE_TOPOLOGY topology=TRIANGLELIST)` | 724 (`super_quad_mesh`: `GRID_RESOLUTION` stacked quads, stride 7 floats, no indices), 725 (`quad_mesh`: single fullscreen quad, stride 6 floats, no indices) | `CreateBuffer` IMMUTABLE vertex buffer (`BIND_VERTEX_BUFFER`); index buffer only created `if(indices && index_count>0)` (both call sites pass NULL/0 → **no index buffer, index_format defaults uninitialized but unused, mesh.index_buffer stays NULL**). Topology defaults to TRIANGLELIST (both use default topology). |
| `graphics::draw_mesh` | `void draw_mesh(Mesh*)` | 1191 (quad_mesh, particles path), 1237/1243/1249 (super_quad_mesh, volume path — one of 3 mutually-exclusive branches per frame based on dominant camera axis), 1291 (quad_mesh, path-tracing display) | `IASetVertexBuffers`, `IASetPrimitiveTopology`; since `index_buffer==NULL` for all main.cpp meshes, always takes the `Draw(vertex_count,0)` path (non-indexed) — `DrawIndexed` path exists but is dead code from main.cpp's perspective. |

### Constant buffers

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::get_constant_buffer` | `ConstantBuffer get_constant_buffer(u32 size)` | 786 (`rendering_settings_buffer`, sizeof(RenderingConfig)), 807 (`config_buffer`, sizeof(SimulationConfig)), 818 (`statistics_config_buffer`, sizeof(StatisticsConfig)) | `CreateBuffer`: Usage `DYNAMIC`, BindFlags `CONSTANT_BUFFER`, CPUAccessFlags `WRITE`. No initial data. |
| `graphics::update_constant_buffer` | `void update_constant_buffer(ConstantBuffer*, void* data)` | 787, 982, 1023, 1052, 1168, 1236, 1242, 1248, 1260 | `Map(D3D11_MAP_WRITE_DISCARD)` → `memcpy(mapped.pData, data, buffer->size)` → `Unmap`. Whole-buffer overwrite every call (no partial updates). |
| `graphics::set_constant_buffer` | `void set_constant_buffer(ConstantBuffer*, u32 slot)` | 788 (rendering_settings_buffer@4, set once), 983 (config_buffer@0, every frame), 1055 (statistics_config_buffer@0, inside histogram block — **same slot 0 as config_buffer, different shader stage active at the time**) | Binds to **all 4 stages simultaneously**: `PSSetConstantBuffers`, `GSSetConstantBuffers`, `VSSetConstantBuffers`, `CSSetConstantBuffers` at the same slot. This is a broad-bind quirk — WebGPU bind groups would need one binding shared across pipelines/stages rather than 4 separate calls. |

### Structured buffers

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::get_structured_buffer` | `StructuredBuffer get_structured_buffer(int element_stride, int num_elements)` | 670/672/674/676/678/680 (particle SoA arrays x,y,z,phi,theta,weights — `sizeof(float) * NUM_PARTICLES` each), 682 (`density_histogram_buffer`, `sizeof(uint)*N_HISTOGRAM_BINS`), 684 (`halos_densities_buffer`, `sizeof(float)*data_count`) | `CreateBuffer`: Usage DEFAULT, BindFlags `UNORDERED_ACCESS` (SRV bind commented out — **structured buffers are UAV-only, never bound as SRV/`t`-register in this codebase**), CPUAccessFlags `READ\|WRITE` (unusual for DEFAULT usage but set anyway — **note this is actually illegal per strict D3D11 usage rules; DEFAULT usage normally disallows CPU access flags — likely tolerated by driver but a quirk to flag**), MiscFlags `BUFFER_STRUCTURED`, `StructureByteStride=element_stride`. UAV created with `Buffer.NumElements=num_elements`, format UNKNOWN (raw structured). |
| `graphics::update_structured_buffer` | `void update_structured_buffer(StructuredBuffer*, void* data)` | 671/673/675/677/679/681/683/685 (initial upload), 917-922 (F2 reset), 1051 (histogram bins zeroed each frame if `compute_histogram`) | `UpdateSubresource(buffer, 0, NULL, data, 0, 0)` — full-buffer CPU→GPU upload, no partial-region support used. |
| `graphics::set_structured_buffer` | `void set_structured_buffer(StructuredBuffer*, u32 slot)` | 996-1001 (agent propagate: x@2,y@3,z@4,phi@5,theta@6,weights@7), 1012-1017 (sort shader: same slots 2-7), 1057-1062 (histogram: histogram_buf@1, x@2,y@3,z@4,weights@5,halos_densities@6), 1170/1172-1174 (particle transform: theta@6, x@2,y@3,z@4) | `CSSetUnorderedAccessViews(slot,1,&ua_view,&init_counts=0)` — **compute-stage only**, no PS/VS equivalent exists in the API. |
| `graphics::capture_structured_buffer` | `void capture_structured_buffer(StructuredBuffer*, void* mapped_data, u32 num_elements, size_t element_size)` | 1101 (halos_densities_buffer, after simulation store), 1140-1143 (particle x/y/z/weights, agent capture), 1300 (density_histogram_buffer, every frame if `compute_histogram`) | `Map(D3D11_MAP_READ)` directly on the **default-usage** GPU buffer (no staging-texture copy step — relies on `CPUAccessFlags=READ` set at buffer creation, see quirk above) → `memcpy` out → `Unmap`. **This is the key readback quirk: normally D3D11 requires a STAGING resource + CopyResource for GPU→CPU readback; this buffer instead sets CPU access flags directly on a DEFAULT/UAV buffer and Maps it in place** — a pattern with no direct WebGPU equivalent (WebGPU requires a separate `MAP_READ` buffer + `CopyBufferToBuffer` + async `mapAsync`). |

### Shaders (compile + create, high-level helpers only — main.cpp never calls the low-level `compile_*`/`get_*_shader` primitives)

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::get_vertex_shader_from_code` | `VertexShader get_vertex_shader_from_code(char* code, u32 len)` | 468 (`vertex_shader` ← `vs_3d.hlsl`), 531 (`vertex_shader_2d` ← `vs_2d.hlsl`) | Compiles via `D3DCompile(..., "main", "vs_5_0", ...)`, then parses source text for a `struct VertexInput { ... };` block via a hand-rolled state-machine tokenizer (`get_vertex_input_desc_from_shader`) supporting only `float4/float2/float3/int4` typed fields, extracts up to `MAX_SEMANTIC_NAME_LENGTH=10`-char semantic names, builds `D3D11_INPUT_ELEMENT_DESC[]` (offset = `D3D11_APPEND_ALIGNED_ELEMENT`), creates `ID3D11VertexShader` + `ID3D11InputLayout`. **Note line 1399: `get_vertex_shader(&compiled, descs, 2)` hardcodes vertex_input_count=2 instead of using the parsed count** — a real bug/quirk (only reads first 2 parsed inputs regardless of how many the shader source actually declares). |
| `graphics::get_pixel_shader_from_code` | `PixelShader get_pixel_shader_from_code(char* code, u32 len)` | 475, 482, 489, 496, 503 (ps_volume_trace/highlight/halocolor/overdensity/velocity), 538 (ps_particles_color), 558 (ps_volpath) | `D3DCompile(..., "ps_5_0")` → `CreatePixelShader`. |
| `graphics::get_compute_shader_from_code` | `ComputeShader get_compute_shader_from_code(char* code, u32 len)` | 454 (cs_particles_transform), 461 (cs_particles_blit), 510 (cs_agents_propagate), 517 (cs_agents_sort), 524 (cs_field_decay), 545 (cs_density_histo), 552 (cs_volpath) | `D3DCompile(..., "cs_5_0")` → `CreateComputeShader`. |
| `graphics::set_vertex_shader` | `void set_vertex_shader(VertexShader*)` | 1187 (vertex_shader_2d), 1199 (vertex_shader), 1287 (vertex_shader_2d) | `IASetInputLayout` + `VSSetShader`. |
| `graphics::set_pixel_shader` | 2 overloads: `()` unbind, `(PixelShader*)` bind — main.cpp only ever uses the bind form | 1188, 1203, 1208, 1216, 1224, 1227, 1288 | `PSSetShader(shader, NULL, 0)`. |
| `graphics::set_compute_shader` | 2 overloads: `()` unbind, `(ComputeShader*)` bind — main.cpp only uses bind form | 989, 1011, 1029, 1054, 1169, 1180, 1262 | `CSSetShader(shader, NULL, 0)`. |

Never used at all: `compile_vertex_shader`, `compile_pixel_shader`, `compile_geometry_shader`, `compile_compute_shader` (low-level), `get_vertex_shader` (both overloads, low-level), `get_pixel_shader` (both overloads, low-level), `get_compute_shader` (both overloads, low-level), `set_pixel_shader()` unbind form, `set_compute_shader()` unbind form, **the entire GeometryShader family**: `compile_geometry_shader`, `get_geometry_shader` (both overloads), `set_geometry_shader` (both overloads), `graphics::release(GeometryShader*)`.

### Compute dispatch

| Function | Signature | Call sites | D3D11 internals |
|---|---|---|---|
| `graphics::run_compute` | `void run_compute(int gx, int gy, int gz)` | 1003, 1021, 1038, 1065, 1177, 1183, 1276 — see full dispatch table in §5 | `context->Dispatch(gx, gy, gz)` — direct pass-through, no validation. |

### is_ready / readiness checks

`graphics::is_ready` — 11 overloads declared (`Texture2D*, Texture3D*, RenderTarget*, DepthBuffer*, Mesh*, ConstantBuffer*, TextureSampler*, VertexShader*, PixelShader*, ComputeShader*, CompiledShader*`). main.cpp calls it 18 times as an `assert()` guard immediately after every resource-creation call (lines 447, 449, 456, 463, 470, 477, 484, 491, 498, 505, 512, 519, 526, 533, 540, 547, 554, 560) — covers RenderTarget, DepthBuffer, and every ComputeShader/VertexShader/PixelShader created via `*_from_code`. **Never called** on Texture2D/Texture3D/Mesh/ConstantBuffer/TextureSampler/StructuredBuffer instances (no readiness check after texture, mesh, constant-buffer, sampler, or structured-buffer creation) and `is_ready(CompiledShader*)` is never called directly (only used internally inside the `*_from_code` helpers via `assert`).

### file_system

| Function | Signature | Call sites | Internals |
|---|---|---|---|
| `file_system::read_file` | `File read_file(const char* path)` | 388 (binary dataset `.bin`), 453,460,467,474,481,488,495,502,509,516,523,530(x2 reused var),537(x2 reused var),544,551,557 — 17 total, one per shader source file + the dataset | `CreateFileA` (GENERIC_READ) → `GetFileAttributesExA` for size → `HeapAlloc(HEAP_ZERO_MEMORY)` → `ReadFile` → returns `{data,size}`. Windows-only API (`windows.h`, `HANDLE`, `HeapAlloc`) — **entire file needs a POSIX/std::ifstream replacement for macOS**, independent of the graphics port. |
| `file_system::release_file` | `void release_file(File)` | 455,462,469,476,483,490,497,504,511,518,525,532,539,546,553,559 — 16 sites (all shader files freed immediately after shader compile; the dataset `raw_data` from line 388 is read but its `File` handle/`.data` pointer is used for the lifetime of the program via `input_data` and is **never released** — a static/permanent allocation by design) | `HeapFree`. |
| `file_system::write_file` | never called by main.cpp | — | (declared, unused) |

---

## 2. Graphics types main.cpp holds, and which fields it touches directly

All types are held by value as local variables in `main()`. main.cpp overwhelmingly interacts through the `graphics::` functions (passing `&var`), which internally read/write the struct fields. Direct field access **from main.cpp itself** is minimal — found via `grep` for `.ua_view`, `.sr_view`, `.rt_view`, `.ds_view`, `.texture`, `.width`, `.height`, `.depth`, `.buffer`, `.size`, etc. outside `graphics::` calls:

- **Only field ever touched directly by main.cpp: `.ua_view`** on `Texture3D`/`Texture2D`, at 5 call sites, all bypassing the `graphics::` API entirely to call raw D3D11 methods on `graphics_context->context`:
  - Line 924-926: `graphics_context->context->ClearUnorderedAccessViewFloat(trail_tex_A.ua_view, clear_tex)`, same for `trail_tex_B.ua_view`, `trace_tex.ua_view` (F2 "reset particles" handler).
  - Line 937: `ClearUnorderedAccessViewFloat(trace_tex.ua_view, clear_trace)` (F8 "reset trace only").
  - Line 1167: `ClearUnorderedAccessViewUint(display_tex_uint.ua_view, clear_tex_uint)` (every VM_PARTICLES frame, clears the uint accumulation texture before the particle-transform compute pass writes atomic values into it).

  **This is the single most important porting hazard**: main.cpp reaches past the `graphics::` abstraction into raw D3D11 for UAV clears, because `graphics.h` never declared a `clear_texture`/`clear_uav` wrapper. A WebGPU fork needs an equivalent (`wgpu` has no direct "clear UAV" call — this must become either a clear-compute-shader dispatch or `queue.writeTexture` with zero data, and the port must add a `graphics::clear_texture_compute(...)` function so main.cpp's call sites can be rewritten to still compile against the `graphics::` namespace, or `graphics_context` must expose an equivalent handle).

- Also uses `graphics_context` itself directly (the global `GraphicsContext*` with `.device`/`.context` members) at those same 5 sites — so `GraphicsContext` (device/context handles) must remain accessible as a global even after the port, or these call sites need rewriting.

Per-type field summary (fields exist on the struct; "touched by main.cpp" = accessed outside graphics.cpp):

| Type | Fields (graphics.h) | Held as (main.cpp variable names) | Fields touched directly by main.cpp |
|---|---|---|---|
| `RenderTarget` | `rt_view, sr_view, texture, width, height` | `render_target_window` | none |
| `DepthBuffer` | `ds_view, sr_view, texture, width, height` | `depth_buffer` | none |
| `Texture2D` | `texture, sr_view, ua_view, width, height` | `display_tex, display_tex_uint, palette_trace_tex, palette_data_tex` | `.ua_view` (display_tex_uint only, line 1167) |
| `Texture3D` | `texture, sr_view, ua_view, width, height, depth` | `trail_tex_A, trail_tex_B, trace_tex` | `.ua_view` (all three, lines 924-926, 937) |
| `Mesh` | `vertex_buffer, index_buffer, vertex_stride, vertex_offset, vertex_count, index_count, index_format, topology` | `super_quad_mesh, quad_mesh` | none |
| `VertexShader` | `vertex_shader, input_layout` | `vertex_shader, vertex_shader_2d` | none |
| `PixelShader` | `pixel_shader` | `pixel_shader, ps_volume_highlight, ps_volume_halocolor, ps_volume_overdensity, ps_volume_velocity, pixel_shader_2d, ps_volpath` | none |
| `ComputeShader` | `compute_shader` | `draw_compute_shader_particle, blit_compute_shader, compute_shader, sort_shader, decay_compute_shader, cs_density_histo, cs_volpath` | none |
| `ConstantBuffer` | `buffer, size` | `rendering_settings_buffer, config_buffer, statistics_config_buffer` | none |
| `StructuredBuffer` | `buffer, ua_view, size` | `particles_buffer_x/y/z/phi/theta/weights, density_histogram_buffer, halos_densities_buffer` | none |
| `TextureSampler` | `sampler` | `tex_sampler_trace, tex_sampler_deposit, tex_sampler_display, tex_sampler_color_palette` | none |
| `CompiledShader` | `blob` | never held (all shader loading goes through `*_from_code` helpers which create+release internally) | n/a |
| `GraphicsContext` | `device, context` | global `graphics_context` | `.context` (raw D3D11 calls, 5 sites above) |
| `SwapChain` | `swap_chain` | internal global only, never touched by main.cpp | n/a |
| `File` (file_system.h) | `data, size` | 17 local `File` variables (one per read shader/dataset) | `.data`, `.size` read directly to pass into shader-compile / cast to `float*` (dataset) |

---

## 3. `graphics::` functions main.cpp never calls (deletion candidates for the fork)

Grouped by why they're unused:

**Entirely dead subsystems (safe to delete wholesale):**
- Geometry shader family: `compile_geometry_shader`, `get_geometry_shader` (2 overloads), `set_geometry_shader` (2 overloads: bind + unbind), `release(GeometryShader*)`, and the `GeometryShader` struct itself. Nothing in main.cpp uses a geometry shader.
- `show_live_objects` (D3D debug-layer live-object dump — D3D-specific concept, no WebGPU equivalent needed).
- `release(SwapChain*)` — main.cpp never releases the swap chain explicitly (only implicitly via `graphics::release()`).

**Low-level primitives superseded by the `_from_code` helpers actually used:**
- `compile_vertex_shader`, `compile_pixel_shader`, `compile_compute_shader` (main.cpp always goes through `get_vertex_shader_from_code`/`get_pixel_shader_from_code`/`get_compute_shader_from_code`, never calls `compile_*` + `get_*_shader` as a manual two-step).
- `get_vertex_shader` (both overloads — `(CompiledShader*, descs, count)` and `(void*, size, descs, count)`).
- `get_pixel_shader` (both overloads).
- `get_compute_shader` (both overloads).
- `get_vertex_input_desc_from_shader` — only called internally by `get_vertex_shader_from_code`, never directly from main.cpp.
- `set_pixel_shader()` (no-arg unbind form) — main.cpp always binds a real shader, never explicitly unbinds.
- `set_compute_shader()` (no-arg unbind form) — same.

**Render-target/viewport overloads not exercised:**
- `get_render_target(uint32_t width, uint32_t height, DXGI_FORMAT format = ...)` — main.cpp only ever gets the window RT via `get_render_target_window()`; the generic offscreen-RT constructor is unused (no offscreen render targets exist in this codebase — all "offscreen" buffers are compute-written textures, not render targets).
- `clear_depth_buffer` — depth buffer is created but never cleared or actually used as a depth target (see §4 quirk).
- `set_render_targets` (all 3 overloads: `(DepthBuffer*)`, `(RenderTarget*)`, `(RenderTarget*, DepthBuffer*)`) — main.cpp only calls the combined `set_render_targets_viewport` wrappers, never the bare `set_render_targets`.
- `set_viewport` (all 3 overloads: `(RenderTarget*)`, `(DepthBuffer*)`, `(Viewport*)`) — same reasoning; only used internally by `set_render_targets_viewport`. The `Viewport` struct itself is therefore also unused by main.cpp.
- `set_render_targets_viewport(RenderTarget*, DepthBuffer*)` and `set_render_targets_viewport(RenderTarget* buffers, uint32_t count, DepthBuffer*)` (multi-RT form) — main.cpp always uses the single-arg `(RenderTarget*)` overload, even at the one call site (line 450) where a depth buffer is in scope.

**Texture/state functions unused:**
- `save_texture2D` (plain TGA save) — only its HDR sibling `save_texture2D_HDR` is called directly; plain `save_texture2D` is still exercised *indirectly* through `capture_current_frame`, so it can't be deleted outright, just note it's never called directly from main.cpp.
- `set_texture(RenderTarget*, slot)`, `set_texture(DepthBuffer*, slot)` — main.cpp never samples the render target or depth buffer as a shader texture (both overloads for Texture2D/Texture3D are used instead).
- `get_blend_state`, `set_rasterizer_state`, `get_rasterizer_state` — blend state is set once and never queried; rasterizer state is never touched at all (stays SOLID for the whole program).

**Struct/enum consequences:** Since `set_rasterizer_state`/`get_rasterizer_state` and the wireframe raster state are unused, `RasterType::WIREFRAME` is dead from main.cpp's perspective (though `graphics::init` still creates the D3D wireframe rasterizer state unconditionally). Since `Viewport` and its three `set_viewport` overloads are unused, that struct can likely be dropped from the fork's public surface too.

---

## 4. Frame lifecycle as main.cpp drives it

Per-frame order (inside the `while(is_running)` loop, `main.cpp:849-1627`):

1. **Timer/title update** — no graphics calls.
2. **Event/input polling** — no graphics calls; handles F1-F10/NUM1 hotkeys that can trigger side-effecting graphics calls off the main path (F2 = raw UAV clear + structured-buffer re-upload; F8 = raw UAV clear of `trace_tex` only).
3. **Update simulation constant buffer**: `update_constant_buffer(&config_buffer, &simulation_config)` then `set_constant_buffer(&config_buffer, 0)` (binds to all 4 stages at slot 0) — done unconditionally every frame regardless of `run_mold`.
4. **Compute: agent propagation** (`if run_mold`) — bind `compute_shader`, bind trail texture A or B (ping-pong via `is_a`) + `trace_tex` as UAVs, bind 6 particle SoA structured buffers, `run_compute(10, 10, grid_z)`, unset texture slots 0/1. (See §5 table.)
5. **Compute: agent sort** (`if run_mold && sort_agents` — `sort_agents` defaults false, dead in default config) — 256 iterations of `run_compute(10,10,grid_z/256)` incrementing `n_iteration` and re-uploading the config CB each iteration.
6. **Compute: decay/diffusion** (`if run_mold`) — bind `decay_compute_shader`, bind trail A→B or B→A cross-read/write (ping-pong swap of slots 0/1) + `trace_tex`@2, `run_compute(GRID_X/8, GRID_Y/8, GRID_Z/8)`, unset 0/1/2.
7. **Compute: density histogram** (`if compute_histogram`, default true) — zero CPU histogram array, re-upload `density_histogram_buffer`, update+bind `statistics_config_buffer`@0 (**overwrites the slot-0 binding set in step 3 for VS/PS/GS/CS — since only CS is used until the next pixel-shader draw rebinds a real CS-irrelevant slot, this works but is a shared-slot quirk**), bind `trace_tex`@0 (compute-sampled UAV), 6 structured buffers, `run_compute(10,10,grid_z)`, unset 0.
8. **Rendering** (mode-dependent, exactly one branch runs per frame):
   - `set_render_targets_viewport(&render_target_window)` (viewport = full window, no depth bound) then `clear_render_target(background_color³, alpha=1)`.
   - **VM_PARTICLES**: raw UAV clear of `display_tex_uint` → compute dispatch (`draw_compute_shader_particle`, splats particles into `display_tex_uint` via presumed atomic ops) → compute dispatch (`blit_compute_shader`, converts uint→float `display_tex`) → VS/PS draw of `quad_mesh` sampling `display_tex`.
   - **VM_VOLUME / VM_VOLUME_HIGHLIGHT / VM_VOLUME_HALOCOLOR / VM_VOLUME_OVERDENSITY / VM_VOLUME_VELOCITY**: bind `vertex_shader` + mode-specific pixel shader + `trace_tex`@0 (PS SRV) [+ trail texture @1 for HIGHLIGHT/HALOCOLOR modes] → pick one of 3 axis-aligned "most-perpendicular-to-camera" stack orientations, update `model`/`texcoord_map` in the rendering CB, `update_constant_buffer` + `draw_mesh(&super_quad_mesh)` (a Z-stack of quads, drawn as one non-indexed draw call — the "volume slicing" technique) → unset texture slots 0/1.
   - **VM_PATH_TRACING**: if `run_pt` and `pt_iteration < 1e5`: dispatch `cs_volpath` (writes into `display_tex`, samples `trace_tex` + trail texture + both palette textures) with `run_compute(screen_w/10, screen_h/10, 1)`, increments `pt_iteration` (accumulation loop across many frames) → unset compute slots 0/1/2/3 → VS/PS draw `quad_mesh` with `ps_volpath` sampling `display_tex`.
9. **Histogram readback + UI histogram/energy-plot draw** (`if compute_histogram`) — CPU-side stats computed from `capture_structured_buffer` readback, then immediate-mode UI draws (`ui::draw_rect`/`ui::draw_text`) into the already-bound `render_target_window`, ends with `ui::end()`.
10. **Frame capture** (`if make_screenshot` / `if capture_screen`) — `capture_current_frame()` (copies swap-chain back buffer, saves TGA) and/or `save_texture2D_HDR(&display_tex, ...)`.
11. **UI panel** (`if show_ui`) — rebinds `set_render_targets_viewport(&render_target_window)` again, then draws sliders/toggles (`ui::` calls, not `graphics::`) that mutate `simulation_config`/`rendering_config` in place; several toggles set `reset_pt` for the path tracer.
12. **`swap_frames()`** — `Present(1,0)`.

**Blend state**: set exactly once at startup (`set_blend_state(BlendType::ALPHA)`, line 586) and never changed again — every draw call for the rest of the program runs with alpha blending on, including the opaque-looking volume slabs (relies on additive/over blending of stacked quad slices for the volume rendering look).

**Rasterizer state**: never explicitly changed by main.cpp; stays at `RasterType::SOLID` (set inside `graphics::init`) for the whole program. WIREFRAME state is created but never activated.

**Viewport**: always the full window size (derived from `render_target_window.width/height` inside `set_render_targets_viewport`); main.cpp never sets a partial/custom viewport (the `Viewport` struct and its `set_viewport` overload are unused, per §3).

**Depth buffer**: created (`depth_buffer`, line 448) and released at shutdown, but **never bound to the pipeline** — `set_render_targets_viewport(&render_target_window)` is always called with the single-RT overload, so `depth_buffer` is inert dead weight in the current main.cpp (the multi-arg overload that would bind it is never invoked). Confirms depth testing is not actually used; all depth-like effects (volume slicing) are done via alpha-blended draw order, not the depth buffer.

---

## 5. Compute dispatch call sites (dimensions, shader, resources/slots)

| # | Line(s) | Shader (.hlsl file) | Dispatch dims (gx, gy, gz) | Resources bound | Slot map |
|---|---|---|---|---|---|
| 1 | 989-1005 | `cs_agents_propagate.hlsl` | `(10, 10, grid_z)` where `grid_z = (NUM_PARTICLES/100) / THREAD_GROUP_SIZE(1000)` | UAV: trail tex (A or B, ping-pong)@0, trace_tex@1; UAV(structured): particles x@2,y@3,z@4,phi@5,theta@6,weights@7 | u0 trail(active), u1 trace, u2-u7 particle SoA |
| 2 | 1011-1021 | `cs_agents_sort.hlsl` | `(10, 10, grid_z/256)` × 256 iterations (loop unrolled at the call-site level, not in shader) | UAV(structured): x@2,y@3,z@4,phi@5,theta@6,weights@7 (no texture bound) | u2-u7 particle SoA only |
| 3 | 1029-1042 | `cs_field_decay.hlsl` | `(GRID_RESOLUTION_X/8, GRID_RESOLUTION_Y/8, GRID_RESOLUTION_Z/8)` | UAV: trail tex read-side@0, trail tex write-side@1 (swapped opposite of dispatch #1's active buffer), trace_tex@2 | u0 trail(prev), u1 trail(next), u2 trace |
| 4 | 1054-1065 | `cs_density_histo.hlsl` | `(10, 10, grid_z)` same `grid_z` formula as #1 | CB: statistics_config_buffer@b0; UAV(compute-sampled? no — see below) trace_tex@0 via `set_texture_compute` (UAV not SRV); UAV(structured): density_histogram_buffer@1, x@2,y@3,z@4,weights@5, halos_densities_buffer@6 | b0 stats CB; u0 trace, u1 histogram, u2-u5 particle SoA subset, u6 halo densities |
| 5 | 1169-1178 | `cs_particles_transform.hlsl` | `(10, 10, grid_z)` same formula | UAV: display_tex_uint@0; UAV(structured): theta@6 (bound first), x@2,y@3,z@4 | u0 display accumulation tex, u2-u4 xyz, u6 theta (note bind order in source: theta bound before x/y/z, slots are what matter not order) |
| 6 | 1180-1185 | `cs_particles_blit.hlsl` | `(window_width, window_height, 1)` — **one thread group dispatched per pixel** (thread-group size presumably 1×1×1 or the shader itself handles sub-groups; no `/8` or `/10` divisor applied here unlike other dispatches) | UAV: display_tex_uint@0 (read), display_tex@1 (write) | u0 src uint tex, u1 dst float tex |
| 7 | 1262-1280 | `cs_volpath.hlsl` | `(screen_width / PT_GROUP_SIZE_X(10), screen_height / PT_GROUP_SIZE_Y(10), 1)` | UAV: display_tex@0 (accumulate); SRV(compute-sampled): trace_tex@1 + sampler tex_sampler_trace@1(compute), trail tex A-or-B@2 (compute-sampled) + sampler tex_sampler_deposit@2 (**bound via PS-stage `set_texture_sampler`, not `set_texture_sampler_compute`** — likely a latent bug since the sampler needed is CS-stage, see note below), palette_trace_tex@3 + sampler@3(compute), palette_data_tex@4 + sampler@4(compute) | u0 display; t1 trace(sampled), t2 trail(sampled), t3/t4 palettes(sampled); s1,s3,s4 compute samplers; s2 sampler bound to PS stage instead of CS (quirk) |

**Quirk flagged for the fork**: dispatch #7's sampler at slot 2 (`tex_sampler_deposit`) is bound with `graphics::set_texture_sampler(&tex_sampler_deposit, 2)` (line 1271, PS-stage `PSSetSamplers`) even though the texture it pairs with (`trail_tex_A`/`B`) is bound compute-side via `set_texture_sampled_compute`. Every other sampler in this dispatch uses the `_compute` variant. This looks like a bug in the original D3D11 code (relies on stale PS-stage sampler state, or the compute shader doesn't actually need slot-2 sampler and it's dead) — worth flagging to whoever designs the WebGPU bind-group layout, since WebGPU has no implicit "stage doesn't care" fallback; each binding's visibility must be declared explicitly.

**Thread group size note**: `THREAD_GROUP_SIZE = 1000` (main.cpp constant, must match shader-side `[numthreads(...)]`) and `PT_GROUP_SIZE_X/Y = 10` — these are documented in main.cpp comments as "must align with settings inside the shader," i.e. the numthreads() declarations live in the .hlsl files (not read in this pass — shaders directory wasn't in scope) but the dispatch-side group counts in the table above are computed to divide total work by these constants.

---

## 6. Readback paths (GPU→CPU)

| What | When | Function | What main.cpp does with the data |
|---|---|---|---|
| `density_histogram_buffer` (uint[17]) | Every frame, `if compute_histogram` (default on), line 1300 | `capture_structured_buffer(&density_histogram_buffer, density_histogram, N_HISTOGRAM_BINS, sizeof(uint))` | Computes `norm_coef` (sum of bins), `energy`/`mean`/`variance` (log-histogram statistics over bins 1..15, weighted by `HISTOGRAM_BASE^(b-6)`), then immediately renders this as a live bar-chart histogram + a scrolling "energy" line plot via `ui::draw_rect`/`ui::draw_text` (in-loop, on-screen debug visualization, not exported to disk). |
| `halos_densities_buffer` (float[data_count]) | Once, on `store_deposit` (F6 hotkey), line 1101 | `capture_structured_buffer(&halos_densities_buffer, halos_densities, data_count, sizeof(float))` | Written to `export/halos_measurements.csv` alongside `particles_weights`/`particles_x/y/z` (already CPU-resident, not re-read from GPU) — one CSV row per data point: mass, trace density, world-space XYZ, grid-space XYZ. |
| `particles_buffer_x/y/z/weights` (float[NUM_PARTICLES] each, 4 of the 6 SoA buffers — phi/theta NOT read back) | Repeatedly while `capture_agents` is true (F5 toggle), up to `N_AGENT_TIMESTEPS_TO_CAPTURE=10` times, lines 1140-1143 | `capture_structured_buffer` ×4 | Appended to `export/agents.txt`, one block per "timestep" capture: for every agent index `i` in `[data_count, NUM_PARTICLES)` (i.e. **excludes the data-point particles, only free-flowing physarum agents**), writes `X Y Z weight` in world-space units (`measure_grid_to_world`). Stops automatically after 10 captures. |
| GPU texture readback (not via `capture_structured_buffer` — via DirectXTex `CaptureTexture`) | On `store_deposit` (F6): `trail_tex_A`-or-`B` + `trace_tex`; on `make_screenshot` (NUM1): `display_tex`; on `capture_screen`/`make_screenshot`: swap-chain back buffer | `save_texture3D`, `save_texture2D_HDR`, `capture_current_frame`→`save_texture2D` | Written to disk as `.dds`+`.bin` (3D textures) or `.hdr`/`.tga` (2D) under `export/` or `capture/`. Not read back into any CPU-side array main.cpp inspects — pure export-to-file. |

**Key D3D11→WebGPU readback-porting hazard**: `capture_structured_buffer` (used for histogram/halo/agent readback — the "live" readback path exercised every frame) maps a `D3D11_USAGE_DEFAULT` buffer directly with `D3D11_MAP_READ`, relying on `CPUAccessFlags = READ|WRITE` set at buffer creation (`get_structured_buffer`, `graphics.cpp:867`) — no staging buffer, no explicit `CopyResource`. This is a **synchronous, blocking, same-frame GPU→CPU stall** (D3D11 `Map` on a GPU-written resource forces the driver to sync). WebGPU has no equivalent: buffers must be created with `MAP_READ` usage XOR compute-writable usage (a WebGPU buffer cannot be both `STORAGE` and `MAP_READ`), so the fork needs a **separate staging/readback buffer per structured buffer that needs CPU visibility**, a `copyBufferToBuffer` command, and an **async** `buffer.mapAsync()` — meaning the per-frame histogram readback (called unconditionally every frame when `compute_histogram` is on) either needs to become latency-tolerant (read last frame's histogram, not this frame's) or the render loop needs a fence/await point, which is a meaningful architectural change, not just an API swap.

---

## Scale summary

- **39 distinct `graphics::` function names called** from main.cpp (some with multiple overloads collapsed into one name — e.g. `is_ready`, `set_pixel_shader`, `set_compute_shader` each have an unused sibling overload); **2 `file_system::` functions called** (`read_file`, `release_file`; `write_file` unused).
- **291 total `graphics::`/`file_system::`/`graphics_context->` call/reference sites** in main.cpp (1670 lines).
- **13 graphics types** held by main.cpp as local variables (RenderTarget, DepthBuffer, Texture2D, Texture3D, Mesh, VertexShader, PixelShader, ComputeShader, ConstantBuffer, StructuredBuffer, TextureSampler, plus File from file_system and the global GraphicsContext) — only 1 field (`.ua_view`) is ever touched directly by main.cpp, at 5 call sites, all bypassing `graphics::` for raw D3D11 UAV-clear calls.
- **~20 API surface items (functions/overload-groups) are candidates for deletion**: entire GeometryShader family (struct + 5 functions), 3 `compile_*` primitives, 3 `get_*_shader` low-level pairs (6 functions), 2 no-arg unbind functions, `get_render_target`, `clear_depth_buffer`, 3 `set_render_targets` overloads, 3 `set_viewport` overloads (+ `Viewport` struct), 2 unused `set_render_targets_viewport` overloads, 2 `set_texture` overloads (RT/DepthBuffer), `get_blend_state`, `set_rasterizer_state`, `get_rasterizer_state`, `show_live_objects`, `release(SwapChain*)`, `get_vertex_input_desc_from_shader`, `file_system::write_file`.
- **7 compute dispatch call sites** (agent propagate, agent sort ×256-loop, field decay, density histogram, particle transform, particle blit, volpath) across 7 distinct .hlsl compute shaders, fully tabulated with dispatch dims and u/t/b slot bindings in §5.
- **4 readback paths**: per-frame histogram buffer (blocking `Map` on a DEFAULT-usage UAV buffer — the biggest WebGPU-porting hazard, no staging/async equivalent exists), on-demand halo-density CSV export, on-demand agent-trajectory CSV export (10-frame capture loop), and pure-export texture/frame captures (DDS/HDR/TGA via DirectXTex, not inspected by CPU code).

Output file: `/private/tmp/claude-501/-Users-rulkens-Development-vendor-cpp-Polyphorm/f4b860ea-f9da-442a-84db-2bb65801a732/scratchpad/m2-research/graphics-api-inventory.md`
