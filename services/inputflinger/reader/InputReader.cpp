/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "Macros.h"

#include "InputReader.h"

#include <android-base/stringprintf.h>
#include <com_android_input_flags.h>
#include <errno.h>
#include <input/Keyboard.h>
#include <input/VirtualKeyMap.h>
#include <inttypes.h>
#include <limits.h>
#include <log/log.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <utils/Errors.h>
#include <utils/Thread.h>
#include <string>

#include "InputDevice.h"
#include "include/gestures.h"

using android::base::StringPrintf;

namespace android {

namespace {

/**
 * Determines if the identifiers passed are a sub-devices. Sub-devices are physical devices
 * that expose multiple input device paths such a keyboard that also has a touchpad input.
 * These are separate devices with unique descriptors in EventHub, but InputReader should
 * create a single InputDevice for them.
 * Sub-devices are detected by the following criteria:
 * 1. The vendor, product, bus, version, and unique id match
 * 2. The location matches. The location is used to distinguish a single device with multiple
 *    inputs versus the same device plugged into multiple ports.
 */

bool isSubDevice(const InputDeviceIdentifier& identifier1,
                 const InputDeviceIdentifier& identifier2) {
    return (identifier1.vendor == identifier2.vendor &&
            identifier1.product == identifier2.product && identifier1.bus == identifier2.bus &&
            identifier1.version == identifier2.version &&
            identifier1.uniqueId == identifier2.uniqueId &&
            identifier1.location == identifier2.location);
}

/**
 * Determines if the device classes passed for two devices represent incompatible combinations
 * that should not be merged into into a single InputDevice.
 */

bool isCompatibleSubDevice(ftl::Flags<InputDeviceClass> classes1,
                           ftl::Flags<InputDeviceClass> classes2) {
    if (!com::android::input::flags::prevent_merging_input_pointer_devices()) {
        return true;
    }

    const ftl::Flags<InputDeviceClass> pointerFlags =
            ftl::Flags<InputDeviceClass>{InputDeviceClass::TOUCH, InputDeviceClass::TOUCH_MT,
                                         InputDeviceClass::CURSOR, InputDeviceClass::TOUCHPAD};

    // Do not merge devices that both have any type of pointer event.
    if (classes1.any(pointerFlags) && classes2.any(pointerFlags)) return false;

    // Safe to merge
    return true;
}

bool isStylusPointerGestureStart(const NotifyMotionArgs& motionArgs) {
    const auto actionMasked = MotionEvent::getActionMasked(motionArgs.action);
    if (actionMasked != AMOTION_EVENT_ACTION_HOVER_ENTER &&
        actionMasked != AMOTION_EVENT_ACTION_DOWN &&
        actionMasked != AMOTION_EVENT_ACTION_POINTER_DOWN) {
        return false;
    }
    const auto actionIndex = MotionEvent::getActionIndex(motionArgs.action);
    return isStylusToolType(motionArgs.pointerProperties[actionIndex].toolType);
}

bool isNewGestureStart(const NotifyMotionArgs& motion) {
    return motion.action == AMOTION_EVENT_ACTION_DOWN ||
            motion.action == AMOTION_EVENT_ACTION_HOVER_ENTER;
}

bool isNewGestureStart(const NotifyKeyArgs& key) {
    return key.action == AKEY_EVENT_ACTION_DOWN;
}

// Return the event's device ID if it marks the start of a new gesture.
std::optional<DeviceId> getDeviceIdOfNewGesture(const NotifyArgs& args) {
    if (const auto* motion = std::get_if<NotifyMotionArgs>(&args); motion != nullptr) {
        return isNewGestureStart(*motion) ? std::make_optional(motion->deviceId) : std::nullopt;
    }
    if (const auto* key = std::get_if<NotifyKeyArgs>(&args); key != nullptr) {
        return isNewGestureStart(*key) ? std::make_optional(key->deviceId) : std::nullopt;
    }
    return std::nullopt;
}

} // namespace

// --- InputReader ---

InputReader::InputReader(std::shared_ptr<EventHubInterface> eventHub,
                         const sp<InputReaderPolicyInterface>& policy,
                         InputListenerInterface& listener)
      : mContext(this),
        mEventHub(eventHub),
        mPolicy(policy),
        mNextListener(listener),
        mKeyboardClassifier(std::make_unique<KeyboardClassifier>()),
        mGlobalMetaState(AMETA_NONE),
        mLedMetaState(AMETA_NONE),
        mGeneration(1),
        mNextInputDeviceId(END_RESERVED_ID),
        mDisableVirtualKeysTimeout(LLONG_MIN),
        mNextTimeout(LLONG_MAX),
        mConfigurationChangesToRefresh(0) {
    refreshConfigurationLocked(/*changes=*/{});
    updateGlobalMetaStateLocked();
}

InputReader::~InputReader() {}

status_t InputReader::start() {
    if (mThread) {
        return ALREADY_EXISTS;
    }
    mThread = std::make_unique<InputThread>(
            "InputReader", [this]() { loopOnce(); }, [this]() { mEventHub->wake(); },
            /*isInCriticalPath=*/true);
    return OK;
}

status_t InputReader::stop() {
    if (mThread && mThread->isCallingThread()) {
        ALOGE("InputReader cannot be stopped from its own thread!");
        return INVALID_OPERATION;
    }
    mThread.reset();
    return OK;
}

void InputReader::loopOnce() {
    int32_t oldGeneration;
    int32_t timeoutMillis;
    // Copy some state so that we can access it outside the lock later.
    bool inputDevicesChanged = false;
    std::vector<InputDeviceInfo> inputDevices;
    std::list<NotifyArgs> notifyArgs;
    { // acquire lock
        std::scoped_lock _l(mLock);

        oldGeneration = mGeneration;
        timeoutMillis = -1;

        auto changes = mConfigurationChangesToRefresh;
        if (changes.any()) {
            mConfigurationChangesToRefresh.clear();
            timeoutMillis = 0;
            refreshConfigurationLocked(changes);
        } else if (mNextTimeout != LLONG_MAX) {
            nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
            timeoutMillis = toMillisecondTimeoutDelay(now, mNextTimeout);
        }
    } // release lock

    std::vector<RawEvent> events = mEventHub->getEvents(timeoutMillis);

    { // acquire lock
        std::scoped_lock _l(mLock);
        mReaderIsAliveCondition.notify_all();

        if (!events.empty()) {
            mPendingArgs += processEventsLocked(events.data(), events.size());
        }

        if (mNextTimeout != LLONG_MAX) {
            nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
            if (now >= mNextTimeout) {
                ALOGD_IF(debugRawEvents(), "Timeout expired, latency=%0.3fms",
                         (now - mNextTimeout) * 0.000001f);
                mNextTimeout = LLONG_MAX;
                mPendingArgs += timeoutExpiredLocked(now);
            }
        }

        if (oldGeneration != mGeneration) {
            // Reset global meta state because it depends on connected input devices.
            updateGlobalMetaStateLocked();

            inputDevicesChanged = true;
            inputDevices = getInputDevicesLocked();
            mPendingArgs.emplace_back(
                    NotifyInputDevicesChangedArgs{mContext.getNextId(), inputDevices});
        }

        std::swap(notifyArgs, mPendingArgs);

        // Keep track of the last used device
        for (const NotifyArgs& args : notifyArgs) {
            mLastUsedDeviceId = getDeviceIdOfNewGesture(args).value_or(mLastUsedDeviceId);
        }
    } // release lock

    // Flush queued events out to the listener.
    // This must happen outside of the lock because the listener could potentially call
    // back into the InputReader's methods, such as getScanCodeState, or become blocked
    // on another thread similarly waiting to acquire the InputReader lock thereby
    // resulting in a deadlock.  This situation is actually quite plausible because the
    // listener is actually the input dispatcher, which calls into the window manager,
    // which occasionally calls into the input reader.
    for (const NotifyArgs& args : notifyArgs) {
        mNextListener.notify(args);
    }

    // Notify the policy that input devices have changed.
    // This must be done after flushing events down the listener chain to ensure that the rest of
    // the listeners are synchronized with the changes before the policy reacts to them.
    if (inputDevicesChanged) {
        mPolicy->notifyInputDevicesChanged(inputDevices);
    }

    // Notify the policy of the start of every new stylus gesture.
    for (const auto& args : notifyArgs) {
        const auto* motionArgs = std::get_if<NotifyMotionArgs>(&args);
        if (motionArgs != nullptr && isStylusPointerGestureStart(*motionArgs)) {
            mPolicy->notifyStylusGestureStarted(motionArgs->deviceId, motionArgs->eventTime);
        }
    }
}

std::list<NotifyArgs> InputReader::processEventsLocked(const RawEvent* rawEvents, size_t count) {
    std::list<NotifyArgs> out;
    for (const RawEvent* rawEvent = rawEvents; count;) {
        int32_t type = rawEvent->type;
        size_t batchSize = 1;
        if (type < EventHubInterface::FIRST_SYNTHETIC_EVENT) {
            int32_t deviceId = rawEvent->deviceId;
            while (batchSize < count) {
                if (rawEvent[batchSize].type >= EventHubInterface::FIRST_SYNTHETIC_EVENT ||
                    rawEvent[batchSize].deviceId != deviceId) {
                    break;
                }
                batchSize += 1;
            }
            ALOGD_IF(debugRawEvents(), "BatchSize: %zu Count: %zu", batchSize, count);
            out += processEventsForDeviceLocked(deviceId, rawEvent, batchSize);
        } else {
            switch (rawEvent->type) {
                case EventHubInterface::DEVICE_ADDED:
                    addDeviceLocked(rawEvent->when, rawEvent->deviceId);
                    break;
                case EventHubInterface::DEVICE_REMOVED:
                    removeDeviceLocked(rawEvent->when, rawEvent->deviceId);
                    break;
                default:
                    ALOG_ASSERT(false); // can't happen
                    break;
            }
        }
        count -= batchSize;
        rawEvent += batchSize;
    }
    return out;
}

void InputReader::addDeviceLocked(nsecs_t when, int32_t eventHubId) {
    if (mDevices.find(eventHubId) != mDevices.end()) {
        ALOGW("Ignoring spurious device added event for eventHubId %d.", eventHubId);
        return;
    }

    InputDeviceIdentifier identifier = mEventHub->getDeviceIdentifier(eventHubId);
    ftl::Flags<InputDeviceClass> classes = mEventHub->getDeviceClasses(eventHubId);
    std::shared_ptr<InputDevice> device = createDeviceLocked(when, eventHubId, identifier, classes);

    mPendingArgs += device->configure(when, mConfig, /*changes=*/{});
    mPendingArgs += device->reset(when);

    if (device->isIgnored()) {
        ALOGI("Device added: id=%d, eventHubId=%d, name='%s', descriptor='%s' "
              "(ignored non-input device)",
              device->getId(), eventHubId, identifier.name.c_str(), identifier.descriptor.c_str());
    } else {
        ALOGI("Device added: id=%d, eventHubId=%d, name='%s', descriptor='%s',sources=%s",
              device->getId(), eventHubId, identifier.name.c_str(), identifier.descriptor.c_str(),
              inputEventSourceToString(device->getSources()).c_str());
    }

    mDevices.emplace(eventHubId, device);
    // Add device to device to EventHub ids map.
    const auto mapIt = mDeviceToEventHubIdsMap.find(device);
    if (mapIt == mDeviceToEventHubIdsMap.end()) {
        std::vector<int32_t> ids = {eventHubId};
        mDeviceToEventHubIdsMap.emplace(device, ids);
    } else {
        mapIt->second.push_back(eventHubId);
    }
    bumpGenerationLocked();

    if (device->getClasses().test(InputDeviceClass::EXTERNAL_STYLUS)) {
        notifyExternalStylusPresenceChangedLocked();
    }

    // Sensor input device is noisy, to save power disable it by default.
    // Input device is classified as SENSOR when any sub device is a SENSOR device, check Eventhub
    // device class to disable SENSOR sub device only.
    if (mEventHub->getDeviceClasses(eventHubId).test(InputDeviceClass::SENSOR)) {
        mEventHub->disableDevice(eventHubId);
    }
}

void InputReader::removeDeviceLocked(nsecs_t when, int32_t eventHubId) {
    auto deviceIt = mDevices.find(eventHubId);
    if (deviceIt == mDevices.end()) {
        ALOGW("Ignoring spurious device removed event for eventHubId %d.", eventHubId);
        return;
    }

    std::shared_ptr<InputDevice> device = std::move(deviceIt->second);
    mDevices.erase(deviceIt);
    // Erase device from device to EventHub ids map.
    auto mapIt = mDeviceToEventHubIdsMap.find(device);
    if (mapIt != mDeviceToEventHubIdsMap.end()) {
        std::vector<int32_t>& eventHubIds = mapIt->second;
        std::erase_if(eventHubIds, [eventHubId](int32_t eId) { return eId == eventHubId; });
        if (eventHubIds.size() == 0) {
            mDeviceToEventHubIdsMap.erase(mapIt);
        }
    }
    bumpGenerationLocked();

    if (device->isIgnored()) {
        ALOGI("Device removed: id=%d, eventHubId=%d, name='%s', descriptor='%s' "
              "(ignored non-input device)",
              device->getId(), eventHubId, device->getName().c_str(),
              device->getDescriptor().c_str());
    } else {
        ALOGI("Device removed: id=%d, eventHubId=%d, name='%s', descriptor='%s', sources=%s",
              device->getId(), eventHubId, device->getName().c_str(),
              device->getDescriptor().c_str(),
              inputEventSourceToString(device->getSources()).c_str());
    }

    device->removeEventHubDevice(eventHubId);

    if (device->getClasses().test(InputDeviceClass::EXTERNAL_STYLUS)) {
        notifyExternalStylusPresenceChangedLocked();
    }

    if (device->hasEventHubDevices()) {
        mPendingArgs += device->configure(when, mConfig, /*changes=*/{});
    }
    mPendingArgs += device->reset(when);
}

std::shared_ptr<InputDevice> InputReader::createDeviceLocked(
        nsecs_t when, int32_t eventHubId, const InputDeviceIdentifier& identifier,
        ftl::Flags<InputDeviceClass> classes) {
    auto deviceIt =
            std::find_if(mDevices.begin(), mDevices.end(), [identifier, classes](auto& devicePair) {
                const InputDeviceIdentifier identifier2 =
                        devicePair.second->getDeviceInfo().getIdentifier();
                const ftl::Flags<InputDeviceClass> classes2 = devicePair.second->getClasses();
                return isSubDevice(identifier, identifier2) &&
                        isCompatibleSubDevice(classes, classes2);
            });

    std::shared_ptr<InputDevice> device;
    if (deviceIt != mDevices.end()) {
        device = deviceIt->second;
    } else {
        int32_t deviceId = (eventHubId < END_RESERVED_ID) ? eventHubId : nextInputDeviceIdLocked();
        device = std::make_shared<InputDevice>(&mContext, deviceId, bumpGenerationLocked(),
                                               identifier);
    }
    mPendingArgs += device->addEventHubDevice(when, eventHubId, mConfig);
    return device;
}

std::list<NotifyArgs> InputReader::processEventsForDeviceLocked(int32_t eventHubId,
                                                                const RawEvent* rawEvents,
                                                                size_t count) {
    auto deviceIt = mDevices.find(eventHubId);
    if (deviceIt == mDevices.end()) {
        ALOGW("Discarding event for unknown eventHubId %d.", eventHubId);
        return {};
    }

    std::shared_ptr<InputDevice>& device = deviceIt->second;
    if (device->isIgnored()) {
        // ALOGD("Discarding event for ignored deviceId %d.", deviceId);
        return {};
    }

    return device->process(rawEvents, count);
}

InputDevice* InputReader::findInputDeviceLocked(int32_t deviceId) const {
    auto deviceIt =
            std::find_if(mDevices.begin(), mDevices.end(), [deviceId](const auto& devicePair) {
                return devicePair.second->getId() == deviceId;
            });
    if (deviceIt != mDevices.end()) {
        return deviceIt->second.get();
    }
    return nullptr;
}

std::list<NotifyArgs> InputReader::timeoutExpiredLocked(nsecs_t when) {
    std::list<NotifyArgs> out;
    for (auto& devicePair : mDevices) {
        std::shared_ptr<InputDevice>& device = devicePair.second;
        if (!device->isIgnored()) {
            out += device->timeoutExpired(when);
        }
    }
    return out;
}

int32_t InputReader::nextInputDeviceIdLocked() {
    return ++mNextInputDeviceId;
}

void InputReader::refreshConfigurationLocked(ConfigurationChanges changes) {
    mPolicy->getReaderConfiguration(&mConfig);
    mEventHub->setExcludedDevices(mConfig.excludedDeviceNames);

    using Change = InputReaderConfiguration::Change;
    if (!changes.any()) return;

    ALOGI("Reconfiguring input devices, changes=%s", changes.string().c_str());
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);

