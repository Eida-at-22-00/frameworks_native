/*
 * Copyright 2022 The Android Open Source Project
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

#include "../Macros.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/thread_annotations.h>
#include <android/input.h>
#include <com_android_input_flags.h>
#include <ftl/enum.h>
#include <input/AccelerationCurve.h>
#include <input/PrintTools.h>
#include <linux/input-event-codes.h>
#include <log/log_main.h>
#include <stats_pull_atom_callback.h>
#include <statslog.h>
#include "InputReaderBase.h"
#include "TouchCursorInputMapperCommon.h"
#include "TouchpadInputMapper.h"
#include "gestures/HardwareProperties.h"
#include "gestures/Logging.h"
#include "gestures/TimerProvider.h"
#include "ui/Rotation.h"

namespace input_flags = com::android::input::flags;

namespace android {

namespace {

std::vector<double> createAccelerationCurveForSensitivity(int32_t sensitivity,
                                                          bool accelerationEnabled,
                                                          size_t propertySize) {
    std::vector<AccelerationCurveSegment> segments = accelerationEnabled
            ? createAccelerationCurveForPointerSensitivity(sensitivity)
            : createFlatAccelerationCurve(sensitivity);
    LOG_ALWAYS_FATAL_IF(propertySize < 4 * segments.size());
    std::vector<double> output(propertySize, 0);

    // The Gestures library uses functions of the following form to define curve segments, where a,
    // b, and c can be specified by us:
    //     output_speed(input_speed_mm) = a * input_speed_mm ^ 2 + b * input_speed_mm + c
    //
    // (a, b, and c are also called sqr_, mul_, and int_ in the Gestures library code.)
    //
    // createAccelerationCurveForPointerSensitivity gives us parameters for a function of the form:
    //     gain(input_speed_mm) = baseGain + reciprocal / input_speed_mm
    // Where "gain" is a multiplier applied to the input speed to produce the output speed:
    //     output_speed(input_speed_mm) = input_speed_mm * gain(input_speed_mm)
    //
    // To put our function in the library's form, we substitute it into the function above:
    //     output_speed(input_speed_mm) = input_speed_mm * (baseGain + reciprocal / input_speed_mm)
    // then expand the brackets so that input_speed_mm cancels out for the reciprocal term:
    //     gain(input_speed_mm) = baseGain * input_speed_mm + reciprocal
    //
    // This gives us the following parameters for the Gestures library function form:
    //     a = 0
    //     b = baseGain
    //     c = reciprocal

    size_t i = 0;
    for (AccelerationCurveSegment seg : segments) {
        // The library's curve format consists of four doubles per segment:
        // * maximum pointer speed for the segment (mm/s)
        // * multiplier for the x² term (a.k.a. "a" or "sqr")
        // * multiplier for the x term (a.k.a. "b" or "mul")
        // * the intercept (a.k.a. "c" or "int")
        // (see struct CurveSegment in the library's AccelFilterInterpreter)
        output[i + 0] = seg.maxPointerSpeedMmPerS;
        output[i + 1] = 0;
        output[i + 2] = seg.baseGain;
        output[i + 3] = seg.reciprocal;
        i += 4;
    }

    return output;
}

void gestureInterpreterCallback(void* clientData, const Gesture* gesture) {
    TouchpadInputMapper* mapper = static_cast<TouchpadInputMapper*>(clientData);
    mapper->consumeGesture(gesture);
}

int32_t linuxBusToInputDeviceBusEnum(int32_t linuxBus, bool isUsiStylus) {
    if (isUsiStylus) {
        // This is a stylus connected over the Universal Stylus Initiative (USI) protocol.
        // For metrics purposes, we treat this protocol as a separate bus.
        return util::INPUT_DEVICE_USAGE_REPORTED__DEVICE_BUS__USI;
    }

    // When adding cases to this switch, also add them to the copy of this method in
    // InputDeviceMetricsCollector.cpp.
    // TODO(b/286394420): deduplicate this method with the one in InputDeviceMetricsCollector.cpp.
    switch (linuxBus) {
        case BUS_USB:
            return util::INPUT_DEVICE_USAGE_REPORTED__DEVICE_BUS__USB;
        case BUS_BLUETOOTH:
            return util::INPUT_DEVICE_USAGE_REPORTED__DEVICE_BUS__BLUETOOTH;
        default:
            return util::INPUT_DEVICE_USAGE_REPORTED__DEVICE_BUS__OTHER;
    }
}

class MetricsAccumulator {
public:
    static MetricsAccumulator& getInstance() {
        static MetricsAccumulator sAccumulator;
        return sAccumulator;
    }

    void recordFinger(const TouchpadInputMapper::MetricsIdentifier& id) {
        std::scoped_lock lock(mLock);
        mCounters[id].fingers++;
    }

    void recordPalm(const TouchpadInputMapper::MetricsIdentifier& id) {
        std::scoped_lock lock(mLock);
        mCounters[id].palms++;
    }

    // Checks whether a Gesture struct is for the end of a gesture that we log metrics for, and
    // records it if so.
    void processGesture(const TouchpadInputMapper::MetricsIdentifier& id, const Gesture& gesture) {
        std::scoped_lock lock(mLock);
        Counters& counters = mCounters[id];
        switch (gesture.type) {
            case kGestureTypeFling:
                if (gesture.details.fling.fling_state == GESTURES_FLING_START) {
                    // Indicates the end of a two-finger scroll gesture.
                    counters.twoFingerSwipeGestures++;
                }
                break;
            case kGestureTypeSwipeLift:
                // The Gestures library occasionally outputs two lift gestures in a row, which can
                // cause inaccurate metrics reporting. To work around this, deduplicate successive
                // lift gestures.
                // TODO(b/404529050): fix the Gestures library, and remove this check.
                if (counters.lastGestureType != kGestureTypeSwipeLift) {
                    counters.threeFingerSwipeGestures++;
                }
                break;
            case kGestureTypeFourFingerSwipeLift:
                // TODO(b/404529050): fix the Gestures library, and remove this check.
                if (counters.lastGestureType != kGestureTypeFourFingerSwipeLift) {
                    counters.fourFingerSwipeGestures++;
                }
                break;
            case kGestureTypePinch:
                if (gesture.details.pinch.zoom_state == GESTURES_ZOOM_END) {
                    counters.pinchGestures++;
                }
                break;
            default:
                // We're not interested in any other gestures.
                break;
        }
        counters.lastGestureType = gesture.type;
    }

private:
    MetricsAccumulator() {
        AStatsManager_setPullAtomCallback(android::util::TOUCHPAD_USAGE, /*metadata=*/nullptr,
                                          MetricsAccumulator::pullAtomCallback, /*cookie=*/nullptr);
    }

    ~MetricsAccumulator() { AStatsManager_clearPullAtomCallback(android::util::TOUCHPAD_USAGE); }

    static AStatsManager_PullAtomCallbackReturn pullAtomCallback(int32_t atomTag,
                                                                 AStatsEventList* outEventList,
                                                                 void* cookie) {
        LOG_ALWAYS_FATAL_IF(atomTag != android::util::TOUCHPAD_USAGE);
        MetricsAccumulator& accumulator = MetricsAccumulator::getInstance();
        accumulator.produceAtomsAndReset(*outEventList);
        return AStatsManager_PULL_SUCCESS;
    }

    void produceAtomsAndReset(AStatsEventList& outEventList) {
        std::scoped_lock lock(mLock);
        produceAtomsLocked(outEventList);
        resetCountersLocked();
    }

    void produceAtomsLocked(AStatsEventList& outEventList) const REQUIRES(mLock) {
        for (auto& [id, counters] : mCounters) {
            auto [busId, vendorId, productId, versionId] = id;
            addAStatsEvent(&outEventList, android::util::TOUCHPAD_USAGE, vendorId, productId,
                           versionId, linuxBusToInputDeviceBusEnum(busId, /*isUsi=*/false),
                           counters.fingers, counters.palms, counters.twoFingerSwipeGestures,
                           counters.threeFingerSwipeGestures, counters.fourFingerSwipeGestures,
                           counters.pinchGestures);
        }
    }

    void resetCountersLocked() REQUIRES(mLock) { mCounters.clear(); }

    // Stores the counters for a specific touchpad model. Fields have the same meanings as those of
    // the TouchpadUsage atom; see that definition for detailed documentation.
    struct Counters {
        int32_t fingers = 0;
        int32_t palms = 0;

        int32_t twoFingerSwipeGestures = 0;
        int32_t threeFingerSwipeGestures = 0;
        int32_t fourFingerSwipeGestures = 0;
        int32_t pinchGestures = 0;

        // Records the last type of gesture received for this device, for deduplication purposes.
        // TODO(b/404529050): fix the Gestures library and remove this field.
        GestureType lastGestureType = kGestureTypeContactInitiated;
    };

    // Metrics are aggregated by device model and version, so if two devices of the same model and
    // version are connected at once, they will have the same counters.
    std::map<TouchpadInputMapper::MetricsIdentifier, Counters> mCounters GUARDED_BY(mLock);

    // Metrics are pulled by a binder thread, so we need to guard them with a mutex.
    mutable std::mutex mLock;
};

} // namespace

