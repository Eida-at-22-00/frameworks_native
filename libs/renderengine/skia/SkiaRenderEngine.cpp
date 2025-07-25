/*
 * Copyright 2020 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "RenderEngine"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "SkiaRenderEngine.h"

#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkColor.h>
#include <SkColorFilter.h>
#include <SkColorMatrix.h>
#include <SkColorSpace.h>
#include <SkData.h>
#include <SkGraphics.h>
#include <SkImage.h>
#include <SkImageFilters.h>
#include <SkImageInfo.h>
#include <SkM44.h>
#include <SkMatrix.h>
#include <SkPaint.h>
#include <SkPath.h>
#include <SkPoint.h>
#include <SkPoint3.h>
#include <SkRRect.h>
#include <SkRect.h>
#include <SkRefCnt.h>
#include <SkRegion.h>
#include <SkRuntimeEffect.h>
#include <SkSamplingOptions.h>
#include <SkScalar.h>
#include <SkShader.h>
#include <SkShadowUtils.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <android-base/stringprintf.h>
#include <common/FlagManager.h>
#include <common/trace.h>
#include <gui/FenceMonitor.h>
#include <include/gpu/ganesh/GrBackendSemaphore.h>
#include <include/gpu/ganesh/GrContextOptions.h>
#include <include/gpu/ganesh/GrTypes.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <pthread.h>
#include <src/core/SkTraceEventCommon.h>
#include <sync/sync.h>
#include <ui/BlurRegion.h>
#include <ui/DebugUtils.h>
#include <ui/GraphicBuffer.h>
#include <ui/HdrRenderTypeUtils.h>

#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <numeric>

#include "Cache.h"
#include "ColorSpaces.h"
#include "compat/SkiaGpuContext.h"
#include "filters/BlurFilter.h"
#include "filters/GainmapFactory.h"
#include "filters/GaussianBlurFilter.h"
#include "filters/KawaseBlurDualFilter.h"
#include "filters/KawaseBlurFilter.h"
#include "filters/LinearEffect.h"
#include "filters/MouriMap.h"
#include "log/log_main.h"
#include "skia/compat/SkiaBackendTexture.h"
#include "skia/debug/SkiaCapture.h"
#include "skia/debug/SkiaMemoryReporter.h"
#include "skia/filters/StretchShaderFactory.h"
#include "system/graphics-base-v1.0.h"

namespace {

// Debugging settings
static const bool kPrintLayerSettings = false;
static const bool kGaneshFlushAfterEveryLayer = kPrintLayerSettings;

} // namespace

// Utility functions related to SkRect

namespace {

static inline SkRect getSkRect(const android::FloatRect& rect) {
    return SkRect::MakeLTRB(rect.left, rect.top, rect.right, rect.bottom);
}

static inline SkRect getSkRect(const android::Rect& rect) {
    return SkRect::MakeLTRB(rect.left, rect.top, rect.right, rect.bottom);
}

/**
 *  Verifies that common, simple bounds + clip combinations can be converted into
 *  a single RRect draw call returning true if possible. If true the radii parameter
 *  will be filled with the correct radii values that combined with bounds param will
 *  produce the insected roundRect. If false, the returned state of the radii param is undefined.
 */
static bool intersectionIsRoundRect(const SkRect& bounds, const SkRect& crop,
                                    const SkRect& insetCrop, const android::vec2& cornerRadius,
                                    SkVector radii[4]) {
    const bool leftEqual = bounds.fLeft == crop.fLeft;
    const bool topEqual = bounds.fTop == crop.fTop;
    const bool rightEqual = bounds.fRight == crop.fRight;
    const bool bottomEqual = bounds.fBottom == crop.fBottom;

    // In the event that the corners of the bounds only partially align with the crop we
    // need to ensure that the resulting shape can still be represented as a round rect.
    // In particular the round rect implementation will scale the value of all corner radii
    // if the sum of the radius along any edge is greater than the length of that edge.
    // See https://www.w3.org/TR/css-backgrounds-3/#corner-overlap
    const bool requiredWidth = bounds.width() > (cornerRadius.x * 2);
    const bool requiredHeight = bounds.height() > (cornerRadius.y * 2);
    if (!requiredWidth || !requiredHeight) {
        return false;
    }

    // Check each cropped corner to ensure that it exactly matches the crop or its corner is
    // contained within the cropped shape and does not need rounded.
    // compute the UpperLeft corner radius
    if (leftEqual && topEqual) {
        radii[0].set(cornerRadius.x, cornerRadius.y);
    } else if ((leftEqual && bounds.fTop >= insetCrop.fTop) ||
               (topEqual && bounds.fLeft >= insetCrop.fLeft)) {
        radii[0].set(0, 0);
    } else {
        return false;
    }
    // compute the UpperRight corner radius
    if (rightEqual && topEqual) {
        radii[1].set(cornerRadius.x, cornerRadius.y);
    } else if ((rightEqual && bounds.fTop >= insetCrop.fTop) ||
               (topEqual && bounds.fRight <= insetCrop.fRight)) {
        radii[1].set(0, 0);
    } else {
        return false;
    }
    // compute the BottomRight corner radius
    if (rightEqual && bottomEqual) {
        radii[2].set(cornerRadius.x, cornerRadius.y);
    } else if ((rightEqual && bounds.fBottom <= insetCrop.fBottom) ||
               (bottomEqual && bounds.fRight <= insetCrop.fRight)) {
        radii[2].set(0, 0);
    } else {
        return false;
    }
    // compute the BottomLeft corner radius
    if (leftEqual && bottomEqual) {
        radii[3].set(cornerRadius.x, cornerRadius.y);
    } else if ((leftEqual && bounds.fBottom <= insetCrop.fBottom) ||
               (bottomEqual && bounds.fLeft >= insetCrop.fLeft)) {
        radii[3].set(0, 0);
    } else {
        return false;
    }

    return true;
}

static inline std::pair<SkRRect, SkRRect> getBoundsAndClip(const android::FloatRect& boundsRect,
                                                           const android::FloatRect& cropRect,
                                                           const android::vec2& cornerRadius) {
    const SkRect bounds = getSkRect(boundsRect);
    const SkRect crop = getSkRect(cropRect);

    SkRRect clip;
    if (cornerRadius.x > 0 && cornerRadius.y > 0) {
        // it the crop and the bounds are equivalent or there is no crop then we don't need a clip
        if (bounds == crop || crop.isEmpty()) {
            return {SkRRect::MakeRectXY(bounds, cornerRadius.x, cornerRadius.y), clip};
        }

        // This makes an effort to speed up common, simple bounds + clip combinations by
        // converting them to a single RRect draw. It is possible there are other cases
        // that can be converted.
        if (crop.contains(bounds)) {
            const auto insetCrop = crop.makeInset(cornerRadius.x, cornerRadius.y);
            if (insetCrop.contains(bounds)) {
                return {SkRRect::MakeRect(bounds), clip}; // clip is empty - no rounding required
            }

            SkVector radii[4];
            if (intersectionIsRoundRect(bounds, crop, insetCrop, cornerRadius, radii)) {
                SkRRect intersectionBounds;
                intersectionBounds.setRectRadii(bounds, radii);
                return {intersectionBounds, clip};
            }
        }

        // we didn't hit any of our fast paths so set the clip to the cropRect
        clip.setRectXY(crop, cornerRadius.x, cornerRadius.y);
    }

    // if we hit this point then we either don't have rounded corners or we are going to rely
    // on the clip to round the corners for us
    return {SkRRect::MakeRect(bounds), clip};
}

static inline bool layerHasBlur(const android::renderengine::LayerSettings& layer,
                                bool colorTransformModifiesAlpha) {
    if (layer.backgroundBlurRadius > 0 || layer.blurRegions.size()) {
        // return false if the content is opaque and would therefore occlude the blur
        const bool opaqueContent = !layer.source.buffer.buffer || layer.source.buffer.isOpaque;
        const bool opaqueAlpha = layer.alpha == 1.0f && !colorTransformModifiesAlpha;
        return layer.skipContentDraw || !(opaqueContent && opaqueAlpha);
    }
    return false;
}