    if (changes.test(Change::MUST_REOPEN)) {
        mEventHub->requestReopenDevices();
    } else {
        for (auto& devicePair : mDevices) {
            std::shared_ptr<InputDevice>& device = devicePair.second;
            mPendingArgs += device->configure(now, mConfig, changes);
        }
    }

    if (changes.test(Change::POINTER_CAPTURE)) {
        if (mCurrentPointerCaptureRequest == mConfig.pointerCaptureRequest) {
            ALOGV("Skipping notifying pointer capture changes: "
                  "There was no change in the pointer capture state.");
        } else {
            mCurrentPointerCaptureRequest = mConfig.pointerCaptureRequest;
            mPendingArgs.emplace_back(
                    NotifyPointerCaptureChangedArgs{mContext.getNextId(), now,
                                                    mCurrentPointerCaptureRequest});
        }
    }
}

void InputReader::updateGlobalMetaStateLocked() {
    mGlobalMetaState = 0;

    for (auto& devicePair : mDevices) {
        std::shared_ptr<InputDevice>& device = devicePair.second;
        mGlobalMetaState |= device->getMetaState();
    }
}

int32_t InputReader::getGlobalMetaStateLocked() {
    return mGlobalMetaState;
}

void InputReader::updateLedMetaStateLocked(int32_t metaState) {
    mLedMetaState = metaState;
    for (auto& devicePair : mDevices) {
        std::shared_ptr<InputDevice>& device = devicePair.second;
        device->updateLedState(false);
    }
}

