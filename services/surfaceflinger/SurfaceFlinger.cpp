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

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wextra"

// #define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "SurfaceFlinger.h"

#include <aidl/android/hardware/power/Boost.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android/configuration.h>
#include <android/gui/IDisplayEventConnection.h>
#include <android/gui/StaticDisplayInfo.h>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/types.h>
#include <android/native_window.h>
#include <android/os/IInputFlinger.h>
#include <android_os.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/PermissionCache.h>
#include <com_android_graphics_libgui_flags.h>
#include <com_android_graphics_surfaceflinger_flags.h>
#include <common/FlagManager.h>
#include <common/WorkloadTracer.h>
#include <common/trace.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/Display.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/DisplayColorProfileCreationArgs.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/LayerFECompositionState.h>
#include <compositionengine/OutputLayer.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/impl/DisplayColorProfile.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <compositionengine/impl/OutputLayerCompositionState.h>
#include <configstore/Utils.h>
#include <cutils/compiler.h>
#include <cutils/properties.h>
#include <fmt/format.h>
#include <ftl/algorithm.h>
#include <ftl/concat.h>
#include <ftl/fake_guard.h>
#include <ftl/future.h>
#include <ftl/unit.h>
#include <gui/AidlUtil.h>
#include <gui/BufferQueue.h>
#include <gui/DebugEGLImageTracker.h>
#include <gui/IProducerListener.h>
#include <gui/LayerMetadata.h>
#include <gui/LayerState.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <hidl/ServiceManagement.h>
#include <layerproto/LayerProtoHeader.h>
#include <linux/sched/types.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>
#include <private/gui/SyncFeatures.h>
#include <processgroup/processgroup.h>
#include <renderengine/RenderEngine.h>
#include <renderengine/impl/ExternalTexture.h>
#include <scheduler/FrameTargeter.h>
#include <statslog_surfaceflinger.h>
#include <sys/types.h>
#include <ui/ColorSpace.h>
#include <ui/DebugUtils.h>
#include <ui/DisplayId.h>
#include <ui/DisplayMode.h>
#include <ui/DisplayStatInfo.h>
#include <ui/DisplayState.h>
#include <ui/DynamicDisplayInfo.h>
#include <ui/FrameRateCategoryRate.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/HdrRenderTypeUtils.h>
#include <ui/LayerStack.h>
#include <ui/PixelFormat.h>
#include <ui/StaticDisplayInfo.h>
#include <unistd.h>
#include <utils/StopWatch.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/Timers.h>
#include <utils/misc.h>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <common/FlagManager.h>
#include <gui/LayerStatePermissions.h>
#include <gui/SchedulingPolicy.h>
#include <gui/SyncScreenCaptureListener.h>
#include <ui/DisplayIdentification.h>
#include "BackgroundExecutor.h"
#include "Client.h"
#include "ClientCache.h"
#include "Colorizer.h"
#include "DisplayDevice.h"
#include "DisplayHardware/ComposerHal.h"
#include "DisplayHardware/FramebufferSurface.h"
#include "DisplayHardware/Hal.h"
#include "DisplayHardware/VirtualDisplaySurface.h"
#include "Effects/Daltonizer.h"
#include "FpsReporter.h"
#include "FrameTimeline/FrameTimeline.h"
#include "FrameTracer/FrameTracer.h"
#include "FrontEnd/LayerCreationArgs.h"
#include "FrontEnd/LayerHandle.h"
#include "FrontEnd/LayerLifecycleManager.h"
#include "FrontEnd/LayerLog.h"
#include "FrontEnd/LayerSnapshot.h"
#include "HdrLayerInfoReporter.h"
#include "Jank/JankTracker.h"
#include "Layer.h"
#include "LayerProtoHelper.h"
#include "LayerVector.h"
#include "MutexUtils.h"
#include "NativeWindowSurface.h"
#include "PowerAdvisor/PowerAdvisor.h"
#include "PowerAdvisor/Workload.h"
#include "RegionSamplingThread.h"
#include "Scheduler/EventThread.h"
#include "Scheduler/LayerHistory.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/VsyncConfiguration.h"
#include "Scheduler/VsyncModulator.h"
#include "ScreenCaptureOutput.h"
#include "SurfaceFlingerProperties.h"
#include "TimeStats/TimeStats.h"
#include "TunnelModeEnabledReporter.h"
#include "Utils/Dumper.h"
#include "WindowInfosListenerInvoker.h"

#ifdef QCOM_UM_FAMILY
#if __has_include("QtiGralloc.h")
#include "QtiGralloc.h"
#else
#include "gralloc_priv.h"
#endif
#endif

#include <aidl/android/hardware/graphics/common/DisplayDecorationSupport.h>
#include <aidl/android/hardware/graphics/composer3/DisplayCapability.h>
#include <aidl/android/hardware/graphics/composer3/OutputType.h>
#include <aidl/android/hardware/graphics/composer3/RenderIntent.h>

#undef NO_THREAD_SAFETY_ANALYSIS
#define NO_THREAD_SAFETY_ANALYSIS \
    _Pragma("GCC error \"Prefer <ftl/fake_guard.h> or MutexUtils.h helpers.\"")

namespace android {
using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

using namespace hardware::configstore;
using namespace hardware::configstore::V1_0;
using namespace sysprop;
using ftl::Flags;
using namespace ftl::flag_operators;

using aidl::android::hardware::graphics::common::DisplayDecorationSupport;
using aidl::android::hardware::graphics::composer3::Capability;
using aidl::android::hardware::graphics::composer3::DisplayCapability;
using CompositionStrategyPredictionState = android::compositionengine::impl::
        OutputCompositionState::CompositionStrategyPredictionState;

using base::StringAppendF;
using display::PhysicalDisplay;
using display::PhysicalDisplays;
using frontend::TransactionHandler;
using gui::DisplayInfo;
using gui::GameMode;
using gui::IDisplayEventConnection;
using gui::IWindowInfosListener;
using gui::LayerMetadata;
using gui::WindowInfo;
using gui::aidl_utils::binderStatusFromStatusT;
using scheduler::VsyncModulator;
using ui::Dataspace;
using ui::DisplayPrimaries;
using ui::RenderIntent;

namespace hal = android::hardware::graphics::composer::hal;

namespace {

static constexpr int FOUR_K_WIDTH = 3840;
static constexpr int FOUR_K_HEIGHT = 2160;

// TODO(b/141333600): Consolidate with DisplayMode::Builder::getDefaultDensity.
constexpr float FALLBACK_DENSITY = ACONFIGURATION_DENSITY_TV;

float getDensityFromProperty(const char* property, bool required) {
    char value[PROPERTY_VALUE_MAX];
    const float density = property_get(property, value, nullptr) > 0 ? std::atof(value) : 0.f;
    if (!density && required) {
        ALOGE("%s must be defined as a build property", property);
        return FALLBACK_DENSITY;
    }
    return density;
}

// Currently we only support V0_SRGB and DISPLAY_P3 as composition preference.
bool validateCompositionDataspace(Dataspace dataspace) {
    return dataspace == Dataspace::V0_SRGB || dataspace == Dataspace::DISPLAY_P3;
}

std::chrono::milliseconds getIdleTimerTimeout(PhysicalDisplayId displayId) {
    if (const int32_t displayIdleTimerMs =
                base::GetIntProperty("debug.sf.set_idle_timer_ms_"s +
                                             std::to_string(displayId.value),
                                     0);
        displayIdleTimerMs > 0) {
        return std::chrono::milliseconds(displayIdleTimerMs);
    }

    const int32_t setIdleTimerMs = base::GetIntProperty("debug.sf.set_idle_timer_ms"s, 0);
    const int32_t millis = setIdleTimerMs ? setIdleTimerMs : sysprop::set_idle_timer_ms(0);
    return std::chrono::milliseconds(millis);
}

bool getKernelIdleTimerSyspropConfig(PhysicalDisplayId displayId) {
    const bool displaySupportKernelIdleTimer =
            base::GetBoolProperty("debug.sf.support_kernel_idle_timer_"s +
                                          std::to_string(displayId.value),
                                  false);

    return displaySupportKernelIdleTimer || sysprop::support_kernel_idle_timer(false);
}

bool isAbove4k30(const ui::DisplayMode& outMode) {
    using fps_approx_ops::operator>;
    Fps refreshRate = Fps::fromValue(outMode.peakRefreshRate);
    return outMode.resolution.getWidth() >= FOUR_K_WIDTH &&
            outMode.resolution.getHeight() >= FOUR_K_HEIGHT && refreshRate > 30_Hz;
}

void excludeDolbyVisionIf4k30Present(const std::vector<ui::Hdr>& displayHdrTypes,
                                     ui::DisplayMode& outMode) {
    if (isAbove4k30(outMode) &&
        std::any_of(displayHdrTypes.begin(), displayHdrTypes.end(),
                    [](ui::Hdr type) { return type == ui::Hdr::DOLBY_VISION_4K30; })) {
        for (ui::Hdr type : displayHdrTypes) {
            if (type != ui::Hdr::DOLBY_VISION_4K30 && type != ui::Hdr::DOLBY_VISION) {
                outMode.supportedHdrTypes.push_back(type);
            }
        }
    } else {
        for (ui::Hdr type : displayHdrTypes) {
            if (type != ui::Hdr::DOLBY_VISION_4K30) {
                outMode.supportedHdrTypes.push_back(type);
            }
        }
    }
}

HdrCapabilities filterOut4k30(const HdrCapabilities& displayHdrCapabilities) {
    std::vector<ui::Hdr> hdrTypes;
    for (ui::Hdr type : displayHdrCapabilities.getSupportedHdrTypes()) {
        if (type != ui::Hdr::DOLBY_VISION_4K30) {
            hdrTypes.push_back(type);
        }
    }
    return {hdrTypes, displayHdrCapabilities.getDesiredMaxLuminance(),
            displayHdrCapabilities.getDesiredMaxAverageLuminance(),
            displayHdrCapabilities.getDesiredMinLuminance()};
}

uint32_t getLayerIdFromSurfaceControl(sp<SurfaceControl> surfaceControl) {
    if (!surfaceControl) {
        return UNASSIGNED_LAYER_ID;
    }
    return LayerHandle::getLayerId(surfaceControl->getHandle());
}

/**
 * Returns true if the file at path exists and is newer than duration.
 */
bool fileNewerThan(const std::string& path, std::chrono::minutes duration) {
    using Clock = std::filesystem::file_time_type::clock;
    std::error_code error;
    std::filesystem::file_time_type updateTime = std::filesystem::last_write_time(path, error);
    if (error) {
        return false;
    }
    return duration > (Clock::now() - updateTime);
}

bool isFrameIntervalOnCadence(TimePoint expectedPresentTime, TimePoint lastExpectedPresentTimestamp,
                              Fps lastFrameInterval, Period timeout, Duration threshold) {
    if (lastFrameInterval.getPeriodNsecs() == 0) {
        return false;
    }

    const auto expectedPresentTimeDeltaNs =
            expectedPresentTime.ns() - lastExpectedPresentTimestamp.ns();

    if (expectedPresentTimeDeltaNs > timeout.ns()) {
        return false;
    }

    const auto expectedPresentPeriods = static_cast<nsecs_t>(
            std::round(static_cast<float>(expectedPresentTimeDeltaNs) /
                       static_cast<float>(lastFrameInterval.getPeriodNsecs())));
    const auto calculatedPeriodsOutNs = lastFrameInterval.getPeriodNsecs() * expectedPresentPeriods;
    const auto calculatedExpectedPresentTimeNs =
            lastExpectedPresentTimestamp.ns() + calculatedPeriodsOutNs;
    const auto presentTimeDelta =
            std::abs(expectedPresentTime.ns() - calculatedExpectedPresentTimeNs);
    return presentTimeDelta < threshold.ns();
}

bool isExpectedPresentWithinTimeout(TimePoint expectedPresentTime,
                                    TimePoint lastExpectedPresentTimestamp,
                                    std::optional<Period> timeoutOpt, Duration threshold) {
    if (!timeoutOpt) {
        // Always within timeout if timeoutOpt is absent and don't send hint
        // for the timeout
        return true;
    }

    if (timeoutOpt->ns() == 0) {
        // Always outside timeout if timeoutOpt is 0 and always send
        // the hint for the timeout.
        return false;
    }

    if (expectedPresentTime.ns() < lastExpectedPresentTimestamp.ns() + timeoutOpt->ns()) {
        return true;
    }

    // Check if within the threshold as it can be just outside the timeout
    return std::abs(expectedPresentTime.ns() -
                    (lastExpectedPresentTimestamp.ns() + timeoutOpt->ns())) < threshold.ns();
}
} // namespace

// ---------------------------------------------------------------------------

const String16 sHardwareTest("android.permission.HARDWARE_TEST");
const String16 sAccessSurfaceFlinger("android.permission.ACCESS_SURFACE_FLINGER");
const String16 sRotateSurfaceFlinger("android.permission.ROTATE_SURFACE_FLINGER");
const String16 sReadFramebuffer("android.permission.READ_FRAME_BUFFER");
const String16 sControlDisplayBrightness("android.permission.CONTROL_DISPLAY_BRIGHTNESS");
const String16 sObservePictureProfiles("android.permission.OBSERVE_PICTURE_PROFILES");
const String16 sDump("android.permission.DUMP");
const String16 sCaptureBlackoutContent("android.permission.CAPTURE_BLACKOUT_CONTENT");
const String16 sInternalSystemWindow("android.permission.INTERNAL_SYSTEM_WINDOW");
const String16 sWakeupSurfaceFlinger("android.permission.WAKEUP_SURFACE_FLINGER");

// ---------------------------------------------------------------------------
int64_t SurfaceFlinger::dispSyncPresentTimeOffset;
bool SurfaceFlinger::useHwcForRgbToYuv;
bool SurfaceFlinger::hasSyncFramework;
int64_t SurfaceFlinger::maxFrameBufferAcquiredBuffers;
int64_t SurfaceFlinger::minAcquiredBuffers = 1;
std::optional<int64_t> SurfaceFlinger::maxAcquiredBuffersOpt;
uint32_t SurfaceFlinger::maxGraphicsWidth;
uint32_t SurfaceFlinger::maxGraphicsHeight;
bool SurfaceFlinger::useContextPriority;
Dataspace SurfaceFlinger::defaultCompositionDataspace = Dataspace::V0_SRGB;
ui::PixelFormat SurfaceFlinger::defaultCompositionPixelFormat = ui::PixelFormat::RGBA_8888;
Dataspace SurfaceFlinger::wideColorGamutCompositionDataspace = Dataspace::V0_SRGB;
ui::PixelFormat SurfaceFlinger::wideColorGamutCompositionPixelFormat = ui::PixelFormat::RGBA_8888;
LatchUnsignaledConfig SurfaceFlinger::enableLatchUnsignaledConfig;

std::string decodeDisplayColorSetting(DisplayColorSetting displayColorSetting) {
    switch (displayColorSetting) {
        case DisplayColorSetting::kManaged:
            return std::string("Managed");
        case DisplayColorSetting::kUnmanaged:
            return std::string("Unmanaged");
        case DisplayColorSetting::kEnhanced:
            return std::string("Enhanced");
        default:
            return std::string("Unknown ") + std::to_string(static_cast<int>(displayColorSetting));
    }
}

bool callingThreadHasPermission(const String16& permission) {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    return uid == AID_GRAPHICS || uid == AID_SYSTEM ||
            PermissionCache::checkPermission(permission, pid, uid);
}

ui::Transform::RotationFlags SurfaceFlinger::sActiveDisplayRotationFlags = ui::Transform::ROT_0;

SurfaceFlinger::SurfaceFlinger(Factory& factory, SkipInitializationTag)
      : mFactory(factory),
        mPid(getpid()),
        mTimeStats(std::make_shared<impl::TimeStats>()),
        mFrameTracer(mFactory.createFrameTracer()),
        mFrameTimeline(mFactory.createFrameTimeline(mTimeStats, mPid)),
        mCompositionEngine(mFactory.createCompositionEngine()),
        mHwcServiceName(base::GetProperty("debug.sf.hwc_service_name"s, "default"s)),
        mTunnelModeEnabledReporter(sp<TunnelModeEnabledReporter>::make()),
        mEmulatedDisplayDensity(getDensityFromProperty("qemu.sf.lcd_density", false)),
        mInternalDisplayDensity(
                getDensityFromProperty("ro.sf.lcd_density", !mEmulatedDisplayDensity)),
        mPowerAdvisor(std::make_unique<
                      adpf::impl::PowerAdvisor>([this] { disableExpensiveRendering(); },
                                                std::chrono::milliseconds(
                                                        sysprop::display_update_imminent_timeout_ms(
                                                                80)))),
        mWindowInfosListenerInvoker(sp<WindowInfosListenerInvoker>::make()),
        mSkipPowerOnForQuiescent(base::GetBoolProperty("ro.boot.quiescent"s, false)) {
    ALOGI("Using HWComposer service: %s", mHwcServiceName.c_str());
}

SurfaceFlinger::SurfaceFlinger(Factory& factory) : SurfaceFlinger(factory, SkipInitialization) {
    SFTRACE_CALL();
    ALOGI("SurfaceFlinger is starting");

    hasSyncFramework = running_without_sync_framework(true);

    dispSyncPresentTimeOffset = present_time_offset_from_vsync_ns(0);

    useHwcForRgbToYuv = force_hwc_copy_for_virtual_displays(false);

    maxFrameBufferAcquiredBuffers = max_frame_buffer_acquired_buffers(2);
    minAcquiredBuffers =
            SurfaceFlingerProperties::min_acquired_buffers().value_or(minAcquiredBuffers);
    maxAcquiredBuffersOpt = SurfaceFlingerProperties::max_acquired_buffers();

    maxGraphicsWidth = std::max(max_graphics_width(0), 0);
    maxGraphicsHeight = std::max(max_graphics_height(0), 0);

    mSupportsWideColor = has_wide_color_display(false);
    mDefaultCompositionDataspace =
            static_cast<ui::Dataspace>(default_composition_dataspace(Dataspace::V0_SRGB));
    mWideColorGamutCompositionDataspace = static_cast<ui::Dataspace>(wcg_composition_dataspace(
            mSupportsWideColor ? Dataspace::DISPLAY_P3 : Dataspace::V0_SRGB));
    defaultCompositionDataspace = mDefaultCompositionDataspace;
    wideColorGamutCompositionDataspace = mWideColorGamutCompositionDataspace;
    defaultCompositionPixelFormat = static_cast<ui::PixelFormat>(
            default_composition_pixel_format(ui::PixelFormat::RGBA_8888));
    wideColorGamutCompositionPixelFormat =
            static_cast<ui::PixelFormat>(wcg_composition_pixel_format(ui::PixelFormat::RGBA_8888));

    mLayerCachingEnabled =
            base::GetBoolProperty("debug.sf.enable_layer_caching"s,
                                  sysprop::SurfaceFlingerProperties::enable_layer_caching()
                                          .value_or(false));

    useContextPriority = use_context_priority(true);

    mInternalDisplayPrimaries = sysprop::getDisplayNativePrimaries();

    // debugging stuff...
    char value[PROPERTY_VALUE_MAX];

    property_get("ro.build.type", value, "user");
    mIsUserBuild = strcmp(value, "user") == 0;

    mDebugFlashDelay = base::GetUintProperty("debug.sf.showupdates"s, 0u);

    property_get("debug.sf.disable_backpressure", value, "0");
    mPropagateBackpressure = !atoi(value);
    ALOGI_IF(!mPropagateBackpressure, "Disabling backpressure propagation");

    mBackpressureGpuComposition = base::GetBoolProperty("debug.sf.enable_gl_backpressure"s, true);
    ALOGI_IF(mBackpressureGpuComposition, "Enabling backpressure for GPU composition");

    property_get("ro.surface_flinger.supports_background_blur", value, "0");
    bool supportsBlurs = atoi(value);
    mSupportsBlur = supportsBlurs;
    ALOGI_IF(!mSupportsBlur, "Disabling blur effects, they are not supported.");

    property_get("debug.sf.luma_sampling", value, "1");
    mLumaSampling = atoi(value);

    property_get("debug.sf.disable_client_composition_cache", value, "0");
    mDisableClientCompositionCache = atoi(value);

    property_get("debug.sf.predict_hwc_composition_strategy", value, "1");
    mPredictCompositionStrategy = atoi(value);

    property_get("debug.sf.treat_170m_as_sRGB", value, "0");
    mTreat170mAsSrgb = atoi(value);

    property_get("debug.sf.dim_in_gamma_in_enhanced_screenshots", value, 0);
    mDimInGammaSpaceForEnhancedScreenshots = atoi(value);

    mIgnoreHwcPhysicalDisplayOrientation =
            base::GetBoolProperty("debug.sf.ignore_hwc_physical_display_orientation"s, false);

    // We should be reading 'persist.sys.sf.color_saturation' here
    // but since /data may be encrypted, we need to wait until after vold
    // comes online to attempt to read the property. The property is
    // instead read after the boot animation

    if (base::GetBoolProperty("debug.sf.treble_testing_override"s, false)) {
        // Without the override SurfaceFlinger cannot connect to HIDL
        // services that are not listed in the manifests.  Considered
        // deriving the setting from the set service name, but it
        // would be brittle if the name that's not 'default' is used
        // for production purposes later on.
        ALOGI("Enabling Treble testing override");
        android::hardware::details::setTrebleTestingOverride(true);
    }

    // TODO (b/270966065) Update the HWC based refresh rate overlay to support spinner
    mRefreshRateOverlaySpinner = property_get_bool("debug.sf.show_refresh_rate_overlay_spinner", 0);
    mRefreshRateOverlayRenderRate =
            property_get_bool("debug.sf.show_refresh_rate_overlay_render_rate", 0);
    mRefreshRateOverlayShowInMiddle =
            property_get_bool("debug.sf.show_refresh_rate_overlay_in_middle", 0);

    if (!mIsUserBuild && base::GetBoolProperty("debug.sf.enable_transaction_tracing"s, true)) {
        mTransactionTracing.emplace();
        mLayerTracing.setTransactionTracing(*mTransactionTracing);
    }

    mIgnoreHdrCameraLayers = ignore_hdr_camera_layers(false);
}

LatchUnsignaledConfig SurfaceFlinger::getLatchUnsignaledConfig() {
    if (base::GetBoolProperty("debug.sf.latch_unsignaled"s, false)) {
        return LatchUnsignaledConfig::Always;
    }
    if (base::GetBoolProperty("debug.sf.auto_latch_unsignaled"s, true)) {
        return LatchUnsignaledConfig::AutoSingleLayer;
    }
    return LatchUnsignaledConfig::Disabled;
}

SurfaceFlinger::~SurfaceFlinger() = default;

void SurfaceFlinger::binderDied(const wp<IBinder>&) {
    // the window manager died on us. prepare its eulogy.
    mBootFinished = false;

    static_cast<void>(mScheduler->schedule([this]() FTL_FAKE_GUARD(kMainThreadContext) {
        // Sever the link to inputflinger since it's gone as well.
        mInputFlinger.clear();

        initializeDisplays();
    }));

    mInitBootPropsFuture.callOnce([this] {
        return std::async(std::launch::async, &SurfaceFlinger::initBootProperties, this);
    });

    mInitBootPropsFuture.wait();
}

void SurfaceFlinger::run() {
    mScheduler->run();
}

sp<IBinder> SurfaceFlinger::createVirtualDisplay(
        const std::string& displayName, bool isSecure,
        gui::ISurfaceComposer::OptimizationPolicy optimizationPolicy, const std::string& uniqueId,
        float requestedRefreshRate) {
    // SurfaceComposerAIDL checks for some permissions, but adding an additional check here.
    // This is to ensure that only root, system, and graphics can request to create a secure
    // display. Secure displays can show secure content so we add an additional restriction on it.
    const uid_t uid = IPCThreadState::self()->getCallingUid();
    if (isSecure && uid != AID_ROOT && uid != AID_GRAPHICS && uid != AID_SYSTEM) {
        ALOGE("Only privileged processes can create a secure display");
        return nullptr;
    }

    ALOGD("Creating virtual display: %s", displayName.c_str());

    class DisplayToken : public BBinder {
        sp<SurfaceFlinger> flinger;
        virtual ~DisplayToken() {
            // no more references, this display must be terminated
            Mutex::Autolock _l(flinger->mStateLock);
            flinger->mCurrentState.displays.removeItem(wp<IBinder>::fromExisting(this));
            flinger->setTransactionFlags(eDisplayTransactionNeeded);
        }

    public:
        explicit DisplayToken(const sp<SurfaceFlinger>& flinger) : flinger(flinger) {}
    };

    sp<BBinder> token = sp<DisplayToken>::make(sp<SurfaceFlinger>::fromExisting(this));

    Mutex::Autolock _l(mStateLock);
    // Display ID is assigned when virtual display is allocated by HWC.
    DisplayDeviceState state;
    state.isSecure = isSecure;
    // Set display as protected when marked as secure to ensure no behavior change
    // TODO (b/314820005): separate as a different arg when creating the display.
    state.isProtected = isSecure;
    state.optimizationPolicy = optimizationPolicy;
    // Virtual displays start in ON mode.
    state.initialPowerMode = hal::PowerMode::ON;
    state.displayName = displayName;
    state.uniqueId = uniqueId;
    state.requestedRefreshRate = Fps::fromValue(requestedRefreshRate);
    mCurrentState.displays.add(token, state);
    return token;
}

status_t SurfaceFlinger::destroyVirtualDisplay(const sp<IBinder>& displayToken) {
    Mutex::Autolock lock(mStateLock);

    const ssize_t index = mCurrentState.displays.indexOfKey(displayToken);
    if (index < 0) {
        ALOGE("%s: Invalid display token %p", __func__, displayToken.get());
        return NAME_NOT_FOUND;
    }

    const DisplayDeviceState& state = mCurrentState.displays.valueAt(index);
    if (state.physical) {
        ALOGE("%s: Invalid operation on physical display", __func__);
        return INVALID_OPERATION;
    }

    ALOGD("Destroying virtual display: %s", state.displayName.c_str());

    mCurrentState.displays.removeItemsAt(index);
    setTransactionFlags(eDisplayTransactionNeeded);
    return NO_ERROR;
}

void SurfaceFlinger::enableHalVirtualDisplays(bool enable) {
    auto& generator = mVirtualDisplayIdGenerators.hal;
    if (!generator && enable) {
        ALOGI("Enabling HAL virtual displays");
        generator.emplace(getHwComposer().getMaxVirtualDisplayCount());
    } else if (generator && !enable) {
        ALOGW_IF(generator->inUse(), "Disabling HAL virtual displays while in use");
        generator.reset();
    }
}

std::optional<VirtualDisplayIdVariant> SurfaceFlinger::acquireVirtualDisplay(
        ui::Size resolution, ui::PixelFormat format, const std::string& uniqueId,
        compositionengine::DisplayCreationArgsBuilder& builder) {
    if (auto& generator = mVirtualDisplayIdGenerators.hal) {
        if (const auto id = generator->generateId()) {
            if (getHwComposer().allocateVirtualDisplay(*id, resolution, &format)) {
                acquireVirtualDisplaySnapshot(*id, uniqueId);
                builder.setId(*id);
                return *id;
            }

            generator->releaseId(*id);
        } else {
            ALOGW("%s: Exhausted HAL virtual displays", __func__);
        }

        ALOGW("%s: Falling back to GPU virtual display", __func__);
    }

    const auto id = mVirtualDisplayIdGenerators.gpu.generateId();
    LOG_ALWAYS_FATAL_IF(!id, "Failed to generate ID for GPU virtual display");
    acquireVirtualDisplaySnapshot(*id, uniqueId);
    builder.setId(*id);
    return *id;
}

void SurfaceFlinger::releaseVirtualDisplay(VirtualDisplayIdVariant displayId) {
    ftl::match(
            displayId,
            [this](HalVirtualDisplayId halVirtualDisplayId) {
                if (auto& generator = mVirtualDisplayIdGenerators.hal) {
                    generator->releaseId(halVirtualDisplayId);
                    releaseVirtualDisplaySnapshot(halVirtualDisplayId);
                }
            },
            [this](GpuVirtualDisplayId gpuVirtualDisplayId) {
                mVirtualDisplayIdGenerators.gpu.releaseId(gpuVirtualDisplayId);
                releaseVirtualDisplaySnapshot(gpuVirtualDisplayId);
            });
}

void SurfaceFlinger::releaseVirtualDisplaySnapshot(VirtualDisplayId displayId) {
    std::lock_guard lock(mVirtualDisplaysMutex);
    if (!mVirtualDisplays.erase(displayId)) {
        ALOGW("%s: Virtual display snapshot was not removed", __func__);
    }
}

std::vector<PhysicalDisplayId> SurfaceFlinger::getPhysicalDisplayIdsLocked() const {
    std::vector<PhysicalDisplayId> displayIds;
    displayIds.reserve(mPhysicalDisplays.size());

    const auto defaultDisplayId = getDefaultDisplayDeviceLocked()->getPhysicalId();
    displayIds.push_back(defaultDisplayId);

    for (const auto& [id, display] : mPhysicalDisplays) {
        if (id != defaultDisplayId) {
            displayIds.push_back(id);
        }
    }

    return displayIds;
}

std::optional<PhysicalDisplayId> SurfaceFlinger::getPhysicalDisplayIdLocked(
        const sp<display::DisplayToken>& displayToken) const {
    return ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
            .transform(&ftl::to_key<PhysicalDisplays>);
}

sp<IBinder> SurfaceFlinger::getPhysicalDisplayToken(PhysicalDisplayId displayId) const {
    Mutex::Autolock lock(mStateLock);
    return getPhysicalDisplayTokenLocked(displayId);
}

HWComposer& SurfaceFlinger::getHwComposer() const {
    return mCompositionEngine->getHwComposer();
}

renderengine::RenderEngine& SurfaceFlinger::getRenderEngine() const {
    return *mRenderEngine;
}

compositionengine::CompositionEngine& SurfaceFlinger::getCompositionEngine() const {
    return *mCompositionEngine.get();
}

void SurfaceFlinger::bootFinished() {
    if (mBootFinished == true) {
        ALOGE("Extra call to bootFinished");
        return;
    }
    mBootFinished = true;
    FlagManager::getMutableInstance().markBootCompleted();

    if (android::os::perfetto_sdk_tracing()) {
        ::tracing_perfetto::registerWithPerfetto();
    }

    mInitBootPropsFuture.wait();
    mRenderEnginePrimeCacheFuture.wait();

    const nsecs_t now = systemTime();
    const nsecs_t duration = now - mBootTime;
    ALOGI("Boot is finished (%ld ms)", long(ns2ms(duration)));

    mFrameTracer->initialize();
    mFrameTimeline->onBootFinished();
    getRenderEngine().setEnableTracing(FlagManager::getInstance().use_skia_tracing());

    // wait patiently for the window manager death
    const String16 name("window");
    mWindowManager = defaultServiceManager()->waitForService(name);
    if (mWindowManager != 0) {
        mWindowManager->linkToDeath(sp<IBinder::DeathRecipient>::fromExisting(this));
    }

    // stop boot animation
    // formerly we would just kill the process, but we now ask it to exit so it
    // can choose where to stop the animation.
    property_set("service.bootanim.exit", "1");

    const int LOGTAG_SF_STOP_BOOTANIM = 60110;
    LOG_EVENT_LONG(LOGTAG_SF_STOP_BOOTANIM, ns2ms(systemTime(SYSTEM_TIME_MONOTONIC)));

    sp<IBinder> input(defaultServiceManager()->waitForService(String16("inputflinger")));

    static_cast<void>(mScheduler->schedule([=, this]() FTL_FAKE_GUARD(kMainThreadContext) {
        if (input == nullptr) {
            ALOGE("Failed to link to input service");
        } else {
            mInputFlinger = interface_cast<os::IInputFlinger>(input);
        }

        readPersistentProperties();
        const bool hintSessionEnabled = FlagManager::getInstance().use_adpf_cpu_hint();
        mPowerAdvisor->enablePowerHintSession(hintSessionEnabled);
        const bool hintSessionUsed = mPowerAdvisor->usePowerHintSession();
        // Ordering is important here, as onBootFinished signals to PowerAdvisor that concurrency
        // is safe because its variables are initialized.
        mPowerAdvisor->onBootFinished();
        ALOGD("Power hint is %s",
              hintSessionUsed ? "supported" : (hintSessionEnabled ? "unsupported" : "disabled"));
        if (hintSessionUsed) {
            std::optional<pid_t> renderEngineTid = getRenderEngine().getRenderEngineTid();
            std::vector<int32_t> tidList;
            tidList.emplace_back(gettid());
            if (renderEngineTid.has_value()) {
                tidList.emplace_back(*renderEngineTid);
            }
            if (!mPowerAdvisor->startPowerHintSession(std::move(tidList))) {
                ALOGW("Cannot start power hint session");
            }
        }

        mBootStage = BootStage::FINISHED;

        if (base::GetBoolProperty("sf.debug.show_refresh_rate_overlay"s, false)) {
            ftl::FakeGuard guard(mStateLock);
            enableRefreshRateOverlay(true);
        }
    }));
}

bool shouldUseGraphiteIfCompiledAndSupported() {
    return FlagManager::getInstance().graphite_renderengine() ||
            (FlagManager::getInstance().graphite_renderengine_preview_rollout() &&
             base::GetBoolProperty(PROPERTY_DEBUG_RENDERENGINE_GRAPHITE_PREVIEW_OPTIN, false));
}

void chooseRenderEngineType(renderengine::RenderEngineCreationArgs::Builder& builder) {
    char prop[PROPERTY_VALUE_MAX];
    property_get(PROPERTY_DEBUG_RENDERENGINE_BACKEND, prop, "");

    // TODO: b/293371537 - Once GraphiteVk is deemed relatively stable, log a warning that
    // PROPERTY_DEBUG_RENDERENGINE_BACKEND is deprecated
    if (strcmp(prop, "skiagl") == 0) {
        builder.setThreaded(renderengine::RenderEngine::Threaded::NO)
                .setGraphicsApi(renderengine::RenderEngine::GraphicsApi::GL);
    } else if (strcmp(prop, "skiaglthreaded") == 0) {
        builder.setThreaded(renderengine::RenderEngine::Threaded::YES)
                .setGraphicsApi(renderengine::RenderEngine::GraphicsApi::GL);
    } else if (strcmp(prop, "skiavk") == 0) {
        builder.setThreaded(renderengine::RenderEngine::Threaded::NO)
                .setGraphicsApi(renderengine::RenderEngine::GraphicsApi::VK);
    } else if (strcmp(prop, "skiavkthreaded") == 0) {
        builder.setThreaded(renderengine::RenderEngine::Threaded::YES)
                .setGraphicsApi(renderengine::RenderEngine::GraphicsApi::VK);
    } else {
        const auto kVulkan = renderengine::RenderEngine::GraphicsApi::VK;
// TODO: b/341728634 - Clean up conditional compilation.
// Note: this guard in particular must check e.g.
// COM_ANDROID_GRAPHICS_SURFACEFLINGER_FLAGS_GRAPHITE_RENDERENGINE directly (instead of calling e.g.
// COM_ANDROID_GRAPHICS_SURFACEFLINGER_FLAGS(GRAPHITE_RENDERENGINE)) because that macro is undefined
// in the libsurfaceflingerflags_test variant of com_android_graphics_surfaceflinger_flags.h, which
// is used by layertracegenerator (which also needs SurfaceFlinger.cpp). :)
#if COM_ANDROID_GRAPHICS_SURFACEFLINGER_FLAGS_GRAPHITE_RENDERENGINE || \
        COM_ANDROID_GRAPHICS_SURFACEFLINGER_FLAGS_FORCE_COMPILE_GRAPHITE_RENDERENGINE
        const bool useGraphite = shouldUseGraphiteIfCompiledAndSupported() &&
                renderengine::RenderEngine::canSupport(kVulkan);
#else
        const bool useGraphite = false;
        if (shouldUseGraphiteIfCompiledAndSupported()) {
            ALOGE("RenderEngine's Graphite Skia backend was requested, but it is not compiled in "
                  "this build! Falling back to Ganesh backend selection logic.");
        }
#endif
        const bool useVulkan = useGraphite ||
                (FlagManager::getInstance().vulkan_renderengine() &&
                 renderengine::RenderEngine::canSupport(kVulkan));

        builder.setSkiaBackend(useGraphite ? renderengine::RenderEngine::SkiaBackend::GRAPHITE
                                           : renderengine::RenderEngine::SkiaBackend::GANESH);
        builder.setGraphicsApi(useVulkan ? kVulkan : renderengine::RenderEngine::GraphicsApi::GL);
    }
}

/**
 * Choose a suggested blurring algorithm if supportsBlur is true. By default Kawase will be
 * suggested as it's faster than a full Gaussian blur and looks close enough.
 */
renderengine::RenderEngine::BlurAlgorithm chooseBlurAlgorithm(bool supportsBlur) {
    if (!supportsBlur) {
        return renderengine::RenderEngine::BlurAlgorithm::NONE;
    }

    auto const algorithm = base::GetProperty(PROPERTY_DEBUG_RENDERENGINE_BLUR_ALGORITHM, "");
    if (algorithm == "gaussian") {
        return renderengine::RenderEngine::BlurAlgorithm::GAUSSIAN;
    } else if (algorithm == "kawase2") {
        return renderengine::RenderEngine::BlurAlgorithm::KAWASE_DUAL_FILTER;
    } else if (algorithm == "kawase") {
        return renderengine::RenderEngine::BlurAlgorithm::KAWASE;
    } else {
        if (FlagManager::getInstance().window_blur_kawase2()) {
            return renderengine::RenderEngine::BlurAlgorithm::KAWASE_DUAL_FILTER;
        }
        return renderengine::RenderEngine::BlurAlgorithm::KAWASE;
    }
}

void SurfaceFlinger::init() FTL_FAKE_GUARD(kMainThreadContext) {
    SFTRACE_CALL();
    ALOGI("SurfaceFlinger's main thread ready to run. "
          "Initializing graphics H/W...");
    addTransactionReadyFilters();
    Mutex::Autolock lock(mStateLock);

    // Get a RenderEngine for the given display / config (can't fail)
    // TODO(b/77156734): We need to stop casting and use HAL types when possible.
    // Sending maxFrameBufferAcquiredBuffers as the cache size is tightly tuned to single-display.
    auto builder = renderengine::RenderEngineCreationArgs::Builder()
                           .setPixelFormat(static_cast<int32_t>(defaultCompositionPixelFormat))
                           .setImageCacheSize(maxFrameBufferAcquiredBuffers)
                           .setEnableProtectedContext(enable_protected_contents(false))
                           .setPrecacheToneMapperShaderOnly(false)
                           .setBlurAlgorithm(chooseBlurAlgorithm(mSupportsBlur))
                           .setContextPriority(
                                   useContextPriority
                                           ? renderengine::RenderEngine::ContextPriority::REALTIME
                                           : renderengine::RenderEngine::ContextPriority::MEDIUM);
    chooseRenderEngineType(builder);
    mRenderEngine = renderengine::RenderEngine::create(builder.build());
    mCompositionEngine->setRenderEngine(mRenderEngine.get());
    mMaxRenderTargetSize =
            std::min(getRenderEngine().getMaxTextureSize(), getRenderEngine().getMaxViewportDims());

    // Set SF main policy after initializing RenderEngine which has its own policy.
    if (!SetTaskProfiles(0, {"SFMainPolicyOverride"})) {
        ALOGW("Failed to set main task profile");
    }

    mCompositionEngine->setTimeStats(mTimeStats);

    mHWComposer = getFactory().createHWComposer(mHwcServiceName);
    mCompositionEngine->setHwComposer(mHWComposer.get());
    auto& composer = mCompositionEngine->getHwComposer();
    composer.setCallback(*this);
    mDisplayModeController.setHwComposer(&composer);

    ClientCache::getInstance().setRenderEngine(&getRenderEngine());

    mHasReliablePresentFences =
            !getHwComposer().hasCapability(Capability::PRESENT_FENCE_IS_NOT_RELIABLE);

    enableLatchUnsignaledConfig = getLatchUnsignaledConfig();

    mAllowHwcForWFD = base::GetBoolProperty("vendor.display.vds_allow_hwc"s, false);
    mAllowHwcForVDS = mAllowHwcForWFD && base::GetBoolProperty("debug.sf.enable_hwc_vds"s, false);
    mFirstApiLevel = android::base::GetIntProperty("ro.product.first_api_level", 0);

    // Process hotplug for displays connected at boot.
    LOG_ALWAYS_FATAL_IF(!configureLocked(),
                        "Initial display configuration failed: HWC did not hotplug");

    mActiveDisplayId = getPrimaryDisplayIdLocked();

    // Commit primary display.
    sp<const DisplayDevice> display;
    if (const auto indexOpt = mCurrentState.getDisplayIndex(mActiveDisplayId)) {
        const auto& displays = mCurrentState.displays;

        const auto& token = displays.keyAt(*indexOpt);
        const auto& state = displays.valueAt(*indexOpt);

        processDisplayAdded(token, state);
        mDrawingState.displays.add(token, state);

        display = getDefaultDisplayDeviceLocked();
    }

    LOG_ALWAYS_FATAL_IF(!display, "Failed to configure the primary display");
    LOG_ALWAYS_FATAL_IF(!getHwComposer().isConnected(display->getPhysicalId()),
                        "Primary display is disconnected");

    // TODO(b/241285876): The Scheduler needlessly depends on creating the CompositionEngine part of
    // the DisplayDevice, hence the above commit of the primary display. Remove that special case by
    // initializing the Scheduler after configureLocked, once decoupled from DisplayDevice.
    initScheduler(display);

    // Start listening after creating the Scheduler, since the listener calls into it.
    mDisplayModeController.setActiveModeListener(
            display::DisplayModeController::ActiveModeListener::make(
                    [this](PhysicalDisplayId displayId, Fps vsyncRate, Fps renderRate) {
                        // This callback cannot lock mStateLock, as some callers already lock it.
                        // Instead, switch context to the main thread.
                        static_cast<void>(mScheduler->schedule([=,
                                                                this]() FTL_FAKE_GUARD(mStateLock) {
                            if (const auto display = getDisplayDeviceLocked(displayId)) {
                                display->updateRefreshRateOverlayRate(vsyncRate, renderRate);
                            }
                        }));
                    }));

    mLayerTracing.setTakeLayersSnapshotProtoFunction(
            [&](uint32_t traceFlags,
                const LayerTracing::OnLayersSnapshotCallback& onLayersSnapshot) {
                // Do not wait the future to avoid deadlocks
                // between main and Perfetto threads (b/313130597)
                static_cast<void>(mScheduler->schedule(
                        [&, traceFlags, onLayersSnapshot]() FTL_FAKE_GUARD(mStateLock)
                                FTL_FAKE_GUARD(kMainThreadContext) {
                                    auto snapshot =
                                            takeLayersSnapshotProto(traceFlags, TimePoint::now(),
                                                                    mLastCommittedVsyncId, true);
                                    onLayersSnapshot(std::move(snapshot));
                                }));
            });

    // Commit secondary display(s).
    processDisplayChangesLocked();

    // initialize our drawing state
    mDrawingState = mCurrentState;

    onActiveDisplayChangedLocked(nullptr, *display);

    static_cast<void>(mScheduler->schedule(
            [this]() FTL_FAKE_GUARD(kMainThreadContext) { initializeDisplays(); }));

    mPowerAdvisor->init();

    if (base::GetBoolProperty("service.sf.prime_shader_cache"s, true)) {
        constexpr const char* kWhence = "primeCache";
        setSchedFifo(false, kWhence);

        mRenderEnginePrimeCacheFuture.callOnce([this] {
            renderengine::PrimeCacheConfig config;
            config.cacheHolePunchLayer =
                    base::GetBoolProperty("debug.sf.prime_shader_cache.hole_punch"s, true);
            config.cacheSolidLayers =
                    base::GetBoolProperty("debug.sf.prime_shader_cache.solid_layers"s, true);
            config.cacheSolidDimmedLayers =
                    base::GetBoolProperty("debug.sf.prime_shader_cache.solid_dimmed_layers"s, true);
            config.cacheImageLayers =
                    base::GetBoolProperty("debug.sf.prime_shader_cache.image_layers"s, true);
            config.cacheImageDimmedLayers =
                    base::GetBoolProperty("debug.sf.prime_shader_cache.image_dimmed_layers"s, true);
            config.cacheClippedLayers =
                    base::GetBoolProperty("debug.sf.prime_shader_cache.clipped_layers"s, true);
            config.cacheShadowLayers =
                    base::GetBoolProperty("debug.sf.prime_shader_cache.shadow_layers"s, true);
            config.cachePIPImageLayers =
                    base::GetBoolProperty("debug.sf.prime_shader_cache.pip_image_layers"s, true);
            config.cacheTransparentImageDimmedLayers = base::
                    GetBoolProperty("debug.sf.prime_shader_cache.transparent_image_dimmed_layers"s,
                                    true);
            config.cacheClippedDimmedImageLayers = base::
                    GetBoolProperty("debug.sf.prime_shader_cache.clipped_dimmed_image_layers"s,
                                    true);
            // ro.surface_flinger.prime_chader_cache.ultrahdr exists as a previous ro property
            // which we maintain for backwards compatibility.
            config.cacheUltraHDR =
                    base::GetBoolProperty("ro.surface_flinger.prime_shader_cache.ultrahdr"s, false);
            config.cacheEdgeExtension =
                    base::GetBoolProperty("debug.sf.prime_shader_cache.edge_extension_shader"s,
                                          true);
            return getRenderEngine().primeCache(config);
        });

        setSchedFifo(true, kWhence);
    }

    // Avoid blocking the main thread on `init` to set properties.
    mInitBootPropsFuture.callOnce([this] {
        return std::async(std::launch::async, &SurfaceFlinger::initBootProperties, this);
    });

    initTransactionTraceWriter();
    ALOGV("Done initializing");
}

// During boot, offload `initBootProperties` to another thread. `property_set` depends on
// `property_service`, which may be delayed by slow operations like `mount_all --late` in
// the `init` process. See b/34499826 and b/63844978.
void SurfaceFlinger::initBootProperties() {
    property_set("service.sf.present_timestamp", mHasReliablePresentFences ? "1" : "0");

    if (base::GetBoolProperty("debug.sf.boot_animation"s, true) &&
        (base::GetIntProperty("debug.sf.nobootanimation"s, 0) == 0)) {
        // Reset and (if needed) start BootAnimation.
        property_set("service.bootanim.exit", "0");
        property_set("service.bootanim.progress", "0");
        property_set("ctl.start", "bootanim");
    }
}

void SurfaceFlinger::initTransactionTraceWriter() {
    if (!mTransactionTracing) {
        return;
    }
    TransactionTraceWriter::getInstance().setWriterFunction(
            [&](const std::string& filename, bool overwrite) {
                auto writeFn = [&]() {
                    if (!overwrite && fileNewerThan(filename, std::chrono::minutes{10})) {
                        ALOGD("TransactionTraceWriter: file=%s already exists", filename.c_str());
                        return;
                    }
                    ALOGD("TransactionTraceWriter: writing file=%s", filename.c_str());
                    mTransactionTracing->writeToFile(filename);
                    mTransactionTracing->flush();
                };
                if (std::this_thread::get_id() == mMainThreadId) {
                    writeFn();
                } else {
                    mScheduler->schedule(writeFn).get();
                }
            });
}

void SurfaceFlinger::readPersistentProperties() {
    Mutex::Autolock _l(mStateLock);

    char value[PROPERTY_VALUE_MAX];

    property_get("persist.sys.sf.color_saturation", value, "1.0");
    mGlobalSaturationFactor = atof(value);
    updateColorMatrixLocked();
    ALOGV("Saturation is set to %.2f", mGlobalSaturationFactor);

    property_get("persist.sys.sf.native_mode", value, "0");
    mDisplayColorSetting = static_cast<DisplayColorSetting>(atoi(value));

    mForceColorMode =
            static_cast<ui::ColorMode>(base::GetIntProperty("persist.sys.sf.color_mode"s, 0));
}

status_t SurfaceFlinger::getSupportedFrameTimestamps(std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
            FrameEvent::REQUESTED_PRESENT,
            FrameEvent::ACQUIRE,
            FrameEvent::LATCH,
            FrameEvent::FIRST_REFRESH_START,
            FrameEvent::LAST_REFRESH_START,
            FrameEvent::GPU_COMPOSITION_DONE,
            FrameEvent::DEQUEUE_READY,
            FrameEvent::RELEASE,
    };

    if (mHasReliablePresentFences) {
        outSupported->push_back(FrameEvent::DISPLAY_PRESENT);
    }
    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayState(const sp<IBinder>& displayToken, ui::DisplayState* state) {
    if (!displayToken || !state) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    state->layerStack = display->getLayerStack();
    state->orientation = display->getOrientation();

    const Rect layerStackRect = display->getLayerStackSpaceRect();
    state->layerStackSpaceRect =
            layerStackRect.isValid() ? layerStackRect.getSize() : display->getSize();

    return NO_ERROR;
}

status_t SurfaceFlinger::getStaticDisplayInfo(int64_t displayId, ui::StaticDisplayInfo* info) {
    if (!info) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);
    const PhysicalDisplayId id = PhysicalDisplayId::fromValue(static_cast<uint64_t>(displayId));
    const auto displayOpt = mPhysicalDisplays.get(id).and_then(getDisplayDeviceAndSnapshot());

    if (!displayOpt) {
        return NAME_NOT_FOUND;
    }

    const auto& [display, snapshotRef] = *displayOpt;
    const auto& snapshot = snapshotRef.get();

    info->connectionType = snapshot.connectionType();
    info->port = snapshot.port();
    info->deviceProductInfo = snapshot.deviceProductInfo();

    if (mEmulatedDisplayDensity) {
        info->density = mEmulatedDisplayDensity;
    } else {
        info->density = info->connectionType == ui::DisplayConnectionType::Internal
                ? mInternalDisplayDensity
                : FALLBACK_DENSITY;
    }
    info->density /= ACONFIGURATION_DENSITY_MEDIUM;

    info->secure = display->isSecure();
    info->installOrientation = display->getPhysicalOrientation();

    return NO_ERROR;
}

void SurfaceFlinger::getDynamicDisplayInfoInternal(ui::DynamicDisplayInfo*& info,
                                                   const sp<DisplayDevice>& display,
                                                   const display::DisplaySnapshot& snapshot) {
    const auto& displayModes = snapshot.displayModes();
    info->supportedDisplayModes.clear();
    info->supportedDisplayModes.reserve(displayModes.size());

    for (const auto& [id, mode] : displayModes) {
        ui::DisplayMode outMode;
        outMode.id = ftl::to_underlying(id);

        auto [width, height] = mode->getResolution();
        auto [xDpi, yDpi] = mode->getDpi();

        if (const auto physicalOrientation = display->getPhysicalOrientation();
            physicalOrientation == ui::ROTATION_90 || physicalOrientation == ui::ROTATION_270) {
            std::swap(width, height);
            std::swap(xDpi, yDpi);
        }

        outMode.resolution = ui::Size(width, height);

        outMode.xDpi = xDpi;
        outMode.yDpi = yDpi;

        const auto peakFps = mode->getPeakFps();
        outMode.peakRefreshRate = peakFps.getValue();
        outMode.vsyncRate = mode->getVsyncRate().getValue();

        const auto vsyncConfigSet =
                mScheduler->getVsyncConfigsForRefreshRate(Fps::fromValue(outMode.peakRefreshRate));
        outMode.appVsyncOffset = vsyncConfigSet.late.appOffset;
        outMode.sfVsyncOffset = vsyncConfigSet.late.sfOffset;
        outMode.group = mode->getGroup();

        // This is how far in advance a buffer must be queued for
        // presentation at a given time.  If you want a buffer to appear
        // on the screen at time N, you must submit the buffer before
        // (N - presentationDeadline).
        //
        // Normally it's one full refresh period (to give SF a chance to
        // latch the buffer), but this can be reduced by configuring a
        // VsyncController offset.  Any additional delays introduced by the hardware
        // composer or panel must be accounted for here.
        //
        // We add an additional 1ms to allow for processing time and
        // differences between the ideal and actual refresh rate.
        outMode.presentationDeadline = peakFps.getPeriodNsecs() - outMode.sfVsyncOffset + 1000000;
        excludeDolbyVisionIf4k30Present(display->getHdrCapabilities().getSupportedHdrTypes(),
                                        outMode);
        info->supportedDisplayModes.push_back(outMode);
    }

    info->supportedColorModes = snapshot.filterColorModes(mSupportsWideColor);

    const PhysicalDisplayId displayId = snapshot.displayId();

    const auto mode = display->refreshRateSelector().getActiveMode();
    info->activeDisplayModeId = ftl::to_underlying(mode.modePtr->getId());
    info->renderFrameRate = mode.fps.getValue();
    info->hasArrSupport = mode.modePtr->getVrrConfig() && FlagManager::getInstance().vrr_config();

    const auto [normal, high] = display->refreshRateSelector().getFrameRateCategoryRates();
    ui::FrameRateCategoryRate frameRateCategoryRate(normal.getValue(), high.getValue());
    info->frameRateCategoryRate = frameRateCategoryRate;

    if (info->hasArrSupport) {
        info->supportedRefreshRates = display->refreshRateSelector().getSupportedFrameRates();
    } else {
        // On non-ARR devices, list the refresh rates same as the supported display modes.
        std::vector<float> supportedFrameRates;
        supportedFrameRates.reserve(info->supportedDisplayModes.size());
        std::transform(info->supportedDisplayModes.begin(), info->supportedDisplayModes.end(),
                       std::back_inserter(supportedFrameRates),
                       [](ui::DisplayMode mode) { return mode.peakRefreshRate; });
        info->supportedRefreshRates = supportedFrameRates;
    }
    info->activeColorMode = display->getCompositionDisplay()->getState().colorMode;
    info->hdrCapabilities = filterOut4k30(display->getHdrCapabilities());

    info->autoLowLatencyModeSupported =
            getHwComposer().hasDisplayCapability(displayId,
                                                 DisplayCapability::AUTO_LOW_LATENCY_MODE);
    info->gameContentTypeSupported =
            getHwComposer().supportsContentType(displayId, hal::ContentType::GAME);

    info->preferredBootDisplayMode = static_cast<ui::DisplayModeId>(-1);

    if (getHwComposer().hasCapability(Capability::BOOT_DISPLAY_CONFIG)) {
        if (const auto hwcId = getHwComposer().getPreferredBootDisplayMode(displayId)) {
            if (const auto modeId = snapshot.translateModeId(*hwcId)) {
                info->preferredBootDisplayMode = ftl::to_underlying(*modeId);
            }
        }
    }
}

status_t SurfaceFlinger::getDynamicDisplayInfoFromId(int64_t physicalDisplayId,
                                                     ui::DynamicDisplayInfo* info) {
    if (!info) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const PhysicalDisplayId id =
            PhysicalDisplayId::fromValue(static_cast<uint64_t>(physicalDisplayId));
    const auto displayOpt = mPhysicalDisplays.get(id).and_then(getDisplayDeviceAndSnapshot());

    if (!displayOpt) {
        return NAME_NOT_FOUND;
    }

    const auto& [display, snapshotRef] = *displayOpt;
    getDynamicDisplayInfoInternal(info, display, snapshotRef.get());
    return NO_ERROR;
}

status_t SurfaceFlinger::getDynamicDisplayInfoFromToken(const sp<IBinder>& displayToken,
                                                        ui::DynamicDisplayInfo* info) {
    if (!displayToken || !info) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayOpt = ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
                                    .transform(&ftl::to_mapped_ref<PhysicalDisplays>)
                                    .and_then(getDisplayDeviceAndSnapshot());

    if (!displayOpt) {
        return NAME_NOT_FOUND;
    }

    const auto& [display, snapshotRef] = *displayOpt;
    getDynamicDisplayInfoInternal(info, display, snapshotRef.get());
    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayStats(const sp<IBinder>& displayToken,
                                         DisplayStatInfo* outStats) {
    if (!outStats) {
        return BAD_VALUE;
    }

    // TODO: b/277364366 - Require a display token from clients and remove fallback to pacesetter.
    std::optional<PhysicalDisplayId> displayIdOpt;
    if (displayToken) {
        Mutex::Autolock lock(mStateLock);
        displayIdOpt = getPhysicalDisplayIdLocked(displayToken);
        if (!displayIdOpt) {
            ALOGW("%s: Invalid physical display token %p", __func__, displayToken.get());
            return NAME_NOT_FOUND;
        }
    }

    const auto schedule = mScheduler->getVsyncSchedule(displayIdOpt);
    if (!schedule) {
        ALOGE("%s: Missing VSYNC schedule for display %s!", __func__,
              to_string(*displayIdOpt).c_str());
        return NAME_NOT_FOUND;
    }
    outStats->vsyncTime = schedule->vsyncDeadlineAfter(TimePoint::now()).ns();
    outStats->vsyncPeriod = schedule->period().ns();
    return NO_ERROR;
}

void SurfaceFlinger::setDesiredMode(display::DisplayModeRequest&& desiredMode) {
    const auto mode = desiredMode.mode;
    const auto displayId = mode.modePtr->getPhysicalDisplayId();

    SFTRACE_NAME(ftl::Concat(__func__, ' ', displayId.value).c_str());

    const bool emitEvent = desiredMode.emitEvent;

    using DesiredModeAction = display::DisplayModeController::DesiredModeAction;

    switch (mDisplayModeController.setDesiredMode(displayId, std::move(desiredMode))) {
        case DesiredModeAction::InitiateDisplayModeSwitch: {
            const auto selectorPtr = mDisplayModeController.selectorPtrFor(displayId);
            if (!selectorPtr) break;

            const auto activeMode = selectorPtr->getActiveMode();
            const Fps renderRate = activeMode.fps;

            // DisplayModeController::setDesiredMode updated the render rate, so inform Scheduler.
            mScheduler->setRenderRate(displayId, renderRate, true /* applyImmediately */);

            // Schedule a new frame to initiate the display mode switch.
            scheduleComposite(FrameHint::kNone);

            // Start receiving vsync samples now, so that we can detect a period
            // switch.
            mScheduler->resyncToHardwareVsync(displayId, true /* allowToEnable */,
                                              mode.modePtr.get());

            // As we called to set period, we will call to onRefreshRateChangeCompleted once
            // VsyncController model is locked.
            mScheduler->modulateVsync(displayId, &VsyncModulator::onRefreshRateChangeInitiated);

            mScheduler->updatePhaseConfiguration(displayId, mode.fps);
            mScheduler->setModeChangePending(true);

            // The mode set to switch resolution is not initiated until the display transaction that
            // resizes the display. DM sends this transaction in response to a mode change event, so
            // emit the event now, not when finalizing the mode change as for a refresh rate switch.
            if (FlagManager::getInstance().synced_resolution_switch() &&
                !mode.matchesResolution(activeMode)) {
                mScheduler->onDisplayModeChanged(displayId, mode,
                                                 /*clearContentRequirements*/ true);
            }
            break;
        }
        case DesiredModeAction::InitiateRenderRateSwitch:
            mScheduler->setRenderRate(displayId, mode.fps, /*applyImmediately*/ false);
            mScheduler->updatePhaseConfiguration(displayId, mode.fps);

            if (emitEvent) {
                mScheduler->onDisplayModeChanged(displayId, mode,
                                                 /*clearContentRequirements*/ false);
            }
            break;
        case DesiredModeAction::None:
            break;
    }
}

status_t SurfaceFlinger::setActiveModeFromBackdoor(const sp<display::DisplayToken>& displayToken,
                                                   DisplayModeId modeId, Fps minFps, Fps maxFps) {
    SFTRACE_CALL();

    if (!displayToken) {
        return BAD_VALUE;
    }

    const char* const whence = __func__;
    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(kMainThreadContext) -> status_t {
        const auto displayOpt =
                FTL_FAKE_GUARD(mStateLock,
                               ftl::find_if(mPhysicalDisplays,
                                            PhysicalDisplay::hasToken(displayToken))
                                       .transform(&ftl::to_mapped_ref<PhysicalDisplays>)
                                       .and_then(getDisplayDeviceAndSnapshot()));
        if (!displayOpt) {
            ALOGE("%s: Invalid physical display token %p", whence, displayToken.get());
            return NAME_NOT_FOUND;
        }

        const auto& [display, snapshotRef] = *displayOpt;
        const auto& snapshot = snapshotRef.get();

        const auto fpsOpt = snapshot.displayModes().get(modeId).transform(
                [](const DisplayModePtr& mode) { return mode->getPeakFps(); });

        if (!fpsOpt) {
            ALOGE("%s: Invalid mode %d for display %s", whence, ftl::to_underlying(modeId),
                  to_string(snapshot.displayId()).c_str());
            return BAD_VALUE;
        }

        const Fps fps = *fpsOpt;
        const FpsRange physical = {fps, fps};
        const FpsRange render = {minFps.isValid() ? minFps : fps, maxFps.isValid() ? maxFps : fps};
        const FpsRanges ranges = {physical, render};

        // Keep the old switching type.
        const bool allowGroupSwitching =
                display->refreshRateSelector().getCurrentPolicy().allowGroupSwitching;

        const scheduler::RefreshRateSelector::DisplayManagerPolicy policy{modeId, ranges, ranges,
                                                                          allowGroupSwitching};

        return setDesiredDisplayModeSpecsInternal(display, policy);
    });

    return future.get();
}

bool SurfaceFlinger::finalizeDisplayModeChange(PhysicalDisplayId displayId) {
    SFTRACE_NAME(ftl::Concat(__func__, ' ', displayId.value).c_str());

    const auto pendingModeOpt = mDisplayModeController.getPendingMode(displayId);
    if (!pendingModeOpt) {
        // There is no pending mode change. This can happen if the active
        // display changed and the mode change happened on a different display.
        return true;
    }

    const auto& activeMode = pendingModeOpt->mode;
    const bool resolutionMatch = !FlagManager::getInstance().synced_resolution_switch() ||
            activeMode.matchesResolution(mDisplayModeController.getActiveMode(displayId));

    if (!FlagManager::getInstance().synced_resolution_switch()) {
        if (const auto oldResolution =
                    mDisplayModeController.getActiveMode(displayId).modePtr->getResolution();
            oldResolution != activeMode.modePtr->getResolution()) {
            auto& state =
                    mCurrentState.displays.editValueFor(getPhysicalDisplayTokenLocked(displayId));
            // We need to generate new sequenceId in order to recreate the display (and this
            // way the framebuffer).
            state.sequenceId = DisplayDeviceState{}.sequenceId;
            state.physical->activeMode = activeMode.modePtr.get();
            processDisplayChangesLocked();

            // The DisplayDevice has been destroyed, so abort the commit for the now dead
            // FrameTargeter.
            return false;
        }
    }

    mDisplayModeController.finalizeModeChange(displayId, activeMode.modePtr->getId(),
                                              activeMode.modePtr->getVsyncRate(), activeMode.fps);

    mScheduler->updatePhaseConfiguration(displayId, activeMode.fps);

    // Skip for resolution changes, since the event was already emitted on setting the desired mode.
    if (resolutionMatch && pendingModeOpt->emitEvent) {
        mScheduler->onDisplayModeChanged(displayId, activeMode, /*clearContentRequirements*/ true);
    }

    return true;
}

void SurfaceFlinger::dropModeRequest(PhysicalDisplayId displayId) {
    mDisplayModeController.clearDesiredMode(displayId);
    if (displayId == mActiveDisplayId) {
        // TODO(b/255635711): Check for pending mode changes on other displays.
        mScheduler->setModeChangePending(false);
    }
}

void SurfaceFlinger::applyActiveMode(PhysicalDisplayId displayId) {
    const auto activeModeOpt = mDisplayModeController.getDesiredMode(displayId);
    auto activeModePtr = activeModeOpt->mode.modePtr;
    const auto renderFps = activeModeOpt->mode.fps;

    dropModeRequest(displayId);

    constexpr bool kAllowToEnable = true;
    mScheduler->resyncToHardwareVsync(displayId, kAllowToEnable, std::move(activeModePtr).take());

    mScheduler->setRenderRate(displayId, renderFps, /*applyImmediately*/ true);
    mScheduler->updatePhaseConfiguration(displayId, renderFps);
}

void SurfaceFlinger::initiateDisplayModeChanges() {
    SFTRACE_CALL();

    for (const auto& [displayId, physical] : mPhysicalDisplays) {
        auto desiredModeOpt = mDisplayModeController.getDesiredMode(displayId);
        if (!desiredModeOpt) {
            continue;
        }

        const auto desiredModeId = desiredModeOpt->mode.modePtr->getId();
        const auto displayModePtrOpt = physical.snapshot().displayModes().get(desiredModeId);

        if (!displayModePtrOpt) {
            ALOGW("Desired display mode is no longer supported. Mode ID = %d",
                  ftl::to_underlying(desiredModeId));
            continue;
        }

        ALOGV("%s changing active mode to %d(%s) for display %s", __func__,
              ftl::to_underlying(desiredModeId),
              to_string(displayModePtrOpt->get()->getVsyncRate()).c_str(),
              to_string(displayId).c_str());

        const auto activeMode = mDisplayModeController.getActiveMode(displayId);

        if (!desiredModeOpt->force && desiredModeOpt->mode == activeMode) {
            applyActiveMode(displayId);
            continue;
        }

        const auto selectorPtr = mDisplayModeController.selectorPtrFor(displayId);

        // Desired active mode was set, it is different than the mode currently in use, however
        // allowed modes might have changed by the time we process the refresh.
        // Make sure the desired mode is still allowed
        if (!selectorPtr->isModeAllowed(desiredModeOpt->mode)) {
            dropModeRequest(displayId);
            continue;
        }

        // TODO(b/142753666) use constrains
        hal::VsyncPeriodChangeConstraints constraints;
        constraints.desiredTimeNanos = systemTime();
        constraints.seamlessRequired = false;
        hal::VsyncPeriodChangeTimeline outTimeline;

        // When initiating a resolution change, wait until the commit that resizes the display.
        if (FlagManager::getInstance().synced_resolution_switch() &&
            !activeMode.matchesResolution(desiredModeOpt->mode)) {
            const auto display = getDisplayDeviceLocked(displayId);
            if (display->getSize() != desiredModeOpt->mode.modePtr->getResolution()) {
                continue;
            }
        }

        const auto error =
                mDisplayModeController.initiateModeChange(displayId, std::move(*desiredModeOpt),
                                                          constraints, outTimeline);
        if (error != display::DisplayModeController::ModeChangeResult::Changed) {
            dropModeRequest(displayId);
            if (FlagManager::getInstance().display_config_error_hal() &&
                error == display::DisplayModeController::ModeChangeResult::Rejected) {
                mScheduler->onDisplayModeRejected(displayId, desiredModeId);
            }
            continue;
        }

        selectorPtr->onModeChangeInitiated();
        mScheduler->onNewVsyncPeriodChangeTimeline(outTimeline);

        if (outTimeline.refreshRequired) {
            scheduleComposite(FrameHint::kNone);
        } else {
            // HWC has requested to apply the mode change immediately rather than on the next frame.
            finalizeDisplayModeChange(displayId);

            const auto desiredModeOpt = mDisplayModeController.getDesiredMode(displayId);
            if (desiredModeOpt &&
                mDisplayModeController.getActiveMode(displayId) == desiredModeOpt->mode) {
                applyActiveMode(displayId);
            }
        }
    }
}

void SurfaceFlinger::disableExpensiveRendering() {
    const char* const whence = __func__;
    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) {
        SFTRACE_NAME(whence);
        if (mPowerAdvisor->isUsingExpensiveRendering()) {
            for (const auto& [_, display] : mDisplays) {
                constexpr bool kDisable = false;
                mPowerAdvisor->setExpensiveRenderingExpected(display->getId(), kDisable);
            }
        }
    });

    future.wait();
}

status_t SurfaceFlinger::getDisplayNativePrimaries(const sp<IBinder>& displayToken,
                                                   ui::DisplayPrimaries& primaries) {
    if (!displayToken) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto display = ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
                                 .transform(&ftl::to_mapped_ref<PhysicalDisplays>);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    if (!display.transform(&PhysicalDisplay::isInternal).value()) {
        return INVALID_OPERATION;
    }

    // TODO(b/229846990): For now, assume that all internal displays have the same primaries.
    primaries = mInternalDisplayPrimaries;
    return NO_ERROR;
}

status_t SurfaceFlinger::setActiveColorMode(const sp<IBinder>& displayToken, ui::ColorMode mode) {
    if (!displayToken) {
        return BAD_VALUE;
    }

    const char* const whence = __func__;
    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) -> status_t {
        const auto displayOpt =
                ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
                        .transform(&ftl::to_mapped_ref<PhysicalDisplays>)
                        .and_then(getDisplayDeviceAndSnapshot());

        if (!displayOpt) {
            ALOGE("%s: Invalid physical display token %p", whence, displayToken.get());
            return NAME_NOT_FOUND;
        }

        const auto& [display, snapshotRef] = *displayOpt;
        const auto& snapshot = snapshotRef.get();

        const auto modes = snapshot.filterColorModes(mSupportsWideColor);
        const bool exists = std::find(modes.begin(), modes.end(), mode) != modes.end();

        if (mode < ui::ColorMode::NATIVE || !exists) {
            ALOGE("%s: Invalid color mode %s (%d) for display %s", whence,
                  decodeColorMode(mode).c_str(), mode, to_string(snapshot.displayId()).c_str());
            return BAD_VALUE;
        }

        display->getCompositionDisplay()->setColorProfile(
                {mode, Dataspace::UNKNOWN, RenderIntent::COLORIMETRIC});

        return NO_ERROR;
    });

    // TODO(b/195698395): Propagate error.
    future.wait();
    return NO_ERROR;
}

status_t SurfaceFlinger::getBootDisplayModeSupport(bool* outSupport) const {
    auto future = mScheduler->schedule(
            [this] { return getHwComposer().hasCapability(Capability::BOOT_DISPLAY_CONFIG); });

    *outSupport = future.get();
    return NO_ERROR;
}

status_t SurfaceFlinger::getOverlaySupport(gui::OverlayProperties* outProperties) const {
    const auto& aidlProperties = getHwComposer().getOverlaySupport();
    // convert aidl OverlayProperties to gui::OverlayProperties
    outProperties->combinations.reserve(aidlProperties.combinations.size());
    for (const auto& combination : aidlProperties.combinations) {
        std::vector<int32_t> pixelFormats;
        pixelFormats.reserve(combination.pixelFormats.size());
        std::transform(combination.pixelFormats.cbegin(), combination.pixelFormats.cend(),
                       std::back_inserter(pixelFormats),
                       [](const auto& val) { return static_cast<int32_t>(val); });
        std::vector<int32_t> standards;
        standards.reserve(combination.standards.size());
        std::transform(combination.standards.cbegin(), combination.standards.cend(),
                       std::back_inserter(standards),
                       [](const auto& val) { return static_cast<int32_t>(val); });
        std::vector<int32_t> transfers;
        transfers.reserve(combination.transfers.size());
        std::transform(combination.transfers.cbegin(), combination.transfers.cend(),
                       std::back_inserter(transfers),
                       [](const auto& val) { return static_cast<int32_t>(val); });
        std::vector<int32_t> ranges;
        ranges.reserve(combination.ranges.size());
        std::transform(combination.ranges.cbegin(), combination.ranges.cend(),
                       std::back_inserter(ranges),
                       [](const auto& val) { return static_cast<int32_t>(val); });
        gui::OverlayProperties::SupportedBufferCombinations outCombination;
        outCombination.pixelFormats = std::move(pixelFormats);
        outCombination.standards = std::move(standards);
        outCombination.transfers = std::move(transfers);
        outCombination.ranges = std::move(ranges);
        outProperties->combinations.emplace_back(outCombination);
    }
    outProperties->supportMixedColorSpaces = aidlProperties.supportMixedColorSpaces;
    if (aidlProperties.lutProperties) {
        std::vector<gui::LutProperties> outLutProperties;
        for (auto properties : *aidlProperties.lutProperties) {
            if (!properties) {
                gui::LutProperties currentProperties;
                currentProperties.dimension =
                        static_cast<gui::LutProperties::Dimension>(properties->dimension);
                currentProperties.size = properties->size;
                currentProperties.samplingKeys.reserve(properties->samplingKeys.size());
                std::transform(properties->samplingKeys.cbegin(), properties->samplingKeys.cend(),
                               std::back_inserter(currentProperties.samplingKeys),
                               [](const auto& val) {
                                   return static_cast<gui::LutProperties::SamplingKey>(val);
                               });
                outLutProperties.push_back(std::move(currentProperties));
            }
        }
        outProperties->lutProperties.emplace(outLutProperties.begin(), outLutProperties.end());
    }
    return NO_ERROR;
}

status_t SurfaceFlinger::setBootDisplayMode(const sp<display::DisplayToken>& displayToken,
                                            DisplayModeId modeId) {
    const char* const whence = __func__;
    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) -> status_t {
        const auto snapshotOpt =
                ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
                        .transform(&ftl::to_mapped_ref<PhysicalDisplays>)
                        .transform(&PhysicalDisplay::snapshotRef);

        if (!snapshotOpt) {
            ALOGE("%s: Invalid physical display token %p", whence, displayToken.get());
            return NAME_NOT_FOUND;
        }

        const auto& snapshot = snapshotOpt->get();
        const auto hwcIdOpt = snapshot.displayModes().get(modeId).transform(
                [](const DisplayModePtr& mode) { return mode->getHwcId(); });

        if (!hwcIdOpt) {
            ALOGE("%s: Invalid mode %d for display %s", whence, ftl::to_underlying(modeId),
                  to_string(snapshot.displayId()).c_str());
            return BAD_VALUE;
        }

        return getHwComposer().setBootDisplayMode(snapshot.displayId(), *hwcIdOpt);
    });
    return future.get();
}

status_t SurfaceFlinger::clearBootDisplayMode(const sp<IBinder>& displayToken) {
    const char* const whence = __func__;
    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) -> status_t {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            return getHwComposer().clearBootDisplayMode(*displayId);
        } else {
            ALOGE("%s: Invalid display token %p", whence, displayToken.get());
            return BAD_VALUE;
        }
    });
    return future.get();
}

status_t SurfaceFlinger::getHdrConversionCapabilities(
        std::vector<gui::HdrConversionCapability>* hdrConversionCapabilities) const {
    bool hdrOutputConversionSupport;
    getHdrOutputConversionSupport(&hdrOutputConversionSupport);
    if (hdrOutputConversionSupport == false) {
        ALOGE("hdrOutputConversion is not supported by this device.");
        return INVALID_OPERATION;
    }
    const auto aidlConversionCapability = getHwComposer().getHdrConversionCapabilities();
    for (auto capability : aidlConversionCapability) {
        gui::HdrConversionCapability tempCapability;
        tempCapability.sourceType = static_cast<int>(capability.sourceType);
        tempCapability.outputType = static_cast<int>(capability.outputType);
        tempCapability.addsLatency = capability.addsLatency;
        hdrConversionCapabilities->push_back(tempCapability);
    }
    return NO_ERROR;
}

status_t SurfaceFlinger::setHdrConversionStrategy(
        const gui::HdrConversionStrategy& hdrConversionStrategy,
        int32_t* outPreferredHdrOutputType) {
    bool hdrOutputConversionSupport;
    getHdrOutputConversionSupport(&hdrOutputConversionSupport);
    if (hdrOutputConversionSupport == false) {
        ALOGE("hdrOutputConversion is not supported by this device.");
        return INVALID_OPERATION;
    }
    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) mutable -> status_t {
        using AidlHdrConversionStrategy =
                aidl::android::hardware::graphics::common::HdrConversionStrategy;
        using GuiHdrConversionStrategyTag = gui::HdrConversionStrategy::Tag;
        AidlHdrConversionStrategy aidlConversionStrategy;
        status_t status;
        aidl::android::hardware::graphics::common::Hdr aidlPreferredHdrOutputType;
        switch (hdrConversionStrategy.getTag()) {
            case GuiHdrConversionStrategyTag::passthrough: {
                aidlConversionStrategy.set<AidlHdrConversionStrategy::Tag::passthrough>(
                        hdrConversionStrategy.get<GuiHdrConversionStrategyTag::passthrough>());
                status = getHwComposer().setHdrConversionStrategy(aidlConversionStrategy,
                                                                  &aidlPreferredHdrOutputType);
                *outPreferredHdrOutputType = static_cast<int32_t>(aidlPreferredHdrOutputType);
                return status;
            }
            case GuiHdrConversionStrategyTag::autoAllowedHdrTypes: {
                auto autoHdrTypes =
                        hdrConversionStrategy
                                .get<GuiHdrConversionStrategyTag::autoAllowedHdrTypes>();
                std::vector<aidl::android::hardware::graphics::common::Hdr> aidlAutoHdrTypes;
                for (auto type : autoHdrTypes) {
                    aidlAutoHdrTypes.push_back(
                            static_cast<aidl::android::hardware::graphics::common::Hdr>(type));
                }
                aidlConversionStrategy.set<AidlHdrConversionStrategy::Tag::autoAllowedHdrTypes>(
                        aidlAutoHdrTypes);
                status = getHwComposer().setHdrConversionStrategy(aidlConversionStrategy,
                                                                  &aidlPreferredHdrOutputType);
                *outPreferredHdrOutputType = static_cast<int32_t>(aidlPreferredHdrOutputType);
                return status;
            }
            case GuiHdrConversionStrategyTag::forceHdrConversion: {
                auto forceHdrConversion =
                        hdrConversionStrategy
                                .get<GuiHdrConversionStrategyTag::forceHdrConversion>();
                aidlConversionStrategy.set<AidlHdrConversionStrategy::Tag::forceHdrConversion>(
                        static_cast<aidl::android::hardware::graphics::common::Hdr>(
                                forceHdrConversion));
                status = getHwComposer().setHdrConversionStrategy(aidlConversionStrategy,
                                                                  &aidlPreferredHdrOutputType);
                *outPreferredHdrOutputType = static_cast<int32_t>(aidlPreferredHdrOutputType);
                return status;
            }
        }
    });
    return future.get();
}

status_t SurfaceFlinger::getHdrOutputConversionSupport(bool* outSupport) const {
    auto future = mScheduler->schedule([this] {
        return getHwComposer().hasCapability(Capability::HDR_OUTPUT_CONVERSION_CONFIG);
    });

    *outSupport = future.get();
    return NO_ERROR;
}

void SurfaceFlinger::setAutoLowLatencyMode(const sp<IBinder>& displayToken, bool on) {
    const char* const whence = __func__;
    static_cast<void>(mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            getHwComposer().setAutoLowLatencyMode(*displayId, on);
        } else {
            ALOGE("%s: Invalid display token %p", whence, displayToken.get());
        }
    }));
}

void SurfaceFlinger::setGameContentType(const sp<IBinder>& displayToken, bool on) {
    const char* const whence = __func__;
    static_cast<void>(mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            const auto type = on ? hal::ContentType::GAME : hal::ContentType::NONE;
            getHwComposer().setContentType(*displayId, type);
        } else {
            ALOGE("%s: Invalid display token %p", whence, displayToken.get());
        }
    }));
}

status_t SurfaceFlinger::getMaxLayerPictureProfiles(const sp<IBinder>& displayToken,
                                                    int32_t* outMaxProfiles) {
    const char* const whence = __func__;
    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) {
        const ssize_t index = mCurrentState.displays.indexOfKey(displayToken);
        if (index < 0) {
            ALOGE("%s: Invalid display token %p", whence, displayToken.get());
            return 0;
        }
        const DisplayDeviceState& state = mCurrentState.displays.valueAt(index);
        return state.maxLayerPictureProfiles > 0 ? state.maxLayerPictureProfiles
                : state.hasPictureProcessing     ? 1
                                                 : 0;
    });
    *outMaxProfiles = future.get();
    return NO_ERROR;
}

status_t SurfaceFlinger::overrideHdrTypes(const sp<IBinder>& displayToken,
                                          const std::vector<ui::Hdr>& hdrTypes) {
    Mutex::Autolock lock(mStateLock);

    auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        ALOGE("%s: Invalid display token %p", __func__, displayToken.get());
        return NAME_NOT_FOUND;
    }

    display->overrideHdrTypes(hdrTypes);
    mScheduler->dispatchHotplug(display->getPhysicalId(), scheduler::Scheduler::Hotplug::Connected);
    return NO_ERROR;
}

status_t SurfaceFlinger::onPullAtom(const int32_t atomId, std::vector<uint8_t>* pulledData,
                                    bool* success) {
    *success = mTimeStats->onPullAtom(atomId, pulledData);
    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayedContentSamplingAttributes(const sp<IBinder>& displayToken,
                                                               ui::PixelFormat* outFormat,
                                                               ui::Dataspace* outDataspace,
                                                               uint8_t* outComponentMask) const {
    if (!outFormat || !outDataspace || !outComponentMask) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }

    return getHwComposer().getDisplayedContentSamplingAttributes(*displayId, outFormat,
                                                                 outDataspace, outComponentMask);
}

status_t SurfaceFlinger::setDisplayContentSamplingEnabled(const sp<IBinder>& displayToken,
                                                          bool enable, uint8_t componentMask,
                                                          uint64_t maxFrames) {
    const char* const whence = __func__;
    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) -> status_t {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            return getHwComposer().setDisplayContentSamplingEnabled(*displayId, enable,
                                                                    componentMask, maxFrames);
        } else {
            ALOGE("%s: Invalid display token %p", whence, displayToken.get());
            return NAME_NOT_FOUND;
        }
    });

    return future.get();
}

status_t SurfaceFlinger::getDisplayedContentSample(const sp<IBinder>& displayToken,
                                                   uint64_t maxFrames, uint64_t timestamp,
                                                   DisplayedFrameStats* outStats) const {
    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }

    return getHwComposer().getDisplayedContentSample(*displayId, maxFrames, timestamp, outStats);
}

status_t SurfaceFlinger::getProtectedContentSupport(bool* outSupported) const {
    if (!outSupported) {
        return BAD_VALUE;
    }
    *outSupported = getRenderEngine().supportsProtectedContent();
    return NO_ERROR;
}

status_t SurfaceFlinger::isWideColorDisplay(const sp<IBinder>& displayToken,
                                            bool* outIsWideColorDisplay) const {
    if (!displayToken || !outIsWideColorDisplay) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);
    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    *outIsWideColorDisplay =
            display->isPrimary() ? mSupportsWideColor : display->hasWideColorGamut();
    return NO_ERROR;
}

status_t SurfaceFlinger::getCompositionPreference(
        Dataspace* outDataspace, ui::PixelFormat* outPixelFormat,
        Dataspace* outWideColorGamutDataspace,
        ui::PixelFormat* outWideColorGamutPixelFormat) const {
    *outDataspace = mDefaultCompositionDataspace;
    *outPixelFormat = defaultCompositionPixelFormat;
    *outWideColorGamutDataspace = mWideColorGamutCompositionDataspace;
    *outWideColorGamutPixelFormat = wideColorGamutCompositionPixelFormat;
    return NO_ERROR;
}

status_t SurfaceFlinger::addRegionSamplingListener(const Rect& samplingArea,
                                                   const sp<IBinder>& stopLayerHandle,
                                                   const sp<IRegionSamplingListener>& listener) {
    if (!listener || samplingArea == Rect::INVALID_RECT || samplingArea.isEmpty()) {
        return BAD_VALUE;
    }

    // LayerHandle::getLayer promotes the layer object in a binder thread but we will not destroy
    // the layer here since the caller has a strong ref to the layer's handle.
    const sp<Layer> stopLayer = LayerHandle::getLayer(stopLayerHandle);
    mRegionSamplingThread->addListener(samplingArea,
                                       stopLayer ? stopLayer->getSequence() : UNASSIGNED_LAYER_ID,
                                       listener);
    return NO_ERROR;
}

status_t SurfaceFlinger::removeRegionSamplingListener(const sp<IRegionSamplingListener>& listener) {
    if (!listener) {
        return BAD_VALUE;
    }
    mRegionSamplingThread->removeListener(listener);
    return NO_ERROR;
}

status_t SurfaceFlinger::addFpsListener(int32_t taskId, const sp<gui::IFpsListener>& listener) {
    if (!listener) {
        return BAD_VALUE;
    }

    mFpsReporter->addListener(listener, taskId);
    return NO_ERROR;
}

status_t SurfaceFlinger::removeFpsListener(const sp<gui::IFpsListener>& listener) {
    if (!listener) {
        return BAD_VALUE;
    }
    mFpsReporter->removeListener(listener);
    return NO_ERROR;
}

status_t SurfaceFlinger::addTunnelModeEnabledListener(
        const sp<gui::ITunnelModeEnabledListener>& listener) {
    if (!listener) {
        return BAD_VALUE;
    }

    mTunnelModeEnabledReporter->addListener(listener);
    return NO_ERROR;
}

status_t SurfaceFlinger::removeTunnelModeEnabledListener(
        const sp<gui::ITunnelModeEnabledListener>& listener) {
    if (!listener) {
        return BAD_VALUE;
    }

    mTunnelModeEnabledReporter->removeListener(listener);
    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayBrightnessSupport(const sp<IBinder>& displayToken,
                                                     bool* outSupport) const {
    if (!displayToken || !outSupport) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }
    *outSupport = getHwComposer().hasDisplayCapability(*displayId, DisplayCapability::BRIGHTNESS);
    return NO_ERROR;
}

status_t SurfaceFlinger::setDisplayBrightness(const sp<IBinder>& displayToken,
                                              const gui::DisplayBrightness& brightness) {
    if (!displayToken) {
        return BAD_VALUE;
    }

    const char* const whence = __func__;
    return ftl::Future(mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) {
               // TODO(b/241285876): Validate that the display is physical instead of failing later.
               if (const auto display = getDisplayDeviceLocked(displayToken)) {
                   const bool supportsDisplayBrightnessCommand =
                           getHwComposer().getComposer()->isSupported(
                                   Hwc2::Composer::OptionalFeature::DisplayBrightnessCommand);
                   // If we support applying display brightness as a command, then we also support
                   // dimming SDR layers.
                   if (supportsDisplayBrightnessCommand) {
                       auto compositionDisplay = display->getCompositionDisplay();
                       float currentDimmingRatio =
                               compositionDisplay->editState().sdrWhitePointNits /
                               compositionDisplay->editState().displayBrightnessNits;
                       static constexpr float kDimmingThreshold = 0.02f;
                       if (brightness.sdrWhitePointNits == 0.f ||
                           abs(brightness.sdrWhitePointNits - brightness.displayBrightnessNits) /
                                           brightness.sdrWhitePointNits >=
                                   kDimmingThreshold) {
                           // to optimize, skip brightness setter if the brightness difference ratio
                           // is lower than threshold
                           compositionDisplay
                                   ->setDisplayBrightness(brightness.sdrWhitePointNits,
                                                          brightness.displayBrightnessNits);
                       } else {
                           compositionDisplay->setDisplayBrightness(brightness.sdrWhitePointNits,
                                                                    brightness.sdrWhitePointNits);
                       }

                       FTL_FAKE_GUARD(kMainThreadContext,
                                      display->stageBrightness(brightness.displayBrightness));
                       float currentHdrSdrRatio =
                               compositionDisplay->editState().displayBrightnessNits /
                               compositionDisplay->editState().sdrWhitePointNits;
                       FTL_FAKE_GUARD(kMainThreadContext,
                                      display->updateHdrSdrRatioOverlayRatio(currentHdrSdrRatio));

                       if (brightness.sdrWhitePointNits / brightness.displayBrightnessNits !=
                           currentDimmingRatio) {
                           scheduleComposite(FrameHint::kNone);
                       } else {
                           scheduleCommit(FrameHint::kNone);
                       }
                       return ftl::yield<status_t>(OK);
                   } else {
                       return getHwComposer()
                               .setDisplayBrightness(display->getPhysicalId(),
                                                     brightness.displayBrightness,
                                                     brightness.displayBrightnessNits,
                                                     Hwc2::Composer::DisplayBrightnessOptions{
                                                             .applyImmediately = true});
                   }
               } else {
                   ALOGE("%s: Invalid display token %p", whence, displayToken.get());
                   return ftl::yield<status_t>(NAME_NOT_FOUND);
               }
           }))
            .then([](ftl::Future<status_t> task) { return task; })
            .get();
}

status_t SurfaceFlinger::addHdrLayerInfoListener(const sp<IBinder>& displayToken,
                                                 const sp<gui::IHdrLayerInfoListener>& listener) {
    if (!displayToken) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }
    const auto displayId = display->getId();
    sp<HdrLayerInfoReporter>& hdrInfoReporter = mHdrLayerInfoListeners[displayId];
    if (!hdrInfoReporter) {
        hdrInfoReporter = sp<HdrLayerInfoReporter>::make();
    }
    hdrInfoReporter->addListener(listener);

    mAddingHDRLayerInfoListener = true;
    return OK;
}

status_t SurfaceFlinger::removeHdrLayerInfoListener(
        const sp<IBinder>& displayToken, const sp<gui::IHdrLayerInfoListener>& listener) {
    if (!displayToken) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }
    const auto displayId = display->getId();
    sp<HdrLayerInfoReporter>& hdrInfoReporter = mHdrLayerInfoListeners[displayId];
    if (hdrInfoReporter) {
        hdrInfoReporter->removeListener(listener);
    }
    return OK;
}

status_t SurfaceFlinger::notifyPowerBoost(int32_t boostId) {
    using aidl::android::hardware::power::Boost;
    Boost powerBoost = static_cast<Boost>(boostId);

    if (powerBoost == Boost::INTERACTION) {
        mScheduler->onTouchHint();
    }

    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayDecorationSupport(
        const sp<IBinder>& displayToken,
        std::optional<DisplayDecorationSupport>* outSupport) const {
    if (!displayToken || !outSupport) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }
    getHwComposer().getDisplayDecorationSupport(*displayId, outSupport);
    return NO_ERROR;
}

// ----------------------------------------------------------------------------

sp<IDisplayEventConnection> SurfaceFlinger::createDisplayEventConnection(
        gui::ISurfaceComposer::VsyncSource vsyncSource, EventRegistrationFlags eventRegistration,
        const sp<IBinder>& layerHandle) {
    const auto cycle = [&] {
        if (FlagManager::getInstance().deprecate_vsync_sf()) {
            ALOGW_IF(vsyncSource == gui::ISurfaceComposer::VsyncSource::eVsyncSourceSurfaceFlinger,
                     "requested unsupported config eVsyncSourceSurfaceFlinger");
            return scheduler::Cycle::Render;
        }

        return vsyncSource == gui::ISurfaceComposer::VsyncSource::eVsyncSourceSurfaceFlinger
                ? scheduler::Cycle::LastComposite
                : scheduler::Cycle::Render;
    }();
    return mScheduler->createDisplayEventConnection(cycle, eventRegistration, layerHandle);
}

void SurfaceFlinger::scheduleCommit(FrameHint hint, Duration workDurationSlack) {
    if (hint == FrameHint::kActive) {
        mScheduler->resetIdleTimer();
    }
    mPowerAdvisor->notifyDisplayUpdateImminentAndCpuReset();
    mScheduler->scheduleFrame(workDurationSlack);
}

void SurfaceFlinger::scheduleComposite(FrameHint hint) {
    mMustComposite = true;
    scheduleCommit(hint);
}

void SurfaceFlinger::scheduleRepaint() {
    mGeometryDirty = true;
    scheduleComposite(FrameHint::kActive);
}

void SurfaceFlinger::scheduleSample() {
    static_cast<void>(mScheduler->schedule([this] { sample(); }));
}

void SurfaceFlinger::onComposerHalVsync(hal::HWDisplayId hwcDisplayId, int64_t timestamp,
                                        std::optional<hal::VsyncPeriodNanos> vsyncPeriod) {
    SFTRACE_NAME(vsyncPeriod
                         ? ftl::Concat(__func__, ' ', hwcDisplayId, ' ', *vsyncPeriod, "ns").c_str()
                         : ftl::Concat(__func__, ' ', hwcDisplayId).c_str());

    Mutex::Autolock lock(mStateLock);
    if (const auto displayIdOpt = getHwComposer().onVsync(hwcDisplayId, timestamp)) {
        if (mScheduler->addResyncSample(*displayIdOpt, timestamp, vsyncPeriod)) {
            // period flushed
            mScheduler->modulateVsync(displayIdOpt, &VsyncModulator::onRefreshRateChangeCompleted);
        }
    }
}

void SurfaceFlinger::onComposerHalHotplugEvent(hal::HWDisplayId hwcDisplayId,
                                               DisplayHotplugEvent event) {
    if (event == DisplayHotplugEvent::CONNECTED || event == DisplayHotplugEvent::DISCONNECTED) {
        const HWComposer::HotplugEvent hotplugEvent = event == DisplayHotplugEvent::CONNECTED
                ? HWComposer::HotplugEvent::Connected
                : HWComposer::HotplugEvent::Disconnected;
        {
            std::lock_guard<std::mutex> lock(mHotplugMutex);
            mPendingHotplugEvents.push_back(HotplugEvent{hwcDisplayId, hotplugEvent});
        }

        if (mScheduler) {
            mScheduler->scheduleConfigure();
        }

        return;
    }

    if (event < DisplayHotplugEvent::ERROR_LINK_UNSTABLE) {
        // This needs to be kept in sync with DisplayHotplugEvent to prevent passing new errors.
        const auto errorCode = static_cast<int32_t>(event);
        ALOGW("%s: Unknown hotplug error %d for hwcDisplayId %" PRIu64, __func__, errorCode,
              hwcDisplayId);
        return;
    }

    if (event == DisplayHotplugEvent::ERROR_LINK_UNSTABLE) {
        if (!FlagManager::getInstance().display_config_error_hal()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mHotplugMutex);
            mPendingHotplugEvents.push_back(
                    HotplugEvent{hwcDisplayId, HWComposer::HotplugEvent::LinkUnstable});
        }
        if (mScheduler) {
            mScheduler->scheduleConfigure();
        }
        // do not return to also report the error.
    }

    // TODO(b/311403559): use enum type instead of int
    const auto errorCode = static_cast<int32_t>(event);
    ALOGD("%s: Hotplug error %d for hwcDisplayId %" PRIu64, __func__, errorCode, hwcDisplayId);
    mScheduler->dispatchHotplugError(errorCode);
}

void SurfaceFlinger::onComposerHalVsyncPeriodTimingChanged(
        hal::HWDisplayId, const hal::VsyncPeriodChangeTimeline& timeline) {
    Mutex::Autolock lock(mStateLock);
    mScheduler->onNewVsyncPeriodChangeTimeline(timeline);

    if (timeline.refreshRequired) {
        scheduleComposite(FrameHint::kNone);
    }
}

void SurfaceFlinger::onComposerHalSeamlessPossible(hal::HWDisplayId) {
    // TODO(b/142753666): use constraints when calling to setActiveModeWithConstraints and
    // use this callback to know when to retry in case of SEAMLESS_NOT_POSSIBLE.
}

void SurfaceFlinger::onComposerHalRefresh(hal::HWDisplayId) {
    Mutex::Autolock lock(mStateLock);
    scheduleComposite(FrameHint::kNone);
}

void SurfaceFlinger::onComposerHalVsyncIdle(hal::HWDisplayId) {
    SFTRACE_CALL();
    mScheduler->forceNextResync();
}

void SurfaceFlinger::onRefreshRateChangedDebug(const RefreshRateChangedDebugData& data) {
    SFTRACE_CALL();
    const char* const whence = __func__;
    static_cast<void>(mScheduler->schedule([=, this]() FTL_FAKE_GUARD(mStateLock) FTL_FAKE_GUARD(
                                                   kMainThreadContext) {
        if (const auto displayIdOpt = getHwComposer().toPhysicalDisplayId(data.display)) {
            if (const auto display = getDisplayDeviceLocked(*displayIdOpt)) {
                const Fps refreshRate = Fps::fromPeriodNsecs(
                        getHwComposer().getComposer()->isVrrSupported() ? data.refreshPeriodNanos
                                                                        : data.vsyncPeriodNanos);
                SFTRACE_FORMAT("%s refresh rate = %d", whence, refreshRate.getIntValue());

                const auto renderRate = mDisplayModeController.getActiveMode(*displayIdOpt).fps;
                constexpr bool kSetByHwc = true;
                display->updateRefreshRateOverlayRate(refreshRate, renderRate, kSetByHwc);
            }
        }
    }));
}

void SurfaceFlinger::onComposerHalHdcpLevelsChanged(hal::HWDisplayId hwcDisplayId,
                                                    const HdcpLevels& levels) {
    if (FlagManager::getInstance().hdcp_level_hal()) {
        // TODO(b/362270040): propagate enum constants
        const int32_t maxLevel = static_cast<int32_t>(levels.maxLevel);
        const int32_t connectedLevel = static_cast<int32_t>(levels.connectedLevel);
        ALOGD("%s: HDCP levels changed (connected=%d, max=%d) for hwcDisplayId %" PRIu64, __func__,
              connectedLevel, maxLevel, hwcDisplayId);
        updateHdcpLevels(hwcDisplayId, connectedLevel, maxLevel);
    }
}

void SurfaceFlinger::configure() {
    Mutex::Autolock lock(mStateLock);
    if (configureLocked()) {
        setTransactionFlags(eDisplayTransactionNeeded);
    }
}

void SurfaceFlinger::updateLayerHistory(nsecs_t now) {
    for (const auto& snapshot : mLayerSnapshotBuilder.getSnapshots()) {
        using Changes = frontend::RequestedLayerState::Changes;
        if (snapshot->path.isClone()) {
            continue;
        }

        const bool updateSmallDirty = FlagManager::getInstance().enable_small_area_detection() &&
                ((snapshot->clientChanges & layer_state_t::eSurfaceDamageRegionChanged) ||
                 snapshot->changes.any(Changes::Geometry));

        const bool hasChanges =
                snapshot->changes.any(Changes::FrameRate | Changes::Buffer | Changes::Animation |
                                      Changes::Geometry | Changes::Visibility) ||
                (snapshot->clientChanges & layer_state_t::eDefaultFrameRateCompatibilityChanged) !=
                        0;

        if (!updateSmallDirty && !hasChanges) {
            continue;
        }

        auto it = mLegacyLayers.find(snapshot->sequence);
        LLOG_ALWAYS_FATAL_WITH_TRACE_IF(it == mLegacyLayers.end(),
                                        "Couldn't find layer object for %s",
                                        snapshot->getDebugString().c_str());

        if (updateSmallDirty) {
            // Update small dirty flag while surface damage region or geometry changed
            it->second->setIsSmallDirty(snapshot.get());
        }

        if (!hasChanges) {
            continue;
        }

        const auto layerProps = scheduler::LayerProps{
                .visible = snapshot->isVisible,
                .bounds = snapshot->geomLayerBounds,
                .transform = snapshot->geomLayerTransform,
                .setFrameRateVote = snapshot->frameRate,
                .frameRateSelectionPriority = snapshot->frameRateSelectionPriority,
                .isSmallDirty = snapshot->isSmallDirty,
                .isFrontBuffered = snapshot->isFrontBuffered(),
        };

        if (snapshot->changes.any(Changes::Geometry | Changes::Visibility)) {
            mScheduler->setLayerProperties(snapshot->sequence, layerProps);
        }

        if (snapshot->clientChanges & layer_state_t::eDefaultFrameRateCompatibilityChanged) {
            mScheduler->setDefaultFrameRateCompatibility(snapshot->sequence,
                                                         snapshot->defaultFrameRateCompatibility);
        }

        if (snapshot->changes.test(Changes::Animation)) {
            it->second->recordLayerHistoryAnimationTx(layerProps, now);
        }

        if (snapshot->changes.test(Changes::FrameRate)) {
            it->second->setFrameRateForLayerTree(snapshot->frameRate, layerProps, now);
        }

        if (snapshot->changes.test(Changes::Buffer)) {
            it->second->recordLayerHistoryBufferUpdate(layerProps, now);
        }
    }
}

bool SurfaceFlinger::updateLayerSnapshots(VsyncId vsyncId, nsecs_t frameTimeNs,
                                          bool flushTransactions, bool& outTransactionsAreEmpty)
        EXCLUDES(mStateLock) {
    using Changes = frontend::RequestedLayerState::Changes;
    SFTRACE_CALL();
    SFTRACE_NAME_FOR_TRACK(WorkloadTracer::TRACK_NAME, "Transaction Handling");
    frontend::Update update;
    if (flushTransactions) {
        SFTRACE_NAME("TransactionHandler:flushTransactions");
        // Locking:
        // 1. to prevent onHandleDestroyed from being called while the state lock is held,
        // we must keep a copy of the transactions (specifically the composer
        // states) around outside the scope of the lock.
        // 2. Transactions and created layers do not share a lock. To prevent applying
        // transactions with layers still in the createdLayer queue, collect the transactions
        // before committing the created layers.
        // 3. Transactions can only be flushed after adding layers, since the layer can be a newly
        // created one
        mTransactionHandler.collectTransactions();
        {
            // TODO(b/238781169) lockless queue this and keep order.
            std::scoped_lock<std::mutex> lock(mCreatedLayersLock);
            update.legacyLayers = std::move(mCreatedLayers);
            mCreatedLayers.clear();
            update.newLayers = std::move(mNewLayers);
            mNewLayers.clear();
            update.layerCreationArgs = std::move(mNewLayerArgs);
            mNewLayerArgs.clear();
            update.destroyedHandles = std::move(mDestroyedHandles);
            mDestroyedHandles.clear();
        }

        size_t addedLayers = update.newLayers.size();
        mLayerLifecycleManager.addLayers(std::move(update.newLayers));
        update.transactions = mTransactionHandler.flushTransactions();
        ftl::Flags<adpf::Workload> committedWorkload;
        for (auto& transaction : update.transactions) {
            committedWorkload |= transaction.workloadHint;
        }
        SFTRACE_INSTANT_FOR_TRACK(WorkloadTracer::TRACK_NAME,
                                  ftl::Concat("Layers: +", addedLayers, " -",
                                              update.destroyedHandles.size(),
                                              " txns:", update.transactions.size())
                                          .c_str());

        mPowerAdvisor->setCommittedWorkload(committedWorkload);
        if (mTransactionTracing) {
            mTransactionTracing->addCommittedTransactions(ftl::to_underlying(vsyncId), frameTimeNs,
                                                          update, mFrontEndDisplayInfos,
                                                          mFrontEndDisplayInfosChanged);
        }
        mLayerLifecycleManager.applyTransactions(update.transactions);
        mLayerLifecycleManager.onHandlesDestroyed(update.destroyedHandles);
        for (auto& legacyLayer : update.legacyLayers) {
            mLegacyLayers[legacyLayer->sequence] = legacyLayer;
        }
        mLayerHierarchyBuilder.update(mLayerLifecycleManager);
    }

    // Keep a copy of the drawing state (that is going to be overwritten
    // by commitTransactionsLocked) outside of mStateLock so that the side
    // effects of the State assignment don't happen with mStateLock held,
    // which can cause deadlocks.
    State drawingState(mDrawingState);
    Mutex::Autolock lock(mStateLock);
    bool mustComposite = false;
    mustComposite |= applyAndCommitDisplayTransactionStatesLocked(update.transactions);

    {
        SFTRACE_NAME("LayerSnapshotBuilder:update");
        frontend::LayerSnapshotBuilder::Args
                args{.root = mLayerHierarchyBuilder.getHierarchy(),
                     .layerLifecycleManager = mLayerLifecycleManager,
                     .includeMetadata = mCompositionEngine->getFeatureFlags().test(
                             compositionengine::Feature::kSnapshotLayerMetadata),
                     .displays = mFrontEndDisplayInfos,
                     .displayChanges = mFrontEndDisplayInfosChanged,
                     .globalShadowSettings = mDrawingState.globalShadowSettings,
                     .supportsBlur = mSupportsBlur,
                     .forceFullDamage = mForceFullDamage,
                     .supportedLayerGenericMetadata =
                             getHwComposer().getSupportedLayerGenericMetadata(),
                     .genericLayerMetadataKeyMap = getGenericLayerMetadataKeyMap(),
                     .skipRoundCornersWhenProtected =
                             !getRenderEngine().supportsProtectedContent()};
        mLayerSnapshotBuilder.update(args);
    }

    if (mLayerLifecycleManager.getGlobalChanges().any(Changes::Geometry | Changes::Input |
                                                      Changes::Hierarchy | Changes::Visibility)) {
        mUpdateInputInfo = true;
    }
    if (mLayerLifecycleManager.getGlobalChanges().any(Changes::VisibleRegion | Changes::Hierarchy |
                                                      Changes::Visibility | Changes::Geometry)) {
        mVisibleRegionsDirty = true;
    }
    if (mLayerLifecycleManager.getGlobalChanges().any(Changes::Hierarchy | Changes::FrameRate)) {
        // The frame rate of attached choreographers can only change as a result of a
        // FrameRate change (including when Hierarchy changes).
        mUpdateAttachedChoreographer = true;
    }
    outTransactionsAreEmpty = mLayerLifecycleManager.getGlobalChanges().get() == 0;
    if (FlagManager::getInstance().vrr_bugfix_24q4()) {
        mustComposite |= mLayerLifecycleManager.getGlobalChanges().any(
                frontend::RequestedLayerState::kMustComposite);
    } else {
        mustComposite |= mLayerLifecycleManager.getGlobalChanges().get() != 0;
    }

    bool newDataLatched = false;
    SFTRACE_NAME("DisplayCallbackAndStatsUpdates");
    mustComposite |= applyTransactionsLocked(update.transactions);
    traverseLegacyLayers([&](Layer* layer) { layer->commitTransaction(); });
    const nsecs_t latchTime = systemTime();
    bool unused = false;

    for (auto& layer : mLayerLifecycleManager.getLayers()) {
        if (layer->changes.test(frontend::RequestedLayerState::Changes::Created) &&
            layer->bgColorLayer) {
            sp<Layer> bgColorLayer = getFactory().createEffectLayer(
                    LayerCreationArgs(this, nullptr, layer->name,
                                      ISurfaceComposerClient::eFXSurfaceEffect, LayerMetadata(),
                                      std::make_optional(layer->id), true));
            mLegacyLayers[bgColorLayer->sequence] = bgColorLayer;
        }
        const bool willReleaseBufferOnLatch = layer->willReleaseBufferOnLatch();

        auto it = mLegacyLayers.find(layer->id);
        if (it == mLegacyLayers.end() &&
            layer->changes.test(frontend::RequestedLayerState::Changes::Destroyed)) {
            // Layer handle was created and immediately destroyed. It was destroyed before it
            // was added to the map.
            continue;
        }

        LLOG_ALWAYS_FATAL_WITH_TRACE_IF(it == mLegacyLayers.end(),
                                        "Couldnt find layer object for %s",
                                        layer->getDebugString().c_str());
        if (!layer->hasReadyFrame() && !willReleaseBufferOnLatch) {
            if (!it->second->hasBuffer()) {
                // The last latch time is used to classify a missed frame as buffer stuffing
                // instead of a missed frame. This is used to identify scenarios where we
                // could not latch a buffer or apply a transaction due to backpressure.
                // We only update the latch time for buffer less layers here, the latch time
                // is updated for buffer layers when the buffer is latched.
                it->second->updateLastLatchTime(latchTime);
            }
            continue;
        }

        const bool bgColorOnly =
                !layer->externalTexture && (layer->bgColorLayerId != UNASSIGNED_LAYER_ID);
        if (willReleaseBufferOnLatch) {
            mLayersWithBuffersRemoved.emplace(it->second);
        }
        it->second->latchBufferImpl(unused, latchTime, bgColorOnly);
        newDataLatched = true;

        frontend::LayerSnapshot* snapshot = mLayerSnapshotBuilder.getSnapshot(it->second->sequence);
        gui::GameMode gameMode = (snapshot) ? snapshot->gameMode : gui::GameMode::Unsupported;
        mLayersWithQueuedFrames.emplace(it->second, gameMode);
    }

    updateLayerHistory(latchTime);
    mLayerSnapshotBuilder.forEachSnapshot([&](const frontend::LayerSnapshot& snapshot) {
        // update output's dirty region if a snapshot is visible and its
        // content is dirty or if a snapshot recently became invisible
        if ((snapshot.isVisible && snapshot.contentDirty) ||
            (!snapshot.isVisible && snapshot.changes.test(Changes::Visibility))) {
            Region visibleReg;
            visibleReg.set(snapshot.transformedBoundsWithoutTransparentRegion);
            invalidateLayerStack(snapshot.outputFilter, visibleReg);
        }
    });

    for (auto& destroyedLayer : mLayerLifecycleManager.getDestroyedLayers()) {
        mLegacyLayers.erase(destroyedLayer->id);
    }

    {
        SFTRACE_NAME("LayerLifecycleManager:commitChanges");
        mLayerLifecycleManager.commitChanges();
    }

    // enter boot animation on first buffer latch
    if (CC_UNLIKELY(mBootStage == BootStage::BOOTLOADER && newDataLatched)) {
        ALOGI("Enter boot animation");
        mBootStage = BootStage::BOOTANIMATION;
    }

    mustComposite |= (getTransactionFlags() & ~eTransactionFlushNeeded) || newDataLatched;
    if (mustComposite) {
        commitTransactions();
    }

    return mustComposite;
}

bool SurfaceFlinger::commit(PhysicalDisplayId pacesetterId,
                            const scheduler::FrameTargets& frameTargets) EXCLUDES(mStateLock) {
    const scheduler::FrameTarget& pacesetterFrameTarget = *frameTargets.get(pacesetterId)->get();

    const VsyncId vsyncId = pacesetterFrameTarget.vsyncId();
    SFTRACE_NAME(ftl::Concat(__func__, ' ', ftl::to_underlying(vsyncId)).c_str());

    if (pacesetterFrameTarget.didMissFrame()) {
        mTimeStats->incrementMissedFrames();
    }

    // If a mode set is pending and the fence hasn't fired yet, wait for the next commit.
    if (std::any_of(frameTargets.begin(), frameTargets.end(),
                    [this](const auto& pair) FTL_FAKE_GUARD(kMainThreadContext) {
                        const auto [displayId, target] = pair;
                        return target->isFramePending() &&
                                mDisplayModeController.isModeSetPending(displayId);
                    })) {
        mScheduler->scheduleFrame();
        return false;
    }

    {
        Mutex::Autolock lock(mStateLock);

        for (const auto [displayId, _] : frameTargets) {
            if (mDisplayModeController.isModeSetPending(displayId)) {
                if (!finalizeDisplayModeChange(displayId)) {
                    mScheduler->scheduleFrame();
                    return false;
                }
            }
        }
    }

    if (pacesetterFrameTarget.wouldBackpressureHwc()) {
        if (mPropagateBackpressure && (mBackpressureGpuComposition || pacesetterFrameTarget.didMissHwcFrame())) {
            if (FlagManager::getInstance().vrr_config()) {
                mScheduler->getVsyncSchedule()->getTracker().onFrameMissed(
                        pacesetterFrameTarget.expectedPresentTime());
            }
            const Duration slack = FlagManager::getInstance().allow_n_vsyncs_in_targeter()
                    ? TimePoint::now() - pacesetterFrameTarget.frameBeginTime()
                    : Duration::fromNs(0);
            scheduleCommit(FrameHint::kNone, slack);
            return false;
        }
    }
    SFTRACE_NAME_FOR_TRACK(WorkloadTracer::TRACK_NAME, "Commit");
    const Period vsyncPeriod = mScheduler->getVsyncSchedule()->period();

    // Save this once per commit + composite to ensure consistency
    // TODO (b/240619471): consider removing active display check once AOD is fixed
    const auto activeDisplay = FTL_FAKE_GUARD(mStateLock, getDisplayDeviceLocked(mActiveDisplayId));
    mPowerHintSessionEnabled = mPowerAdvisor->usePowerHintSession() && activeDisplay &&
            activeDisplay->getPowerMode() == hal::PowerMode::ON;
    if (mPowerHintSessionEnabled) {
        mPowerAdvisor->setCommitStart(pacesetterFrameTarget.frameBeginTime());
        mPowerAdvisor->setExpectedPresentTime(pacesetterFrameTarget.expectedPresentTime());

        // Frame delay is how long we should have minus how long we actually have.
        const Duration idealSfWorkDuration =
                mScheduler->vsyncModulator().getVsyncConfig().sfWorkDuration;
        const Duration frameDelay =
                idealSfWorkDuration - pacesetterFrameTarget.expectedFrameDuration();

        mPowerAdvisor->setFrameDelay(frameDelay);
        mPowerAdvisor->setTotalFrameTargetWorkDuration(idealSfWorkDuration);

        const Period idealVsyncPeriod =
                mDisplayModeController.getActiveMode(pacesetterId).fps.getPeriod();
        mPowerAdvisor->updateTargetWorkDuration(idealVsyncPeriod);
    }

    if (mRefreshRateOverlaySpinner || mHdrSdrRatioOverlay) {
        Mutex::Autolock lock(mStateLock);
        if (const auto display = getDefaultDisplayDeviceLocked()) {
            display->animateOverlay();
        }
    }

    // Composite if transactions were committed, or if requested by HWC.
    bool mustComposite = mMustComposite.exchange(false);
    {
        mFrameTimeline->setSfWakeUp(ftl::to_underlying(vsyncId),
                                    pacesetterFrameTarget.frameBeginTime().ns(),
                                    Fps::fromPeriodNsecs(vsyncPeriod.ns()),
                                    mScheduler->getPacesetterRefreshRate());

        const bool flushTransactions = clearTransactionFlags(eTransactionFlushNeeded);
        bool transactionsAreEmpty = false;
        mustComposite |= updateLayerSnapshots(vsyncId, pacesetterFrameTarget.frameBeginTime().ns(),
                                              flushTransactions, transactionsAreEmpty);

        // Tell VsyncTracker that we are going to present this frame before scheduling
        // setTransactionFlags which will schedule another SF frame. This was if the tracker
        // needs to adjust the vsync timeline, it will be done before the next frame.
        if (FlagManager::getInstance().vrr_config() && mustComposite) {
            mScheduler->getVsyncSchedule()
                    ->getTracker()
                    .onFrameBegin(pacesetterFrameTarget.expectedPresentTime(),
                                  pacesetterFrameTarget.lastSignaledFrameTime());
        }
        if (transactionFlushNeeded()) {
            setTransactionFlags(eTransactionFlushNeeded);
        }

        // This has to be called after latchBuffers because we want to include the layers that have
        // been latched in the commit callback
        if (transactionsAreEmpty) {
            // Invoke empty transaction callbacks early.
            mTransactionCallbackInvoker.sendCallbacks(false /* onCommitOnly */);
        } else {
            // Invoke OnCommit callbacks.
            mTransactionCallbackInvoker.sendCallbacks(true /* onCommitOnly */);
        }
    }

    // Layers need to get updated (in the previous line) before we can use them for
    // choosing the refresh rate.
    // Hold mStateLock as chooseRefreshRateForContent promotes wp<Layer> to sp<Layer>
    // and may eventually call to ~Layer() if it holds the last reference
    {
        SFTRACE_NAME_FOR_TRACK(WorkloadTracer::TRACK_NAME, "Refresh Rate Selection");
        bool updateAttachedChoreographer = mUpdateAttachedChoreographer;
        mUpdateAttachedChoreographer = false;

        Mutex::Autolock lock(mStateLock);
        mScheduler->chooseRefreshRateForContent(&mLayerHierarchyBuilder.getHierarchy(),
                                                updateAttachedChoreographer);

        initiateDisplayModeChanges();
    }

    updateCursorAsync();
    if (!mustComposite) {
        updateInputFlinger(vsyncId, pacesetterFrameTarget.frameBeginTime());
    }
    doActiveLayersTracingIfNeeded(false, mVisibleRegionsDirty,
                                  pacesetterFrameTarget.frameBeginTime(), vsyncId);

    mLastCommittedVsyncId = vsyncId;

    persistDisplayBrightness(mustComposite);

    return mustComposite && CC_LIKELY(mBootStage != BootStage::BOOTLOADER);
}

CompositeResultsPerDisplay SurfaceFlinger::composite(
        PhysicalDisplayId pacesetterId, const scheduler::FrameTargeters& frameTargeters) {
    SFTRACE_ASYNC_FOR_TRACK_BEGIN(WorkloadTracer::TRACK_NAME, "Composition",
                                  WorkloadTracer::COMPOSITION_TRACE_COOKIE);
    const scheduler::FrameTarget& pacesetterTarget =
            frameTargeters.get(pacesetterId)->get()->target();

    const VsyncId vsyncId = pacesetterTarget.vsyncId();
    SFTRACE_NAME(ftl::Concat(__func__, ' ', ftl::to_underlying(vsyncId)).c_str());

    compositionengine::CompositionRefreshArgs refreshArgs;
    refreshArgs.powerCallback = this;
    const auto& displays = FTL_FAKE_GUARD(mStateLock, mDisplays);
    refreshArgs.outputs.reserve(displays.size());

    // Track layer stacks of physical displays that might be added to CompositionEngine
    // output. Layer stacks are not tracked in Display when we iterate through
    // frameTargeters. Cross-referencing layer stacks allows us to filter out displays
    // by ID with duplicate layer stacks before adding them to CompositionEngine output.
    ui::DisplayMap<PhysicalDisplayId, ui::LayerStack> physicalDisplayLayerStacks;
    for (auto& [_, display] : displays) {
        const auto id = asPhysicalDisplayId(display->getDisplayIdVariant());
        if (id && frameTargeters.contains(*id)) {
            physicalDisplayLayerStacks.try_emplace(*id, display->getLayerStack());
        }
    }

    // Tracks layer stacks of displays that are added to CompositionEngine output.
    ui::DisplayMap<ui::LayerStack, ftl::Unit> outputLayerStacks;
    auto isUniqueOutputLayerStack = [&outputLayerStacks](DisplayId id, ui::LayerStack layerStack) {
        if (FlagManager::getInstance().reject_dupe_layerstacks()) {
            if (layerStack != ui::INVALID_LAYER_STACK && outputLayerStacks.contains(layerStack)) {
                // TODO: remove log and DisplayId from params once reject_dupe_layerstacks flag is
                // removed
                ALOGD("Existing layer stack ID %d output to another display %" PRIu64
                      ", dropping display from outputs",
                      layerStack.id, id.value);
                return false;
            }
        }

        outputLayerStacks.try_emplace(layerStack);
        return true;
    };

    // Add outputs for physical displays.
    for (const auto& [id, targeter] : frameTargeters) {
        ftl::FakeGuard guard(mStateLock);

        if (const auto display = getCompositionDisplayLocked(id)) {
            const auto layerStack = physicalDisplayLayerStacks.get(id)->get();
            if (isUniqueOutputLayerStack(display->getId(), layerStack)) {
                refreshArgs.outputs.push_back(display);
            }
        }

        refreshArgs.frameTargets.try_emplace(id, &targeter->target());
    }

    std::vector<DisplayId> displayIds;
    for (const auto& [_, display] : displays) {
        displayIds.push_back(display->getId());
        display->tracePowerMode();

        // Add outputs for virtual displays.
        if (display->isVirtual()) {
            const Fps refreshRate = display->getAdjustedRefreshRate();

            if (!refreshRate.isValid() ||
                mScheduler->isVsyncInPhase(pacesetterTarget.frameBeginTime(), refreshRate)) {
                if (isUniqueOutputLayerStack(display->getId(), display->getLayerStack())) {
                    refreshArgs.outputs.push_back(display->getCompositionDisplay());
                }
            }
        }
    }
    mPowerAdvisor->setDisplays(displayIds);

    const bool updateTaskMetadata = mCompositionEngine->getFeatureFlags().test(
            compositionengine::Feature::kSnapshotLayerMetadata);

    refreshArgs.bufferIdsToUncache = std::move(mBufferIdsToUncache);
    refreshArgs.outputColorSetting = mDisplayColorSetting;
    refreshArgs.forceOutputColorMode = mForceColorMode;

    refreshArgs.updatingOutputGeometryThisFrame = mVisibleRegionsDirty;
    refreshArgs.updatingGeometryThisFrame = mGeometryDirty.exchange(false) ||
            mVisibleRegionsDirty || mDrawingState.colorMatrixChanged;
    refreshArgs.internalDisplayRotationFlags = getActiveDisplayRotationFlags();

    if (CC_UNLIKELY(mDrawingState.colorMatrixChanged)) {
        refreshArgs.colorTransformMatrix = mDrawingState.colorMatrix;
        mDrawingState.colorMatrixChanged = false;
    }

    refreshArgs.devOptForceClientComposition = mDebugDisableHWC;

    if (mDebugFlashDelay != 0) {
        refreshArgs.devOptForceClientComposition = true;
        refreshArgs.devOptFlashDirtyRegionsDelay = std::chrono::milliseconds(mDebugFlashDelay);
    }

    // TODO(b/255601557) Update frameInterval per display
    refreshArgs.frameInterval =
            mScheduler->getNextFrameInterval(pacesetterId, pacesetterTarget.expectedPresentTime());
    const auto scheduledFrameResultOpt = mScheduler->getScheduledFrameResult();
    const auto scheduledFrameTimeOpt = scheduledFrameResultOpt
            ? std::optional{scheduledFrameResultOpt->callbackTime}
            : std::nullopt;
    refreshArgs.scheduledFrameTime = scheduledFrameTimeOpt;
    refreshArgs.hasTrustedPresentationListener = mNumTrustedPresentationListeners > 0;
    // Store the present time just before calling to the composition engine so we could notify
    // the scheduler.
    const auto presentTime = systemTime();

    constexpr bool kCursorOnly = false;
    const auto layers = moveSnapshotsToCompositionArgs(refreshArgs, kCursorOnly);

    if (!mVisibleRegionsDirty) {
        for (const auto& [token, display] : FTL_FAKE_GUARD(mStateLock, mDisplays)) {
            auto compositionDisplay = display->getCompositionDisplay();
            if (!compositionDisplay->getState().isEnabled) continue;
            for (const auto* outputLayer : compositionDisplay->getOutputLayersOrderedByZ()) {
                if (outputLayer->getLayerFE().getCompositionState() == nullptr) {
                    // This is unexpected but instead of crashing, capture traces to disk
                    // and recover gracefully by forcing CE to rebuild layer stack.
                    ALOGE("Output layer %s for display %s %" PRIu64 " has a null "
                          "snapshot. Forcing mVisibleRegionsDirty",
                          outputLayer->getLayerFE().getDebugName(),
                          compositionDisplay->getName().c_str(), compositionDisplay->getId().value);

                    TransactionTraceWriter::getInstance().invoke(__func__, /* overwrite= */ false);
                    mVisibleRegionsDirty = true;
                    refreshArgs.updatingOutputGeometryThisFrame = mVisibleRegionsDirty;
                    refreshArgs.updatingGeometryThisFrame = mVisibleRegionsDirty;
                }
            }
        }
    }

    refreshArgs.refreshStartTime = systemTime(SYSTEM_TIME_MONOTONIC);
    for (auto& [layer, layerFE] : layers) {
        layer->onPreComposition(refreshArgs.refreshStartTime);
    }

    for (auto& [layer, layerFE] : layers) {
        attachReleaseFenceFutureToLayer(layer, layerFE,
                                        layerFE->mSnapshot->outputFilter.layerStack);
    }

    refreshArgs.layersWithQueuedFrames.reserve(mLayersWithQueuedFrames.size());
    for (auto& [layer, _] : mLayersWithQueuedFrames) {
        if (const auto& layerFE =
                    layer->getCompositionEngineLayerFE({static_cast<uint32_t>(layer->sequence)})) {
            refreshArgs.layersWithQueuedFrames.push_back(layerFE);
            // Some layers are not displayed and do not yet have a future release fence
            if (layerFE->getReleaseFencePromiseStatus() ==
                        LayerFE::ReleaseFencePromiseStatus::UNINITIALIZED ||
                layerFE->getReleaseFencePromiseStatus() ==
                        LayerFE::ReleaseFencePromiseStatus::FULFILLED) {
                // layerStack is invalid because layer is not on a display
                attachReleaseFenceFutureToLayer(layer.get(), layerFE.get(),
                                                ui::INVALID_LAYER_STACK);
            }
        }
    }

    mCompositionEngine->present(refreshArgs);
    ftl::Flags<adpf::Workload> compositedWorkload;
    if (refreshArgs.updatingGeometryThisFrame || refreshArgs.updatingOutputGeometryThisFrame) {
        compositedWorkload |= adpf::Workload::VISIBLE_REGION;
    }
    if (mFrontEndDisplayInfosChanged) {
        compositedWorkload |= adpf::Workload::DISPLAY_CHANGES;
        SFTRACE_INSTANT_FOR_TRACK(WorkloadTracer::TRACK_NAME, "Display Changes");
    }

    int index = 0;
    ftl::StaticVector<char, WorkloadTracer::COMPOSITION_SUMMARY_SIZE> compositionSummary;
    auto lastLayerStack = ui::INVALID_LAYER_STACK;

    uint64_t prevOverrideBufferId = 0;
    for (auto& [layer, layerFE] : layers) {
        CompositionResult compositionResult{layerFE->stealCompositionResult()};
        if (lastLayerStack != layerFE->mSnapshot->outputFilter.layerStack) {
            if (lastLayerStack != ui::INVALID_LAYER_STACK) {
                // add a space to separate displays
                compositionSummary.push_back(' ');
            }
            lastLayerStack = layerFE->mSnapshot->outputFilter.layerStack;
        }

        // If there are N layers in a cached set they should all share the same buffer id.
        // The first layer in the cached set will be not skipped and layers 1..N-1 will be skipped.
        // We expect all layers in the cached set to be marked as composited by HWC.
        // Here is a made up example of how it is visualized
        //
        //      [b:rrc][s:cc]
        //
        // This should be interpreted to mean that there are 2 cached sets.
        // So there are only 2 non skipped layers -- b and s.
        // The layers rrc and cc are flattened into layers b and s respectively.
        const LayerFE::HwcLayerDebugState& hwcState = layerFE->getLastHwcState();
        if (hwcState.overrideBufferId != prevOverrideBufferId) {
            // End the existing run.
            if (prevOverrideBufferId) {
                compositionSummary.push_back(']');
            }
            // Start a new run.
            if (hwcState.overrideBufferId) {
                compositionSummary.push_back('[');
            }
        }

        compositionSummary.push_back(layerFE->mSnapshot->classifyCompositionForDebug(hwcState));

        if (hwcState.overrideBufferId && !hwcState.wasSkipped) {
            compositionSummary.push_back(':');
        }
        prevOverrideBufferId = hwcState.overrideBufferId;

        if (layerFE->mSnapshot->hasEffect()) {
            compositedWorkload |= adpf::Workload::EFFECTS;
        }

        if (compositionResult.lastClientCompositionFence) {
            layer->setWasClientComposed(compositionResult.lastClientCompositionFence);
        }
        if (com_android_graphics_libgui_flags_apply_picture_profiles()) {
            mActivePictureTracker.onLayerComposed(*layer, *layerFE, compositionResult);
        }
    }
    // End the last run.
    if (prevOverrideBufferId) {
        compositionSummary.push_back(']');
    }

    // Concisely describe the layers composited this frame using single chars. GPU composited layers
    // are uppercase, DPU composited are lowercase. Special chars denote effects (blur, shadow,
    // etc.). This provides a snapshot of the compositing workload.
    SFTRACE_INSTANT_FOR_TRACK(WorkloadTracer::TRACK_NAME,
                              ftl::Concat("Layers: ", layers.size(), " ",
                                          ftl::truncated<WorkloadTracer::COMPOSITION_SUMMARY_SIZE>(
                                                  std::string_view(compositionSummary.begin(),
                                                                   compositionSummary.size())))
                                      .c_str());

    mPowerAdvisor->setCompositedWorkload(compositedWorkload);
    SFTRACE_ASYNC_FOR_TRACK_END(WorkloadTracer::TRACK_NAME,
                                WorkloadTracer::COMPOSITION_TRACE_COOKIE);
    SFTRACE_NAME_FOR_TRACK(WorkloadTracer::TRACK_NAME, "Post Composition");
    SFTRACE_NAME("postComposition");

    if (mDisplayModeController.supportsHdcp()) {
        for (const auto& [id, _] : frameTargeters) {
            ftl::FakeGuard guard(mStateLock);
            if (const auto display = getCompositionDisplayLocked(id)) {
                if (!display->isSecure() && display->hasSecureLayers()) {
                    mDisplayModeController.startHdcpNegotiation(id);
                }
            }
        }
    }

    moveSnapshotsFromCompositionArgs(refreshArgs, layers);
    mTimeStats->recordFrameDuration(pacesetterTarget.frameBeginTime().ns(), systemTime());

    // Send a power hint after presentation is finished.
    if (mPowerHintSessionEnabled) {
        // Now that the current frame has been presented above, PowerAdvisor needs the present time
        // of the previous frame (whose fence is signaled by now) to determine how long the HWC had
        // waited on that fence to retire before presenting.
        // TODO(b/355238809) `presentFenceForPreviousFrame` might not always be signaled (e.g. on
        // devices
        //  where HWC does not block on the previous present fence). Revise this assumtion.
        const auto& previousPresentFence = pacesetterTarget.presentFenceForPreviousFrame();

        mPowerAdvisor->setSfPresentTiming(TimePoint::fromNs(previousPresentFence->getSignalTime()),
                                          TimePoint::now());
        mPowerAdvisor->reportActualWorkDuration();
    }

    if (mScheduler->onCompositionPresented(presentTime)) {
        scheduleComposite(FrameHint::kNone);
    }

    mNotifyExpectedPresentMap[pacesetterId].hintStatus = NotifyExpectedPresentHintStatus::Start;
    onCompositionPresented(pacesetterId, frameTargeters, presentTime);

    const bool hadGpuComposited =
            multiDisplayUnion(mCompositionCoverage).test(CompositionCoverage::Gpu);
    mCompositionCoverage.clear();

    TimeStats::ClientCompositionRecord clientCompositionRecord;

    for (const auto& [_, display] : displays) {
        const auto& state = display->getCompositionDisplay()->getState();
        CompositionCoverageFlags& flags =
                mCompositionCoverage.try_emplace(display->getDisplayIdVariant()).first->second;

        if (state.usesDeviceComposition) {
            flags |= CompositionCoverage::Hwc;
        }

        if (state.reusedClientComposition) {
            flags |= CompositionCoverage::GpuReuse;
        } else if (state.usesClientComposition) {
            flags |= CompositionCoverage::Gpu;
        }

        clientCompositionRecord.predicted |=
                (state.strategyPrediction != CompositionStrategyPredictionState::DISABLED);
        clientCompositionRecord.predictionSucceeded |=
                (state.strategyPrediction == CompositionStrategyPredictionState::SUCCESS);
    }

    const auto coverage = multiDisplayUnion(mCompositionCoverage);
    const bool hasGpuComposited = coverage.test(CompositionCoverage::Gpu);

    clientCompositionRecord.hadClientComposition = hasGpuComposited;
    clientCompositionRecord.reused = coverage.test(CompositionCoverage::GpuReuse);
    clientCompositionRecord.changed = hadGpuComposited != hasGpuComposited;

    mTimeStats->pushCompositionStrategyState(clientCompositionRecord);

    using namespace ftl::flag_operators;

    // TODO(b/160583065): Enable skip validation when SF caches all client composition layers.
    const bool hasGpuUseOrReuse =
            coverage.any(CompositionCoverage::Gpu | CompositionCoverage::GpuReuse);
    mScheduler->modulateVsync({}, &VsyncModulator::onDisplayRefresh, hasGpuUseOrReuse);

    mLayersWithQueuedFrames.clear();
    doActiveLayersTracingIfNeeded(true, mVisibleRegionsDirty, pacesetterTarget.frameBeginTime(),
                                  vsyncId);

    updateInputFlinger(vsyncId, pacesetterTarget.frameBeginTime());

    if (mVisibleRegionsDirty) mHdrLayerInfoChanged = true;
    mVisibleRegionsDirty = false;

    if (mCompositionEngine->needsAnotherUpdate()) {
        scheduleCommit(FrameHint::kNone);
    }

    if (mPowerHintSessionEnabled) {
        mPowerAdvisor->setCompositeEnd(TimePoint::now());
    }

    CompositeResultsPerDisplay resultsPerDisplay;

    // Filter out virtual displays.
    for (const auto& [idVar, coverage] : mCompositionCoverage) {
        if (const auto idOpt = asPhysicalDisplayId(idVar)) {
            resultsPerDisplay.try_emplace(*idOpt, CompositeResult{coverage});
        }
    }

    return resultsPerDisplay;
}

bool SurfaceFlinger::isHdrLayer(const frontend::LayerSnapshot& snapshot) const {
    // Even though the camera layer may be using an HDR transfer function or otherwise be "HDR"
    // the device may need to avoid boosting the brightness as a result of these layers to
    // reduce power consumption during camera recording
    if (mIgnoreHdrCameraLayers) {
        if (snapshot.externalTexture &&
            (snapshot.externalTexture->getUsage() & GRALLOC_USAGE_HW_CAMERA_WRITE) != 0) {
            return false;
        }
    }
    // RANGE_EXTENDED layer may identify themselves as being "HDR"
    // via a desired hdr/sdr ratio
    auto pixelFormat = snapshot.buffer
            ? std::make_optional(static_cast<ui::PixelFormat>(snapshot.buffer->getPixelFormat()))
            : std::nullopt;

    if (getHdrRenderType(snapshot.dataspace, pixelFormat, snapshot.desiredHdrSdrRatio) !=
        HdrRenderType::SDR) {
        return true;
    }
    // If the layer is not allowed to be dimmed, treat it as HDR. WindowManager may disable
    // dimming in order to keep animations invoking SDR screenshots of HDR layers seamless.
    // Treat such tagged layers as HDR so that DisplayManagerService does not try to change
    // the screen brightness
    if (!snapshot.dimmingEnabled) {
        return true;
    }
    return false;
}

ui::Rotation SurfaceFlinger::getPhysicalDisplayOrientation(PhysicalDisplayId displayId,
                                                           bool isPrimary) const {
    if (!mIgnoreHwcPhysicalDisplayOrientation &&
        getHwComposer().getComposer()->isSupported(
                Hwc2::Composer::OptionalFeature::PhysicalDisplayOrientation)) {
        switch (getHwComposer().getPhysicalDisplayOrientation(displayId)) {
            case Hwc2::AidlTransform::ROT_90:
                return ui::ROTATION_90;
            case Hwc2::AidlTransform::ROT_180:
                return ui::ROTATION_180;
            case Hwc2::AidlTransform::ROT_270:
                return ui::ROTATION_270;
            default:
                return ui::ROTATION_0;
        }
    }

    if (isPrimary) {
        using Values = SurfaceFlingerProperties::primary_display_orientation_values;
        switch (primary_display_orientation(Values::ORIENTATION_0)) {
            case Values::ORIENTATION_90:
                return ui::ROTATION_90;
            case Values::ORIENTATION_180:
                return ui::ROTATION_180;
            case Values::ORIENTATION_270:
                return ui::ROTATION_270;
            default:
                break;
        }
    }
    return ui::ROTATION_0;
}

void SurfaceFlinger::onCompositionPresented(PhysicalDisplayId pacesetterId,
                                            const scheduler::FrameTargeters& frameTargeters,
                                            nsecs_t presentStartTime) {
    SFTRACE_CALL();

    ui::PhysicalDisplayMap<PhysicalDisplayId, std::shared_ptr<FenceTime>> presentFences;
    ui::PhysicalDisplayMap<PhysicalDisplayId, const sp<Fence>> gpuCompositionDoneFences;

    for (const auto& [id, targeter] : frameTargeters) {
        auto presentFence = getHwComposer().getPresentFence(id);

        if (id == pacesetterId) {
            mTransactionCallbackInvoker.addPresentFence(presentFence);
        }

        if (auto fenceTime = targeter->setPresentFence(std::move(presentFence));
            fenceTime->isValid()) {
            presentFences.try_emplace(id, std::move(fenceTime));
        }

        ftl::FakeGuard guard(mStateLock);
        if (const auto display = getCompositionDisplayLocked(id);
            display && display->getState().usesClientComposition) {
            gpuCompositionDoneFences
                    .try_emplace(id, display->getRenderSurface()->getClientTargetAcquireFence());
        }
    }

    const auto pacesetterDisplay = FTL_FAKE_GUARD(mStateLock, getDisplayDeviceLocked(pacesetterId));

    std::shared_ptr<FenceTime> pacesetterPresentFenceTime =
            presentFences.get(pacesetterId)
                    .transform([](const FenceTimePtr& ptr) { return ptr; })
                    .value_or(FenceTime::NO_FENCE);

    std::shared_ptr<FenceTime> pacesetterGpuCompositionDoneFenceTime =
            gpuCompositionDoneFences.get(pacesetterId)
                    .transform([](sp<Fence> fence) {
                        return std::make_shared<FenceTime>(std::move(fence));
                    })
                    .value_or(FenceTime::NO_FENCE);

    const TimePoint presentTime = TimePoint::now();

    // Set presentation information before calling Layer::releasePendingBuffer, such that jank
    // information from previous' frame classification is already available when sending jank info
    // to clients, so they get jank classification as early as possible.
    mFrameTimeline->setSfPresent(presentTime.ns(), pacesetterPresentFenceTime,
                                 pacesetterGpuCompositionDoneFenceTime);

    // We use the CompositionEngine::getLastFrameRefreshTimestamp() which might
    // be sampled a little later than when we started doing work for this frame,
    // but that should be okay since CompositorTiming has snapping logic.
    const TimePoint compositeTime =
            TimePoint::fromNs(mCompositionEngine->getLastFrameRefreshTimestamp());
    const Duration presentLatency = mHasReliablePresentFences
            ? mPresentLatencyTracker.trackPendingFrame(compositeTime, pacesetterPresentFenceTime)
            : Duration::zero();

    const auto schedule = mScheduler->getVsyncSchedule();
    const TimePoint vsyncDeadline = schedule->vsyncDeadlineAfter(presentTime);
    const Fps renderRate = pacesetterDisplay->refreshRateSelector().getActiveMode().fps;
    const nsecs_t vsyncPhase = mScheduler->getCurrentVsyncConfigs().late.sfOffset;

    const CompositorTiming compositorTiming(vsyncDeadline.ns(), renderRate.getPeriodNsecs(),
                                            vsyncPhase, presentLatency.ns());

    ui::DisplayMap<ui::LayerStack, const DisplayDevice*> layerStackToDisplay;
    {
        if (!mLayersWithBuffersRemoved.empty() || mNumTrustedPresentationListeners > 0) {
            Mutex::Autolock lock(mStateLock);
            for (const auto& [token, display] : mDisplays) {
                layerStackToDisplay.emplace_or_replace(display->getLayerStack(), display.get());
            }
        }
    }

    for (auto layer : mLayersWithBuffersRemoved) {
        std::vector<ui::LayerStack> previouslyPresentedLayerStacks =
                std::move(layer->mPreviouslyPresentedLayerStacks);
        layer->mPreviouslyPresentedLayerStacks.clear();
        for (auto layerStack : previouslyPresentedLayerStacks) {
            auto optDisplay = layerStackToDisplay.get(layerStack);
            if (optDisplay && !optDisplay->get()->isVirtual()) {
                auto fence = getHwComposer().getPresentFence(optDisplay->get()->getPhysicalId());
                layer->prepareReleaseCallbacks(ftl::yield<FenceResult>(fence),
                                               ui::INVALID_LAYER_STACK);
            }
        }
        layer->releasePendingBuffer(presentTime.ns());
    }
    mLayersWithBuffersRemoved.clear();

    for (const auto& [layer, gameMode] : mLayersWithQueuedFrames) {
        layer->onCompositionPresented(pacesetterDisplay.get(),
                                      pacesetterGpuCompositionDoneFenceTime,
                                      pacesetterPresentFenceTime, compositorTiming, gameMode);
        layer->releasePendingBuffer(presentTime.ns());
    }

    for (const auto& layerEvent : mLayerEvents) {
        auto result =
                stats::stats_write(stats::SURFACE_CONTROL_EVENT,
                                   static_cast<int32_t>(layerEvent.uid),
                                   static_cast<int64_t>(layerEvent.timeSinceLastEvent.count()),
                                   static_cast<int32_t>(layerEvent.dataspace));
        if (result < 0) {
            ALOGW("Failed to report layer event with error: %d", result);
        }
    }
    mLayerEvents.clear();

    std::vector<std::pair<std::shared_ptr<compositionengine::Display>, sp<HdrLayerInfoReporter>>>
            hdrInfoListeners;
    bool haveNewHdrInfoListeners = false;
    ActivePictureTracker::Listeners activePictureListenersToAdd;
    ActivePictureTracker::Listeners activePictureListenersToRemove;
    {
        Mutex::Autolock lock(mStateLock);
        if (mFpsReporter) {
            mFpsReporter->dispatchLayerFps(mLayerHierarchyBuilder.getHierarchy());
        }

        if (mTunnelModeEnabledReporter) {
            mTunnelModeEnabledReporter->updateTunnelModeStatus();
        }

        hdrInfoListeners.reserve(mHdrLayerInfoListeners.size());
        for (const auto& [displayId, reporter] : mHdrLayerInfoListeners) {
            if (reporter && reporter->hasListeners()) {
                if (const auto display = getDisplayDeviceLocked(displayId)) {
                    hdrInfoListeners.emplace_back(display->getCompositionDisplay(), reporter);
                }
            }
        }
        haveNewHdrInfoListeners = mAddingHDRLayerInfoListener; // grab this with state lock
        mAddingHDRLayerInfoListener = false;

        std::swap(activePictureListenersToAdd, mActivePictureListenersToAdd);
        std::swap(activePictureListenersToRemove, mActivePictureListenersToRemove);
    }

    if (haveNewHdrInfoListeners || mHdrLayerInfoChanged) {
        for (auto& [compositionDisplay, listener] : hdrInfoListeners) {
            HdrLayerInfoReporter::HdrLayerInfo info;
            int32_t maxArea = 0;

            auto updateInfoFn =
                    [&](const std::shared_ptr<compositionengine::Display>& compositionDisplay,
                        const frontend::LayerSnapshot& snapshot, const sp<LayerFE>& layerFe) {
                        if (snapshot.isVisible &&
                            compositionDisplay->includesLayer(snapshot.outputFilter)) {
                            if (isHdrLayer(snapshot)) {
                                const auto* outputLayer =
                                        compositionDisplay->getOutputLayerForLayer(layerFe);
                                if (outputLayer) {
                                    const float desiredHdrSdrRatio =
                                            snapshot.desiredHdrSdrRatio < 1.f
                                            ? std::numeric_limits<float>::infinity()
                                            : snapshot.desiredHdrSdrRatio;

                                    float desiredRatio = desiredHdrSdrRatio;
                                    if (FlagManager::getInstance().begone_bright_hlg() &&
                                        desiredHdrSdrRatio ==
                                                std::numeric_limits<float>::infinity()) {
                                        desiredRatio = getIdealizedMaxHeadroom(snapshot.dataspace);
                                    }

                                    info.mergeDesiredRatio(desiredRatio);
                                    info.numberOfHdrLayers++;
                                    const auto displayFrame = outputLayer->getState().displayFrame;
                                    const int32_t area =
                                            displayFrame.width() * displayFrame.height();
                                    if (area > maxArea) {
                                        maxArea = area;
                                        info.maxW = displayFrame.width();
                                        info.maxH = displayFrame.height();
                                    }
                                }
                            }
                        }
                    };

            mLayerSnapshotBuilder.forEachVisibleSnapshot(
                    [&, compositionDisplay = compositionDisplay](
                            std::unique_ptr<frontend::LayerSnapshot>& snapshot)
                            FTL_FAKE_GUARD(kMainThreadContext) {
                                auto it = mLegacyLayers.find(snapshot->sequence);
                                LLOG_ALWAYS_FATAL_WITH_TRACE_IF(it == mLegacyLayers.end(),
                                                                "Couldnt find layer object for %s",
                                                                snapshot->getDebugString().c_str());
                                auto& legacyLayer = it->second;
                                sp<LayerFE> layerFe =
                                        legacyLayer->getCompositionEngineLayerFE(snapshot->path);

                                updateInfoFn(compositionDisplay, *snapshot, layerFe);
                            });
            listener->dispatchHdrLayerInfo(info);
        }
    }
    mHdrLayerInfoChanged = false;

    if (com_android_graphics_libgui_flags_apply_picture_profiles()) {
        // Track, update and notify changes to active pictures - layers that are undergoing
        // picture processing
        mActivePictureTracker.updateAndNotifyListeners(activePictureListenersToAdd,
                                                       activePictureListenersToRemove);
    }

    mTransactionCallbackInvoker.sendCallbacks(false /* onCommitOnly */);
    mTransactionCallbackInvoker.clearCompletedTransactions();

    mTimeStats->incrementTotalFrames();
    mTimeStats->setPresentFenceGlobal(pacesetterPresentFenceTime);

    for (auto&& [id, presentFence] : presentFences) {
        mScheduler->addPresentFence(id, std::move(presentFence));
    }

    const bool hasPacesetterDisplay =
            pacesetterDisplay && getHwComposer().isConnected(pacesetterId);

    if (!hasSyncFramework) {
        if (hasPacesetterDisplay && pacesetterDisplay->isPoweredOn()) {
            mScheduler->enableHardwareVsync(pacesetterId);
        }
    }

    if (hasPacesetterDisplay && !pacesetterDisplay->isPoweredOn()) {
        getRenderEngine().cleanupPostRender();
        return;
    }

    // Cleanup any outstanding resources due to rendering a prior frame.
    getRenderEngine().cleanupPostRender();

    if (mNumTrustedPresentationListeners > 0) {
        // We avoid any reverse traversal upwards so this shouldn't be too expensive
        traverseLegacyLayers([&](Layer* layer) FTL_FAKE_GUARD(kMainThreadContext) {
            if (!layer->hasTrustedPresentationListener()) {
                return;
            }
            const frontend::LayerSnapshot* snapshot =
                    mLayerSnapshotBuilder.getSnapshot(layer->sequence);
            std::optional<const DisplayDevice*> displayOpt = std::nullopt;
            if (snapshot) {
                displayOpt = layerStackToDisplay.get(snapshot->outputFilter.layerStack);
            }
            const DisplayDevice* display = displayOpt.value_or(nullptr);
            layer->updateTrustedPresentationState(display, snapshot,
                                                  nanoseconds_to_milliseconds(presentStartTime),
                                                  false);
        });
    }

    // Even though SFTRACE_INT64 already checks if tracing is enabled, it doesn't prevent the
    // side-effect of getTotalSize(), so we check that again here
    if (SFTRACE_ENABLED()) {
        // getTotalSize returns the total number of buffers that were allocated by SurfaceFlinger
        SFTRACE_INT64("Total Buffer Size", GraphicBufferAllocator::get().getTotalSize());
    }
}

void SurfaceFlinger::commitTransactions() {
    SFTRACE_CALL();
    mDebugInTransaction = systemTime();

    // Here we're guaranteed that some transaction flags are set
    // so we can call commitTransactionsLocked unconditionally.
    // We clear the flags with mStateLock held to guarantee that
    // mCurrentState won't change until the transaction is committed.
    mScheduler->modulateVsync({}, &VsyncModulator::onTransactionCommit);
    commitTransactionsLocked(clearTransactionFlags(eTransactionMask));
    mDebugInTransaction = 0;
}

std::pair<DisplayModes, DisplayModePtr> SurfaceFlinger::loadDisplayModes(
        PhysicalDisplayId displayId) const {
    std::vector<HWComposer::HWCDisplayMode> hwcModes;
    std::optional<hal::HWConfigId> activeModeHwcIdOpt;

    const bool isExternalDisplay = getHwComposer().getDisplayConnectionType(displayId) ==
            ui::DisplayConnectionType::External;

    int attempt = 0;
    constexpr int kMaxAttempts = 3;
    do {
        hwcModes = getHwComposer().getModes(displayId,
                                            scheduler::RefreshRateSelector::kMinSupportedFrameRate
                                                    .getPeriodNsecs());
        const auto activeModeHwcIdExp = getHwComposer().getActiveMode(displayId);
        activeModeHwcIdOpt = activeModeHwcIdExp.value_opt();

        if (isExternalDisplay &&
            activeModeHwcIdExp.has_error([](status_t error) { return error == NO_INIT; })) {
            constexpr nsecs_t k59HzVsyncPeriod = 16949153;
            constexpr nsecs_t k60HzVsyncPeriod = 16666667;

            // DM sets the initial mode for an external display to 1080p@60, but
            // this comes after SF creates its own state (including the
            // DisplayDevice). For now, pick the same mode in order to avoid
            // inconsistent state and unnecessary mode switching.
            // TODO (b/318534874): Let DM decide the initial mode.
            //
            // Try to find 1920x1080 @ 60 Hz
            if (const auto iter = std::find_if(hwcModes.begin(), hwcModes.end(),
                                               [](const auto& mode) {
                                                   return mode.width == 1920 &&
                                                           mode.height == 1080 &&
                                                           mode.vsyncPeriod == k60HzVsyncPeriod;
                                               });
                iter != hwcModes.end()) {
                activeModeHwcIdOpt = iter->hwcId;
                break;
            }

            // Try to find 1920x1080 @ 59-60 Hz
            if (const auto iter = std::find_if(hwcModes.begin(), hwcModes.end(),
                                               [](const auto& mode) {
                                                   return mode.width == 1920 &&
                                                           mode.height == 1080 &&
                                                           mode.vsyncPeriod >= k60HzVsyncPeriod &&
                                                           mode.vsyncPeriod <= k59HzVsyncPeriod;
                                               });
                iter != hwcModes.end()) {
                activeModeHwcIdOpt = iter->hwcId;
                break;
            }

            // The display does not support 1080p@60, and this is the last attempt to pick a display
            // mode. Prefer 60 Hz if available, with the closest resolution to 1080p.
            if (attempt + 1 == kMaxAttempts) {
                std::vector<HWComposer::HWCDisplayMode> hwcModeOpts;

                for (const auto& mode : hwcModes) {
                    if (mode.width <= 1920 && mode.height <= 1080 &&
                        mode.vsyncPeriod >= k60HzVsyncPeriod &&
                        mode.vsyncPeriod <= k59HzVsyncPeriod) {
                        hwcModeOpts.push_back(mode);
                    }
                }

                if (const auto iter = std::max_element(hwcModeOpts.begin(), hwcModeOpts.end(),
                                                       [](const auto& a, const auto& b) {
                                                           const auto aSize = a.width * a.height;
                                                           const auto bSize = b.width * b.height;
                                                           if (aSize < bSize)
                                                               return true;
                                                           else if (aSize == bSize)
                                                               return a.vsyncPeriod > b.vsyncPeriod;
                                                           else
                                                               return false;
                                                       });
                    iter != hwcModeOpts.end()) {
                    activeModeHwcIdOpt = iter->hwcId;
                    break;
                }

                // hwcModeOpts was empty, use hwcModes[0] as the last resort
                activeModeHwcIdOpt = hwcModes[0].hwcId;
            }
        }

        const auto isActiveMode = [activeModeHwcIdOpt](const HWComposer::HWCDisplayMode& mode) {
            return mode.hwcId == activeModeHwcIdOpt;
        };

        if (std::any_of(hwcModes.begin(), hwcModes.end(), isActiveMode)) {
            break;
        }
    } while (++attempt < kMaxAttempts);

    if (attempt == kMaxAttempts) {
        const std::string activeMode =
                activeModeHwcIdOpt ? std::to_string(*activeModeHwcIdOpt) : "unknown"s;
        ALOGE("HWC failed to report an active mode that is supported: activeModeHwcId=%s, "
              "hwcModes={%s}",
              activeMode.c_str(), base::Join(hwcModes, ", ").c_str());
        return {};
    }

    const DisplayModes oldModes = mPhysicalDisplays.get(displayId)
                                          .transform([](const PhysicalDisplay& display) {
                                              return display.snapshot().displayModes();
                                          })
                                          .value_or(DisplayModes{});

    DisplayModeId nextModeId = std::accumulate(oldModes.begin(), oldModes.end(), DisplayModeId(-1),
                                               [](DisplayModeId max, const auto& pair) {
                                                   return std::max(max, pair.first);
                                               });
    ++nextModeId;

    DisplayModes newModes;
    for (const auto& hwcMode : hwcModes) {
        const auto id = nextModeId++;
        OutputType hdrOutputType = FlagManager::getInstance().connected_display_hdr()
                ? hwcMode.hdrOutputType
                : OutputType::INVALID;
        newModes.try_emplace(id,
                             DisplayMode::Builder(hwcMode.hwcId)
                                     .setId(id)
                                     .setPhysicalDisplayId(displayId)
                                     .setResolution({hwcMode.width, hwcMode.height})
                                     .setVsyncPeriod(hwcMode.vsyncPeriod)
                                     .setVrrConfig(hwcMode.vrrConfig)
                                     .setDpiX(hwcMode.dpiX)
                                     .setDpiY(hwcMode.dpiY)
                                     .setGroup(hwcMode.configGroup)
                                     .setHdrOutputType(hdrOutputType)
                                     .build());
    }

    const bool sameModes =
            std::equal(newModes.begin(), newModes.end(), oldModes.begin(), oldModes.end(),
                       [](const auto& lhs, const auto& rhs) {
                           return equalsExceptDisplayModeId(*lhs.second, *rhs.second);
                       });

    // Keep IDs if modes have not changed.
    const auto& modes = sameModes ? oldModes : newModes;
    const DisplayModePtr activeMode =
            std::find_if(modes.begin(), modes.end(), [activeModeHwcIdOpt](const auto& pair) {
                return pair.second->getHwcId() == activeModeHwcIdOpt;
            })->second;

    if (isExternalDisplay) {
        ALOGI("External display %s initial mode: {%s}", to_string(displayId).c_str(),
              to_string(*activeMode).c_str());
    }
    return {modes, activeMode};
}

bool SurfaceFlinger::configureLocked() {
    std::vector<HotplugEvent> events;
    {
        std::lock_guard<std::mutex> lock(mHotplugMutex);
        events = std::move(mPendingHotplugEvents);
    }

    for (const auto [hwcDisplayId, event] : events) {
        if (auto info = getHwComposer().onHotplug(hwcDisplayId, event)) {
            const auto displayId = info->id;
            const ftl::Concat displayString("display ", displayId.value, "(HAL ID ", hwcDisplayId,
                                            ')');
            // TODO: b/393126541 - replace if with switch as all cases are handled.
            if (event == HWComposer::HotplugEvent::Connected ||
                event == HWComposer::HotplugEvent::LinkUnstable) {
                const auto activeModeIdOpt =
                        processHotplugConnect(displayId, hwcDisplayId, std::move(*info),
                                              displayString.c_str(), event);
                if (!activeModeIdOpt) {
                    mScheduler->dispatchHotplugError(
                            static_cast<int32_t>(DisplayHotplugEvent::ERROR_UNKNOWN));
                    getHwComposer().disconnectDisplay(displayId);
                    continue;
                }

                const auto [kernelIdleTimerController, idleTimerTimeoutMs] =
                        getKernelIdleTimerProperties(displayId);

                using Config = scheduler::RefreshRateSelector::Config;
                const Config config =
                        {.enableFrameRateOverride = sysprop::enable_frame_rate_override(true)
                                 ? Config::FrameRateOverride::Enabled
                                 : Config::FrameRateOverride::Disabled,
                         .frameRateMultipleThreshold =
                                 base::GetIntProperty("debug.sf.frame_rate_multiple_threshold"s, 0),
                         .legacyIdleTimerTimeout = idleTimerTimeoutMs,
                         .kernelIdleTimerController = kernelIdleTimerController};

                const auto snapshotOpt =
                        mPhysicalDisplays.get(displayId).transform(&PhysicalDisplay::snapshotRef);
                LOG_ALWAYS_FATAL_IF(!snapshotOpt);

                mDisplayModeController.registerDisplay(*snapshotOpt, *activeModeIdOpt, config);
            } else { // event == HWComposer::HotplugEvent::Disconnected
                // Unregister before destroying the DisplaySnapshot below.
                mDisplayModeController.unregisterDisplay(displayId);

                processHotplugDisconnect(displayId, displayString.c_str());
            }
        }
    }

    return !events.empty();
}

std::optional<DisplayModeId> SurfaceFlinger::processHotplugConnect(PhysicalDisplayId displayId,
                                                                   hal::HWDisplayId hwcDisplayId,
                                                                   DisplayIdentificationInfo&& info,
                                                                   const char* displayString,
                                                                   HWComposer::HotplugEvent event) {
    auto [displayModes, activeMode] = loadDisplayModes(displayId);
    if (!activeMode) {
        ALOGE("Failed to hotplug %s", displayString);
        return std::nullopt;
    }

    const DisplayModeId activeModeId = activeMode->getId();
    ui::ColorModes colorModes = getHwComposer().getColorModes(displayId);

    if (const auto displayOpt = mPhysicalDisplays.get(displayId)) {
        const auto& display = displayOpt->get();
        const auto& snapshot = display.snapshot();
        const uint8_t port = snapshot.port();

        std::optional<DeviceProductInfo> deviceProductInfo;
        if (getHwComposer().updatesDeviceProductInfoOnHotplugReconnect()) {
            deviceProductInfo = std::move(info.deviceProductInfo);
        } else {
            deviceProductInfo = snapshot.deviceProductInfo();
        }

        // Use the cached port via snapshot because we are updating an existing
        // display on reconnect.
        const auto it =
                mPhysicalDisplays.try_replace(displayId, display.token(), displayId, port,
                                              snapshot.connectionType(), std::move(displayModes),
                                              std::move(colorModes), std::move(deviceProductInfo));

        auto& state = mCurrentState.displays.editValueFor(it->second.token());
        state.sequenceId = DisplayDeviceState{}.sequenceId; // Generate new sequenceId.
        state.physical->activeMode = std::move(activeMode);
        state.physical->port = port;
        ALOGI("Reconnecting %s", displayString);
        return activeModeId;
    } else if (event == HWComposer::HotplugEvent::LinkUnstable) {
        ALOGE("Failed to reconnect unknown %s", displayString);
        return std::nullopt;
    }

    const sp<IBinder> token = sp<BBinder>::make();
    const ui::DisplayConnectionType connectionType =
            getHwComposer().getDisplayConnectionType(displayId);

    mPhysicalDisplays.try_emplace(displayId, token, displayId, info.port, connectionType,
                                  std::move(displayModes), std::move(colorModes),
                                  std::move(info.deviceProductInfo));

    DisplayDeviceState state;
    state.physical = {.id = displayId,
                      .hwcDisplayId = hwcDisplayId,
                      .port = info.port,
                      .activeMode = std::move(activeMode)};
    // TODO: b/349703362 - Remove first condition when HDCP aidl APIs are enforced
    state.isSecure = !mDisplayModeController.supportsHdcp() ||
            connectionType == ui::DisplayConnectionType::Internal;
    state.isProtected = true;
    state.displayName = std::move(info.name);
    state.maxLayerPictureProfiles = getHwComposer().getMaxLayerPictureProfiles(displayId);
    state.hasPictureProcessing =
            getHwComposer().hasDisplayCapability(displayId, DisplayCapability::PICTURE_PROCESSING);
    mCurrentState.displays.add(token, state);
    ALOGI("Connecting %s", displayString);
    return activeModeId;
}

void SurfaceFlinger::processHotplugDisconnect(PhysicalDisplayId displayId,
                                              const char* displayString) {
    ALOGI("Disconnecting %s", displayString);

    const auto displayOpt = mPhysicalDisplays.get(displayId);
    LOG_ALWAYS_FATAL_IF(!displayOpt);
    const auto& display = displayOpt->get();

    if (const ssize_t index = mCurrentState.displays.indexOfKey(display.token()); index >= 0) {
        mCurrentState.displays.removeItemsAt(index);
    }

    mPhysicalDisplays.erase(displayId);
}

sp<DisplayDevice> SurfaceFlinger::setupNewDisplayDeviceInternal(
        const wp<IBinder>& displayToken,
        std::shared_ptr<compositionengine::Display> compositionDisplay,
        const DisplayDeviceState& state,
        const sp<compositionengine::DisplaySurface>& displaySurface,
        const sp<IGraphicBufferProducer>& producer) {
    DisplayDeviceCreationArgs creationArgs(sp<SurfaceFlinger>::fromExisting(this), getHwComposer(),
                                           displayToken, compositionDisplay);
    creationArgs.sequenceId = state.sequenceId;
    creationArgs.isSecure = state.isSecure;
    creationArgs.isProtected = state.isProtected;
    creationArgs.displaySurface = displaySurface;
    creationArgs.hasWideColorGamut = false;
    creationArgs.supportedPerFrameMetadata = 0;

    if (const auto physicalIdOpt =
                compositionDisplay->getDisplayIdVariant().and_then(asPhysicalDisplayId)) {
        const auto physicalId = *physicalIdOpt;

        creationArgs.isPrimary = physicalId == getPrimaryDisplayIdLocked();
        creationArgs.refreshRateSelector =
                FTL_FAKE_GUARD(kMainThreadContext,
                               mDisplayModeController.selectorPtrFor(physicalId));
        creationArgs.physicalOrientation =
                getPhysicalDisplayOrientation(physicalId, creationArgs.isPrimary);
        ALOGV("Display Orientation: %s", toCString(creationArgs.physicalOrientation));

        mPhysicalDisplays.get(physicalId)
                .transform(&PhysicalDisplay::snapshotRef)
                .transform(ftl::unit_fn([&](const display::DisplaySnapshot& snapshot) {
                    for (const auto mode : snapshot.colorModes()) {
                        creationArgs.hasWideColorGamut |= ui::isWideColorMode(mode);
                        creationArgs.hwcColorModes
                                .emplace(mode, getHwComposer().getRenderIntents(physicalId, mode));
                    }
                }));
    }

    if (const auto id = compositionDisplay->getDisplayIdVariant().and_then(
                asHalDisplayId<DisplayIdVariant>)) {
        getHwComposer().getHdrCapabilities(*id, &creationArgs.hdrCapabilities);
        creationArgs.supportedPerFrameMetadata = getHwComposer().getSupportedPerFrameMetadata(*id);
    }

    auto nativeWindowSurface = getFactory().createNativeWindowSurface(producer);
    auto nativeWindow = nativeWindowSurface->getNativeWindow();
    creationArgs.nativeWindow = nativeWindow;

    // Make sure that composition can never be stalled by a virtual display
    // consumer that isn't processing buffers fast enough. We have to do this
    // here, in case the display is composed entirely by HWC.
    if (state.isVirtual()) {
        nativeWindow->setSwapInterval(nativeWindow.get(), 0);
    }

    if (FlagManager::getInstance().correct_virtual_display_power_state()) {
        creationArgs.initialPowerMode = state.initialPowerMode;
    } else {
        creationArgs.initialPowerMode =
                state.isVirtual() ? hal::PowerMode::ON : hal::PowerMode::OFF;
    }

    creationArgs.requestedRefreshRate = state.requestedRefreshRate;

    sp<DisplayDevice> display = getFactory().createDisplayDevice(creationArgs);

    nativeWindowSurface->preallocateBuffers();

    ui::ColorMode defaultColorMode = ui::ColorMode::NATIVE;
    Dataspace defaultDataSpace = Dataspace::UNKNOWN;
    if (display->hasWideColorGamut()) {
        defaultColorMode = ui::ColorMode::SRGB;
        defaultDataSpace = Dataspace::V0_SRGB;
    }
    display->getCompositionDisplay()->setColorProfile(
            compositionengine::Output::ColorProfile{defaultColorMode, defaultDataSpace,
                                                    RenderIntent::COLORIMETRIC});

    if (const auto& physical = state.physical) {
        const auto& mode = *physical->activeMode;
        mDisplayModeController.setActiveMode(physical->id, mode.getId(), mode.getVsyncRate(),
                                             mode.getPeakFps());
    }

    display->setLayerFilter(
            makeLayerFilterForDisplay(display->getDisplayIdVariant(), state.layerStack));
    display->setProjection(state.orientation, state.layerStackSpaceRect,
                           state.orientedDisplaySpaceRect);
    display->setDisplayName(state.displayName);
    display->setOptimizationPolicy(state.optimizationPolicy);
    display->setFlags(state.flags);

    return display;
}

void SurfaceFlinger::incRefreshableDisplays() {
    if (FlagManager::getInstance().no_vsyncs_on_screen_off()) {
        mRefreshableDisplays++;
        if (mRefreshableDisplays == 1) {
            ftl::FakeGuard guard(kMainThreadContext);
            mScheduler->omitVsyncDispatching(false);
        }
    }
}

void SurfaceFlinger::decRefreshableDisplays() {
    if (FlagManager::getInstance().no_vsyncs_on_screen_off()) {
        mRefreshableDisplays--;
        if (mRefreshableDisplays == 0) {
            ftl::FakeGuard guard(kMainThreadContext);
            mScheduler->omitVsyncDispatching(true);
        }
    }
}

void SurfaceFlinger::processDisplayAdded(const wp<IBinder>& displayToken,
                                         const DisplayDeviceState& state) {
#ifdef QCOM_UM_FAMILY
    bool canAllocateHwcForVDS = false;
#else
    bool canAllocateHwcForVDS = true;
#endif
    ui::Size resolution(0, 0);
    ui::PixelFormat pixelFormat = static_cast<ui::PixelFormat>(PIXEL_FORMAT_UNKNOWN);
    if (state.physical) {
        resolution = state.physical->activeMode->getResolution();
        pixelFormat = static_cast<ui::PixelFormat>(PIXEL_FORMAT_RGBA_8888);
    } else if (state.surface != nullptr) {
        int status = state.surface->query(NATIVE_WINDOW_WIDTH, &resolution.width);
        ALOGE_IF(status != NO_ERROR, "Unable to query width (%d)", status);
        status = state.surface->query(NATIVE_WINDOW_HEIGHT, &resolution.height);
        ALOGE_IF(status != NO_ERROR, "Unable to query height (%d)", status);
        int format;
        status = state.surface->query(NATIVE_WINDOW_FORMAT, &format);
        ALOGE_IF(status != NO_ERROR, "Unable to query format (%d)", status);
        pixelFormat = static_cast<ui::PixelFormat>(format);
#ifdef QCOM_UM_FAMILY
        // Check if VDS is allowed to use HWC
        size_t maxVirtualDisplaySize = getHwComposer().getMaxVirtualDisplayDimension();
        if (maxVirtualDisplaySize == 0 || ((uint64_t)resolution.width <= maxVirtualDisplaySize &&
            (uint64_t)resolution.height <= maxVirtualDisplaySize)) {
            uint64_t usage = 0;
            // Replace with native_window_get_consumer_usage ?
            status = state .surface->getConsumerUsage(&usage);
            ALOGW_IF(status != NO_ERROR, "Unable to query usage (%d)", status);
            if ((status == NO_ERROR) && canAllocateHwcDisplayIdForVDS(usage)) {
                canAllocateHwcForVDS = true;
            }
        }
#endif
    } else {
        // Virtual displays without a surface are dormant:
        // they have external state (layer stack, projection,
        // etc.) but no internal state (i.e. a DisplayDevice).
        ALOGD("Not adding dormant virtual display with token %p: %s", displayToken.unsafe_get(),
              state.displayName.c_str());
        return;
    }

    compositionengine::DisplayCreationArgsBuilder builder;
    std::optional<VirtualDisplayIdVariant> virtualDisplayIdVariantOpt;
    if (const auto& physical = state.physical) {
        builder.setId(physical->id);
    } else {
        virtualDisplayIdVariantOpt =
                acquireVirtualDisplay(resolution, pixelFormat, state.uniqueId, builder);
    }

    builder.setPixels(resolution);
    builder.setIsSecure(state.isSecure);
    builder.setIsProtected(state.isProtected);
    builder.setHasPictureProcessing(state.hasPictureProcessing);
    builder.setMaxLayerPictureProfiles(state.maxLayerPictureProfiles);
    builder.setPowerAdvisor(mPowerAdvisor.get());
    builder.setName(state.displayName);
    auto compositionDisplay = getCompositionEngine().createDisplay(builder.build());
    compositionDisplay->setLayerCachingEnabled(mLayerCachingEnabled);

    sp<compositionengine::DisplaySurface> displaySurface;
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferProducer> bqProducer;
    sp<IGraphicBufferConsumer> bqConsumer;
    getFactory().createBufferQueue(&bqProducer, &bqConsumer, /*consumerIsSurfaceFlinger =*/false);

    if (state.isVirtual()) {
        LOG_FATAL_IF(!virtualDisplayIdVariantOpt);
        auto surface = sp<VirtualDisplaySurface>::make(getHwComposer(), *virtualDisplayIdVariantOpt,
                                                       state.surface, bqProducer, bqConsumer,
                                                       state.displayName, state.isSecure);
        displaySurface = surface;
        producer = std::move(surface);
    } else {
        ALOGE_IF(state.surface != nullptr,
                 "adding a supported display, but rendering "
                 "surface is provided (%p), ignoring it",
                 state.surface.get());
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
        const auto frameBufferSurface =
                sp<FramebufferSurface>::make(getHwComposer(), state.physical->id, bqProducer,
                                             bqConsumer,
                                             state.physical->activeMode->getResolution(),
                                             ui::Size(maxGraphicsWidth, maxGraphicsHeight));
        displaySurface = frameBufferSurface;
        producer = frameBufferSurface->getSurface()->getIGraphicBufferProducer();
#else
        displaySurface =
                sp<FramebufferSurface>::make(getHwComposer(), state.physical->id, bqConsumer,
                                             state.physical->activeMode->getResolution(),
                                             ui::Size(maxGraphicsWidth, maxGraphicsHeight));
        producer = bqProducer;
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
    }

    LOG_FATAL_IF(!displaySurface);
    auto display = setupNewDisplayDeviceInternal(displayToken, std::move(compositionDisplay), state,
                                                 displaySurface, producer);

    if (mScheduler && !display->isVirtual()) {
        // For hotplug reconnect, renew the registration since display modes have been reloaded.
        mScheduler->registerDisplay(display->getPhysicalId(), display->holdRefreshRateSelector(),
                                    mActiveDisplayId);
    }

    if (display->isVirtual()) {
        display->adjustRefreshRate(mScheduler->getPacesetterRefreshRate());
    }

    if (display->isRefreshable()) {
        incRefreshableDisplays();
    }

    if (FlagManager::getInstance().correct_virtual_display_power_state()) {
        applyOptimizationPolicy(__func__);
    }

    mDisplays.try_emplace(displayToken, std::move(display));

    // For an external display, loadDisplayModes already attempted to select the same mode
    // as DM, but SF still needs to be updated to match.
    // TODO (b/318534874): Let DM decide the initial mode.
    if (const auto& physical = state.physical; mScheduler && physical) {
        const bool isInternalDisplay = mPhysicalDisplays.get(physical->id)
                                               .transform(&PhysicalDisplay::isInternal)
                                               .value_or(false);

        if (!isInternalDisplay) {
            auto activeModePtr = physical->activeMode;
            const auto fps = activeModePtr->getPeakFps();

            setDesiredMode(
                    {.mode = scheduler::FrameRateMode{fps,
                                                      ftl::as_non_null(std::move(activeModePtr))},
                     .emitEvent = false,
                     .force = true});
        }
    }
}

void SurfaceFlinger::processDisplayRemoved(const wp<IBinder>& displayToken) {
    auto display = getDisplayDeviceLocked(displayToken);
    if (display) {
        display->disconnect();

        if (const auto virtualDisplayIdVariant = display->getVirtualDisplayIdVariant()) {
            releaseVirtualDisplay(*virtualDisplayIdVariant);
        } else {
            mScheduler->unregisterDisplay(display->getPhysicalId(), mActiveDisplayId);
        }

        if (display->isRefreshable()) {
            decRefreshableDisplays();
        }
    }

    mDisplays.erase(displayToken);

    if (display && display->isVirtual()) {
        static_cast<void>(mScheduler->schedule([display = std::move(display)] {
            // Destroy the display without holding the mStateLock.
            // This is a temporary solution until we can manage transaction queues without
            // holding the mStateLock.
            // With blast, the IGBP that is passed to the VirtualDisplaySurface is owned by the
            // client. When the IGBP is disconnected, its buffer cache in SF will be cleared
            // via SurfaceComposerClient::doUncacheBufferTransaction. This call from the client
            // ends up running on the main thread causing a deadlock since setTransactionstate
            // will try to acquire the mStateLock. Instead we extend the lifetime of
            // DisplayDevice and destroy it in the main thread without holding the mStateLock.
            // The display will be disconnected and removed from the mDisplays list so it will
            // not be accessible.
        }));
    }

    if (FlagManager::getInstance().correct_virtual_display_power_state()) {
        applyOptimizationPolicy(__func__);
    }
}

void SurfaceFlinger::processDisplayChanged(const wp<IBinder>& displayToken,
                                           const DisplayDeviceState& currentState,
                                           const DisplayDeviceState& drawingState) {
    const sp<IBinder> currentBinder = IInterface::asBinder(currentState.surface);
    const sp<IBinder> drawingBinder = IInterface::asBinder(drawingState.surface);

    // Recreate the DisplayDevice if the surface or sequence ID changed.
    if (currentBinder != drawingBinder || currentState.sequenceId != drawingState.sequenceId) {
        if (const auto display = getDisplayDeviceLocked(displayToken)) {
            display->disconnect();
            if (const auto virtualDisplayIdVariant = display->getVirtualDisplayIdVariant()) {
                releaseVirtualDisplay(*virtualDisplayIdVariant);
            }

            if (display->isRefreshable()) {
                decRefreshableDisplays();
            }
        }

        mDisplays.erase(displayToken);

        if (const auto& physical = currentState.physical) {
            getHwComposer().allocatePhysicalDisplay(physical->hwcDisplayId, physical->id,
                                                    physical->port, /*physicalSize=*/std::nullopt);
        }

        processDisplayAdded(displayToken, currentState);

        if (currentState.physical) {
            const auto display = getDisplayDeviceLocked(displayToken);
            if (!mSkipPowerOnForQuiescent) {
                setPhysicalDisplayPowerMode(display, hal::PowerMode::ON);
            }

            if (display->getPhysicalId() == mActiveDisplayId) {
                onActiveDisplayChangedLocked(nullptr, *display);
            }
        }
        return;
    }

    if (const auto display = getDisplayDeviceLocked(displayToken)) {
        if (currentState.layerStack != drawingState.layerStack) {
            display->setLayerFilter(makeLayerFilterForDisplay(display->getDisplayIdVariant(),
                                                              currentState.layerStack));
        }
        if (currentState.flags != drawingState.flags) {
            display->setFlags(currentState.flags);
        }

        const auto updateDisplaySize = [&]() {
            if (currentState.width != drawingState.width ||
                currentState.height != drawingState.height) {
                const ui::Size resolution = ui::Size(currentState.width, currentState.height);

                // Resize the framebuffer. For a virtual display, always do so. For a physical
                // display, only do so if it has a pending modeset for the matching resolution.
                if (!currentState.physical ||
                    (FlagManager::getInstance().synced_resolution_switch() &&
                     mDisplayModeController.getDesiredMode(display->getPhysicalId())
                             .transform([resolution](const auto& request) {
                                 return resolution == request.mode.modePtr->getResolution();
                             })
                             .value_or(false))) {
                    display->setDisplaySize(resolution);
                }

                if (display->getId() == mActiveDisplayId) {
                    onActiveDisplaySizeChanged(*display);
                }
            }
        };

        if (FlagManager::getInstance().synced_resolution_switch()) {
            // Update display size first, as display projection below depends on it.
            updateDisplaySize();
        }

        if ((currentState.orientation != drawingState.orientation) ||
            (currentState.layerStackSpaceRect != drawingState.layerStackSpaceRect) ||
            (currentState.orientedDisplaySpaceRect != drawingState.orientedDisplaySpaceRect)) {
            display->setProjection(currentState.orientation, currentState.layerStackSpaceRect,
                                   currentState.orientedDisplaySpaceRect);
            if (display->getId() == mActiveDisplayId) {
                mActiveDisplayTransformHint = display->getTransformHint();
                sActiveDisplayRotationFlags =
                        ui::Transform::toRotationFlags(display->getOrientation());
            }
        }

        if (!FlagManager::getInstance().synced_resolution_switch()) {
            updateDisplaySize();
        }
    }
}

void SurfaceFlinger::processDisplayChangesLocked() {
    // here we take advantage of Vector's copy-on-write semantics to
    // improve performance by skipping the transaction entirely when
    // know that the lists are identical
    const KeyedVector<wp<IBinder>, DisplayDeviceState>& curr(mCurrentState.displays);
    const KeyedVector<wp<IBinder>, DisplayDeviceState>& draw(mDrawingState.displays);
    if (!curr.isIdenticalTo(draw)) {
        mVisibleRegionsDirty = true;
        mUpdateInputInfo = true;

        // Apply the current color matrix to any added or changed display.
        mCurrentState.colorMatrixChanged = true;

        // find the displays that were removed
        // (ie: in drawing state but not in current state)
        // also handle displays that changed
        // (ie: displays that are in both lists)
        for (size_t i = 0; i < draw.size(); i++) {
            const wp<IBinder>& displayToken = draw.keyAt(i);
            const ssize_t j = curr.indexOfKey(displayToken);
            if (j < 0) {
                // in drawing state but not in current state
                processDisplayRemoved(displayToken);
            } else {
                // this display is in both lists. see if something changed.
                const DisplayDeviceState& currentState = curr[j];
                const DisplayDeviceState& drawingState = draw[i];
                processDisplayChanged(displayToken, currentState, drawingState);
            }
        }

        // find displays that were added
        // (ie: in current state but not in drawing state)
        for (size_t i = 0; i < curr.size(); i++) {
            const wp<IBinder>& displayToken = curr.keyAt(i);
            if (draw.indexOfKey(displayToken) < 0) {
                processDisplayAdded(displayToken, curr[i]);
            }
        }
    }

    mDrawingState.displays = mCurrentState.displays;
}

void SurfaceFlinger::commitTransactionsLocked(uint32_t transactionFlags) {
    // Commit display transactions.
    const bool displayTransactionNeeded = transactionFlags & eDisplayTransactionNeeded;
    mFrontEndDisplayInfosChanged = displayTransactionNeeded;

    if (mSomeChildrenChanged) {
        mVisibleRegionsDirty = true;
        mSomeChildrenChanged = false;
        mUpdateInputInfo = true;
    }

    if (mLayersAdded) {
        mLayersAdded = false;
        // Layers have been added.
        mVisibleRegionsDirty = true;
        mUpdateInputInfo = true;
    }

    // some layers might have been removed, so
    // we need to update the regions they're exposing.
    if (mLayersRemoved) {
        mLayersRemoved = false;
        mVisibleRegionsDirty = true;
        mUpdateInputInfo = true;
    }

    if (transactionFlags & eInputInfoUpdateNeeded) {
        mUpdateInputInfo = true;
    }

    doCommitTransactions();
}

void SurfaceFlinger::updateInputFlinger(VsyncId vsyncId, TimePoint frameTime) {
    if (!mInputFlinger || (!mUpdateInputInfo && mInputWindowCommands.empty())) {
        return;
    }
    SFTRACE_CALL();

    std::vector<WindowInfo> windowInfos;
    std::vector<DisplayInfo> displayInfos;
    bool updateWindowInfo = false;
    if (mUpdateInputInfo) {
        mUpdateInputInfo = false;
        updateWindowInfo = true;
        buildWindowInfos(windowInfos, displayInfos);
    }

    std::unordered_set<int32_t> visibleWindowIds;
    for (WindowInfo& windowInfo : windowInfos) {
        if (!windowInfo.inputConfig.test(WindowInfo::InputConfig::NOT_VISIBLE)) {
            visibleWindowIds.insert(windowInfo.id);
        }
    }
    bool visibleWindowsChanged = false;
    if (visibleWindowIds != mVisibleWindowIds) {
        visibleWindowsChanged = true;
        mVisibleWindowIds = std::move(visibleWindowIds);
    }

    BackgroundExecutor::getInstance().sendCallbacks(
            {[updateWindowInfo, windowInfos = std::move(windowInfos),
              displayInfos = std::move(displayInfos),
              inputWindowCommands = std::move(mInputWindowCommands), inputFlinger = mInputFlinger,
              this, visibleWindowsChanged, vsyncId, frameTime]() mutable {
                SFTRACE_NAME("BackgroundExecutor::updateInputFlinger");
                if (updateWindowInfo) {
                    mWindowInfosListenerInvoker
                            ->windowInfosChanged(gui::WindowInfosUpdate{std::move(windowInfos),
                                                                        std::move(displayInfos),
                                                                        ftl::to_underlying(vsyncId),
                                                                        frameTime.ns()},
                                                 inputWindowCommands.releaseListeners(),
                                                 /* forceImmediateCall= */ visibleWindowsChanged ||
                                                         !inputWindowCommands.getFocusRequests()
                                                                  .empty());
                } else {
                    // If there are listeners but no changes to input windows, call the listeners
                    // immediately.
                    for (const auto& listener : inputWindowCommands.getListeners()) {
                        if (IInterface::asBinder(listener)->isBinderAlive()) {
                            listener->onWindowInfosReported();
                        }
                    }
                }
                for (const auto& focusRequest : inputWindowCommands.getFocusRequests()) {
                    inputFlinger->setFocusedWindow(focusRequest);
                }
            }});

    mInputWindowCommands.clear();
}

void SurfaceFlinger::persistDisplayBrightness(bool needsComposite) {
    const bool supportsDisplayBrightnessCommand = getHwComposer().getComposer()->isSupported(
            Hwc2::Composer::OptionalFeature::DisplayBrightnessCommand);
    if (!supportsDisplayBrightnessCommand) {
        return;
    }

    for (const auto& [_, display] : FTL_FAKE_GUARD(mStateLock, mDisplays)) {
        if (const auto brightness = display->getStagedBrightness(); brightness) {
            if (!needsComposite) {
                const status_t error =
                        getHwComposer()
                                .setDisplayBrightness(display->getPhysicalId(), *brightness,
                                                      display->getCompositionDisplay()
                                                              ->getState()
                                                              .displayBrightnessNits,
                                                      Hwc2::Composer::DisplayBrightnessOptions{
                                                              .applyImmediately = true})
                                .get();

                ALOGE_IF(error != NO_ERROR,
                         "Error setting display brightness for display %s: %d (%s)",
                         to_string(display->getId()).c_str(), error, strerror(error));
            }
            display->persistBrightness(needsComposite);
        }
    }
}

void SurfaceFlinger::buildWindowInfos(std::vector<WindowInfo>& outWindowInfos,
                                      std::vector<DisplayInfo>& outDisplayInfos) {
    static size_t sNumWindowInfos = 0;
    outWindowInfos.reserve(sNumWindowInfos);
    sNumWindowInfos = 0;

    mLayerSnapshotBuilder.forEachInputSnapshot(
            [&outWindowInfos](const frontend::LayerSnapshot& snapshot) {
                outWindowInfos.push_back(snapshot.inputInfo);
            });

    sNumWindowInfos = outWindowInfos.size();

    outDisplayInfos.reserve(mFrontEndDisplayInfos.size());
    for (const auto& [_, info] : mFrontEndDisplayInfos) {
        outDisplayInfos.push_back(info.info);
    }
}

void SurfaceFlinger::updateCursorAsync() {
    compositionengine::CompositionRefreshArgs refreshArgs;
    for (const auto& [_, display] : FTL_FAKE_GUARD(mStateLock, mDisplays)) {
        if (asHalDisplayId(display->getDisplayIdVariant())) {
            refreshArgs.outputs.push_back(display->getCompositionDisplay());
        }
    }

    constexpr bool kCursorOnly = true;
    const auto layers = moveSnapshotsToCompositionArgs(refreshArgs, kCursorOnly);
    mCompositionEngine->updateCursorAsync(refreshArgs);
    moveSnapshotsFromCompositionArgs(refreshArgs, layers);
}

void SurfaceFlinger::requestHardwareVsync(PhysicalDisplayId displayId, bool enable) {
    getHwComposer().setVsyncEnabled(displayId, enable ? hal::Vsync::ENABLE : hal::Vsync::DISABLE);
}

void SurfaceFlinger::requestDisplayModes(std::vector<display::DisplayModeRequest> modeRequests) {
    if (mBootStage != BootStage::FINISHED) {
        ALOGV("Currently in the boot stage, skipping display mode changes");
        return;
    }

    SFTRACE_CALL();

    // If this is called from the main thread mStateLock must be locked before
    // Currently the only way to call this function from the main thread is from
    // Scheduler::chooseRefreshRateForContent

    ConditionalLock lock(mStateLock, std::this_thread::get_id() != mMainThreadId);

    for (auto& request : modeRequests) {
        const auto& modePtr = request.mode.modePtr;

        const auto displayId = modePtr->getPhysicalDisplayId();
        const auto display = getDisplayDeviceLocked(displayId);

        if (!display) continue;

        if (display->refreshRateSelector().isModeAllowed(request.mode)) {
            setDesiredMode(std::move(request));
        } else {
            ALOGV("%s: Mode %d is disallowed for display %s", __func__,
                  ftl::to_underlying(modePtr->getId()), to_string(displayId).c_str());
        }
    }
}

void SurfaceFlinger::notifyCpuLoadUp() {
    mPowerAdvisor->notifyCpuLoadUp();
}

void SurfaceFlinger::onChoreographerAttached() {
    SFTRACE_CALL();
    mUpdateAttachedChoreographer = true;
    scheduleCommit(FrameHint::kNone);
}

void SurfaceFlinger::onExpectedPresentTimePosted(TimePoint expectedPresentTime,
                                                 ftl::NonNull<DisplayModePtr> modePtr,
                                                 Fps renderRate) {
    const auto vsyncPeriod = modePtr->getVsyncRate().getPeriod();
    const auto timeoutOpt = [&]() -> std::optional<Period> {
        const auto vrrConfig = modePtr->getVrrConfig();
        if (!vrrConfig) return std::nullopt;

        const auto notifyExpectedPresentConfig =
                modePtr->getVrrConfig()->notifyExpectedPresentConfig;
        if (!notifyExpectedPresentConfig) return std::nullopt;
        return Period::fromNs(notifyExpectedPresentConfig->timeoutNs);
    }();

    notifyExpectedPresentIfRequired(modePtr->getPhysicalDisplayId(), vsyncPeriod,
                                    expectedPresentTime, renderRate, timeoutOpt);
}

void SurfaceFlinger::notifyExpectedPresentIfRequired(PhysicalDisplayId displayId,
                                                     Period vsyncPeriod,
                                                     TimePoint expectedPresentTime,
                                                     Fps frameInterval,
                                                     std::optional<Period> timeoutOpt) {
    auto& data = mNotifyExpectedPresentMap[displayId];
    const auto lastExpectedPresentTimestamp = data.lastExpectedPresentTimestamp;
    const auto lastFrameInterval = data.lastFrameInterval;
    data.lastFrameInterval = frameInterval;
    data.lastExpectedPresentTimestamp = expectedPresentTime;
    const auto threshold = Duration::fromNs(vsyncPeriod.ns() / 2);

    const constexpr nsecs_t kOneSecondNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(1s).count();
    const auto timeout =
            Period::fromNs(timeoutOpt && timeoutOpt->ns() > 0 ? timeoutOpt->ns() : kOneSecondNs);
    const bool frameIntervalIsOnCadence =
            isFrameIntervalOnCadence(expectedPresentTime, lastExpectedPresentTimestamp,
                                     lastFrameInterval, timeout, threshold);

    const bool expectedPresentWithinTimeout =
            isExpectedPresentWithinTimeout(expectedPresentTime, lastExpectedPresentTimestamp,
                                           timeoutOpt, threshold);
    if (expectedPresentWithinTimeout && frameIntervalIsOnCadence) {
        return;
    }

    auto hintStatus = data.hintStatus.load();
    if (!expectedPresentWithinTimeout) {
        if ((hintStatus != NotifyExpectedPresentHintStatus::Sent &&
             hintStatus != NotifyExpectedPresentHintStatus::ScheduleOnTx) ||
            (timeoutOpt && timeoutOpt->ns() == 0)) {
            // Send the hint immediately if timeout, as the hint gets
            // delayed otherwise, as the frame is scheduled close
            // to the actual present.
            if (data.hintStatus
                        .compare_exchange_strong(hintStatus,
                                                 NotifyExpectedPresentHintStatus::ScheduleOnTx)) {
                scheduleNotifyExpectedPresentHint(displayId);
                return;
            }
        }
    }

    if (hintStatus == NotifyExpectedPresentHintStatus::Sent &&
        data.hintStatus.compare_exchange_strong(hintStatus,
                                                NotifyExpectedPresentHintStatus::ScheduleOnTx)) {
        return;
    }
    if (hintStatus != NotifyExpectedPresentHintStatus::Start) {
        return;
    }
    data.hintStatus.store(NotifyExpectedPresentHintStatus::ScheduleOnPresent);
    mScheduler->scheduleFrame();
}

void SurfaceFlinger::scheduleNotifyExpectedPresentHint(PhysicalDisplayId displayId,
                                                       VsyncId vsyncId) {
    auto itr = mNotifyExpectedPresentMap.find(displayId);
    if (itr == mNotifyExpectedPresentMap.end()) {
        return;
    }

    const char* const whence = __func__;
    const auto sendHint = [=, this]() {
        auto& data = mNotifyExpectedPresentMap.at(displayId);
        TimePoint expectedPresentTime = data.lastExpectedPresentTimestamp;
        if (ftl::to_underlying(vsyncId) != FrameTimelineInfo::INVALID_VSYNC_ID) {
            const auto predictionOpt = mFrameTimeline->getTokenManager()->getPredictionsForToken(
                    ftl::to_underlying(vsyncId));
            const auto expectedPresentTimeOnPredictor = TimePoint::fromNs(
                    predictionOpt ? predictionOpt->presentTime : expectedPresentTime.ns());
            const auto scheduledFrameResultOpt = mScheduler->getScheduledFrameResult();
            const auto expectedPresentTimeOnScheduler = scheduledFrameResultOpt.has_value()
                    ? scheduledFrameResultOpt->vsyncTime
                    : TimePoint::fromNs(0);
            expectedPresentTime =
                    std::max(expectedPresentTimeOnPredictor, expectedPresentTimeOnScheduler);
        }

        if (expectedPresentTime < TimePoint::now()) {
            expectedPresentTime =
                    mScheduler->getVsyncSchedule()->vsyncDeadlineAfter(TimePoint::now());
            if (mScheduler->vsyncModulator().getVsyncConfig().sfWorkDuration >
                mScheduler->getVsyncSchedule(displayId)->period()) {
                expectedPresentTime += mScheduler->getVsyncSchedule(displayId)->period();
            }
        }
        const auto status = getHwComposer().notifyExpectedPresent(displayId, expectedPresentTime,
                                                                  data.lastFrameInterval);
        if (status != NO_ERROR) {
            ALOGE("%s failed to notifyExpectedPresentHint for display %" PRId64, whence,
                  displayId.value);
        }
    };

    if (itr->second.hintStatus == NotifyExpectedPresentHintStatus::ScheduleOnTx) {
        return static_cast<void>(mScheduler->schedule([=,
                                                       this]() FTL_FAKE_GUARD(kMainThreadContext) {
            auto& data = mNotifyExpectedPresentMap.at(displayId);
            auto scheduleHintOnTx = NotifyExpectedPresentHintStatus::ScheduleOnTx;
            if (data.hintStatus.compare_exchange_strong(scheduleHintOnTx,
                                                        NotifyExpectedPresentHintStatus::Sent)) {
                sendHint();
                constexpr bool kAllowToEnable = true;
                mScheduler->resyncToHardwareVsync(displayId, kAllowToEnable);
            }
        }));
    }
    auto scheduleHintOnPresent = NotifyExpectedPresentHintStatus::ScheduleOnPresent;
    if (itr->second.hintStatus.compare_exchange_strong(scheduleHintOnPresent,
                                                       NotifyExpectedPresentHintStatus::Sent)) {
        sendHint();
    }
}

void SurfaceFlinger::sendNotifyExpectedPresentHint(PhysicalDisplayId displayId) {
    if (auto itr = mNotifyExpectedPresentMap.find(displayId);
        itr == mNotifyExpectedPresentMap.end() ||
        itr->second.hintStatus != NotifyExpectedPresentHintStatus::ScheduleOnPresent) {
        return;
    }
    scheduleNotifyExpectedPresentHint(displayId);
}

void SurfaceFlinger::onCommitNotComposited() {
    if (FlagManager::getInstance().commit_not_composited()) {
        mFrameTimeline->onCommitNotComposited();
    }
}

void SurfaceFlinger::initScheduler(const sp<const DisplayDevice>& display) {
    using namespace scheduler;

    LOG_ALWAYS_FATAL_IF(mScheduler);

    const auto activeMode = display->refreshRateSelector().getActiveMode();
    const Fps activeRefreshRate = activeMode.fps;

    FeatureFlags features;

    const auto defaultContentDetectionValue =
            FlagManager::getInstance().enable_fro_dependent_features() &&
            sysprop::enable_frame_rate_override(true);
    if (sysprop::use_content_detection_for_refresh_rate(defaultContentDetectionValue)) {
        features |= Feature::kContentDetection;
        if (FlagManager::getInstance().enable_small_area_detection()) {
            features |= Feature::kSmallDirtyContentDetection;
        }
    }
    if (base::GetBoolProperty("debug.sf.show_predicted_vsync"s, false)) {
        features |= Feature::kTracePredictedVsync;
    }
    if (!base::GetBoolProperty("debug.sf.vsync_reactor_ignore_present_fences"s, false) &&
        mHasReliablePresentFences) {
        features |= Feature::kPresentFences;
    }
    if (display->refreshRateSelector().kernelIdleTimerController()) {
        features |= Feature::kKernelIdleTimer;
    }
    if (mBackpressureGpuComposition) {
        features |= Feature::kBackpressureGpuComposition;
    }
    if (mPropagateBackpressure) {
        features |= Feature::kPropagateBackpressure;
    }
    if (getHwComposer().getComposer()->isSupported(
                Hwc2::Composer::OptionalFeature::ExpectedPresentTime)) {
        features |= Feature::kExpectedPresentTime;
    }

    mScheduler = std::make_unique<Scheduler>(static_cast<ICompositor&>(*this),
                                             static_cast<ISchedulerCallback&>(*this), features,
                                             getFactory(), activeRefreshRate, *mTimeStats);

    // The pacesetter must be registered before EventThread creation below.
    mScheduler->registerDisplay(display->getPhysicalId(), display->holdRefreshRateSelector(),
                                mActiveDisplayId);
    if (FlagManager::getInstance().vrr_config()) {
        mScheduler->setRenderRate(display->getPhysicalId(), activeMode.fps,
                                  /*applyImmediately*/ true);
    }

    const auto configs = mScheduler->getCurrentVsyncConfigs();

    mScheduler->createEventThread(scheduler::Cycle::Render, mFrameTimeline->getTokenManager(),
                                  /* workDuration */ configs.late.appWorkDuration,
                                  /* readyDuration */ configs.late.sfWorkDuration);
    mScheduler->createEventThread(scheduler::Cycle::LastComposite,
                                  mFrameTimeline->getTokenManager(),
                                  /* workDuration */ activeRefreshRate.getPeriod(),
                                  /* readyDuration */ configs.late.sfWorkDuration);

    // Dispatch after EventThread creation, since registerDisplay above skipped dispatch.
    mScheduler->dispatchHotplug(display->getPhysicalId(), scheduler::Scheduler::Hotplug::Connected);

    mScheduler->initVsync(*mFrameTimeline->getTokenManager(), configs.late.sfWorkDuration);

    mRegionSamplingThread =
            sp<RegionSamplingThread>::make(*this,
                                           RegionSamplingThread::EnvironmentTimingTunables());
    mFpsReporter = sp<FpsReporter>::make(*mFrameTimeline);

    // Timer callbacks may fire, so do this last.
    mScheduler->startTimers();
}

void SurfaceFlinger::doCommitTransactions() {
    SFTRACE_CALL();
    mDrawingState = mCurrentState;
    mCurrentState.colorMatrixChanged = false;
}

void SurfaceFlinger::invalidateLayerStack(const ui::LayerFilter& layerFilter, const Region& dirty) {
    for (const auto& [token, displayDevice] : FTL_FAKE_GUARD(mStateLock, mDisplays)) {
        auto display = displayDevice->getCompositionDisplay();
        if (display->includesLayer(layerFilter)) {
            display->editState().dirtyRegion.orSelf(dirty);
        }
    }
}

status_t SurfaceFlinger::addClientLayer(LayerCreationArgs& args, const sp<IBinder>& handle,
                                        const sp<Layer>& layer, const wp<Layer>& parent,
                                        uint32_t* outTransformHint) {
    if (outTransformHint) {
        *outTransformHint = mActiveDisplayTransformHint;
    }
    args.parentId = LayerHandle::getLayerId(args.parentHandle.promote());
    args.layerIdToMirror = LayerHandle::getLayerId(args.mirrorLayerHandle.promote());
    {
        std::scoped_lock<std::mutex> lock(mCreatedLayersLock);
        mCreatedLayers.emplace_back(layer);
        mNewLayers.emplace_back(std::make_unique<frontend::RequestedLayerState>(args));
        args.mirrorLayerHandle.clear();
        args.parentHandle.clear();
        mNewLayerArgs.emplace_back(std::move(args));
    }

    setTransactionFlags(eTransactionNeeded);
    return NO_ERROR;
}

uint32_t SurfaceFlinger::getTransactionFlags() const {
    return mTransactionFlags;
}

uint32_t SurfaceFlinger::clearTransactionFlags(uint32_t mask) {
    uint32_t transactionFlags = mTransactionFlags.fetch_and(~mask);
    SFTRACE_INT("mTransactionFlags", transactionFlags);
    return transactionFlags & mask;
}

void SurfaceFlinger::setTransactionFlags(uint32_t mask, TransactionSchedule schedule,
                                         const sp<IBinder>& applyToken, FrameHint frameHint) {
    mScheduler->modulateVsync({}, &VsyncModulator::setTransactionSchedule, schedule, applyToken);
    uint32_t transactionFlags = mTransactionFlags.fetch_or(mask);
    SFTRACE_INT("mTransactionFlags", transactionFlags);

    if (const bool scheduled = transactionFlags & mask; !scheduled) {
        scheduleCommit(frameHint);
    } else if (frameHint == FrameHint::kActive) {
        // Even if the next frame is already scheduled, we should reset the idle timer
        // as a new activity just happened.
        mScheduler->resetIdleTimer();
    }
}

TransactionHandler::TransactionReadiness SurfaceFlinger::transactionReadyTimelineCheck(
        const TransactionHandler::TransactionFlushState& flushState) {
    const auto& transaction = *flushState.transaction;

    const TimePoint desiredPresentTime = TimePoint::fromNs(transaction.desiredPresentTime);
    const TimePoint expectedPresentTime = mScheduler->expectedPresentTimeForPacesetter();

    using TransactionReadiness = TransactionHandler::TransactionReadiness;

    // Do not present if the desiredPresentTime has not passed unless it is more than
    // one second in the future. We ignore timestamps more than 1 second in the future
    // for stability reasons.
    if (!transaction.isAutoTimestamp && desiredPresentTime >= expectedPresentTime &&
        desiredPresentTime < expectedPresentTime + 1s) {
        SFTRACE_FORMAT("not current desiredPresentTime: %" PRId64 " expectedPresentTime: %" PRId64,
                       desiredPresentTime, expectedPresentTime);
        return TransactionReadiness::NotReady;
    }

    const auto vsyncId = VsyncId{transaction.frameTimelineInfo.vsyncId};

    // Transactions with VsyncId are already throttled by the vsyncId (i.e. Choreographer issued
    // the vsyncId according to the frame rate override cadence) so we shouldn't throttle again
    // when applying the transaction. Otherwise we might throttle older transactions
    // incorrectly as the frame rate of SF changed before it drained the older transactions.
    if (ftl::to_underlying(vsyncId) == FrameTimelineInfo::INVALID_VSYNC_ID &&
        !mScheduler->isVsyncValid(expectedPresentTime, transaction.originUid)) {
        SFTRACE_FORMAT("!isVsyncValid expectedPresentTime: %" PRId64 " uid: %d",
                       expectedPresentTime, transaction.originUid);
        return TransactionReadiness::NotReady;
    }

    // If the client didn't specify desiredPresentTime, use the vsyncId to determine the
    // expected present time of this transaction.
    if (transaction.isAutoTimestamp && frameIsEarly(expectedPresentTime, vsyncId)) {
        SFTRACE_FORMAT("frameIsEarly vsyncId: %" PRId64 " expectedPresentTime: %" PRId64,
                       transaction.frameTimelineInfo.vsyncId, expectedPresentTime);
        return TransactionReadiness::NotReady;
    }

    return TransactionReadiness::Ready;
}

TransactionHandler::TransactionReadiness SurfaceFlinger::transactionReadyBufferCheck(
        const TransactionHandler::TransactionFlushState& flushState) {
    using TransactionReadiness = TransactionHandler::TransactionReadiness;
    auto ready = TransactionReadiness::Ready;
    flushState.transaction->traverseStatesWithBuffersWhileTrue(
            [&](const ResolvedComposerState& resolvedState) FTL_FAKE_GUARD(
                    kMainThreadContext) -> bool {
                const frontend::RequestedLayerState* layer =
                        mLayerLifecycleManager.getLayerFromId(resolvedState.layerId);
                const auto& transaction = *flushState.transaction;
                const auto& s = resolvedState.state;
                // check for barrier frames
                if (s.bufferData->hasBarrier) {
                    // The current producerId is already a newer producer than the buffer that has a
                    // barrier. This means the incoming buffer is older and we can release it here.
                    // We don't wait on the barrier since we know that's stale information.
                    if (layer->barrierProducerId > s.bufferData->producerId) {
                        if (s.bufferData->releaseBufferListener) {
                            uint32_t currentMaxAcquiredBufferCount =
                                    getMaxAcquiredBufferCountForCurrentRefreshRate(
                                            layer->ownerUid.val());
                            SFTRACE_FORMAT_INSTANT("callReleaseBufferCallback %s - %" PRIu64,
                                                   layer->name.c_str(), s.bufferData->frameNumber);
                            s.bufferData->releaseBufferListener
                                    ->onReleaseBuffer({resolvedState.externalTexture->getBuffer()
                                                               ->getId(),
                                                       s.bufferData->frameNumber},
                                                      s.bufferData->acquireFence
                                                              ? s.bufferData->acquireFence
                                                              : Fence::NO_FENCE,
                                                      currentMaxAcquiredBufferCount);
                        }

                        // Delete the entire state at this point and not just release the buffer
                        // because everything associated with the Layer in this Transaction is now
                        // out of date.
                        SFTRACE_FORMAT("DeleteStaleBuffer %s barrierProducerId:%d > %d",
                                       layer->name.c_str(), layer->barrierProducerId,
                                       s.bufferData->producerId);
                        return TraverseBuffersReturnValues::DELETE_AND_CONTINUE_TRAVERSAL;
                    }

                    if (layer->barrierFrameNumber < s.bufferData->barrierFrameNumber) {
                        const bool willApplyBarrierFrame =
                                flushState.bufferLayersReadyToPresent.contains(s.surface.get()) &&
                                ((flushState.bufferLayersReadyToPresent.get(s.surface.get()) >=
                                  s.bufferData->barrierFrameNumber));
                        if (!willApplyBarrierFrame) {
                            SFTRACE_FORMAT("NotReadyBarrier %s barrierFrameNumber:%" PRId64
                                           " > %" PRId64,
                                           layer->name.c_str(), layer->barrierFrameNumber,
                                           s.bufferData->barrierFrameNumber);
                            ready = TransactionReadiness::NotReadyBarrier;
                            return TraverseBuffersReturnValues::STOP_TRAVERSAL;
                        }
                    }
                }

                // If backpressure is enabled and we already have a buffer to commit, keep
                // the transaction in the queue.
                const bool hasPendingBuffer =
                        flushState.bufferLayersReadyToPresent.contains(s.surface.get());
                if (layer->backpressureEnabled() && hasPendingBuffer &&
                    transaction.isAutoTimestamp) {
                    SFTRACE_FORMAT("hasPendingBuffer %s", layer->name.c_str());
                    ready = TransactionReadiness::NotReady;
                    return TraverseBuffersReturnValues::STOP_TRAVERSAL;
                }

                // ignore the acquire fence if LatchUnsignaledConfig::Always is set.
                const bool checkAcquireFence =
                        enableLatchUnsignaledConfig != LatchUnsignaledConfig::Always;
                const bool acquireFenceAvailable = s.bufferData &&
                        s.bufferData->flags.test(BufferData::BufferDataChange::fenceChanged) &&
                        s.bufferData->acquireFence;
                const bool fenceSignaled = !checkAcquireFence || !acquireFenceAvailable ||
                        s.bufferData->acquireFence->getStatus() != Fence::Status::Unsignaled;
                if (!fenceSignaled) {
                    // check fence status
                    const bool allowLatchUnsignaled =
                            shouldLatchUnsignaled(s, transaction.states.size(),
                                                  flushState.firstTransaction) &&
                            layer->isSimpleBufferUpdate(s);
                    if (allowLatchUnsignaled) {
                        SFTRACE_FORMAT("fence unsignaled try allowLatchUnsignaled %s",
                                       layer->name.c_str());
                        ready = TransactionReadiness::NotReadyUnsignaled;
                    } else {
                        ready = TransactionReadiness::NotReady;
                        auto& listener = s.bufferData->releaseBufferListener;
                        if (listener &&
                            (flushState.queueProcessTime - transaction.postTime) >
                                    std::chrono::nanoseconds(4s).count()) {
                            mTransactionHandler
                                    .onTransactionQueueStalled(transaction.id,
                                                               {.pid = layer->ownerPid.val(),
                                                                .layerId = layer->id,
                                                                .layerName = layer->name,
                                                                .bufferId = s.bufferData->getId(),
                                                                .frameNumber =
                                                                        s.bufferData->frameNumber});
                        }
                        SFTRACE_FORMAT("fence unsignaled %s", layer->name.c_str());
                        return TraverseBuffersReturnValues::STOP_TRAVERSAL;
                    }
                }
                return TraverseBuffersReturnValues::CONTINUE_TRAVERSAL;
            });
    return ready;
}

void SurfaceFlinger::addTransactionReadyFilters() {
    mTransactionHandler.addTransactionReadyFilter(
            std::bind(&SurfaceFlinger::transactionReadyTimelineCheck, this, std::placeholders::_1));
    mTransactionHandler.addTransactionReadyFilter(
            std::bind(&SurfaceFlinger::transactionReadyBufferCheck, this, std::placeholders::_1));
}

// For tests only
bool SurfaceFlinger::flushTransactionQueues() {
    mTransactionHandler.collectTransactions();
    std::vector<QueuedTransactionState> transactions = mTransactionHandler.flushTransactions();
    return applyTransactions(transactions);
}

bool SurfaceFlinger::applyTransactions(std::vector<QueuedTransactionState>& transactions)
        EXCLUDES(mStateLock) {
    Mutex::Autolock lock(mStateLock);
    return applyTransactionsLocked(transactions);
}

bool SurfaceFlinger::applyTransactionsLocked(std::vector<QueuedTransactionState>& transactions)
        REQUIRES(mStateLock) {
    bool needsTraversal = false;
    // Now apply all transactions.
    for (auto& transaction : transactions) {
        needsTraversal |=
                applyTransactionState(transaction.frameTimelineInfo, transaction.states,
                                      transaction.displays, transaction.flags,
                                      transaction.inputWindowCommands,
                                      transaction.desiredPresentTime, transaction.isAutoTimestamp,
                                      std::move(transaction.uncacheBufferIds), transaction.postTime,
                                      transaction.hasListenerCallbacks,
                                      transaction.listenerCallbacks, transaction.originPid,
                                      transaction.originUid, transaction.id);
    }
    return needsTraversal;
}

bool SurfaceFlinger::transactionFlushNeeded() {
    return mTransactionHandler.hasPendingTransactions();
}

bool SurfaceFlinger::frameIsEarly(TimePoint expectedPresentTime, VsyncId vsyncId) const {
    const auto prediction =
            mFrameTimeline->getTokenManager()->getPredictionsForToken(ftl::to_underlying(vsyncId));
    if (!prediction) {
        return false;
    }

    const auto predictedPresentTime = TimePoint::fromNs(prediction->presentTime);

    if (std::chrono::abs(predictedPresentTime - expectedPresentTime) >=
        scheduler::VsyncConfig::kEarlyLatchMaxThreshold) {
        return false;
    }

    const Duration earlyLatchVsyncThreshold = mScheduler->getVsyncSchedule()->minFramePeriod() / 2;

    return predictedPresentTime >= expectedPresentTime &&
            predictedPresentTime - expectedPresentTime >= earlyLatchVsyncThreshold;
}

bool SurfaceFlinger::shouldLatchUnsignaled(const layer_state_t& state, size_t numStates,
                                           bool firstTransaction) const {
    if (enableLatchUnsignaledConfig == LatchUnsignaledConfig::Disabled) {
        SFTRACE_FORMAT_INSTANT("%s: false (LatchUnsignaledConfig::Disabled)", __func__);
        return false;
    }

    if (enableLatchUnsignaledConfig == LatchUnsignaledConfig::Always) {
        SFTRACE_FORMAT_INSTANT("%s: true (LatchUnsignaledConfig::Always)", __func__);
        return true;
    }

    // We only want to latch unsignaled when a single layer is updated in this
    // transaction (i.e. not a blast sync transaction).
    if (numStates != 1) {
        SFTRACE_FORMAT_INSTANT("%s: false (numStates=%zu)", __func__, numStates);
        return false;
    }

    if (enableLatchUnsignaledConfig == LatchUnsignaledConfig::AutoSingleLayer) {
        if (!firstTransaction) {
            SFTRACE_FORMAT_INSTANT("%s: false (LatchUnsignaledConfig::AutoSingleLayer; not first "
                                   "transaction)",
                                   __func__);
            return false;
        }

        // We don't want to latch unsignaled if are in early / client composition
        // as it leads to jank due to RenderEngine waiting for unsignaled buffer
        // or window animations being slow.
        if (mScheduler->vsyncModulator().isVsyncConfigEarly()) {
            SFTRACE_FORMAT_INSTANT("%s: false (LatchUnsignaledConfig::AutoSingleLayer; "
                                   "isVsyncConfigEarly)",
                                   __func__);
            return false;
        }
    }

    return true;
}

status_t SurfaceFlinger::setTransactionState(TransactionState&& transactionState) {
    SFTRACE_CALL();

    IPCThreadState* ipc = IPCThreadState::self();
    const int originPid = ipc->getCallingPid();
    const int originUid = ipc->getCallingUid();
    uint32_t permissions = LayerStatePermissions::getTransactionPermissions(originPid, originUid);
    ftl::Flags<adpf::Workload> queuedWorkload;
    for (auto& composerState : transactionState.mComposerStates) {
        composerState.state.sanitize(permissions);
        if (composerState.state.what & layer_state_t::COMPOSITION_EFFECTS) {
            queuedWorkload |= adpf::Workload::EFFECTS;
        }
        if (composerState.state.what & layer_state_t::VISIBLE_REGION_CHANGES) {
            queuedWorkload |= adpf::Workload::VISIBLE_REGION;
        }
    }

    for (DisplayState& display : transactionState.mDisplayStates) {
        display.sanitize(permissions);
    }

    if (!transactionState.mInputWindowCommands.empty() &&
        (permissions & layer_state_t::Permission::ACCESS_SURFACE_FLINGER) == 0) {
        ALOGE("Only privileged callers are allowed to send input commands.");
        transactionState.mInputWindowCommands.clear();
    }

    if (transactionState.mFlags & (eEarlyWakeupStart | eEarlyWakeupEnd)) {
        const bool hasPermission =
                (permissions & layer_state_t::Permission::ACCESS_SURFACE_FLINGER) ||
                callingThreadHasPermission(sWakeupSurfaceFlinger);
        if (!hasPermission) {
            ALOGE("Caller needs permission android.permission.WAKEUP_SURFACE_FLINGER to use "
                  "eEarlyWakeup[Start|End] flags");
            transactionState.mFlags &= ~(eEarlyWakeupStart | eEarlyWakeupEnd);
        }
    }
    if (transactionState.mFlags & eEarlyWakeupStart) {
        queuedWorkload |= adpf::Workload::WAKEUP;
    }
    mPowerAdvisor->setQueuedWorkload(queuedWorkload);

    const int64_t postTime = systemTime();

    std::vector<uint64_t> uncacheBufferIds;
    uncacheBufferIds.reserve(transactionState.mUncacheBuffers.size());
    for (const auto& uncacheBuffer : transactionState.mUncacheBuffers) {
        sp<GraphicBuffer> buffer = ClientCache::getInstance().erase(uncacheBuffer);
        if (buffer != nullptr) {
            uncacheBufferIds.push_back(buffer->getId());
        }
    }

    std::vector<ResolvedComposerState> resolvedStates;
    resolvedStates.reserve(transactionState.mComposerStates.size());
    for (auto& state : transactionState.mComposerStates) {
        resolvedStates.emplace_back(std::move(state));
        auto& resolvedState = resolvedStates.back();
        resolvedState.layerId = LayerHandle::getLayerId(resolvedState.state.surface);
        if (resolvedState.state.hasBufferChanges() && resolvedState.state.hasValidBuffer() &&
            resolvedState.state.surface) {
            sp<Layer> layer = LayerHandle::getLayer(resolvedState.state.surface);
            std::string layerName =
                    (layer) ? layer->getDebugName() : std::to_string(resolvedState.state.layerId);
            resolvedState.externalTexture =
                    getExternalTextureFromBufferData(*resolvedState.state.bufferData,
                                                     layerName.c_str(), transactionState.getId());
            if (resolvedState.externalTexture) {
                resolvedState.state.bufferData->buffer = resolvedState.externalTexture->getBuffer();
                if (FlagManager::getInstance().monitor_buffer_fences()) {
                    resolvedState.state.bufferData->buffer->getDependencyMonitor()
                            .addIngress(FenceTime::makeValid(
                                                resolvedState.state.bufferData->acquireFence),
                                        "Incoming txn");
                }
            }
            mBufferCountTracker.increment(resolvedState.layerId);
        }
        if (resolvedState.state.what & layer_state_t::eReparent) {
            resolvedState.parentId = getLayerIdFromSurfaceControl(
                    resolvedState.state.getParentSurfaceControlForChild());
        }
        if (resolvedState.state.what & layer_state_t::eRelativeLayerChanged) {
            resolvedState.relativeParentId = getLayerIdFromSurfaceControl(
                    resolvedState.state.getRelativeLayerSurfaceControl());
        }
        if (resolvedState.state.what & layer_state_t::eInputInfoChanged) {
            wp<IBinder>& touchableRegionCropHandle =
                    resolvedState.state.editWindowInfo()->touchableRegionCropHandle;
            resolvedState.touchCropId =
                    LayerHandle::getLayerId(touchableRegionCropHandle.promote());
        }
    }

    QueuedTransactionState state{std::move(transactionState),
                                 std::move(resolvedStates),
                                 std::move(uncacheBufferIds),
                                 postTime,
                                 originPid,
                                 originUid};
    state.workloadHint = queuedWorkload;

    if (mTransactionTracing) {
        mTransactionTracing->addQueuedTransaction(state);
    }

    const auto schedule = [](uint32_t flags) {
        if (flags & eEarlyWakeupEnd) return TransactionSchedule::EarlyEnd;
        if (flags & eEarlyWakeupStart) return TransactionSchedule::EarlyStart;
        return TransactionSchedule::Late;
    }(state.flags);

    const auto frameHint = state.isFrameActive() ? FrameHint::kActive : FrameHint::kNone;
    // Copy fields of |state| needed after it is moved into queueTransaction
    VsyncId vsyncId{state.frameTimelineInfo.vsyncId};
    auto applyToken = state.applyToken;
    {
        // Transactions are added via a lockless queue and does not need to be added from the main
        // thread.
        ftl::FakeGuard guard(kMainThreadContext);
        mTransactionHandler.queueTransaction(std::move(state));
    }

    for (const auto& [displayId, data] : mNotifyExpectedPresentMap) {
        if (data.hintStatus.load() == NotifyExpectedPresentHintStatus::ScheduleOnTx) {
            scheduleNotifyExpectedPresentHint(displayId, vsyncId);
        }
    }
    setTransactionFlags(eTransactionFlushNeeded, schedule, applyToken, frameHint);
    return NO_ERROR;
}

bool SurfaceFlinger::applyTransactionState(
        const FrameTimelineInfo& frameTimelineInfo, std::vector<ResolvedComposerState>& states,
        std::span<DisplayState> displays, uint32_t flags,
        const InputWindowCommands& inputWindowCommands, const int64_t desiredPresentTime,
        bool isAutoTimestamp, const std::vector<uint64_t>& uncacheBufferIds, const int64_t postTime,
        bool hasListenerCallbacks, const std::vector<ListenerCallbacks>& listenerCallbacks,
        int originPid, int originUid, uint64_t transactionId) REQUIRES(mStateLock) {
    uint32_t transactionFlags = 0;

    // start and end registration for listeners w/ no surface so they can get their callback.  Note
    // that listeners with SurfaceControls will start registration during setClientStateLocked
    // below.
    for (const auto& listener : listenerCallbacks) {
        mTransactionCallbackInvoker.addEmptyTransaction(listener);
    }
    uint32_t clientStateFlags = 0;
    for (auto& resolvedState : states) {
        clientStateFlags |=
                updateLayerCallbacksAndStats(frameTimelineInfo, resolvedState, desiredPresentTime,
                                             isAutoTimestamp, postTime, transactionId);
    }

    transactionFlags |= clientStateFlags;
    transactionFlags |= addInputWindowCommands(inputWindowCommands);

    for (uint64_t uncacheBufferId : uncacheBufferIds) {
        mBufferIdsToUncache.push_back(uncacheBufferId);
    }

    // If a synchronous transaction is explicitly requested without any changes, force a transaction
    // anyway. This can be used as a flush mechanism for previous async transactions.
    // Empty animation transaction can be used to simulate back-pressure, so also force a
    // transaction for empty animation transactions.
    if (transactionFlags == 0 && (flags & eAnimation)) {
        transactionFlags = eTransactionNeeded;
    }

    bool needsTraversal = false;
    if (transactionFlags) {
        // We are on the main thread, we are about to perform a traversal. Clear the traversal bit
        // so we don't have to wake up again next frame to perform an unnecessary traversal.
        if (transactionFlags & eTraversalNeeded) {
            transactionFlags = transactionFlags & (~eTraversalNeeded);
            needsTraversal = true;
        }
        if (transactionFlags) {
            setTransactionFlags(transactionFlags);
        }
    }

    return needsTraversal;
}

bool SurfaceFlinger::applyAndCommitDisplayTransactionStatesLocked(
        std::vector<QueuedTransactionState>& transactions) {
    bool needsTraversal = false;
    uint32_t transactionFlags = 0;
    for (auto& transaction : transactions) {
        for (DisplayState& display : transaction.displays) {
            transactionFlags |= setDisplayStateLocked(display);
        }
    }

    if (transactionFlags) {
        // We are on the main thread, we are about to perform a traversal. Clear the traversal bit
        // so we don't have to wake up again next frame to perform an unnecessary traversal.
        if (transactionFlags & eTraversalNeeded) {
            transactionFlags = transactionFlags & (~eTraversalNeeded);
            needsTraversal = true;
        }
        if (transactionFlags) {
            setTransactionFlags(transactionFlags);
        }
    }

    mFrontEndDisplayInfosChanged = mTransactionFlags & eDisplayTransactionNeeded;
    if (mFrontEndDisplayInfosChanged) {
        processDisplayChangesLocked();
        mFrontEndDisplayInfos.clear();
        for (const auto& [_, display] : mDisplays) {
            mFrontEndDisplayInfos.try_emplace(display->getLayerStack(), display->getFrontEndInfo());
        }
        needsTraversal = true;
    }

    return needsTraversal;
}

uint32_t SurfaceFlinger::setDisplayStateLocked(const DisplayState& s) {
    const ssize_t index = mCurrentState.displays.indexOfKey(s.token);
    if (index < 0) return 0;

    uint32_t flags = 0;
    DisplayDeviceState& state = mCurrentState.displays.editValueAt(index);

    const uint32_t what = s.what;
    if (what & DisplayState::eSurfaceChanged) {
        if (IInterface::asBinder(state.surface) != IInterface::asBinder(s.surface)) {
            state.surface = s.surface;
            flags |= eDisplayTransactionNeeded;
        }
    }
    if (what & DisplayState::eLayerStackChanged) {
        if (state.layerStack != s.layerStack) {
            state.layerStack = s.layerStack;
            flags |= eDisplayTransactionNeeded;
        }
    }
    if (what & DisplayState::eFlagsChanged) {
        if (state.flags != s.flags) {
            state.flags = s.flags;
            flags |= eDisplayTransactionNeeded;
        }
    }
    if (what & DisplayState::eDisplayProjectionChanged) {
        if (state.orientation != s.orientation) {
            state.orientation = s.orientation;
            flags |= eDisplayTransactionNeeded;
        }
        if (state.orientedDisplaySpaceRect != s.orientedDisplaySpaceRect) {
            state.orientedDisplaySpaceRect = s.orientedDisplaySpaceRect;
            flags |= eDisplayTransactionNeeded;
        }
        if (state.layerStackSpaceRect != s.layerStackSpaceRect) {
            state.layerStackSpaceRect = s.layerStackSpaceRect;
            flags |= eDisplayTransactionNeeded;
        }
    }
    if (what & DisplayState::eDisplaySizeChanged) {
        if (state.width != s.width) {
            state.width = s.width;
            flags |= eDisplayTransactionNeeded;
        }
        if (state.height != s.height) {
            state.height = s.height;
            flags |= eDisplayTransactionNeeded;
        }
    }

    return flags;
}

bool SurfaceFlinger::callingThreadHasUnscopedSurfaceFlingerAccess(bool usePermissionCache) {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS && uid != AID_SYSTEM) &&
        (usePermissionCache ? !PermissionCache::checkPermission(sAccessSurfaceFlinger, pid, uid)
                            : !checkPermission(sAccessSurfaceFlinger, pid, uid))) {
        return false;
    }
    return true;
}

uint32_t SurfaceFlinger::updateLayerCallbacksAndStats(const FrameTimelineInfo& frameTimelineInfo,
                                                      ResolvedComposerState& composerState,
                                                      int64_t desiredPresentTime,
                                                      bool isAutoTimestamp, int64_t postTime,
                                                      uint64_t transactionId) REQUIRES(mStateLock) {
    layer_state_t& s = composerState.state;

    std::vector<ListenerCallbacks> filteredListeners;
    for (auto& listener : s.listeners) {
        // Starts a registration but separates the callback ids according to callback type. This
        // allows the callback invoker to send on latch callbacks earlier.
        // note that startRegistration will not re-register if the listener has
        // already be registered for a prior surface control

        ListenerCallbacks onCommitCallbacks = listener.filter(CallbackId::Type::ON_COMMIT);
        if (!onCommitCallbacks.callbackIds.empty()) {
            filteredListeners.push_back(onCommitCallbacks);
        }

        ListenerCallbacks onCompleteCallbacks = listener.filter(CallbackId::Type::ON_COMPLETE);
        if (!onCompleteCallbacks.callbackIds.empty()) {
            filteredListeners.push_back(onCompleteCallbacks);
        }
    }

    const uint64_t what = s.what;
    uint32_t flags = 0;
    sp<Layer> layer = nullptr;
    if (s.surface) {
        layer = LayerHandle::getLayer(s.surface);
    } else {
        // The client may provide us a null handle. Treat it as if the layer was removed.
        ALOGW("Attempt to set client state with a null layer handle");
    }
    if (layer == nullptr) {
        for (auto& [listener, callbackIds] : s.listeners) {
            mTransactionCallbackInvoker.addCallbackHandle(
                    sp<CallbackHandle>::make(listener, callbackIds, s.surface));
        }
        return 0;
    }
    if (what & layer_state_t::eProducerDisconnect) {
        layer->onDisconnect();
    }

    std::vector<sp<CallbackHandle>> callbackHandles;
    if ((what & layer_state_t::eHasListenerCallbacksChanged) && (!filteredListeners.empty())) {
        for (auto& [listener, callbackIds] : filteredListeners) {
            callbackHandles.emplace_back(
                    sp<CallbackHandle>::make(listener, callbackIds, s.surface));
        }
    }

    frontend::LayerSnapshot* snapshot = nullptr;
    gui::GameMode gameMode = gui::GameMode::Unsupported;
    if (what & (layer_state_t::eSidebandStreamChanged | layer_state_t::eBufferChanged) ||
        frameTimelineInfo.vsyncId != FrameTimelineInfo::INVALID_VSYNC_ID) {
        snapshot = mLayerSnapshotBuilder.getSnapshot(layer->sequence);
        if (snapshot) {
            gameMode = snapshot->gameMode;
        }
    }

    // TODO(b/238781169) remove after screenshot refactor, currently screenshots
    // requires to read drawing state from binder thread. So we need to fix that
    // before removing this.
    if (what & layer_state_t::eBufferTransformChanged) {
        if (layer->setTransform(s.bufferTransform)) flags |= eTraversalNeeded;
    }
    if (what & layer_state_t::eTransformToDisplayInverseChanged) {
        if (layer->setTransformToDisplayInverse(s.transformToDisplayInverse))
            flags |= eTraversalNeeded;
    }
    if (what & layer_state_t::eCropChanged) {
        if (layer->setCrop(s.crop)) flags |= eTraversalNeeded;
    }
    if (what & layer_state_t::eSidebandStreamChanged) {
        if (layer->setSidebandStream(s.sidebandStream, frameTimelineInfo, postTime, gameMode))
            flags |= eTraversalNeeded;
    }
    if (what & layer_state_t::eDataspaceChanged) {
        if (layer->setDataspace(s.dataspace)) flags |= eTraversalNeeded;
    }
    if (what & layer_state_t::eExtendedRangeBrightnessChanged) {
        if (layer->setExtendedRangeBrightness(s.currentHdrSdrRatio, s.desiredHdrSdrRatio)) {
            flags |= eTraversalNeeded;
        }
    }
    if (what & layer_state_t::eDesiredHdrHeadroomChanged) {
        if (layer->setDesiredHdrHeadroom(s.desiredHdrSdrRatio)) {
            flags |= eTraversalNeeded;
        }
    }
    if (what & layer_state_t::eBufferChanged) {
        std::optional<ui::Transform::RotationFlags> transformHint = std::nullopt;
        if (snapshot) {
            transformHint = snapshot->transformHint;
        }
        layer->setTransformHint(transformHint);
        if (layer->setBuffer(composerState.externalTexture, *s.bufferData, postTime,
                             desiredPresentTime, isAutoTimestamp, frameTimelineInfo, gameMode)) {
            flags |= eTraversalNeeded;
        }
        mLayersWithQueuedFrames.emplace(layer, gameMode);
    } else if (frameTimelineInfo.vsyncId != FrameTimelineInfo::INVALID_VSYNC_ID) {
        layer->setFrameTimelineVsyncForBufferlessTransaction(frameTimelineInfo, postTime, gameMode);
    }

    if ((what & layer_state_t::eBufferChanged) == 0) {
        layer->setDesiredPresentTime(desiredPresentTime, isAutoTimestamp);
    }

    if (what & layer_state_t::eTrustedPresentationInfoChanged) {
        if (layer->setTrustedPresentationInfo(s.trustedPresentationThresholds,
                                              s.trustedPresentationListener)) {
            flags |= eTraversalNeeded;
        }
    }

    if (what & layer_state_t::eBufferReleaseChannelChanged) {
        layer->setBufferReleaseChannel(s.bufferReleaseChannel);
    }

    const auto& requestedLayerState = mLayerLifecycleManager.getLayerFromId(layer->getSequence());
    bool willPresentCurrentTransaction = requestedLayerState &&
            (requestedLayerState->hasReadyFrame() ||
             requestedLayerState->willReleaseBufferOnLatch());
    if (layer->setTransactionCompletedListeners(callbackHandles, willPresentCurrentTransaction))
        flags |= eTraversalNeeded;

    return flags;
}

uint32_t SurfaceFlinger::addInputWindowCommands(const InputWindowCommands& inputWindowCommands) {
    bool hasChanges = mInputWindowCommands.merge(inputWindowCommands);
    return hasChanges ? eTraversalNeeded : 0;
}

status_t SurfaceFlinger::mirrorLayer(const LayerCreationArgs& args,
                                     const sp<IBinder>& mirrorFromHandle,
                                     gui::CreateSurfaceResult& outResult) {
    if (!mirrorFromHandle) {
        return NAME_NOT_FOUND;
    }

    sp<Layer> mirrorLayer;
    sp<Layer> mirrorFrom;
    LayerCreationArgs mirrorArgs = LayerCreationArgs::fromOtherArgs(args);
    {
        Mutex::Autolock _l(mStateLock);
        mirrorFrom = LayerHandle::getLayer(mirrorFromHandle);
        if (!mirrorFrom) {
            return NAME_NOT_FOUND;
        }
        mirrorArgs.flags |= ISurfaceComposerClient::eNoColorFill;
        mirrorArgs.mirrorLayerHandle = mirrorFromHandle;
        mirrorArgs.addToRoot = false;
        status_t result = createEffectLayer(mirrorArgs, &outResult.handle, &mirrorLayer);
        if (result != NO_ERROR) {
            return result;
        }
    }

    outResult.layerId = mirrorLayer->sequence;
    outResult.layerName = String16(mirrorLayer->getDebugName());
    return addClientLayer(mirrorArgs, outResult.handle, mirrorLayer /* layer */,
                          nullptr /* parent */, nullptr /* outTransformHint */);
}

status_t SurfaceFlinger::mirrorDisplay(DisplayId displayId, const LayerCreationArgs& args,
                                       gui::CreateSurfaceResult& outResult) {
    IPCThreadState* ipc = IPCThreadState::self();
    const int uid = ipc->getCallingUid();
    if (uid != AID_ROOT && uid != AID_GRAPHICS && uid != AID_SYSTEM && uid != AID_SHELL) {
        ALOGE("Permission denied when trying to mirror display");
        return PERMISSION_DENIED;
    }

    ui::LayerStack layerStack;
    sp<Layer> rootMirrorLayer;
    status_t result = 0;

    {
        Mutex::Autolock lock(mStateLock);

        const auto display = getDisplayDeviceLocked(displayId);
        if (!display) {
            return NAME_NOT_FOUND;
        }

        layerStack = display->getLayerStack();
        LayerCreationArgs mirrorArgs = LayerCreationArgs::fromOtherArgs(args);
        mirrorArgs.flags |= ISurfaceComposerClient::eNoColorFill;
        mirrorArgs.addToRoot = true;
        mirrorArgs.layerStackToMirror = layerStack;
        result = createEffectLayer(mirrorArgs, &outResult.handle, &rootMirrorLayer);
        if (result != NO_ERROR) {
            return result;
        }
        outResult.layerId = rootMirrorLayer->sequence;
        outResult.layerName = String16(rootMirrorLayer->getDebugName());
        addClientLayer(mirrorArgs, outResult.handle, rootMirrorLayer /* layer */,
                       nullptr /* parent */, nullptr /* outTransformHint */);
    }

    setTransactionFlags(eTransactionFlushNeeded);
    return NO_ERROR;
}

status_t SurfaceFlinger::createLayer(LayerCreationArgs& args, gui::CreateSurfaceResult& outResult) {
    status_t result = NO_ERROR;

    sp<Layer> layer;

    switch (args.flags & ISurfaceComposerClient::eFXSurfaceMask) {
        case ISurfaceComposerClient::eFXSurfaceBufferQueue:
        case ISurfaceComposerClient::eFXSurfaceContainer:
        case ISurfaceComposerClient::eFXSurfaceBufferState:
            args.flags |= ISurfaceComposerClient::eNoColorFill;
            [[fallthrough]];
        case ISurfaceComposerClient::eFXSurfaceEffect: {
            result = createBufferStateLayer(args, &outResult.handle, &layer);
            if (result != NO_ERROR) {
                return result;
            }
            std::atomic<int32_t>* pendingBufferCounter = layer->getPendingBufferCounter();
            if (pendingBufferCounter) {
                std::string counterName = layer->getPendingBufferCounterName();
                mBufferCountTracker.add(LayerHandle::getLayerId(outResult.handle), counterName,
                                        pendingBufferCounter);
                args.pendingBuffers = pendingBufferCounter;
            }
        } break;
        default:
            result = BAD_VALUE;
            break;
    }

    if (result != NO_ERROR) {
        return result;
    }

    args.addToRoot = args.addToRoot && callingThreadHasUnscopedSurfaceFlingerAccess();
    // We can safely promote the parent layer in binder thread because we have a strong reference
    // to the layer's handle inside this scope.
    sp<Layer> parent = LayerHandle::getLayer(args.parentHandle.promote());
    if (args.parentHandle != nullptr && parent == nullptr) {
        ALOGE("Invalid parent handle %p", args.parentHandle.promote().get());
        args.addToRoot = false;
    }

    uint32_t outTransformHint;
    result = addClientLayer(args, outResult.handle, layer, parent, &outTransformHint);
    if (result != NO_ERROR) {
        return result;
    }

    outResult.transformHint = static_cast<int32_t>(outTransformHint);
    outResult.layerId = layer->sequence;
    outResult.layerName = String16(layer->getDebugName());
    return result;
}

status_t SurfaceFlinger::createBufferStateLayer(LayerCreationArgs& args, sp<IBinder>* handle,
                                                sp<Layer>* outLayer) {
    if (checkLayerLeaks() != NO_ERROR) {
        return NO_MEMORY;
    }
    *outLayer = getFactory().createBufferStateLayer(args);
    *handle = (*outLayer)->getHandle();
    return NO_ERROR;
}

status_t SurfaceFlinger::createEffectLayer(const LayerCreationArgs& args, sp<IBinder>* handle,
                                           sp<Layer>* outLayer) {
    if (checkLayerLeaks() != NO_ERROR) {
        return NO_MEMORY;
    }
    *outLayer = getFactory().createEffectLayer(args);
    *handle = (*outLayer)->getHandle();
    return NO_ERROR;
}

status_t SurfaceFlinger::checkLayerLeaks() {
    if (mNumLayers >= MAX_LAYERS) {
        static std::atomic<nsecs_t> lasttime{0};
        nsecs_t now = systemTime();
        if (lasttime != 0 && ns2s(now - lasttime.load()) < 10) {
            ALOGE("CreateLayer already dumped 10s before");
            return NO_MEMORY;
        } else {
            lasttime = now;
        }

        ALOGE("CreateLayer failed, mNumLayers (%zu) >= MAX_LAYERS (%zu)", mNumLayers.load(),
              MAX_LAYERS);
        static_cast<void>(mScheduler->schedule([&]() FTL_FAKE_GUARD(kMainThreadContext) {
            ALOGE("Dumping on-screen layers.");
            mLayerHierarchyBuilder.dumpLayerSample(mLayerHierarchyBuilder.getHierarchy());
            ALOGE("Dumping off-screen layers.");
            mLayerHierarchyBuilder.dumpLayerSample(mLayerHierarchyBuilder.getOffscreenHierarchy());
        }));
        return NO_MEMORY;
    }
    return NO_ERROR;
}

void SurfaceFlinger::onHandleDestroyed(sp<Layer>& layer, uint32_t layerId) {
    {
        // Used to remove stalled transactions which uses an internal lock.
        ftl::FakeGuard guard(kMainThreadContext);
        mTransactionHandler.onLayerDestroyed(layerId);
    }
    JankTracker::flushJankData(layerId);

    std::scoped_lock<std::mutex> lock(mCreatedLayersLock);
    mDestroyedHandles.emplace_back(layerId, layer->getDebugName());

    Mutex::Autolock stateLock(mStateLock);
    layer->onHandleDestroyed();
    mBufferCountTracker.remove(layerId);
    layer.clear();
    setTransactionFlags(eTransactionFlushNeeded | eTransactionNeeded);
}

void SurfaceFlinger::initializeDisplays() {
    QueuedTransactionState state;
    state.inputWindowCommands = mInputWindowCommands;
    const nsecs_t now = systemTime();
    state.desiredPresentTime = now;
    state.postTime = now;
    state.originPid = mPid;
    state.originUid = static_cast<int>(getuid());
    const uint64_t transactionId = (static_cast<uint64_t>(mPid) << 32) | mUniqueTransactionId++;
    state.id = transactionId;

    auto layerStack = ui::DEFAULT_LAYER_STACK.id;
    for (const auto& [id, display] : FTL_FAKE_GUARD(mStateLock, mPhysicalDisplays)) {
        state.displays.emplace_back(
                DisplayState(display.token(), ui::LayerStack::fromValue(layerStack++)));
    }

    std::vector<QueuedTransactionState> transactions;
    transactions.emplace_back(state);

    {
        Mutex::Autolock lock(mStateLock);
        applyAndCommitDisplayTransactionStatesLocked(transactions);
    }

    {
        ftl::FakeGuard guard(mStateLock);

        // In case of a restart, ensure all displays are off.
        for (const auto& [id, display] : mPhysicalDisplays) {
            setPhysicalDisplayPowerMode(getDisplayDeviceLocked(id), hal::PowerMode::OFF);
        }

        // Power on all displays. The primary display is first, so becomes the active display. Also,
        // the DisplayCapability set of a display is populated on its first powering on. Do this now
        // before responding to any Binder query from DisplayManager about display capabilities.
        // Additionally, do not turn on displays if the boot should be quiescent.
        if (!mSkipPowerOnForQuiescent) {
            for (const auto& [id, display] : mPhysicalDisplays) {
                setPhysicalDisplayPowerMode(getDisplayDeviceLocked(id), hal::PowerMode::ON);
            }
        }
    }
}

void SurfaceFlinger::setPhysicalDisplayPowerMode(const sp<DisplayDevice>& display,
                                                 hal::PowerMode mode) {
    if (display->isVirtual()) {
        // TODO(b/241285876): This code path should not be reachable, so enforce this at compile
        // time.
        ALOGE("%s: Invalid operation on virtual display", __func__);
        return;
    }

    const auto displayId = display->getPhysicalId();
    ALOGD("Setting power mode %d on physical display %s", mode, to_string(displayId).c_str());

    const auto currentMode = display->getPowerMode();
    if (currentMode == mode) {
        return;
    }

    const bool isInternalDisplay = mPhysicalDisplays.get(displayId)
                                           .transform(&PhysicalDisplay::isInternal)
                                           .value_or(false);

    const auto activeDisplay = getDisplayDeviceLocked(mActiveDisplayId);

    ALOGW_IF(display != activeDisplay && isInternalDisplay && activeDisplay &&
                     activeDisplay->isPoweredOn(),
             "Trying to change power mode on inactive display without powering off active display");

    const bool couldRefresh = display->isRefreshable();
    display->setPowerMode(mode);
    const bool canRefresh = display->isRefreshable();

    if (couldRefresh && !canRefresh) {
        decRefreshableDisplays();
    } else if (!couldRefresh && canRefresh) {
        incRefreshableDisplays();
    }

    const auto activeMode = display->refreshRateSelector().getActiveMode().modePtr;
    if (currentMode == hal::PowerMode::OFF) {
        // Turn on the display

        // Activate the display (which involves a modeset to the active mode) when the inner or
        // outer display of a foldable is powered on. This condition relies on the above
        // DisplayDevice::setPowerMode. If `display` and `activeDisplay` are the same display,
        // then the `activeDisplay->isPoweredOn()` below is true, such that the display is not
        // activated every time it is powered on.
        //
        // TODO(b/255635821): Remove the concept of active display.
        if (isInternalDisplay && (!activeDisplay || !activeDisplay->isPoweredOn())) {
            onActiveDisplayChangedLocked(activeDisplay.get(), *display);
        }

        if (displayId == mActiveDisplayId) {
            if (FlagManager::getInstance().correct_virtual_display_power_state()) {
                applyOptimizationPolicy("setPhysicalDisplayPowerMode(ON)");
            } else {
                disablePowerOptimizations("setPhysicalDisplayPowerMode(ON)");
            }
        }

        getHwComposer().setPowerMode(displayId, mode);
        if (mode != hal::PowerMode::DOZE_SUSPEND) {
            const bool enable =
                    mScheduler->getVsyncSchedule(displayId)->getPendingHardwareVsyncState();
            requestHardwareVsync(displayId, enable);

            if (displayId == mActiveDisplayId) {
                mScheduler->enableSyntheticVsync(false);
            }

            constexpr bool kAllowToEnable = true;
            mScheduler->resyncToHardwareVsync(displayId, kAllowToEnable, activeMode.get());
        }

        mVisibleRegionsDirty = true;
        scheduleComposite(FrameHint::kActive);
    } else if (mode == hal::PowerMode::OFF) {
        const bool currentModeNotDozeSuspend = (currentMode != hal::PowerMode::DOZE_SUSPEND);
        // Turn off the display
        if (displayId == mActiveDisplayId) {
            if (const auto display = getActivatableDisplay()) {
                onActiveDisplayChangedLocked(activeDisplay.get(), *display);
            } else {
                if (FlagManager::getInstance().correct_virtual_display_power_state()) {
                    applyOptimizationPolicy("setPhysicalDisplayPowerMode(OFF)");
                } else {
                    enablePowerOptimizations("setPhysicalDisplayPowerMode(OFF)");
                }

                if (currentModeNotDozeSuspend) {
                    mScheduler->enableSyntheticVsync();
                }
            }
        }
        if (currentModeNotDozeSuspend) {
            constexpr bool kDisallow = true;
            mScheduler->disableHardwareVsync(displayId, kDisallow);
        }

        // We must disable VSYNC *before* turning off the display. The call to
        // disableHardwareVsync, above, schedules a task to turn it off after
        // this method returns. But by that point, the display is OFF, so the
        // call just updates the pending state, without actually disabling
        // VSYNC.
        requestHardwareVsync(displayId, false);
        getHwComposer().setPowerMode(displayId, mode);

        mVisibleRegionsDirty = true;
        // from this point on, SF will stop drawing on this display
    } else if (mode == hal::PowerMode::DOZE || mode == hal::PowerMode::ON) {
        // Update display while dozing
        getHwComposer().setPowerMode(displayId, mode);
        if (currentMode == hal::PowerMode::DOZE_SUSPEND) {
            if (displayId == mActiveDisplayId) {
                ALOGI("Force repainting for DOZE_SUSPEND -> DOZE or ON.");
                mVisibleRegionsDirty = true;
                scheduleRepaint();
                mScheduler->enableSyntheticVsync(false);
            }
            constexpr bool kAllowToEnable = true;
            mScheduler->resyncToHardwareVsync(displayId, kAllowToEnable, activeMode.get());
        }
    } else if (mode == hal::PowerMode::DOZE_SUSPEND) {
        // Leave display going to doze
        constexpr bool kDisallow = true;
        mScheduler->disableHardwareVsync(displayId, kDisallow);

        if (displayId == mActiveDisplayId) {
            mScheduler->enableSyntheticVsync();
        }
        getHwComposer().setPowerMode(displayId, mode);
    } else {
        ALOGE("Attempting to set unknown power mode: %d\n", mode);
        getHwComposer().setPowerMode(displayId, mode);
    }

    if (displayId == mActiveDisplayId) {
        mTimeStats->setPowerMode(mode);
        mScheduler->setActiveDisplayPowerModeForRefreshRateStats(mode);
    }

    mScheduler->setDisplayPowerMode(displayId, mode);

    ALOGD("Finished setting power mode %d on physical display %s", mode,
          to_string(displayId).c_str());
}

void SurfaceFlinger::setVirtualDisplayPowerMode(const sp<DisplayDevice>& display,
                                                hal::PowerMode mode) {
    if (!display->isVirtual()) {
        ALOGE("%s: Invalid operation on physical display", __func__);
        return;
    }

    const auto displayId = display->getVirtualId();
    ALOGD("Setting power mode %d on virtual display %s %s", mode, to_string(displayId).c_str(),
          display->getDisplayName().c_str());

    display->setPowerMode(static_cast<hal::PowerMode>(mode));

    applyOptimizationPolicy(__func__);

    ALOGD("Finished setting power mode %d on virtual display %s", mode,
          to_string(displayId).c_str());
}

bool SurfaceFlinger::shouldOptimizeForPerformance() {
    for (const auto& [_, display] : mDisplays) {
        // Displays that are optimized for power are always powered on and should not influence
        // whether there is an active display for the purpose of power optimization, etc. If these
        // displays are being shown somewhere, a different (physical or virtual) display that is
        // optimized for performance will be powered on in addition. Displays optimized for
        // performance will change power mode, so if they are off then they are not active.
        if (display->isPoweredOn() &&
            display->getOptimizationPolicy() ==
                    gui::ISurfaceComposer::OptimizationPolicy::optimizeForPerformance) {
            return true;
        }
    }
    return false;
}

void SurfaceFlinger::enablePowerOptimizations(const char* whence) {
    ALOGD("%s: Enabling power optimizations", whence);

    setSchedAttr(false, whence);
    setSchedFifo(false, whence);
}

void SurfaceFlinger::disablePowerOptimizations(const char* whence) {
    ALOGD("%s: Disabling power optimizations", whence);

    // TODO: b/281692563 - Merge the syscalls. For now, keep uclamp in a separate syscall
    // and set it before SCHED_FIFO due to b/190237315.
    setSchedAttr(true, whence);
    setSchedFifo(true, whence);
}

void SurfaceFlinger::applyOptimizationPolicy(const char* whence) {
    if (shouldOptimizeForPerformance()) {
        disablePowerOptimizations(whence);
    } else {
        enablePowerOptimizations(whence);
    }
}

void SurfaceFlinger::setPowerMode(const sp<IBinder>& displayToken, int mode) {
    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(kMainThreadContext) {
        mSkipPowerOnForQuiescent = false;
        const auto display = FTL_FAKE_GUARD(mStateLock, getDisplayDeviceLocked(displayToken));
        if (!display) {
            Mutex::Autolock lock(mStateLock);
            const ssize_t index = mCurrentState.displays.indexOfKey(displayToken);
            if (index >= 0) {
                auto& state = mCurrentState.displays.editValueFor(displayToken);
                if (state.isVirtual()) {
                    ALOGD("Setting power mode %d for a dormant virtual display with token %p", mode,
                          displayToken.get());
                    state.initialPowerMode = static_cast<hal::PowerMode>(mode);
                    return;
                }
            }
            ALOGE("Failed to set power mode %d for display token %p", mode, displayToken.get());
        } else if (display->isVirtual()) {
            if (FlagManager::getInstance().correct_virtual_display_power_state()) {
                ftl::FakeGuard guard(mStateLock);
                setVirtualDisplayPowerMode(display, static_cast<hal::PowerMode>(mode));
            } else {
                ALOGW("Attempt to set power mode %d for virtual display", mode);
            }
        } else {
            ftl::FakeGuard guard(mStateLock);
            setPhysicalDisplayPowerMode(display, static_cast<hal::PowerMode>(mode));
        }
    });

    future.wait();
}

status_t SurfaceFlinger::doDump(int fd, const DumpArgs& args, bool asProto) {
    std::string result;

    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();

    if ((uid != AID_SHELL) && !PermissionCache::checkPermission(sDump, pid, uid)) {
        StringAppendF(&result, "Permission Denial: can't dump SurfaceFlinger from pid=%d, uid=%d\n",
                      pid, uid);
        write(fd, result.c_str(), result.size());
        return NO_ERROR;
    }

    if (asProto && args.empty()) {
        perfetto::protos::LayersTraceFileProto traceFileProto =
                mLayerTracing.createTraceFileProto();
        perfetto::protos::LayersSnapshotProto* layersTrace = traceFileProto.add_entry();
        perfetto::protos::LayersProto layersProto = dumpProtoFromMainThread();
        layersTrace->mutable_layers()->Swap(&layersProto);
        auto displayProtos = dumpDisplayProto();
        layersTrace->mutable_displays()->Swap(&displayProtos);
        result.append(traceFileProto.SerializeAsString());
        write(fd, result.c_str(), result.size());
        return NO_ERROR;
    }

    static const std::unordered_map<std::string, Dumper> dumpers = {
            {"--comp-displays"s, dumper(&SurfaceFlinger::dumpCompositionDisplays)},
            {"--display-id"s, dumper(&SurfaceFlinger::dumpDisplayIdentificationData)},
            {"--displays"s, dumper(&SurfaceFlinger::dumpDisplays)},
            {"--edid"s, argsDumper(&SurfaceFlinger::dumpRawDisplayIdentificationData)},
            {"--events"s, dumper(&SurfaceFlinger::dumpEvents)},
            {"--frametimeline"s, argsDumper(&SurfaceFlinger::dumpFrameTimeline)},
            {"--frontend"s, mainThreadDumper(&SurfaceFlinger::dumpFrontEnd)},
            {"--hdrinfo"s, dumper(&SurfaceFlinger::dumpHdrInfo)},
            {"--hwclayers"s, mainThreadDumper(&SurfaceFlinger::dumpHwcLayersMinidump)},
            {"--latency"s, argsMainThreadDumper(&SurfaceFlinger::dumpStats)},
            {"--latency-clear"s, argsMainThreadDumper(&SurfaceFlinger::clearStats)},
            {"--list"s, mainThreadDumper(&SurfaceFlinger::listLayers)},
            {"--planner"s, argsDumper(&SurfaceFlinger::dumpPlannerInfo)},
            {"--scheduler"s, dumper(&SurfaceFlinger::dumpScheduler)},
            {"--timestats"s, protoDumper(&SurfaceFlinger::dumpTimeStats)},
            {"--vsync"s, dumper(&SurfaceFlinger::dumpVsync)},
            {"--wide-color"s, dumper(&SurfaceFlinger::dumpWideColorInfo)},
    };

    const auto flag = args.empty() ? ""s : std::string(String8(args[0]));
    if (const auto it = dumpers.find(flag); it != dumpers.end()) {
        (it->second)(args, asProto, result);
        write(fd, result.c_str(), result.size());
        return NO_ERROR;
    }

    // Collect debug data from main thread
    std::string compositionLayers;
    mScheduler
            ->schedule([&]() FTL_FAKE_GUARD(mStateLock) FTL_FAKE_GUARD(kMainThreadContext) {
                dumpVisibleFrontEnd(compositionLayers);
            })
            .get();
    // get window info listener data without the state lock
    auto windowInfosDebug = mWindowInfosListenerInvoker->getDebugInfo();
    compositionLayers.append("Window Infos:\n");
    StringAppendF(&compositionLayers, "  max send vsync id: %" PRId64 "\n",
                  ftl::to_underlying(windowInfosDebug.maxSendDelayVsyncId));
    StringAppendF(&compositionLayers, "  max send delay (ns): %" PRId64 " ns\n",
                  windowInfosDebug.maxSendDelayDuration);
    StringAppendF(&compositionLayers, "  unsent messages: %zu\n",
                  windowInfosDebug.pendingMessageCount);
    compositionLayers.append("\n");
    dumpAll(args, compositionLayers, result);
    write(fd, result.c_str(), result.size());
    return NO_ERROR;
}

status_t SurfaceFlinger::dumpCritical(int fd, const DumpArgs&, bool asProto) {
    return doDump(fd, DumpArgs(), asProto);
}

void SurfaceFlinger::listLayers(std::string& result) const {
    for (const auto& layer : mLayerLifecycleManager.getLayers()) {
        StringAppendF(&result, "%s\n", layer->getDebugString().c_str());
    }
}

void SurfaceFlinger::dumpStats(const DumpArgs& args, std::string& result) const {
    StringAppendF(&result, "%" PRId64 "\n", mScheduler->getPacesetterVsyncPeriod().ns());
    if (args.size() < 2) return;

    const auto name = String8(args[1]);
    traverseLegacyLayers([&](Layer* layer) {
        if (layer->getName() == name.c_str()) {
            layer->dumpFrameStats(result);
        }
    });
}

void SurfaceFlinger::clearStats(const DumpArgs& args, std::string&) {
    const bool clearAll = args.size() < 2;
    const auto name = clearAll ? String8() : String8(args[1]);

    traverseLegacyLayers([&](Layer* layer) {
        if (clearAll || layer->getName() == name.c_str()) {
            layer->clearFrameStats();
        }
    });
}

void SurfaceFlinger::dumpTimeStats(const DumpArgs& args, bool asProto, std::string& result) const {
    mTimeStats->parseArgs(asProto, args, result);
}

void SurfaceFlinger::dumpFrameTimeline(const DumpArgs& args, std::string& result) const {
    mFrameTimeline->parseArgs(args, result);
}

void SurfaceFlinger::logFrameStats(TimePoint now) {
    static TimePoint sTimestamp = now;
    if (now - sTimestamp < 30min) return;
    sTimestamp = now;

    SFTRACE_CALL();
    traverseLegacyLayers([&](Layer* layer) { layer->logFrameStats(); });
}

void SurfaceFlinger::appendSfConfigString(std::string& result) const {
    result.append(" [sf");

    StringAppendF(&result, " PRESENT_TIME_OFFSET=%" PRId64, dispSyncPresentTimeOffset);
    StringAppendF(&result, " FORCE_HWC_FOR_RBG_TO_YUV=%d", useHwcForRgbToYuv);
    StringAppendF(&result, " MAX_VIRT_DISPLAY_DIM=%zu",
                  getHwComposer().getMaxVirtualDisplayDimension());
    StringAppendF(&result, " RUNNING_WITHOUT_SYNC_FRAMEWORK=%d", !hasSyncFramework);
    StringAppendF(&result, " NUM_FRAMEBUFFER_SURFACE_BUFFERS=%" PRId64,
                  maxFrameBufferAcquiredBuffers);
    result.append("]");
}

void SurfaceFlinger::dumpScheduler(std::string& result) const {
    utils::Dumper dumper{result};

    mScheduler->dump(dumper);

    // TODO(b/241285876): Move to DisplayModeController.
    dumper.dump("debugDisplayModeSetByBackdoor"sv, mDebugDisplayModeSetByBackdoor);
    dumper.eol();
}

void SurfaceFlinger::dumpEvents(std::string& result) const {
    mScheduler->dump(scheduler::Cycle::Render, result);
}

void SurfaceFlinger::dumpVsync(std::string& result) const {
    mScheduler->dumpVsync(result);
}

void SurfaceFlinger::dumpPlannerInfo(const DumpArgs& args, std::string& result) const {
    for (const auto& [token, display] : mDisplays) {
        const auto compositionDisplay = display->getCompositionDisplay();
        compositionDisplay->dumpPlannerInfo(args, result);
    }
}

void SurfaceFlinger::dumpCompositionDisplays(std::string& result) const {
    for (const auto& [token, display] : mDisplays) {
        display->getCompositionDisplay()->dump(result);
        result += '\n';
    }
}

void SurfaceFlinger::dumpDisplays(std::string& result) const {
    utils::Dumper dumper{result};

    for (const auto& [id, display] : mPhysicalDisplays) {
        utils::Dumper::Section section(dumper, ftl::Concat("Display ", id.value).str());

        display.snapshot().dump(dumper);

        if (const auto device = getDisplayDeviceLocked(id)) {
            device->dump(dumper);
        }
    }

    for (const auto& [token, display] : mDisplays) {
        if (display->isVirtual()) {
            const VirtualDisplayId virtualId = display->getVirtualId();
            utils::Dumper::Section section(dumper,
                                           ftl::Concat("Virtual Display ", virtualId.value).str());
            display->dump(dumper);

            std::lock_guard lock(mVirtualDisplaysMutex);
            if (const auto snapshotOpt = mVirtualDisplays.get(virtualId)) {
                snapshotOpt->get().dump(dumper);
            }
        }
    }
}

void SurfaceFlinger::dumpDisplayIdentificationData(std::string& result) const {
    for (const auto& [token, display] : mDisplays) {
        const auto displayId = asPhysicalDisplayId(display->getDisplayIdVariant());
        if (!displayId) {
            continue;
        }

        const auto hwcDisplayId = getHwComposer().fromPhysicalDisplayId(*displayId);
        if (!hwcDisplayId) {
            continue;
        }

        StringAppendF(&result,
                      "Display %s (HWC display %" PRIu64 "): ", to_string(*displayId).c_str(),
                      *hwcDisplayId);

        uint8_t port;
        DisplayIdentificationData data;
        if (!getHwComposer().getDisplayIdentificationData(*hwcDisplayId, &port, &data)) {
            result.append("no display identification data\n");
            continue;
        }

        if (data.empty()) {
            result.append("empty display identification data\n");
            continue;
        }

        if (!isEdid(data)) {
            result.append("unknown format for display identification data\n");
            continue;
        }

        const auto edid = parseEdid(data);
        if (!edid) {
            result.append("invalid EDID\n");
            continue;
        }

        StringAppendF(&result, "port=%u pnpId=%s displayName=\"", port, edid->pnpId.data());
        result.append(edid->displayName.data(), edid->displayName.length());
        result.append("\"\n");
    }

    for (const auto& [token, display] : mDisplays) {
        const auto virtualDisplayId = asVirtualDisplayId(display->getDisplayIdVariant());
        if (virtualDisplayId) {
            StringAppendF(&result, "Display %s (Virtual display): displayName=\"%s\"",
                          to_string(*virtualDisplayId).c_str(), display->getDisplayName().c_str());
            std::lock_guard lock(mVirtualDisplaysMutex);
            if (const auto snapshotOpt = mVirtualDisplays.get(*virtualDisplayId)) {
                StringAppendF(&result, " uniqueId=\"%s\"", snapshotOpt->get().uniqueId().c_str());
            }
            result.append("\n");
        }
    }
}

void SurfaceFlinger::dumpRawDisplayIdentificationData(const DumpArgs& args,
                                                      std::string& result) const {
    hal::HWDisplayId hwcDisplayId;
    uint8_t port;
    DisplayIdentificationData data;

    if (args.size() > 1 && base::ParseUint(String8(args[1]), &hwcDisplayId) &&
        getHwComposer().getDisplayIdentificationData(hwcDisplayId, &port, &data)) {
        result.append(reinterpret_cast<const char*>(data.data()), data.size());
    }
}

void SurfaceFlinger::dumpWideColorInfo(std::string& result) const {
    StringAppendF(&result, "Device supports wide color: %d\n", mSupportsWideColor);
    StringAppendF(&result, "DisplayColorSetting: %s\n",
                  decodeDisplayColorSetting(mDisplayColorSetting).c_str());

    // TODO: print out if wide-color mode is active or not.

    for (const auto& [id, display] : mPhysicalDisplays) {
        StringAppendF(&result, "Display %s color modes:\n", to_string(id).c_str());
        for (const auto mode : display.snapshot().colorModes()) {
            StringAppendF(&result, "    %s (%d)\n", decodeColorMode(mode).c_str(),
                          fmt::underlying(mode));
        }

        if (const auto display = getDisplayDeviceLocked(id)) {
            ui::ColorMode currentMode = display->getCompositionDisplay()->getState().colorMode;
            StringAppendF(&result, "    Current color mode: %s (%d)\n",
                          decodeColorMode(currentMode).c_str(), fmt::underlying(currentMode));
        }
    }
    result.append("\n");
}

void SurfaceFlinger::dumpHdrInfo(std::string& result) const {
    for (const auto& [displayId, listener] : mHdrLayerInfoListeners) {
        StringAppendF(&result, "HDR events for display %" PRIu64 "\n", displayId.value);
        listener->dump(result);
        result.append("\n");
    }
}

void SurfaceFlinger::dumpFrontEnd(std::string& result) {
    std::ostringstream out;
    out << "\nComposition list (bottom to top)\n";
    ui::LayerStack lastPrintedLayerStackHeader = ui::INVALID_LAYER_STACK;
    for (const auto& snapshot : mLayerSnapshotBuilder.getSnapshots()) {
        if (lastPrintedLayerStackHeader != snapshot->outputFilter.layerStack) {
            lastPrintedLayerStackHeader = snapshot->outputFilter.layerStack;
            out << "LayerStack=" << lastPrintedLayerStackHeader.id << "\n";
        }
        out << "  " << *snapshot << "\n";
    }

    out << "\nInput list\n";
    lastPrintedLayerStackHeader = ui::INVALID_LAYER_STACK;
    mLayerSnapshotBuilder.forEachInputSnapshot([&](const frontend::LayerSnapshot& snapshot) {
        if (lastPrintedLayerStackHeader != snapshot.outputFilter.layerStack) {
            lastPrintedLayerStackHeader = snapshot.outputFilter.layerStack;
            out << "LayerStack=" << lastPrintedLayerStackHeader.id << "\n";
        }
        out << "  " << snapshot << "\n";
    });

    out << "\nLayer Hierarchy\n"
        << mLayerHierarchyBuilder.getHierarchy().dump() << "\nOffscreen Hierarchy\n"
        << mLayerHierarchyBuilder.getOffscreenHierarchy().dump() << "\n\n";
    result.append(out.str());
}

void SurfaceFlinger::dumpVisibleFrontEnd(std::string& result) {
    std::ostringstream out;
    out << "\nComposition list (bottom to top)\n";
    ui::LayerStack lastPrintedLayerStackHeader = ui::INVALID_LAYER_STACK;
    mLayerSnapshotBuilder.forEachVisibleSnapshot(
            [&](std::unique_ptr<frontend::LayerSnapshot>& snapshot) {
                if (snapshot->hasSomethingToDraw()) {
                    if (lastPrintedLayerStackHeader != snapshot->outputFilter.layerStack) {
                        lastPrintedLayerStackHeader = snapshot->outputFilter.layerStack;
                        out << "LayerStack=" << lastPrintedLayerStackHeader.id << "\n";
                    }
                    out << "  " << *snapshot << "\n";
                }
            });

    out << "\nInput list\n";
    lastPrintedLayerStackHeader = ui::INVALID_LAYER_STACK;
    mLayerSnapshotBuilder.forEachInputSnapshot([&](const frontend::LayerSnapshot& snapshot) {
        if (lastPrintedLayerStackHeader != snapshot.outputFilter.layerStack) {
            lastPrintedLayerStackHeader = snapshot.outputFilter.layerStack;
            out << "LayerStack=" << lastPrintedLayerStackHeader.id << "\n";
        }
        out << "  " << snapshot << "\n";
    });

    out << "\nLayer Hierarchy\n"
        << mLayerHierarchyBuilder.getHierarchy() << "\nOffscreen Hierarchy\n"
        << mLayerHierarchyBuilder.getOffscreenHierarchy() << "\n\n";
    result = out.str();
    dumpHwcLayersMinidump(result);
}

perfetto::protos::LayersProto SurfaceFlinger::dumpDrawingStateProto(uint32_t traceFlags) const {
    std::unordered_set<uint64_t> stackIdsToSkip;

    // Determine if virtual layers display should be skipped
    if ((traceFlags & LayerTracing::TRACE_VIRTUAL_DISPLAYS) == 0) {
        for (const auto& [_, display] : FTL_FAKE_GUARD(mStateLock, mDisplays)) {
            if (display->isVirtual()) {
                stackIdsToSkip.insert(display->getLayerStack().id);
            }
        }
    }

    auto traceGenerator =
            LayerProtoFromSnapshotGenerator(mLayerSnapshotBuilder, mFrontEndDisplayInfos,
                                            mLegacyLayers, traceFlags)
                    .with(mLayerHierarchyBuilder.getHierarchy());

    if (traceFlags & LayerTracing::Flag::TRACE_EXTRA) {
        traceGenerator.withOffscreenLayers(mLayerHierarchyBuilder.getOffscreenHierarchy());
    }

    return traceGenerator.generate();
}

google::protobuf::RepeatedPtrField<perfetto::protos::DisplayProto>
SurfaceFlinger::dumpDisplayProto() const {
    google::protobuf::RepeatedPtrField<perfetto::protos::DisplayProto> displays;
    for (const auto& [_, display] : FTL_FAKE_GUARD(mStateLock, mDisplays)) {
        perfetto::protos::DisplayProto* displayProto = displays.Add();
        displayProto->set_id(display->getId().value);
        displayProto->set_name(display->getDisplayName());
        displayProto->set_layer_stack(display->getLayerStack().id);

        if (!display->isVirtual()) {
            const auto dpi = display->refreshRateSelector().getActiveMode().modePtr->getDpi();
            displayProto->set_dpi_x(dpi.x);
            displayProto->set_dpi_y(dpi.y);
        }

        LayerProtoHelper::writeSizeToProto(display->getWidth(), display->getHeight(),
                                           [&]() { return displayProto->mutable_size(); });
        LayerProtoHelper::writeToProto(display->getLayerStackSpaceRect(), [&]() {
            return displayProto->mutable_layer_stack_space_rect();
        });
        LayerProtoHelper::writeTransformToProto(display->getTransform(),
                                                displayProto->mutable_transform());
        displayProto->set_is_virtual(display->isVirtual());
    }
    return displays;
}

void SurfaceFlinger::dumpHwc(std::string& result) const {
    getHwComposer().dump(result);
}

perfetto::protos::LayersProto SurfaceFlinger::dumpProtoFromMainThread(uint32_t traceFlags) {
    return mScheduler
            ->schedule([=, this]() FTL_FAKE_GUARD(kMainThreadContext) {
                return dumpDrawingStateProto(traceFlags);
            })
            .get();
}

void SurfaceFlinger::dumpHwcLayersMinidump(std::string& result) const {
    for (const auto& [token, display] : mDisplays) {
        const auto displayId = asHalDisplayId(display->getDisplayIdVariant());
        if (!displayId) {
            continue;
        }

        StringAppendF(&result, "Display %s (%s) HWC layers:\n", to_string(*displayId).c_str(),
                      displayId == mActiveDisplayId ? "active" : "inactive");
        Layer::miniDumpHeader(result);

        const DisplayDevice& ref = *display;
        mLayerSnapshotBuilder.forEachVisibleSnapshot(
                [&](const frontend::LayerSnapshot& snapshot) FTL_FAKE_GUARD(kMainThreadContext) {
                    if (!snapshot.hasSomethingToDraw() ||
                        ref.getLayerStack() != snapshot.outputFilter.layerStack) {
                        return;
                    }
                    auto it = mLegacyLayers.find(snapshot.sequence);
                    LLOG_ALWAYS_FATAL_WITH_TRACE_IF(it == mLegacyLayers.end(),
                                                    "Couldnt find layer object for %s",
                                                    snapshot.getDebugString().c_str());
                    it->second->miniDump(result, snapshot, ref);
                });
        result.append("\n");
    }
}

void SurfaceFlinger::dumpAll(const DumpArgs& args, const std::string& compositionLayers,
                             std::string& result) const {
    TimedLock lock(mStateLock, s2ns(1), __func__);
    if (!lock.locked()) {
        StringAppendF(&result, "Dumping without lock after timeout: %s (%d)\n",
                      strerror(-lock.status), lock.status);
    }

    const bool colorize = !args.empty() && args[0] == String16("--color");
    Colorizer colorizer(colorize);

    // figure out if we're stuck somewhere
    const nsecs_t now = systemTime();
    const nsecs_t inTransaction(mDebugInTransaction);
    nsecs_t inTransactionDuration = (inTransaction) ? now - inTransaction : 0;

    /*
     * Dump library configuration.
     */

    colorizer.bold(result);
    result.append("Build configuration:");
    colorizer.reset(result);
    appendSfConfigString(result);
    result.append("\n");

    result.append("\nDisplay identification data:\n");
    dumpDisplayIdentificationData(result);

    result.append("\nWide-Color information:\n");
    dumpWideColorInfo(result);

    dumpHdrInfo(result);

    colorizer.bold(result);
    result.append("Sync configuration: ");
    colorizer.reset(result);
    result.append(SyncFeatures::getInstance().toString());
    result.append("\n\n");

    colorizer.bold(result);
    result.append("Scheduler:\n");
    colorizer.reset(result);
    dumpScheduler(result);
    dumpEvents(result);
    dumpVsync(result);
    result.append("\n");

    /*
     * Dump the visible layer list
     */
    colorizer.bold(result);
    StringAppendF(&result, "SurfaceFlinger New Frontend Enabled:%s\n", "true");
    StringAppendF(&result, "Active Layers - layers with client handles (count = %zu)\n",
                  mNumLayers.load());
    colorizer.reset(result);

    result.append(compositionLayers);

    colorizer.bold(result);
    StringAppendF(&result, "Displays (%zu entries)\n", mDisplays.size());
    colorizer.reset(result);
    dumpDisplays(result);
    dumpCompositionDisplays(result);
    result.push_back('\n');

    mCompositionEngine->dump(result);

    /*
     * Dump SurfaceFlinger global state
     */

    colorizer.bold(result);
    result.append("SurfaceFlinger global state:\n");
    colorizer.reset(result);

    getRenderEngine().dump(result);

    result.append("ClientCache state:\n");
    ClientCache::getInstance().dump(result);
    DebugEGLImageTracker::getInstance()->dump(result);

    if (const auto display = getDefaultDisplayDeviceLocked()) {
        display->getCompositionDisplay()->getState().undefinedRegion.dump(result,
                                                                          "undefinedRegion");
        StringAppendF(&result, "  orientation=%s, isPoweredOn=%d\n",
                      toCString(display->getOrientation()), display->isPoweredOn());
    }
    StringAppendF(&result, "  transaction-flags         : %08x\n", mTransactionFlags.load());

    if (const auto display = getDefaultDisplayDeviceLocked()) {
        std::string peakFps, xDpi, yDpi;
        const auto activeMode = display->refreshRateSelector().getActiveMode();
        if (const auto activeModePtr = activeMode.modePtr.get()) {
            peakFps = to_string(activeMode.modePtr->getPeakFps());
            const auto dpi = activeModePtr->getDpi();
            xDpi = base::StringPrintf("%.2f", dpi.x);
            yDpi = base::StringPrintf("%.2f", dpi.y);
        } else {
            peakFps = "unknown";
            xDpi = "unknown";
            yDpi = "unknown";
        }
        StringAppendF(&result,
                      "  peak-refresh-rate         : %s\n"
                      "  x-dpi                     : %s\n"
                      "  y-dpi                     : %s\n",
                      peakFps.c_str(), xDpi.c_str(), yDpi.c_str());
    }

    StringAppendF(&result, "  transaction time: %f us\n", inTransactionDuration / 1000.0);

    result.append("\nTransaction tracing: ");
    if (mTransactionTracing) {
        result.append("enabled\n");
        mTransactionTracing->dump(result);
    } else {
        result.append("disabled\n");
    }
    result.push_back('\n');

    {
        DumpArgs plannerArgs;
        plannerArgs.add(); // first argument is ignored
        plannerArgs.add(String16("--layers"));
        dumpPlannerInfo(plannerArgs, result);
    }

    /*
     * Dump HWComposer state
     */
    colorizer.bold(result);
    result.append("h/w composer state:\n");
    colorizer.reset(result);
    const bool hwcDisabled = mDebugDisableHWC || mDebugFlashDelay;
    StringAppendF(&result, "  h/w composer %s\n", hwcDisabled ? "disabled" : "enabled");
    dumpHwc(result);

    /*
     * Dump gralloc state
     */
    const GraphicBufferAllocator& alloc(GraphicBufferAllocator::get());
    alloc.dump(result);

    /*
     * Dump flag/property manager state
     */
    FlagManager::getInstance().dump(result);

    result.append(mTimeStats->miniDump());
    result.append("\n");
}

mat4 SurfaceFlinger::calculateColorMatrix(float saturation) {
    if (saturation == 1) {
        return mat4();
    }

    float3 luminance{0.213f, 0.715f, 0.072f};
    luminance *= 1.0f - saturation;
    mat4 saturationMatrix = mat4(vec4{luminance.r + saturation, luminance.r, luminance.r, 0.0f},
                                 vec4{luminance.g, luminance.g + saturation, luminance.g, 0.0f},
                                 vec4{luminance.b, luminance.b, luminance.b + saturation, 0.0f},
                                 vec4{0.0f, 0.0f, 0.0f, 1.0f});
    return saturationMatrix;
}

void SurfaceFlinger::updateColorMatrixLocked() {
    mat4 colorMatrix =
            mClientColorMatrix * calculateColorMatrix(mGlobalSaturationFactor) * mDaltonizer();

    if (mCurrentState.colorMatrix != colorMatrix) {
        mCurrentState.colorMatrix = colorMatrix;
        mCurrentState.colorMatrixChanged = true;
        setTransactionFlags(eTransactionNeeded);
    }
}

status_t SurfaceFlinger::CheckTransactCodeCredentials(uint32_t code) {
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wswitch-enum"
    switch (static_cast<ISurfaceComposerTag>(code)) {
        // These methods should at minimum make sure that the client requested
        // access to SF.
        case GET_HDR_CAPABILITIES:
        case GET_AUTO_LOW_LATENCY_MODE_SUPPORT:
        case GET_GAME_CONTENT_TYPE_SUPPORT:
        case ACQUIRE_FRAME_RATE_FLEXIBILITY_TOKEN: {
            // OVERRIDE_HDR_TYPES is used by CTS tests, which acquire the necessary
            // permission dynamically. Don't use the permission cache for this check.
            bool usePermissionCache = code != OVERRIDE_HDR_TYPES;
            if (!callingThreadHasUnscopedSurfaceFlingerAccess(usePermissionCache)) {
                IPCThreadState* ipc = IPCThreadState::self();
                ALOGE("Permission Denial: can't access SurfaceFlinger pid=%d, uid=%d",
                      ipc->getCallingPid(), ipc->getCallingUid());
                return PERMISSION_DENIED;
            }
            return OK;
        }
        // The following calls are currently used by clients that do not
        // request necessary permissions. However, they do not expose any secret
        // information, so it is OK to pass them.
        case GET_ACTIVE_COLOR_MODE:
        case GET_ACTIVE_DISPLAY_MODE:
        case GET_DISPLAY_COLOR_MODES:
        case GET_DISPLAY_MODES:
        case GET_SCHEDULING_POLICY:
        // Calling setTransactionState is safe, because you need to have been
        // granted a reference to Client* and Handle* to do anything with it.
        case SET_TRANSACTION_STATE: {
            // This is not sensitive information, so should not require permission control.
            return OK;
        }
        case BOOT_FINISHED:
        // Used by apps to hook Choreographer to SurfaceFlinger.
        case CREATE_DISPLAY_EVENT_CONNECTION:
        case CREATE_CONNECTION:
        case CREATE_VIRTUAL_DISPLAY:
        case DESTROY_VIRTUAL_DISPLAY:
        case GET_PRIMARY_PHYSICAL_DISPLAY_ID:
        case GET_PHYSICAL_DISPLAY_IDS:
        case GET_PHYSICAL_DISPLAY_TOKEN:
        case AUTHENTICATE_SURFACE:
        case SET_POWER_MODE:
        case GET_SUPPORTED_FRAME_TIMESTAMPS:
        case GET_DISPLAY_STATE:
        case GET_DISPLAY_STATS:
        case GET_STATIC_DISPLAY_INFO:
        case GET_DYNAMIC_DISPLAY_INFO:
        case GET_DISPLAY_NATIVE_PRIMARIES:
        case SET_ACTIVE_COLOR_MODE:
        case SET_BOOT_DISPLAY_MODE:
        case CLEAR_BOOT_DISPLAY_MODE:
        case GET_BOOT_DISPLAY_MODE_SUPPORT:
        case SET_AUTO_LOW_LATENCY_MODE:
        case SET_GAME_CONTENT_TYPE:
        case CAPTURE_LAYERS:
        case CAPTURE_DISPLAY:
        case CAPTURE_DISPLAY_BY_ID:
        case CLEAR_ANIMATION_FRAME_STATS:
        case GET_ANIMATION_FRAME_STATS:
        case OVERRIDE_HDR_TYPES:
        case ON_PULL_ATOM:
        case ENABLE_VSYNC_INJECTIONS:
        case INJECT_VSYNC:
        case GET_LAYER_DEBUG_INFO:
        case GET_COLOR_MANAGEMENT:
        case GET_COMPOSITION_PREFERENCE:
        case GET_DISPLAYED_CONTENT_SAMPLING_ATTRIBUTES:
        case SET_DISPLAY_CONTENT_SAMPLING_ENABLED:
        case GET_DISPLAYED_CONTENT_SAMPLE:
        case GET_PROTECTED_CONTENT_SUPPORT:
        case IS_WIDE_COLOR_DISPLAY:
        case ADD_REGION_SAMPLING_LISTENER:
        case REMOVE_REGION_SAMPLING_LISTENER:
        case ADD_FPS_LISTENER:
        case REMOVE_FPS_LISTENER:
        case ADD_TUNNEL_MODE_ENABLED_LISTENER:
        case REMOVE_TUNNEL_MODE_ENABLED_LISTENER:
        case ADD_WINDOW_INFOS_LISTENER:
        case REMOVE_WINDOW_INFOS_LISTENER:
        case SET_DESIRED_DISPLAY_MODE_SPECS:
        case GET_DESIRED_DISPLAY_MODE_SPECS:
        case GET_DISPLAY_BRIGHTNESS_SUPPORT:
        case SET_DISPLAY_BRIGHTNESS:
        case ADD_HDR_LAYER_INFO_LISTENER:
        case REMOVE_HDR_LAYER_INFO_LISTENER:
        case NOTIFY_POWER_BOOST:
        case SET_GLOBAL_SHADOW_SETTINGS:
        case GET_DISPLAY_DECORATION_SUPPORT:
        case SET_FRAME_RATE:
        case SET_OVERRIDE_FRAME_RATE:
        case SET_FRAME_TIMELINE_INFO:
        case ADD_TRANSACTION_TRACE_LISTENER:
        case GET_GPU_CONTEXT_PRIORITY:
        case GET_MAX_ACQUIRED_BUFFER_COUNT:
            LOG_FATAL("Deprecated opcode: %d, migrated to AIDL", code);
            return PERMISSION_DENIED;
    }

    // These codes are used for the IBinder protocol to either interrogate the recipient
    // side of the transaction for its canonical interface descriptor or to dump its state.
    // We let them pass by default.
    if (code == IBinder::INTERFACE_TRANSACTION || code == IBinder::DUMP_TRANSACTION ||
        code == IBinder::PING_TRANSACTION || code == IBinder::SHELL_COMMAND_TRANSACTION ||
        code == IBinder::SYSPROPS_TRANSACTION) {
        return OK;
    }
    // Numbers from 1000 to 1047 are currently used for backdoors. The code
    // in onTransact verifies that the user is root, and has access to use SF.
    if (code >= 1000 && code <= 1047) {
        ALOGV("Accessing SurfaceFlinger through backdoor code: %u", code);
        return OK;
    }
    ALOGE("Permission Denial: SurfaceFlinger did not recognize request code: %u", code);
    return PERMISSION_DENIED;
#pragma clang diagnostic pop
}

status_t SurfaceFlinger::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                    uint32_t flags) {
    if (const status_t error = CheckTransactCodeCredentials(code); error != OK) {
        return error;
    }

    status_t err = BnSurfaceComposer::onTransact(code, data, reply, flags);
    if (err == UNKNOWN_TRANSACTION || err == PERMISSION_DENIED) {
        CHECK_INTERFACE(ISurfaceComposer, data, reply);
        IPCThreadState* ipc = IPCThreadState::self();
        const int uid = ipc->getCallingUid();
        if (CC_UNLIKELY(uid != AID_SYSTEM &&
                        !PermissionCache::checkCallingPermission(sHardwareTest))) {
            const int pid = ipc->getCallingPid();
            ALOGE("Permission Denial: "
                  "can't access SurfaceFlinger pid=%d, uid=%d",
                  pid, uid);
            return PERMISSION_DENIED;
        }
        int n;
        switch (code) {
            case 1000: // Unused.
            case 1001:
                return NAME_NOT_FOUND;
            case 1002: // Toggle flashing on surface damage.
                sfdo_setDebugFlash(data.readInt32());
                return NO_ERROR;
            case 1004: // Force composite ahead of next VSYNC.
            case 1006:
                sfdo_scheduleComposite();
                return NO_ERROR;
            case 1005: { // Force commit ahead of next VSYNC.
                sfdo_scheduleCommit();
                return NO_ERROR;
            }
            case 1007: // Unused.
                return NAME_NOT_FOUND;
            case 1008: // Toggle forced GPU composition.
                sfdo_forceClientComposition(data.readInt32() != 0);
                return NO_ERROR;
            case 1009: // Toggle use of transform hint.
                mDebugDisableTransformHint = data.readInt32() != 0;
                scheduleRepaint();
                return NO_ERROR;
            case 1010: // Interrogate.
                reply->writeInt32(0);
                reply->writeInt32(0);
                reply->writeInt32(mDebugFlashDelay);
                reply->writeInt32(0);
                reply->writeInt32(mDebugDisableHWC);
                return NO_ERROR;
            case 1013: // Unused.
                return NAME_NOT_FOUND;
            case 1014: {
                Mutex::Autolock _l(mStateLock);
                // daltonize
                n = data.readInt32();
                mDaltonizer.setLevel(data.readInt32());
                switch (n % 10) {
                    case 1:
                        mDaltonizer.setType(ColorBlindnessType::Protanomaly);
                        break;
                    case 2:
                        mDaltonizer.setType(ColorBlindnessType::Deuteranomaly);
                        break;
                    case 3:
                        mDaltonizer.setType(ColorBlindnessType::Tritanomaly);
                        break;
                    default:
                        mDaltonizer.setType(ColorBlindnessType::None);
                        break;
                }
                if (n >= 10) {
                    mDaltonizer.setMode(ColorBlindnessMode::Correction);
                } else {
                    mDaltonizer.setMode(ColorBlindnessMode::Simulation);
                }

                updateColorMatrixLocked();
                return NO_ERROR;
            }
            case 1015: {
                Mutex::Autolock _l(mStateLock);
                // apply a color matrix
                n = data.readInt32();
                if (n) {
                    // color matrix is sent as a column-major mat4 matrix
                    for (size_t i = 0; i < 4; i++) {
                        for (size_t j = 0; j < 4; j++) {
                            mClientColorMatrix[i][j] = data.readFloat();
                        }
                    }
                } else {
                    mClientColorMatrix = mat4();
                }

                // Check that supplied matrix's last row is {0,0,0,1} so we can avoid
                // the division by w in the fragment shader
                float4 lastRow(transpose(mClientColorMatrix)[3]);
                if (any(greaterThan(abs(lastRow - float4{0, 0, 0, 1}), float4{1e-4f}))) {
                    ALOGE("The color transform's last row must be (0, 0, 0, 1)");
                }

                updateColorMatrixLocked();
                return NO_ERROR;
            }
            case 1016: { // Unused.
                return NAME_NOT_FOUND;
            }
            case 1017: {
                n = data.readInt32();
                mForceFullDamage = n != 0;
                return NO_ERROR;
            }
            case 1018: { // Set the render deadline as a duration until VSYNC.
                n = data.readInt32();
                mScheduler->setDuration(scheduler::Cycle::Render, std::chrono::nanoseconds(n), 0ns);
                return NO_ERROR;
            }
            case 1019: { // Set the deadline of the last composite as a duration until VSYNC.
                n = data.readInt32();
                mScheduler->setDuration(scheduler::Cycle::LastComposite,
                                        std::chrono::nanoseconds(n), 0ns);
                return NO_ERROR;
            }
            case 1020: { // Unused
                return NAME_NOT_FOUND;
            }
            case 1021: { // Disable HWC virtual displays
                const bool enable = data.readInt32() != 0;
                static_cast<void>(
                        mScheduler->schedule([this, enable] { enableHalVirtualDisplays(enable); }));
                return NO_ERROR;
            }
            case 1022: { // Set saturation boost
                Mutex::Autolock _l(mStateLock);
                mGlobalSaturationFactor = std::max(0.0f, std::min(data.readFloat(), 2.0f));

                updateColorMatrixLocked();
                return NO_ERROR;
            }
            case 1023: { // Set color mode.
                mDisplayColorSetting = static_cast<DisplayColorSetting>(data.readInt32());

                if (int32_t colorMode; data.readInt32(&colorMode) == NO_ERROR) {
                    mForceColorMode = static_cast<ui::ColorMode>(colorMode);
                }
                scheduleRepaint();
                return NO_ERROR;
            }
            // Deprecate, use 1030 to check whether the device is color managed.
            case 1024: {
                return NAME_NOT_FOUND;
            }
            // Deprecated, use perfetto to start/stop the layer tracing
            case 1025: {
                return NAME_NOT_FOUND;
            }
            // Deprecated, execute "adb shell perfetto --query" to see the ongoing tracing sessions
            case 1026: {
                return NAME_NOT_FOUND;
            }
            // Is a DisplayColorSetting supported?
            case 1027: {
                const auto display = getDefaultDisplayDevice();
                if (!display) {
                    return NAME_NOT_FOUND;
                }

                DisplayColorSetting setting = static_cast<DisplayColorSetting>(data.readInt32());
                switch (setting) {
                    case DisplayColorSetting::kManaged:
                    case DisplayColorSetting::kUnmanaged:
                        reply->writeBool(true);
                        break;
                    case DisplayColorSetting::kEnhanced:
                        reply->writeBool(display->hasRenderIntent(RenderIntent::ENHANCE));
                        break;
                    default: // vendor display color setting
                        reply->writeBool(
                                display->hasRenderIntent(static_cast<RenderIntent>(setting)));
                        break;
                }
                return NO_ERROR;
            }
            case 1028: { // Unused.
                return NAME_NOT_FOUND;
            }
            // Deprecated, use perfetto to set the active layer tracing buffer size
            case 1029: {
                return NAME_NOT_FOUND;
            }
            // Is device color managed?
            case 1030: {
                // ColorDisplayManager stil calls this
                reply->writeBool(true);
                return NO_ERROR;
            }
            // Override default composition data space
            // adb shell service call SurfaceFlinger 1031 i32 1 DATASPACE_NUMBER DATASPACE_NUMBER \
            // && adb shell stop zygote && adb shell start zygote
            // to restore: adb shell service call SurfaceFlinger 1031 i32 0 && \
            // adb shell stop zygote && adb shell start zygote
            case 1031: {
                Mutex::Autolock _l(mStateLock);
                n = data.readInt32();
                if (n) {
                    n = data.readInt32();
                    if (n) {
                        Dataspace dataspace = static_cast<Dataspace>(n);
                        if (!validateCompositionDataspace(dataspace)) {
                            return BAD_VALUE;
                        }
                        mDefaultCompositionDataspace = dataspace;
                    }
                    n = data.readInt32();
                    if (n) {
                        Dataspace dataspace = static_cast<Dataspace>(n);
                        if (!validateCompositionDataspace(dataspace)) {
                            return BAD_VALUE;
                        }
                        mWideColorGamutCompositionDataspace = dataspace;
                    }
                } else {
                    // restore composition data space.
                    mDefaultCompositionDataspace = defaultCompositionDataspace;
                    mWideColorGamutCompositionDataspace = wideColorGamutCompositionDataspace;
                }
                return NO_ERROR;
            }
            // Deprecated, use perfetto to set layer trace flags
            case 1033: {
                return NAME_NOT_FOUND;
            }
            case 1034: {
                n = data.readInt32();
                if (n == 0 || n == 1) {
                    sfdo_enableRefreshRateOverlay(static_cast<bool>(n));
                } else {
                    Mutex::Autolock lock(mStateLock);
                    reply->writeBool(isRefreshRateOverlayEnabled());
                }
                return NO_ERROR;
            }
            case 1035: {
                // Parameters:
                // - (required) i32 mode id.
                // - (optional) i64 display id. Using default display if not provided.
                // - (optional) f min render rate. Using mode's fps is not provided.
                // - (optional) f max render rate. Using mode's fps is not provided.

                const int modeId = data.readInt32();

                const auto display = [&]() -> sp<IBinder> {
                    uint64_t value;
                    if (data.readUint64(&value) != NO_ERROR) {
                        return getDefaultDisplayDevice()->getDisplayToken().promote();
                    }

                    if (const auto token =
                                getPhysicalDisplayToken(PhysicalDisplayId::fromValue(value))) {
                        return token;
                    }

                    ALOGE("Invalid physical display ID");
                    return nullptr;
                }();

                const auto getFps = [&] {
                    float value;
                    if (data.readFloat(&value) == NO_ERROR) {
                        return Fps::fromValue(value);
                    }

                    return Fps();
                };

                const auto minFps = getFps();
                const auto maxFps = getFps();

                mDebugDisplayModeSetByBackdoor = false;
                const status_t result =
                        setActiveModeFromBackdoor(display, DisplayModeId{modeId}, minFps, maxFps);
                mDebugDisplayModeSetByBackdoor = result == NO_ERROR;
                return result;
            }
            // Turn on/off frame rate flexibility mode. When turned on it overrides the display
            // manager frame rate policy a new policy which allows switching between all refresh
            // rates.
            case 1036: {
                if (data.readInt32() > 0) { // turn on
                    return mScheduler
                            ->schedule([this]() FTL_FAKE_GUARD(kMainThreadContext) {
                                const auto display =
                                        FTL_FAKE_GUARD(mStateLock, getDefaultDisplayDeviceLocked());

                                // This is a little racy, but not in a way that hurts anything. As
                                // we grab the defaultMode from the display manager policy, we could
                                // be setting a new display manager policy, leaving us using a stale
                                // defaultMode. The defaultMode doesn't matter for the override
                                // policy though, since we set allowGroupSwitching to true, so it's
                                // not a problem.
                                scheduler::RefreshRateSelector::OverridePolicy overridePolicy;
                                overridePolicy.defaultMode = display->refreshRateSelector()
                                                                     .getDisplayManagerPolicy()
                                                                     .defaultMode;
                                overridePolicy.allowGroupSwitching = true;
                                return setDesiredDisplayModeSpecsInternal(display, overridePolicy);
                            })
                            .get();
                } else { // turn off
                    return mScheduler
                            ->schedule([this]() FTL_FAKE_GUARD(kMainThreadContext) {
                                const auto display =
                                        FTL_FAKE_GUARD(mStateLock, getDefaultDisplayDeviceLocked());
                                return setDesiredDisplayModeSpecsInternal(
                                        display,
                                        scheduler::RefreshRateSelector::NoOverridePolicy{});
                            })
                            .get();
                }
            }
            // Inject a hotplug connected event for the primary display. This will deallocate and
            // reallocate the display state including framebuffers.
            case 1037: {
                const hal::HWDisplayId hwcId =
                        (Mutex::Autolock(mStateLock), getHwComposer().getPrimaryHwcDisplayId());

                onComposerHalHotplugEvent(hwcId, DisplayHotplugEvent::CONNECTED);
                return NO_ERROR;
            }
            // Modify the max number of display frames stored within FrameTimeline
            case 1038: {
                n = data.readInt32();
                if (n < 0 || n > MAX_ALLOWED_DISPLAY_FRAMES) {
                    ALOGW("Invalid max size. Maximum allowed is %d", MAX_ALLOWED_DISPLAY_FRAMES);
                    return BAD_VALUE;
                }
                if (n == 0) {
                    // restore to default
                    mFrameTimeline->reset();
                    return NO_ERROR;
                }
                mFrameTimeline->setMaxDisplayFrames(n);
                return NO_ERROR;
            }
            case 1039: {
                const auto uid = static_cast<uid_t>(data.readInt32());
                const auto refreshRate = data.readFloat();
                mScheduler->setPreferredRefreshRateForUid(FrameRateOverride{uid, refreshRate});
                return NO_ERROR;
            }
            // Toggle caching feature
            // First argument is an int32 - nonzero enables caching and zero disables caching
            // Second argument is an optional uint64 - if present, then limits enabling/disabling
            // caching to a particular physical display
            case 1040: {
                auto future = mScheduler->schedule([&] {
                    n = data.readInt32();
                    PhysicalDisplayId inputId;
                    if (uint64_t inputDisplayId; data.readUint64(&inputDisplayId) == NO_ERROR) {
                        inputId = PhysicalDisplayId::fromValue(inputDisplayId);
                        if (!getPhysicalDisplayToken(inputId)) {
                            ALOGE("No display with id: %" PRIu64, inputDisplayId);
                            return NAME_NOT_FOUND;
                        }
                    }
                    {
                        Mutex::Autolock lock(mStateLock);
                        mLayerCachingEnabled = n != 0;
                        for (const auto& [_, display] : mDisplays) {
                            if (inputId == display->getPhysicalId()) {
                                display->enableLayerCaching(mLayerCachingEnabled);
                            }
                        }
                    }
                    return OK;
                });

                if (const status_t error = future.get(); error != OK) {
                    return error;
                }
                scheduleRepaint();
                return NO_ERROR;
            }
            case 1041: { // Transaction tracing
                if (mTransactionTracing) {
                    int arg = data.readInt32();
                    if (arg == -1) {
                        mScheduler->schedule([&]() { mTransactionTracing.reset(); }).get();
                    } else if (arg > 0) {
                        // Transaction tracing is always running but allow the user to temporarily
                        // increase the buffer when actively debugging.
                        mTransactionTracing->setBufferSize(
                                TransactionTracing::LEGACY_ACTIVE_TRACING_BUFFER_SIZE);
                    } else {
                        TransactionTraceWriter::getInstance().invoke("", /* overwrite= */ true);
                        mTransactionTracing->setBufferSize(
                                TransactionTracing::CONTINUOUS_TRACING_BUFFER_SIZE);
                    }
                }
                reply->writeInt32(NO_ERROR);
                return NO_ERROR;
            }
            case 1042: { // Write transaction trace to file
                if (mTransactionTracing) {
                    mTransactionTracing->writeToFile();
                }
                reply->writeInt32(NO_ERROR);
                return NO_ERROR;
            }
            // hdr sdr ratio overlay
            case 1043: {
                auto future = mScheduler->schedule(
                        [&]() FTL_FAKE_GUARD(mStateLock) FTL_FAKE_GUARD(kMainThreadContext) {
                            n = data.readInt32();
                            if (n == 0 || n == 1) {
                                mHdrSdrRatioOverlay = n != 0;
                                enableHdrSdrRatioOverlay(mHdrSdrRatioOverlay);
                            } else {
                                reply->writeBool(isHdrSdrRatioOverlayEnabled());
                            }
                        });
                future.wait();
                return NO_ERROR;
            }

            case 1044: { // Enable/Disable mirroring from one display to another
                /*
                 * Mirror one display onto another.
                 * Ensure the source and destination displays are on.
                 * Commands:
                 * 0: Mirror one display to another
                 * 1: Disable mirroring to a previously mirrored display
                 * 2: Disable mirroring on previously mirrored displays
                 *
                 * Ex:
                 * Get the display ids:
                 * adb shell dumpsys SurfaceFlinger --display-id
                 * Mirror first display to the second:
                 * adb shell service call SurfaceFlinger 1044 i64 0 i64 4619827677550801152 i64
                 * 4619827677550801153
                 * Stop mirroring:
                 * adb shell service call SurfaceFlinger 1044 i64 1
                 */

                int64_t arg0 = data.readInt64();

                switch (arg0) {
                    case 0: {
                        // Mirror arg1 to arg2
                        int64_t arg1 = data.readInt64();
                        int64_t arg2 = data.readInt64();
                        // Enable mirroring for one display
                        auto mirrorRoot = SurfaceComposerClient::getDefault()->mirrorDisplay(
                                DisplayId::fromValue(arg1));
                        const auto token2 =
                                getPhysicalDisplayToken(PhysicalDisplayId::fromValue(arg2));
                        ui::LayerStack layerStack;
                        {
                            Mutex::Autolock lock(mStateLock);
                            sp<DisplayDevice> display = getDisplayDeviceLocked(token2);
                            layerStack = display->getLayerStack();
                        }
                        SurfaceComposerClient::Transaction t;
                        t.setDisplayLayerStack(token2, layerStack);
                        t.setLayer(mirrorRoot, INT_MAX); // Top-most layer
                        t.setLayerStack(mirrorRoot, layerStack);
                        t.apply();

                        mMirrorMapForDebug.emplace_or_replace(arg2, mirrorRoot);
                        break;
                    }

                    case 1: {
                        // Disable mirroring for arg1
                        int64_t arg1 = data.readInt64();
                        mMirrorMapForDebug.erase(arg1);
                        break;
                    }

                    case 2: {
                        // Disable mirroring for all displays
                        mMirrorMapForDebug.clear();
                        break;
                    }

                    default:
                        return BAD_VALUE;
                }
                return NO_ERROR;
            }
            // Inject jank
            // First argument is a float that describes the fraction of frame duration to jank by.
            // Second argument is a delay in ms for triggering the jank. This is useful for working
            // with tools that steal the adb connection. This argument is optional.
            case 1045: {
                if (FlagManager::getInstance().vrr_config()) {
                    float jankAmount = data.readFloat();
                    int32_t jankDelayMs = 0;
                    if (data.readInt32(&jankDelayMs) != NO_ERROR) {
                        jankDelayMs = 0;
                    }

                    const auto jankDelayDuration = Duration(std::chrono::milliseconds(jankDelayMs));

                    const bool jankAmountValid = jankAmount > 0.0 && jankAmount < 100.0;

                    if (!jankAmountValid) {
                        ALOGD("Ignoring invalid jank amount: %f", jankAmount);
                        reply->writeInt32(BAD_VALUE);
                        return BAD_VALUE;
                    }

                    (void)mScheduler->scheduleDelayed(
                            [&, jankAmount]() FTL_FAKE_GUARD(kMainThreadContext) {
                                mScheduler->injectPacesetterDelay(jankAmount);
                                scheduleComposite(FrameHint::kActive);
                            },
                            jankDelayDuration.ns());
                    reply->writeInt32(NO_ERROR);
                    return NO_ERROR;
                }
                return err;
            }
            // Introduce jank to HWC
            case 1046: {
                int32_t jankDelayMs = 0;
                if (data.readInt32(&jankDelayMs) != NO_ERROR) {
                    return BAD_VALUE;
                }
                mScheduler->setDebugPresentDelay(TimePoint::fromNs(ms2ns(jankDelayMs)));
                return NO_ERROR;
            }
                // Update WorkDuration
                // parameters:
                // - (required) i64 minSfNs, used as the late.sf WorkDuration.
                // - (required) i64 maxSfNs, used as the early.sf and earlyGl.sf WorkDuration.
                // - (required) i64 appDurationNs, used as the late.app, early.app and earlyGl.app
                // WorkDuration.
                // Usage:
                // adb shell service call SurfaceFlinger 1047 i64 12333333 i64 16666666 i64 16666666
            case 1047: {
                if (!property_get_bool("debug.sf.use_phase_offsets_as_durations", false)) {
                    ALOGE("Not supported when work duration is not enabled");
                    return INVALID_OPERATION;
                }
                int64_t minSfNs = 0;
                int64_t maxSfNs = 0;
                int64_t appDurationNs = 0;
                if (data.readInt64(&minSfNs) != NO_ERROR || data.readInt64(&maxSfNs) != NO_ERROR ||
                    data.readInt64(&appDurationNs) != NO_ERROR) {
                    return BAD_VALUE;
                }
                mScheduler->reloadPhaseConfiguration(mDisplayModeController
                                                             .getActiveMode(mActiveDisplayId)
                                                             .fps,
                                                     Duration::fromNs(minSfNs),
                                                     Duration::fromNs(maxSfNs),
                                                     Duration::fromNs(appDurationNs));
                return NO_ERROR;
            }
        }
    }
    return err;
}

void SurfaceFlinger::kernelTimerChanged(bool expired) {
    static bool updateOverlay =
            property_get_bool("debug.sf.kernel_idle_timer_update_overlay", true);
    if (!updateOverlay) return;

    // Update the overlay on the main thread to avoid race conditions with
    // RefreshRateSelector::getActiveMode
    static_cast<void>(mScheduler->schedule([=, this]() FTL_FAKE_GUARD(kMainThreadContext) {
        const auto display = FTL_FAKE_GUARD(mStateLock, getDefaultDisplayDeviceLocked());
        if (!display) {
            ALOGW("%s: default display is null", __func__);
            return;
        }
        if (!display->isRefreshRateOverlayEnabled()) return;

        const auto state = mDisplayModeController.getKernelIdleTimerState(display->getPhysicalId());

        if (display->onKernelTimerChanged(state.desiredModeIdOpt, state.isEnabled && expired)) {
            mScheduler->scheduleFrame();
        }
    }));
}

void SurfaceFlinger::vrrDisplayIdle(bool idle) {
    // Update the overlay on the main thread to avoid race conditions with
    // RefreshRateSelector::getActiveMode
    static_cast<void>(mScheduler->schedule([=, this] {
        const auto display = FTL_FAKE_GUARD(mStateLock, getDefaultDisplayDeviceLocked());
        if (!display) {
            ALOGW("%s: default display is null", __func__);
            return;
        }
        if (!display->isRefreshRateOverlayEnabled()) return;

        display->onVrrIdle(idle);
        mScheduler->scheduleFrame();
    }));
}

auto SurfaceFlinger::getKernelIdleTimerProperties(PhysicalDisplayId displayId)
        -> std::pair<std::optional<KernelIdleTimerController>, std::chrono::milliseconds> {
    const bool isKernelIdleTimerHwcSupported = getHwComposer().getComposer()->isSupported(
            android::Hwc2::Composer::OptionalFeature::KernelIdleTimer);
    const auto timeout = getIdleTimerTimeout(displayId);
    if (isKernelIdleTimerHwcSupported) {
        if (getHwComposer().hasDisplayIdleTimerCapability(displayId)) {
            // In order to decide if we can use the HWC api for idle timer
            // we query DisplayCapability::DISPLAY_IDLE_TIMER directly on the composer
            // without relying on hasDisplayCapability.
            // hasDisplayCapability relies on DisplayCapabilities
            // which are updated after we set the PowerMode::ON.
            // DISPLAY_IDLE_TIMER is a display driver property
            // and is available before the PowerMode::ON
            return {KernelIdleTimerController::HwcApi, timeout};
        }
        return {std::nullopt, timeout};
    }
    if (getKernelIdleTimerSyspropConfig(displayId)) {
        return {KernelIdleTimerController::Sysprop, timeout};
    }

    return {std::nullopt, timeout};
}

// A simple RAII class to disconnect from an ANativeWindow* when it goes out of scope
class WindowDisconnector {
public:
    WindowDisconnector(ANativeWindow* window, int api) : mWindow(window), mApi(api) {}
    ~WindowDisconnector() { native_window_api_disconnect(mWindow, mApi); }

private:
    ANativeWindow* mWindow;
    const int mApi;
};

static bool hasCaptureBlackoutContentPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    return uid == AID_GRAPHICS || uid == AID_SYSTEM ||
            PermissionCache::checkPermission(sCaptureBlackoutContent, pid, uid);
}

static status_t validateScreenshotPermissions(const CaptureArgs& captureArgs) {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if (uid == AID_GRAPHICS || uid == AID_SYSTEM ||
        PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        return OK;
    }

    // If the caller doesn't have the correct permissions but is only attempting to screenshot
    // itself, we allow it to continue.
    if (captureArgs.uid == uid) {
        return OK;
    }

    ALOGE("Permission Denial: can't take screenshot pid=%d, uid=%d", pid, uid);
    return PERMISSION_DENIED;
}

void SurfaceFlinger::setSchedFifo(bool enabled, const char* whence) {
    static constexpr int kFifoPriority = 2;
    static constexpr int kOtherPriority = 0;

    struct sched_param param = {0};
    int sched_policy;
    if (enabled && !FlagManager::getInstance().disable_sched_fifo_sf()) {
        sched_policy = SCHED_FIFO;
        param.sched_priority = kFifoPriority;
    } else {
        sched_policy = SCHED_OTHER;
        param.sched_priority = kOtherPriority;
    }

    if (sched_setscheduler(0, sched_policy, &param) != 0) {
        const char* kPolicy[] = {"SCHED_OTHER", "SCHED_FIFO"};
        ALOGW("%s: Failed to set %s: %s", whence, kPolicy[sched_policy == SCHED_FIFO],
              strerror(errno));
    }
}

void SurfaceFlinger::setSchedAttr(bool enabled, const char* whence) {
    static const unsigned int kUclampMin =
            base::GetUintProperty<unsigned int>("ro.surface_flinger.uclamp.min"s, 0U);

    if (!kUclampMin) {
        // uclamp.min set to 0 (default), skip setting
        return;
    }

    sched_attr attr = {};
    attr.size = sizeof(attr);

    attr.sched_flags = (SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP);
    attr.sched_util_min = enabled ? kUclampMin : 0;
    attr.sched_util_max = 1024;

    if (syscall(__NR_sched_setattr, 0, &attr, 0)) {
        const char* kAction[] = {"disable", "enable"};
        ALOGW("%s: Failed to %s uclamp.min: %s", whence, kAction[enabled], strerror(errno));
    }
}

namespace {

ui::Dataspace pickBestDataspace(ui::Dataspace requestedDataspace, ui::ColorMode colorMode,
                                bool capturingHdrLayers, bool hintForSeamlessTransition) {
    if (requestedDataspace != ui::Dataspace::UNKNOWN) {
        return requestedDataspace;
    }

    const auto dataspaceForColorMode = ui::pickDataspaceFor(colorMode);

    // TODO: Enable once HDR screenshots are ready.
    if constexpr (/* DISABLES CODE */ (false)) {
        // For now since we only support 8-bit screenshots, just use HLG and
        // assume that 1.0 >= display max luminance. This isn't quite as future
        // proof as PQ is, but is good enough.
        // Consider using PQ once we support 16-bit screenshots and we're able
        // to consistently supply metadata to image encoders.
        return ui::Dataspace::BT2020_HLG;
    }

    return dataspaceForColorMode;
}

} // namespace

static void invokeScreenCaptureError(const status_t status,
                                     const sp<IScreenCaptureListener>& captureListener) {
    ScreenCaptureResults captureResults;
    captureResults.fenceResult = base::unexpected(status);
    captureListener->onScreenCaptureCompleted(captureResults);
}

void SurfaceFlinger::captureDisplay(const DisplayCaptureArgs& args,
                                    const sp<IScreenCaptureListener>& captureListener) {
    SFTRACE_CALL();

    const auto& captureArgs = args.captureArgs;
    status_t validate = validateScreenshotPermissions(captureArgs);
    if (validate != OK) {
        ALOGD("Permission denied to captureDisplay");
        invokeScreenCaptureError(validate, captureListener);
        return;
    }

    if (!args.displayToken) {
        ALOGD("Invalid display token to captureDisplay");
        invokeScreenCaptureError(BAD_VALUE, captureListener);
        return;
    }

    if (captureArgs.captureSecureLayers && !hasCaptureBlackoutContentPermission()) {
        ALOGD("Attempting to capture secure layers without CAPTURE_BLACKOUT_CONTENT");
        invokeScreenCaptureError(PERMISSION_DENIED, captureListener);
        return;
    }

    wp<const DisplayDevice> displayWeak;
    ftl::Optional<DisplayIdVariant> displayIdVariantOpt;
    ui::LayerStack layerStack;
    ui::Size reqSize(args.width, args.height);
    std::unordered_set<uint32_t> excludeLayerIds;
    Rect layerStackSpaceRect;
    bool displayIsSecure;

    {
        Mutex::Autolock lock(mStateLock);
        sp<DisplayDevice> display = getDisplayDeviceLocked(args.displayToken);
        if (!display) {
            ALOGD("Unable to find display device for captureDisplay");
            invokeScreenCaptureError(NAME_NOT_FOUND, captureListener);
            return;
        }
        displayWeak = display;
        displayIdVariantOpt = display->getDisplayIdVariant();
        layerStack = display->getLayerStack();
        displayIsSecure = display->isSecure();

        layerStackSpaceRect = display->getLayerStackSpaceRect();
        // set the requested width/height to the logical display layer stack rect size by default
        if (args.width == 0 || args.height == 0) {
            reqSize = layerStackSpaceRect.getSize();
        }

        for (const auto& handle : captureArgs.excludeHandles) {
            uint32_t excludeLayer = LayerHandle::getLayerId(handle);
            if (excludeLayer != UNASSIGNED_LAYER_ID) {
                excludeLayerIds.emplace(excludeLayer);
            } else {
                ALOGD("Invalid layer handle passed as excludeLayer to captureDisplay");
                invokeScreenCaptureError(NAME_NOT_FOUND, captureListener);
                return;
            }
        }
    }

    GetLayerSnapshotsFunction getLayerSnapshotsFn =
            getLayerSnapshotsForScreenshots(layerStack, captureArgs.uid,
                                            std::move(excludeLayerIds));

    ScreenshotArgs screenshotArgs;
    screenshotArgs.captureTypeVariant = displayWeak;
    screenshotArgs.displayIdVariant = displayIdVariantOpt;
    screenshotArgs.sourceCrop = gui::aidl_utils::fromARect(captureArgs.sourceCrop);
    if (screenshotArgs.sourceCrop.isEmpty()) {
        screenshotArgs.sourceCrop = layerStackSpaceRect;
    }
    screenshotArgs.reqSize = reqSize;
    screenshotArgs.dataspace = static_cast<ui::Dataspace>(captureArgs.dataspace);
    screenshotArgs.isSecure = captureArgs.captureSecureLayers && displayIsSecure;
    screenshotArgs.seamlessTransition = captureArgs.hintForSeamlessTransition;

    captureScreenCommon(screenshotArgs, getLayerSnapshotsFn, reqSize,
                        static_cast<ui::PixelFormat>(captureArgs.pixelFormat),
                        captureArgs.allowProtected, captureArgs.grayscale, captureListener);
}

void SurfaceFlinger::captureDisplay(DisplayId displayId, const CaptureArgs& args,
                                    const sp<IScreenCaptureListener>& captureListener) {
    ui::LayerStack layerStack;
    wp<const DisplayDevice> displayWeak;
    ftl::Optional<DisplayIdVariant> displayIdVariantOpt;
    ui::Size size;
    Rect layerStackSpaceRect;
    bool displayIsSecure;

    {
        Mutex::Autolock lock(mStateLock);

        const auto display = getDisplayDeviceLocked(displayId);
        if (!display) {
            ALOGD("Unable to find display device for captureDisplay");
            invokeScreenCaptureError(NAME_NOT_FOUND, captureListener);
            return;
        }

        displayWeak = display;
        displayIdVariantOpt = display->getDisplayIdVariant();
        layerStack = display->getLayerStack();
        layerStackSpaceRect = display->getLayerStackSpaceRect();
        size = display->getLayerStackSpaceRect().getSize();
        displayIsSecure = display->isSecure();
    }

    size.width *= args.frameScaleX;
    size.height *= args.frameScaleY;

    // We could query a real value for this but it'll be a long, long time until we support
    // displays that need upwards of 1GB per buffer so...
    constexpr auto kMaxTextureSize = 16384;
    if (size.width <= 0 || size.height <= 0 || size.width >= kMaxTextureSize ||
        size.height >= kMaxTextureSize) {
        ALOGD("captureDisplay resolved to invalid size %d x %d", size.width, size.height);
        invokeScreenCaptureError(BAD_VALUE, captureListener);
        return;
    }

    GetLayerSnapshotsFunction getLayerSnapshotsFn =
            getLayerSnapshotsForScreenshots(layerStack, CaptureArgs::UNSET_UID,
                                            /*snapshotFilterFn=*/nullptr);

    if (captureListener == nullptr) {
        ALOGE("capture screen must provide a capture listener callback");
        invokeScreenCaptureError(BAD_VALUE, captureListener);
        return;
    }

    constexpr bool kAllowProtected = false;
    constexpr bool kGrayscale = false;

    ScreenshotArgs screenshotArgs;
    screenshotArgs.captureTypeVariant = displayWeak;
    screenshotArgs.displayIdVariant = displayIdVariantOpt;
    screenshotArgs.sourceCrop = layerStackSpaceRect;
    screenshotArgs.reqSize = size;
    screenshotArgs.dataspace = static_cast<ui::Dataspace>(args.dataspace);
    screenshotArgs.isSecure = args.captureSecureLayers && displayIsSecure;
    screenshotArgs.seamlessTransition = args.hintForSeamlessTransition;

    captureScreenCommon(screenshotArgs, getLayerSnapshotsFn, size,
                        static_cast<ui::PixelFormat>(args.pixelFormat), kAllowProtected, kGrayscale,
                        captureListener);
}

ScreenCaptureResults SurfaceFlinger::captureLayersSync(const LayerCaptureArgs& args) {
    sp<SyncScreenCaptureListener> captureListener = sp<SyncScreenCaptureListener>::make();
    captureLayers(args, captureListener);
    return captureListener->waitForResults();
}

void SurfaceFlinger::captureLayers(const LayerCaptureArgs& args,
                                   const sp<IScreenCaptureListener>& captureListener) {
    SFTRACE_CALL();

    const auto& captureArgs = args.captureArgs;

    status_t validate = validateScreenshotPermissions(captureArgs);
    if (validate != OK) {
        ALOGD("Permission denied to captureLayers");
        invokeScreenCaptureError(validate, captureListener);
        return;
    }

    auto crop = gui::aidl_utils::fromARect(captureArgs.sourceCrop);

    ui::Size reqSize;
    sp<Layer> parent;
    std::unordered_set<uint32_t> excludeLayerIds;
    ui::Dataspace dataspace = static_cast<ui::Dataspace>(captureArgs.dataspace);

    if (captureArgs.captureSecureLayers && !hasCaptureBlackoutContentPermission()) {
        ALOGD("Attempting to capture secure layers without CAPTURE_BLACKOUT_CONTENT");
        invokeScreenCaptureError(PERMISSION_DENIED, captureListener);
        return;
    }

    {
        Mutex::Autolock lock(mStateLock);

        parent = LayerHandle::getLayer(args.layerHandle);
        if (parent == nullptr) {
            ALOGD("captureLayers called with an invalid or removed parent");
            invokeScreenCaptureError(NAME_NOT_FOUND, captureListener);
            return;
        }

        Rect parentSourceBounds = parent->getCroppedBufferSize(parent->getDrawingState());
        if (crop.width() <= 0) {
            crop.left = 0;
            crop.right = parentSourceBounds.getWidth();
        }

        if (crop.height() <= 0) {
            crop.top = 0;
            crop.bottom = parentSourceBounds.getHeight();
        }

        if (crop.isEmpty() || captureArgs.frameScaleX <= 0.0f || captureArgs.frameScaleY <= 0.0f) {
            // Error out if the layer has no source bounds (i.e. they are boundless) and a source
            // crop was not specified, or an invalid frame scale was provided.
            ALOGD("Boundless layer, unspecified crop, or invalid frame scale to captureLayers");
            invokeScreenCaptureError(BAD_VALUE, captureListener);
            return;
        }
        reqSize = ui::Size(crop.width() * captureArgs.frameScaleX,
                           crop.height() * captureArgs.frameScaleY);

        for (const auto& handle : captureArgs.excludeHandles) {
            uint32_t excludeLayer = LayerHandle::getLayerId(handle);
            if (excludeLayer != UNASSIGNED_LAYER_ID) {
                excludeLayerIds.emplace(excludeLayer);
            } else {
                ALOGD("Invalid layer handle passed as excludeLayer to captureLayers");
                invokeScreenCaptureError(NAME_NOT_FOUND, captureListener);
                return;
            }
        }
    } // mStateLock

    // really small crop or frameScale
    if (reqSize.width <= 0 || reqSize.height <= 0) {
        ALOGD("Failed to captureLayers: crop or scale too small");
        invokeScreenCaptureError(BAD_VALUE, captureListener);
        return;
    }

    std::optional<FloatRect> parentCrop = std::nullopt;
    if (args.childrenOnly) {
        parentCrop = crop.isEmpty() ? FloatRect(0, 0, reqSize.width, reqSize.height)
                                    : crop.toFloatRect();
    }

    GetLayerSnapshotsFunction getLayerSnapshotsFn =
            getLayerSnapshotsForScreenshots(parent->sequence, captureArgs.uid,
                                            std::move(excludeLayerIds), args.childrenOnly,
                                            parentCrop);

    if (captureListener == nullptr) {
        ALOGD("capture screen must provide a capture listener callback");
        invokeScreenCaptureError(BAD_VALUE, captureListener);
        return;
    }

    ScreenshotArgs screenshotArgs;
    screenshotArgs.captureTypeVariant = parent->getSequence();
    screenshotArgs.childrenOnly = args.childrenOnly;
    screenshotArgs.sourceCrop = crop;
    screenshotArgs.reqSize = reqSize;
    screenshotArgs.dataspace = static_cast<ui::Dataspace>(captureArgs.dataspace);
    screenshotArgs.isSecure = captureArgs.captureSecureLayers;
    screenshotArgs.seamlessTransition = captureArgs.hintForSeamlessTransition;

    captureScreenCommon(screenshotArgs, getLayerSnapshotsFn, reqSize,
                        static_cast<ui::PixelFormat>(captureArgs.pixelFormat),
                        captureArgs.allowProtected, captureArgs.grayscale, captureListener);
}

// Creates a Future release fence for a layer and keeps track of it in a list to
// release the buffer when the Future is complete. Calls from composittion
// involve needing to refresh the composition start time for stats.
void SurfaceFlinger::attachReleaseFenceFutureToLayer(Layer* layer, LayerFE* layerFE,
                                                     ui::LayerStack layerStack) {
    ftl::Future<FenceResult> futureFence = layerFE->createReleaseFenceFuture();
    layer->prepareReleaseCallbacks(std::move(futureFence), layerStack);
}

// Loop over all visible layers to see whether there's any protected layer. A protected layer is
// typically a layer with DRM contents, or have the GRALLOC_USAGE_PROTECTED set on the buffer.
// A protected layer has no implication on whether it's secure, which is explicitly set by
// application to avoid being screenshot or drawn via unsecure display.
bool SurfaceFlinger::layersHasProtectedLayer(
        const std::vector<std::pair<Layer*, sp<LayerFE>>>& layers) const {
    bool protectedLayerFound = false;
    for (auto& [_, layerFE] : layers) {
        protectedLayerFound |=
                (layerFE->mSnapshot->isVisible && layerFE->mSnapshot->hasProtectedContent);
        if (protectedLayerFound) {
            break;
        }
    }
    return protectedLayerFound;
}

// Getting layer snapshots and accessing display state should take place on
// main thread. Accessing display requires mStateLock, and contention for
// this lock is reduced when grabbed from the main thread, thus also reducing
// risk of deadlocks. Returns false if no display is found.
bool SurfaceFlinger::getSnapshotsFromMainThread(
        ScreenshotArgs& args, GetLayerSnapshotsFunction getLayerSnapshotsFn,
        std::vector<std::pair<Layer*, sp<LayerFE>>>& layers) {
    return mScheduler
            ->schedule([=, this, &args, &layers]() REQUIRES(kMainThreadContext) {
                SFTRACE_NAME_FOR_TRACK(WorkloadTracer::TRACK_NAME, "Screenshot");
                mPowerAdvisor->setScreenshotWorkload();
                SFTRACE_NAME("getSnapshotsFromMainThread");
                layers = getLayerSnapshotsFn();
                // Non-threaded RenderEngine eventually returns to the main thread a 2nd time
                // to complete the screenshot. Release fences should only be added during the 2nd
                // hop to main thread in order to avoid potential deadlocks from waiting for the
                // the future fence to fire.
                if (mRenderEngine->isThreaded()) {
                    for (auto& [layer, layerFE] : layers) {
                        attachReleaseFenceFutureToLayer(layer, layerFE.get(),
                                                        ui::INVALID_LAYER_STACK);
                    }
                }
                return getDisplayStateOnMainThread(args);
            })
            .get();
}

void SurfaceFlinger::captureScreenCommon(ScreenshotArgs& args,
                                         GetLayerSnapshotsFunction getLayerSnapshotsFn,
                                         ui::Size bufferSize, ui::PixelFormat reqPixelFormat,
                                         bool allowProtected, bool grayscale,
                                         const sp<IScreenCaptureListener>& captureListener) {
    SFTRACE_CALL();

    if (exceedsMaxRenderTargetSize(bufferSize.getWidth(), bufferSize.getHeight())) {
        ALOGE("Attempted to capture screen with size (%" PRId32 ", %" PRId32
              ") that exceeds render target size limit.",
              bufferSize.getWidth(), bufferSize.getHeight());
        invokeScreenCaptureError(BAD_VALUE, captureListener);
        return;
    }

    std::vector<std::pair<Layer*, sp<LayerFE>>> layers;
    bool hasDisplayState = getSnapshotsFromMainThread(args, getLayerSnapshotsFn, layers);
    if (!hasDisplayState) {
        ALOGD("Display state not found");
        invokeScreenCaptureError(NO_MEMORY, captureListener);
    }

    const bool hasHdrLayer = std::any_of(layers.cbegin(), layers.cend(), [this](const auto& layer) {
        return isHdrLayer(*(layer.second->mSnapshot.get()));
    });

    const bool supportsProtected = getRenderEngine().supportsProtectedContent();
    bool hasProtectedLayer = false;
    if (allowProtected && supportsProtected) {
        hasProtectedLayer = layersHasProtectedLayer(layers);
    }
    const bool isProtected = hasProtectedLayer && allowProtected && supportsProtected;
    const uint32_t usage = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER |
            GRALLOC_USAGE_HW_TEXTURE |
            (isProtected ? GRALLOC_USAGE_PROTECTED
                         : GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
    sp<GraphicBuffer> buffer =
            getFactory().createGraphicBuffer(bufferSize.getWidth(), bufferSize.getHeight(),
                                             static_cast<android_pixel_format>(reqPixelFormat),
                                             1 /* layerCount */, usage, "screenshot");

    const status_t bufferStatus = buffer->initCheck();
    if (bufferStatus != OK) {
        // Animations may end up being really janky, but don't crash here.
        // Otherwise an irreponsible process may cause an SF crash by allocating
        // too much.
        ALOGE("%s: Buffer failed to allocate: %d", __func__, bufferStatus);
        invokeScreenCaptureError(bufferStatus, captureListener);
        return;
    }
    const std::shared_ptr<renderengine::ExternalTexture> texture = std::make_shared<
            renderengine::impl::ExternalTexture>(buffer, getRenderEngine(),
                                                 renderengine::impl::ExternalTexture::Usage::
                                                         WRITEABLE);

    std::shared_ptr<renderengine::impl::ExternalTexture> hdrTexture;
    std::shared_ptr<renderengine::impl::ExternalTexture> gainmapTexture;

    if (hasHdrLayer && !args.seamlessTransition &&
        FlagManager::getInstance().true_hdr_screenshots()) {
        const auto hdrBuffer =
                getFactory().createGraphicBuffer(buffer->getWidth(), buffer->getHeight(),
                                                 HAL_PIXEL_FORMAT_RGBA_FP16, 1 /* layerCount */,
                                                 buffer->getUsage(), "screenshot-hdr");
        const auto gainmapBuffer =
                getFactory().createGraphicBuffer(buffer->getWidth(), buffer->getHeight(),
                                                 buffer->getPixelFormat(), 1 /* layerCount */,
                                                 buffer->getUsage(), "screenshot-gainmap");

        const status_t hdrBufferStatus = hdrBuffer->initCheck();
        const status_t gainmapBufferStatus = gainmapBuffer->initCheck();

        if (hdrBufferStatus != OK || gainmapBufferStatus != -OK) {
            if (hdrBufferStatus != OK) {
                ALOGW("%s: Buffer failed to allocate for hdr: %d. Screenshoting SDR instead.",
                      __func__, hdrBufferStatus);
            } else {
                ALOGW("%s: Buffer failed to allocate for gainmap: %d. Screenshoting SDR instead.",
                      __func__, gainmapBufferStatus);
            }
        } else {
            hdrTexture = std::make_shared<
                    renderengine::impl::ExternalTexture>(hdrBuffer, getRenderEngine(),
                                                         renderengine::impl::ExternalTexture::
                                                                 Usage::WRITEABLE);
            gainmapTexture = std::make_shared<
                    renderengine::impl::ExternalTexture>(gainmapBuffer, getRenderEngine(),
                                                         renderengine::impl::ExternalTexture::
                                                                 Usage::WRITEABLE);
        }
    }

    auto futureFence =
            captureScreenshot(args, texture, false /* regionSampling */, grayscale, isProtected,
                              captureListener, layers, hdrTexture, gainmapTexture);
    futureFence.get();
}

// Returns true if display is found and args was populated with display state
// data. Otherwise, returns false.
bool SurfaceFlinger::getDisplayStateOnMainThread(ScreenshotArgs& args) {
    sp<const DisplayDevice> display = nullptr;
    {
        Mutex::Autolock lock(mStateLock);
        // Screenshot initiated through captureLayers
        if (auto* layerSequence = std::get_if<int32_t>(&args.captureTypeVariant)) {
            // LayerSnapshotBuilder should only be accessed from the main thread.
            const frontend::LayerSnapshot* snapshot =
                    mLayerSnapshotBuilder.getSnapshot(*layerSequence);
            if (!snapshot) {
                ALOGW("Couldn't find layer snapshot for %d", *layerSequence);
            } else {
                if (!args.childrenOnly) {
                    args.transform = snapshot->localTransform.inverse();
                }
                if (args.sourceCrop.isEmpty()) {
                    args.sourceCrop = snapshot->bufferSize;
                }
                display = findDisplay(
                        [layerStack = snapshot->outputFilter.layerStack](const auto& display) {
                            return display.getLayerStack() == layerStack;
                        });
            }

            // Screenshot initiated through captureDisplay
        } else if (auto* displayWeak =
                           std::get_if<wp<const DisplayDevice>>(&args.captureTypeVariant)) {
            display = displayWeak->promote();
        }

        if (display == nullptr) {
            display = getDefaultDisplayDeviceLocked();
        }

        if (display != nullptr) {
            const auto& state = display->getCompositionDisplay()->getState();
            args.displayBrightnessNits = state.displayBrightnessNits;
            args.sdrWhitePointNits = state.sdrWhitePointNits;
            args.renderIntent = state.renderIntent;
            args.colorMode = state.colorMode;
            return true;
        }
    }
    return false;
}

ftl::SharedFuture<FenceResult> SurfaceFlinger::captureScreenshot(
        ScreenshotArgs& args, const std::shared_ptr<renderengine::ExternalTexture>& buffer,
        bool regionSampling, bool grayscale, bool isProtected,
        const sp<IScreenCaptureListener>& captureListener,
        const std::vector<std::pair<Layer*, sp<LayerFE>>>& layers,
        const std::shared_ptr<renderengine::ExternalTexture>& hdrBuffer,
        const std::shared_ptr<renderengine::ExternalTexture>& gainmapBuffer) {
    SFTRACE_CALL();

    ScreenCaptureResults captureResults;
    ftl::SharedFuture<FenceResult> renderFuture;

    float hdrSdrRatio = args.displayBrightnessNits / args.sdrWhitePointNits;

    if (hdrBuffer && gainmapBuffer) {
        ftl::SharedFuture<FenceResult> hdrRenderFuture =
                renderScreenImpl(args, hdrBuffer, regionSampling, grayscale, isProtected,
                                 captureResults, layers);
        captureResults.buffer = buffer->getBuffer();
        captureResults.optionalGainMap = gainmapBuffer->getBuffer();

        renderFuture =
                ftl::Future(std::move(hdrRenderFuture))
                        .then([&, hdrSdrRatio, dataspace = captureResults.capturedDataspace, buffer,
                               hdrBuffer, gainmapBuffer](FenceResult fenceResult) -> FenceResult {
                            if (!fenceResult.ok()) {
                                return fenceResult;
                            }

                            return getRenderEngine()
                                    .tonemapAndDrawGainmap(hdrBuffer, fenceResult.value()->get(),
                                                           hdrSdrRatio,
                                                           static_cast<ui::Dataspace>(dataspace),
                                                           buffer, gainmapBuffer)
                                    .get();
                        })
                        .share();
    } else {
        renderFuture = renderScreenImpl(args, buffer, regionSampling, grayscale, isProtected,
                                        captureResults, layers);
    }

    if (captureListener) {
        // Defer blocking on renderFuture back to the Binder thread.
        return ftl::Future(std::move(renderFuture))
                .then([captureListener, captureResults = std::move(captureResults),
                       hdrSdrRatio](FenceResult fenceResult) mutable -> FenceResult {
                    captureResults.fenceResult = std::move(fenceResult);
                    captureResults.hdrSdrRatio = hdrSdrRatio;
                    captureListener->onScreenCaptureCompleted(captureResults);
                    return base::unexpected(NO_ERROR);
                })
                .share();
    }
    return renderFuture;
}

ftl::SharedFuture<FenceResult> SurfaceFlinger::renderScreenImpl(
        ScreenshotArgs& args, const std::shared_ptr<renderengine::ExternalTexture>& buffer,
        bool regionSampling, bool grayscale, bool isProtected, ScreenCaptureResults& captureResults,
        const std::vector<std::pair<Layer*, sp<LayerFE>>>& layers) {
    SFTRACE_CALL();

    for (auto& [_, layerFE] : layers) {
        frontend::LayerSnapshot* snapshot = layerFE->mSnapshot.get();
        captureResults.capturedSecureLayers |= (snapshot->isVisible && snapshot->isSecure);
        captureResults.capturedHdrLayers |= isHdrLayer(*snapshot);
        layerFE->mSnapshot->geomLayerTransform =
                args.transform * layerFE->mSnapshot->geomLayerTransform;
        layerFE->mSnapshot->geomInverseLayerTransform =
                layerFE->mSnapshot->geomLayerTransform.inverse();
    }

    const bool enableLocalTonemapping =
            FlagManager::getInstance().local_tonemap_screenshots() && !args.seamlessTransition;

    captureResults.capturedDataspace =
            pickBestDataspace(args.dataspace, args.colorMode, captureResults.capturedHdrLayers,
                              args.seamlessTransition);

    // Only clamp the display brightness if this is not a seamless transition.
    // Otherwise for seamless transitions it's important to match the current
    // display state as the buffer will be shown under these same conditions, and we
    // want to avoid any flickers.
    if (captureResults.capturedHdrLayers) {
        if (!enableLocalTonemapping && args.sdrWhitePointNits > 1.0f && !args.seamlessTransition) {
            // Restrict the amount of HDR "headroom" in the screenshot to avoid
            // over-dimming the SDR portion. 2.0 chosen by experimentation
            constexpr float kMaxScreenshotHeadroom = 2.0f;
            // TODO: Aim to update displayBrightnessNits earlier in screenshot
            // path so ScreenshotArgs can be passed as const
            args.displayBrightnessNits = std::min(args.sdrWhitePointNits * kMaxScreenshotHeadroom,
                                                  args.displayBrightnessNits);
        }
    } else {
        args.displayBrightnessNits = args.sdrWhitePointNits;
    }

    auto renderIntent = RenderIntent::TONE_MAP_COLORIMETRIC;
    // Screenshots leaving the device should be colorimetric
    if (args.dataspace == ui::Dataspace::UNKNOWN && args.seamlessTransition) {
        renderIntent = args.renderIntent;
    }

    auto capturedBuffer = buffer;
    captureResults.buffer = capturedBuffer->getBuffer();

    ui::LayerStack layerStack{ui::DEFAULT_LAYER_STACK};
    if (!layers.empty()) {
        const sp<LayerFE>& layerFE = layers.back().second;
        layerStack = layerFE->getCompositionState()->outputFilter.layerStack;
    }

    auto present = [this, buffer = capturedBuffer, dataspace = captureResults.capturedDataspace,
                    grayscale, isProtected, layers, layerStack, regionSampling, args, renderIntent,
                    enableLocalTonemapping]() -> FenceResult {
        std::unique_ptr<compositionengine::CompositionEngine> compositionEngine =
                mFactory.createCompositionEngine();
        compositionEngine->setRenderEngine(mRenderEngine.get());
        compositionEngine->setHwComposer(mHWComposer.get());

        std::vector<sp<compositionengine::LayerFE>> layerFEs;
        layerFEs.reserve(layers.size());
        for (auto& [layer, layerFE] : layers) {
            // Release fences were not yet added for non-threaded render engine. To avoid
            // deadlocks between main thread and binder threads waiting for the future fence
            // result, fences should be added to layers in the same hop onto the main thread.
            if (!mRenderEngine->isThreaded()) {
                attachReleaseFenceFutureToLayer(layer, layerFE.get(), ui::INVALID_LAYER_STACK);
            }
            layerFEs.push_back(layerFE);
        }

        compositionengine::Output::ColorProfile colorProfile{.dataspace = dataspace,
                                                             .renderIntent = renderIntent};

        float targetBrightness = 1.0f;
        if (enableLocalTonemapping) {
            // Boost the whole scene so that SDR white is at 1.0 while still communicating the hdr
            // sdr ratio via display brightness / sdrWhite nits.
            targetBrightness = args.sdrWhitePointNits / args.displayBrightnessNits;
        } else if (dataspace == ui::Dataspace::BT2020_HLG) {
            const float maxBrightnessNits =
                    args.displayBrightnessNits / args.sdrWhitePointNits * 203;
            // With a low dimming ratio, don't fit the entire curve. Otherwise mixed content
            // will appear way too bright.
            if (maxBrightnessNits < 1000.f) {
                targetBrightness = 1000.f / maxBrightnessNits;
            }
        }

        // Capturing screenshots using layers have a clear capture fill (0 alpha).
        // Capturing via display or displayId, which do not use args.layerSequence,
        // has an opaque capture fill (1 alpha).
        const float layerAlpha =
                std::holds_alternative<int32_t>(args.captureTypeVariant) ? 0.0f : 1.0f;

        // Screenshots leaving the device must not dim in gamma space.
        const bool dimInGammaSpaceForEnhancedScreenshots =
                mDimInGammaSpaceForEnhancedScreenshots && args.seamlessTransition;

        std::shared_ptr<ScreenCaptureOutput> output = createScreenCaptureOutput(
                ScreenCaptureOutputArgs{.compositionEngine = *compositionEngine,
                                        .colorProfile = colorProfile,
                                        .layerStack = layerStack,
                                        .sourceCrop = args.sourceCrop,
                                        .buffer = std::move(buffer),
                                        .displayIdVariant = args.displayIdVariant,
                                        .reqBufferSize = args.reqSize,
                                        .sdrWhitePointNits = args.sdrWhitePointNits,
                                        .displayBrightnessNits = args.displayBrightnessNits,
                                        .targetBrightness = targetBrightness,
                                        .layerAlpha = layerAlpha,
                                        .regionSampling = regionSampling,
                                        .treat170mAsSrgb = mTreat170mAsSrgb,
                                        .dimInGammaSpaceForEnhancedScreenshots =
                                                dimInGammaSpaceForEnhancedScreenshots,
                                        .isSecure = args.isSecure,
                                        .isProtected = isProtected,
                                        .enableLocalTonemapping = enableLocalTonemapping});

        const float colorSaturation = grayscale ? 0 : 1;
        compositionengine::CompositionRefreshArgs refreshArgs{
                .outputs = {output},
                .layers = std::move(layerFEs),
                .updatingOutputGeometryThisFrame = true,
                .updatingGeometryThisFrame = true,
                .colorTransformMatrix = calculateColorMatrix(colorSaturation),
        };
        compositionEngine->present(refreshArgs);

        return output->getRenderSurface()->getClientTargetAcquireFence();
    };

    // If RenderEngine is threaded, we can safely call CompositionEngine::present off the main
    // thread as the RenderEngine::drawLayers call will run on RenderEngine's thread. Otherwise,
    // we need RenderEngine to run on the main thread so we call CompositionEngine::present
    // immediately.
    //
    // TODO(b/196334700) Once we use RenderEngineThreaded everywhere we can always defer the call
    // to CompositionEngine::present.
    ftl::SharedFuture<FenceResult> presentFuture = mRenderEngine->isThreaded()
            ? ftl::yield(present()).share()
            : mScheduler->schedule(std::move(present)).share();

    return presentFuture;
}

void SurfaceFlinger::traverseLegacyLayers(const LayerVector::Visitor& visitor) const {
    for (auto& layer : mLegacyLayers) {
        visitor(layer.second.get());
    }
}

// ---------------------------------------------------------------------------

ftl::Optional<scheduler::FrameRateMode> SurfaceFlinger::getPreferredDisplayMode(
        PhysicalDisplayId displayId, DisplayModeId defaultModeId) const {
    if (const auto schedulerMode = mScheduler->getPreferredDisplayMode();
        schedulerMode.modePtr->getPhysicalDisplayId() == displayId) {
        return schedulerMode;
    }

    return mPhysicalDisplays.get(displayId)
            .transform(&PhysicalDisplay::snapshotRef)
            .and_then([&](const display::DisplaySnapshot& snapshot) {
                return snapshot.displayModes().get(defaultModeId);
            })
            .transform([](const DisplayModePtr& modePtr) {
                return scheduler::FrameRateMode{modePtr->getPeakFps(), ftl::as_non_null(modePtr)};
            });
}

status_t SurfaceFlinger::setDesiredDisplayModeSpecsInternal(
        const sp<DisplayDevice>& display,
        const scheduler::RefreshRateSelector::PolicyVariant& policy) {
    const auto displayId = display->getPhysicalId();
    SFTRACE_NAME(ftl::Concat(__func__, ' ', displayId.value).c_str());

    Mutex::Autolock lock(mStateLock);

    if (mDebugDisplayModeSetByBackdoor) {
        // ignore this request as mode is overridden by backdoor
        return NO_ERROR;
    }

    auto& selector = display->refreshRateSelector();
    using SetPolicyResult = scheduler::RefreshRateSelector::SetPolicyResult;

    switch (selector.setPolicy(policy)) {
        case SetPolicyResult::Invalid:
            return BAD_VALUE;
        case SetPolicyResult::Unchanged:
            return NO_ERROR;
        case SetPolicyResult::Changed:
            break;
    }

    return applyRefreshRateSelectorPolicy(displayId, selector);
}

status_t SurfaceFlinger::applyRefreshRateSelectorPolicy(
        PhysicalDisplayId displayId, const scheduler::RefreshRateSelector& selector) {
    const scheduler::RefreshRateSelector::Policy currentPolicy = selector.getCurrentPolicy();
    ALOGV("Setting desired display mode specs: %s", currentPolicy.toString().c_str());

    if (mScheduler->onDisplayModeChanged(displayId, selector.getActiveMode(),
                                         /*clearContentRequirements*/ true)) {
        mDisplayModeController.updateKernelIdleTimer(displayId);
    }

    auto preferredModeOpt = getPreferredDisplayMode(displayId, currentPolicy.defaultMode);
    if (!preferredModeOpt) {
        ALOGE("%s: Preferred mode is unknown", __func__);
        return NAME_NOT_FOUND;
    }

    auto preferredMode = std::move(*preferredModeOpt);
    const auto preferredModeId = preferredMode.modePtr->getId();

    const Fps preferredFps = preferredMode.fps;
    ALOGV("Switching to Scheduler preferred mode %d (%s)", ftl::to_underlying(preferredModeId),
          to_string(preferredFps).c_str());

    if (!selector.isModeAllowed(preferredMode)) {
        ALOGE("%s: Preferred mode %d is disallowed", __func__, ftl::to_underlying(preferredModeId));
        return INVALID_OPERATION;
    }

    setDesiredMode({std::move(preferredMode), .emitEvent = true});

    // Update the frameRateOverride list as the display render rate might have changed
    mScheduler->updateFrameRateOverrides(scheduler::GlobalSignals{}, preferredFps);
    return NO_ERROR;
}

namespace {
FpsRange translate(const gui::DisplayModeSpecs::RefreshRateRanges::RefreshRateRange& aidlRange) {
    return FpsRange{Fps::fromValue(aidlRange.min), Fps::fromValue(aidlRange.max)};
}

FpsRanges translate(const gui::DisplayModeSpecs::RefreshRateRanges& aidlRanges) {
    return FpsRanges{translate(aidlRanges.physical), translate(aidlRanges.render)};
}

gui::DisplayModeSpecs::RefreshRateRanges::RefreshRateRange translate(const FpsRange& range) {
    gui::DisplayModeSpecs::RefreshRateRanges::RefreshRateRange aidlRange;
    aidlRange.min = range.min.getValue();
    aidlRange.max = range.max.getValue();
    return aidlRange;
}

gui::DisplayModeSpecs::RefreshRateRanges translate(const FpsRanges& ranges) {
    gui::DisplayModeSpecs::RefreshRateRanges aidlRanges;
    aidlRanges.physical = translate(ranges.physical);
    aidlRanges.render = translate(ranges.render);
    return aidlRanges;
}

} // namespace

#ifdef QCOM_UM_FAMILY
bool SurfaceFlinger::canAllocateHwcDisplayIdForVDS(uint64_t usage) {
    uint64_t flag_mask_pvt_wfd = ~0;
    uint64_t flag_mask_hw_video = ~0;
    // Reserve hardware acceleration for WFD use-case
    // GRALLOC_USAGE_PRIVATE_WFD + GRALLOC_USAGE_HW_VIDEO_ENCODER = WFD using HW composer.
    flag_mask_pvt_wfd = GRALLOC_USAGE_PRIVATE_WFD;
    flag_mask_hw_video = GRALLOC_USAGE_HW_VIDEO_ENCODER;
    bool isWfd = (usage & flag_mask_pvt_wfd) && (usage & flag_mask_hw_video);
    // Enabling only the vendor property would allow WFD to use HWC
    // Enabling both the aosp and vendor properties would allow all other VDS to use HWC
    // Disabling both would set all virtual displays to fall back to GPU
    // In vendor frozen targets, allow WFD to use HWC without any property settings.
    bool canAllocate = mAllowHwcForVDS || (isWfd && mAllowHwcForWFD) || (isWfd &&
                       mFirstApiLevel < __ANDROID_API_T__);

    if (canAllocate) {
        enableHalVirtualDisplays(true);
    }

    return canAllocate;

}
#else
bool SurfaceFlinger::canAllocateHwcDisplayIdForVDS(uint64_t) {
    return true;
}
#endif

status_t SurfaceFlinger::setDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                                    const gui::DisplayModeSpecs& specs) {
    SFTRACE_CALL();

    if (!displayToken) {
        return BAD_VALUE;
    }

    auto future = mScheduler->schedule([=, this]() FTL_FAKE_GUARD(kMainThreadContext) -> status_t {
        const auto display = FTL_FAKE_GUARD(mStateLock, getDisplayDeviceLocked(displayToken));
        if (!display) {
            ALOGE("Attempt to set desired display modes for invalid display token %p",
                  displayToken.get());
            return NAME_NOT_FOUND;
        } else if (display->isVirtual()) {
            ALOGW("Attempt to set desired display modes for virtual display");
            return INVALID_OPERATION;
        } else {
            using Policy = scheduler::RefreshRateSelector::DisplayManagerPolicy;
            const auto idleScreenConfigOpt =
                    FlagManager::getInstance().idle_screen_refresh_rate_timeout()
                    ? specs.idleScreenRefreshRateConfig
                    : std::nullopt;
            const Policy policy{DisplayModeId(specs.defaultMode), translate(specs.primaryRanges),
                                translate(specs.appRequestRanges), specs.allowGroupSwitching,
                                idleScreenConfigOpt};

            return setDesiredDisplayModeSpecsInternal(display, policy);
        }
    });

    return future.get();
}

status_t SurfaceFlinger::getDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                                    gui::DisplayModeSpecs* outSpecs) {
    SFTRACE_CALL();

    if (!displayToken || !outSpecs) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);
    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    if (display->isVirtual()) {
        return INVALID_OPERATION;
    }

    scheduler::RefreshRateSelector::Policy policy =
            display->refreshRateSelector().getDisplayManagerPolicy();
    outSpecs->defaultMode = ftl::to_underlying(policy.defaultMode);
    outSpecs->allowGroupSwitching = policy.allowGroupSwitching;
    outSpecs->primaryRanges = translate(policy.primaryRanges);
    outSpecs->appRequestRanges = translate(policy.appRequestRanges);
    return NO_ERROR;
}

void SurfaceFlinger::onLayerFirstRef(Layer* layer) {
    mNumLayers++;
    mScheduler->registerLayer(layer, scheduler::FrameRateCompatibility::Default);
}

void SurfaceFlinger::onLayerDestroyed(Layer* layer) {
    mNumLayers--;
    mScheduler->deregisterLayer(layer);
    if (mTransactionTracing) {
        mTransactionTracing->onLayerRemoved(layer->getSequence());
    }
    mScheduler->onLayerDestroyed(layer);
}

void SurfaceFlinger::onLayerUpdate() {
    scheduleCommit(FrameHint::kActive);
}

status_t SurfaceFlinger::setGlobalShadowSettings(const half4& ambientColor, const half4& spotColor,
                                                 float lightPosY, float lightPosZ,
                                                 float lightRadius) {
    Mutex::Autolock _l(mStateLock);
    mCurrentState.globalShadowSettings.ambientColor = vec4(ambientColor);
    mCurrentState.globalShadowSettings.spotColor = vec4(spotColor);
    mCurrentState.globalShadowSettings.lightPos.y = lightPosY;
    mCurrentState.globalShadowSettings.lightPos.z = lightPosZ;
    mCurrentState.globalShadowSettings.lightRadius = lightRadius;

    // these values are overridden when calculating the shadow settings for a layer.
    mCurrentState.globalShadowSettings.lightPos.x = 0.f;
    mCurrentState.globalShadowSettings.length = 0.f;
    return NO_ERROR;
}

const std::unordered_map<std::string, uint32_t>& SurfaceFlinger::getGenericLayerMetadataKeyMap()
        const {
    // TODO(b/149500060): Remove this fixed/static mapping. Please prefer taking
    // on the work to remove the table in that bug rather than adding more to
    // it.
    static const std::unordered_map<std::string, uint32_t> genericLayerMetadataKeyMap{
            {"org.chromium.arc.V1_0.TaskId", gui::METADATA_TASK_ID},
            {"org.chromium.arc.V1_0.CursorInfo", gui::METADATA_MOUSE_CURSOR},
    };
    return genericLayerMetadataKeyMap;
}

status_t SurfaceFlinger::setGameModeFrameRateOverride(uid_t uid, float frameRate) {
    mScheduler->setGameModeFrameRateForUid(FrameRateOverride{uid, frameRate});
    return NO_ERROR;
}

status_t SurfaceFlinger::setGameDefaultFrameRateOverride(uid_t uid, float frameRate) {
    if (FlagManager::getInstance().game_default_frame_rate()) {
        mScheduler->setGameDefaultFrameRateForUid(
                FrameRateOverride{static_cast<uid_t>(uid), frameRate});
    }
    return NO_ERROR;
}

status_t SurfaceFlinger::updateSmallAreaDetection(
        std::vector<std::pair<int32_t, float>>& appIdThresholdMappings) {
    mScheduler->updateSmallAreaDetection(appIdThresholdMappings);
    return NO_ERROR;
}

status_t SurfaceFlinger::setSmallAreaDetectionThreshold(int32_t appId, float threshold) {
    mScheduler->setSmallAreaDetectionThreshold(appId, threshold);
    return NO_ERROR;
}

void SurfaceFlinger::enableRefreshRateOverlay(bool enable) {
    bool setByHwc = getHwComposer().hasCapability(Capability::REFRESH_RATE_CHANGED_CALLBACK_DEBUG);
    for (const auto& [displayId, physical] : mPhysicalDisplays) {
        if (physical.snapshot().connectionType() == ui::DisplayConnectionType::Internal ||
            FlagManager::getInstance().refresh_rate_overlay_on_external_display()) {
            if (const auto display = getDisplayDeviceLocked(displayId)) {
                const auto enableOverlay = [&](bool setByHwc) FTL_FAKE_GUARD(kMainThreadContext) {
                    const auto activeMode = mDisplayModeController.getActiveMode(displayId);
                    const Fps refreshRate = activeMode.modePtr->getVsyncRate();
                    const Fps renderFps = activeMode.fps;

                    display->enableRefreshRateOverlay(enable, setByHwc, refreshRate, renderFps,
                                                      mRefreshRateOverlaySpinner,
                                                      mRefreshRateOverlayRenderRate,
                                                      mRefreshRateOverlayShowInMiddle);
                };

                enableOverlay(setByHwc);
                if (setByHwc) {
                    const auto status =
                            getHwComposer().setRefreshRateChangedCallbackDebugEnabled(displayId,
                                                                                      enable);
                    if (status != NO_ERROR) {
                        ALOGE("Error %s refresh rate changed callback debug",
                              enable ? "enabling" : "disabling");
                        enableOverlay(/*setByHwc*/ false);
                    }
                }
            }
        }
    }
}

void SurfaceFlinger::enableHdrSdrRatioOverlay(bool enable) {
    for (const auto& [id, display] : mPhysicalDisplays) {
        if (display.snapshot().connectionType() == ui::DisplayConnectionType::Internal) {
            if (const auto device = getDisplayDeviceLocked(id)) {
                device->enableHdrSdrRatioOverlay(enable);
            }
        }
    }
}

int SurfaceFlinger::getGpuContextPriority() {
    return getRenderEngine().getContextPriority();
}

int SurfaceFlinger::calculateMaxAcquiredBufferCount(Fps refreshRate,
                                                    std::chrono::nanoseconds presentLatency) {
    int64_t pipelineDepth = presentLatency.count() / refreshRate.getPeriodNsecs();
    if (presentLatency.count() % refreshRate.getPeriodNsecs()) {
        pipelineDepth++;
    }
    const int64_t maxAcquiredBuffers =
            std::min(pipelineDepth - 1, maxAcquiredBuffersOpt.value_or(pipelineDepth - 1));
    return std::max(minAcquiredBuffers, maxAcquiredBuffers);
}

status_t SurfaceFlinger::getMaxAcquiredBufferCount(int* buffers) const {
    Fps maxRefreshRate = 60_Hz;

    if (!getHwComposer().isHeadless()) {
        if (const auto display = getDefaultDisplayDevice()) {
            maxRefreshRate = display->refreshRateSelector().getSupportedRefreshRateRange().max;
        }
    }

    *buffers = getMaxAcquiredBufferCountForRefreshRate(maxRefreshRate);
    return NO_ERROR;
}

uint32_t SurfaceFlinger::getMaxAcquiredBufferCountForCurrentRefreshRate(uid_t uid) const {
    Fps refreshRate = 60_Hz;

    if (const auto frameRateOverride = mScheduler->getFrameRateOverride(uid)) {
        refreshRate = *frameRateOverride;
    } else if (!getHwComposer().isHeadless()) {
        if (const auto display = FTL_FAKE_GUARD(mStateLock, getDefaultDisplayDeviceLocked())) {
            refreshRate = display->refreshRateSelector().getActiveMode().fps;
        }
    }

    return getMaxAcquiredBufferCountForRefreshRate(refreshRate);
}

int SurfaceFlinger::getMaxAcquiredBufferCountForRefreshRate(Fps refreshRate) const {
    const auto vsyncConfig = mScheduler->getVsyncConfigsForRefreshRate(refreshRate).late;
    const auto presentLatency = vsyncConfig.appWorkDuration + vsyncConfig.sfWorkDuration;
    return calculateMaxAcquiredBufferCount(refreshRate, presentLatency);
}

void SurfaceFlinger::sample() {
    if (!mLumaSampling || !mRegionSamplingThread) {
        return;
    }

    const auto scheduledFrameResultOpt = mScheduler->getScheduledFrameResult();
    const auto scheduleFrameTimeOpt = scheduledFrameResultOpt
            ? std::optional{scheduledFrameResultOpt->callbackTime}
            : std::nullopt;
    mRegionSamplingThread->onCompositionComplete(scheduleFrameTimeOpt);
}

void SurfaceFlinger::onActiveDisplaySizeChanged(const DisplayDevice& activeDisplay) {
    mScheduler->onActiveDisplayAreaChanged(activeDisplay.getWidth() * activeDisplay.getHeight());
    getRenderEngine().onActiveDisplaySizeChanged(activeDisplay.getSize());
}

sp<DisplayDevice> SurfaceFlinger::getActivatableDisplay() const {
    if (mPhysicalDisplays.size() == 1) return nullptr;

    // TODO(b/255635821): Choose the pacesetter display, considering both internal and external
    // displays. For now, pick the other internal display, assuming a dual-display foldable.
    return findDisplay([this](const DisplayDevice& display) REQUIRES(mStateLock) {
        const auto idOpt = asPhysicalDisplayId(display.getDisplayIdVariant());
        return idOpt.has_value() && *idOpt != mActiveDisplayId && display.isPoweredOn() &&
                mPhysicalDisplays.get(*idOpt)
                        .transform(&PhysicalDisplay::isInternal)
                        .value_or(false);
    });
}

void SurfaceFlinger::onActiveDisplayChangedLocked(const DisplayDevice* inactiveDisplayPtr,
                                                  const DisplayDevice& activeDisplay) {
    SFTRACE_CALL();

    if (inactiveDisplayPtr) {
        inactiveDisplayPtr->getCompositionDisplay()->setLayerCachingTexturePoolEnabled(false);
    }

    mActiveDisplayId = activeDisplay.getPhysicalId();
    activeDisplay.getCompositionDisplay()->setLayerCachingTexturePoolEnabled(true);

    // TODO(b/255635711): Check for pending mode changes on other displays.
    mScheduler->setModeChangePending(false);

    mScheduler->setPacesetterDisplay(mActiveDisplayId);

    onActiveDisplaySizeChanged(activeDisplay);
    mActiveDisplayTransformHint = activeDisplay.getTransformHint();
    sActiveDisplayRotationFlags = ui::Transform::toRotationFlags(activeDisplay.getOrientation());

    // Whether or not the policy of the new active/pacesetter display changed while it was inactive
    // (in which case its preferred mode has already been propagated to HWC via setDesiredMode), the
    // Scheduler's cachedModeChangedParams must be initialized to the newly active mode, and the
    // kernel idle timer of the newly active display must be toggled.
    applyRefreshRateSelectorPolicy(mActiveDisplayId, activeDisplay.refreshRateSelector());
}

status_t SurfaceFlinger::addWindowInfosListener(const sp<IWindowInfosListener>& windowInfosListener,
                                                gui::WindowInfosListenerInfo* outInfo) {
    mWindowInfosListenerInvoker->addWindowInfosListener(windowInfosListener, outInfo);
    setTransactionFlags(eInputInfoUpdateNeeded);
    return NO_ERROR;
}

status_t SurfaceFlinger::removeWindowInfosListener(
        const sp<IWindowInfosListener>& windowInfosListener) const {
    mWindowInfosListenerInvoker->removeWindowInfosListener(windowInfosListener);
    return NO_ERROR;
}

status_t SurfaceFlinger::getStalledTransactionInfo(
        int pid, std::optional<TransactionHandler::StalledTransactionInfo>& result) {
    // Used to add a stalled transaction which uses an internal lock.
    ftl::FakeGuard guard(kMainThreadContext);
    result = mTransactionHandler.getStalledTransactionInfo(pid);
    return NO_ERROR;
}

void SurfaceFlinger::updateHdcpLevels(hal::HWDisplayId hwcDisplayId, int32_t connectedLevel,
                                      int32_t maxLevel) {
    Mutex::Autolock lock(mStateLock);

    const auto idOpt = getHwComposer().toPhysicalDisplayId(hwcDisplayId);
    if (!idOpt) {
        ALOGE("No display found for HDCP level changed event: connected=%d, max=%d for "
              "display=%" PRIu64,
              connectedLevel, maxLevel, hwcDisplayId);
        return;
    }

    const bool isInternalDisplay =
            mPhysicalDisplays.get(*idOpt).transform(&PhysicalDisplay::isInternal).value_or(false);
    if (isInternalDisplay) {
        ALOGW("Unexpected HDCP level changed for internal display: connected=%d, max=%d for "
              "display=%" PRIu64,
              connectedLevel, maxLevel, hwcDisplayId);
        return;
    }

    static_cast<void>(mScheduler->schedule([this, displayId = *idOpt, connectedLevel, maxLevel]() {
        const bool secure = connectedLevel >= 2 /* HDCP_V1 */;
        if (const auto display = FTL_FAKE_GUARD(mStateLock, getDisplayDeviceLocked(displayId))) {
            Mutex::Autolock lock(mStateLock);
            display->setSecure(secure);
        }
        FTL_FAKE_GUARD(kMainThreadContext, mDisplayModeController.setSecure(displayId, secure));
        mScheduler->onHdcpLevelsChanged(scheduler::Cycle::Render, displayId, connectedLevel,
                                        maxLevel);
    }));
}

void SurfaceFlinger::addActivePictureListener(const sp<gui::IActivePictureListener>& listener) {
    Mutex::Autolock lock(mStateLock);
    std::erase_if(mActivePictureListenersToRemove, [listener](const auto& otherListener) {
        return IInterface::asBinder(listener) == IInterface::asBinder(otherListener);
    });
    mActivePictureListenersToAdd.push_back(listener);
}

void SurfaceFlinger::removeActivePictureListener(const sp<gui::IActivePictureListener>& listener) {
    Mutex::Autolock lock(mStateLock);
    std::erase_if(mActivePictureListenersToAdd, [listener](const auto& otherListener) {
        return IInterface::asBinder(listener) == IInterface::asBinder(otherListener);
    });
    mActivePictureListenersToRemove.push_back(listener);
}

std::shared_ptr<renderengine::ExternalTexture> SurfaceFlinger::getExternalTextureFromBufferData(
        BufferData& bufferData, const char* layerName, uint64_t transactionId) {
    if (bufferData.buffer &&
        exceedsMaxRenderTargetSize(bufferData.buffer->getWidth(), bufferData.buffer->getHeight())) {
        std::string errorMessage =
                base::StringPrintf("Attempted to create an ExternalTexture with size (%u, %u) for "
                                   "layer %s that exceeds render target size limit of %u.",
                                   bufferData.buffer->getWidth(), bufferData.buffer->getHeight(),
                                   layerName, static_cast<uint32_t>(mMaxRenderTargetSize));
        ALOGD("%s", errorMessage.c_str());
        if (bufferData.releaseBufferListener) {
            bufferData.releaseBufferListener->onTransactionQueueStalled(
                    String8(errorMessage.c_str()));
        }
        return nullptr;
    }

    bool cachedBufferChanged =
            bufferData.flags.test(BufferData::BufferDataChange::cachedBufferChanged);
    if (cachedBufferChanged && bufferData.buffer) {
        auto result = ClientCache::getInstance().add(bufferData.cachedBuffer, bufferData.buffer);
        if (result.ok()) {
            return result.value();
        }

        if (result.error() == ClientCache::AddError::CacheFull) {
            ALOGE("Attempted to create an ExternalTexture for layer %s but CacheFull", layerName);

            if (bufferData.releaseBufferListener) {
                bufferData.releaseBufferListener->onTransactionQueueStalled(
                        String8("Buffer processing hung due to full buffer cache"));
            }
        }

        return nullptr;
    }

    if (cachedBufferChanged) {
        return ClientCache::getInstance().get(bufferData.cachedBuffer);
    }

    if (bufferData.buffer) {
        return std::make_shared<
                renderengine::impl::ExternalTexture>(bufferData.buffer, getRenderEngine(),
                                                     renderengine::impl::ExternalTexture::Usage::
                                                             READABLE);
    }

    return nullptr;
}

void SurfaceFlinger::moveSnapshotsFromCompositionArgs(
        compositionengine::CompositionRefreshArgs& refreshArgs,
        const std::vector<std::pair<Layer*, LayerFE*>>& layers) {
    std::vector<std::unique_ptr<frontend::LayerSnapshot>>& snapshots =
            mLayerSnapshotBuilder.getSnapshots();
    for (auto [_, layerFE] : layers) {
        auto i = layerFE->mSnapshot->globalZ;
        snapshots[i] = std::move(layerFE->mSnapshot);
    }
}

std::vector<std::pair<Layer*, LayerFE*>> SurfaceFlinger::moveSnapshotsToCompositionArgs(
        compositionengine::CompositionRefreshArgs& refreshArgs, bool cursorOnly) {
    std::vector<std::pair<Layer*, LayerFE*>> layers;
    nsecs_t currentTime = systemTime();
    const bool needsMetadata = mCompositionEngine->getFeatureFlags().test(
            compositionengine::Feature::kSnapshotLayerMetadata);
    mLayerSnapshotBuilder.forEachSnapshot(
            [&](std::unique_ptr<frontend::LayerSnapshot>& snapshot) FTL_FAKE_GUARD(
                    kMainThreadContext) {
                if (cursorOnly &&
                    snapshot->compositionType !=
                            aidl::android::hardware::graphics::composer3::Composition::CURSOR) {
                    return;
                }

                if (!snapshot->hasSomethingToDraw()) {
                    return;
                }

                auto it = mLegacyLayers.find(snapshot->sequence);
                LLOG_ALWAYS_FATAL_WITH_TRACE_IF(it == mLegacyLayers.end(),
                                                "Couldnt find layer object for %s",
                                                snapshot->getDebugString().c_str());
                auto& legacyLayer = it->second;
                sp<LayerFE> layerFE = legacyLayer->getCompositionEngineLayerFE(snapshot->path);
                snapshot->fps = getLayerFramerate(currentTime, snapshot->sequence);
                layerFE->mSnapshot = std::move(snapshot);
                refreshArgs.layers.push_back(layerFE);
                layers.emplace_back(legacyLayer.get(), layerFE.get());
            },
            [needsMetadata](const frontend::LayerSnapshot& snapshot) {
                return snapshot.isVisible ||
                        (needsMetadata &&
                         snapshot.changes.test(frontend::RequestedLayerState::Changes::Metadata));
            });
    return layers;
}

std::function<std::vector<std::pair<Layer*, sp<LayerFE>>>()>
SurfaceFlinger::getLayerSnapshotsForScreenshots(
        std::optional<ui::LayerStack> layerStack, uint32_t uid,
        std::function<bool(const frontend::LayerSnapshot&, bool& outStopTraversal)>
                snapshotFilterFn) {
    return [&, layerStack, uid]() FTL_FAKE_GUARD(kMainThreadContext) {
        std::vector<std::pair<Layer*, sp<LayerFE>>> layers;
        bool stopTraversal = false;
        mLayerSnapshotBuilder.forEachVisibleSnapshot(
                [&](std::unique_ptr<frontend::LayerSnapshot>& snapshot) FTL_FAKE_GUARD(
                        kMainThreadContext) {
                    if (stopTraversal) {
                        return;
                    }
                    if (layerStack && snapshot->outputFilter.layerStack != *layerStack) {
                        return;
                    }
                    if (uid != CaptureArgs::UNSET_UID && snapshot->uid != gui::Uid(uid)) {
                        return;
                    }
                    if (!snapshot->hasSomethingToDraw()) {
                        return;
                    }
                    if (snapshotFilterFn && !snapshotFilterFn(*snapshot, stopTraversal)) {
                        return;
                    }

                    auto it = mLegacyLayers.find(snapshot->sequence);
                    LLOG_ALWAYS_FATAL_WITH_TRACE_IF(it == mLegacyLayers.end(),
                                                    "Couldnt find layer object for %s",
                                                    snapshot->getDebugString().c_str());
                    Layer* legacyLayer = (it == mLegacyLayers.end()) ? nullptr : it->second.get();
                    sp<LayerFE> layerFE = getFactory().createLayerFE(snapshot->name, legacyLayer);
                    layerFE->mSnapshot = std::make_unique<frontend::LayerSnapshot>(*snapshot);
                    layers.emplace_back(legacyLayer, std::move(layerFE));
                });

        return layers;
    };
}

std::function<std::vector<std::pair<Layer*, sp<LayerFE>>>()>
SurfaceFlinger::getLayerSnapshotsForScreenshots(std::optional<ui::LayerStack> layerStack,
                                                uint32_t uid,
                                                std::unordered_set<uint32_t> excludeLayerIds) {
    return [&, layerStack, uid,
            excludeLayerIds = std::move(excludeLayerIds)]() FTL_FAKE_GUARD(kMainThreadContext) {
        if (excludeLayerIds.empty()) {
            auto getLayerSnapshotsFn =
                    getLayerSnapshotsForScreenshots(layerStack, uid, /*snapshotFilterFn=*/nullptr);
            std::vector<std::pair<Layer*, sp<LayerFE>>> layers = getLayerSnapshotsFn();
            return layers;
        }

        frontend::LayerSnapshotBuilder::Args
                args{.root = mLayerHierarchyBuilder.getHierarchy(),
                     .layerLifecycleManager = mLayerLifecycleManager,
                     .forceUpdate = frontend::LayerSnapshotBuilder::ForceUpdateFlags::HIERARCHY,
                     .displays = mFrontEndDisplayInfos,
                     .displayChanges = true,
                     .globalShadowSettings = mDrawingState.globalShadowSettings,
                     .supportsBlur = mSupportsBlur,
                     .forceFullDamage = mForceFullDamage,
                     .excludeLayerIds = std::move(excludeLayerIds),
                     .supportedLayerGenericMetadata =
                             getHwComposer().getSupportedLayerGenericMetadata(),
                     .genericLayerMetadataKeyMap = getGenericLayerMetadataKeyMap(),
                     .skipRoundCornersWhenProtected =
                             !getRenderEngine().supportsProtectedContent()};
        mLayerSnapshotBuilder.update(args);

        auto getLayerSnapshotsFn =
                getLayerSnapshotsForScreenshots(layerStack, uid, /*snapshotFilterFn=*/nullptr);
        std::vector<std::pair<Layer*, sp<LayerFE>>> layers = getLayerSnapshotsFn();

        args.excludeLayerIds.clear();
        mLayerSnapshotBuilder.update(args);

        return layers;
    };
}

std::function<std::vector<std::pair<Layer*, sp<LayerFE>>>()>
SurfaceFlinger::getLayerSnapshotsForScreenshots(uint32_t rootLayerId, uint32_t uid,
                                                std::unordered_set<uint32_t> excludeLayerIds,
                                                bool childrenOnly,
                                                const std::optional<FloatRect>& parentCrop) {
    return [&, rootLayerId, uid, excludeLayerIds = std::move(excludeLayerIds), childrenOnly,
            parentCrop]() FTL_FAKE_GUARD(kMainThreadContext) {
        auto root = mLayerHierarchyBuilder.getPartialHierarchy(rootLayerId, childrenOnly);
        frontend::LayerSnapshotBuilder::Args
                args{.root = root,
                     .layerLifecycleManager = mLayerLifecycleManager,
                     .forceUpdate = frontend::LayerSnapshotBuilder::ForceUpdateFlags::HIERARCHY,
                     .displays = mFrontEndDisplayInfos,
                     .displayChanges = true,
                     .globalShadowSettings = mDrawingState.globalShadowSettings,
                     .supportsBlur = mSupportsBlur,
                     .forceFullDamage = mForceFullDamage,
                     .parentCrop = parentCrop,
                     .excludeLayerIds = std::move(excludeLayerIds),
                     .supportedLayerGenericMetadata =
                             getHwComposer().getSupportedLayerGenericMetadata(),
                     .genericLayerMetadataKeyMap = getGenericLayerMetadataKeyMap(),
                     .skipRoundCornersWhenProtected =
                             !getRenderEngine().supportsProtectedContent()};
        // The layer may not exist if it was just created and a screenshot was requested immediately
        // after. In this case, the hierarchy will be empty so we will not render any layers.
        args.rootSnapshot.isSecure = mLayerLifecycleManager.getLayerFromId(rootLayerId) &&
                mLayerLifecycleManager.isLayerSecure(rootLayerId);
        mLayerSnapshotBuilder.update(args);

        auto getLayerSnapshotsFn =
                getLayerSnapshotsForScreenshots({}, uid, /*snapshotFilterFn=*/nullptr);
        std::vector<std::pair<Layer*, sp<LayerFE>>> layers = getLayerSnapshotsFn();
        args.root = mLayerHierarchyBuilder.getHierarchy();
        args.parentCrop.reset();
        args.excludeLayerIds.clear();
        mLayerSnapshotBuilder.update(args);
        return layers;
    };
}

void SurfaceFlinger::doActiveLayersTracingIfNeeded(bool isCompositionComputed,
                                                   bool visibleRegionDirty, TimePoint time,
                                                   VsyncId vsyncId) {
    if (!mLayerTracing.isActiveTracingStarted()) {
        return;
    }
    if (isCompositionComputed !=
        mLayerTracing.isActiveTracingFlagSet(LayerTracing::Flag::TRACE_COMPOSITION)) {
        return;
    }
    if (!visibleRegionDirty &&
        !mLayerTracing.isActiveTracingFlagSet(LayerTracing::Flag::TRACE_BUFFERS)) {
        return;
    }
    auto snapshot = takeLayersSnapshotProto(mLayerTracing.getActiveTracingFlags(), time, vsyncId,
                                            visibleRegionDirty);
    mLayerTracing.addProtoSnapshotToOstream(std::move(snapshot), LayerTracing::Mode::MODE_ACTIVE);
}

perfetto::protos::LayersSnapshotProto SurfaceFlinger::takeLayersSnapshotProto(
        uint32_t traceFlags, TimePoint time, VsyncId vsyncId, bool visibleRegionDirty) {
    SFTRACE_CALL();
    perfetto::protos::LayersSnapshotProto snapshot;
    snapshot.set_elapsed_realtime_nanos(time.ns());
    snapshot.set_vsync_id(ftl::to_underlying(vsyncId));
    snapshot.set_where(visibleRegionDirty ? "visibleRegionsDirty" : "bufferLatched");
    snapshot.set_excludes_composition_state((traceFlags & LayerTracing::Flag::TRACE_COMPOSITION) ==
                                            0);

    auto layers = dumpDrawingStateProto(traceFlags);
    *snapshot.mutable_layers() = std::move(layers);

    if (traceFlags & LayerTracing::Flag::TRACE_HWC) {
        std::string hwcDump;
        dumpHwc(hwcDump);
        snapshot.set_hwc_blob(std::move(hwcDump));
    }

    *snapshot.mutable_displays() = dumpDisplayProto();

    return snapshot;
}

// sfdo functions

void SurfaceFlinger::sfdo_enableRefreshRateOverlay(bool active) {
    auto future = mScheduler->schedule(
            [&]() FTL_FAKE_GUARD(mStateLock)
                    FTL_FAKE_GUARD(kMainThreadContext) { enableRefreshRateOverlay(active); });
    future.wait();
}

void SurfaceFlinger::sfdo_setDebugFlash(int delay) {
    if (delay > 0) {
        mDebugFlashDelay = delay;
    } else {
        mDebugFlashDelay = mDebugFlashDelay ? 0 : 1;
    }
    scheduleRepaint();
}

void SurfaceFlinger::sfdo_scheduleComposite() {
    scheduleComposite(SurfaceFlinger::FrameHint::kActive);
}

void SurfaceFlinger::sfdo_scheduleCommit() {
    Mutex::Autolock lock(mStateLock);
    setTransactionFlags(eTransactionNeeded | eDisplayTransactionNeeded | eTraversalNeeded);
}

void SurfaceFlinger::sfdo_forceClientComposition(bool enabled) {
    mDebugDisableHWC = enabled;
    scheduleRepaint();
}

// gui::ISurfaceComposer

binder::Status SurfaceComposerAIDL::bootFinished() {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->bootFinished();
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::createDisplayEventConnection(
        VsyncSource vsyncSource, EventRegistration eventRegistration,
        const sp<IBinder>& layerHandle, sp<IDisplayEventConnection>* outConnection) {
    sp<IDisplayEventConnection> conn =
            mFlinger->createDisplayEventConnection(vsyncSource, eventRegistration, layerHandle);
    if (conn == nullptr) {
        *outConnection = nullptr;
        return binderStatusFromStatusT(BAD_VALUE);
    } else {
        *outConnection = conn;
        return binder::Status::ok();
    }
}

binder::Status SurfaceComposerAIDL::createConnection(sp<gui::ISurfaceComposerClient>* outClient) {
    const sp<Client> client = sp<Client>::make(mFlinger);
    if (client->initCheck() == NO_ERROR) {
        *outClient = client;
        if (FlagManager::getInstance().misc1()) {
            const int policy = SCHED_FIFO;
            client->setMinSchedulerPolicy(policy, sched_get_priority_min(policy));
        }
        return binder::Status::ok();
    } else {
        *outClient = nullptr;
        return binderStatusFromStatusT(BAD_VALUE);
    }
}

binder::Status SurfaceComposerAIDL::createVirtualDisplay(
        const std::string& displayName, bool isSecure,
        gui::ISurfaceComposer::OptimizationPolicy optimizationPolicy, const std::string& uniqueId,
        float requestedRefreshRate, sp<IBinder>* outDisplay) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    *outDisplay = mFlinger->createVirtualDisplay(displayName, isSecure, optimizationPolicy,
                                                 uniqueId, requestedRefreshRate);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::destroyVirtualDisplay(const sp<IBinder>& displayToken) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    return binder::Status::fromStatusT(mFlinger->destroyVirtualDisplay(displayToken));
}

binder::Status SurfaceComposerAIDL::getPhysicalDisplayIds(std::vector<int64_t>* outDisplayIds) {
    std::vector<PhysicalDisplayId> physicalDisplayIds = mFlinger->getPhysicalDisplayIds();
    std::vector<int64_t> displayIds;
    displayIds.reserve(physicalDisplayIds.size());
    for (const auto id : physicalDisplayIds) {
        displayIds.push_back(static_cast<int64_t>(id.value));
    }
    *outDisplayIds = std::move(displayIds);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::getPhysicalDisplayToken(int64_t displayId,
                                                            sp<IBinder>* outDisplay) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    const PhysicalDisplayId id = PhysicalDisplayId::fromValue(static_cast<uint64_t>(displayId));
    *outDisplay = mFlinger->getPhysicalDisplayToken(id);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::setPowerMode(const sp<IBinder>& display, int mode) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->setPowerMode(display, mode);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) {
    status_t status;
    if (!outSupported) {
        status = UNEXPECTED_NULL;
    } else {
        outSupported->clear();
        status = mFlinger->getSupportedFrameTimestamps(outSupported);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getDisplayStats(const sp<IBinder>& display,
                                                    gui::DisplayStatInfo* outStatInfo) {
    DisplayStatInfo statInfo;
    status_t status = mFlinger->getDisplayStats(display, &statInfo);
    if (status == NO_ERROR) {
        outStatInfo->vsyncTime = static_cast<long>(statInfo.vsyncTime);
        outStatInfo->vsyncPeriod = static_cast<long>(statInfo.vsyncPeriod);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getDisplayState(const sp<IBinder>& display,
                                                    gui::DisplayState* outState) {
    ui::DisplayState state;
    status_t status = mFlinger->getDisplayState(display, &state);
    if (status == NO_ERROR) {
        outState->layerStack = state.layerStack.id;
        outState->orientation = static_cast<gui::Rotation>(state.orientation);
        outState->layerStackSpaceRect.width = state.layerStackSpaceRect.width;
        outState->layerStackSpaceRect.height = state.layerStackSpaceRect.height;
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getStaticDisplayInfo(int64_t displayId,
                                                         gui::StaticDisplayInfo* outInfo) {
    using Tag = gui::DeviceProductInfo::ManufactureOrModelDate::Tag;
    ui::StaticDisplayInfo info;

    status_t status = mFlinger->getStaticDisplayInfo(displayId, &info);
    if (status == NO_ERROR) {
        // convert ui::StaticDisplayInfo to gui::StaticDisplayInfo
        outInfo->connectionType = static_cast<gui::DisplayConnectionType>(info.connectionType);
        outInfo->port = info.port;
        outInfo->density = info.density;
        outInfo->secure = info.secure;
        outInfo->installOrientation = static_cast<gui::Rotation>(info.installOrientation);

        if (const std::optional<DeviceProductInfo> dpi = info.deviceProductInfo) {
            gui::DeviceProductInfo dinfo;
            dinfo.name = std::move(dpi->name);
            dinfo.manufacturerPnpId = std::vector<uint8_t>(dpi->manufacturerPnpId.begin(),
                                                           dpi->manufacturerPnpId.end());
            dinfo.productId = dpi->productId;
            dinfo.relativeAddress =
                    std::vector<uint8_t>(dpi->relativeAddress.begin(), dpi->relativeAddress.end());
            if (const auto* model =
                        std::get_if<DeviceProductInfo::ModelYear>(&dpi->manufactureOrModelDate)) {
                gui::DeviceProductInfo::ModelYear modelYear;
                modelYear.year = model->year;
                dinfo.manufactureOrModelDate.set<Tag::modelYear>(modelYear);
            } else if (const auto* manufacture = std::get_if<DeviceProductInfo::ManufactureYear>(
                               &dpi->manufactureOrModelDate)) {
                gui::DeviceProductInfo::ManufactureYear date;
                date.modelYear.year = manufacture->year;
                dinfo.manufactureOrModelDate.set<Tag::manufactureYear>(date);
            } else if (const auto* manufacture =
                               std::get_if<DeviceProductInfo::ManufactureWeekAndYear>(
                                       &dpi->manufactureOrModelDate)) {
                gui::DeviceProductInfo::ManufactureWeekAndYear date;
                date.manufactureYear.modelYear.year = manufacture->year;
                date.week = manufacture->week;
                dinfo.manufactureOrModelDate.set<Tag::manufactureWeekAndYear>(date);
            }

            outInfo->deviceProductInfo = dinfo;
        }
    }
    return binderStatusFromStatusT(status);
}

void SurfaceComposerAIDL::getDynamicDisplayInfoInternal(ui::DynamicDisplayInfo& info,
                                                        gui::DynamicDisplayInfo*& outInfo) {
    // convert ui::DynamicDisplayInfo to gui::DynamicDisplayInfo
    outInfo->supportedDisplayModes.clear();
    outInfo->supportedDisplayModes.reserve(info.supportedDisplayModes.size());
    for (const auto& mode : info.supportedDisplayModes) {
        gui::DisplayMode outMode;
        outMode.id = mode.id;
        outMode.resolution.width = mode.resolution.width;
        outMode.resolution.height = mode.resolution.height;
        outMode.xDpi = mode.xDpi;
        outMode.yDpi = mode.yDpi;
        outMode.peakRefreshRate = mode.peakRefreshRate;
        outMode.vsyncRate = mode.vsyncRate;
        outMode.appVsyncOffset = mode.appVsyncOffset;
        outMode.sfVsyncOffset = mode.sfVsyncOffset;
        outMode.presentationDeadline = mode.presentationDeadline;
        outMode.group = mode.group;
        std::transform(mode.supportedHdrTypes.begin(), mode.supportedHdrTypes.end(),
                       std::back_inserter(outMode.supportedHdrTypes),
                       [](const ui::Hdr& value) { return static_cast<int32_t>(value); });
        outInfo->supportedDisplayModes.push_back(outMode);
    }

    outInfo->activeDisplayModeId = info.activeDisplayModeId;
    outInfo->renderFrameRate = info.renderFrameRate;
    outInfo->hasArrSupport = info.hasArrSupport;
    gui::FrameRateCategoryRate& frameRateCategoryRate = outInfo->frameRateCategoryRate;
    frameRateCategoryRate.normal = info.frameRateCategoryRate.getNormal();
    frameRateCategoryRate.high = info.frameRateCategoryRate.getHigh();
    outInfo->supportedRefreshRates.clear();
    outInfo->supportedRefreshRates.reserve(info.supportedRefreshRates.size());
    for (float supportedRefreshRate : info.supportedRefreshRates) {
        outInfo->supportedRefreshRates.push_back(supportedRefreshRate);
    }

    outInfo->supportedColorModes.clear();
    outInfo->supportedColorModes.reserve(info.supportedColorModes.size());
    for (const auto& cmode : info.supportedColorModes) {
        outInfo->supportedColorModes.push_back(static_cast<int32_t>(cmode));
    }

    outInfo->activeColorMode = static_cast<int32_t>(info.activeColorMode);

    gui::HdrCapabilities& hdrCapabilities = outInfo->hdrCapabilities;
    hdrCapabilities.supportedHdrTypes.clear();
    hdrCapabilities.supportedHdrTypes.reserve(info.hdrCapabilities.getSupportedHdrTypes().size());
    for (const auto& hdr : info.hdrCapabilities.getSupportedHdrTypes()) {
        hdrCapabilities.supportedHdrTypes.push_back(static_cast<int32_t>(hdr));
    }
    hdrCapabilities.maxLuminance = info.hdrCapabilities.getDesiredMaxLuminance();
    hdrCapabilities.maxAverageLuminance = info.hdrCapabilities.getDesiredMaxAverageLuminance();
    hdrCapabilities.minLuminance = info.hdrCapabilities.getDesiredMinLuminance();

    outInfo->autoLowLatencyModeSupported = info.autoLowLatencyModeSupported;
    outInfo->gameContentTypeSupported = info.gameContentTypeSupported;
    outInfo->preferredBootDisplayMode = info.preferredBootDisplayMode;
}

binder::Status SurfaceComposerAIDL::getDynamicDisplayInfoFromToken(
        const sp<IBinder>& display, gui::DynamicDisplayInfo* outInfo) {
    ui::DynamicDisplayInfo info;
    status_t status = mFlinger->getDynamicDisplayInfoFromToken(display, &info);
    if (status == NO_ERROR) {
        getDynamicDisplayInfoInternal(info, outInfo);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getDynamicDisplayInfoFromId(int64_t displayId,
                                                                gui::DynamicDisplayInfo* outInfo) {
    ui::DynamicDisplayInfo info;
    status_t status = mFlinger->getDynamicDisplayInfoFromId(displayId, &info);
    if (status == NO_ERROR) {
        getDynamicDisplayInfoInternal(info, outInfo);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getDisplayNativePrimaries(const sp<IBinder>& display,
                                                              gui::DisplayPrimaries* outPrimaries) {
    ui::DisplayPrimaries primaries;
    status_t status = mFlinger->getDisplayNativePrimaries(display, primaries);
    if (status == NO_ERROR) {
        outPrimaries->red.X = primaries.red.X;
        outPrimaries->red.Y = primaries.red.Y;
        outPrimaries->red.Z = primaries.red.Z;

        outPrimaries->green.X = primaries.green.X;
        outPrimaries->green.Y = primaries.green.Y;
        outPrimaries->green.Z = primaries.green.Z;

        outPrimaries->blue.X = primaries.blue.X;
        outPrimaries->blue.Y = primaries.blue.Y;
        outPrimaries->blue.Z = primaries.blue.Z;

        outPrimaries->white.X = primaries.white.X;
        outPrimaries->white.Y = primaries.white.Y;
        outPrimaries->white.Z = primaries.white.Z;
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setActiveColorMode(const sp<IBinder>& display, int colorMode) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->setActiveColorMode(display, static_cast<ui::ColorMode>(colorMode));
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setBootDisplayMode(const sp<IBinder>& display,
                                                       int displayModeId) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->setBootDisplayMode(display, DisplayModeId{displayModeId});
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::clearBootDisplayMode(const sp<IBinder>& display) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->clearBootDisplayMode(display);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getOverlaySupport(gui::OverlayProperties* outProperties) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->getOverlaySupport(outProperties);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getBootDisplayModeSupport(bool* outMode) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->getBootDisplayModeSupport(outMode);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getHdrConversionCapabilities(
        std::vector<gui::HdrConversionCapability>* hdrConversionCapabilities) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->getHdrConversionCapabilities(hdrConversionCapabilities);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setHdrConversionStrategy(
        const gui::HdrConversionStrategy& hdrConversionStrategy,
        int32_t* outPreferredHdrOutputType) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->setHdrConversionStrategy(hdrConversionStrategy,
                                                    outPreferredHdrOutputType);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getHdrOutputConversionSupport(bool* outMode) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->getHdrOutputConversionSupport(outMode);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setAutoLowLatencyMode(const sp<IBinder>& display, bool on) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->setAutoLowLatencyMode(display, on);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::setGameContentType(const sp<IBinder>& display, bool on) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->setGameContentType(display, on);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::getMaxLayerPictureProfiles(const sp<IBinder>& display,
                                                               int32_t* outMaxProfiles) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->getMaxLayerPictureProfiles(display, outMaxProfiles);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::captureDisplay(
        const DisplayCaptureArgs& args, const sp<IScreenCaptureListener>& captureListener) {
    mFlinger->captureDisplay(args, captureListener);
    return binderStatusFromStatusT(NO_ERROR);
}

binder::Status SurfaceComposerAIDL::captureDisplayById(
        int64_t displayId, const CaptureArgs& args,
        const sp<IScreenCaptureListener>& captureListener) {
    // status_t status;
    IPCThreadState* ipc = IPCThreadState::self();
    const int uid = ipc->getCallingUid();
    if (uid == AID_ROOT || uid == AID_GRAPHICS || uid == AID_SYSTEM || uid == AID_SHELL) {
        std::optional<DisplayId> id = DisplayId::fromValue(static_cast<uint64_t>(displayId));
        mFlinger->captureDisplay(*id, args, captureListener);
    } else {
        ALOGD("Permission denied to captureDisplayById");
        invokeScreenCaptureError(PERMISSION_DENIED, captureListener);
    }
    return binderStatusFromStatusT(NO_ERROR);
}

binder::Status SurfaceComposerAIDL::captureLayersSync(const LayerCaptureArgs& args,
                                                      ScreenCaptureResults* outResults) {
    *outResults = mFlinger->captureLayersSync(args);
    return binderStatusFromStatusT(NO_ERROR);
}

binder::Status SurfaceComposerAIDL::captureLayers(
        const LayerCaptureArgs& args, const sp<IScreenCaptureListener>& captureListener) {
    mFlinger->captureLayers(args, captureListener);
    return binderStatusFromStatusT(NO_ERROR);
}

binder::Status SurfaceComposerAIDL::overrideHdrTypes(const sp<IBinder>& display,
                                                     const std::vector<int32_t>& hdrTypes) {
    // overrideHdrTypes is used by CTS tests, which acquire the necessary
    // permission dynamically. Don't use the permission cache for this check.
    status_t status = checkAccessPermission(false);
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }

    std::vector<ui::Hdr> hdrTypesVector;
    for (int32_t i : hdrTypes) {
        hdrTypesVector.push_back(static_cast<ui::Hdr>(i));
    }
    status = mFlinger->overrideHdrTypes(display, hdrTypesVector);
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::onPullAtom(int32_t atomId, gui::PullAtomData* outPullData) {
    status_t status;
    const int uid = IPCThreadState::self()->getCallingUid();
    if (uid != AID_SYSTEM) {
        status = PERMISSION_DENIED;
    } else {
        status = mFlinger->onPullAtom(atomId, &outPullData->data, &outPullData->success);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getCompositionPreference(gui::CompositionPreference* outPref) {
    ui::Dataspace dataspace;
    ui::PixelFormat pixelFormat;
    ui::Dataspace wideColorGamutDataspace;
    ui::PixelFormat wideColorGamutPixelFormat;
    status_t status =
            mFlinger->getCompositionPreference(&dataspace, &pixelFormat, &wideColorGamutDataspace,
                                               &wideColorGamutPixelFormat);
    if (status == NO_ERROR) {
        outPref->defaultDataspace = static_cast<int32_t>(dataspace);
        outPref->defaultPixelFormat = static_cast<int32_t>(pixelFormat);
        outPref->wideColorGamutDataspace = static_cast<int32_t>(wideColorGamutDataspace);
        outPref->wideColorGamutPixelFormat = static_cast<int32_t>(wideColorGamutPixelFormat);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getDisplayedContentSamplingAttributes(
        const sp<IBinder>& display, gui::ContentSamplingAttributes* outAttrs) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }

    ui::PixelFormat format;
    ui::Dataspace dataspace;
    uint8_t componentMask;
    status = mFlinger->getDisplayedContentSamplingAttributes(display, &format, &dataspace,
                                                             &componentMask);
    if (status == NO_ERROR) {
        outAttrs->format = static_cast<int32_t>(format);
        outAttrs->dataspace = static_cast<int32_t>(dataspace);
        outAttrs->componentMask = static_cast<int8_t>(componentMask);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setDisplayContentSamplingEnabled(const sp<IBinder>& display,
                                                                     bool enable,
                                                                     int8_t componentMask,
                                                                     int64_t maxFrames) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->setDisplayContentSamplingEnabled(display, enable,
                                                            static_cast<uint8_t>(componentMask),
                                                            static_cast<uint64_t>(maxFrames));
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getDisplayedContentSample(const sp<IBinder>& display,
                                                              int64_t maxFrames, int64_t timestamp,
                                                              gui::DisplayedFrameStats* outStats) {
    if (!outStats) {
        return binderStatusFromStatusT(BAD_VALUE);
    }

    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }

    DisplayedFrameStats stats;
    status = mFlinger->getDisplayedContentSample(display, static_cast<uint64_t>(maxFrames),
                                                 static_cast<uint64_t>(timestamp), &stats);
    if (status == NO_ERROR) {
        // convert from ui::DisplayedFrameStats to gui::DisplayedFrameStats
        outStats->numFrames = static_cast<int64_t>(stats.numFrames);
        outStats->component_0_sample.reserve(stats.component_0_sample.size());
        for (const auto& s : stats.component_0_sample) {
            outStats->component_0_sample.push_back(static_cast<int64_t>(s));
        }
        outStats->component_1_sample.reserve(stats.component_1_sample.size());
        for (const auto& s : stats.component_1_sample) {
            outStats->component_1_sample.push_back(static_cast<int64_t>(s));
        }
        outStats->component_2_sample.reserve(stats.component_2_sample.size());
        for (const auto& s : stats.component_2_sample) {
            outStats->component_2_sample.push_back(static_cast<int64_t>(s));
        }
        outStats->component_3_sample.reserve(stats.component_3_sample.size());
        for (const auto& s : stats.component_3_sample) {
            outStats->component_3_sample.push_back(static_cast<int64_t>(s));
        }
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getProtectedContentSupport(bool* outSupported) {
    status_t status = mFlinger->getProtectedContentSupport(outSupported);
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::isWideColorDisplay(const sp<IBinder>& token,
                                                       bool* outIsWideColorDisplay) {
    status_t status = mFlinger->isWideColorDisplay(token, outIsWideColorDisplay);
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::addRegionSamplingListener(
        const gui::ARect& samplingArea, const sp<IBinder>& stopLayerHandle,
        const sp<gui::IRegionSamplingListener>& listener) {
    status_t status = checkReadFrameBufferPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    android::Rect rect;
    rect.left = samplingArea.left;
    rect.top = samplingArea.top;
    rect.right = samplingArea.right;
    rect.bottom = samplingArea.bottom;
    status = mFlinger->addRegionSamplingListener(rect, stopLayerHandle, listener);
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::removeRegionSamplingListener(
        const sp<gui::IRegionSamplingListener>& listener) {
    status_t status = checkReadFrameBufferPermission();
    if (status == OK) {
        status = mFlinger->removeRegionSamplingListener(listener);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::addFpsListener(int32_t taskId,
                                                   const sp<gui::IFpsListener>& listener) {
    status_t status = checkReadFrameBufferPermission();
    if (status == OK) {
        status = mFlinger->addFpsListener(taskId, listener);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::removeFpsListener(const sp<gui::IFpsListener>& listener) {
    status_t status = checkReadFrameBufferPermission();
    if (status == OK) {
        status = mFlinger->removeFpsListener(listener);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::addTunnelModeEnabledListener(
        const sp<gui::ITunnelModeEnabledListener>& listener) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->addTunnelModeEnabledListener(listener);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::removeTunnelModeEnabledListener(
        const sp<gui::ITunnelModeEnabledListener>& listener) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->removeTunnelModeEnabledListener(listener);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                                               const gui::DisplayModeSpecs& specs) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->setDesiredDisplayModeSpecs(displayToken, specs);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                                               gui::DisplayModeSpecs* outSpecs) {
    if (!outSpecs) {
        return binderStatusFromStatusT(BAD_VALUE);
    }

    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }

    status = mFlinger->getDesiredDisplayModeSpecs(displayToken, outSpecs);
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getDisplayBrightnessSupport(const sp<IBinder>& displayToken,
                                                                bool* outSupport) {
    status_t status = mFlinger->getDisplayBrightnessSupport(displayToken, outSupport);
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setDisplayBrightness(const sp<IBinder>& displayToken,
                                                         const gui::DisplayBrightness& brightness) {
    status_t status = checkControlDisplayBrightnessPermission();
    if (status == OK) {
        status = mFlinger->setDisplayBrightness(displayToken, brightness);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::addHdrLayerInfoListener(
        const sp<IBinder>& displayToken, const sp<gui::IHdrLayerInfoListener>& listener) {
    status_t status = checkControlDisplayBrightnessPermission();
    if (status == OK) {
        status = mFlinger->addHdrLayerInfoListener(displayToken, listener);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::removeHdrLayerInfoListener(
        const sp<IBinder>& displayToken, const sp<gui::IHdrLayerInfoListener>& listener) {
    status_t status = checkControlDisplayBrightnessPermission();
    if (status == OK) {
        status = mFlinger->removeHdrLayerInfoListener(displayToken, listener);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::addActivePictureListener(
        const sp<gui::IActivePictureListener>& listener) {
    status_t status = checkObservePictureProfilesPermission();
    if (status == OK) {
        mFlinger->addActivePictureListener(listener);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::removeActivePictureListener(
        const sp<gui::IActivePictureListener>& listener) {
    status_t status = checkObservePictureProfilesPermission();
    if (status == OK) {
        mFlinger->removeActivePictureListener(listener);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::notifyPowerBoost(int boostId) {
    status_t status = checkAccessPermission();
    if (status == OK) {
        status = mFlinger->notifyPowerBoost(boostId);
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setGlobalShadowSettings(const gui::Color& ambientColor,
                                                            const gui::Color& spotColor,
                                                            float lightPosY, float lightPosZ,
                                                            float lightRadius) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }

    half4 ambientColorHalf = {ambientColor.r, ambientColor.g, ambientColor.b, ambientColor.a};
    half4 spotColorHalf = {spotColor.r, spotColor.g, spotColor.b, spotColor.a};
    status = mFlinger->setGlobalShadowSettings(ambientColorHalf, spotColorHalf, lightPosY,
                                               lightPosZ, lightRadius);
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getDisplayDecorationSupport(
        const sp<IBinder>& displayToken, std::optional<gui::DisplayDecorationSupport>* outSupport) {
    std::optional<aidl::android::hardware::graphics::common::DisplayDecorationSupport> support;
    status_t status = mFlinger->getDisplayDecorationSupport(displayToken, &support);
    if (status != NO_ERROR) {
        ALOGE("getDisplayDecorationSupport failed with error %d", status);
        return binderStatusFromStatusT(status);
    }

    if (!support || !support.has_value()) {
        outSupport->reset();
    } else {
        outSupport->emplace();
        outSupport->value().format = static_cast<int32_t>(support->format);
        outSupport->value().alphaInterpretation =
                static_cast<int32_t>(support->alphaInterpretation);
    }

    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::setGameModeFrameRateOverride(int32_t uid, float frameRate) {
    status_t status;
    const int c_uid = IPCThreadState::self()->getCallingUid();
    if (c_uid == AID_ROOT || c_uid == AID_SYSTEM) {
        status = mFlinger->setGameModeFrameRateOverride(uid, frameRate);
    } else {
        ALOGE("setGameModeFrameRateOverride() permission denied for uid: %d", c_uid);
        status = PERMISSION_DENIED;
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setGameDefaultFrameRateOverride(int32_t uid, float frameRate) {
    status_t status;
    const int c_uid = IPCThreadState::self()->getCallingUid();
    if (c_uid == AID_ROOT || c_uid == AID_SYSTEM) {
        status = mFlinger->setGameDefaultFrameRateOverride(uid, frameRate);
    } else {
        ALOGE("setGameDefaultFrameRateOverride() permission denied for uid: %d", c_uid);
        status = PERMISSION_DENIED;
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::enableRefreshRateOverlay(bool active) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->sfdo_enableRefreshRateOverlay(active);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::setDebugFlash(int delay) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->sfdo_setDebugFlash(delay);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::scheduleComposite() {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->sfdo_scheduleComposite();
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::scheduleCommit() {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->sfdo_scheduleCommit();
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::forceClientComposition(bool enabled) {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->sfdo_forceClientComposition(enabled);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::updateSmallAreaDetection(const std::vector<int32_t>& appIds,
                                                             const std::vector<float>& thresholds) {
    status_t status;
    const int c_uid = IPCThreadState::self()->getCallingUid();
    if (c_uid == AID_ROOT || c_uid == AID_SYSTEM) {
        if (appIds.size() != thresholds.size()) return binderStatusFromStatusT(BAD_VALUE);

        std::vector<std::pair<int32_t, float>> mappings;
        const size_t size = appIds.size();
        mappings.reserve(size);
        for (int i = 0; i < size; i++) {
            auto row = std::make_pair(appIds[i], thresholds[i]);
            mappings.push_back(row);
        }
        status = mFlinger->updateSmallAreaDetection(mappings);
    } else {
        ALOGE("updateSmallAreaDetection() permission denied for uid: %d", c_uid);
        status = PERMISSION_DENIED;
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::setSmallAreaDetectionThreshold(int32_t appId, float threshold) {
    status_t status;
    const int c_uid = IPCThreadState::self()->getCallingUid();
    if (c_uid == AID_ROOT || c_uid == AID_SYSTEM) {
        status = mFlinger->setSmallAreaDetectionThreshold(appId, threshold);
    } else {
        ALOGE("setSmallAreaDetectionThreshold() permission denied for uid: %d", c_uid);
        status = PERMISSION_DENIED;
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getGpuContextPriority(int32_t* outPriority) {
    *outPriority = mFlinger->getGpuContextPriority();
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::getMaxAcquiredBufferCount(int32_t* buffers) {
    status_t status = mFlinger->getMaxAcquiredBufferCount(buffers);
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::addWindowInfosListener(
        const sp<gui::IWindowInfosListener>& windowInfosListener,
        gui::WindowInfosListenerInfo* outInfo) {
    status_t status;
    const int pid = IPCThreadState::self()->getCallingPid();
    const int uid = IPCThreadState::self()->getCallingUid();
    // TODO(b/270566761) update permissions check so that only system_server and shell can add
    // WindowInfosListeners
    if (uid == AID_SYSTEM || uid == AID_GRAPHICS ||
        checkPermission(sAccessSurfaceFlinger, pid, uid)) {
        status = mFlinger->addWindowInfosListener(windowInfosListener, outInfo);
    } else {
        status = PERMISSION_DENIED;
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::removeWindowInfosListener(
        const sp<gui::IWindowInfosListener>& windowInfosListener) {
    status_t status;
    const int pid = IPCThreadState::self()->getCallingPid();
    const int uid = IPCThreadState::self()->getCallingUid();
    if (uid == AID_SYSTEM || uid == AID_GRAPHICS ||
        checkPermission(sAccessSurfaceFlinger, pid, uid)) {
        status = mFlinger->removeWindowInfosListener(windowInfosListener);
    } else {
        status = PERMISSION_DENIED;
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getStalledTransactionInfo(
        int pid, std::optional<gui::StalledTransactionInfo>* outInfo) {
    const int callingPid = IPCThreadState::self()->getCallingPid();
    const int callingUid = IPCThreadState::self()->getCallingUid();
    if (!checkPermission(sAccessSurfaceFlinger, callingPid, callingUid)) {
        return binderStatusFromStatusT(PERMISSION_DENIED);
    }

    std::optional<TransactionHandler::StalledTransactionInfo> stalledTransactionInfo;
    status_t status = mFlinger->getStalledTransactionInfo(pid, stalledTransactionInfo);
    if (stalledTransactionInfo) {
        gui::StalledTransactionInfo result;
        result.layerName = String16{stalledTransactionInfo->layerName.c_str()},
        result.bufferId = stalledTransactionInfo->bufferId,
        result.frameNumber = stalledTransactionInfo->frameNumber,
        outInfo->emplace(std::move(result));
    } else {
        outInfo->reset();
    }
    return binderStatusFromStatusT(status);
}

binder::Status SurfaceComposerAIDL::getSchedulingPolicy(gui::SchedulingPolicy* outPolicy) {
    return gui::getSchedulingPolicy(outPolicy);
}

binder::Status SurfaceComposerAIDL::notifyShutdown() {
    TransactionTraceWriter::getInstance().invoke("systemShutdown_", /* overwrite= */ false);
    return ::android::binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::addJankListener(const sp<IBinder>& layerHandle,
                                                    const sp<gui::IJankListener>& listener) {
    sp<Layer> layer = LayerHandle::getLayer(layerHandle);
    if (layer == nullptr) {
        return binder::Status::fromExceptionCode(binder::Status::EX_NULL_POINTER);
    }
    JankTracker::addJankListener(layer->sequence, IInterface::asBinder(listener));
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::flushJankData(int32_t layerId) {
    JankTracker::flushJankData(layerId);
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::removeJankListener(int32_t layerId,
                                                       const sp<gui::IJankListener>& listener,
                                                       int64_t afterVsync) {
    JankTracker::removeJankListener(layerId, IInterface::asBinder(listener), afterVsync);
    return binder::Status::ok();
}

status_t SurfaceComposerAIDL::checkAccessPermission(bool usePermissionCache) {
    if (!mFlinger->callingThreadHasUnscopedSurfaceFlingerAccess(usePermissionCache)) {
        IPCThreadState* ipc = IPCThreadState::self();
        ALOGE("Permission Denial: can't access SurfaceFlinger pid=%d, uid=%d", ipc->getCallingPid(),
              ipc->getCallingUid());
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkControlDisplayBrightnessPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && (uid != AID_SYSTEM) &&
        !PermissionCache::checkPermission(sControlDisplayBrightness, pid, uid)) {
        ALOGE("Permission Denial: can't control brightness pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkObservePictureProfilesPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if (!PermissionCache::checkPermission(sObservePictureProfiles, pid, uid)) {
        ALOGE("Permission Denial: can't manage picture profiles pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

void SurfaceFlinger::forceFutureUpdate(int delayInMs) {
    static_cast<void>(mScheduler->scheduleDelayed([&]() { scheduleRepaint(); }, ms2ns(delayInMs)));
}

const DisplayDevice* SurfaceFlinger::getDisplayFromLayerStack(ui::LayerStack layerStack) {
    for (const auto& [_, display] : mDisplays) {
        if (display->getLayerStack() == layerStack) {
            return display.get();
        }
    }
    return nullptr;
}

} // namespace android

#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif

#if defined(__gl2_h_)
#error "don't include gl2/gl2.h in this file"
#endif

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion -Wextra"
