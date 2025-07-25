/*
 * Copyright 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GraphiteGpuContext.h"

#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTraceMemoryDump.h>
#include <include/gpu/graphite/GraphiteTypes.h>
#include <include/gpu/graphite/Surface.h>
#include <include/gpu/graphite/vk/VulkanGraphiteUtils.h>

#include "GpuTypes.h"
#include "skia/compat/GraphiteBackendTexture.h"

#include <android-base/macros.h>
#include <log/log_main.h>
#include <memory>

namespace android::renderengine::skia {

namespace {
static skgpu::graphite::ContextOptions graphiteOptions() {
    skgpu::graphite::ContextOptions options;
    options.fDisableDriverCorrectnessWorkarounds = true;
    return options;
}
} // namespace

std::unique_ptr<SkiaGpuContext> SkiaGpuContext::MakeVulkan_Graphite(
        const skgpu::VulkanBackendContext& vulkanBackendContext) {
    return std::make_unique<GraphiteGpuContext>(
            skgpu::graphite::ContextFactory::MakeVulkan(vulkanBackendContext, graphiteOptions()));
}

GraphiteGpuContext::GraphiteGpuContext(std::unique_ptr<skgpu::graphite::Context> context)
      : mContext(std::move(context)) {
    LOG_ALWAYS_FATAL_IF(mContext.get() == nullptr, "graphite::Context creation failed");
    LOG_ALWAYS_FATAL_IF(mContext->backend() != skgpu::BackendApi::kVulkan,
                        "graphite::Context::backend() == %d, but GraphiteBackendContext makes "
                        "assumptions that are only valid for Vulkan (%d)",
                        static_cast<int>(mContext->backend()),
                        static_cast<int>(skgpu::BackendApi::kVulkan));

    // TODO: b/293371537 - Iterate on default cache limits (the Recorder should have the majority of
    // the budget, and the Context should be given a smaller fraction.)
    skgpu::graphite::RecorderOptions recorderOptions = skgpu::graphite::RecorderOptions();
    this->mRecorder = mContext->makeRecorder(recorderOptions);
    LOG_ALWAYS_FATAL_IF(mRecorder.get() == nullptr, "graphite::Recorder creation failed");
}

GraphiteGpuContext::~GraphiteGpuContext() {
    // The equivalent operation would occur when destroying the graphite::Context, but calling this
    // explicitly allows any outstanding GraphiteBackendTextures to be released, thus allowing us to
    // assert that this GraphiteGpuContext holds the last ref to the underlying graphite::Recorder.
    mContext->submit(skgpu::graphite::SyncToCpu::kYes);
    // We must call the Context's and Recorder's dtors before exiting this function, so all other
    // refs must be released by now. Note: these assertions may be unreliable in a hypothetical
    // future world where we take advantage of Graphite's multi-threading capabilities!
    LOG_ALWAYS_FATAL_IF(mRecorder.use_count() > 1,
                        "Something other than GraphiteGpuContext holds a ref to the underlying "
                        "graphite::Recorder");
    LOG_ALWAYS_FATAL_IF(mContext.use_count() > 1,
                        "Something other than GraphiteGpuContext holds a ref to the underlying "
                        "graphite::Context");
};

std::shared_ptr<skgpu::graphite::Context> GraphiteGpuContext::graphiteContext() {
    return mContext;
}

std::shared_ptr<skgpu::graphite::Recorder> GraphiteGpuContext::graphiteRecorder() {
    return mRecorder;
}

std::unique_ptr<SkiaBackendTexture> GraphiteGpuContext::makeBackendTexture(AHardwareBuffer* buffer,
                                                                           bool isOutputBuffer) {
    return std::make_unique<GraphiteBackendTexture>(graphiteRecorder(), buffer, isOutputBuffer);
}

sk_sp<SkSurface> GraphiteGpuContext::createRenderTarget(SkImageInfo imageInfo) {
    constexpr SkSurfaceProps* kProps = nullptr;
    return SkSurfaces::RenderTarget(mRecorder.get(), imageInfo, skgpu::Mipmapped::kNo, kProps);
}

size_t GraphiteGpuContext::getMaxRenderTargetSize() const {
    // maxRenderTargetSize only differs from maxTextureSize on GL, so as long as Graphite implies
    // Vk, then the distinction is irrelevant.
    return getMaxTextureSize();
};

size_t GraphiteGpuContext::getMaxTextureSize() const {
    return mContext->maxTextureSize();
};

bool GraphiteGpuContext::isAbandonedOrDeviceLost() {
    return mContext->isDeviceLost();
}

void GraphiteGpuContext::setResourceCacheLimit(size_t maxResourceBytes) {
    // Graphite has a separate budget for its Context and its Recorder. For now the majority of
    // memory that Graphite will allocate will be on the Recorder and minimal amount on the Context.
    // The main allocations on the Context are MSAA buffers (not often, if ever used in
    // RenderEngine) and stencil buffers. However, both of these should be "memoryless" in Vulkan on
    // tiled GPUs, so they don't actually use GPU memory. However, in Vulkan there are scenarios
    // where Vulkan could end up using real memory for them. Skia will regularly query the device to
    // get the real memory usage and update the budgeted appropriately. Though for all real usage
    // patterns we don't expect to ever trigger the device to allocate real memory.
    //
    // Therefore, we set the full maxResourceBytes budget on the Recorder. However, in the rare
    // chance that the devcies does allocate real memory we don't want to immediately kill device
    // performance by constantly trashing allocations on the Context. Thus we set the Context's
    // budget to be 50% of the total budget to make sure we allow the MSAA or Stencil buffers to be
    // allocated in Skia and not immediately discarded. But even with this extra 50% budget, as
    // described above, this shouldn't result in actual GPU memory usage.
    //
    // TODO: We will need to revise this strategy for GLES which does not have the same memoryless
    // textures.
    // TODO: Work in Graphite has started to move a lot more of its scratch resources to be owned
    // by the Context and not on Recorders. This will mean most memory is actually owned by the
    // Context and thus the budgeting here will need to be updated.
    mContext->setMaxBudgetedBytes(maxResourceBytes / 2);
    mRecorder->setMaxBudgetedBytes(maxResourceBytes);
}

void GraphiteGpuContext::purgeUnlockedScratchResources() {
    mContext->freeGpuResources();
    mRecorder->freeGpuResources();
}

void GraphiteGpuContext::dumpMemoryStatistics(SkTraceMemoryDump* traceMemoryDump) const {
    mContext->dumpMemoryStatistics(traceMemoryDump);
    mRecorder->dumpMemoryStatistics(traceMemoryDump);
}

} // namespace android::renderengine::skia