int32_t InputReader::getLedMetaStateLocked() {
    return mLedMetaState;
}

void InputReader::notifyExternalStylusPresenceChangedLocked() {
    refreshConfigurationLocked(InputReaderConfiguration::Change::EXTERNAL_STYLUS_PRESENCE);
}

void InputReader::getExternalStylusDevicesLocked(std::vector<InputDeviceInfo>& outDevices) {
    for (auto& devicePair : mDevices) {
        std::shared_ptr<InputDevice>& device = devicePair.second;
        if (device->getClasses().test(InputDeviceClass::EXTERNAL_STYLUS) && !device->isIgnored()) {
            outDevices.push_back(device->getDeviceInfo());
        }
    }
}

std::list<NotifyArgs> InputReader::dispatchExternalStylusStateLocked(const StylusState& state) {
    std::list<NotifyArgs> out;
    for (auto& devicePair : mDevices) {
        std::shared_ptr<InputDevice>& device = devicePair.second;
        out += device->updateExternalStylusState(state);
    }
    return out;
}

void InputReader::disableVirtualKeysUntilLocked(nsecs_t time) {
    mDisableVirtualKeysTimeout = time;
}

bool InputReader::shouldDropVirtualKeyLocked(nsecs_t now, int32_t keyCode, int32_t scanCode) {
    if (now < mDisableVirtualKeysTimeout) {
        ALOGI("Dropping virtual key from device because virtual keys are "
              "temporarily disabled for the next %0.3fms.  keyCode=%d, scanCode=%d",
              (mDisableVirtualKeysTimeout - now) * 0.000001, keyCode, scanCode);
        return true;
    } else {
        return false;
    }
}