static inline SkColor getSkColor(const android::vec4& color) {
    return SkColorSetARGB(color.a * 255, color.r * 255, color.g * 255, color.b * 255);
}

static inline SkM44 getSkM44(const android::mat4& matrix) {
    return SkM44(matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0],
                 matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1],
                 matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2],
                 matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]);
}

static inline SkPoint3 getSkPoint3(const android::vec3& vector) {
    return SkPoint3::Make(vector.x, vector.y, vector.z);
}

} // namespace

namespace android {
namespace renderengine {
namespace skia {

namespace {
void trace(sp<Fence> fence) {
    if (SFTRACE_ENABLED()) {
        static gui::FenceMonitor sMonitor("RE Completion");
        sMonitor.queueFence(std::move(fence));
    }
}
} // namespace

using base::StringAppendF;

std::future<void> SkiaRenderEngine::primeCache(PrimeCacheConfig config) {
    Cache::primeShaderCache(this, config);
    return {};
}

sk_sp<SkData> SkiaRenderEngine::SkSLCacheMonitor::load(const SkData& key) {
    // This "cache" does not actually cache anything. It just allows us to
    // monitor Skia's internal cache. So this method always returns null.
    return nullptr;
}

void SkiaRenderEngine::SkSLCacheMonitor::store(const SkData& key, const SkData& data,
                                               const SkString& description) {
    mShadersCachedSinceLastCall++;
    mTotalShadersCompiled++;
    SFTRACE_FORMAT("SF cache: %i shaders", mTotalShadersCompiled);
}

int SkiaRenderEngine::reportShadersCompiled() {
    return mSkSLCacheMonitor.totalShadersCompiled();
}

void SkiaRenderEngine::setEnableTracing(bool tracingEnabled) {
    SkAndroidFrameworkTraceUtil::setEnableTracing(tracingEnabled);
}

SkiaRenderEngine::SkiaRenderEngine(Threaded threaded, PixelFormat pixelFormat,
                                   BlurAlgorithm blurAlgorithm)
      : RenderEngine(threaded), mDefaultPixelFormat(pixelFormat) {
    switch (blurAlgorithm) {
        case BlurAlgorithm::GAUSSIAN: {
            ALOGD("Background Blurs Enabled (Gaussian algorithm)");
            mBlurFilter = new GaussianBlurFilter();
            break;
        }
        case BlurAlgorithm::KAWASE: {
            ALOGD("Background Blurs Enabled (Kawase algorithm)");
            mBlurFilter = new KawaseBlurFilter();
            break;
        }
        case BlurAlgorithm::KAWASE_DUAL_FILTER: {
            ALOGD("Background Blurs Enabled (Kawase dual-filtering algorithm)");
            mBlurFilter = new KawaseBlurDualFilter();
            break;
        }
        default: {
            mBlurFilter = nullptr;
            break;
        }
    }

    mCapture = std::make_unique<SkiaCapture>();
}

SkiaRenderEngine::~SkiaRenderEngine() { }

// To be called from backend dtors. Used to clean up Skia objects before GPU API contexts are
// destroyed by subclasses.
void SkiaRenderEngine::finishRenderingAndAbandonContexts() {
    std::lock_guard<std::mutex> lock(mRenderingMutex);

    if (mBlurFilter) {
        delete mBlurFilter;
    }

    // Leftover textures may hold refs to backend-specific Skia contexts, which must be released
    // before ~SkiaGpuContext is called.
    mTextureCleanupMgr.setDeferredStatus(false);
    mTextureCleanupMgr.cleanup();

    // ~SkiaGpuContext must be called before GPU API contexts are torn down.
    mContext.reset();
    mProtectedContext.reset();
}

void SkiaRenderEngine::useProtectedContext(bool useProtectedContext) {
    if (useProtectedContext == mInProtectedContext ||
        (useProtectedContext && !supportsProtectedContent())) {
        return;
    }

    // release any scratch resources before switching into a new mode
    if (getActiveContext()) {
        getActiveContext()->purgeUnlockedScratchResources();
    }

    // Backend-specific way to switch to protected context
    if (useProtectedContextImpl(
            useProtectedContext ? GrProtected::kYes : GrProtected::kNo)) {
        mInProtectedContext = useProtectedContext;
        SFTRACE_INT("RE inProtectedContext", mInProtectedContext);
        // given that we are sharing the same thread between two contexts we need to
        // make sure that the thread state is reset when switching between the two.
        if (getActiveContext()) {
            getActiveContext()->resetContextIfApplicable();
        }
    }
}

SkiaGpuContext* SkiaRenderEngine::getActiveContext() {
    return mInProtectedContext ? mProtectedContext.get() : mContext.get();
}

static float toDegrees(uint32_t transform) {
    switch (transform) {
        case ui::Transform::ROT_90:
            return 90.0;
        case ui::Transform::ROT_180:
            return 180.0;
        case ui::Transform::ROT_270:
            return 270.0;
        default:
            return 0.0;
    }
}

static SkColorMatrix toSkColorMatrix(const android::mat4& matrix) {
    return SkColorMatrix(matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0], 0, matrix[0][1],
                         matrix[1][1], matrix[2][1], matrix[3][1], 0, matrix[0][2], matrix[1][2],
                         matrix[2][2], matrix[3][2], 0, matrix[0][3], matrix[1][3], matrix[2][3],
                         matrix[3][3], 0);
}

static bool needsToneMapping(ui::Dataspace sourceDataspace, ui::Dataspace destinationDataspace) {
    int64_t sourceTransfer = sourceDataspace & HAL_DATASPACE_TRANSFER_MASK;
    int64_t destTransfer = destinationDataspace & HAL_DATASPACE_TRANSFER_MASK;

    // Treat unsupported dataspaces as srgb
    if (destTransfer != HAL_DATASPACE_TRANSFER_LINEAR &&
        destTransfer != HAL_DATASPACE_TRANSFER_HLG &&
        destTransfer != HAL_DATASPACE_TRANSFER_ST2084) {
        destTransfer = HAL_DATASPACE_TRANSFER_SRGB;
    }

    if (sourceTransfer != HAL_DATASPACE_TRANSFER_LINEAR &&
        sourceTransfer != HAL_DATASPACE_TRANSFER_HLG &&
        sourceTransfer != HAL_DATASPACE_TRANSFER_ST2084) {
        sourceTransfer = HAL_DATASPACE_TRANSFER_SRGB;
    }

    const bool isSourceLinear = sourceTransfer == HAL_DATASPACE_TRANSFER_LINEAR;
    const bool isSourceSRGB = sourceTransfer == HAL_DATASPACE_TRANSFER_SRGB;
    const bool isDestLinear = destTransfer == HAL_DATASPACE_TRANSFER_LINEAR;
    const bool isDestSRGB = destTransfer == HAL_DATASPACE_TRANSFER_SRGB;

    return !(isSourceLinear && isDestSRGB) && !(isSourceSRGB && isDestLinear) &&
            sourceTransfer != destTransfer;
}

void SkiaRenderEngine::ensureContextsCreated() {
    if (mContext) {
        return;
    }

    std::tie(mContext, mProtectedContext) = createContexts();
}

void SkiaRenderEngine::mapExternalTextureBuffer(const sp<GraphicBuffer>& buffer,
                                                  bool isRenderable) {
    // Only run this if RE is running on its own thread. This
    // way the access to GL/VK operations is guaranteed to be happening on the
    // same thread.
    if (!isThreaded()) {
        return;
    }
    // We don't attempt to map a buffer if the buffer contains protected content. In GL this is
    // important because GPU resources for protected buffers are much more limited. (In Vk we
    // simply match the existing behavior for protected buffers.)  We also never cache any
    // buffers while in a protected context.
    const bool isProtectedBuffer = buffer->getUsage() & GRALLOC_USAGE_PROTECTED;
    // Don't attempt to map buffers if we're not gpu sampleable. Callers shouldn't send a buffer
    // over to RenderEngine.
    const bool isGpuSampleable = buffer->getUsage() & GRALLOC_USAGE_HW_TEXTURE;
    if (isProtectedBuffer || isProtected() || !isGpuSampleable) {
        return;
    }
    SFTRACE_CALL();

    // If we were to support caching protected buffers then we will need to switch the
    // currently bound context if we are not already using the protected context (and subsequently
    // switch back after the buffer is cached).
    auto context = getActiveContext();
    auto& cache = mTextureCache;

    std::lock_guard<std::mutex> lock(mRenderingMutex);
    mGraphicBufferExternalRefs[buffer->getId()]++;

    if (const auto& iter = cache.find(buffer->getId()); iter == cache.end()) {
        if (FlagManager::getInstance().renderable_buffer_usage()) {
            isRenderable = buffer->getUsage() & GRALLOC_USAGE_HW_RENDER;
        }
        std::unique_ptr<SkiaBackendTexture> backendTexture =
                context->makeBackendTexture(buffer->toAHardwareBuffer(), isRenderable);
        auto imageTextureRef =
                std::make_shared<AutoBackendTexture::LocalRef>(std::move(backendTexture),
                                                               mTextureCleanupMgr);
        cache.insert({buffer->getId(), imageTextureRef});
    }
}

void SkiaRenderEngine::unmapExternalTextureBuffer(sp<GraphicBuffer>&& buffer) {
    SFTRACE_CALL();
    std::lock_guard<std::mutex> lock(mRenderingMutex);
    if (const auto& iter = mGraphicBufferExternalRefs.find(buffer->getId());
        iter != mGraphicBufferExternalRefs.end()) {
        if (iter->second == 0) {
            ALOGW("Attempted to unmap GraphicBuffer <id: %" PRId64
                  "> from RenderEngine texture, but the "
                  "ref count was already zero!",
                  buffer->getId());
            mGraphicBufferExternalRefs.erase(buffer->getId());
            return;
        }

        iter->second--;

        // Swap contexts if needed prior to deleting this buffer
        // See Issue 1 of
        // https://www.khronos.org/registry/EGL/extensions/EXT/EGL_EXT_protected_content.txt: even
        // when a protected context and an unprotected context are part of the same share group,
        // protected surfaces may not be accessed by an unprotected context, implying that protected
        // surfaces may only be freed when a protected context is active.
        const bool inProtected = mInProtectedContext;
        useProtectedContext(buffer->getUsage() & GRALLOC_USAGE_PROTECTED);

        if (iter->second == 0) {
            mTextureCache.erase(buffer->getId());
            mGraphicBufferExternalRefs.erase(buffer->getId());
        }

        // Swap back to the previous context so that cached values of isProtected in SurfaceFlinger
        // are up-to-date.
        if (inProtected != mInProtectedContext) {
            useProtectedContext(inProtected);
        }
    }
}

std::shared_ptr<AutoBackendTexture::LocalRef> SkiaRenderEngine::getOrCreateBackendTexture(
        const sp<GraphicBuffer>& buffer, bool isOutputBuffer) {
    // Do not lookup the buffer in the cache for protected contexts
    if (!isProtected()) {
        if (const auto& it = mTextureCache.find(buffer->getId()); it != mTextureCache.end()) {
            return it->second;
        }
    }
    std::unique_ptr<SkiaBackendTexture> backendTexture =
            getActiveContext()->makeBackendTexture(buffer->toAHardwareBuffer(), isOutputBuffer);
    return std::make_shared<AutoBackendTexture::LocalRef>(std::move(backendTexture),
                                                          mTextureCleanupMgr);
}

bool SkiaRenderEngine::canSkipPostRenderCleanup() const {
    std::lock_guard<std::mutex> lock(mRenderingMutex);
    return mTextureCleanupMgr.isEmpty();
}

void SkiaRenderEngine::cleanupPostRender() {
    SFTRACE_CALL();
    std::lock_guard<std::mutex> lock(mRenderingMutex);
    mTextureCleanupMgr.cleanup();
}

sk_sp<SkShader> SkiaRenderEngine::createRuntimeEffectShader(
        const RuntimeEffectShaderParameters& parameters) {
    // The given surface will be stretched by HWUI via matrix transformation
    // which gets similar results for most surfaces
    // Determine later on if we need to leverage the stretch shader within
    // surface flinger
    const auto& stretchEffect = parameters.layer.stretchEffect;
    const auto& targetBuffer = parameters.layer.source.buffer.buffer;
    const auto graphicBuffer = targetBuffer ? targetBuffer->getBuffer() : nullptr;

    auto shader = parameters.shader;
    if (graphicBuffer && parameters.shader) {
        if (stretchEffect.hasEffect()) {
            shader = mStretchShaderFactory.createSkShader(shader, stretchEffect);
        }
        // The given surface requires to be filled outside of its buffer bounds if the edge
        // extension is required
        const auto& edgeExtensionEffect = parameters.layer.edgeExtensionEffect;
        if (edgeExtensionEffect.hasEffect()) {
            shader = mEdgeExtensionShaderFactory.createSkShader(shader, parameters.layer,
                                                                parameters.imageBounds);
        }
    }

    if (graphicBuffer && parameters.layer.luts) {
        const bool dimInLinearSpace = parameters.display.dimmingStage !=
                aidl::android::hardware::graphics::composer3::DimmingStage::GAMMA_OETF;
        const ui::Dataspace runtimeEffectDataspace = !dimInLinearSpace
                ? static_cast<ui::Dataspace>(
                          (parameters.outputDataSpace & ui::Dataspace::STANDARD_MASK) |
                          ui::Dataspace::TRANSFER_GAMMA2_2 |
                          (parameters.outputDataSpace & ui::Dataspace::RANGE_MASK))
                : parameters.outputDataSpace;

        shader = mLutShader.lutShader(shader, parameters.layer.luts,
                                      parameters.layer.sourceDataspace,
                                      toSkColorSpace(runtimeEffectDataspace));
    }

    if (parameters.requiresLinearEffect) {
        const auto format = targetBuffer != nullptr
                ? std::optional<ui::PixelFormat>(
                          static_cast<ui::PixelFormat>(targetBuffer->getPixelFormat()))
                : std::nullopt;

        const auto hdrType = getHdrRenderType(parameters.layer.sourceDataspace, format,
                                              parameters.layerDimmingRatio);

        const auto usingLocalTonemap =
                parameters.display.tonemapStrategy == DisplaySettings::TonemapStrategy::Local &&
                hdrType != HdrRenderType::SDR &&
                shader->isAImage((SkMatrix*)nullptr, (SkTileMode*)nullptr) &&
                (hdrType != HdrRenderType::DISPLAY_HDR ||
                 parameters.display.targetHdrSdrRatio < parameters.layerDimmingRatio);
        if (usingLocalTonemap) {
            const float inputRatio =
                    hdrType == HdrRenderType::GENERIC_HDR ? 1.0f : parameters.layerDimmingRatio;
            shader = localTonemap(shader, inputRatio, parameters.display.targetHdrSdrRatio);
        }

        // disable tonemapping if we already locally tonemapped
        // skip tonemapping if the luts is in use
        auto inputDataspace = usingLocalTonemap || (graphicBuffer && parameters.layer.luts)
                ? parameters.outputDataSpace
                : parameters.layer.sourceDataspace;
        auto effect =
                shaders::LinearEffect{.inputDataspace = inputDataspace,
                                      .outputDataspace = parameters.outputDataSpace,
                                      .undoPremultipliedAlpha = parameters.undoPremultipliedAlpha,
                                      .fakeOutputDataspace = parameters.fakeOutputDataspace};

        auto effectIter = mRuntimeEffects.find(effect);
        sk_sp<SkRuntimeEffect> runtimeEffect = nullptr;
        if (effectIter == mRuntimeEffects.end()) {
            runtimeEffect = buildRuntimeEffect(effect);
            mRuntimeEffects.insert({effect, runtimeEffect});
        } else {
            runtimeEffect = effectIter->second;
        }

        mat4 colorTransform = parameters.layer.colorTransform;

        if (!usingLocalTonemap) {
            colorTransform *=
                    mat4::scale(vec4(parameters.layerDimmingRatio, parameters.layerDimmingRatio,
                                     parameters.layerDimmingRatio, 1.f));
        }

        const auto hardwareBuffer = graphicBuffer ? graphicBuffer->toAHardwareBuffer() : nullptr;
        return createLinearEffectShader(shader, effect, runtimeEffect, std::move(colorTransform),
                                        parameters.display.maxLuminance,
                                        parameters.display.currentLuminanceNits,
                                        parameters.layer.source.buffer.maxLuminanceNits,
                                        hardwareBuffer, parameters.display.renderIntent);
    }
    return shader;
}

sk_sp<SkShader> SkiaRenderEngine::localTonemap(sk_sp<SkShader> shader, float inputMultiplier,
                                               float targetHdrSdrRatio) {
    static MouriMap kMapper;
    return kMapper.mouriMap(getActiveContext(), shader, inputMultiplier, targetHdrSdrRatio);
}

void SkiaRenderEngine::initCanvas(SkCanvas* canvas, const DisplaySettings& display) {
    if (CC_UNLIKELY(mCapture->isCaptureRunning())) {
        // Record display settings when capture is running.
        std::stringstream displaySettings;
        PrintTo(display, &displaySettings);
        // Store the DisplaySettings in additional information.
        canvas->drawAnnotation(SkRect::MakeEmpty(), "DisplaySettings",
                               SkData::MakeWithCString(displaySettings.str().c_str()));
    }

    // Before doing any drawing, let's make sure that we'll start at the origin of the display.
    // Some displays don't start at 0,0 for example when we're mirroring the screen. Also, virtual
    // displays might have different scaling when compared to the physical screen.

    canvas->clipRect(getSkRect(display.physicalDisplay));
    canvas->translate(display.physicalDisplay.left, display.physicalDisplay.top);

    const auto clipWidth = display.clip.width();
    const auto clipHeight = display.clip.height();
    auto rotatedClipWidth = clipWidth;
    auto rotatedClipHeight = clipHeight;
    // Scale is contingent on the rotation result.
    if (display.orientation & ui::Transform::ROT_90) {
        std::swap(rotatedClipWidth, rotatedClipHeight);
    }
    const auto scaleX = static_cast<SkScalar>(display.physicalDisplay.width()) /
            static_cast<SkScalar>(rotatedClipWidth);
    const auto scaleY = static_cast<SkScalar>(display.physicalDisplay.height()) /
            static_cast<SkScalar>(rotatedClipHeight);
    canvas->scale(scaleX, scaleY);

    // Canvas rotation is done by centering the clip window at the origin, rotating, translating
    // back so that the top left corner of the clip is at (0, 0).
    canvas->translate(rotatedClipWidth / 2, rotatedClipHeight / 2);
    canvas->rotate(toDegrees(display.orientation));
    canvas->translate(-clipWidth / 2, -clipHeight / 2);
    canvas->translate(-display.clip.left, -display.clip.top);
}

class AutoSaveRestore {
public:
    AutoSaveRestore(SkCanvas* canvas) : mCanvas(canvas) { mSaveCount = canvas->save(); }
    ~AutoSaveRestore() { restore(); }
    void replace(SkCanvas* canvas) {
        mCanvas = canvas;
        mSaveCount = canvas->save();
    }
    void restore() {
        if (mCanvas) {
            mCanvas->restoreToCount(mSaveCount);
            mCanvas = nullptr;
        }
    }

private:
    SkCanvas* mCanvas;
    int mSaveCount;
};

static SkRRect getBlurRRect(const BlurRegion& region) {
    const auto rect = SkRect::MakeLTRB(region.left, region.top, region.right, region.bottom);
    const SkVector radii[4] = {SkVector::Make(region.cornerRadiusTL, region.cornerRadiusTL),
                               SkVector::Make(region.cornerRadiusTR, region.cornerRadiusTR),
                               SkVector::Make(region.cornerRadiusBR, region.cornerRadiusBR),
                               SkVector::Make(region.cornerRadiusBL, region.cornerRadiusBL)};
    SkRRect roundedRect;
    roundedRect.setRectRadii(rect, radii);
    return roundedRect;
}

// Arbitrary default margin which should be close enough to zero.
constexpr float kDefaultMargin = 0.0001f;
static bool equalsWithinMargin(float expected, float value, float margin = kDefaultMargin) {
    LOG_ALWAYS_FATAL_IF(margin < 0.f, "Margin is negative!");
    return std::abs(expected - value) < margin;
}

namespace {
template <typename T>
void logSettings(const T& t) {
    std::stringstream stream;
    PrintTo(t, &stream);
    auto string = stream.str();
    size_t pos = 0;
    // Perfetto ignores \n, so split up manually into separate ALOGD statements.
    const size_t size = string.size();
    while (pos < size) {
        const size_t end = std::min(string.find("\n", pos), size);
        ALOGD("%s", string.substr(pos, end - pos).c_str());
        pos = end + 1;
    }
}
} // namespace

// Helper class intended to be used on the stack to ensure that texture cleanup
// is deferred until after this class goes out of scope.
class DeferTextureCleanup final {
public:
    DeferTextureCleanup(AutoBackendTexture::CleanupManager& mgr) : mMgr(mgr) {
        mMgr.setDeferredStatus(true);
    }
    ~DeferTextureCleanup() { mMgr.setDeferredStatus(false); }

private:
    DISALLOW_COPY_AND_ASSIGN(DeferTextureCleanup);
    AutoBackendTexture::CleanupManager& mMgr;
};

void SkiaRenderEngine::drawLayersInternal(
        const std::shared_ptr<std::promise<FenceResult>>&& resultPromise,
        const DisplaySettings& display, const std::vector<LayerSettings>& layers,
        const std::shared_ptr<ExternalTexture>& buffer, base::unique_fd&& bufferFence) {
    SFTRACE_FORMAT("%s for %s", __func__, display.namePlusId.c_str());

    std::lock_guard<std::mutex> lock(mRenderingMutex);

    if (buffer == nullptr) {
        ALOGE("No output buffer provided. Aborting GPU composition.");
        resultPromise->set_value(base::unexpected(BAD_VALUE));
        return;
    }

    validateOutputBufferUsage(buffer->getBuffer());

    auto context = getActiveContext();
    LOG_ALWAYS_FATAL_IF(context->isAbandonedOrDeviceLost(),
                        "Context is abandoned/device lost at start of %s", __func__);

    // any AutoBackendTexture deletions will now be deferred until cleanupPostRender is called
    DeferTextureCleanup dtc(mTextureCleanupMgr);

    auto surfaceTextureRef = getOrCreateBackendTexture(buffer->getBuffer(), true);

    // wait on the buffer to be ready to use prior to using it
    waitFence(context, bufferFence);

    sk_sp<SkSurface> dstSurface = surfaceTextureRef->getOrCreateSurface(display.outputDataspace);

    SkCanvas* dstCanvas = mCapture->tryCapture(dstSurface.get());
    if (dstCanvas == nullptr) {
        ALOGE("Cannot acquire canvas from Skia.");
        resultPromise->set_value(base::unexpected(BAD_VALUE));
        return;
    }

    // setup color filter if necessary
    sk_sp<SkColorFilter> displayColorTransform;
    if (display.colorTransform != mat4() && !display.deviceHandlesColorTransform) {
        displayColorTransform = SkColorFilters::Matrix(toSkColorMatrix(display.colorTransform));
    }
    const bool ctModifiesAlpha =
            displayColorTransform && !displayColorTransform->isAlphaUnchanged();

    // Find the max layer white point to determine the max luminance of the scene...
    const float maxLayerWhitePoint = std::transform_reduce(
            layers.cbegin(), layers.cend(), 0.f,
            [](float left, float right) { return std::max(left, right); },
            [&](const auto& l) { return l.whitePointNits; });

    // ...and compute the dimming ratio if dimming is requested
    const float displayDimmingRatio = display.targetLuminanceNits > 0.f && maxLayerWhitePoint > 0.f
            ? maxLayerWhitePoint / display.targetLuminanceNits
            : 1.f;

    // Find if any layers have requested blur, we'll use that info to decide when to render to an
    // offscreen buffer and when to render to the native buffer.
    sk_sp<SkSurface> activeSurface(dstSurface);
    SkCanvas* canvas = dstCanvas;
    SkiaCapture::OffscreenState offscreenCaptureState;
    const LayerSettings* blurCompositionLayer = nullptr;
    if (mBlurFilter) {
        bool requiresCompositionLayer = false;
        for (const auto& layer : layers) {
            // if the layer doesn't have blur or it is not visible then continue
            if (!layerHasBlur(layer, ctModifiesAlpha)) {
                continue;
            }
            if (layer.backgroundBlurRadius > 0 &&
                layer.backgroundBlurRadius < mBlurFilter->getMaxCrossFadeRadius()) {
                requiresCompositionLayer = true;
            }
            for (auto region : layer.blurRegions) {
                if (region.blurRadius < mBlurFilter->getMaxCrossFadeRadius()) {
                    requiresCompositionLayer = true;
                }
            }
            if (requiresCompositionLayer) {
                activeSurface = dstSurface->makeSurface(dstSurface->imageInfo());
                canvas = mCapture->tryOffscreenCapture(activeSurface.get(), &offscreenCaptureState);
                blurCompositionLayer = &layer;
                break;
            }
        }
    }

    AutoSaveRestore surfaceAutoSaveRestore(canvas);
    // Clear the entire canvas with a transparent black to prevent ghost images.
    canvas->clear(SK_ColorTRANSPARENT);
    initCanvas(canvas, display);

    if (kPrintLayerSettings) {
        logSettings(display);
    }
    for (const auto& layer : layers) {
        SFTRACE_FORMAT("DrawLayer: %s", layer.name.c_str());

        if (kPrintLayerSettings) {
            logSettings(layer);
        }

        sk_sp<SkImage> blurInput;
        if (blurCompositionLayer == &layer) {
            LOG_ALWAYS_FATAL_IF(activeSurface == dstSurface);
            LOG_ALWAYS_FATAL_IF(canvas == dstCanvas);

            blurInput = activeSurface->makeTemporaryImage();

            // blit the offscreen framebuffer into the destination AHB. This ensures that
            // even if the blurred image does not cover the screen (for example, during
            // a rotation animation, or if blur regions are used), the entire screen is
            // initialized.
            if (layer.blurRegions.size() || FlagManager::getInstance().restore_blur_step()) {
                SkPaint paint;
                paint.setBlendMode(SkBlendMode::kSrc);
                if (CC_UNLIKELY(mCapture->isCaptureRunning())) {
                    uint64_t id = mCapture->endOffscreenCapture(&offscreenCaptureState);
                    dstCanvas->drawAnnotation(SkRect::Make(dstCanvas->imageInfo().dimensions()),
                                              String8::format("SurfaceID|%" PRId64, id).c_str(),
                                              nullptr);
                }
                dstCanvas->drawImage(blurInput, 0, 0, SkSamplingOptions(), &paint);
            }
            // assign dstCanvas to canvas and ensure that the canvas state is up to date
            canvas = dstCanvas;
            surfaceAutoSaveRestore.replace(canvas);
            initCanvas(canvas, display);

            LOG_ALWAYS_FATAL_IF(activeSurface->getCanvas()->getSaveCount() !=
                                dstSurface->getCanvas()->getSaveCount());
            LOG_ALWAYS_FATAL_IF(activeSurface->getCanvas()->getTotalMatrix() !=
                                dstSurface->getCanvas()->getTotalMatrix());

            // assign dstSurface to activeSurface
            activeSurface = dstSurface;
        }

        SkAutoCanvasRestore layerAutoSaveRestore(canvas, true);
        if (CC_UNLIKELY(mCapture->isCaptureRunning())) {
            // Record the name of the layer if the capture is running.
            std::stringstream layerSettings;
            PrintTo(layer, &layerSettings);
            // Store the LayerSettings in additional information.
            canvas->drawAnnotation(SkRect::MakeEmpty(), layer.name.c_str(),
                                   SkData::MakeWithCString(layerSettings.str().c_str()));
        }
        // Layers have a local transform that should be applied to them
        canvas->concat(getSkM44(layer.geometry.positionTransform).asM33());

        const auto [bounds, roundRectClip] =
                getBoundsAndClip(layer.geometry.boundaries, layer.geometry.roundedCornersCrop,
                                 layer.geometry.roundedCornersRadius);
        if (mBlurFilter && layerHasBlur(layer, ctModifiesAlpha)) {
            std::unordered_map<uint32_t, sk_sp<SkImage>> cachedBlurs;

            // rect to be blurred in the coordinate space of blurInput
            SkRect blurRect = canvas->getTotalMatrix().mapRect(bounds.rect());

            // Some layers may be much bigger than the screen. If we used
            // `blurRect` directly, this would allocate a large buffer with no
            // benefit. Apply the clip, which already takes the display size
            // into account. The clipped size will then be used to calculate the
            // size of the buffer we will create for blurring.
            if (!blurRect.intersect(SkRect::Make(canvas->getDeviceClipBounds()))) {
                // This should not happen, but if it did, we would use the full
                // sized layer, which should still be fine.
                ALOGW("blur bounds does not intersect display clip!");
            }

            // if the clip needs to be applied then apply it now and make sure
            // it is restored before we attempt to draw any shadows.
            SkAutoCanvasRestore acr(canvas, true);
            if (!roundRectClip.isEmpty()) {
                canvas->clipRRect(roundRectClip, true);
            }

            // TODO(b/182216890): Filter out empty layers earlier
            if (blurRect.width() > 0 && blurRect.height() > 0) {
                // if multiple layers have blur, then we need to take a snapshot now because
                // only the lowest layer will have blurImage populated earlier
                if (!blurInput) {
                    bool requiresCrossFadeWithBlurInput = false;
                    if (layer.backgroundBlurRadius > 0 &&
                        layer.backgroundBlurRadius < mBlurFilter->getMaxCrossFadeRadius()) {
                        requiresCrossFadeWithBlurInput = true;
                    }
                    for (auto region : layer.blurRegions) {
                        if (region.blurRadius < mBlurFilter->getMaxCrossFadeRadius()) {
                            requiresCrossFadeWithBlurInput = true;
                        }
                    }
                    if (requiresCrossFadeWithBlurInput) {
                        // If we require cross fading with the blur input, we need to make sure we
                        // make a copy of the surface to the image since we will be writing to the
                        // surface while sampling the blurInput.
                        blurInput = activeSurface->makeImageSnapshot();
                    } else {
                        blurInput = activeSurface->makeTemporaryImage();
                    }
                }

                if (layer.backgroundBlurRadius > 0) {
                    SFTRACE_NAME("BackgroundBlur");
                    auto blurredImage = mBlurFilter->generate(context, layer.backgroundBlurRadius,
                                                              blurInput, blurRect);

                    cachedBlurs[layer.backgroundBlurRadius] = blurredImage;

                    mBlurFilter->drawBlurRegion(canvas, bounds, layer.backgroundBlurRadius, 1.0f,
                                                blurRect, blurredImage, blurInput);
                }

                canvas->concat(getSkM44(layer.blurRegionTransform).asM33());
                for (auto region : layer.blurRegions) {
                    if (cachedBlurs[region.blurRadius] == nullptr) {
                        SFTRACE_NAME("BlurRegion");
                        cachedBlurs[region.blurRadius] =
                                mBlurFilter->generate(context, region.blurRadius, blurInput,
                                                      blurRect);
                    }

                    mBlurFilter->drawBlurRegion(canvas, getBlurRRect(region), region.blurRadius,
                                                region.alpha, blurRect,
                                                cachedBlurs[region.blurRadius], blurInput);
                }
            }
        }

        if (layer.shadow.length > 0) {
            // This would require a new parameter/flag to SkShadowUtils::DrawShadow
            LOG_ALWAYS_FATAL_IF(layer.disableBlending, "Cannot disableBlending with a shadow");

            SkRRect shadowBounds, shadowClip;
            if (layer.geometry.boundaries == layer.shadow.boundaries) {
                shadowBounds = bounds;
                shadowClip = roundRectClip;
            } else {
                std::tie(shadowBounds, shadowClip) =
                        getBoundsAndClip(layer.shadow.boundaries, layer.geometry.roundedCornersCrop,
                                         layer.geometry.roundedCornersRadius);
            }

            // Technically, if bounds is a rect and roundRectClip is not empty,
            // it means that the bounds and roundedCornersCrop were different
            // enough that we should intersect them to find the proper shadow.
            // In practice, this often happens when the two rectangles appear to
            // not match due to rounding errors. Draw the rounded version, which
            // looks more like the intent.
            const auto& rrect =
                    shadowBounds.isRect() && !shadowClip.isEmpty() ? shadowClip : shadowBounds;
            drawShadow(canvas, rrect, layer.shadow);
        }

        // Similar to shadows, do the rendering before the clip is applied because even when the
        // layer is occluded it should have an outline.
        if (layer.borderSettings.strokeWidth > 0) {
            // TODO(b/367464660): Move this code to the parent scope and
            // update shadow rendering above to use these bounds since they should be
            // identical.
            SkRRect originalBounds, originalClip;
            std::tie(originalBounds, originalClip) =
                    getBoundsAndClip(layer.geometry.boundaries, layer.geometry.roundedCornersCrop,
                                     layer.geometry.roundedCornersRadius);
            const SkRRect& preferredOriginalBounds =
                    originalBounds.isRect() && !originalClip.isEmpty() ? originalClip
                                                                       : originalBounds;

            SkRRect outlineRect = preferredOriginalBounds;
            outlineRect.outset(layer.borderSettings.strokeWidth, layer.borderSettings.strokeWidth);

            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(layer.borderSettings.color);
            paint.setStyle(SkPaint::kFill_Style);
            canvas->drawDRRect(outlineRect, preferredOriginalBounds, paint);
        }

        const float layerDimmingRatio = layer.whitePointNits <= 0.f
                ? displayDimmingRatio
                : (layer.whitePointNits / maxLayerWhitePoint) * displayDimmingRatio;

        const bool dimInLinearSpace = display.dimmingStage !=
                aidl::android::hardware::graphics::composer3::DimmingStage::GAMMA_OETF;

        const bool isExtendedHdr = (layer.sourceDataspace & ui::Dataspace::RANGE_MASK) ==
                        static_cast<int32_t>(ui::Dataspace::RANGE_EXTENDED) &&
                (display.outputDataspace & ui::Dataspace::TRANSFER_MASK) ==
                        static_cast<int32_t>(ui::Dataspace::TRANSFER_SRGB);

        const bool useFakeOutputDataspaceForRuntimeEffect = !dimInLinearSpace && isExtendedHdr;

        const ui::Dataspace fakeDataspace = useFakeOutputDataspaceForRuntimeEffect
                ? static_cast<ui::Dataspace>(
                          (display.outputDataspace & ui::Dataspace::STANDARD_MASK) |
                          ui::Dataspace::TRANSFER_GAMMA2_2 |
                          (display.outputDataspace & ui::Dataspace::RANGE_MASK))
                : ui::Dataspace::UNKNOWN;

        // If the input dataspace is range extended, the output dataspace transfer is sRGB
        // and dimmingStage is GAMMA_OETF, dim in linear space instead, and
        // set the output dataspace's transfer to be GAMMA2_2.
        // This allows DPU side to use oetf_gamma_2p2 for extended HDR layer
        // to avoid tone shift.
        // The reason of tone shift here is because HDR layers manage white point
        // luminance in linear space, which color pipelines request GAMMA_OETF break
        // without a gamma 2.2 fixup.
        const bool requiresLinearEffect = layer.colorTransform != mat4() ||
                (needsToneMapping(layer.sourceDataspace, display.outputDataspace)) ||
                (dimInLinearSpace && !equalsWithinMargin(1.f, layerDimmingRatio)) ||
                (!dimInLinearSpace && isExtendedHdr);

        // quick abort from drawing the remaining portion of the layer
        if (layer.skipContentDraw ||
            (layer.alpha == 0 && !requiresLinearEffect && !layer.disableBlending &&
             (!displayColorTransform || displayColorTransform->isAlphaUnchanged()))) {
            continue;
        }

        const ui::Dataspace layerDataspace = layer.sourceDataspace;

        SkPaint paint;
        if (layer.source.buffer.buffer) {
            SFTRACE_NAME("DrawImage");
            validateInputBufferUsage(layer.source.buffer.buffer->getBuffer());
            const auto& item = layer.source.buffer;
            auto imageTextureRef = getOrCreateBackendTexture(item.buffer->getBuffer(), false);

            // if the layer's buffer has a fence, then we must respect the fence prior to using
            // the buffer.
            if (layer.source.buffer.fence != nullptr) {
                waitFence(context, layer.source.buffer.fence->get());
            }

            // isOpaque means we need to ignore the alpha in the image,
            // replacing it with the alpha specified by the LayerSettings. See
            // https://developer.android.com/reference/android/view/SurfaceControl.Builder#setOpaque(boolean)
            // The proper way to do this is to use an SkColorType that ignores
            // alpha, like kRGB_888x_SkColorType, and that is used if the
            // incoming image is kRGBA_8888_SkColorType. However, the incoming
            // image may be kRGBA_F16_SkColorType, for which there is no RGBX
            // SkColorType, or kRGBA_1010102_SkColorType, for which we have
            // kRGB_101010x_SkColorType, but it is not yet supported as a source
            // on the GPU. (Adding both is tracked in skbug.com/12048.) In the
            // meantime, we'll use a workaround that works unless we need to do
            // any color conversion. The workaround requires that we pretend the
            // image is already premultiplied, so that we do not premultiply it
            // before applying SkBlendMode::kPlus.
            const bool useIsOpaqueWorkaround = item.isOpaque &&
                    (imageTextureRef->colorType() == kRGBA_1010102_SkColorType ||
                     imageTextureRef->colorType() == kRGBA_F16_SkColorType);
            const auto alphaType = useIsOpaqueWorkaround ? kPremul_SkAlphaType
                    : item.isOpaque                      ? kOpaque_SkAlphaType
                    : item.usePremultipliedAlpha         ? kPremul_SkAlphaType
                                                         : kUnpremul_SkAlphaType;
            sk_sp<SkImage> image = imageTextureRef->makeImage(layerDataspace, alphaType);

            auto texMatrix = getSkM44(item.textureTransform).asM33();
            // textureTansform was intended to be passed directly into a shader, so when
            // building the total matrix with the textureTransform we need to first
            // normalize it, then apply the textureTransform, then scale back up.
            texMatrix.preScale(1.0f / bounds.width(), 1.0f / bounds.height());
            texMatrix.postScale(image->width(), image->height());

            SkMatrix matrix;
            if (!texMatrix.invert(&matrix)) {
                matrix = texMatrix;
            }
            // The shader does not respect the translation, so we add it to the texture
            // transform for the SkImage. This will make sure that the correct layer contents
            // are drawn in the correct part of the screen.
            matrix.postTranslate(bounds.rect().fLeft, bounds.rect().fTop);

            sk_sp<SkShader> shader;

            if (layer.source.buffer.useTextureFiltering) {
                shader = image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                           SkSamplingOptions(
                                                   {SkFilterMode::kLinear, SkMipmapMode::kNone}),
                                           &matrix);
            } else {
                shader = image->makeShader(SkSamplingOptions(), matrix);
            }

            if (useIsOpaqueWorkaround) {
                shader = SkShaders::Blend(SkBlendMode::kPlus, shader,
                                          SkShaders::Color(SkColors::kBlack,
                                                           toSkColorSpace(layerDataspace)));
            }

            SkRect imageBounds;
            matrix.mapRect(&imageBounds, SkRect::Make(image->bounds()));

            paint.setShader(createRuntimeEffectShader(RuntimeEffectShaderParameters{
                    .shader = shader,
                    .layer = layer,
                    .display = display,
                    .undoPremultipliedAlpha = !item.isOpaque && item.usePremultipliedAlpha,
                    .requiresLinearEffect = requiresLinearEffect,
                    .layerDimmingRatio = dimInLinearSpace ? layerDimmingRatio : 1.f,
                    .outputDataSpace = display.outputDataspace,
                    .fakeOutputDataspace = fakeDataspace,
                    .imageBounds = imageBounds,
            }));

            // Turn on dithering when dimming beyond this (arbitrary) threshold...
            static constexpr float kDimmingThreshold = 0.9f;
            // ...or we're rendering an HDR layer down to an 8-bit target
            // Most HDR standards require at least 10-bits of color depth for source content, so we
            // can just extract the transfer function rather than dig into precise gralloc layout.
            // Furthermore, we can assume that the only 8-bit target we support is RGBA8888.
            const bool requiresDownsample =
                    getHdrRenderType(layer.sourceDataspace,
                                     std::optional<ui::PixelFormat>(static_cast<ui::PixelFormat>(
                                             buffer->getPixelFormat()))) != HdrRenderType::SDR &&
                    buffer->getPixelFormat() == PIXEL_FORMAT_RGBA_8888;
            if (layerDimmingRatio <= kDimmingThreshold || requiresDownsample) {
                paint.setDither(true);
            }
            paint.setAlphaf(layer.alpha);

            if (imageTextureRef->colorType() == kAlpha_8_SkColorType) {
                LOG_ALWAYS_FATAL_IF(layer.disableBlending, "Cannot disableBlending with A8");

                // SysUI creates the alpha layer as a coverage layer, which is
                // appropriate for the DPU. Use a color matrix to convert it to
                // a mask.
                // TODO (b/219525258): Handle input as a mask.
                //
                // The color matrix will convert A8 pixels with no alpha to
                // black, as described by this vector. If the display handles
                // the color transform, we need to invert it to find the color
                // that will result in black after the DPU applies the transform.
                SkV4 black{0.0f, 0.0f, 0.0f, 1.0f}; // r, g, b, a
                if (display.colorTransform != mat4() && display.deviceHandlesColorTransform) {
                    SkM44 colorSpaceMatrix = getSkM44(display.colorTransform);
                    if (colorSpaceMatrix.invert(&colorSpaceMatrix)) {
                        black = colorSpaceMatrix * black;
                    } else {
                        // We'll just have to use 0,0,0 as black, which should
                        // be close to correct.
                        ALOGI("Could not invert colorTransform!");
                    }
                }
                SkColorMatrix colorMatrix(0, 0, 0, 0, black[0],
                                          0, 0, 0, 0, black[1],
                                          0, 0, 0, 0, black[2],
                                          0, 0, 0, -1, 1);
                if (display.colorTransform != mat4() && !display.deviceHandlesColorTransform) {
                    // On the other hand, if the device doesn't handle it, we
                    // have to apply it ourselves.
                    colorMatrix.postConcat(toSkColorMatrix(display.colorTransform));
                }
                paint.setColorFilter(SkColorFilters::Matrix(colorMatrix));
            }
        } else {
            SFTRACE_NAME("DrawColor");
            const auto color = layer.source.solidColor;
            sk_sp<SkShader> shader = SkShaders::Color(SkColor4f{.fR = color.r,
                                                                .fG = color.g,
                                                                .fB = color.b,
                                                                .fA = layer.alpha},
                                                      toSkColorSpace(layerDataspace));
            paint.setShader(createRuntimeEffectShader(
                    RuntimeEffectShaderParameters{.shader = shader,
                                                  .layer = layer,
                                                  .display = display,
                                                  .undoPremultipliedAlpha = false,
                                                  .requiresLinearEffect = requiresLinearEffect,
                                                  .layerDimmingRatio = layerDimmingRatio,
                                                  .outputDataSpace = display.outputDataspace,
                                                  .fakeOutputDataspace = fakeDataspace,
                                                  .imageBounds = SkRect::MakeEmpty()}));
        }

        if (layer.disableBlending) {
            paint.setBlendMode(SkBlendMode::kSrc);
        }

        // An A8 buffer will already have the proper color filter attached to
        // its paint, including the displayColorTransform as needed.
        if (!paint.getColorFilter()) {
            if (!dimInLinearSpace && !equalsWithinMargin(1.0, layerDimmingRatio)) {
                // If we don't dim in linear space, then when we gamma correct the dimming ratio we
                // can assume a gamma 2.2 transfer function.
                static constexpr float kInverseGamma22 = 1.f / 2.2f;
                const auto gammaCorrectedDimmingRatio =
                        std::pow(layerDimmingRatio, kInverseGamma22);
                auto dimmingMatrix =
                        mat4::scale(vec4(gammaCorrectedDimmingRatio, gammaCorrectedDimmingRatio,
                                         gammaCorrectedDimmingRatio, 1.f));

                const auto colorFilter =
                        SkColorFilters::Matrix(toSkColorMatrix(std::move(dimmingMatrix)));
                paint.setColorFilter(displayColorTransform
                                             ? displayColorTransform->makeComposed(colorFilter)
                                             : colorFilter);
            } else {
                paint.setColorFilter(displayColorTransform);
            }
        }

        if (!roundRectClip.isEmpty()) {
            canvas->clipRRect(roundRectClip, true);
        }

        if (!bounds.isRect()) {
            paint.setAntiAlias(true);
            canvas->drawRRect(bounds, paint);
        } else {
            canvas->drawRect(bounds.rect(), paint);
        }
        if (kGaneshFlushAfterEveryLayer) {
            SFTRACE_NAME("flush surface");
            // No-op in Graphite. If "flushing" Skia's drawing commands after each layer is desired
            // in Graphite, then a graphite::Recording would need to be snapped and tracked for each
            // layer, which is likely possible but adds non-trivial complexity (in both bookkeeping
            // and refactoring).
            skgpu::ganesh::Flush(activeSurface);
        }
    }

