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

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <InputDevice.h>
#include <InputReaderBase.h>

#include "input/DisplayViewport.h"
#include "input/InputDevice.h"

namespace android {

DisplayViewport createViewport(ui::LogicalDisplayId displayId, int32_t width, int32_t height,
                               ui::Rotation orientation, bool isActive, const std::string& uniqueId,
                               std::optional<uint8_t> physicalPort, ViewportType type);

class FakeInputReaderPolicy : public InputReaderPolicyInterface {
protected:
    virtual ~FakeInputReaderPolicy() {}

public:
    FakeInputReaderPolicy() {}

    void assertInputDevicesChanged();
    void assertInputDevicesNotChanged();
    void assertStylusGestureNotified(int32_t deviceId);
    void assertStylusGestureNotNotified();
    void assertTouchpadHardwareStateNotified();
    void assertTouchpadThreeFingerTapNotified();

    virtual void clearViewports();
    std::optional<DisplayViewport> getDisplayViewportByUniqueId(const std::string& uniqueId) const;
    std::optional<DisplayViewport> getDisplayViewportByType(ViewportType type) const;
    std::optional<DisplayViewport> getDisplayViewportByPort(uint8_t displayPort) const;
    void addDisplayViewport(DisplayViewport viewport);
    bool updateViewport(const DisplayViewport& viewport);
    void addExcludedDeviceName(const std::string& deviceName);
    void addInputPortAssociation(const std::string& inputPort, uint8_t displayPort);
    void addDeviceTypeAssociation(const std::string& inputPort, const std::string& type);
    void addInputUniqueIdAssociation(const std::string& inputUniqueId,
                                     const std::string& displayUniqueId);
    void addKeyboardLayoutAssociation(const std::string& inputUniqueId,
                                      const KeyboardLayoutInfo& layoutInfo);
    void addDisabledDevice(int32_t deviceId);
    void removeDisabledDevice(int32_t deviceId);
    const InputReaderConfiguration& getReaderConfiguration() const;
    const std::vector<InputDeviceInfo> getInputDevices() const;
    TouchAffineTransformation getTouchAffineTransformation(const std::string& inputDeviceDescriptor,
                                                           ui::Rotation surfaceRotation);
    void setTouchAffineTransformation(const TouchAffineTransformation t);
    PointerCaptureRequest setPointerCapture(const sp<IBinder>& window);
    void setDefaultPointerDisplayId(ui::LogicalDisplayId pointerDisplayId);
    void setPointerGestureEnabled(bool enabled);
    float getPointerGestureMovementSpeedRatio();
    float getPointerGestureZoomSpeedRatio();
    void setVelocityControlParams(const VelocityControlParameters& params);
    void setStylusButtonMotionEventsEnabled(bool enabled);
    void setStylusPointerIconEnabled(bool enabled);
    void setIsInputMethodConnectionActive(bool active);
    bool isInputMethodConnectionActive() override;
    std::optional<DisplayViewport> getPointerViewportForAssociatedDisplay(
            ui::LogicalDisplayId associatedDisplayId) override;

private:
    void getReaderConfiguration(InputReaderConfiguration* outConfig) override;
    void notifyInputDevicesChanged(const std::vector<InputDeviceInfo>& inputDevices) override;
    void notifyTouchpadHardwareState(const SelfContainedHardwareState& schs,
                                     int32_t deviceId) override;
    void notifyTouchpadGestureInfo(GestureType type, int32_t deviceId) override;
    void notifyTouchpadThreeFingerTap() override;
    std::shared_ptr<KeyCharacterMap> getKeyboardLayoutOverlay(
            const InputDeviceIdentifier&, const std::optional<KeyboardLayoutInfo>) override;
    std::string getDeviceAlias(const InputDeviceIdentifier&) override;
    void waitForInputDevices(std::function<void(bool)> processDevicesChanged,
                             std::chrono::milliseconds timeout);
    void notifyStylusGestureStarted(int32_t deviceId, nsecs_t eventTime) override;

    mutable std::mutex mLock;
    std::condition_variable mDevicesChangedCondition;

    InputReaderConfiguration mConfig;
    std::vector<InputDeviceInfo> mInputDevices GUARDED_BY(mLock);
    bool mInputDevicesChanged GUARDED_BY(mLock){false};
    std::vector<DisplayViewport> mViewports;
    TouchAffineTransformation transform;
    bool mIsInputMethodConnectionActive{false};

    std::condition_variable mStylusGestureNotifiedCondition;
    std::optional<DeviceId> mDeviceIdOfNotifiedStylusGesture GUARDED_BY(mLock){};

    std::condition_variable mTouchpadHardwareStateNotified;
    std::optional<SelfContainedHardwareState> mTouchpadHardwareState GUARDED_BY(mLock){};

    std::condition_variable mTouchpadThreeFingerTapNotified;
    bool mTouchpadThreeFingerTapHasBeenReported{false};

    uint32_t mNextPointerCaptureSequenceNumber{0};
};

} // namespace android