TouchpadInputMapper::TouchpadInputMapper(InputDeviceContext& deviceContext,
                                         const InputReaderConfiguration& readerConfig)
      : InputMapper(deviceContext, readerConfig),
        mGestureInterpreter(NewGestureInterpreter(), DeleteGestureInterpreter),
        mTimerProvider(*getContext()),
        mStateConverter(deviceContext, mMotionAccumulator),
        mGestureConverter(*getContext(), deviceContext, getDeviceId()),
        mCapturedEventConverter(*getContext(), deviceContext, mMotionAccumulator, getDeviceId()),
        mMetricsId(metricsIdFromInputDeviceIdentifier(deviceContext.getDeviceIdentifier())) {
    if (std::optional<RawAbsoluteAxisInfo> slotAxis =
                deviceContext.getAbsoluteAxisInfo(ABS_MT_SLOT);
        slotAxis && slotAxis->maxValue >= 0) {
        mMotionAccumulator.configure(deviceContext, slotAxis->maxValue + 1, true);
    } else {
        LOG(WARNING) << "Touchpad " << deviceContext.getName()
                     << " doesn't have a valid ABS_MT_SLOT axis, and probably won't work properly.";
        mMotionAccumulator.configure(deviceContext, 1, true);
    }

    mGestureInterpreter->Initialize(GESTURES_DEVCLASS_TOUCHPAD);
    mHardwareProperties = createHardwareProperties(deviceContext);
    mGestureInterpreter->SetHardwareProperties(mHardwareProperties);
    // Even though we don't explicitly delete copy/move semantics, it's safe to
    // give away pointers to TouchpadInputMapper and its members here because
    // 1) mGestureInterpreter's lifecycle is determined by TouchpadInputMapper, and
    // 2) TouchpadInputMapper is stored as a unique_ptr and not moved.
    mGestureInterpreter->SetPropProvider(const_cast<GesturesPropProvider*>(&gesturePropProvider),
                                         &mPropertyProvider);
    mGestureInterpreter->SetTimerProvider(const_cast<GesturesTimerProvider*>(
                                                  &kGestureTimerProvider),
                                          &mTimerProvider);
    mGestureInterpreter->SetCallback(gestureInterpreterCallback, this);
}