    surfaceAutoSaveRestore.restore();
    mCapture->endCapture();

    LOG_ALWAYS_FATAL_IF(activeSurface != dstSurface);
    auto drawFence = sp<Fence>::make(flushAndSubmit(context, dstSurface));
    trace(drawFence);
    FenceTimePtr fenceTime = FenceTime::makeValid(drawFence);
    for (const auto& layer : layers) {
        if (FlagManager::getInstance().monitor_buffer_fences()) {
            if (layer.source.buffer.buffer) {
                layer.source.buffer.buffer->getBuffer()
                        ->getDependencyMonitor()
                        .addAccessCompletion(fenceTime, "RE");
            }
        }
    }
    resultPromise->set_value(std::move(drawFence));
}

void SkiaRenderEngine::tonemapAndDrawGainmapInternal(
        const std::shared_ptr<std::promise<FenceResult>>&& resultPromise,
        const std::shared_ptr<ExternalTexture>& hdr, base::borrowed_fd&& hdrFence,
        float hdrSdrRatio, ui::Dataspace dataspace, const std::shared_ptr<ExternalTexture>& sdr,
        const std::shared_ptr<ExternalTexture>& gainmap) {
    std::lock_guard<std::mutex> lock(mRenderingMutex);
    auto context = getActiveContext();
    auto gainmapTextureRef = getOrCreateBackendTexture(gainmap->getBuffer(), true);
    sk_sp<SkSurface> gainmapSurface =
            gainmapTextureRef->getOrCreateSurface(ui::Dataspace::V0_SRGB_LINEAR);

    auto sdrTextureRef = getOrCreateBackendTexture(sdr->getBuffer(), true);
    sk_sp<SkSurface> sdrSurface = sdrTextureRef->getOrCreateSurface(dataspace);

    waitFence(context, hdrFence);
    const auto hdrTextureRef = getOrCreateBackendTexture(hdr->getBuffer(), false);
    const auto hdrImage = hdrTextureRef->makeImage(dataspace, kPremul_SkAlphaType);
    const auto hdrShader =
            hdrImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                 SkSamplingOptions({SkFilterMode::kNearest, SkMipmapMode::kNone}),
                                 nullptr);

