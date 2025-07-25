/*
 * Copyright (C) 2005 The Android Open Source Project
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

#include <bitset>
#include <climits>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <batteryservice/BatteryService.h>
#include <ftl/flags.h>
#include <input/BlockingQueue.h>
#include <input/Input.h>
#include <input/InputDevice.h>
#include <input/KeyCharacterMap.h>
#include <input/KeyLayoutMap.h>
#include <input/Keyboard.h>
#include <input/PropertyMap.h>
#include <input/VirtualKeyMap.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <utils/BitSet.h>
#include <utils/Errors.h>
#include <utils/List.h>
#include <utils/Log.h>
#include <utils/Mutex.h>

#include "TouchVideoDevice.h"
#include "VibrationElement.h"

struct inotify_event;

namespace android {

/* Number of colors : {red, green, blue} */
static constexpr size_t COLOR_NUM = 3;
/*
 * A raw event as retrieved from the EventHub.
 */
struct RawEvent {
    // Time when the event happened
    nsecs_t when;
    // Time when the event was read by EventHub. Only populated for input events.
    // For other events (device added/removed/etc), this value is undefined and should not be read.
    nsecs_t readTime;
    int32_t deviceId;
    int32_t type;
    int32_t code;
    int32_t value;
};

/* Describes an absolute axis. */
struct RawAbsoluteAxisInfo {
    int32_t minValue{};   // minimum value
    int32_t maxValue{};   // maximum value
    int32_t flat{};       // center flat position, eg. flat == 8 means center is between -8 and 8
    int32_t fuzz{};       // error tolerance, eg. fuzz == 4 means value is +/- 4 due to noise
    int32_t resolution{}; // resolution in units per mm or radians per mm
};

std::ostream& operator<<(std::ostream& out, const std::optional<RawAbsoluteAxisInfo>& info);

/*
 * Input device classes.
 *
 * These classes are duplicated in rust side here: /frameworks/native/libs/input/rust/input.rs.
 * If any new classes are added, we need to add them in rust input side too.
 */
enum class InputDeviceClass : uint32_t {
    // LINT.IfChange
    /* The input device is a keyboard or has buttons. */
    KEYBOARD = android::os::IInputConstants::DEVICE_CLASS_KEYBOARD,

    /* The input device is an alpha-numeric keyboard (not just a dial pad). */
    ALPHAKEY = android::os::IInputConstants::DEVICE_CLASS_ALPHAKEY,

    /* The input device is a touchscreen or a touchpad (either single-touch or multi-touch). */
    TOUCH = android::os::IInputConstants::DEVICE_CLASS_TOUCH,

    /* The input device is a cursor device such as a trackball or mouse. */
    CURSOR = android::os::IInputConstants::DEVICE_CLASS_CURSOR,

    /* The input device is a multi-touch touchscreen or touchpad. */
    TOUCH_MT = android::os::IInputConstants::DEVICE_CLASS_TOUCH_MT,

    /* The input device is a directional pad (implies keyboard, has DPAD keys). */
    DPAD = android::os::IInputConstants::DEVICE_CLASS_DPAD,

    /* The input device is a gamepad (implies keyboard, has BUTTON keys). */
    GAMEPAD = android::os::IInputConstants::DEVICE_CLASS_GAMEPAD,

    /* The input device has switches. */
    SWITCH = android::os::IInputConstants::DEVICE_CLASS_SWITCH,

    /* The input device is a joystick (implies gamepad, has joystick absolute axes). */
    JOYSTICK = android::os::IInputConstants::DEVICE_CLASS_JOYSTICK,

    /* The input device has a vibrator (supports FF_RUMBLE). */
    VIBRATOR = android::os::IInputConstants::DEVICE_CLASS_VIBRATOR,

    /* The input device has a microphone. */
    MIC = android::os::IInputConstants::DEVICE_CLASS_MIC,

    /* The input device is an external stylus (has data we want to fuse with touch data). */
    EXTERNAL_STYLUS = android::os::IInputConstants::DEVICE_CLASS_EXTERNAL_STYLUS,

    /* The input device has a rotary encoder */
    ROTARY_ENCODER = android::os::IInputConstants::DEVICE_CLASS_ROTARY_ENCODER,

    /* The input device has a sensor like accelerometer, gyro, etc */
    SENSOR = android::os::IInputConstants::DEVICE_CLASS_SENSOR,

