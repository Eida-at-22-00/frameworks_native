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

#include <map>
#include <memory>
#include <optional>

#include <EventHub.h>
#include <InputDevice.h>
#include <InputMapper.h>
#include <InputReader.h>
#include <ThreadSafeFuzzedDataProvider.h>

constexpr size_t kValidTypes[] = {EV_SW,
                                  EV_SYN,
                                  EV_ABS,
                                  EV_KEY,
                                  EV_MSC,
                                  EV_REL,
                                  android::EventHubInterface::DEVICE_ADDED,
                                  android::EventHubInterface::DEVICE_REMOVED};

static const android::InputDeviceClass kInputDeviceClasses[] = {
        android::InputDeviceClass::KEYBOARD,
        android::InputDeviceClass::ALPHAKEY,
        android::InputDeviceClass::TOUCH,
        android::InputDeviceClass::CURSOR,
        android::InputDeviceClass::TOUCH_MT,
        android::InputDeviceClass::DPAD,
        android::InputDeviceClass::GAMEPAD,
        android::InputDeviceClass::SWITCH,
        android::InputDeviceClass::JOYSTICK,
        android::InputDeviceClass::VIBRATOR,
        android::InputDeviceClass::MIC,
        android::InputDeviceClass::EXTERNAL_STYLUS,
        android::InputDeviceClass::ROTARY_ENCODER,
        android::InputDeviceClass::SENSOR,
        android::InputDeviceClass::BATTERY,
        android::InputDeviceClass::LIGHT,
        android::InputDeviceClass::TOUCHPAD,
        android::InputDeviceClass::VIRTUAL,
        android::InputDeviceClass::EXTERNAL,
};

constexpr size_t kValidCodes[] = {
        SYN_REPORT,
        ABS_MT_SLOT,
        SYN_MT_REPORT,
        ABS_MT_POSITION_X,
        ABS_MT_POSITION_Y,
        ABS_MT_TOUCH_MAJOR,
        ABS_MT_TOUCH_MINOR,
        ABS_MT_WIDTH_MAJOR,
        ABS_MT_WIDTH_MINOR,
        ABS_MT_ORIENTATION,
        ABS_MT_TRACKING_ID,
        ABS_MT_PRESSURE,
        ABS_MT_DISTANCE,
        ABS_MT_TOOL_TYPE,
        MSC_SCAN,
        REL_X,
        REL_Y,
        REL_WHEEL,
        REL_HWHEEL,
        BTN_LEFT,
        BTN_RIGHT,
        BTN_MIDDLE,
        BTN_BACK,
        BTN_SIDE,
        BTN_FORWARD,
        BTN_EXTRA,
        BTN_TASK,
};

constexpr size_t kMaxSize = 256;

namespace android {

template<class Fdp>
ToolType getFuzzedToolType(Fdp& fdp) {
    const int32_t toolType = fdp.template ConsumeIntegralInRange<int32_t>(
                            static_cast<int32_t>(ToolType::ftl_first),
                            static_cast<int32_t>(ToolType::ftl_last));
    return static_cast<ToolType>(toolType);
}

template <class Fdp>
RawEvent getFuzzedRawEvent(Fdp& fdp) {
    const int32_t type = fdp.ConsumeBool() ? fdp.PickValueInArray(kValidTypes)
                                           : fdp.template ConsumeIntegral<int32_t>();
    const int32_t code = fdp.ConsumeBool() ? fdp.PickValueInArray(kValidCodes)
                                           : fdp.template ConsumeIntegral<int32_t>();
    return RawEvent{
            .when = fdp.template ConsumeIntegral<nsecs_t>(),
            .readTime = fdp.template ConsumeIntegral<nsecs_t>(),
            .deviceId = fdp.template ConsumeIntegral<int32_t>(),
            .type = type,
            .code = code,
            .value = fdp.template ConsumeIntegral<int32_t>(),
    };
}

class FuzzEventHub : public EventHubInterface {
    InputDeviceIdentifier mIdentifier;
    std::vector<TouchVideoFrame> mVideoFrames;
    PropertyMap mFuzzConfig;
    std::map<int32_t /* deviceId */, std::map<int /* axis */, RawAbsoluteAxisInfo>> mAxes;
    std::shared_ptr<ThreadSafeFuzzedDataProvider> mFdp;

public:
    FuzzEventHub(std::shared_ptr<ThreadSafeFuzzedDataProvider> fdp) : mFdp(std::move(fdp)) {}
    ~FuzzEventHub() {}
    void addProperty(std::string key, std::string value) { mFuzzConfig.addProperty(key, value); }

