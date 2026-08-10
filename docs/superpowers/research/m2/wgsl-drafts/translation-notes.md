# HLSL → WGSL translation notes — MCPM core shaders

Reference material for the M2 plan. Covers the two hardest compute shaders:

| source (repo) | draft |
|---|---|
| `shaders/cs_agents_propagate.hlsl` (259 lines) | `cs_agents_propagate.wgsl` (557 lines) |
| `shaders/cs_field_decay.hlsl` (95 lines) | `cs_field_decay.wgsl` (314 lines) |

> **Path correction for the M2 plan.** The task brief referred to
> `bin/cs_agents_propagate.hlsl` / `bin/cs_field_decay.hlsl`. On branch
> `macos-webgpu-port` there is no `bin/` directory — the shaders live in
> `/Users/rulkens/Development/vendor/cpp/Polyphorm/shaders/`. `bin/` was the
> upstream *install* directory. The design doc's "`shaders/` install step"
> (spec line 51) is therefore the right target; anything in the plan that
> says `bin/` should be corrected.

Neither draft has been compiled — no Dawn/Tint binary is reachable from this
sandbox (`build/_deps/dawn-src` is source-only). Validation was done by a
line-by-line walk against the HLSL; see [§7 Uncertain lines](#7-uncertain-lines).

---

## 0. The finding that changes the resource mapping

The design doc (spec line 80) maps `RWTexture3D<half…>` → `r32float`, and the
brief repeats it. That mapping looked lossy, because the shaders declare
`RWTexture3D<half2>` (deposit) and `RWTexture3D<half4>` (trace) while
WebGPU only allows `read_write` on the single-channel `r32*` formats.

**It is not lossy.** In the regime this port targets:

- `main.cpp:23` `#define REGIME_SDSS`; `main.cpp:36` `// #define VELOCITY_ANALYSIS`
  and `main.cpp:38` `// #define HALO_COLOR_ANALYSIS` are both **commented out**.
- `main.cpp:568-569` therefore create the deposit ping-pong as
  **`DXGI_FORMAT_R16_FLOAT`** (1 channel), not `R16G16_FLOAT`.
- `main.cpp:574` creates the trace as **`DXGI_FORMAT_R16_FLOAT`** (1 channel),
  not `R16G16B16A16_FLOAT`.
- `cpplib/graphics.cpp:510` builds the UAV with `Format = format`, i.e. the
  same single-channel format.

D3D11 typed-UAV semantics then silently discard the surplus components on
store and return `(r, 0, 0, 1)` on load. So in the shipped build:

- the deposit **`color` channel** (`cs_agents_propagate.hlsl:110-111`) is
  computed and thrown away;
- the trace **direction channels** `.yzw` (`:258`) are computed and thrown away;
- `cs_field_decay.hlsl`'s `v.y` is structurally always `0`.

**Consequence for M2:** single-channel `r32float, read_write` is an *exact*
behavioural match for both textures, not an approximation. The drafts keep the
`vec2`/`vec4` arithmetic verbatim so re-enabling `VELOCITY_ANALYSIS` /
`HALO_COLOR_ANALYSIS` later is a format widening only. This is tracked as
`QUIRK(r16f_channel_truncation)`.

**Second consequence:** memory is 2× f16 per channel but 1 channel, not 2 or 4.
The spec's "≈ 7.5 GB at VAC scale" figure assumes 1-channel r32float for
2 deposit textures + 1 trace = 3 × 712×1200×728 × 4 B ≈ **7.46 GB**. That
checks out — but only because of the finding above. If someone "fixes" the
half2/half4 mismatch by widening the formats, it becomes ~22 GB.

---

## 1. Quirk toggle mechanism (chosen, applied consistently)

**Pipeline-overridable constants (`override … : bool`) branched with plain
`if`.** Not load-time string preprocessing.

```wgsl
override QUIRK_DECAY_WEIGHT_ALL_INT3: bool = true;
...
if (QUIRK_DECAY_WEIGHT_ALL_INT3) { /* buggy kernel */ } else { /* intended */ }
```

Rationale:

- Tint const-folds override branches at pipeline-creation time, so the dead
  branch costs nothing — identical codegen to a `#define`.
- The on-disk `.wgsl` stays a single always-valid file, preserving the
  HLSL-style edit-and-reload iteration the fork explicitly wants (spec line 51).
- Host side it is `WGPUComputePipelineDescriptor.constants[]` keyed by the
  override **name** (Dawn accepts name keys when no `@id` is given), so the
  spec's *"each preserved quirk is independently toggleable"* (line 145)
  becomes a **runtime** bisection knob for the M5 A/B hunt — recreate the
  pipeline, no C++ recompile, no shader re-authoring.
- The same mechanism carries the four real `#define`s
  (`PROBABILISTIC_SAMPLING`, `AGENT_REROUTING`,
  `FIXED_AGENT_DISTANCE_SAMPLING`, `IGNORE_DATA`) and the workgroup size
  (`WG_X/Y/Z`, usable directly in `@workgroup_size`).

Limits of the mechanism, and how they are handled:

| can't express | quirk | handling |
|---|---|---|
| resource *type* changes | `r16f_channel_truncation` | documented in a comment block; the toggle is a host-side format change |
| host dispatch shape | `dispatch_truncation` | documented in the shader; toggle lives in `main.cpp:1002` |

**RNG-stream caveat that makes `override` the right choice.** Toggles that
change how many `random_float()` calls execute (`FIXED_AGENT_DISTANCE_SAMPLING`)
must put the call *inside* the branch, exactly as the `#ifdef` did — otherwise
the whole downstream stream shifts. The drafts do this
(`cs_agents_propagate.wgsl:336-343`). A naive `select()`-based toggle would
have evaluated both sides and silently desynced the RNG; **do not refactor
these to `select()`.**

---

## 2. `SimulationConfig` uniform layout (byte offsets)

C++ `struct SimulationConfig` at `main.cpp:234-254`. The HLSL `cbuffer` in
`cs_agents_propagate.hlsl:16-32` declares the first **14** members; the one in
`cs_field_decay.hlsl:5-17` declares the first **10**. Both drafts declare all
**16** so one uniform buffer + one `@group(0)` bind group serves every pass.

| # | member | type (C++) | type (WGSL) | offset | size | HLSL cb slot |
|---|---|---|---|---|---|---|
| 0 | `sense_spread` | `float` | `f32` | 0 | 4 | c0.x |
| 1 | `sense_distance` | `float` | `f32` | 4 | 4 | c0.y |
| 2 | `turn_angle` | `float` | `f32` | 8 | 4 | c0.z |
| 3 | `move_distance` | `float` | `f32` | 12 | 4 | c0.w |
| 4 | `deposit_value` | `float` | `f32` | 16 | 4 | c1.x |
| 5 | `decay_factor` | `float` | `f32` | 20 | 4 | c1.y |
| 6 | `center_attraction` | `float` | `f32` | 24 | 4 | c1.z |
| 7 | `world_width` | `int` | `i32` | 28 | 4 | c1.w |
| 8 | `world_height` | `int` | `i32` | 32 | 4 | c2.x |
| 9 | `world_depth` | `int` | `i32` | 36 | 4 | c2.y |
| 10 | `move_sense_coef` | `float` | `f32` | 40 | 4 | c2.z |
| 11 | `normalization_factor` | `float` | `f32` | 44 | 4 | c2.w |
| 12 | `n_data_points` | `int` | `i32` | 48 | 4 | c3.x |
| 13 | `n_agents` | `int` | `i32` | 52 | 4 | c3.y |
| 14 | `n_iteration` | `int` | `i32` | 56 | 4 | c3.z (not in cbuffer) |
| 15 | `filler3` | `int` | `i32` | 60 | 4 | c3.w (not in cbuffer) |

`sizeof == 64`. WGSL struct alignment in the uniform address space is
`roundUp(16, max member align) = 16`; `roundUp(16, 64) = 64`, so **no tail
padding** and **no internal padding** — every member is a 4-byte scalar and
nothing straddles a 16-byte boundary. The C++ struct, the HLSL cbuffer and the
WGSL struct agree byte-for-byte with zero fixups.

### The silent-corruption traps to guard in M2

This struct is safe *today* only because it is 16 scalars. The failure modes
are all future edits:

1. **Any `vec3<f32>` member.** WGSL gives `vec3` align 16 / size 12; HLSL
   cbuffer packing gives it align 4 with a "does not straddle float4"
   rule. A `vec3` at offset 4 lands at WGSL offset 16 but HLSL offset 16 too —
   *usually* agreeing, but a `vec3` at offset 20 lands at WGSL 32 vs HLSL 20.
   **Rule: never add a `vec3` to a shared config struct; use 3 scalars or a
   `vec4`.**
2. **Any array member.** In WGSL's uniform address space array stride is
   rounded up to 16. `array<f32, N>` in a uniform is a 16-byte-per-element
   trap. Use `vec4<f32>` arrays or a storage buffer.
3. **Reordering to group by type.** The C++, HLSL and WGSL declarations are
   three independent copies of the same layout with no compile-time link.
   *Recommendation for M2:* add a CPU-side `static_assert(sizeof(...) == 64)`
   plus `offsetof` asserts, in the CTest target the spec already calls for
   (spec line 165). This is the cheapest possible guard against the class of
   bug that would present as "energy plateaus at the wrong value" rather than
   as a crash.
4. **Trailing-member truncation.** WGSL requires the bound buffer range to be
   ≥ the struct size. Declaring all 16 members means the uniform buffer must
   be ≥ 64 B. It is (`main.cpp:807` uses `sizeof(SimulationConfig)`).

`StatisticsConfig` (`main.cpp:307`, 8 scalars, 32 B) and `RenderingConfig`
(`main.cpp:256`, 3 × `Matrix4x4` + 36 scalars) are out of scope here, but
`RenderingConfig` **does** contain matrices and is the one to check carefully:
WGSL `mat4x4<f32>` is align 16 / size 64, matching HLSL `float4x4`, so it
should be clean — but its offsets need the same `offsetof` assert treatment,
and HLSL's default **column-major** matrix packing vs WGSL's **column-major**
`matCxR` needs an explicit check against how `Matrix4x4` is filled.

---

## 3. `cs_agents_propagate` — semantic difference table

Risk key: **H** = can silently change validation results · **M** = needs a
deliberate decision or a device capability · **L** = mechanical, verifiable by
inspection.

| # | area | HLSL | WGSL | difference bridged | risk |
|---|---|---|---|---|---|
| A1 | texture type | `:1-2` `RWTexture3D<half2>` / `<half4>` | `:125`,`:129` `texture_storage_3d<r32float, read_write>` | see §0 — the bound format is already 1-channel `R16_FLOAT`, so the channel drop is pre-existing, not introduced. f16→f32 storage is a real widening (removes half-precision saturation of the deposit field near hot voxels) | **H** |
| A2 | texture read | `:138,140,145,230` `tex_deposit[coord]` | `:358,370,381,499` `textureLoad(tex_deposit, coord).x` | HLSL's operator returns the declared vector type; WGSL always returns `vec4<f32>`. `.x` / `.xy` extraction is explicit | L |
| A3 | texture RMW | `:111,255,258` `tex[c] += v` | `:314-317`, `:531-535`, `:550-556` load → add → `textureStore` | WGSL has no `+=` on textures. **The race is preserved** — still a non-atomic load-add-store, still lost updates, by design (spec line 81) | **H** (intentional) |
| A4 | OOB access | `:140,145` sensing can leave the grid; D3D11 typed-UAV OOB load ⇒ 0 | same lines, `textureLoad` | WGSL *guarantees* the zero value for an invalid texel address; D3D11 guarantees the same. Exact match, no clamp emulation needed | M |
| A5 | int truncation | `:137` `int3(x,y,z)`, `:140,145` `int3(offset)` | `:353,370,381` `vec3<i32>(...)` | both truncate toward zero. `QUIRK(int3_truncated_sensing)` | L |
| A6 | float→uint | `:111,230,255,258` `uint3(x,y,z)` | `:314,499,524` `vec3<u32>(vec3<f32>(...))` | HLSL: out-of-range is *undefined*; WGSL: *indeterminate*. Same hazard class. `mod()` keeps the value in `[0,w)`, except for the f32 edge case where `x - w*floor(x/w)` rounds up to exactly `w` — then the store lands OOB and is discarded in both APIs | M |
| A7 | float→uint (seed) | `:98` `uint(x*y*z)` | `:289` `u32(x*y*z)` | fine to ~4.29e9; VAC grid max ≈ 6.2e8, 1024³ ≈ 1.07e9. A grid above ~1625³ would make this indeterminate in both languages | L |
| A8 | RNG struct | `:34-70` struct with member functions | `:148-197` struct + `ptr<function, RNG>` free functions | WGSL has no methods. All constants (`0x464fffff`, `0x9068ffff`, `36969`, `18000`, `65535`, `61`, `9`, `0x27d4eb2d`, shifts 16/4/15) are byte-identical | L |
| A9 | RNG divisor | `:59` `float(0xFFFFFFFFU)` | `:196` `f32(0xFFFFFFFFu)` | both round to `4294967296.0`; identical `[0,1)` mapping | L |
| A10 | u32 overflow | `:53-55,64,66` | `:187-189,158,160` | HLSL and WGSL both define unsigned wrap-around mod 2³². `<<` discards high bits for unsigned in both | L |
| A11 | RNG seed bug | `:44` tests `m_w` where it means `m_z` | `:171-183` | `QUIRK(rng_seed_guard_typo)`; a zero `m_z` is never repaired | M |
| A12 | RNG draw order | draws at `:121,130,143,154,234,235,236` | `:329,342,376,398,507,508,509` | **order and count preserved exactly**, including the unconditional draw at `:154` before the branch and the three conditional draws on the reroute path only | **H** |
| A13 | `%` / mod | `:77-79` `float mod()` | `:216-218` `fn mod_floor()` | **renamed: `mod` is a WGSL reserved word.** Floor-mod ⇒ movement wrap is fully periodic (contrast the decay shader's `%`) | L |
| A14 | int→float promotion | `:200` `world_width / 2.0`, `:234-236` `rand * world_width` | `:440-442`, `:507-509` explicit `f32(cfg.world_width)` | WGSL has no implicit conversions. Writing `f32(cfg.world_width / 2)` would be integer division — a half-cell attractor shift on odd dims. Flagged in-line | **H** |
| A15 | scalar→vector broadcast | `:205` `float3 center_angle = acos(<scalar>)` | `:451` `vec3<f32>(acos(...))` | HLSL implicit broadcast; WGSL explicit. All 3 components equal, so the slerp result is unchanged | L |
| A16 | `const` on non-const | `:227,229` `const float` initialised from a uniform | `:494,496` `let` | WGSL `const` needs a const-expression. `:493` `const w_f = 0.9;` stays `const` | L |
| A17 | `#define` → toggle | `:11-14` | `:45-52` `override` | see §1 | M |
| A18 | brace-less nested `if` | `:155-160` outer `if` with no braces | `:403-410` | WGSL requires braces; nesting semantics identical | L |
| A19 | ternary | `:110` nested `?:` | `:302-305` `if/else if/else` | WGSL has no `?:`. `select()` was **not** used — it evaluates both arms, which is fine here but sets a bad precedent next to A12 | L |
| A20 | `numthreads` | `:86` `[numthreads(10,10,10)]` = **1000** invocations | `:237-241` `@workgroup_size(WG_X,WG_Y,WG_Z)` overrides, default `10,10,10` | **exceeds WebGPU's default `maxComputeInvocationsPerWorkgroup` of 256.** See §5 | **M/blocker** |
| A21 | `SV_GroupIndex` | `:87` | `:245` `@builtin(local_invocation_index)` | identically defined as `x + y·Sx + z·Sx·Sy` | L |
| A22 | flat index math | `:88-89` hard-coded `10 / 100 / 1000` | `:260-263` via `@builtin(num_workgroups)` and `WG_*` | reduces to the HLSL constants for the real dispatch `(10,10,grid_z)`; stays a bijection under reshape. See §5 | M |
| A23 | dead load | `:138` `current_deposit` assigned then overwritten at `:230` | `:355-359` `_ = textureLoad(...)` behind `QUIRK_DEAD_CURRENT_DEPOSIT_READ` | phony-assignment; Tint may elide it either way. No observable effect | L |
| A24 | `pow` domain | `:148-149` `pow(max(d,0), sharpness)` | `:390-391` | both undefined for a negative base; the `max` guard is preserved. `pow(0,0)==1` in both | L |
| A25 | `acos` domain | `:165,205,209` unclamped | `:421,451,458` unclamped | `|arg|` can exceed 1 by an ULP ⇒ NaN (HLSL) / indeterminate (WGSL). Deliberately not clamped — clamping is a behaviour change | M |
| A26 | div-by-zero | `:207` `/ sin(center_angle)` when the agent already faces the centre | `:455-456` | HLSL ⇒ ±inf/NaN; WGSL float div-by-zero ⇒ indeterminate. Inert for VAC (`center_attraction = 0`) | L |
| A27 | `atan2` arg order | `:164,210` `atan2(y, x)` | `:417,459` `atan2(y, x)` | same order, same branch cuts | L |
| A28 | `PI` | `:82` `3.141592` | `:222` `3.141592` | the original's truncated π (7 s.f., error 6.5e-7) kept verbatim — it biases `random_angle` and must not be "fixed" | L |
| A29 | barriers | none | none | no groupshared memory, no `workgroupBarrier`. Cross-invocation visibility is exactly as (un)defined as the original | L |
| A30 | dispatch truncation | `main.cpp:1002` `grid_z = (N/100)/1000` drops `N % 100000` | documented at `:265-272`; no bounds guard added | `QUIRK(dispatch_truncation)`. Data points are at the buffer front and always run | M |
| A31 | trace weighting | `:258` increment `= distance_scaling_factor / normalization_factor` | `:550-556` | `QUIRK(trace_weighted_by_sense_distance)`; `normalization_factor` is pinned to 1.0 (`main.cpp:803`, adaptive update commented out at `main.cpp:1315`) | M |
| A32 | move/sense coupling | `:215` step scaled by `(0.1 + 0.9·distance_scaling_factor)` | `:471-474` | `QUIRK(move_sense_distance_coupling)` — reuses the *sensing* MB draw for the *step length* | M |
| A33 | shared jitter | `:121` `xiDirectional` reused at `:161` for the turn magnitude | `:329`, `:411` | one random, two uses. A "clean" rewrite would draw twice and desync | M |
| A34 | deposit site | `:230` reads at the post-move position; `:255,258` write at the **post-reroute** position | `:499`, `:524` (`wc` computed after the reroute block) | easy to get wrong: a rerouted agent deposits at its *new* random location, not where it moved to | **H** |
| A35 | storage buffers | `:4-9` `RWStructuredBuffer<float>` u2–u7 | `:132-137` `var<storage, read_write> array<f32>` | direct. `arrayLength()` not used, matching the original's absence of bounds checks | L |

## 4. `cs_field_decay` — semantic difference table

| # | area | HLSL | WGSL | difference bridged | risk |
|---|---|---|---|---|---|
| D1 | access modes | `:1-3` all three are `RWTexture3D` | `:108` `<r32float, read>`, `:111` `<r32float, write>`, `:116` `<r32float, read_write>` | `tex_in` is only read and `tex_out` only written, so narrowing the access mode is free and encodes the ping-pong contract. Needs the `readonly_and_readwrite_storage_textures` WGSL language feature (core in current WGSL/Dawn) | M |
| D2 | the weighting bug | `:69` `(all(int3(dx,dy,dz)) == 0) ? 1.0 : 1.0/sqrt(...)` | `:216-243` | WGSL's `all()` takes `vecN<bool>`, so HLSL's `all(int3)` ("every component non-zero") becomes `all(vec3<bool>(dx!=0, dy!=0, dz!=0))`, and `== 0` becomes `!`. **19 taps @ 1.0 + 8 corners @ 0.5773503, Σw = 23.6188021.** Intended was `all(int3(...) == 0)` ⇒ centre 1.0 / faces 1.0 / edges 0.7071068 / corners 0.5773503, Σw = 20.1043. The bug over-weights the 12 edge taps by ~41%, making the kernel *more* diagonal — the exact artefact `:63`'s comment says it was avoiding. `QUIRK(decay_weight_all_int3)` | **H** |
| D3 | `1/sqrt(0)` | `:69` the ternary's false arm would be `1/sqrt(0)` at the centre tap | `:216-243` falloff **not hoisted** out of the branches | both forms take the 1.0 branch at the centre, so it is never evaluated. Hoisting it (tempting, since it is loop-invariant per tap) would compute an indeterminate value; harmless but noisy in captures | L |
| D4 | `%` boundary | `:71-73` `txcoord.x % world_width` | `:251-253` `txcoord.x % cfg.world_width` | **literal port.** WGSL `%` on `i32` is `e1 - e2*trunc(e1/e2)` — sign of dividend, same as HLSL. `-1 % w == -1`, so the low faces do **not** wrap; the load falls OOB and returns 0 (D1/A4). High side wraps correctly (`w % w == 0`). `QUIRK(nonperiodic_low_boundary)` | **H** |
| D5 | boundary darkening | `:65,76` `w` always sums 23.6188 | `:211,268` | the denominator does **not** compensate for taps that returned 0, so the three low faces are additionally darkened on top of the mass leak. Part of D4; no separate toggle | M |
| D6 | `int3(uint3)` | `:70` `int3(p)` | `:245` `vec3<i32>(p)` | value-preserving below 2³¹ in both | L |
| D7 | texture read | `:74` `float2 val = tex_in[txcoord]` | `:266` `textureLoad(tex_in, txcoord).xy` | `r32float` yields `(r,0,0,1)`; `.xy == (r,0)` matches the D3D11 `half2`-on-`R16_FLOAT` result exactly | L |
| D8 | texture write | `:84` `tex_out[p] = v` (float2) | `:279` `textureStore(tex_out, p, vec4<f32>(v, 0.0, 0.0))` | `textureStore` needs a full `vec4`; only `.x` lands | L |
| D9 | trace RMW | `:93` `tex_trace[p] *= f` | `:312-313` load → mul → store | **race-free** here: `p` is `global_invocation_id`, one invocation per texel, nothing else writes it in this dispatch. Contrast A3 | L |
| D10 | dithered decay | `:93` `0.985 + 0.01*rand` | `:304-309` | `QUIRK(dithered_trace_decay)`. The original mitigation targeted f16 quantisation (a constant sub-ULP decay would round back and never decay); at f32 it is unnecessary, but it also sets the *mean* rate to 0.99 and injects correlated noise the published cube was fitted with. The "off" arm uses `0.99` — the value `:87` keeps commented out — not `1.0` | **H** |
| D11 | dither seed | `:90` `wang_hash(uint(113.0*v.x))` | `:300` | f32→u32 **truncation**: for most voxels `113·v.x < 1`, so seed1 collapses to `wang_hash(0)` across all of empty space. The dither is banded by deposit magnitude, not white. `QUIRK(quantized_dither_seed)` | M |
| D12 | dither seed 2 | `:91` `wang_hash(uint(p.x*p.y*p.z))` | `:301` | `u32` product, wraps identically in both. **Zero on every voxel of the `x=0`, `y=0`, `z=0` planes**, and aliases heavily elsewhere (`(2,3,4)` and `(1,4,6)` collide). Preserved | M |
| D13 | RNG | `:19-55` — a verbatim copy of the agent shader's | `:129-179` — same, including the `m_w`/`m_z` typo at `:29` | keeping two copies (rather than a shared include) mirrors the original; WGSL has no `#include`. If the fork later adopts WESL/`wesl-plugin`, this is the obvious first shared module | L |
| D14 | unused builtins | `:58-59` declare `SV_GroupThreadID` and `SV_GroupID`, both unused | `:200-203` only `global_invocation_id` | dropping unused entry-point params is behaviour-neutral | L |
| D15 | `numthreads` | `:57` `[numthreads(8,8,8)]` = **512** | `:195-199` overrides, default `8,8,8` | **exceeds the default 256 limit.** But unlike A20 the reshape here is *bitwise* identical (see §5) | **M/blocker** |
| D16 | dispatch coverage | `main.cpp:1038` `(GX/8, GY/8, GZ/8)`; grid dims rounded to multiples of 8 at `main.cpp:412-414` | no tail guard added | coverage is exact; the original has no guard and none is needed | L |
| D17 | cbuffer subset | `:5-17` declares 10 of the 16 `SimulationConfig` members | `:75-98` declares all 16 | offsets of the first 10 are identical (all 4-byte scalars), so one uniform buffer + one `@group(0)` bind group is shared with the agent pass | L |

---

## 5. `numthreads` → `@workgroup_size` (the one real blocker)

| shader | HLSL | invocations | vs WebGPU default `maxComputeInvocationsPerWorkgroup` = 256 |
|---|---|---|---|
| `cs_agents_propagate` | `(10,10,10)` | **1000** | over by 3.9× |
| `cs_field_decay` | `(8,8,8)` | **512** | over by 2× |

Per-dimension limits are fine (`maxComputeWorkgroupSizeX/Y` = 256,
`…SizeZ` = 64 by default; 10 and 8 clear both).

**Option A — raise the limit (preferred, keeps the drafts literal).**
Query `adapter.limits.maxComputeInvocationsPerWorkgroup` and request ≥ 1000 in
`requiredLimits` at device creation. Metal's
`maxTotalThreadsPerThreadgroup` is 1024 on Apple Silicon, so Dawn's Metal
backend is expected to report 1024 — but this **must be verified on the target
machine in M2**, not assumed. Per the spec's error-handling section (line 152),
a failure here should be a named fatal at startup, not a mystery.

**Option B — reshape (fallback, and provably safe).**

*`cs_field_decay`:* `@workgroup_size(8,8,4)` = 256, dispatch
`(GX/8, GY/8, GZ/4)`. Every invocation writes exactly one texel derived only
from its own `global_invocation_id`, with no shared memory and no barriers, so
this is **bitwise identical**. Zero risk.

*`cs_agents_propagate`:* `@workgroup_size(10,10,2)` = 200, dispatch
`(10, 10, 5·grid_z)`. Justification:

- The only use of the thread/group ids is `idx`, a particle index.
- Original: `idx = local_index + 1000·(gx + gy·10 + gz·100)`, a bijection onto
  `[0, 100000·grid_z)`.
- Generalised as written at `cs_agents_propagate.wgsl:260-263`:
  `idx = local_index + (Sx·Sy·Sz)·(gx + gy·Nx + gz·Nx·Ny)`, which **reduces to
  the HLSL expression exactly** for the original dispatch `N = (10,10,grid_z)`,
  and remains a bijection onto `[0, total)` for any shape.
- Reshaped: `200·(gx + gy·10 + gz·100)` with `gz ∈ [0, 5·grid_z)` ⇒ the same
  range `[0, 100000·grid_z)`, same particle set, no particle run twice.
- The *assignment* of particles to hardware groups changes, which changes only
  the interleaving of the already-racy deposit writes — i.e. it is
  **statistically** identical, which is the only reproducibility the port has
  anyway (spec line 113).
- Dispatch count check: for the 10M-agent VAC run, `NUM_PARTICLES ≈ 10,324,849`
  ⇒ `grid_z = 103`, so `5·grid_z = 515`, well under
  `maxComputeWorkgroupsPerDimension` = 65535.

Both options are one host-side line because `WG_X/Y/Z` are `override`s used
directly in `@workgroup_size`. **Recommendation:** try A, keep B as a startup
fallback, and log which path was taken so an M5 discrepancy can be attributed.

---

## 6. Bind group layout proposal

The spec says "one bind group per pass" (line 78). The drafts use **two
groups**, a deliberate refinement:

```
@group(0) @binding(0)  var<uniform> cfg: SimulationConfig     // per-frame, shared by every pass
@group(1) @binding(N)  <pass resources>, N == the HLSL register index
```

Rationale:
- `SimulationConfig` is written once per frame (`main.cpp:982-983`) and read by
  the agent pass, the decay pass and the sort pass. One bind group, set once.
- Keeping `@binding(N) == uN` makes the WGSL diffable against the HLSL during
  review, which matters a lot for a bug-for-bug port. `b0` cannot also be
  `@binding(0)` in the same group, which is precisely why the uniform gets its
  own group.

Resulting layouts:

**`cs_agents_propagate`**

| group | binding | resource | WebGPU type |
|---|---|---|---|
| 0 | 0 | `cfg` | `uniform`, min size 64 |
| 1 | 0 | `tex_deposit` (`trail_tex_A` or `_B`) | storage texture, `r32float`, `read-write`, 3d |
| 1 | 1 | `tex_trace` | storage texture, `r32float`, `read-write`, 3d |
| 1 | 2..7 | `particles_{x,y,z,phi,theta,weights}` | storage buffer, `read-write` |

**`cs_field_decay`**

| group | binding | resource | WebGPU type |
|---|---|---|---|
| 0 | 0 | `cfg` | `uniform`, min size 64 |
| 1 | 0 | `tex_in` (ping) | storage texture, `r32float`, **`read-only`**, 3d |
| 1 | 1 | `tex_out` (pong) | storage texture, `r32float`, **`write-only`**, 3d |
| 1 | 2 | `tex_trace` | storage texture, `r32float`, `read-write`, 3d |

Two bind groups per pass for the deposit ping-pong (A-as-ping / B-as-ping),
pre-created at startup and selected by the `is_a` flag (`main.cpp:988`).

Texture usages needed at creation:
- `trail_tex_A/B`: `STORAGE_BINDING | TEXTURE_BINDING` (the volume shaders
  sample the deposit at `main.cpp:1210-1221`) `| COPY_SRC` for readback.
- `trace_tex`: same, plus it is the F6 export source.

**Validation points to check in M2:**
- `r32float` needs the **`float32-filterable`** device feature to be sampled
  with a linear/aniso sampler (spec line 82). Only the *render* passes need it;
  these two compute passes do not.
- WebGPU's usage-scope conflict rule (a writable storage texture may not alias
  another binding in the same scope) is **not** violated here: for compute
  passes the usage scope is a **single dispatch**, and within one dispatch
  `tex_in` and `tex_out` are always different textures. Running the agent pass
  and the decay pass back-to-back in the *same* `ComputePassEncoder` is
  therefore legal, and WebGPU inserts the memory barrier between dispatches.
  Dawn will still complain loudly if the ping-pong flag ever gets out of sync
  and both bindings resolve to the same texture — a useful free assertion the
  D3D11 version never had.

---

## 7. Uncertain lines

Everything below was checked by inspection against the HLSL but **could not be
compiled**. Ordered by how likely it is to bite.

### Would fail to compile (syntax/validation), if wrong

1. **`cs_agents_propagate.wgsl:196` / `cs_field_decay.wgsl:178` —
   `f32(0xFFFFFFFFu)`.** The const-eval converts `4294967295u` to `f32`, which
   rounds to `4294967296.0`. WGSL permits rounding on int→float conversion, so
   this should be accepted, but Tint could plausibly emit an out-of-range
   diagnostic. *Safe rewrite if it errors:* `4294967296.0` — the identical
   value, no behaviour change.
2. **`cs_agents_propagate.wgsl:237-241`, `cs_field_decay.wgsl:195-199` —
   `override` used inside `@workgroup_size` AND in a runtime expression
   (`WG_X * WG_Y * WG_Z` at `:263`).** Both are legal per spec; I have not
   verified Tint accepts the same override in both positions.
3. **`cs_field_decay.wgsl:108` — `texture_storage_3d<r32float, read>`.**
   Read-only storage textures are core WGSL now, but if the pinned Dawn
   revision predates it, this needs `enable`-less fallback to `read_write`
   (harmless; just a wider access mode).
4. **`cs_agents_propagate.wgsl:358` — `_ = textureLoad(...).x;`.** Phony
   assignment of a texture-load result. Legal, but I am not certain Tint keeps
   the load rather than eliding it. Unobservable either way (this is
   `QUIRK(dead_current_deposit_read)`).
5. **`cs_agents_propagate.wgsl:493` — `const w_f: f32 = 0.9;` at function
   scope.** Function-scope `const` is legal WGSL; if the toolchain objects,
   `let` is an exact substitute.
6. **Reserved-word sweep.** I renamed `mod` → `mod_floor` (`mod` *is* on the
   WGSL reserved list). I believe `rotate`, `set_seed`, `random_uint`,
   `random_float`, `wang_hash`, `weight`, `falloff`, `dir`, `add`, `prev`,
   `sum`, `val` are all clear, but I could not run a checker. `filter`,
   `sample`, `common`, `shared`, `match`, `move` are reserved and are not used.

### Would compile but could differ numerically

7. **`cs_agents_propagate.wgsl:451-456` — the `center_angle` `vec3` slerp.**
   I kept HLSL's implicit scalar→`vec3` broadcast as an explicit `vec3`, so the
   division is componentwise on three equal values. Mathematically identical to
   the scalar form; I am *fairly* confident an FMA-contraction difference
   cannot arise from the vectorisation, but not certain. **Inert for VAC
   validation** (`center_attraction = 0`), so this is low-priority to resolve.
8. **`cs_agents_propagate.wgsl:314`, `:499`, `:524` —
   `vec3<u32>(vec3<f32>(x,y,z))` when a coordinate is exactly `world_dim`.**
   `mod_floor` can produce exactly `w` for tiny negative inputs. Both APIs then
   discard the store / return 0 on load, so I believe this matches — but the
   D3D11 behaviour for a *typed UAV store* one texel past the end is
   "discarded" by spec, and I have not confirmed Dawn/Metal does not clamp
   instead. If Metal clamps, mass that D3D11 dropped would land on the far
   face. **Worth a targeted M2 test** (single agent at `x = -1e-7`).
9. **`cs_field_decay.wgsl:266` — OOB `textureLoad` returning zero.** WGSL
   guarantees the zero value for an invalid texel address; D3D11 guarantees 0
   for an OOB typed-UAV load. I am confident about both specs, but this is the
   single load-bearing assumption behind `QUIRK(nonperiodic_low_boundary)`
   porting *literally* rather than needing emulation. **This deserves an
   explicit M2 smoke test**: fill `tex_in` with 1.0, run one decay step, and
   check three voxels:
   - interior ⇒ `1.0 · decay_factor` (all 27 taps present, `v = w/w`);
   - on the `x=0` face but interior in y and z ⇒
     `0.6905262 · decay_factor`. The `dx = -1` slab loses 9 taps whose weights
     sum to `5·1.0 + 4·(1/√3) = 7.3094011` (5 of the 9 have `dy` or `dz`
     zero ⇒ weight 1.0; the 4 with both non-zero are corners ⇒ 0.5773503),
     and `w` still sums the full `23.6188021`, so the ratio is
     `(23.6188021 − 7.3094011)/23.6188021`;
   - on the `x = world_width − 1` face ⇒ `1.0 · decay_factor`, because the
     high side *does* wrap.

   That one test pins D2 (the exact weight multiset), D4 (sign-of-dividend
   `%`), D5 (uncompensated denominator) and A4 (OOB ⇒ 0) simultaneously, and
   it is worth writing before anything else in M2 — all four are invisible in
   a rendered image and all four would sail past a "looks like filaments"
   eyeball check.
10. **`cs_agents_propagate.wgsl:390-391` — `pow` on an f32-widened deposit.**
    Unchanged code, but the *input* is now f32 rather than f16-quantised
    (A1). With `sharpness = 2.5` (the VAC value), small deposit differences
    that f16 used to flatten to equality now break ties in the softmax. This is
    a genuine, unavoidable numerical divergence from the published run — it
    should be called out in the M5 report as an expected source of
    correlation loss, not treated as a bug.
11. **`cs_field_decay.wgsl:300` — `u32(113.0 * v.x)`.** If `v.x` ever exceeds
    ~3.8e7 the conversion is indeterminate in WGSL / undefined in HLSL. Deposit
    values that large would mean the simulation has already diverged, so this
    is a latent-only concern.

### Open questions for the M2 plan

- **Does the M2 energy-rise smoke test (spec line 166) read `.x` of the trace
  texture?** With single-channel `r32float` there is nothing else to read, but
  any code written against the `half4` declaration will silently read zeros.
- **Where does the ping-pong `is_a` flag live** once bind groups are
  pre-created? The D3D11 code toggles it at `main.cpp:988` *inside* the
  `run_mold` branch and the decay pass at `main.cpp:1030` depends on that same
  value — an off-by-one-frame here would decay the texture the agents just
  wrote *into*, which would still "look like it works".
- **Should the two RNG copies be unified?** D13. Keeping them duplicated is
  faithful; the fork's `wesl-shaders` tooling would make a shared module cheap.
  Recommend deferring until after M5 so the diff stays reviewable.
- **`static_assert`/`offsetof` guards on `SimulationConfig`** — §2, cheap, and
  the only defence against the silent-corruption class.