void InputReader::requestTimeoutAtTimeLocked(nsecs_t when) {
    if (when < mNextTimeout) {
        mNextTimeout = when;
        mEventHub->wake();
    }
}

int32_t InputReader::bumpGenerationLocked() {
    return ++mGeneration;
}

std::vector<InputDeviceInfo> InputReader::getInputDevices() const {
    std::scoped_lock _l(mLock);
    return getInputDevicesLocked();
}

std::vector<InputDeviceInfo> InputReader::getInputDevicesLocked() const {
    std::vector<InputDeviceInfo> outInputDevices;
    outInputDevices.reserve(mDeviceToEventHubIdsMap.size());

    for (const auto& [device, eventHubIds] : mDeviceToEventHubIdsMap) {
        if (!device->isIgnored()) {
            outInputDevices.push_back(device->getDeviceInfo());
        }
    }
    return outInputDevices;
}

int32_t InputReader::getKeyCodeState(int32_t deviceId, uint32_t sourceMask, int32_t keyCode) {
    std::scoped_lock _l(mLock);

    return getStateLocked(deviceId, sourceMask, keyCode, &InputDevice::getKeyCodeState);
}

int32_t InputReader::getScanCodeState(int32_t deviceId, uint32_t sourceMask, int32_t scanCode) {
    std::scoped_lock _l(mLock);

    return getStateLocked(deviceId, sourceMask, scanCode, &InputDevice::getScanCodeState);
}