    const auto tonemappedShader = localTonemap(hdrShader, 1.0f, 1.0f);

    static GainmapFactory kGainmapFactory;
    const auto gainmapShader =
            kGainmapFactory.createSkShader(tonemappedShader, hdrShader, hdrSdrRatio);

    sp<Fence> drawFence;

    {
        const auto canvas = sdrSurface->getCanvas();
        SkPaint paint;
        paint.setShader(tonemappedShader);
        paint.setBlendMode(SkBlendMode::kSrc);
        canvas->drawPaint(paint);

        drawFence = sp<Fence>::make(flushAndSubmit(context, sdrSurface));
        trace(drawFence);
    }

    {
        const auto canvas = gainmapSurface->getCanvas();
        SkPaint paint;
        paint.setShader(gainmapShader);
        paint.setBlendMode(SkBlendMode::kSrc);
        canvas->drawPaint(paint);

        auto gmFence = sp<Fence>::make(flushAndSubmit(context, gainmapSurface));
        trace(gmFence);
        drawFence = Fence::merge("gm-ss", drawFence, gmFence);
    }
    resultPromise->set_value(std::move(drawFence));
}

size_t SkiaRenderEngine::getMaxTextureSize() const {
    return mContext->getMaxTextureSize();
}

size_t SkiaRenderEngine::getMaxViewportDims() const {
    return mContext->getMaxRenderTargetSize();
}

