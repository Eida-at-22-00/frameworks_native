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

#pragma once

#include <ftl/flags.h>
#include <input/DisplayViewport.h>
#include <input/InputDevice.h>
#include <input/PropertyMap.h>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "EventHub.h"
#include "InputReaderBase.h"
#include "InputReaderContext.h"
#include "NotifyArgs.h"

namespace android {

class PeripheralController;
class PeripheralControllerInterface;
class InputDeviceContext;
class InputMapper;

/* Represents the state of a single input device. */
class InputDevice {
public:
    InputDevice(InputReaderContext* context, int32_t id, int32_t generation,
                const InputDeviceIdentifier& identifier);
    virtual ~InputDevice();

    inline InputReaderContext* getContext() { return mContext; }
    inline int32_t getId() const { return mId; }
    inline int32_t getControllerNumber() const { return mControllerNumber; }
    inline virtual int32_t getGeneration() const { return mGeneration; }
    inline const std::string getName() const { return mIdentifier.name; }
    inline const std::string getDescriptor() { return mIdentifier.descriptor; }
    inline std::optional<std::string> getBluetoothAddress() const {
        return mIdentifier.bluetoothAddress;
    }
    inline const std::string getLocation() const { return mIdentifier.location; }
    inline ftl::Flags<InputDeviceClass> getClasses() const { return mClasses; }
    inline virtual uint32_t getSources() const { return mSources; }
    inline bool hasEventHubDevices() const { return !mDevices.empty(); }

    inline virtual bool isExternal() { return mIsExternal; }
    inline std::optional<uint8_t> getAssociatedDisplayPort() const {
        return mAssociatedDisplayPort;
    }
    inline std::optional<std::string> getAssociatedDisplayUniqueIdByPort() const {
        return mAssociatedDisplayUniqueIdByPort;
    }
    inline std::optional<std::string> getAssociatedDisplayUniqueIdByDescriptor() const {
        return mAssociatedDisplayUniqueIdByDescriptor;
    }
    inline std::optional<std::string> getDeviceTypeAssociation() const {
        return mAssociatedDeviceType;
    }
    inline virtual std::optional<DisplayViewport> getAssociatedViewport() const {
        return mAssociatedViewport;
    }
    inline bool hasMic() const { return mHasMic; }

    inline bool isIgnored() { return !getMapperCount() && !mController; }

    inline virtual KeyboardType getKeyboardType() const { return mKeyboardType; }

    inline std::filesystem::path getSysfsRootPath() const { return mSysfsRootPath; }

    bool isEnabled();

    void dump(std::string& dump, const std::string& eventHubDevStr);
    void addEmptyEventHubDevice(int32_t eventHubId);
    [[nodiscard]] std::list<NotifyArgs> addEventHubDevice(
            nsecs_t when, int32_t eventHubId, const InputReaderConfiguration& readerConfig);
    void removeEventHubDevice(int32_t eventHubId);
    [[nodiscard]] std::list<NotifyArgs> configure(nsecs_t when,
                                                  const InputReaderConfiguration& readerConfig,
                                                  ConfigurationChanges changes);
    [[nodiscard]] std::list<NotifyArgs> reset(nsecs_t when);
    [[nodiscard]] std::list<NotifyArgs> process(const RawEvent* rawEvents, size_t count);
    [[nodiscard]] std::list<NotifyArgs> timeoutExpired(nsecs_t when);
    [[nodiscard]] std::list<NotifyArgs> updateExternalStylusState(const StylusState& state);

    InputDeviceInfo getDeviceInfo();
    int32_t getKeyCodeState(uint32_t sourceMask, int32_t keyCode);
    int32_t getScanCodeState(uint32_t sourceMask, int32_t scanCode);
    int32_t getSwitchState(uint32_t sourceMask, int32_t switchCode);
    int32_t getKeyCodeForKeyLocation(int32_t locationKeyCode) const;
    bool markSupportedKeyCodes(uint32_t sourceMask, const std::vector<int32_t>& keyCodes,
                               uint8_t* outFlags);
    [[nodiscard]] std::list<NotifyArgs> vibrate(const VibrationSequence& sequence, ssize_t repeat,
                                                int32_t token);
    [[nodiscard]] std::list<NotifyArgs> cancelVibrate(int32_t token);
    bool isVibrating();
    std::vector<int32_t> getVibratorIds();
    [[nodiscard]] std::list<NotifyArgs> cancelTouch(nsecs_t when, nsecs_t readTime);
    bool enableSensor(InputDeviceSensorType sensorType, std::chrono::microseconds samplingPeriod,
                      std::chrono::microseconds maxBatchReportLatency);
    void disableSensor(InputDeviceSensorType sensorType);
    void flushSensor(InputDeviceSensorType sensorType);

