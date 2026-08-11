#pragma once
#include <webgpu/webgpu_cpp.h>
struct Window;

struct GpuContext {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Surface surface;
    wgpu::TextureFormat surface_format = wgpu::TextureFormat::BGRA8Unorm;
    uint32_t width = 0, height = 0;
    uint32_t max_workgroup_invocations = 0;      // granted maxComputeInvocationsPerWorkgroup
    uint64_t max_storage_buffer_binding_size = 0;
    uint64_t max_buffer_size = 0;
};

namespace gpu {
// Headless-safe device initialization (instance/adapter/device/queue and all
// feature/limit gating). Returns false on failure (fatal will have been called).
bool init_device(GpuContext *ctx);
// Surface creation and configuration at framebuffer resolution (preserves Retina fix).
// Returns false on failure (fatal will have been called).
bool init_surface(GpuContext *ctx, Window *window);
// Re-Configure an already-created surface at a new framebuffer size (M4a
// Task 2b: window resize support). Caller (graphics::resize_surface) is
// responsible for skipping this when width/height is 0 (minimized/
// degenerate window — Dawn requires positive Configure extents) and for
// calling only between frames (no open command encoder/render pass).
void resize_surface(GpuContext *ctx, uint32_t width, uint32_t height);
// Combined initialization: calls init_device then init_surface in sequence.
// Fatal (prints the missing feature/limit name and aborts) on any failure —
// spec "Error handling": startup capability misses must be loud and named.
GpuContext init(Window *window);
// Acquire the current surface texture, run a render pass that clears it
// to (r, g, b), submit, and present. M2 replaces this with the full
// graphics:: pass API; nothing else may grow onto it.
void clear_and_present(GpuContext *ctx, float r, float g, float b);
}