int32_t InputReader::getSwitchState(int32_t deviceId, uint32_t sourceMask, int32_t switchCode) {
    std::scoped_lock _l(mLock);

    return getStateLocked(deviceId, sourceMask, switchCode, &InputDevice::getSwitchState);
}

int32_t InputReader::getStateLocked(int32_t deviceId, uint32_t sourceMask, int32_t code,
                                    GetStateFunc getStateFunc) {
    int32_t result = AKEY_STATE_UNKNOWN;
    if (deviceId >= 0) {
        InputDevice* device = findInputDeviceLocked(deviceId);
        if (device && !device->isIgnored() && sourcesMatchMask(device->getSources(), sourceMask)) {
            result = (device->*getStateFunc)(sourceMask, code);
        }
    } else {
        for (auto& devicePair : mDevices) {
            std::shared_ptr<InputDevice>& device = devicePair.second;
            if (!device->isIgnored() && sourcesMatchMask(device->getSources(), sourceMask)) {
                // If any device reports AKEY_STATE_DOWN or AKEY_STATE_VIRTUAL, return that
                // value.  Otherwise, return AKEY_STATE_UP as long as one device reports it.
                int32_t currentResult = (device.get()->*getStateFunc)(sourceMask, code);
                if (currentResult >= AKEY_STATE_DOWN) {
                    return currentResult;
                } else if (currentResult == AKEY_STATE_UP) {
                    result = currentResult;
                }
            }
        }
    }
    return result;
}

void InputReader::toggleCapsLockState(int32_t deviceId) {
    std::scoped_lock _l(mLock);
    if (mKeyboardClassifier->getKeyboardType(deviceId) == KeyboardType::ALPHABETIC) {
        updateLedMetaStateLocked(mLedMetaState ^ AMETA_CAPS_LOCK_ON);
    }
}

void InputReader::resetLockedModifierState() {
    std::scoped_lock _l(mLock);
    updateLedMetaStateLocked(0);
}

bool InputReader::hasKeys(int32_t deviceId, uint32_t sourceMask,
                          const std::vector<int32_t>& keyCodes, uint8_t* outFlags) {
    std::scoped_lock _l(mLock);

    memset(outFlags, 0, keyCodes.size());
    return markSupportedKeyCodesLocked(deviceId, sourceMask, keyCodes, outFlags);
}

bool InputReader::markSupportedKeyCodesLocked(int32_t deviceId, uint32_t sourceMask,
                                              const std::vector<int32_t>& keyCodes,
                                              uint8_t* outFlags) {
    bool result = false;
    if (deviceId >= 0) {
        InputDevice* device = findInputDeviceLocked(deviceId);
        if (device && !device->isIgnored() && sourcesMatchMask(device->getSources(), sourceMask)) {
            result = device->markSupportedKeyCodes(sourceMask, keyCodes, outFlags);
        }
    } else {
        for (auto& devicePair : mDevices) {
            std::shared_ptr<InputDevice>& device = devicePair.second;
            if (!device->isIgnored() && sourcesMatchMask(device->getSources(), sourceMask)) {
                result |= device->markSupportedKeyCodes(sourceMask, keyCodes, outFlags);
            }
        }
    }
    return result;
}

int32_t InputReader::getKeyCodeForKeyLocation(int32_t deviceId, int32_t locationKeyCode) const {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device == nullptr) {
        ALOGW("Failed to get key code for key location: Input device with id %d not found",
              deviceId);
        return AKEYCODE_UNKNOWN;
    }
    return device->getKeyCodeForKeyLocation(locationKeyCode);
}

void InputReader::requestRefreshConfiguration(ConfigurationChanges changes) {
    std::scoped_lock _l(mLock);

    if (changes.any()) {
        bool needWake = !mConfigurationChangesToRefresh.any();
        mConfigurationChangesToRefresh |= changes;

        if (needWake) {
            mEventHub->wake();
        }
    }
}