    /* The input device has a battery */
    BATTERY = android::os::IInputConstants::DEVICE_CLASS_BATTERY,

    /* The input device has sysfs controllable lights */
    LIGHT = android::os::IInputConstants::DEVICE_CLASS_LIGHT,

    /* The input device is a touchpad, requiring an on-screen cursor. */
    TOUCHPAD = android::os::IInputConstants::DEVICE_CLASS_TOUCHPAD,

    /* The input device is virtual (not a real device, not part of UI configuration). */
    VIRTUAL = android::os::IInputConstants::DEVICE_CLASS_VIRTUAL,

    /* The input device is external (not built-in). */
    EXTERNAL = android::os::IInputConstants::DEVICE_CLASS_EXTERNAL,
    // LINT.ThenChange(frameworks/native/services/inputflinger/tests/fuzzers/MapperHelpers.h)
};

enum class SysfsClass : uint32_t {
    POWER_SUPPLY = 0,
    LEDS = 1,

    ftl_last = LEDS
};

enum class LightColor : uint32_t {
    RED = 0,
    GREEN = 1,
    BLUE = 2,
};

enum class InputLightClass : uint32_t {
    /* The input light has brightness node. */
    BRIGHTNESS = 0x00000001,
    /* The input light has red name. */
    RED = 0x00000002,
    /* The input light has green name. */
    GREEN = 0x00000004,
    /* The input light has blue name. */
    BLUE = 0x00000008,
    /* The input light has global name. */
    GLOBAL = 0x00000010,
    /* The input light has multi index node. */
    MULTI_INDEX = 0x00000020,
    /* The input light has multi intensity node. */
    MULTI_INTENSITY = 0x00000040,
    /* The input light has max brightness node. */
    MAX_BRIGHTNESS = 0x00000080,
    /* The input light has kbd_backlight name */
    KEYBOARD_BACKLIGHT = 0x00000100,
    /* The input light has mic_mute name */
    KEYBOARD_MIC_MUTE = 0x00000200,
    /* The input light has mute name */
    KEYBOARD_VOLUME_MUTE = 0x00000400,
};

enum class InputBatteryClass : uint32_t {
    /* The input device battery has capacity node. */
    CAPACITY = 0x00000001,
    /* The input device battery has capacity_level node. */
    CAPACITY_LEVEL = 0x00000002,
    /* The input device battery has status node. */
    STATUS = 0x00000004,
};

/* Describes a raw light. */
struct RawLightInfo {
    int32_t id;
    std::string name;
    std::optional<int32_t> maxBrightness;
    ftl::Flags<InputLightClass> flags;
    std::array<int32_t, COLOR_NUM> rgbIndex;
    std::filesystem::path path;

    bool operator==(const RawLightInfo&) const = default;
    bool operator!=(const RawLightInfo&) const = default;
};

/* Describes a raw battery. */
struct RawBatteryInfo {
    int32_t id;
    std::string name;
    ftl::Flags<InputBatteryClass> flags;
    std::filesystem::path path;

    bool operator==(const RawBatteryInfo&) const = default;
    bool operator!=(const RawBatteryInfo&) const = default;
};

/* Layout information associated with the device */
struct RawLayoutInfo {
    std::string languageTag;
    std::string layoutType;

    bool operator==(const RawLayoutInfo&) const = default;
    bool operator!=(const RawLayoutInfo&) const = default;
};

/*
 * Gets the class that owns an axis, in cases where multiple classes might claim
 * the same axis for different purposes.
 */
extern ftl::Flags<InputDeviceClass> getAbsAxisUsage(int32_t axis,
                                                    ftl::Flags<InputDeviceClass> deviceClasses);

/*
 * Grand Central Station for events.
 *
 * The event hub aggregates input events received across all known input
 * devices on the system, including devices that may be emulated by the simulator
 * environment.  In addition, the event hub generates fake input events to indicate
 * when devices are added or removed.
 *
 * The event hub provides a stream of input events (via the getEvent function).
 * It also supports querying the current actual state of input devices such as identifying
 * which keys are currently down.  Finally, the event hub keeps track of the capabilities of
 * individual input devices, such as their class and the set of key codes that they support.
 */
class EventHubInterface {
public:
    EventHubInterface() {}
    virtual ~EventHubInterface() {}

    // Synthetic raw event type codes produced when devices are added or removed.
    enum {
        // Sent when a device is added.
        DEVICE_ADDED = 0x10000000,
        // Sent when a device is removed.
        DEVICE_REMOVED = 0x20000000,