    std::optional<int32_t> getBatteryEventHubId() const;

    bool setLightColor(int32_t lightId, int32_t color);
    bool setLightPlayerId(int32_t lightId, int32_t playerId);
    std::optional<int32_t> getLightColor(int32_t lightId);
    std::optional<int32_t> getLightPlayerId(int32_t lightId);

    int32_t getMetaState();
    void setKeyboardType(KeyboardType keyboardType);

    virtual void bumpGeneration();

    [[nodiscard]] NotifyDeviceResetArgs notifyReset(nsecs_t when);

    inline virtual const PropertyMap& getConfiguration() const { return mConfiguration; }
    inline EventHubInterface* getEventHub() { return mContext->getEventHub(); }

    std::optional<ui::LogicalDisplayId> getAssociatedDisplayId();

    void updateLedState(bool reset);

    size_t getMapperCount();

    std::optional<HardwareProperties> getTouchpadHardwareProperties();

    bool setKernelWakeEnabled(bool enabled);

    // construct and add a mapper to the input device
    template <class T, typename... Args>
    T& addMapper(int32_t eventHubId, Args... args) {
        // ensure a device entry exists for this eventHubId
        addEmptyEventHubDevice(eventHubId);

        // create mapper
        auto& devicePair = mDevices[eventHubId];
        auto& deviceContext = devicePair.first;
        auto& mappers = devicePair.second;
        T* mapper = new T(*deviceContext, args...);
        mappers.emplace_back(mapper);
        return *mapper;
    }

    template <class T, typename... Args>
    T& constructAndAddMapper(int32_t eventHubId, Args... args) {
        // create mapper
        auto& devicePair = mDevices[eventHubId];
        auto& deviceContext = devicePair.first;
        auto& mappers = devicePair.second;
        mappers.push_back(createInputMapper<T>(*deviceContext, args...));
        return static_cast<T&>(*mappers.back());
    }

    // construct and add a controller to the input device
    template <class T>
    T& addController(int32_t eventHubId) {
        // ensure a device entry exists for this eventHubId
        addEmptyEventHubDevice(eventHubId);

        // create controller
        auto& devicePair = mDevices[eventHubId];
        auto& deviceContext = devicePair.first;

        mController = std::make_unique<T>(*deviceContext);
        return *(reinterpret_cast<T*>(mController.get()));
    }

private:
    InputReaderContext* mContext;
    int32_t mId;
    int32_t mGeneration;
    int32_t mControllerNumber;
    InputDeviceIdentifier mIdentifier;
    std::string mAlias;
    ftl::Flags<InputDeviceClass> mClasses;

    // map from eventHubId to device context and mappers
    using MapperVector = std::vector<std::unique_ptr<InputMapper>>;
    using DevicePair = std::pair<std::unique_ptr<InputDeviceContext>, MapperVector>;
    // Map from EventHub ID to pair of device context and vector of mapper.
    std::unordered_map<int32_t, DevicePair> mDevices;
    // Misc devices controller for lights, battery, etc.
    std::unique_ptr<PeripheralControllerInterface> mController;

    uint32_t mSources;
    bool mIsWaking;
    bool mIsExternal;
    KeyboardType mKeyboardType = KeyboardType::NONE;
    std::optional<uint8_t> mAssociatedDisplayPort;
    std::optional<std::string> mAssociatedDisplayUniqueIdByPort;
    std::optional<std::string> mAssociatedDisplayUniqueIdByDescriptor;
    std::optional<std::string> mAssociatedDeviceType;
    std::optional<DisplayViewport> mAssociatedViewport;
    bool mHasMic;
    bool mDropUntilNextSync;
    std::optional<bool> mShouldSmoothScroll;
    std::filesystem::path mSysfsRootPath;