void InputReader::vibrate(int32_t deviceId, const VibrationSequence& sequence, ssize_t repeat,
                          int32_t token) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        mPendingArgs += device->vibrate(sequence, repeat, token);
    }
}

void InputReader::cancelVibrate(int32_t deviceId, int32_t token) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        mPendingArgs += device->cancelVibrate(token);
    }
}

bool InputReader::isVibrating(int32_t deviceId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        return device->isVibrating();
    }
    return false;
}

std::vector<int32_t> InputReader::getVibratorIds(int32_t deviceId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        return device->getVibratorIds();
    }
    return {};
}

void InputReader::disableSensor(int32_t deviceId, InputDeviceSensorType sensorType) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        device->disableSensor(sensorType);
    }
}

bool InputReader::enableSensor(int32_t deviceId, InputDeviceSensorType sensorType,
                               std::chrono::microseconds samplingPeriod,
                               std::chrono::microseconds maxBatchReportLatency) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        return device->enableSensor(sensorType, samplingPeriod, maxBatchReportLatency);
    }
    return false;
}

void InputReader::flushSensor(int32_t deviceId, InputDeviceSensorType sensorType) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        device->flushSensor(sensorType);
    }
}

std::optional<int32_t> InputReader::getBatteryCapacity(int32_t deviceId) {
    std::optional<int32_t> eventHubId;
    {
        // Do not query the battery state while holding the lock. For some peripheral devices,
        // reading battery state can be broken and take 5+ seconds. Holding the lock in this case
        // would block all other event processing during this time. For now, we assume this
        // call never happens on the InputReader thread and get the battery state outside the
        // lock to prevent event processing from being blocked by this call.
        std::scoped_lock _l(mLock);
        InputDevice* device = findInputDeviceLocked(deviceId);
        if (!device) return {};
        eventHubId = device->getBatteryEventHubId();
    } // release lock

    if (!eventHubId) return {};
    const auto batteryIds = mEventHub->getRawBatteryIds(*eventHubId);
    if (batteryIds.empty()) {
        ALOGW("%s: There are no battery ids for EventHub device %d", __func__, *eventHubId);
        return {};
    }
    return mEventHub->getBatteryCapacity(*eventHubId, batteryIds.front());
}

std::optional<int32_t> InputReader::getBatteryStatus(int32_t deviceId) {
    std::optional<int32_t> eventHubId;
    {
        // Do not query the battery state while holding the lock. For some peripheral devices,
        // reading battery state can be broken and take 5+ seconds. Holding the lock in this case
        // would block all other event processing during this time. For now, we assume this
        // call never happens on the InputReader thread and get the battery state outside the
        // lock to prevent event processing from being blocked by this call.
        std::scoped_lock _l(mLock);
        InputDevice* device = findInputDeviceLocked(deviceId);
        if (!device) return {};
        eventHubId = device->getBatteryEventHubId();
    } // release lock

    if (!eventHubId) return {};
    const auto batteryIds = mEventHub->getRawBatteryIds(*eventHubId);
    if (batteryIds.empty()) {
        ALOGW("%s: There are no battery ids for EventHub device %d", __func__, *eventHubId);
        return {};
    }
    return mEventHub->getBatteryStatus(*eventHubId, batteryIds.front());
}

std::optional<std::string> InputReader::getBatteryDevicePath(int32_t deviceId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (!device) return {};

    std::optional<int32_t> eventHubId = device->getBatteryEventHubId();
    if (!eventHubId) return {};
    const auto batteryIds = mEventHub->getRawBatteryIds(*eventHubId);
    if (batteryIds.empty()) {
        ALOGW("%s: There are no battery ids for EventHub device %d", __func__, *eventHubId);
        return {};
    }
    const auto batteryInfo = mEventHub->getRawBatteryInfo(*eventHubId, batteryIds.front());
    if (!batteryInfo) {
        ALOGW("%s: Failed to get RawBatteryInfo for battery %d of EventHub device %d", __func__,
              batteryIds.front(), *eventHubId);
        return {};
    }
    return batteryInfo->path;
}

std::vector<InputDeviceLightInfo> InputReader::getLights(int32_t deviceId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device == nullptr) {
        return {};
    }

    return device->getDeviceInfo().getLights();
}

std::vector<InputDeviceSensorInfo> InputReader::getSensors(int32_t deviceId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device == nullptr) {
        return {};
    }

    return device->getDeviceInfo().getSensors();
}

std::optional<HardwareProperties> InputReader::getTouchpadHardwareProperties(int32_t deviceId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);

    if (device == nullptr) {
        return {};
    }

    return device->getTouchpadHardwareProperties();
}

bool InputReader::setLightColor(int32_t deviceId, int32_t lightId, int32_t color) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        return device->setLightColor(lightId, color);
    }
    return false;
}

bool InputReader::setLightPlayerId(int32_t deviceId, int32_t lightId, int32_t playerId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        return device->setLightPlayerId(lightId, playerId);
    }
    return false;
}

std::optional<int32_t> InputReader::getLightColor(int32_t deviceId, int32_t lightId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        return device->getLightColor(lightId);
    }
    return std::nullopt;
}

std::optional<int32_t> InputReader::getLightPlayerId(int32_t deviceId, int32_t lightId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        return device->getLightPlayerId(lightId);
    }
    return std::nullopt;
}