        FIRST_SYNTHETIC_EVENT = DEVICE_ADDED,
    };

    virtual ftl::Flags<InputDeviceClass> getDeviceClasses(int32_t deviceId) const = 0;

    virtual InputDeviceIdentifier getDeviceIdentifier(int32_t deviceId) const = 0;

    virtual int32_t getDeviceControllerNumber(int32_t deviceId) const = 0;

    /**
     * Get the PropertyMap for the provided EventHub device, if available.
     * This acquires the device lock, so a copy is returned rather than the raw pointer
     * to the device's PropertyMap. A std::nullopt may be returned if the device could
     * not be found, or if it doesn't have any configuration.
     */
    virtual std::optional<PropertyMap> getConfiguration(int32_t deviceId) const = 0;

    virtual std::optional<RawAbsoluteAxisInfo> getAbsoluteAxisInfo(int32_t deviceId,
                                                                   int axis) const = 0;

    virtual bool hasRelativeAxis(int32_t deviceId, int axis) const = 0;

    virtual bool hasInputProperty(int32_t deviceId, int property) const = 0;

    virtual bool hasMscEvent(int32_t deviceId, int mscEvent) const = 0;

    virtual void setKeyRemapping(int32_t deviceId,
                                 const std::map<int32_t, int32_t>& keyRemapping) const = 0;

    virtual status_t mapKey(int32_t deviceId, int32_t scanCode, int32_t usageCode,
                            int32_t metaState, int32_t* outKeycode, int32_t* outMetaState,
                            uint32_t* outFlags) const = 0;

    virtual status_t mapAxis(int32_t deviceId, int32_t scanCode, AxisInfo* outAxisInfo) const = 0;

    // Sets devices that are excluded from opening.
    // This can be used to ignore input devices for sensors.
    virtual void setExcludedDevices(const std::vector<std::string>& devices) = 0;

    /*
     * Wait for events to become available and returns them.
     * After returning, the EventHub holds onto a wake lock until the next call to getEvent.
     * This ensures that the device will not go to sleep while the event is being processed.
     * If the device needs to remain awake longer than that, then the caller is responsible
     * for taking care of it (say, by poking the power manager user activity timer).
     *
     * The timeout is advisory only.  If the device is asleep, it will not wake just to
     * service the timeout.
     *
     * Returns the number of events obtained, or 0 if the timeout expired.
     */
    virtual std::vector<RawEvent> getEvents(int timeoutMillis) = 0;
    virtual std::vector<TouchVideoFrame> getVideoFrames(int32_t deviceId) = 0;
    virtual base::Result<std::pair<InputDeviceSensorType, int32_t>> mapSensor(
            int32_t deviceId, int32_t absCode) const = 0;
    // Raw batteries are sysfs power_supply nodes we found from the EventHub device sysfs node,
    // containing the raw info of the sysfs node structure.
    virtual std::vector<int32_t> getRawBatteryIds(int32_t deviceId) const = 0;
    virtual std::optional<RawBatteryInfo> getRawBatteryInfo(int32_t deviceId,
                                                            int32_t BatteryId) const = 0;

    // Raw lights are sysfs led light nodes we found from the EventHub device sysfs node,
    // containing the raw info of the sysfs node structure.
    virtual std::vector<int32_t> getRawLightIds(int32_t deviceId) const = 0;
    virtual std::optional<RawLightInfo> getRawLightInfo(int32_t deviceId,
                                                        int32_t lightId) const = 0;
    virtual std::optional<int32_t> getLightBrightness(int32_t deviceId, int32_t lightId) const = 0;
    virtual void setLightBrightness(int32_t deviceId, int32_t lightId, int32_t brightness) = 0;
    virtual std::optional<std::unordered_map<LightColor, int32_t>> getLightIntensities(
            int32_t deviceId, int32_t lightId) const = 0;
    virtual void setLightIntensities(int32_t deviceId, int32_t lightId,
                                     std::unordered_map<LightColor, int32_t> intensities) = 0;
    /* Query Layout info associated with the input device. */
    virtual std::optional<RawLayoutInfo> getRawLayoutInfo(int32_t deviceId) const = 0;
    /* Query current input state. */
    virtual int32_t getScanCodeState(int32_t deviceId, int32_t scanCode) const = 0;
    virtual int32_t getKeyCodeState(int32_t deviceId, int32_t keyCode) const = 0;
    virtual int32_t getSwitchState(int32_t deviceId, int32_t sw) const = 0;
    virtual std::optional<int32_t> getAbsoluteAxisValue(int32_t deviceId, int32_t axis) const = 0;
    /* Query Multi-Touch slot values for an axis. Returns error or an 1 indexed array of size
     * (slotCount + 1). The value at the 0 index is set to queried axis. */
    virtual base::Result<std::vector<int32_t>> getMtSlotValues(int32_t deviceId, int32_t axis,
                                                               size_t slotCount) const = 0;
    virtual int32_t getKeyCodeForKeyLocation(int32_t deviceId, int32_t locationKeyCode) const = 0;