    typedef int32_t (InputMapper::*GetStateFunc)(uint32_t sourceMask, int32_t code);
    int32_t getState(uint32_t sourceMask, int32_t code, GetStateFunc getStateFunc);

    std::vector<std::unique_ptr<InputMapper>> createMappers(
            InputDeviceContext& contextPtr, const InputReaderConfiguration& readerConfig);

    [[nodiscard]] std::list<NotifyArgs> configureInternal(
            nsecs_t when, const InputReaderConfiguration& readerConfig,
            ConfigurationChanges changes, bool forceEnable = false);

    [[nodiscard]] std::list<NotifyArgs> updateEnableState(
            nsecs_t when, const InputReaderConfiguration& readerConfig, bool forceEnable = false);

    PropertyMap mConfiguration;

    // Runs logic post a `process` call. This can be used to update the generated `NotifyArgs` as
    // per the properties of the InputDevice.
    void postProcess(std::list<NotifyArgs>& args) const;

    // helpers to interate over the devices collection
    // run a function against every mapper on every subdevice
    inline void for_each_mapper(std::function<void(InputMapper&)> f) {
        for (auto& deviceEntry : mDevices) {
            auto& devicePair = deviceEntry.second;
            auto& mappers = devicePair.second;
            for (auto& mapperPtr : mappers) {
                f(*mapperPtr);
            }
        }
    }

    // run a function against every mapper on a specific subdevice
    inline void for_each_mapper_in_subdevice(int32_t eventHubDevice,
                                             std::function<void(InputMapper&)> f) {
        auto deviceIt = mDevices.find(eventHubDevice);
        if (deviceIt != mDevices.end()) {
            auto& devicePair = deviceIt->second;
            auto& mappers = devicePair.second;
            for (auto& mapperPtr : mappers) {
                f(*mapperPtr);
            }
        }
    }

    // run a function against every subdevice
    inline void for_each_subdevice(std::function<void(InputDeviceContext&)> f) {
        for (auto& deviceEntry : mDevices) {
            auto& devicePair = deviceEntry.second;
            auto& contextPtr = devicePair.first;
            f(*contextPtr);
        }
    }

    // return the first value returned by a function over every mapper.
    // if all mappers return nullopt, return nullopt.
    template <typename T>
    inline std::optional<T> first_in_mappers(
            std::function<std::optional<T>(InputMapper&)> f) const {
        for (auto& deviceEntry : mDevices) {
            auto& devicePair = deviceEntry.second;
            auto& mappers = devicePair.second;
            for (auto& mapperPtr : mappers) {
                std::optional<T> ret = f(*mapperPtr);
                if (ret) {
                    return ret;
                }
            }
        }
        return std::nullopt;
    }
};

/* Provides access to EventHub methods, but limits access to the current InputDevice.
 * Essentially an implementation of EventHubInterface, but for a specific device id.
 * Helps hide implementation details of InputDevice and EventHub. Used by mappers to
 * check the status of the associated hardware device
 */
class InputDeviceContext {
public:
    InputDeviceContext(InputDevice& device, int32_t eventHubId);
    virtual ~InputDeviceContext();

    inline InputReaderContext* getContext() { return mContext; }
    inline int32_t getId() { return mDeviceId; }
    inline int32_t getEventHubId() { return mId; }

    inline ftl::Flags<InputDeviceClass> getDeviceClasses() const {
        return mEventHub->getDeviceClasses(mId);
    }
    inline uint32_t getDeviceSources() const { return mDevice.getSources(); }
    inline InputDeviceIdentifier getDeviceIdentifier() const {
        return mEventHub->getDeviceIdentifier(mId);
    }
    inline int32_t getDeviceControllerNumber() const {
        return mEventHub->getDeviceControllerNumber(mId);
    }
    inline std::optional<RawAbsoluteAxisInfo> getAbsoluteAxisInfo(int32_t code) const {
        std::optional<RawAbsoluteAxisInfo> info = mEventHub->getAbsoluteAxisInfo(mId, code);

        // Validate axis info for InputDevice.
        if (info && info->minValue == info->maxValue) {
            // Historically, we deem axes with the same min and max values as invalid to avoid
            // dividing by zero when scaling by max - min.
            // TODO(b/291772515): Perform axis info validation on a per-axis basis when it is used.
            return std::nullopt;
        }
        return info;
    }
    inline bool hasRelativeAxis(int32_t code) const {
        return mEventHub->hasRelativeAxis(mId, code);
    }
    inline bool hasInputProperty(int32_t property) const {
        return mEventHub->hasInputProperty(mId, property);
    }