    ftl::Flags<InputDeviceClass> getDeviceClasses(int32_t deviceId) const override {
        uint32_t flags = 0;
        for (auto inputDeviceClass : kInputDeviceClasses) {
            if (mFdp->ConsumeBool()) {
                flags |= static_cast<uint32_t>(inputDeviceClass);
            }
        }
        return ftl::Flags<InputDeviceClass>(flags);
    }
    InputDeviceIdentifier getDeviceIdentifier(int32_t deviceId) const override {
        return mIdentifier;
    }
    int32_t getDeviceControllerNumber(int32_t deviceId) const override {
        return mFdp->ConsumeIntegral<int32_t>();
    }
    std::optional<PropertyMap> getConfiguration(int32_t deviceId) const override {
        return mFuzzConfig;
    }
    void setAbsoluteAxisInfo(int32_t deviceId, int axis, const RawAbsoluteAxisInfo& axisInfo) {
        mAxes[deviceId][axis] = axisInfo;
    }
    std::optional<RawAbsoluteAxisInfo> getAbsoluteAxisInfo(int32_t deviceId,
                                                           int axis) const override {
        if (auto deviceAxesIt = mAxes.find(deviceId); deviceAxesIt != mAxes.end()) {
            const std::map<int, RawAbsoluteAxisInfo>& deviceAxes = deviceAxesIt->second;
            if (auto axisInfoIt = deviceAxes.find(axis); axisInfoIt != deviceAxes.end()) {
                return axisInfoIt->second;
            }
        }
        if (mFdp->ConsumeBool()) {
            return std::optional<RawAbsoluteAxisInfo>({
                    .minValue = mFdp->ConsumeIntegral<int32_t>(),
                    .maxValue = mFdp->ConsumeIntegral<int32_t>(),
                    .flat = mFdp->ConsumeIntegral<int32_t>(),
                    .fuzz = mFdp->ConsumeIntegral<int32_t>(),
                    .resolution = mFdp->ConsumeIntegral<int32_t>(),
            });
        } else {
            return std::nullopt;
        }
    }
    bool hasRelativeAxis(int32_t deviceId, int axis) const override { return mFdp->ConsumeBool(); }
    bool hasInputProperty(int32_t deviceId, int property) const override {
        return mFdp->ConsumeBool();
    }
    bool hasMscEvent(int32_t deviceId, int mscEvent) const override { return mFdp->ConsumeBool(); }
    status_t mapKey(int32_t deviceId, int32_t scanCode, int32_t usageCode, int32_t metaState,
                    int32_t* outKeycode, int32_t* outMetaState, uint32_t* outFlags) const override {
        return mFdp->ConsumeIntegral<status_t>();
    }
    status_t mapAxis(int32_t deviceId, int32_t scanCode, AxisInfo* outAxisInfo) const override {
        return mFdp->ConsumeIntegral<status_t>();
    }
    void setExcludedDevices(const std::vector<std::string>& devices) override {}
    std::vector<RawEvent> getEvents(int timeoutMillis) override {
        std::vector<RawEvent> events;
        const size_t count = mFdp->ConsumeIntegralInRange<size_t>(0, kMaxSize);
        for (size_t i = 0; i < count; ++i) {
            events.push_back(getFuzzedRawEvent(*mFdp));
        }
        return events;
    }
    std::vector<TouchVideoFrame> getVideoFrames(int32_t deviceId) override { return mVideoFrames; }

