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

#pragma once

#include <list>
#include <memory>
#include <set>
#include <vector>

#include <PointerControllerInterface.h>
#include <utils/Timers.h>

#include "CapturedTouchpadEventConverter.h"
#include "EventHub.h"
#include "InputDevice.h"
#include "InputMapper.h"
#include "InputReaderBase.h"
#include "NotifyArgs.h"
#include "accumulator/MultiTouchMotionAccumulator.h"
#include "gestures/GestureConverter.h"
#include "gestures/HardwareStateConverter.h"
#include "gestures/PropertyProvider.h"
#include "gestures/TimerProvider.h"

#include "include/gestures.h"

namespace android {

class TouchpadInputMapper : public InputMapper {
public:
    template <class T, class... Args>
    friend std::unique_ptr<T> createInputMapper(InputDeviceContext& deviceContext,
                                                const InputReaderConfiguration& readerConfig,
                                                Args... args);
    ~TouchpadInputMapper();

    uint32_t getSources() const override;
    void populateDeviceInfo(InputDeviceInfo& deviceInfo) override;
    void dump(std::string& dump) override;

    [[nodiscard]] std::list<NotifyArgs> reconfigure(nsecs_t when,
                                                    const InputReaderConfiguration& config,
                                                    ConfigurationChanges changes) override;
    [[nodiscard]] std::list<NotifyArgs> reset(nsecs_t when) override;
    [[nodiscard]] std::list<NotifyArgs> process(const RawEvent& rawEvent) override;
    [[nodiscard]] std::list<NotifyArgs> timeoutExpired(nsecs_t when) override;

    void consumeGesture(const Gesture* gesture);

    // A subset of InputDeviceIdentifier used for logging metrics, to avoid storing a copy of the
    // strings in that bigger struct.
    using MetricsIdentifier = std::tuple<uint16_t /*busId*/, uint16_t /*vendorId*/,
                                         uint16_t /*productId*/, uint16_t /*version*/>;

    std::optional<ui::LogicalDisplayId> getAssociatedDisplayId() const override;

    std::optional<HardwareProperties> getTouchpadHardwareProperties() override;

    std::optional<GesturesProp> getGesturePropertyForTesting(const std::string& name);

private:
    void resetGestureInterpreter(nsecs_t when);
    explicit TouchpadInputMapper(InputDeviceContext& deviceContext,
                                 const InputReaderConfiguration& readerConfig);
    void updatePalmDetectionMetrics();
    [[nodiscard]] std::list<NotifyArgs> sendHardwareState(nsecs_t when, nsecs_t readTime,
                                                          SelfContainedHardwareState schs);
    [[nodiscard]] std::list<NotifyArgs> processGestures(nsecs_t when, nsecs_t readTime);

    std::unique_ptr<gestures::GestureInterpreter, void (*)(gestures::GestureInterpreter*)>
            mGestureInterpreter;

    PropertyProvider mPropertyProvider;
    TimerProvider mTimerProvider;

    // The MultiTouchMotionAccumulator is shared between the HardwareStateConverter and
    // CapturedTouchpadEventConverter, so that if the touchpad is captured or released while touches
    // are down, the relevant converter can still benefit from the current axis values stored in the
    // accumulator.
    MultiTouchMotionAccumulator mMotionAccumulator;

    HardwareStateConverter mStateConverter;
    GestureConverter mGestureConverter;
    CapturedTouchpadEventConverter mCapturedEventConverter;
    HardwareProperties mHardwareProperties;

    bool mPointerCaptured = false;
    bool mResettingInterpreter = false;
    std::vector<Gesture> mGesturesToProcess;

    static MetricsIdentifier metricsIdFromInputDeviceIdentifier(const InputDeviceIdentifier& id) {
        return std::make_tuple(id.bus, id.vendor, id.product, id.version);
    }
    const MetricsIdentifier mMetricsId;
    // Tracking IDs for touches on the pad in the last evdev frame.
    std::set<int32_t> mLastFrameTrackingIds;
    // Tracking IDs for touches that have at some point been reported as palms by the touchpad.
    std::set<int32_t> mPalmTrackingIds;

    // The display that events generated by this mapper should target. This can be set to
    // LogicalDisplayId::INVALID to target the focused display. If there is no display target (i.e.
    // std::nullopt), all events will be ignored.
    std::optional<ui::LogicalDisplayId> mDisplayId;

    nsecs_t mGestureStartTime{0};

    // True if hardware state update notifications is available for usage based on its feature flag
    // and settings value.
    bool mTouchpadHardwareStateNotificationsEnabled = false;
};

} // namespace android
