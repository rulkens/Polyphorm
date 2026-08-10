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
};

namespace gpu {
// Fatal (prints the missing feature/limit name and aborts) on any failure —
// spec "Error handling": startup capability misses must be loud and named.
GpuContext init(Window *window);
// Acquire the current surface texture, run a render pass that clears it
// to (r, g, b), submit, and present. M2 replaces this with the full
// graphics:: pass API; nothing else may grow onto it.
void clear_and_present(GpuContext *ctx, float r, float g, float b);
}
