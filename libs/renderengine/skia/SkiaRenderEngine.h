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

#ifndef SF_SKIARENDERENGINE_H_
#define SF_SKIARENDERENGINE_H_

#include <renderengine/RenderEngine.h>

#include <android-base/thread_annotations.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>
#include <include/gpu/ganesh/GrBackendSemaphore.h>
#include <include/gpu/ganesh/GrContextOptions.h>
#include <renderengine/ExternalTexture.h>
#include <renderengine/RenderEngine.h>
#include <sys/types.h>

#include <memory>
#include <mutex>
#include <unordered_map>

#include "AutoBackendTexture.h"
#include "android-base/macros.h"
#include "compat/SkiaGpuContext.h"
#include "debug/SkiaCapture.h"
#include "filters/BlurFilter.h"
#include "filters/EdgeExtensionShaderFactory.h"
#include "filters/LinearEffect.h"
#include "filters/LutShader.h"
#include "filters/StretchShaderFactory.h"

class SkData;

struct SkPoint3;

namespace android {

namespace renderengine {

class Mesh;
class Texture;

namespace skia {

class BlurFilter;

class SkiaRenderEngine : public RenderEngine {
public:
    static std::unique_ptr<SkiaRenderEngine> create(const RenderEngineCreationArgs& args);
    SkiaRenderEngine(Threaded, PixelFormat pixelFormat, BlurAlgorithm);
    ~SkiaRenderEngine() override;

    std::future<void> primeCache(PrimeCacheConfig config) override final;
    void cleanupPostRender() override final;
    bool supportsBackgroundBlur() override final {
        return mBlurFilter != nullptr;
    }
    void onActiveDisplaySizeChanged(ui::Size size) override final;
    int reportShadersCompiled();

    virtual void setEnableTracing(bool tracingEnabled) override final;

    void useProtectedContext(bool useProtectedContext) override;
    bool supportsProtectedContent() const override {
        return supportsProtectedContentImpl();
    }
    void ensureContextsCreated();

protected:
    // This is so backends can stop the generic rendering state first before cleaning up
    // backend-specific state. SkiaGpuContexts are invalid after invocation.
    void finishRenderingAndAbandonContexts();

    // Functions that a given backend (GLES, Vulkan) must implement
    using Contexts = std::pair<unique_ptr<SkiaGpuContext>, unique_ptr<SkiaGpuContext>>;
    virtual Contexts createContexts() = 0;
    virtual bool supportsProtectedContentImpl() const = 0;
    virtual bool useProtectedContextImpl(GrProtected isProtected) = 0;
    virtual void waitFence(SkiaGpuContext* context, base::borrowed_fd fenceFd) = 0;
    virtual base::unique_fd flushAndSubmit(SkiaGpuContext* context,
                                           sk_sp<SkSurface> dstSurface) = 0;
    virtual void appendBackendSpecificInfoToDump(std::string& result) = 0;

    size_t getMaxTextureSize() const override final;
    size_t getMaxViewportDims() const override final;
    // TODO: b/293371537 - Return reference instead of pointer? (Cleanup)
    SkiaGpuContext* getActiveContext();

    bool isProtected() const { return mInProtectedContext; }

    // Implements PersistentCache as a way to monitor what SkSL shaders Skia has
    // cached.
    class SkSLCacheMonitor : public GrContextOptions::PersistentCache {
    public:
        SkSLCacheMonitor() = default;
        ~SkSLCacheMonitor() override = default;

        sk_sp<SkData> load(const SkData& key) override;

        void store(const SkData& key, const SkData& data, const SkString& description) override;

        int shadersCachedSinceLastCall() {
            const int shadersCachedSinceLastCall = mShadersCachedSinceLastCall;
            mShadersCachedSinceLastCall = 0;
            return shadersCachedSinceLastCall;
        }

        int totalShadersCompiled() const { return mTotalShadersCompiled; }

    private:
        int mShadersCachedSinceLastCall = 0;
        int mTotalShadersCompiled = 0;
    };