    /*
     * Examine key input devices for specific framework keycode support
     */
    virtual bool markSupportedKeyCodes(int32_t deviceId, const std::vector<int32_t>& keyCodes,
                                       uint8_t* outFlags) const = 0;

    virtual bool hasScanCode(int32_t deviceId, int32_t scanCode) const = 0;
    virtual bool hasKeyCode(int32_t deviceId, int32_t keyCode) const = 0;

    /* LED related functions expect Android LED constants, not scan codes or HID usages */
    virtual bool hasLed(int32_t deviceId, int32_t led) const = 0;
    virtual void setLedState(int32_t deviceId, int32_t led, bool on) = 0;

    virtual void getVirtualKeyDefinitions(
            int32_t deviceId, std::vector<VirtualKeyDefinition>& outVirtualKeys) const = 0;

    virtual const std::shared_ptr<KeyCharacterMap> getKeyCharacterMap(int32_t deviceId) const = 0;
    virtual bool setKeyboardLayoutOverlay(int32_t deviceId,
                                          std::shared_ptr<KeyCharacterMap> map) = 0;

    /* Control the vibrator. */
    virtual void vibrate(int32_t deviceId, const VibrationElement& effect) = 0;
    virtual void cancelVibrate(int32_t deviceId) = 0;
    virtual std::vector<int32_t> getVibratorIds(int32_t deviceId) const = 0;

    /* Query battery level. */
    virtual std::optional<int32_t> getBatteryCapacity(int32_t deviceId,
                                                      int32_t batteryId) const = 0;

    /* Query battery status. */
    virtual std::optional<int32_t> getBatteryStatus(int32_t deviceId, int32_t batteryId) const = 0;

    /* Requests the EventHub to reopen all input devices on the next call to getEvents(). */
    virtual void requestReopenDevices() = 0;

    /* Wakes up getEvents() if it is blocked on a read. */
    virtual void wake() = 0;

    /* Dump EventHub state to a string. */
    virtual void dump(std::string& dump) const = 0;

    /* Called by the heatbeat to ensures that the reader has not deadlocked. */
    virtual void monitor() const = 0;

    /* Return true if the device is enabled. */
    virtual bool isDeviceEnabled(int32_t deviceId) const = 0;

    /* Enable an input device */
    virtual status_t enableDevice(int32_t deviceId) = 0;

    /* Disable an input device. Closes file descriptor to that device. */
    virtual status_t disableDevice(int32_t deviceId) = 0;

    /* Gets the sysfs root path for this device. Returns an empty path if there is none. */
    virtual std::filesystem::path getSysfsRootPath(int32_t deviceId) const = 0;

    /* Sysfs node changed. Reopen the Eventhub device if any new Peripheral like Light, Battery,
     * etc. is detected. */
    virtual void sysfsNodeChanged(const std::string& sysfsNodePath) = 0;

    /* Set whether the given input device can wake up the kernel from sleep
     * when it generates input events. By default, usually only internal (built-in)
     * input devices can wake the kernel from sleep. For an external input device
     * that supports remote wakeup to be able to wake the kernel, this must be called
     * after each time the device is connected/added. */
    virtual bool setKernelWakeEnabled(int32_t deviceId, bool enabled) = 0;
};