    inline bool hasMscEvent(int mscEvent) const { return mEventHub->hasMscEvent(mId, mscEvent); }

    inline void setKeyRemapping(const std::map<int32_t, int32_t>& keyRemapping) const {
        mEventHub->setKeyRemapping(mId, keyRemapping);
    }

    inline status_t mapKey(int32_t scanCode, int32_t usageCode, int32_t metaState,
                           int32_t* outKeycode, int32_t* outMetaState, uint32_t* outFlags) const {
        return mEventHub->mapKey(mId, scanCode, usageCode, metaState, outKeycode, outMetaState,
                                 outFlags);
    }
    inline status_t mapAxis(int32_t scanCode, AxisInfo* outAxisInfo) const {
        return mEventHub->mapAxis(mId, scanCode, outAxisInfo);
    }
    inline base::Result<std::pair<InputDeviceSensorType, int32_t>> mapSensor(int32_t absCode) {
        return mEventHub->mapSensor(mId, absCode);
    }

    inline const std::vector<int32_t> getRawLightIds() { return mEventHub->getRawLightIds(mId); }

    inline std::optional<RawLightInfo> getRawLightInfo(int32_t lightId) {
        return mEventHub->getRawLightInfo(mId, lightId);
    }

    inline std::optional<int32_t> getLightBrightness(int32_t lightId) {
        return mEventHub->getLightBrightness(mId, lightId);
    }

    inline void setLightBrightness(int32_t lightId, int32_t brightness) {
        return mEventHub->setLightBrightness(mId, lightId, brightness);
    }

    inline std::optional<std::unordered_map<LightColor, int32_t>> getLightIntensities(
            int32_t lightId) {
        return mEventHub->getLightIntensities(mId, lightId);
    }

    inline void setLightIntensities(int32_t lightId,
                                    std::unordered_map<LightColor, int32_t> intensities) {
        return mEventHub->setLightIntensities(mId, lightId, intensities);
    }

    inline std::vector<TouchVideoFrame> getVideoFrames() { return mEventHub->getVideoFrames(mId); }
    inline int32_t getScanCodeState(int32_t scanCode) const {
        return mEventHub->getScanCodeState(mId, scanCode);
    }
    inline int32_t getKeyCodeState(int32_t keyCode) const {
        return mEventHub->getKeyCodeState(mId, keyCode);
    }
    inline int32_t getKeyCodeForKeyLocation(int32_t locationKeyCode) const {
        return mEventHub->getKeyCodeForKeyLocation(mId, locationKeyCode);
    }
    inline int32_t getSwitchState(int32_t sw) const { return mEventHub->getSwitchState(mId, sw); }
    inline std::optional<int32_t> getAbsoluteAxisValue(int32_t code) const {
        return mEventHub->getAbsoluteAxisValue(mId, code);
    }
    inline base::Result<std::vector<int32_t>> getMtSlotValues(int32_t axis,
                                                              size_t slotCount) const {
        return mEventHub->getMtSlotValues(mId, axis, slotCount);
    }
    inline bool markSupportedKeyCodes(const std::vector<int32_t>& keyCodes,
                                      uint8_t* outFlags) const {
        return mEventHub->markSupportedKeyCodes(mId, keyCodes, outFlags);
    }
    inline bool hasScanCode(int32_t scanCode) const {
        return mEventHub->hasScanCode(mId, scanCode);
    }
    inline bool hasKeyCode(int32_t keyCode) const { return mEventHub->hasKeyCode(mId, keyCode); }
    inline bool hasLed(int32_t led) const { return mEventHub->hasLed(mId, led); }
    inline void setLedState(int32_t led, bool on) { return mEventHub->setLedState(mId, led, on); }
    inline void getVirtualKeyDefinitions(std::vector<VirtualKeyDefinition>& outVirtualKeys) const {
        return mEventHub->getVirtualKeyDefinitions(mId, outVirtualKeys);
    }
    inline const std::shared_ptr<KeyCharacterMap> getKeyCharacterMap() const {
        return mEventHub->getKeyCharacterMap(mId);
    }
    inline bool setKeyboardLayoutOverlay(std::shared_ptr<KeyCharacterMap> map) {
        return mEventHub->setKeyboardLayoutOverlay(mId, map);
    }
    inline const std::optional<RawLayoutInfo> getRawLayoutInfo() {
        return mEventHub->getRawLayoutInfo(mId);
    }
    inline void vibrate(const VibrationElement& element) {
        return mEventHub->vibrate(mId, element);
    }
    inline void cancelVibrate() { return mEventHub->cancelVibrate(mId); }