void SkiaRenderEngine::drawShadow(SkCanvas* canvas,
                                  const SkRRect& casterRRect,
                                  const ShadowSettings& settings) {
    SFTRACE_CALL();
    const float casterZ = settings.length / 2.0f;
    const auto flags =
            settings.casterIsTranslucent ? kTransparentOccluder_ShadowFlag : kNone_ShadowFlag;

    SkShadowUtils::DrawShadow(canvas, SkPath::RRect(casterRRect), SkPoint3::Make(0, 0, casterZ),
                              getSkPoint3(settings.lightPos), settings.lightRadius,
                              getSkColor(settings.ambientColor), getSkColor(settings.spotColor),
                              flags);
}

void SkiaRenderEngine::onActiveDisplaySizeChanged(ui::Size size) {
    // This cache multiplier was selected based on review of cache sizes relative
    // to the screen resolution. Looking at the worst case memory needed by blur (~1.5x),
    // shadows (~1x), and general data structures (e.g. vertex buffers) we selected this as a
    // conservative default based on that analysis.
    const float SURFACE_SIZE_MULTIPLIER = 3.5f * bytesPerPixel(mDefaultPixelFormat);
    const int maxResourceBytes = size.width * size.height * SURFACE_SIZE_MULTIPLIER;

    // start by resizing the current context
    getActiveContext()->setResourceCacheLimit(maxResourceBytes);

    // if it is possible to switch contexts then we will resize the other context
    const bool originalProtectedState = mInProtectedContext;
    useProtectedContext(!mInProtectedContext);
    if (mInProtectedContext != originalProtectedState) {
        getActiveContext()->setResourceCacheLimit(maxResourceBytes);
        // reset back to the initial context that was active when this method was called
        useProtectedContext(originalProtectedState);
    }
}

