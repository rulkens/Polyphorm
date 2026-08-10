#include "gpu/gpu_context.h"
#include "platform.h"
#include <webgpu/webgpu_glfw.h>
#include <cstdio>
#include <cstdlib>

namespace gpu {

static void fatal(const char *what) {
    std::fprintf(stderr, "[gpu] FATAL: %s\n", what);
    std::abort();
}

GpuContext init(Window *window) {
    GpuContext ctx;
    ctx.instance = wgpu::CreateInstance(nullptr);
    if (!ctx.instance) fatal("wgpu::CreateInstance failed");

    // Synchronous adapter/device acquisition: Dawn's async requests are
    // pumped to completion with ProcessEvents. The names of the callback
    // enums drift between Dawn revisions; the pinned revision in
    // CMakeLists.txt is the source of truth — keep the semantics
    // (request, pump until the callback fired) and fix names per the
    // compiler's suggestions if the pin ever moves.
    wgpu::RequestAdapterOptions adapter_opts = {};
    adapter_opts.powerPreference = wgpu::PowerPreference::HighPerformance;
    bool done = false;
    ctx.instance.RequestAdapter(
        &adapter_opts, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView msg) {
            if (status != wgpu::RequestAdapterStatus::Success)
                fatal("RequestAdapter failed");
            ctx.adapter = adapter;
            done = true;
        });
    while (!done) ctx.instance.ProcessEvents();

    // float32-filterable is required by the volume renderer (spec: GPU
    // resource mapping — trace is r32float and sampled with a linear
    // sampler). Fail HERE, with the feature's name, not later.
    if (!ctx.adapter.HasFeature(wgpu::FeatureName::Float32Filterable))
        fatal("adapter lacks required feature: float32-filterable");

    wgpu::FeatureName required[] = { wgpu::FeatureName::Float32Filterable };
    wgpu::DeviceDescriptor dev_desc = {};
    dev_desc.requiredFeatures = required;
    dev_desc.requiredFeatureCount = 1;
    dev_desc.SetUncapturedErrorCallback(
        [](const wgpu::Device &, wgpu::ErrorType type, wgpu::StringView msg) {
            std::fprintf(stderr, "[gpu] uncaptured error (%d): %.*s\n",
                         (int)type, (int)msg.length, msg.data);
        });
    dev_desc.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device &, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
            std::fprintf(stderr, "[gpu] device lost (%d): %.*s\n",
                         (int)reason, (int)msg.length, msg.data);
        });

    done = false;
    ctx.adapter.RequestDevice(
        &dev_desc, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView msg) {
            if (status != wgpu::RequestDeviceStatus::Success)
                fatal("RequestDevice failed");
            ctx.device = device;
            done = true;
        });
    while (!done) ctx.instance.ProcessEvents();
    ctx.queue = ctx.device.GetQueue();

    ctx.surface = wgpu::glfw::CreateSurfaceForWindow(ctx.instance, window->window_handle);
    if (!ctx.surface) fatal("CreateSurfaceForWindow failed");
    ctx.width = window->window_width;
    ctx.height = window->window_height;

    wgpu::SurfaceConfiguration config = {};
    config.device = ctx.device;
    config.format = ctx.surface_format;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.width = ctx.width;
    config.height = ctx.height;
    config.presentMode = wgpu::PresentMode::Fifo;  // = the original's Present(1,0) vsync
    ctx.surface.Configure(&config);
    return ctx;
}

void clear_and_present(GpuContext *ctx, float r, float g, float b) {
    wgpu::SurfaceTexture surface_tex;
    ctx->surface.GetCurrentTexture(&surface_tex);
    if (surface_tex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surface_tex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal)
        fatal("GetCurrentTexture failed");

    wgpu::RenderPassColorAttachment att = {};
    att.view = surface_tex.texture.CreateView();
    att.loadOp = wgpu::LoadOp::Clear;
    att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {r, g, b, 1.0};
    wgpu::RenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;

    wgpu::CommandEncoder enc = ctx->device.CreateCommandEncoder();
    enc.BeginRenderPass(&rp).End();
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx->queue.Submit(1, &cmd);
    ctx->surface.Present();
    ctx->instance.ProcessEvents();
}

}  // namespace gpu
