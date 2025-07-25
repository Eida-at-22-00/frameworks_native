/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include <memory>
#include <string>
#include <unordered_map>

#include <android-base/thread_annotations.h>
#include <android/native_window.h>
#include <binder/IBinder.h>
#include <compositionengine/Display.h>
#include <compositionengine/DisplaySurface.h>
#include <gui/LayerState.h>
#include <math/mat4.h>
#include <renderengine/RenderEngine.h>
#include <system/window.h>
#include <ui/DisplayId.h>
#include <ui/DisplayIdentification.h>
#include <ui/DisplayState.h>
#include <ui/GraphicTypes.h>
#include <ui/HdrCapabilities.h>
#include <ui/Region.h>
#include <ui/StaticDisplayInfo.h>
#include <ui/Transform.h>
#include <utils/Errors.h>
#include <utils/Mutex.h>
#include <utils/RefBase.h>
#include <utils/Timers.h>

#include "DisplayHardware/DisplayMode.h"
#include "DisplayHardware/Hal.h"
#include "FrontEnd/DisplayInfo.h"
#include "Scheduler/RefreshRateSelector.h"
#include "ThreadContext.h"
#include "TracedOrdinal.h"
#include "Utils/Dumper.h"

namespace android {

class Fence;
class HWComposer;
class HdrSdrRatioOverlay;
class IGraphicBufferProducer;
class Layer;
class RefreshRateOverlay;
class SurfaceFlinger;

struct CompositionInfo;
struct DisplayDeviceCreationArgs;

namespace display {
class DisplaySnapshot;
} // namespace display

class DisplayDevice : public RefBase {
public:
    constexpr static float sDefaultMinLumiance = 0.0;
    constexpr static float sDefaultMaxLumiance = 500.0;
    enum { eReceivesInput = 0x01 };

    explicit DisplayDevice(DisplayDeviceCreationArgs& args);

    // Must be destroyed on the main thread because it may call into HWComposer.
    virtual ~DisplayDevice();

    std::shared_ptr<compositionengine::Display> getCompositionDisplay() const {
        return mCompositionDisplay;
    }

    bool isVirtual() const;
    bool isPrimary() const { return mIsPrimary; }

    // isSecure indicates whether this display can be trusted to display
    // secure surfaces.
    bool isSecure() const;
    void setSecure(bool secure);

    // The optimization policy influences whether this display is optimized for power or
    // performance.
    gui::ISurfaceComposer::OptimizationPolicy getOptimizationPolicy() const;
    void setOptimizationPolicy(gui::ISurfaceComposer::OptimizationPolicy optimizationPolicy);

    int getWidth() const;
    int getHeight() const;
    ui::Size getSize() const { return {getWidth(), getHeight()}; }

    void setLayerFilter(ui::LayerFilter);
    void setDisplaySize(ui::Size);
    void setProjection(ui::Rotation orientation, Rect viewport, Rect frame);
    void stageBrightness(float brightness) REQUIRES(kMainThreadContext);
    void persistBrightness(bool needsComposite) REQUIRES(kMainThreadContext);
    bool isBrightnessStale() const REQUIRES(kMainThreadContext);
    void setFlags(uint32_t flags);

    ui::Rotation getPhysicalOrientation() const { return mPhysicalOrientation; }
    ui::Rotation getOrientation() const { return mOrientation; }

    std::optional<float> getStagedBrightness() const REQUIRES(kMainThreadContext);
    ui::Transform::RotationFlags getTransformHint() const;
    const ui::Transform& getTransform() const;
    const Rect& getLayerStackSpaceRect() const;
    const Rect& getOrientedDisplaySpaceRect() const;
    ui::LayerStack getLayerStack() const;
    bool receivesInput() const { return mFlags & eReceivesInput; }

    DisplayId getId() const;

    DisplayIdVariant getDisplayIdVariant() const {
        const auto idVariant = mCompositionDisplay->getDisplayIdVariant();
        LOG_FATAL_IF(!idVariant);
        return *idVariant;
    }

    std::optional<VirtualDisplayIdVariant> getVirtualDisplayIdVariant() const {
        return ftl::match(
                getDisplayIdVariant(),
                [](PhysicalDisplayId) { return std::optional<VirtualDisplayIdVariant>(); },
                [](auto id) { return std::optional<VirtualDisplayIdVariant>(id); });
    }