TouchpadInputMapper::~TouchpadInputMapper() {
    // The gesture interpreter's destructor will try to free its property and timer providers,
    // calling PropertyProvider::freeProperty and TimerProvider::freeTimer using a raw pointers.
    // Depending on the declaration order in TouchpadInputMapper.h, those providers may have already
    // been freed, causing allocation errors or use-after-free bugs. Depending on declaration order
    // to avoid this seems rather fragile, so explicitly clear the providers here to ensure all the
    // freeProperty and freeTimer calls happen before the providers are destructed.
    mGestureInterpreter->SetPropProvider(nullptr, nullptr);
    mGestureInterpreter->SetTimerProvider(nullptr, nullptr);
}

uint32_t TouchpadInputMapper::getSources() const {
    return AINPUT_SOURCE_MOUSE | AINPUT_SOURCE_TOUCHPAD;
}

void TouchpadInputMapper::populateDeviceInfo(InputDeviceInfo& info) {
    InputMapper::populateDeviceInfo(info);
    if (mPointerCaptured) {
        mCapturedEventConverter.populateMotionRanges(info);
    } else {
        mGestureConverter.populateMotionRanges(info);
    }
}

void TouchpadInputMapper::dump(std::string& dump) {
    dump += INDENT2 "Touchpad Input Mapper:\n";
    if (mResettingInterpreter) {
        dump += INDENT3 "Currently resetting gesture interpreter\n";
    }
    dump += StringPrintf(INDENT3 "Pointer captured: %s\n", toString(mPointerCaptured));
    dump += INDENT3 "Gesture converter:\n";
    dump += addLinePrefix(mGestureConverter.dump(), INDENT4);
    dump += INDENT3 "Gesture properties:\n";
    dump += addLinePrefix(mPropertyProvider.dump(), INDENT4);
    dump += INDENT3 "Timer provider:\n";
    dump += addLinePrefix(mTimerProvider.dump(), INDENT4);
    dump += INDENT3 "Captured event converter:\n";
    dump += addLinePrefix(mCapturedEventConverter.dump(), INDENT4);
    dump += StringPrintf(INDENT3 "DisplayId: %s\n",
                         toString(mDisplayId, streamableToString).c_str());
}