void SkiaRenderEngine::dump(std::string& result) {
    // Dump for the specific backend (GLES or Vk)
    appendBackendSpecificInfoToDump(result);

    // Info about protected content
    StringAppendF(&result, "RenderEngine supports protected context: %d\n",
                  supportsProtectedContent());
    StringAppendF(&result, "RenderEngine is in protected context: %d\n", mInProtectedContext);
    StringAppendF(&result, "RenderEngine shaders cached since last dump/primeCache: %d\n",
                  mSkSLCacheMonitor.shadersCachedSinceLastCall());

    std::vector<ResourcePair> cpuResourceMap = {
            {"skia/sk_resource_cache/bitmap_", "Bitmaps"},
            {"skia/sk_resource_cache/rrect-blur_", "Masks"},
            {"skia/sk_resource_cache/rects-blur_", "Masks"},
            {"skia/sk_resource_cache/tessellated", "Shadows"},
            {"skia", "Other"},
    };
    SkiaMemoryReporter cpuReporter(cpuResourceMap, false);
    SkGraphics::DumpMemoryStatistics(&cpuReporter);
    StringAppendF(&result, "Skia CPU Caches: ");
    cpuReporter.logTotals(result);
    cpuReporter.logOutput(result);

    {
        std::lock_guard<std::mutex> lock(mRenderingMutex);

        std::vector<ResourcePair> gpuResourceMap = {
                {"texture_renderbuffer", "Texture/RenderBuffer"},
                {"texture", "Texture"},
                {"gr_text_blob_cache", "Text"},
                {"skia", "Other"},
        };
        SkiaMemoryReporter gpuReporter(gpuResourceMap, true);
        mContext->dumpMemoryStatistics(&gpuReporter);
        StringAppendF(&result, "Skia's GPU Caches: ");
        gpuReporter.logTotals(result);
        gpuReporter.logOutput(result);
        StringAppendF(&result, "Skia's Wrapped Objects:\n");
        gpuReporter.logOutput(result, true);

        StringAppendF(&result, "RenderEngine tracked buffers: %zu\n",
                      mGraphicBufferExternalRefs.size());
        StringAppendF(&result, "Dumping buffer ids...\n");
        for (const auto& [id, refCounts] : mGraphicBufferExternalRefs) {
            StringAppendF(&result, "- 0x%" PRIx64 " - %d refs \n", id, refCounts);
        }
        StringAppendF(&result, "RenderEngine AHB/BackendTexture cache size: %zu\n",
                      mTextureCache.size());
        StringAppendF(&result, "Dumping buffer ids...\n");
        // TODO(178539829): It would be nice to know which layer these are coming from and what
        // the texture sizes are.
        for (const auto& [id, unused] : mTextureCache) {
            StringAppendF(&result, "- 0x%" PRIx64 "\n", id);
        }
        StringAppendF(&result, "\n");

        SkiaMemoryReporter gpuProtectedReporter(gpuResourceMap, true);
        if (mProtectedContext) {
            mProtectedContext->dumpMemoryStatistics(&gpuProtectedReporter);
        }
        StringAppendF(&result, "Skia's GPU Protected Caches: ");
        gpuProtectedReporter.logTotals(result);
        gpuProtectedReporter.logOutput(result);
        StringAppendF(&result, "Skia's Protected Wrapped Objects:\n");
        gpuProtectedReporter.logOutput(result, true);

        StringAppendF(&result, "\n");
        StringAppendF(&result, "RenderEngine runtime effects: %zu\n", mRuntimeEffects.size());
        for (const auto& [linearEffect, unused] : mRuntimeEffects) {
            StringAppendF(&result, "- inputDataspace: %s\n",
                          dataspaceDetails(
                                  static_cast<android_dataspace>(linearEffect.inputDataspace))
                                  .c_str());
            StringAppendF(&result, "- outputDataspace: %s\n",
                          dataspaceDetails(
                                  static_cast<android_dataspace>(linearEffect.outputDataspace))
                                  .c_str());
            StringAppendF(&result, "undoPremultipliedAlpha: %s\n",
                          linearEffect.undoPremultipliedAlpha ? "true" : "false");
        }
    }
    StringAppendF(&result, "\n");
}

} // namespace skia
} // namespace renderengine
} // namespace android