template <std::size_t BITS>
class BitArray {
    /* Array element type and vector of element type. */
    using Element = std::uint32_t;
    /* Number of bits in each BitArray element. */
    static constexpr size_t WIDTH = sizeof(Element) * CHAR_BIT;
    /* Number of elements to represent a bit array of the specified size of bits. */
    static constexpr size_t COUNT = (BITS + WIDTH - 1) / WIDTH;

public:
    /* BUFFER type declaration for BitArray */
    using Buffer = std::array<Element, COUNT>;
    /* To tell if a bit is set in array, it selects an element from the array, and test
     * if the relevant bit set.
     * Note the parameter "bit" is an index to the bit, 0 <= bit < BITS.
     */
    inline bool test(size_t bit) const {
        return (bit < BITS) && mData[bit / WIDTH].test(bit % WIDTH);
    }
    /* Sets the given bit in the bit array to given value.
     * Returns true if the given bit is a valid index and thus was set successfully.
     */
    inline bool set(size_t bit, bool value) {
        if (bit >= BITS) {
            return false;
        }
        mData[bit / WIDTH].set(bit % WIDTH, value);
        return true;
    }
    /* Returns total number of bytes needed for the array */
    inline size_t bytes() { return (BITS + CHAR_BIT - 1) / CHAR_BIT; }
    /* Returns true if array contains any non-zero bit from the range defined by start and end
     * bit index [startIndex, endIndex).
     */
    bool any(size_t startIndex, size_t endIndex) {
        if (startIndex >= endIndex || startIndex > BITS || endIndex > BITS + 1) {
            ALOGE("Invalid start/end index. start = %zu, end = %zu, total bits = %zu", startIndex,
                  endIndex, BITS);
            return false;
        }
        size_t se = startIndex / WIDTH; // Start of element
        size_t ee = endIndex / WIDTH;   // End of element
        size_t si = startIndex % WIDTH; // Start index in start element
        size_t ei = endIndex % WIDTH;   // End index in end element
        // Need to check first unaligned bitset for any non zero bit
        if (si > 0) {
            size_t nBits = se == ee ? ei - si : WIDTH - si;
            // Generate the mask of interested bit range
            Element mask = ((1 << nBits) - 1) << si;
            if (mData[se++].to_ulong() & mask) {
                return true;
            }
        }
        // Check whole bitset for any bit set
        for (; se < ee; se++) {
            if (mData[se].any()) {
                return true;
            }
        }
        // Need to check last unaligned bitset for any non zero bit
        if (ei > 0 && se <= ee) {
            // Generate the mask of interested bit range
            Element mask = (1 << ei) - 1;
            if (mData[se].to_ulong() & mask) {
                return true;
            }
        }
        return false;
    }
    /* Load bit array values from buffer */
    void loadFromBuffer(const Buffer& buffer) {
        for (size_t i = 0; i < COUNT; i++) {
            mData[i] = std::bitset<WIDTH>(buffer[i]);
        }
    }
    /* Dump the indices in the bit array that are set. */
    inline std::string dumpSetIndices(std::string separator,
                                      std::function<std::string(size_t /*index*/)> format) {
        std::string dmp;
        for (size_t i = 0; i < BITS; i++) {
            if (test(i)) {
                if (!dmp.empty()) {
                    dmp += separator;
                }
                dmp += format(i);
            }
        }
        return dmp.empty() ? "<none>" : dmp;
    }

private:
    std::array<std::bitset<WIDTH>, COUNT> mData;
};

class EventHub : public EventHubInterface {
public:
    EventHub();

    ftl::Flags<InputDeviceClass> getDeviceClasses(int32_t deviceId) const override final;

    InputDeviceIdentifier getDeviceIdentifier(int32_t deviceId) const override final;

    int32_t getDeviceControllerNumber(int32_t deviceId) const override final;

    std::optional<PropertyMap> getConfiguration(int32_t deviceId) const override final;

    std::optional<RawAbsoluteAxisInfo> getAbsoluteAxisInfo(int32_t deviceId,
                                                           int axis) const override final;

    bool hasRelativeAxis(int32_t deviceId, int axis) const override final;

    bool hasInputProperty(int32_t deviceId, int property) const override final;

    bool hasMscEvent(int32_t deviceId, int mscEvent) const override final;

    void setKeyRemapping(int32_t deviceId,
                         const std::map<int32_t, int32_t>& keyRemapping) const override final;

    status_t mapKey(int32_t deviceId, int32_t scanCode, int32_t usageCode, int32_t metaState,
                    int32_t* outKeycode, int32_t* outMetaState,
                    uint32_t* outFlags) const override final;

    status_t mapAxis(int32_t deviceId, int32_t scanCode,
                     AxisInfo* outAxisInfo) const override final;