    base::Result<std::pair<InputDeviceSensorType, int32_t>> mapSensor(
            int32_t deviceId, int32_t absCode) const override {
        return base::ResultError("Fuzzer", UNKNOWN_ERROR);
    };
    // Raw batteries are sysfs power_supply nodes we found from the EventHub device sysfs node,
    // containing the raw info of the sysfs node structure.
    std::vector<int32_t> getRawBatteryIds(int32_t deviceId) const override { return {}; }
    std::optional<RawBatteryInfo> getRawBatteryInfo(int32_t deviceId,
                                                    int32_t BatteryId) const override {
        return std::nullopt;
    };

    std::vector<int32_t> getRawLightIds(int32_t deviceId) const override { return {}; };
    std::optional<RawLightInfo> getRawLightInfo(int32_t deviceId, int32_t lightId) const override {
        return std::nullopt;
    };
    std::optional<int32_t> getLightBrightness(int32_t deviceId, int32_t lightId) const override {
        return std::nullopt;
    };
    void setLightBrightness(int32_t deviceId, int32_t lightId, int32_t brightness) override{};
    std::optional<std::unordered_map<LightColor, int32_t>> getLightIntensities(
            int32_t deviceId, int32_t lightId) const override {
        return std::nullopt;
    };
    void setLightIntensities(int32_t deviceId, int32_t lightId,
                             std::unordered_map<LightColor, int32_t> intensities) override{};

    std::optional<RawLayoutInfo> getRawLayoutInfo(int32_t deviceId) const override {
        return std::nullopt;
    };

    int32_t getScanCodeState(int32_t deviceId, int32_t scanCode) const override {
        return mFdp->ConsumeIntegral<int32_t>();
    }
    int32_t getKeyCodeState(int32_t deviceId, int32_t keyCode) const override {
        return mFdp->ConsumeIntegral<int32_t>();
    }
    int32_t getSwitchState(int32_t deviceId, int32_t sw) const override {
        return mFdp->ConsumeIntegral<int32_t>();
    }
    void setKeyRemapping(int32_t deviceId,
                         const std::map<int32_t, int32_t>& keyRemapping) const override {}
    int32_t getKeyCodeForKeyLocation(int32_t deviceId, int32_t locationKeyCode) const override {
        return mFdp->ConsumeIntegral<int32_t>();
    }
    std::optional<int32_t> getAbsoluteAxisValue(int32_t deviceId, int32_t axis) const override {
        if (mFdp->ConsumeBool()) {
            return mFdp->ConsumeIntegral<int32_t>();
        } else {
            return std::nullopt;
        }
    }
    base::Result<std::vector<int32_t>> getMtSlotValues(int32_t deviceId, int32_t axis,
                                                       size_t slotCount) const override {
        if (mFdp->ConsumeBool()) {
            std::vector<int32_t> outValues(slotCount + 1);
            for (size_t i = 0; i < outValues.size(); i++) {
                outValues.push_back(mFdp->ConsumeIntegral<int32_t>());
            }
            return std::move(outValues);
        } else {
            return base::ResultError("Fuzzer", UNKNOWN_ERROR);
        }
    }
    bool markSupportedKeyCodes(int32_t deviceId, const std::vector<int32_t>& keyCodes,
                               uint8_t* outFlags) const override {
        return mFdp->ConsumeBool();
    }
    bool hasScanCode(int32_t deviceId, int32_t scanCode) const override {
        return mFdp->ConsumeBool();
    }
    bool hasKeyCode(int32_t deviceId, int32_t keyCode) const override {
        return mFdp->ConsumeBool();
    }
    bool hasLed(int32_t deviceId, int32_t led) const override { return mFdp->ConsumeBool(); }
    void setLedState(int32_t deviceId, int32_t led, bool on) override {}
    void getVirtualKeyDefinitions(
            int32_t deviceId, std::vector<VirtualKeyDefinition>& outVirtualKeys) const override {}
    const std::shared_ptr<KeyCharacterMap> getKeyCharacterMap(int32_t deviceId) const override {
        return nullptr;
    }
    bool setKeyboardLayoutOverlay(int32_t deviceId, std::shared_ptr<KeyCharacterMap> map) override {
        return mFdp->ConsumeBool();
    }
    void vibrate(int32_t deviceId, const VibrationElement& effect) override {}
    void cancelVibrate(int32_t deviceId) override {}

