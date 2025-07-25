/*
 * Copyright 2018 The Android Open Source Project
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

#pragma once

#include <gmock/gmock.h>
#include <renderengine/DisplaySettings.h>
#include <renderengine/LayerSettings.h>
#include <renderengine/RenderEngine.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <ui/Region.h>

namespace android {
namespace renderengine {
namespace mock {

class RenderEngine : public renderengine::RenderEngine {
public:
    RenderEngine();
    ~RenderEngine() override;

    MOCK_METHOD1(primeCache, std::future<void>(PrimeCacheConfig));
    MOCK_METHOD1(dump, void(std::string&));
    MOCK_CONST_METHOD0(getMaxTextureSize, size_t());
    MOCK_CONST_METHOD0(getMaxViewportDims, size_t());
    MOCK_CONST_METHOD0(isProtected, bool());
    MOCK_CONST_METHOD0(supportsProtectedContent, bool());
    MOCK_METHOD1(useProtectedContext, void(bool));
    MOCK_METHOD0(cleanupPostRender, void());
    MOCK_CONST_METHOD0(canSkipPostRenderCleanup, bool());
    MOCK_METHOD4(drawLayers,
                 ftl::Future<FenceResult>(const DisplaySettings&, const std::vector<LayerSettings>&,
                                          const std::shared_ptr<ExternalTexture>&,
                                          base::unique_fd&&));
    MOCK_METHOD6(tonemapAndDrawGainmap,
                 ftl::Future<FenceResult>(const std::shared_ptr<ExternalTexture>&,
                                          base::borrowed_fd&&, float, ui::Dataspace,
                                          const std::shared_ptr<ExternalTexture>&,
                                          const std::shared_ptr<ExternalTexture>&));
    MOCK_METHOD7(tonemapAndDrawGainmapInternal,
                 void(const std::shared_ptr<std::promise<FenceResult>>&&,
                      const std::shared_ptr<ExternalTexture>&, base::borrowed_fd&&, float,
                      ui::Dataspace, const std::shared_ptr<ExternalTexture>&,
                      const std::shared_ptr<ExternalTexture>&));
    MOCK_METHOD5(drawLayersInternal,
                 void(const std::shared_ptr<std::promise<FenceResult>>&&, const DisplaySettings&,
                      const std::vector<LayerSettings>&, const std::shared_ptr<ExternalTexture>&,
                      base::unique_fd&&));
    MOCK_METHOD0(getContextPriority, int());
    MOCK_METHOD0(supportsBackgroundBlur, bool());
    MOCK_METHOD1(onActiveDisplaySizeChanged, void(ui::Size));

protected:
    // mock renderengine still needs to implement these, but callers should never need to call them.
    void mapExternalTextureBuffer(const sp<GraphicBuffer>&, bool) {}
    void unmapExternalTextureBuffer(sp<GraphicBuffer>&&) {}
};

} // namespace mock
} // namespace renderengine
} // namespace android