    // Shorthand to upcast the ID of a display whose type is known as a precondition.
    PhysicalDisplayId getPhysicalId() const {
        const auto physicalDisplayId = asPhysicalDisplayId(getDisplayIdVariant());
        LOG_FATAL_IF(!physicalDisplayId);
        return *physicalDisplayId;
    }

    VirtualDisplayId getVirtualId() const {
        const auto virtualDisplayId = asVirtualDisplayId(getDisplayIdVariant());
        LOG_FATAL_IF(!virtualDisplayId);
        return *virtualDisplayId;
    }

    const wp<IBinder>& getDisplayToken() const { return mDisplayToken; }
    int32_t getSequenceId() const { return mSequenceId; }

    const Region& getUndefinedRegion() const;

    int32_t getSupportedPerFrameMetadata() const;

    bool hasWideColorGamut() const;
    // Whether h/w composer has native support for specific HDR type.
    bool hasHDR10PlusSupport() const;
    bool hasHDR10Support() const;
    bool hasHLGSupport() const;
    bool hasDolbyVisionSupport() const;

    void overrideHdrTypes(const std::vector<ui::Hdr>& hdrTypes);

    // The returned HdrCapabilities is the combination of HDR capabilities from
    // hardware composer and RenderEngine. When the DisplayDevice supports wide
    // color gamut, RenderEngine is able to simulate HDR support in Display P3
    // color space for both PQ and HLG HDR contents. The minimum and maximum
    // luminance will be set to sDefaultMinLumiance and sDefaultMaxLumiance
    // respectively if hardware composer doesn't return meaningful values.
    HdrCapabilities getHdrCapabilities() const;

    // Return true if intent is supported by the display.
    bool hasRenderIntent(ui::RenderIntent intent) const;

    const Rect getBounds() const;
    const Rect bounds() const { return getBounds(); }

    void setDisplayName(const std::string& displayName);
    const std::string& getDisplayName() const { return mDisplayName; }

    surfaceflinger::frontend::DisplayInfo getFrontEndInfo() const;

    /* ------------------------------------------------------------------------
     * Display power mode management.
     */
    hardware::graphics::composer::hal::PowerMode getPowerMode() const;
    void setPowerMode(hardware::graphics::composer::hal::PowerMode);
    bool isPoweredOn() const;
    bool isRefreshable() const;
    void tracePowerMode();

    // Enables layer caching on this DisplayDevice
    void enableLayerCaching(bool enable);

    ui::Dataspace getCompositionDataSpace() const;

    scheduler::RefreshRateSelector& refreshRateSelector() const { return *mRefreshRateSelector; }

    // Extends the lifetime of the RefreshRateSelector, so it can outlive this DisplayDevice.
    std::shared_ptr<scheduler::RefreshRateSelector> holdRefreshRateSelector() const {
        return mRefreshRateSelector;
    }

    // Enables an overlay to be displayed with the current refresh rate
    // TODO(b/241285876): Move overlay to DisplayModeController.
    void enableRefreshRateOverlay(bool enable, bool setByHwc, Fps refreshRate, Fps renderFps,
                                  bool showSpinner, bool showRenderRate, bool showInMiddle)
            REQUIRES(kMainThreadContext);
    void updateRefreshRateOverlayRate(Fps refreshRate, Fps renderFps, bool setByHwc = false);
    bool isRefreshRateOverlayEnabled() const { return mRefreshRateOverlay != nullptr; }
    void animateOverlay();
    bool onKernelTimerChanged(std::optional<DisplayModeId>, bool timerExpired);
    void onVrrIdle(bool idle);

    // Enables an overlay to be display with the hdr/sdr ratio
    void enableHdrSdrRatioOverlay(bool enable) REQUIRES(kMainThreadContext);
    void updateHdrSdrRatioOverlayRatio(float currentHdrSdrRatio);
    bool isHdrSdrRatioOverlayEnabled() const { return mHdrSdrRatioOverlay != nullptr; }

    Fps getAdjustedRefreshRate() const { return mAdjustedRefreshRate; }

    // Round the requested refresh rate to match a divisor of the pacesetter
    // display's refresh rate. Only supported for virtual displays.
    void adjustRefreshRate(Fps pacesetterDisplayRefreshRate);

    // release HWC resources (if any) for removable displays
    void disconnect();

    void dump(utils::Dumper&) const;

private:
    const sp<SurfaceFlinger> mFlinger;
    HWComposer& mHwComposer;
    const wp<IBinder> mDisplayToken;
    const int32_t mSequenceId;

    const std::shared_ptr<compositionengine::Display> mCompositionDisplay;