    std::vector<int32_t> getVibratorIds(int32_t deviceId) const override { return {}; };

    /* Query battery level. */
    std::optional<int32_t> getBatteryCapacity(int32_t deviceId, int32_t batteryId) const override {
        return std::nullopt;
    };

    /* Query battery status. */
    std::optional<int32_t> getBatteryStatus(int32_t deviceId, int32_t batteryId) const override {
        return std::nullopt;
    };

    void requestReopenDevices() override {}
    void wake() override {}
    void dump(std::string& dump) const override {}
    void monitor() const override {}
    bool isDeviceEnabled(int32_t deviceId) const override { return mFdp->ConsumeBool(); }
    status_t enableDevice(int32_t deviceId) override { return mFdp->ConsumeIntegral<status_t>(); }
    status_t disableDevice(int32_t deviceId) override { return mFdp->ConsumeIntegral<status_t>(); }
    std::filesystem::path getSysfsRootPath(int32_t deviceId) const override { return {}; }
    void sysfsNodeChanged(const std::string& sysfsNodePath) override {}
    bool setKernelWakeEnabled(int32_t deviceId, bool enabled) override {
        return mFdp->ConsumeBool();
    }
};

class FuzzInputReaderPolicy : public InputReaderPolicyInterface {
    TouchAffineTransformation mTransform;
    std::shared_ptr<ThreadSafeFuzzedDataProvider> mFdp;

protected:
    ~FuzzInputReaderPolicy() {}

public:
    FuzzInputReaderPolicy(std::shared_ptr<ThreadSafeFuzzedDataProvider> mFdp) : mFdp(mFdp) {}
    void getReaderConfiguration(InputReaderConfiguration* outConfig) override {}
    void notifyInputDevicesChanged(const std::vector<InputDeviceInfo>& inputDevices) override {}
    void notifyTouchpadHardwareState(const SelfContainedHardwareState& schs,
                                     int32_t deviceId) override {}
    void notifyTouchpadGestureInfo(GestureType type, int32_t deviceId) override {}
    void notifyTouchpadThreeFingerTap() override {}
    std::shared_ptr<KeyCharacterMap> getKeyboardLayoutOverlay(
            const InputDeviceIdentifier& identifier,
            const std::optional<KeyboardLayoutInfo> layoutInfo) override {
        return nullptr;
    }
    std::string getDeviceAlias(const InputDeviceIdentifier& identifier) {
        return mFdp->ConsumeRandomLengthString(32);
    }
    TouchAffineTransformation getTouchAffineTransformation(const std::string& inputDeviceDescriptor,
                                                           ui::Rotation surfaceRotation) override {
        return mTransform;
    }
    void setTouchAffineTransformation(const TouchAffineTransformation t) { mTransform = t; }
    void notifyStylusGestureStarted(int32_t, nsecs_t) {}
    bool isInputMethodConnectionActive() override { return mFdp->ConsumeBool(); }
    std::optional<DisplayViewport> getPointerViewportForAssociatedDisplay(
            ui::LogicalDisplayId associatedDisplayId) override {
        return {};
    }
};

class FuzzInputListener : public virtual InputListenerInterface {
public:
    void notifyInputDevicesChanged(const NotifyInputDevicesChangedArgs& args) override {}
    void notifyKey(const NotifyKeyArgs& args) override {}
    void notifyMotion(const NotifyMotionArgs& args) override {}
    void notifySwitch(const NotifySwitchArgs& args) override {}
    void notifySensor(const NotifySensorArgs& args) override{};
    void notifyVibratorState(const NotifyVibratorStateArgs& args) override{};
    void notifyDeviceReset(const NotifyDeviceResetArgs& args) override {}
    void notifyPointerCaptureChanged(const NotifyPointerCaptureChangedArgs& args) override{};
};

class FuzzInputReaderContext : public InputReaderContext {
    std::shared_ptr<EventHubInterface> mEventHub;
    sp<InputReaderPolicyInterface> mPolicy;
    std::shared_ptr<ThreadSafeFuzzedDataProvider> mFdp;

public:
    FuzzInputReaderContext(std::shared_ptr<EventHubInterface> eventHub,
                           std::shared_ptr<ThreadSafeFuzzedDataProvider> fdp)
          : mEventHub(eventHub), mPolicy(sp<FuzzInputReaderPolicy>::make(fdp)), mFdp(fdp) {}
    ~FuzzInputReaderContext() {}
    std::string dump() { return "(dump from FuzzInputReaderContext)"; }
    void updateGlobalMetaState() override {}
    int32_t getGlobalMetaState() { return mFdp->ConsumeIntegral<int32_t>(); }
    void disableVirtualKeysUntil(nsecs_t time) override {}
    bool shouldDropVirtualKey(nsecs_t now, int32_t keyCode, int32_t scanCode) override {
        return mFdp->ConsumeBool();
    }
    void requestTimeoutAtTime(nsecs_t when) override {}
    int32_t bumpGeneration() override { return mFdp->ConsumeIntegral<int32_t>(); }
    void getExternalStylusDevices(std::vector<InputDeviceInfo>& outDevices) override {}
    std::list<NotifyArgs> dispatchExternalStylusState(const StylusState& outState) override {
        return {};
    }
    InputReaderPolicyInterface* getPolicy() override { return mPolicy.get(); }
    EventHubInterface* getEventHub() override { return mEventHub.get(); }
    int32_t getNextId() const override { return mFdp->ConsumeIntegral<int32_t>(); }

