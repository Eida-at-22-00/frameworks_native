/*
 * Copyright 2013 The Android Open Source Project
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

#ifndef SF_RENDERENGINE_H_
#define SF_RENDERENGINE_H_

#include <android-base/unique_fd.h>
#include <ftl/future.h>
#include <math/mat4.h>
#include <renderengine/DisplaySettings.h>
#include <renderengine/ExternalTexture.h>
#include <renderengine/LayerSettings.h>
#include <stdint.h>
#include <sys/types.h>
#include <ui/FenceResult.h>
#include <ui/GraphicTypes.h>
#include <ui/Transform.h>

#include <future>
#include <memory>

/**
 * Allows to override the RenderEngine backend.
 */
#define PROPERTY_DEBUG_RENDERENGINE_BACKEND "debug.renderengine.backend"

/**
 * Allows opting particular devices into an initial preview rollout of RenderEngine on Graphite.
 *
 * Only applicable within SurfaceFlinger, and if relevant aconfig flags are enabled.
 */
#define PROPERTY_DEBUG_RENDERENGINE_GRAPHITE_PREVIEW_OPTIN \
    "debug.renderengine.graphite_preview_optin"

/**
 * Turns on recording of skia commands in SkiaGL version of the RE. This property
 * defines number of milliseconds for the recording to take place. A non zero value
 * turns on the recording.
 */
#define PROPERTY_DEBUG_RENDERENGINE_CAPTURE_SKIA_MS "debug.renderengine.capture_skia_ms"

/**
 * Set to the most recently saved file once the capture is finished.
 */
#define PROPERTY_DEBUG_RENDERENGINE_CAPTURE_FILENAME "debug.renderengine.capture_filename"

/**
 * Switches the cross-window background blur algorithm.
 */
#define PROPERTY_DEBUG_RENDERENGINE_BLUR_ALGORITHM "debug.renderengine.blur_algorithm"

/**
 * Allows recording of Skia drawing commands with systrace.
 */
#define PROPERTY_SKIA_ATRACE_ENABLED "debug.renderengine.skia_atrace_enabled"

struct ANativeWindowBuffer;

namespace android {

class Rect;
class Region;

namespace renderengine {

class ExternalTexture;
class Image;
class Mesh;
class Texture;
struct RenderEngineCreationArgs;

namespace threaded {
class RenderEngineThreaded;
}

namespace impl {
class RenderEngine;
class ExternalTexture;
}

enum class Protection {
    UNPROTECTED = 1,
    PROTECTED = 2,
};

// Toggles for skipping or enabling priming of particular shaders.
struct PrimeCacheConfig {
    bool cacheHolePunchLayer = true;
    bool cacheSolidLayers = true;
    bool cacheSolidDimmedLayers = true;
    bool cacheImageLayers = true;
    bool cacheImageDimmedLayers = true;
    bool cacheClippedLayers = true;
    bool cacheShadowLayers = true;
    bool cacheEdgeExtension = true;
    bool cachePIPImageLayers = true;
    bool cacheTransparentImageDimmedLayers = true;
    bool cacheClippedDimmedImageLayers = true;
    bool cacheUltraHDR = true;
};

class RenderEngine {
public:
    enum class ContextPriority {
        LOW = 1,
        MEDIUM = 2,
        HIGH = 3,
        REALTIME = 4,
    };

    enum class Threaded {
        NO,
        YES,
    };

    enum class GraphicsApi {
        GL,
        VK,
    };

    enum class SkiaBackend {
        GANESH,
        GRAPHITE,
    };

    enum class BlurAlgorithm {
        NONE,
        GAUSSIAN,
        KAWASE,
        KAWASE_DUAL_FILTER,
    };

    static std::unique_ptr<RenderEngine> create(const RenderEngineCreationArgs& args);

    // Check if the device supports the given GraphicsApi.
    //
    // If called for GraphicsApi::VK then underlying (unprotected) VK resources will be preserved
    // to optimize subsequent VK initialization, but teardown(GraphicsApi::VK) must be invoked if
    // the caller subsequently decides to NOT use VK.
    //
    // The first call may require significant resource initialization, but subsequent checks are
    // cached internally.
    static bool canSupport(GraphicsApi graphicsApi);