std::list<NotifyArgs> TouchpadInputMapper::reconfigure(nsecs_t when,
                                                       const InputReaderConfiguration& config,
                                                       ConfigurationChanges changes) {
    if (!changes.any()) {
        // First time configuration
        mPropertyProvider.loadPropertiesFromIdcFile(getDeviceContext().getConfiguration());
    }

    if (!changes.any() || changes.test(InputReaderConfiguration::Change::DISPLAY_INFO)) {
        mDisplayId = ui::LogicalDisplayId::INVALID;
        std::optional<DisplayViewport> resolvedViewport;
        std::optional<FloatRect> boundsInLogicalDisplay;
        if (auto assocViewport = mDeviceContext.getAssociatedViewport(); assocViewport) {
            // This InputDevice is associated with a viewport.
            // Only generate events for the associated display.
            mDisplayId = assocViewport->displayId;
            resolvedViewport = *assocViewport;
        } else {
            // The InputDevice is not associated with a viewport, but it controls the mouse pointer.
            // Always use DISPLAY_ID_NONE for touchpad events.
            // PointerChoreographer will make it target the correct the displayId later.
            resolvedViewport = getContext()->getPolicy()->getPointerViewportForAssociatedDisplay();
            mDisplayId = resolvedViewport ? std::make_optional(ui::LogicalDisplayId::INVALID)
                                          : std::nullopt;
        }

        mGestureConverter.setDisplayId(mDisplayId);
        mGestureConverter.setOrientation(resolvedViewport
                                                 ? getInverseRotation(resolvedViewport->orientation)
                                                 : ui::ROTATION_0);

        if (!boundsInLogicalDisplay) {
            boundsInLogicalDisplay = resolvedViewport
                    ? FloatRect{static_cast<float>(resolvedViewport->logicalLeft),
                                static_cast<float>(resolvedViewport->logicalTop),
                                static_cast<float>(resolvedViewport->logicalRight - 1),
                                static_cast<float>(resolvedViewport->logicalBottom - 1)}
                    : FloatRect{0, 0, 0, 0};
        }
        mGestureConverter.setBoundsInLogicalDisplay(*boundsInLogicalDisplay);

        bumpGeneration();
    }
    std::list<NotifyArgs> out;
    if (!changes.any() || changes.test(InputReaderConfiguration::Change::TOUCHPAD_SETTINGS)) {
        mPropertyProvider.getProperty("Use Custom Touchpad Pointer Accel Curve")
                .setBoolValues({true});
        GesturesProp accelCurveProp = mPropertyProvider.getProperty("Pointer Accel Curve");
        accelCurveProp.setRealValues(
                createAccelerationCurveForSensitivity(config.touchpadPointerSpeed,
                                                      config.touchpadAccelerationEnabled,
                                                      accelCurveProp.getCount()));
        mPropertyProvider.getProperty("Use Custom Touchpad Scroll Accel Curve")
                .setBoolValues({true});
        GesturesProp scrollCurveProp = mPropertyProvider.getProperty("Scroll Accel Curve");
        scrollCurveProp.setRealValues(
                createAccelerationCurveForSensitivity(config.touchpadPointerSpeed,
                                                      config.touchpadAccelerationEnabled,
                                                      scrollCurveProp.getCount()));
        mPropertyProvider.getProperty("Scroll X Out Scale").setRealValues({1.0});
        mPropertyProvider.getProperty("Scroll Y Out Scale").setRealValues({1.0});
        mPropertyProvider.getProperty("Invert Scrolling")
                .setBoolValues({config.touchpadNaturalScrollingEnabled});
        mPropertyProvider.getProperty("Tap Enable")
                .setBoolValues({config.touchpadTapToClickEnabled});
        mPropertyProvider.getProperty("Tap Drag Enable")
                .setBoolValues({config.touchpadTapDraggingEnabled});
        mPropertyProvider.getProperty("Button Right Click Zone Enable")
                .setBoolValues({config.touchpadRightClickZoneEnabled});
        mTouchpadHardwareStateNotificationsEnabled = config.shouldNotifyTouchpadHardwareState;
        mGestureConverter.setThreeFingerTapShortcutEnabled(
                config.touchpadThreeFingerTapShortcutEnabled);
        out += mGestureConverter.setEnableSystemGestures(when,
                                                         config.touchpadSystemGesturesEnabled);
    }
    if ((!changes.any() && config.pointerCaptureRequest.isEnable()) ||
        changes.test(InputReaderConfiguration::Change::POINTER_CAPTURE)) {
        mPointerCaptured = config.pointerCaptureRequest.isEnable();
        // The motion ranges are going to change, so bump the generation to clear the cached ones.
        bumpGeneration();
        if (mPointerCaptured) {
            // The touchpad is being captured, so we need to tidy up any fake fingers etc. that are
            // still being reported for a gesture in progress.
            out += reset(when);
        } else {
            // We're transitioning from captured to uncaptured.
            mCapturedEventConverter.reset();
        }
        if (changes.any()) {
            out.push_back(NotifyDeviceResetArgs(getContext()->getNextId(), when, getDeviceId()));
        }
    }
    return out;
}

