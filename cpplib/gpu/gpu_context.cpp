#include "gpu/gpu_context.h"
#include "platform.h"
#include "logging.h"
#include <webgpu/webgpu_glfw.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>

namespace gpu {

[[noreturn]] static void fatal(const char *what) {
    std::fprintf(stderr, "[gpu] FATAL: %s\n", what);
    std::abort();
}

bool init_device(GpuContext *ctx) {
    ctx->instance = wgpu::CreateInstance(nullptr);
    if (!ctx->instance) fatal("wgpu::CreateInstance failed");

    // Synchronous adapter/device acquisition: Dawn's async requests are
    // pumped to completion with ProcessEvents. The names of the callback
    // enums drift between Dawn revisions; the pinned revision in
    // CMakeLists.txt is the source of truth — keep the semantics
    // (request, pump until the callback fired) and fix names per the
    // compiler's suggestions if the pin ever moves.
    wgpu::RequestAdapterOptions adapter_opts = {};
    adapter_opts.powerPreference = wgpu::PowerPreference::HighPerformance;
    bool done = false;
    ctx->instance.RequestAdapter(
        &adapter_opts, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView msg) {
            if (status != wgpu::RequestAdapterStatus::Success)
                fatal("RequestAdapter failed");
            ctx->adapter = adapter;
            done = true;
        });
    while (!done) ctx->instance.ProcessEvents();

    // float32-filterable is required by the volume renderer (spec: GPU
    // resource mapping — trace is r32float and sampled with a linear
    // sampler). Fail HERE, with the feature's name, not later.
    if (!ctx->adapter.HasFeature(wgpu::FeatureName::Float32Filterable))
        fatal("adapter lacks required feature: float32-filterable");

    // Adapter-supported limits: request what the sim needs, degrade where allowed.
    wgpu::Limits supported = {};
    ctx->adapter.GetLimits(&supported);

    wgpu::Limits required = {};   // zero-init: fields we don't set stay at spec defaults
    // Compute workgroup: shaders prefer their original 1000/512-invocation
    // shapes; they can reshape via override constants if the adapter grants
    // less, so request the adapter's own maximum rather than gating hard.
    required.maxComputeInvocationsPerWorkgroup = supported.maxComputeInvocationsPerWorkgroup;
    required.maxComputeWorkgroupSizeX = supported.maxComputeWorkgroupSizeX;
    required.maxComputeWorkgroupSizeY = supported.maxComputeWorkgroupSizeY;
    required.maxComputeWorkgroupSizeZ = supported.maxComputeWorkgroupSizeZ;
    // Storage/readback: VAC-scale grids need multi-GB buffers (M5 export
    // readback of a 712x1200x728 r32float trace is ~2.5 GB).
    required.maxBufferSize = supported.maxBufferSize;
    required.maxStorageBufferBindingSize = supported.maxStorageBufferBindingSize;
    required.maxTextureDimension3D = supported.maxTextureDimension3D;

    // Hard floors: below these the sim cannot run at all; fatal NAMES the limit.
    if (supported.maxComputeInvocationsPerWorkgroup < 256)
        fatal("adapter limit too low: maxComputeInvocationsPerWorkgroup < 256");
    if (supported.maxTextureDimension3D < 1024)
        fatal("adapter limit too low: maxTextureDimension3D < 1024 (grid resolution)");

    wgpu::FeatureName required_features[] = { wgpu::FeatureName::Float32Filterable };
    wgpu::DeviceDescriptor dev_desc = {};
    dev_desc.requiredFeatures = required_features;
    dev_desc.requiredFeatureCount = 1;
    dev_desc.requiredLimits = &required;
    dev_desc.SetUncapturedErrorCallback(
        [](const wgpu::Device &, wgpu::ErrorType type, wgpu::StringView msg) {
            logging::print("[gpu] uncaptured error (%d): %.*s",
                         (int)type, (int)msg.length, msg.data);
        });
    dev_desc.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device &, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
            logging::print("[gpu] device lost (%d): %.*s",
                         (int)reason, (int)msg.length, msg.data);
        });

    done = false;
    ctx->adapter.RequestDevice(
        &dev_desc, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView msg) {
            if (status != wgpu::RequestDeviceStatus::Success)
                fatal("RequestDevice failed");
            ctx->device = device;
            done = true;
        });
    while (!done) ctx->instance.ProcessEvents();
    ctx->queue = ctx->device.GetQueue();

    // Store granted limit values for Task 3 and later tasks
    wgpu::Limits granted = {};
    ctx->device.GetLimits(&granted);
    ctx->max_workgroup_invocations = granted.maxComputeInvocationsPerWorkgroup;
    ctx->max_storage_buffer_binding_size = granted.maxStorageBufferBindingSize;
    ctx->max_buffer_size = granted.maxBufferSize;

    // Log granted limits for debugging and Task 2b planning
    logging::print("[gpu] limits: workgroup_invocations=%u storage_binding=%llu MiB buffer=%llu MiB",
            ctx->max_workgroup_invocations,
            (unsigned long long)(ctx->max_storage_buffer_binding_size >> 20),
            (unsigned long long)(ctx->max_buffer_size >> 20));

    return true;
}

bool init_surface(GpuContext *ctx, Window *window) {
    ctx->surface = wgpu::glfw::CreateSurfaceForWindow(ctx->instance, window->window_handle);
    if (!ctx->surface) fatal("CreateSurfaceForWindow failed");
    // Use the framebuffer size, not the logical window size: on Retina
    // displays the framebuffer is 2x logical, and configuring the surface
    // at the logical size leaves the swapchain at half resolution.
    int fb_w, fb_h;
    glfwGetFramebufferSize(window->window_handle, &fb_w, &fb_h);
    ctx->width = fb_w;
    ctx->height = fb_h;

    static wgpu::TextureFormat srgb = wgpu::TextureFormat::BGRA8UnormSrgb;
    wgpu::SurfaceConfiguration config = {};
    config.device = ctx->device;
    config.format = ctx->surface_format;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.width = ctx->width;
    config.height = ctx->height;
    config.presentMode = wgpu::PresentMode::Fifo;  // = the original's Present(1,0) vsync
    config.viewFormatCount = 1;
    config.viewFormats = &srgb;
    ctx->surface.Configure(&config);
    return true;
}

GpuContext init(Window *window) {
    GpuContext ctx;
    init_device(&ctx);
    init_surface(&ctx, window);
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