    // Teardown any GPU API resources that were previously initialized but are no longer needed.
    //
    // Must be called with GraphicsApi::VK if canSupport(GraphicsApi::VK) was previously invoked but
    // the caller subsequently decided to not use VK.
    //
    // This is safe to call if there is nothing to teardown, but NOT safe to call if a RenderEngine
    // instance exists. The RenderEngine destructor will handle its own teardown logic.
    static void teardown(GraphicsApi graphicsApi);

    virtual ~RenderEngine() = 0;

    // ----- BEGIN DEPRECATED INTERFACE -----
    // This interface, while still in use until a suitable replacement is built,
    // should be considered deprecated, minus some methods which still may be
    // used to support legacy behavior.
    virtual std::future<void> primeCache(PrimeCacheConfig config) = 0;

    // dump the extension strings. always call the base class.
    virtual void dump(std::string& result) = 0;

    // queries that are required to be thread safe
    virtual size_t getMaxTextureSize() const = 0;
    virtual size_t getMaxViewportDims() const = 0;

    // ----- END DEPRECATED INTERFACE -----

    // ----- BEGIN NEW INTERFACE -----

    // queries that are required to be thread safe
    virtual bool supportsProtectedContent() const = 0;

    // Notify RenderEngine of changes to the dimensions of the active display
    // so that it can configure its internal caches accordingly.
    virtual void onActiveDisplaySizeChanged(ui::Size size) = 0;

    // Renders layers for a particular display via GPU composition. This method
    // should be called for every display that needs to be rendered via the GPU.
    // @param display The display-wide settings that should be applied prior to
    // drawing any layers.
    //
    // Assumptions when calling this method:
    // 1. There is exactly one caller - i.e. multi-threading is not supported.
    // 2. Additional threads may be calling the {bind,cache}ExternalTexture
    // methods above. But the main thread is responsible for holding resources
    // such that Image destruction does not occur while this method is called.
    //
    // TODO(b/136806342): This should behavior should ideally be fixed since
    // the above two assumptions are brittle, as conditional thread safetyness
    // may be insufficient when maximizing rendering performance in the future.
    //
    // @param layers The layers to draw onto the display, in Z-order.
    // @param buffer The buffer which will be drawn to. This buffer will be
    // ready once drawFence fires.
    // @param bufferFence Fence signalling that the buffer is ready to be drawn
    // to.
    // @return A future object of FenceResult indicating whether drawing was
    // successful in async mode.
    virtual ftl::Future<FenceResult> drawLayers(const DisplaySettings& display,
                                                const std::vector<LayerSettings>& layers,
                                                const std::shared_ptr<ExternalTexture>& buffer,
                                                base::unique_fd&& bufferFence);

    // Tonemaps an HDR input image and draws an SDR rendition, plus a gainmap
    // describing how to recover the HDR image.
    //
    // The HDR input image is ALWAYS encoded with an sRGB transfer function and
    // is a floating point format. Accordingly, the hdrSdrRatio describes the
    // max luminance in the HDR input image above SDR, and the dataspace
    // describes the input primaries.
    virtual ftl::Future<FenceResult> tonemapAndDrawGainmap(
            const std::shared_ptr<ExternalTexture>& hdr, base::borrowed_fd&& hdrFence,
            float hdrSdrRatio, ui::Dataspace dataspace, const std::shared_ptr<ExternalTexture>& sdr,
            const std::shared_ptr<ExternalTexture>& gainmap);

    // Clean-up method that should be called on the main thread after the
    // drawFence returned by drawLayers fires. This method will free up
    // resources used by the most recently drawn frame. If the frame is still
    // being drawn, then the implementation is free to silently ignore this call.
    virtual void cleanupPostRender() = 0;

    // Returns the priority this context was actually created with. Note: this
    // may not be the same as specified at context creation time, due to
    // implementation limits on the number of contexts that can be created at a
    // specific priority level in the system.
    //
    // This should return a valid EGL context priority enum as described by
    // https://registry.khronos.org/EGL/extensions/IMG/EGL_IMG_context_priority.txt
    // or
    // https://registry.khronos.org/EGL/extensions/NV/EGL_NV_context_priority_realtime.txt
    virtual int getContextPriority() = 0;