    std::string mDisplayName;

    const ui::Rotation mPhysicalOrientation;
    ui::Rotation mOrientation = ui::ROTATION_0;
    bool mIsOrientationChanged = false;

    TracedOrdinal<hardware::graphics::composer::hal::PowerMode> mPowerMode;

    std::optional<float> mStagedBrightness;
    std::optional<float> mBrightness;

    // TODO(b/182939859): Remove special cases for primary display.
    const bool mIsPrimary;

    gui::ISurfaceComposer::OptimizationPolicy mOptimizationPolicy =
            gui::ISurfaceComposer::OptimizationPolicy::optimizeForPerformance;

    uint32_t mFlags = 0;

    // Requested refresh rate in fps, supported only for virtual displays.
    // when this value is non zero, SurfaceFlinger will try to drop frames
    // for virtual displays to match this requested refresh rate.
    const Fps mRequestedRefreshRate;

    // Adjusted refresh rate, rounded to match a divisor of the pacesetter
    // display's refresh rate. Only supported for virtual displays.
    Fps mAdjustedRefreshRate = 0_Hz;

    std::vector<ui::Hdr> mOverrideHdrTypes;

    std::shared_ptr<scheduler::RefreshRateSelector> mRefreshRateSelector;
    std::unique_ptr<RefreshRateOverlay> mRefreshRateOverlay;
    std::unique_ptr<HdrSdrRatioOverlay> mHdrSdrRatioOverlay;
    // This parameter is only used for hdr/sdr ratio overlay
    float mHdrSdrRatio = 1.0f;
};

struct DisplayDeviceState {
    struct Physical {
        PhysicalDisplayId id;
        hardware::graphics::composer::hal::HWDisplayId hwcDisplayId;
        uint8_t port;
        DisplayModePtr activeMode;

        bool operator==(const Physical& other) const {
            return id == other.id && hwcDisplayId == other.hwcDisplayId;
        }
    };

    bool isVirtual() const { return !physical; }

    int32_t sequenceId = sNextSequenceId++;
    std::optional<Physical> physical;
    sp<IGraphicBufferProducer> surface;
    ui::LayerStack layerStack;
    uint32_t flags = 0;
    Rect layerStackSpaceRect;
    Rect orientedDisplaySpaceRect;
    ui::Rotation orientation = ui::ROTATION_0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string displayName;
    std::string uniqueId;
    bool isSecure = false;

    gui::ISurfaceComposer::OptimizationPolicy optimizationPolicy =
            gui::ISurfaceComposer::OptimizationPolicy::optimizeForPerformance;
    bool isProtected = false;
    // Refer to DisplayDevice::mRequestedRefreshRate, for virtual display only
    Fps requestedRefreshRate;
    int32_t maxLayerPictureProfiles = 0;
    bool hasPictureProcessing = false;
    hardware::graphics::composer::hal::PowerMode initialPowerMode{
            hardware::graphics::composer::hal::PowerMode::OFF};

private:
    static std::atomic<int32_t> sNextSequenceId;
};

struct DisplayDeviceCreationArgs {
    // We use a constructor to ensure some of the values are set, without
    // assuming a default value.
    DisplayDeviceCreationArgs(const sp<SurfaceFlinger>&, HWComposer& hwComposer,
                              const wp<IBinder>& displayToken,
                              std::shared_ptr<compositionengine::Display>);
    const sp<SurfaceFlinger> flinger;
    HWComposer& hwComposer;
    const wp<IBinder> displayToken;
    const std::shared_ptr<compositionengine::Display> compositionDisplay;
    std::shared_ptr<scheduler::RefreshRateSelector> refreshRateSelector;

    int32_t sequenceId{0};
    bool isSecure{false};
    bool isProtected{false};
    sp<ANativeWindow> nativeWindow;
    sp<compositionengine::DisplaySurface> displaySurface;
    ui::Rotation physicalOrientation{ui::ROTATION_0};
    bool hasWideColorGamut{false};
    HdrCapabilities hdrCapabilities;
    int32_t supportedPerFrameMetadata{0};
    std::unordered_map<ui::ColorMode, std::vector<ui::RenderIntent>> hwcColorModes;
    hardware::graphics::composer::hal::PowerMode initialPowerMode{
            hardware::graphics::composer::hal::PowerMode::OFF};
    bool isPrimary{false};
    // Refer to DisplayDevice::mRequestedRefreshRate, for virtual display only
    Fps requestedRefreshRate;
};

} // namespace android