    SkSLCacheMonitor mSkSLCacheMonitor;

private:
    void mapExternalTextureBuffer(const sp<GraphicBuffer>& buffer,
                                  bool isRenderable) override final;
    void unmapExternalTextureBuffer(sp<GraphicBuffer>&& buffer) override final;
    bool canSkipPostRenderCleanup() const override final;

    std::shared_ptr<AutoBackendTexture::LocalRef> getOrCreateBackendTexture(
            const sp<GraphicBuffer>& buffer, bool isOutputBuffer) REQUIRES(mRenderingMutex);
    void initCanvas(SkCanvas* canvas, const DisplaySettings& display);
    void drawShadow(SkCanvas* canvas, const SkRRect& casterRRect,
                    const ShadowSettings& shadowSettings);
    void drawLayersInternal(const std::shared_ptr<std::promise<FenceResult>>&& resultPromise,
                            const DisplaySettings& display,
                            const std::vector<LayerSettings>& layers,
                            const std::shared_ptr<ExternalTexture>& buffer,
                            base::unique_fd&& bufferFence) override final;
    void tonemapAndDrawGainmapInternal(
            const std::shared_ptr<std::promise<FenceResult>>&& resultPromise,
            const std::shared_ptr<ExternalTexture>& hdr, base::borrowed_fd&& hdrFence,
            float hdrSdrRatio, ui::Dataspace dataspace, const std::shared_ptr<ExternalTexture>& sdr,
            const std::shared_ptr<ExternalTexture>& gainmap) override final;

    void dump(std::string& result) override final;

    // If requiresLinearEffect is true or the layer has a stretchEffect a new shader is returned.
    // Otherwise it returns the input shader.
    struct RuntimeEffectShaderParameters {
        sk_sp<SkShader> shader;
        const LayerSettings& layer;
        const DisplaySettings& display;
        bool undoPremultipliedAlpha;
        bool requiresLinearEffect;
        float layerDimmingRatio;
        const ui::Dataspace outputDataSpace;
        const ui::Dataspace fakeOutputDataspace;
        const SkRect& imageBounds;
    };
    sk_sp<SkShader> createRuntimeEffectShader(const RuntimeEffectShaderParameters&);

    sk_sp<SkShader> localTonemap(sk_sp<SkShader>, float inputMultiplier, float targetHdrSdrRatio);

    const PixelFormat mDefaultPixelFormat;

    // Identifier used for various mappings of layers to various
    // textures or shaders
    using GraphicBufferId = uint64_t;

    // Number of external holders of ExternalTexture references, per GraphicBuffer ID.
    std::unordered_map<GraphicBufferId, int32_t> mGraphicBufferExternalRefs
            GUARDED_BY(mRenderingMutex);
    std::unordered_map<GraphicBufferId, std::shared_ptr<AutoBackendTexture::LocalRef>> mTextureCache
            GUARDED_BY(mRenderingMutex);
    std::unordered_map<shaders::LinearEffect, sk_sp<SkRuntimeEffect>, shaders::LinearEffectHasher>
            mRuntimeEffects;
    AutoBackendTexture::CleanupManager mTextureCleanupMgr GUARDED_BY(mRenderingMutex);

    StretchShaderFactory mStretchShaderFactory;
    EdgeExtensionShaderFactory mEdgeExtensionShaderFactory;
    LutShader mLutShader;

    sp<Fence> mLastDrawFence;
    BlurFilter* mBlurFilter = nullptr;

    // Object to capture commands send to Skia.
    std::unique_ptr<SkiaCapture> mCapture;

    // Mutex guarding rendering operations, so that internal state related to
    // rendering that is potentially modified by multiple threads is guaranteed thread-safe.
    mutable std::mutex mRenderingMutex;

    // Graphics context used for creating surfaces and submitting commands
    unique_ptr<SkiaGpuContext> mContext;
    // Same as above, but for protected content (eg. DRM)
    unique_ptr<SkiaGpuContext> mProtectedContext;
    bool mInProtectedContext = false;
};

} // namespace skia
} // namespace renderengine
} // namespace android

#endif /* SF_GLESRENDERENGINE_H_ */