std::optional<std::string> InputReader::getBluetoothAddress(int32_t deviceId) const {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        return device->getBluetoothAddress();
    }
    return std::nullopt;
}

bool InputReader::canDispatchToDisplay(int32_t deviceId, ui::LogicalDisplayId displayId) {
    std::scoped_lock _l(mLock);

    InputDevice* device = findInputDeviceLocked(deviceId);
    if (!device) {
        ALOGW("Ignoring invalid device id %" PRId32 ".", deviceId);
        return false;
    }

    if (!device->isEnabled()) {
        ALOGW("Ignoring disabled device %s", device->getName().c_str());
        return false;
    }

    std::optional<ui::LogicalDisplayId> associatedDisplayId = device->getAssociatedDisplayId();
    // No associated display. By default, can dispatch to all displays.
    if (!associatedDisplayId || !associatedDisplayId->isValid()) {
        return true;
    }

    return *associatedDisplayId == displayId;
}

std::filesystem::path InputReader::getSysfsRootPath(int32_t deviceId) const {
    std::scoped_lock _l(mLock);

    const InputDevice* device = findInputDeviceLocked(deviceId);
    if (!device) {
        return {};
    }
    return device->getSysfsRootPath();
}

void InputReader::sysfsNodeChanged(const std::string& sysfsNodePath) {
    mEventHub->sysfsNodeChanged(sysfsNodePath);
    mEventHub->wake();
}

DeviceId InputReader::getLastUsedInputDeviceId() {
    std::scoped_lock _l(mLock);
    return mLastUsedDeviceId;
}

void InputReader::notifyMouseCursorFadedOnTyping() {
    std::scoped_lock _l(mLock);
    // disable touchpad taps when cursor has faded due to typing
    mPreventingTouchpadTaps = true;
}

bool InputReader::setKernelWakeEnabled(int32_t deviceId, bool enabled) {
    std::scoped_lock _l(mLock);
    if (!com::android::input::flags::set_input_device_kernel_wake()){
        return false;
    }
    InputDevice* device = findInputDeviceLocked(deviceId);
    if (device) {
        return device->setKernelWakeEnabled(enabled);
    }
    return false;
}

void InputReader::dump(std::string& dump) {
    std::scoped_lock _l(mLock);
    dumpLocked(dump);
}

void InputReader::dumpLocked(std::string& dump) {
    mEventHub->dump(dump);
    dump += "\n";

    dump += StringPrintf("Input Reader State (Nums of device: %zu):\n",
                         mDeviceToEventHubIdsMap.size());

    for (const auto& devicePair : mDeviceToEventHubIdsMap) {
        const std::shared_ptr<InputDevice>& device = devicePair.first;
        std::string eventHubDevStr = INDENT "EventHub Devices: [ ";
        for (const auto& eId : devicePair.second) {
            eventHubDevStr += StringPrintf("%d ", eId);
        }
        eventHubDevStr += "] \n";
        device->dump(dump, eventHubDevStr);
    }

    dump += StringPrintf(INDENT "NextTimeout: %" PRId64 "\n", mNextTimeout);
    dump += INDENT "Configuration:\n";
    dump += INDENT2 "ExcludedDeviceNames: [";
    for (size_t i = 0; i < mConfig.excludedDeviceNames.size(); i++) {
        if (i != 0) {
            dump += ", ";
        }
        dump += mConfig.excludedDeviceNames[i];
    }
    dump += "]\n";
    dump += StringPrintf(INDENT2 "VirtualKeyQuietTime: %0.1fms\n",
                         mConfig.virtualKeyQuietTime * 0.000001f);

    dump += StringPrintf(INDENT2 "PointerVelocityControlParameters: "
                                 "scale=%0.3f, lowThreshold=%0.3f, highThreshold=%0.3f, "
                                 "acceleration=%0.3f\n",
                         mConfig.pointerVelocityControlParameters.scale,
                         mConfig.pointerVelocityControlParameters.lowThreshold,
                         mConfig.pointerVelocityControlParameters.highThreshold,
                         mConfig.pointerVelocityControlParameters.acceleration);

    dump += StringPrintf(INDENT2 "WheelVelocityControlParameters: "
                                 "scale=%0.3f, lowThreshold=%0.3f, highThreshold=%0.3f, "
                                 "acceleration=%0.3f\n",
                         mConfig.wheelVelocityControlParameters.scale,
                         mConfig.wheelVelocityControlParameters.lowThreshold,
                         mConfig.wheelVelocityControlParameters.highThreshold,
                         mConfig.wheelVelocityControlParameters.acceleration);

    dump += StringPrintf(INDENT2 "PointerGesture:\n");
    dump += StringPrintf(INDENT3 "Enabled: %s\n", toString(mConfig.pointerGesturesEnabled));
    dump += StringPrintf(INDENT3 "QuietInterval: %0.1fms\n",
                         mConfig.pointerGestureQuietInterval * 0.000001f);
    dump += StringPrintf(INDENT3 "DragMinSwitchSpeed: %0.1fpx/s\n",
                         mConfig.pointerGestureDragMinSwitchSpeed);
    dump += StringPrintf(INDENT3 "TapInterval: %0.1fms\n",
                         mConfig.pointerGestureTapInterval * 0.000001f);
    dump += StringPrintf(INDENT3 "TapDragInterval: %0.1fms\n",
                         mConfig.pointerGestureTapDragInterval * 0.000001f);
    dump += StringPrintf(INDENT3 "TapSlop: %0.1fpx\n", mConfig.pointerGestureTapSlop);
    dump += StringPrintf(INDENT3 "MultitouchSettleInterval: %0.1fms\n",
                         mConfig.pointerGestureMultitouchSettleInterval * 0.000001f);
    dump += StringPrintf(INDENT3 "MultitouchMinDistance: %0.1fpx\n",
                         mConfig.pointerGestureMultitouchMinDistance);
    dump += StringPrintf(INDENT3 "SwipeTransitionAngleCosine: %0.1f\n",
                         mConfig.pointerGestureSwipeTransitionAngleCosine);
    dump += StringPrintf(INDENT3 "SwipeMaxWidthRatio: %0.1f\n",
                         mConfig.pointerGestureSwipeMaxWidthRatio);
    dump += StringPrintf(INDENT3 "MovementSpeedRatio: %0.1f\n",
                         mConfig.pointerGestureMovementSpeedRatio);
    dump += StringPrintf(INDENT3 "ZoomSpeedRatio: %0.1f\n", mConfig.pointerGestureZoomSpeedRatio);

    dump += INDENT3 "Viewports:\n";
    mConfig.dump(dump);
}