    base::Result<std::pair<InputDeviceSensorType, int32_t>> mapSensor(
            int32_t deviceId, int32_t absCode) const override final;

    std::vector<int32_t> getRawBatteryIds(int32_t deviceId) const override final;
    std::optional<RawBatteryInfo> getRawBatteryInfo(int32_t deviceId,
                                                    int32_t BatteryId) const override final;

    std::vector<int32_t> getRawLightIds(int32_t deviceId) const override final;

    std::optional<RawLightInfo> getRawLightInfo(int32_t deviceId,
                                                int32_t lightId) const override final;

    std::optional<int32_t> getLightBrightness(int32_t deviceId,
                                              int32_t lightId) const override final;
    void setLightBrightness(int32_t deviceId, int32_t lightId, int32_t brightness) override final;
    std::optional<std::unordered_map<LightColor, int32_t>> getLightIntensities(
            int32_t deviceId, int32_t lightId) const override final;
    void setLightIntensities(int32_t deviceId, int32_t lightId,
                             std::unordered_map<LightColor, int32_t> intensities) override final;

    std::optional<RawLayoutInfo> getRawLayoutInfo(int32_t deviceId) const override final;

    void setExcludedDevices(const std::vector<std::string>& devices) override final;

    int32_t getScanCodeState(int32_t deviceId, int32_t scanCode) const override final;
    int32_t getKeyCodeState(int32_t deviceId, int32_t keyCode) const override final;
    int32_t getSwitchState(int32_t deviceId, int32_t sw) const override final;
    int32_t getKeyCodeForKeyLocation(int32_t deviceId,
                                     int32_t locationKeyCode) const override final;
    std::optional<int32_t> getAbsoluteAxisValue(int32_t deviceId,
                                                int32_t axis) const override final;
    base::Result<std::vector<int32_t>> getMtSlotValues(int32_t deviceId, int32_t axis,
                                                       size_t slotCount) const override final;

    bool markSupportedKeyCodes(int32_t deviceId, const std::vector<int32_t>& keyCodes,
                               uint8_t* outFlags) const override final;

    std::vector<RawEvent> getEvents(int timeoutMillis) override final;
    std::vector<TouchVideoFrame> getVideoFrames(int32_t deviceId) override final;

    bool hasScanCode(int32_t deviceId, int32_t scanCode) const override final;
    bool hasKeyCode(int32_t deviceId, int32_t keyCode) const override final;
    bool hasLed(int32_t deviceId, int32_t led) const override final;
    void setLedState(int32_t deviceId, int32_t led, bool on) override final;

    void getVirtualKeyDefinitions(
            int32_t deviceId,
            std::vector<VirtualKeyDefinition>& outVirtualKeys) const override final;

    const std::shared_ptr<KeyCharacterMap> getKeyCharacterMap(
            int32_t deviceId) const override final;
    bool setKeyboardLayoutOverlay(int32_t deviceId,
                                  std::shared_ptr<KeyCharacterMap> map) override final;

    void vibrate(int32_t deviceId, const VibrationElement& effect) override final;
    void cancelVibrate(int32_t deviceId) override final;
    std::vector<int32_t> getVibratorIds(int32_t deviceId) const override final;

    void requestReopenDevices() override final;

    void wake() override final;

    void dump(std::string& dump) const override final;

    void monitor() const override final;

    std::optional<int32_t> getBatteryCapacity(int32_t deviceId,
                                              int32_t batteryId) const override final;

    std::optional<int32_t> getBatteryStatus(int32_t deviceId,
                                            int32_t batteryId) const override final;

    bool isDeviceEnabled(int32_t deviceId) const override final;

    status_t enableDevice(int32_t deviceId) override final;

    status_t disableDevice(int32_t deviceId) override final;

    std::filesystem::path getSysfsRootPath(int32_t deviceId) const override final;

    void sysfsNodeChanged(const std::string& sysfsNodePath) override final;

    bool setKernelWakeEnabled(int32_t deviceId, bool enabled) override final;

    ~EventHub() override;

private:
    // Holds information about the sysfs device associated with the Device.
    struct AssociatedDevice {
        AssociatedDevice(const std::filesystem::path& sysfsRootPath,
                         std::shared_ptr<PropertyMap> baseDevConfig);
        // The sysfs root path of the misc device.
        std::filesystem::path sysfsRootPath;
        // The configuration of the base device.
        std::shared_ptr<PropertyMap> baseDevConfig;
        std::unordered_map<int32_t /*batteryId*/, RawBatteryInfo> batteryInfos;
        std::unordered_map<int32_t /*lightId*/, RawLightInfo> lightInfos;
        std::optional<RawLayoutInfo> layoutInfo;