std::list<NotifyArgs> TouchpadInputMapper::reset(nsecs_t when) {
    mStateConverter.reset();
    resetGestureInterpreter(when);
    std::list<NotifyArgs> out = mGestureConverter.reset(when);
    out += InputMapper::reset(when);
    return out;
}

void TouchpadInputMapper::resetGestureInterpreter(nsecs_t when) {
    // The GestureInterpreter has no official reset method, but sending a HardwareState with no
    // fingers down or buttons pressed should get it into a clean state.
    HardwareState state;
    state.timestamp = std::chrono::duration<stime_t>(std::chrono::nanoseconds(when)).count();
    mResettingInterpreter = true;
    mGestureInterpreter->PushHardwareState(&state);
    mResettingInterpreter = false;
}

std::list<NotifyArgs> TouchpadInputMapper::process(const RawEvent& rawEvent) {
    if (mPointerCaptured) {
        return mCapturedEventConverter.process(rawEvent);
    }
    if (mMotionAccumulator.getActiveSlotsCount() == 0) {
        mGestureStartTime = rawEvent.when;
    }
    std::optional<SelfContainedHardwareState> state = mStateConverter.processRawEvent(rawEvent);
    if (state) {
        if (mTouchpadHardwareStateNotificationsEnabled) {
            getPolicy()->notifyTouchpadHardwareState(*state, getDeviceId());
        }
        updatePalmDetectionMetrics();
        return sendHardwareState(rawEvent.when, rawEvent.readTime, *state);
    } else {
        return {};
    }
}