    // Returns true if blur was requested in the RenderEngineCreationArgs and the implementation
    // also supports background blur.  If false, no blur will be applied when drawing layers. This
    // query is required to be thread safe.
    virtual bool supportsBackgroundBlur() = 0;

    // TODO(b/180767535): This is only implemented to allow for backend-specific behavior, which
    // we should not allow in general, so remove this.
    bool isThreaded() const { return mThreaded == Threaded::YES; }

    static void validateInputBufferUsage(const sp<GraphicBuffer>&);
    static void validateOutputBufferUsage(const sp<GraphicBuffer>&);

    // Allows flinger to get the render engine thread id for power management with ADPF
    // Returns the tid of the renderengine thread if it's threaded, and std::nullopt otherwise
    virtual std::optional<pid_t> getRenderEngineTid() const { return std::nullopt; }

    virtual void setEnableTracing(bool /*tracingEnabled*/) {}

protected:
    RenderEngine() : RenderEngine(Threaded::NO) {}

    RenderEngine(Threaded threaded) : mThreaded(threaded) {}

    // Maps GPU resources for this buffer.
    // Note that work may be deferred to an additional thread, i.e. this call
    // is made asynchronously, but the caller can expect that map/unmap calls
    // are performed in a manner that's conflict serializable, i.e. unmapping
    // a buffer should never occur before binding the buffer if the caller
    // called mapExternalTextureBuffer before calling unmap.
    // Note also that if the buffer contains protected content, then mapping those GPU resources may
    // be deferred until the buffer is really used for drawing. This is because typical SoCs that
    // support protected memory only support a limited amount, so optimisitically mapping protected
    // memory may be too burdensome. If a buffer contains protected content and the RenderEngine
    // implementation supports protected context, then GPU resources may be mapped into both the
    // protected and unprotected contexts.
    // If the buffer may ever be written to by RenderEngine, then isRenderable must be true.
    virtual void mapExternalTextureBuffer(const sp<GraphicBuffer>& buffer, bool isRenderable) = 0;
    // Unmaps GPU resources used by this buffer. This method should be
    // invoked when the caller will no longer hold a reference to a GraphicBuffer
    // and needs to clean up its resources.
    // Note that if there are multiple callers holding onto the same buffer, then the buffer's
    // resources may be internally ref-counted to guard against use-after-free errors. Note that
    // work may be deferred to an additional thread, i.e. this call is expected to be made
    // asynchronously, but the caller can expect that map/unmap calls are performed in a manner
    // that's conflict serializable, i.e. unmap a buffer should never occur before binding the
    // buffer if the caller called mapExternalTextureBuffer before calling unmap.
    virtual void unmapExternalTextureBuffer(sp<GraphicBuffer>&& buffer) = 0;

    // A thread safe query to determine if any post rendering cleanup is necessary.  Returning true
    // is a signal that calling the postRenderCleanup method would be a no-op and that callers can
    // avoid any thread synchronization that may be required by directly calling postRenderCleanup.
    virtual bool canSkipPostRenderCleanup() const = 0;

    friend class impl::ExternalTexture;
    friend class threaded::RenderEngineThreaded;
    friend class RenderEngineTest_cleanupPostRender_cleansUpOnce_Test;
    const Threaded mThreaded;

    // Update protectedContext mode depending on whether or not any layer has a protected buffer.
    void updateProtectedContext(const std::vector<LayerSettings>&,
                                std::vector<const ExternalTexture*>);
    // Attempt to switch RenderEngine into and out of protectedContext mode
    virtual void useProtectedContext(bool useProtectedContext) = 0;

    virtual void drawLayersInternal(
            const std::shared_ptr<std::promise<FenceResult>>&& resultPromise,
            const DisplaySettings& display, const std::vector<LayerSettings>& layers,
            const std::shared_ptr<ExternalTexture>& buffer, base::unique_fd&& bufferFence) = 0;

