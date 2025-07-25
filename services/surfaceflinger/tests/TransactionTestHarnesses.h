/*
 * Copyright (C) 2019 The Android Open Source Project
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
#ifndef ANDROID_TRANSACTION_TEST_HARNESSES
#define ANDROID_TRANSACTION_TEST_HARNESSES

#include <com_android_graphics_libgui_flags.h>
#include <ui/DisplayState.h>

#include "LayerTransactionTest.h"
#include "ui/LayerStack.h"

namespace android {

using android::hardware::graphics::common::V1_1::BufferUsage;

class LayerRenderPathTestHarness {
public:
    LayerRenderPathTestHarness(LayerTransactionTest* delegate, RenderPath renderPath)
          : mDelegate(delegate), mRenderPath(renderPath) {}

    std::unique_ptr<ScreenCapture> getScreenCapture() {
        switch (mRenderPath) {
            case RenderPath::SCREENSHOT:
                return mDelegate->screenshot();
            case RenderPath::VIRTUAL_DISPLAY:

                const auto ids = SurfaceComposerClient::getPhysicalDisplayIds();
                const PhysicalDisplayId displayId = ids.front();
                const auto displayToken = ids.empty()
                        ? nullptr
                        : SurfaceComposerClient::getPhysicalDisplayToken(displayId);

                ui::DisplayState displayState;
                SurfaceComposerClient::getDisplayState(displayToken, &displayState);

                ui::DisplayMode displayMode;
                SurfaceComposerClient::getActiveDisplayMode(displayToken, &displayMode);
                ui::Size resolution = displayMode.resolution;
                if (displayState.orientation == ui::Rotation::Rotation90 ||
                    displayState.orientation == ui::Rotation::Rotation270) {
                    std::swap(resolution.width, resolution.height);
                }

                sp<IBinder> vDisplay;

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
                sp<BufferItemConsumer> itemConsumer = sp<BufferItemConsumer>::make(
                        // Sample usage bits from screenrecord
                        GRALLOC_USAGE_HW_VIDEO_ENCODER | GRALLOC_USAGE_SW_READ_OFTEN);
                sp<BufferListener> listener = sp<BufferListener>::make(this);
                itemConsumer->setFrameAvailableListener(listener);
                itemConsumer->setName(String8("Virtual disp consumer (TransactionTest)"));
                itemConsumer->setDefaultBufferSize(resolution.getWidth(), resolution.getHeight());
#else
                sp<IGraphicBufferProducer> producer;
                sp<IGraphicBufferConsumer> consumer;
                sp<BufferItemConsumer> itemConsumer;
                BufferQueue::createBufferQueue(&producer, &consumer);

                consumer->setConsumerName(String8("Virtual disp consumer (TransactionTest)"));
                consumer->setDefaultBufferSize(resolution.getWidth(), resolution.getHeight());

                itemConsumer = sp<BufferItemConsumer>::make(consumer,
                                                            // Sample usage bits from screenrecord
                                                            GRALLOC_USAGE_HW_VIDEO_ENCODER |
                                                                    GRALLOC_USAGE_SW_READ_OFTEN);
                sp<BufferListener> listener = sp<BufferListener>::make(this);
                itemConsumer->setFrameAvailableListener(listener);
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)

                static const std::string kDisplayName("VirtualDisplay");
                vDisplay = SurfaceComposerClient::createVirtualDisplay(kDisplayName,
                                                                       false /*isSecure*/);

                constexpr ui::LayerStack layerStack{
                        848472}; // ASCII for TTH (TransactionTestHarnesses)
                sp<SurfaceControl> mirrorSc =
                        SurfaceComposerClient::getDefault()->mirrorDisplay(displayId);

                SurfaceComposerClient::Transaction t;
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
                t.setDisplaySurface(vDisplay,
                                    itemConsumer->getSurface()->getIGraphicBufferProducer());
#else
                t.setDisplaySurface(vDisplay, producer);
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
                t.setDisplayProjection(vDisplay, ui::Rotation::Rotation0, Rect(resolution),
                                       Rect(resolution));
                t.setDisplayLayerStack(vDisplay, layerStack);
                t.setLayerStack(mirrorSc, layerStack);
                t.apply();
                SurfaceComposerClient::Transaction().apply(true);

                std::unique_lock lock(mMutex);
                mAvailable = false;
                // Wait for frame buffer ready.
                mCondition.wait_for(lock, std::chrono::seconds(2),
                                    [this]() NO_THREAD_SAFETY_ANALYSIS { return mAvailable; });

                BufferItem item;
                itemConsumer->acquireBuffer(&item, 0, true);
                constexpr bool kContainsHdr = false;
                auto sc = std::make_unique<ScreenCapture>(item.mGraphicBuffer, kContainsHdr);
                itemConsumer->releaseBuffer(item);

                // Possible race condition with destroying virtual displays, in which
                // CompositionEngine::present may attempt to be called on the same
                // display multiple times. The layerStack is set to invalid here so
                // that the display is ignored if that scenario occurs.
                t.setLayerStack(mirrorSc, ui::INVALID_LAYER_STACK);
                t.apply(true);
                SurfaceComposerClient::destroyVirtualDisplay(vDisplay);
                return sc;
        }
    }

protected:
    LayerTransactionTest* mDelegate;
    RenderPath mRenderPath;
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mAvailable = false;

    void onFrameAvailable() {
        std::unique_lock lock(mMutex);
        mAvailable = true;
        mCondition.notify_all();
    }

    class BufferListener : public ConsumerBase::FrameAvailableListener {
    public:
        BufferListener(LayerRenderPathTestHarness* owner) : mOwner(owner) {}
        LayerRenderPathTestHarness* mOwner;

        void onFrameAvailable(const BufferItem& /*item*/) { mOwner->onFrameAvailable(); }
    };
};

class LayerTypeTransactionHarness : public LayerTransactionTest {
public:
    LayerTypeTransactionHarness(uint32_t layerType) : mLayerType(layerType) {}

    sp<SurfaceControl> createLayer(const char* name, uint32_t width, uint32_t height,
                                   uint32_t flags = 0, SurfaceControl* parent = nullptr,
                                   uint32_t* outTransformHint = nullptr,
                                   PixelFormat format = PIXEL_FORMAT_RGBA_8888) {
        // if the flags already have a layer type specified, return an error
        if (flags & ISurfaceComposerClient::eFXSurfaceMask) {
            return nullptr;
        }
        return LayerTransactionTest::createLayer(name, width, height, flags | mLayerType, parent,
                                                 outTransformHint, format);
    }

    void fillLayerColor(const sp<SurfaceControl>& layer, const Color& color, uint32_t bufferWidth,
                        uint32_t bufferHeight) {
        ASSERT_NO_FATAL_FAILURE(LayerTransactionTest::fillLayerColor(mLayerType, layer, color,
                                                                     bufferWidth, bufferHeight));
    }

    void fillLayerQuadrant(const sp<SurfaceControl>& layer, uint32_t bufferWidth,
                           uint32_t bufferHeight, const Color& topLeft, const Color& topRight,
                           const Color& bottomLeft, const Color& bottomRight) {
        ASSERT_NO_FATAL_FAILURE(LayerTransactionTest::fillLayerQuadrant(mLayerType, layer,
                                                                        bufferWidth, bufferHeight,
                                                                        topLeft, topRight,
                                                                        bottomLeft, bottomRight));
    }

protected:
    uint32_t mLayerType;
};
} // namespace android
#endif