    inline std::vector<int32_t> getVibratorIds() { return mEventHub->getVibratorIds(mId); }

    inline const std::vector<int32_t> getRawBatteryIds() {
        return mEventHub->getRawBatteryIds(mId);
    }

    inline std::optional<RawBatteryInfo> getRawBatteryInfo(int32_t batteryId) {
        return mEventHub->getRawBatteryInfo(mId, batteryId);
    }

    inline std::optional<int32_t> getBatteryCapacity(int32_t batteryId) {
        return mEventHub->getBatteryCapacity(mId, batteryId);
    }

    inline std::optional<int32_t> getBatteryStatus(int32_t batteryId) {
        return mEventHub->getBatteryStatus(mId, batteryId);
    }

    inline bool hasAbsoluteAxis(int32_t code) const {
        return mEventHub->getAbsoluteAxisInfo(mId, code).has_value();
    }
    inline bool isKeyPressed(int32_t scanCode) const {
        return mEventHub->getScanCodeState(mId, scanCode) == AKEY_STATE_DOWN;
    }
    inline bool isKeyCodePressed(int32_t keyCode) const {
        return mEventHub->getKeyCodeState(mId, keyCode) == AKEY_STATE_DOWN;
    }
    inline bool isDeviceEnabled() { return mEventHub->isDeviceEnabled(mId); }
    inline status_t enableDevice() { return mEventHub->enableDevice(mId); }
    inline status_t disableDevice() { return mEventHub->disableDevice(mId); }

    inline const std::string getName() const { return mDevice.getName(); }
    inline const std::string getDescriptor() { return mDevice.getDescriptor(); }
    inline const std::string getLocation() { return mDevice.getLocation(); }
    inline bool isExternal() const { return mDevice.isExternal(); }
    inline std::optional<uint8_t> getAssociatedDisplayPort() const {
        return mDevice.getAssociatedDisplayPort();
    }
    inline std::optional<std::string> getAssociatedDisplayUniqueIdByPort() const {
        return mDevice.getAssociatedDisplayUniqueIdByPort();
    }
    inline std::optional<std::string> getAssociatedDisplayUniqueIdByDescriptor() const {
        return mDevice.getAssociatedDisplayUniqueIdByDescriptor();
    }
    inline std::optional<std::string> getDeviceTypeAssociation() const {
        return mDevice.getDeviceTypeAssociation();
    }
    virtual std::optional<DisplayViewport> getAssociatedViewport() const {
        return mDevice.getAssociatedViewport();
    }
    [[nodiscard]] inline std::list<NotifyArgs> cancelTouch(nsecs_t when, nsecs_t readTime) {
        return mDevice.cancelTouch(when, readTime);
    }
    inline void bumpGeneration() { mDevice.bumpGeneration(); }
    inline const PropertyMap& getConfiguration() const { return mDevice.getConfiguration(); }
    inline KeyboardType getKeyboardType() const { return mDevice.getKeyboardType(); }
    inline void setKeyboardType(KeyboardType keyboardType) {
        return mDevice.setKeyboardType(keyboardType);
    }
    inline std::filesystem::path getSysfsRootPath() const {
        return mEventHub->getSysfsRootPath(mId);
    }
    inline bool setKernelWakeEnabled(bool enabled) {
        return mEventHub->setKernelWakeEnabled(mId, enabled);
    }

private:
    InputDevice& mDevice;
    InputReaderContext* mContext;
    EventHubInterface* mEventHub;
    int32_t mId;
    int32_t mDeviceId;
};

} // namespace android