    virtual void tonemapAndDrawGainmapInternal(
            const std::shared_ptr<std::promise<FenceResult>>&& resultPromise,
            const std::shared_ptr<ExternalTexture>& hdr, base::borrowed_fd&& hdrFence,
            float hdrSdrRatio, ui::Dataspace dataspace, const std::shared_ptr<ExternalTexture>& sdr,
            const std::shared_ptr<ExternalTexture>& gainmap) = 0;
};

struct RenderEngineCreationArgs {
    int pixelFormat;
    uint32_t imageCacheSize;
    bool useColorManagement;
    bool enableProtectedContext;
    bool precacheToneMapperShaderOnly;
    RenderEngine::BlurAlgorithm blurAlgorithm;
    RenderEngine::ContextPriority contextPriority;
    RenderEngine::Threaded threaded;
    RenderEngine::GraphicsApi graphicsApi;
    RenderEngine::SkiaBackend skiaBackend;

    struct Builder;

private:
    // must be created by Builder via constructor with full argument list
    RenderEngineCreationArgs(int _pixelFormat, uint32_t _imageCacheSize,
                             bool _enableProtectedContext, bool _precacheToneMapperShaderOnly,
                             RenderEngine::BlurAlgorithm _blurAlgorithm,
                             RenderEngine::ContextPriority _contextPriority,
                             RenderEngine::Threaded _threaded,
                             RenderEngine::GraphicsApi _graphicsApi,
                             RenderEngine::SkiaBackend _skiaBackend)
          : pixelFormat(_pixelFormat),
            imageCacheSize(_imageCacheSize),
            enableProtectedContext(_enableProtectedContext),
            precacheToneMapperShaderOnly(_precacheToneMapperShaderOnly),
            blurAlgorithm(_blurAlgorithm),
            contextPriority(_contextPriority),
            threaded(_threaded),
            graphicsApi(_graphicsApi),
            skiaBackend(_skiaBackend) {}
    RenderEngineCreationArgs() = delete;
};

struct RenderEngineCreationArgs::Builder {
    Builder() {}

    Builder& setPixelFormat(int pixelFormat) {
        this->pixelFormat = pixelFormat;
        return *this;
    }
    Builder& setImageCacheSize(uint32_t imageCacheSize) {
        this->imageCacheSize = imageCacheSize;
        return *this;
    }
    Builder& setEnableProtectedContext(bool enableProtectedContext) {
        this->enableProtectedContext = enableProtectedContext;
        return *this;
    }
    Builder& setPrecacheToneMapperShaderOnly(bool precacheToneMapperShaderOnly) {
        this->precacheToneMapperShaderOnly = precacheToneMapperShaderOnly;
        return *this;
    }
    Builder& setBlurAlgorithm(RenderEngine::BlurAlgorithm blurAlgorithm) {
        this->blurAlgorithm = blurAlgorithm;
        return *this;
    }
    Builder& setContextPriority(RenderEngine::ContextPriority contextPriority) {
        this->contextPriority = contextPriority;
        return *this;
    }
    Builder& setThreaded(RenderEngine::Threaded threaded) {
        this->threaded = threaded;
        return *this;
    }
    Builder& setGraphicsApi(RenderEngine::GraphicsApi graphicsApi) {
        this->graphicsApi = graphicsApi;
        return *this;
    }
    Builder& setSkiaBackend(RenderEngine::SkiaBackend skiaBackend) {
        this->skiaBackend = skiaBackend;
        return *this;
    }
    RenderEngineCreationArgs build() const {
        return RenderEngineCreationArgs(pixelFormat, imageCacheSize, enableProtectedContext,
                                        precacheToneMapperShaderOnly, blurAlgorithm,
                                        contextPriority, threaded, graphicsApi, skiaBackend);
    }

private:
    // 1 means RGBA_8888
    int pixelFormat = 1;
    uint32_t imageCacheSize = 0;
    bool enableProtectedContext = false;
    bool precacheToneMapperShaderOnly = false;
    RenderEngine::BlurAlgorithm blurAlgorithm = RenderEngine::BlurAlgorithm::NONE;
    RenderEngine::ContextPriority contextPriority = RenderEngine::ContextPriority::MEDIUM;
    RenderEngine::Threaded threaded = RenderEngine::Threaded::YES;
    RenderEngine::GraphicsApi graphicsApi = RenderEngine::GraphicsApi::GL;
    RenderEngine::SkiaBackend skiaBackend = RenderEngine::SkiaBackend::GANESH;
};

} // namespace renderengine
} // namespace android

#endif /* SF_RENDERENGINE_H_ */