    void updateLedMetaState(int32_t metaState) override{};
    int32_t getLedMetaState() override { return mFdp->ConsumeIntegral<int32_t>(); };
    void notifyStylusGestureStarted(int32_t, nsecs_t) {}

    void setPreventingTouchpadTaps(bool prevent) override {}
    bool isPreventingTouchpadTaps() override { return mFdp->ConsumeBool(); };

    void setLastKeyDownTimestamp(nsecs_t when) { mLastKeyDownTimestamp = when; };
    nsecs_t getLastKeyDownTimestamp() { return mLastKeyDownTimestamp; };
    KeyboardClassifier& getKeyboardClassifier() override { return *mClassifier; }

private:
    nsecs_t mLastKeyDownTimestamp;
    std::unique_ptr<KeyboardClassifier> mClassifier = std::make_unique<KeyboardClassifier>();
};

template <class Fdp>
InputDevice getFuzzedInputDevice(Fdp& fdp, FuzzInputReaderContext* context) {
    InputDeviceIdentifier identifier;
    identifier.name = fdp.ConsumeRandomLengthUtf8String(16);
    identifier.location = fdp.ConsumeRandomLengthUtf8String(12);
    int32_t deviceID = fdp.ConsumeIntegralInRange(0, 5);
    int32_t deviceGeneration = fdp.ConsumeIntegralInRange(0, 5);
    return InputDevice(context, deviceID, deviceGeneration, identifier);
}

template <class Fdp>
void configureAndResetDevice(Fdp& fdp, InputDevice& device) {
    nsecs_t arbitraryTime = fdp.template ConsumeIntegral<nsecs_t>();
    std::list<NotifyArgs> out;
    out += device.configure(arbitraryTime, /*readerConfig=*/{}, /*changes=*/{});
    out += device.reset(arbitraryTime);
}

template <class Fdp, class T, typename... Args>
T& getMapperForDevice(Fdp& fdp, InputDevice& device, Args... args) {
    int32_t eventhubId = fdp.template ConsumeIntegral<int32_t>();
    // ensure a device entry exists for this eventHubId
    device.addEmptyEventHubDevice(eventhubId);
    configureAndResetDevice(fdp, device);

    return device.template constructAndAddMapper<T>(eventhubId, args...);
}

} // namespace android