        bool operator==(const AssociatedDevice&) const = default;
        bool operator!=(const AssociatedDevice&) const = default;
        std::string dump() const;
    };

    struct Device {
        int fd; // may be -1 if device is closed
        const int32_t id;
        const std::string path;
        const InputDeviceIdentifier identifier;

        std::unique_ptr<TouchVideoDevice> videoDevice;

        ftl::Flags<InputDeviceClass> classes;

        BitArray<KEY_CNT> keyBitmask;
        BitArray<KEY_CNT> keyState;
        BitArray<REL_CNT> relBitmask;
        BitArray<SW_CNT> swBitmask;
        BitArray<SW_CNT> swState;
        BitArray<LED_CNT> ledBitmask;
        BitArray<FF_CNT> ffBitmask;
        BitArray<INPUT_PROP_CNT> propBitmask;
        BitArray<MSC_CNT> mscBitmask;
        BitArray<ABS_CNT> absBitmask;
        struct AxisState {
            RawAbsoluteAxisInfo info;
            int value;
        };
        std::map<int /*axis*/, AxisState> absState;

        std::string configurationFile;
        std::shared_ptr<PropertyMap> configuration;
        std::unique_ptr<VirtualKeyMap> virtualKeyMap;
        KeyMap keyMap;

        bool ffEffectPlaying;
        int16_t ffEffectId; // initially -1

        // A shared_ptr of a device associated with the input device.
        // The input devices that have the same sysfs path have the same associated device.
        std::shared_ptr<const AssociatedDevice> associatedDevice;

        int32_t controllerNumber;

        Device(int fd, int32_t id, std::string path, InputDeviceIdentifier identifier,
               std::shared_ptr<PropertyMap> config);
        ~Device();

        void close();

        bool enabled; // initially true
        status_t enable();
        status_t disable();
        bool hasValidFd() const;
        const bool isVirtual; // set if fd < 0 is passed to constructor

        const std::shared_ptr<KeyCharacterMap> getKeyCharacterMap() const;

        template <std::size_t N>
        status_t readDeviceBitMask(unsigned long ioctlCode, BitArray<N>& bitArray);

        void configureFd();
        void populateAbsoluteAxisStates();
        bool hasKeycodeLocked(int keycode) const;
        bool hasKeycodeInternalLocked(int keycode) const;
        bool loadVirtualKeyMapLocked();
        status_t loadKeyMapLocked();
        bool isExternalDeviceLocked();
        bool deviceHasMicLocked();
        void setLedForControllerLocked();
        status_t mapLed(int32_t led, int32_t* outScanCode) const;
        void setLedStateLocked(int32_t led, bool on);

        bool currentFrameDropped;
        void trackInputEvent(const struct input_event& event);
        void readDeviceState();
    };

    /**
     * Create a new device for the provided path.
     */
    void openDeviceLocked(const std::string& devicePath) REQUIRES(mLock);
    void openVideoDeviceLocked(const std::string& devicePath) REQUIRES(mLock);
    /**
     * Try to associate a video device with an input device. If the association succeeds,
     * the videoDevice is moved into the input device. 'videoDevice' will become null if this
     * happens.
     * Return true if the association succeeds.
     * Return false otherwise.
     */
    bool tryAddVideoDeviceLocked(Device& device, std::unique_ptr<TouchVideoDevice>& videoDevice)
            REQUIRES(mLock);
    void createVirtualKeyboardLocked() REQUIRES(mLock);
    void addDeviceLocked(std::unique_ptr<Device> device) REQUIRES(mLock);
    void assignDescriptorLocked(InputDeviceIdentifier& identifier) REQUIRES(mLock);
    std::shared_ptr<const AssociatedDevice> obtainAssociatedDeviceLocked(
            const std::filesystem::path& devicePath,
            const std::shared_ptr<PropertyMap>& config) const REQUIRES(mLock);

    void closeDeviceByPathLocked(const std::string& devicePath) REQUIRES(mLock);
    void closeVideoDeviceByPathLocked(const std::string& devicePath) REQUIRES(mLock);
    void closeDeviceLocked(Device& device) REQUIRES(mLock);
    void closeAllDevicesLocked() REQUIRES(mLock);