void TouchpadInputMapper::updatePalmDetectionMetrics() {
    std::set<int32_t> currentTrackingIds;
    for (size_t i = 0; i < mMotionAccumulator.getSlotCount(); i++) {
        const MultiTouchMotionAccumulator::Slot& slot = mMotionAccumulator.getSlot(i);
        if (!slot.isInUse()) {
            continue;
        }
        currentTrackingIds.insert(slot.getTrackingId());
        if (slot.getToolType() == ToolType::PALM) {
            mPalmTrackingIds.insert(slot.getTrackingId());
        }
    }
    std::vector<int32_t> liftedTouches;
    std::set_difference(mLastFrameTrackingIds.begin(), mLastFrameTrackingIds.end(),
                        currentTrackingIds.begin(), currentTrackingIds.end(),
                        std::inserter(liftedTouches, liftedTouches.begin()));
    for (int32_t trackingId : liftedTouches) {
        if (mPalmTrackingIds.erase(trackingId) > 0) {
            MetricsAccumulator::getInstance().recordPalm(mMetricsId);
        } else {
            MetricsAccumulator::getInstance().recordFinger(mMetricsId);
        }
    }
    mLastFrameTrackingIds = currentTrackingIds;
}

std::list<NotifyArgs> TouchpadInputMapper::sendHardwareState(nsecs_t when, nsecs_t readTime,
                                                             SelfContainedHardwareState schs) {
    ALOGD_IF(debugTouchpadGestures(), "New hardware state: %s", schs.state.String().c_str());
    mGestureInterpreter->PushHardwareState(&schs.state);
    return processGestures(when, readTime);
}

std::list<NotifyArgs> TouchpadInputMapper::timeoutExpired(nsecs_t when) {
    mTimerProvider.triggerCallbacks(when);
    return processGestures(when, when);
}

void TouchpadInputMapper::consumeGesture(const Gesture* gesture) {
    ALOGD_IF(debugTouchpadGestures(), "Gesture ready: %s", gesture->String().c_str());
    if (mResettingInterpreter) {
        // We already handle tidying up fake fingers etc. in GestureConverter::reset, so we should
        // ignore any gestures produced from the interpreter while we're resetting it.
        return;
    }
    mGesturesToProcess.push_back(*gesture);
    if (mTouchpadHardwareStateNotificationsEnabled) {
        getPolicy()->notifyTouchpadGestureInfo(gesture->type, getDeviceId());
    }
}

std::list<NotifyArgs> TouchpadInputMapper::processGestures(nsecs_t when, nsecs_t readTime) {
    std::list<NotifyArgs> out = {};
    if (mDisplayId) {
        MetricsAccumulator& metricsAccumulator = MetricsAccumulator::getInstance();
        for (Gesture& gesture : mGesturesToProcess) {
            out += mGestureConverter.handleGesture(when, readTime, mGestureStartTime, gesture);
            metricsAccumulator.processGesture(mMetricsId, gesture);
        }
    }
    mGesturesToProcess.clear();
    return out;
}

std::optional<ui::LogicalDisplayId> TouchpadInputMapper::getAssociatedDisplayId() const {
    return mDisplayId;
}

std::optional<HardwareProperties> TouchpadInputMapper::getTouchpadHardwareProperties() {
    return mHardwareProperties;
}

std::optional<GesturesProp> TouchpadInputMapper::getGesturePropertyForTesting(
        const std::string& name) {
    if (!mPropertyProvider.hasProperty(name)) {
        return std::nullopt;
    }
    return mPropertyProvider.getProperty(name);
}

} // namespace android