void InputReader::monitor() {
    // Acquire and release the lock to ensure that the reader has not deadlocked.
    std::unique_lock<std::mutex> lock(mLock);
    mEventHub->wake();
    mReaderIsAliveCondition.wait(lock);
    // Check the EventHub
    mEventHub->monitor();
}

// --- InputReader::ContextImpl ---

InputReader::ContextImpl::ContextImpl(InputReader* reader)
      : mReader(reader), mIdGenerator(IdGenerator::Source::INPUT_READER) {}

std::string InputReader::ContextImpl::dump() {
    std::string dump;
    mReader->dumpLocked(dump);
    return dump;
}

void InputReader::ContextImpl::updateGlobalMetaState() {
    // lock is already held by the input loop
    mReader->updateGlobalMetaStateLocked();
}

int32_t InputReader::ContextImpl::getGlobalMetaState() {
    // lock is already held by the input loop
    return mReader->getGlobalMetaStateLocked();
}

void InputReader::ContextImpl::updateLedMetaState(int32_t metaState) {
    // lock is already held by the input loop
    mReader->updateLedMetaStateLocked(metaState);
}

int32_t InputReader::ContextImpl::getLedMetaState() {
    // lock is already held by the input loop
    return mReader->getLedMetaStateLocked();
}

void InputReader::ContextImpl::setPreventingTouchpadTaps(bool prevent) {
    // lock is already held by the input loop
    mReader->mPreventingTouchpadTaps = prevent;
}

bool InputReader::ContextImpl::isPreventingTouchpadTaps() {
    // lock is already held by the input loop
    return mReader->mPreventingTouchpadTaps;
}

void InputReader::ContextImpl::setLastKeyDownTimestamp(nsecs_t when) {
    mReader->mLastKeyDownTimestamp = when;
}

nsecs_t InputReader::ContextImpl::getLastKeyDownTimestamp() {
    return mReader->mLastKeyDownTimestamp;
}

void InputReader::ContextImpl::disableVirtualKeysUntil(nsecs_t time) {
    // lock is already held by the input loop
    mReader->disableVirtualKeysUntilLocked(time);
}

bool InputReader::ContextImpl::shouldDropVirtualKey(nsecs_t now, int32_t keyCode,
                                                    int32_t scanCode) {
    // lock is already held by the input loop
    return mReader->shouldDropVirtualKeyLocked(now, keyCode, scanCode);
}

void InputReader::ContextImpl::requestTimeoutAtTime(nsecs_t when) {
    // lock is already held by the input loop
    mReader->requestTimeoutAtTimeLocked(when);
}

int32_t InputReader::ContextImpl::bumpGeneration() {
    // lock is already held by the input loop
    return mReader->bumpGenerationLocked();
}

void InputReader::ContextImpl::getExternalStylusDevices(std::vector<InputDeviceInfo>& outDevices) {
    // lock is already held by whatever called refreshConfigurationLocked
    mReader->getExternalStylusDevicesLocked(outDevices);
}

std::list<NotifyArgs> InputReader::ContextImpl::dispatchExternalStylusState(
        const StylusState& state) {
    return mReader->dispatchExternalStylusStateLocked(state);
}

InputReaderPolicyInterface* InputReader::ContextImpl::getPolicy() {
    return mReader->mPolicy.get();
}

EventHubInterface* InputReader::ContextImpl::getEventHub() {
    return mReader->mEventHub.get();
}

int32_t InputReader::ContextImpl::getNextId() const {
    return mIdGenerator.nextId();
}

KeyboardClassifier& InputReader::ContextImpl::getKeyboardClassifier() {
    return *mReader->mKeyboardClassifier;
}

} // namespace android