    status_t registerFdForEpoll(int fd);
    status_t unregisterFdFromEpoll(int fd);
    status_t registerDeviceForEpollLocked(Device& device) REQUIRES(mLock);
    void registerVideoDeviceForEpollLocked(const TouchVideoDevice& videoDevice) REQUIRES(mLock);
    status_t unregisterDeviceFromEpollLocked(Device& device) REQUIRES(mLock);
    void unregisterVideoDeviceFromEpollLocked(const TouchVideoDevice& videoDevice) REQUIRES(mLock);

    status_t scanDirLocked(const std::string& dirname) REQUIRES(mLock);
    status_t scanVideoDirLocked(const std::string& dirname) REQUIRES(mLock);
    void scanDevicesLocked() REQUIRES(mLock);
    base::Result<void> readNotifyLocked() REQUIRES(mLock);
    void handleNotifyEventLocked(const inotify_event&) REQUIRES(mLock);

    Device* getDeviceLocked(int32_t deviceId) const REQUIRES(mLock);
    Device* getDeviceByPathLocked(const std::string& devicePath) const REQUIRES(mLock);
    /**
     * Look through all available fd's (both for input devices and for video devices),
     * and return the device pointer.
     */
    Device* getDeviceByFdLocked(int fd) const REQUIRES(mLock);

    int32_t getNextControllerNumberLocked(const std::string& name) REQUIRES(mLock);

    bool hasDeviceWithDescriptorLocked(const std::string& descriptor) const REQUIRES(mLock);

    void releaseControllerNumberLocked(int32_t num) REQUIRES(mLock);
    void reportDeviceAddedForStatisticsLocked(const InputDeviceIdentifier& identifier,
                                              ftl::Flags<InputDeviceClass> classes) REQUIRES(mLock);

    const std::unordered_map<int32_t, RawBatteryInfo>& getBatteryInfoLocked(int32_t deviceId) const
            REQUIRES(mLock);

    const std::unordered_map<int32_t, RawLightInfo>& getLightInfoLocked(int32_t deviceId) const
            REQUIRES(mLock);

    void addDeviceInputInotify();
    void addDeviceInotify();

    void handleSysfsNodeChangeNotificationsLocked() REQUIRES(mLock);

    // Protect all internal state.
    mutable std::mutex mLock;

    // The actual id of the built-in keyboard, or NO_BUILT_IN_KEYBOARD if none.
    // EventHub remaps the built-in keyboard to id 0 externally as required by the API.
    enum {
        // Must not conflict with any other assigned device ids, including
        // the virtual keyboard id (-1).
        NO_BUILT_IN_KEYBOARD = -2,
    };
    int32_t mBuiltInKeyboardId;

    int32_t mNextDeviceId;

    BitSet32 mControllerNumbers;

    std::unordered_map<int32_t, std::unique_ptr<Device>> mDevices;
    /**
     * Video devices that report touchscreen heatmap, but have not (yet) been paired
     * with a specific input device. Video device discovery is independent from input device
     * discovery, so the two types of devices could be found in any order.
     * Ideally, video devices in this queue do not have an open fd, or at least aren't
     * actively streaming.
     */
    std::vector<std::unique_ptr<TouchVideoDevice>> mUnattachedVideoDevices;

    std::vector<std::unique_ptr<Device>> mOpeningDevices;
    std::vector<std::unique_ptr<Device>> mClosingDevices;

    bool mNeedToReopenDevices;
    bool mNeedToScanDevices;
    std::vector<std::string> mExcludedDevices;
    std::vector<int32_t> mDeviceIdsToReopen;

    int mEpollFd;
    int mINotifyFd;
    int mWakeReadPipeFd;
    int mWakeWritePipeFd;

    int mDeviceInputWd;
    int mDeviceWd = -1;

    // Maximum number of signalled FDs to handle at a time.
    static const int EPOLL_MAX_EVENTS = 16;

    // The array of pending epoll events and the index of the next event to be handled.
    struct epoll_event mPendingEventItems[EPOLL_MAX_EVENTS];
    size_t mPendingEventCount;
    size_t mPendingEventIndex;
    bool mPendingINotify;

    // The sysfs node change notifications that have been sent to EventHub.
    // Enqueuing notifications does not require the lock to be held.
    BlockingQueue<std::string> mChangedSysfsNodeNotifications;
};

} // namespace android
