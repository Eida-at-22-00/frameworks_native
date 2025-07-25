/*
 * Copyright (C) 2021 The Android Open Source Project
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

#define LOG_TAG "LatencyTracker"
#include "LatencyTracker.h"
#include "../InputDeviceMetricsSource.h"

#include <inttypes.h>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android/os/IInputConstants.h>
#include <input/Input.h>
#include <input/InputDevice.h>
#include <log/log.h>

using android::base::HwTimeoutMultiplier;
using android::base::StringPrintf;

namespace android::inputdispatcher {

namespace {

/**
 * Events that are older than this time will be considered mature, at which point we will stop
 * waiting for the apps to provide further information about them.
 * It's likely that the apps will ANR if the events are not received by this deadline, and we
 * already track ANR metrics separately.
 */
const std::chrono::duration ANR_TIMEOUT = std::chrono::milliseconds(
        android::os::IInputConstants::UNMULTIPLIED_DEFAULT_DISPATCHING_TIMEOUT_MILLIS *
        HwTimeoutMultiplier());

static bool isMatureEvent(nsecs_t eventTime, nsecs_t now) {
    std::chrono::duration age = std::chrono::nanoseconds(now) - std::chrono::nanoseconds(eventTime);
    return age > ANR_TIMEOUT;
}

/**
 * A multimap allows to have several entries with the same key. This function just erases a specific
 * key-value pair. Equivalent to the imaginary std api std::multimap::erase(key, value).
 */
template <typename K, typename V>
static void eraseByValue(std::multimap<K, V>& map, const V& value) {
    for (auto it = map.begin(); it != map.end();) {
        if (it->second == value) {
            it = map.erase(it);
        } else {
            it++;
        }
    }
}

} // namespace

LatencyTracker::LatencyTracker(InputEventTimelineProcessor& processor,
                               std::vector<InputDeviceInfo>& inputDevices)
      : mTimelineProcessor(&processor), mInputDevices(inputDevices) {}

void LatencyTracker::trackListener(const NotifyArgs& args) {
    if (const NotifyKeyArgs* keyArgs = std::get_if<NotifyKeyArgs>(&args)) {
        std::set<InputDeviceUsageSource> sources =
                getUsageSourcesForKeyArgs(*keyArgs, mInputDevices);
        trackListener(keyArgs->id, keyArgs->eventTime, keyArgs->readTime, keyArgs->deviceId,
                      sources, keyArgs->action, InputEventType::KEY);

    } else if (const NotifyMotionArgs* motionArgs = std::get_if<NotifyMotionArgs>(&args)) {
        std::set<InputDeviceUsageSource> sources = getUsageSourcesForMotionArgs(*motionArgs);
        trackListener(motionArgs->id, motionArgs->eventTime, motionArgs->readTime,
                      motionArgs->deviceId, sources, motionArgs->action, InputEventType::MOTION);
    } else {
        LOG(FATAL) << "Unexpected NotifyArgs type: " << args.index();
    }
}

void LatencyTracker::trackListener(int32_t inputEventId, nsecs_t eventTime, nsecs_t readTime,
                                   DeviceId deviceId,
                                   const std::set<InputDeviceUsageSource>& sources,
                                   int32_t inputEventAction, InputEventType inputEventType) {
    reportAndPruneMatureRecords(eventTime);
    const auto it = mTimelines.find(inputEventId);
    if (it != mTimelines.end()) {
        // Input event ids are randomly generated, so it's possible that two events have the same
        // event id. Drop this event, and also drop the existing event because the apps would
        // confuse us by reporting the rest of the timeline for one of them. This should happen
        // rarely, so we won't lose much data
        mTimelines.erase(it);
        eraseByValue(mEventTimes, inputEventId);
        return;
    }

    // Create an InputEventTimeline for the device ID. The vendorId and productId
    // can be obtained from the InputDeviceIdentifier of the particular device.
    const InputDeviceIdentifier* identifier = nullptr;
    for (auto& inputDevice : mInputDevices) {
        if (deviceId == inputDevice.getId()) {
            identifier = &inputDevice.getIdentifier();
            break;
        }
    }

    // If no matching ids can be found for the device from among the input devices connected,
    // the call to trackListener will be dropped.
    // Note: there generally isn't expected to be a situation where we can't find an InputDeviceInfo
    // but a possibility of it is handled in case of race conditions
    if (identifier == nullptr) {
        ALOGE("Could not find input device identifier. Dropping call to LatencyTracker.");
        return;
    }

    const InputEventActionType inputEventActionType = [&]() {
        switch (inputEventType) {
            case InputEventType::MOTION: {
                switch (MotionEvent::getActionMasked(inputEventAction)) {
                    case AMOTION_EVENT_ACTION_DOWN:
                        return InputEventActionType::MOTION_ACTION_DOWN;
                    case AMOTION_EVENT_ACTION_MOVE:
                        return InputEventActionType::MOTION_ACTION_MOVE;
                    case AMOTION_EVENT_ACTION_UP:
                        return InputEventActionType::MOTION_ACTION_UP;
                    case AMOTION_EVENT_ACTION_HOVER_MOVE:
                        return InputEventActionType::MOTION_ACTION_HOVER_MOVE;
                    case AMOTION_EVENT_ACTION_SCROLL:
                        return InputEventActionType::MOTION_ACTION_SCROLL;
                    default:
                        return InputEventActionType::UNKNOWN_INPUT_EVENT;
                }
            }
            case InputEventType::KEY: {
                switch (inputEventAction) {
                    case AKEY_EVENT_ACTION_DOWN:
                    case AKEY_EVENT_ACTION_UP:
                        return InputEventActionType::KEY;
                    default:
                        return InputEventActionType::UNKNOWN_INPUT_EVENT;
                }
            }
            default:
                return InputEventActionType::UNKNOWN_INPUT_EVENT;
        }
    }();

    mTimelines.emplace(inputEventId,
                       InputEventTimeline(eventTime, readTime, identifier->vendor,
                                          identifier->product, sources, inputEventActionType));
    mEventTimes.emplace(eventTime, inputEventId);
}

void LatencyTracker::trackFinishedEvent(int32_t inputEventId, const sp<IBinder>& connectionToken,
                                        nsecs_t deliveryTime, nsecs_t consumeTime,
                                        nsecs_t finishTime) {
    const auto it = mTimelines.find(inputEventId);
    if (it == mTimelines.end()) {
        // This could happen if we erased this event when duplicate events were detected. It's
        // also possible that an app sent a bad (or late) 'Finish' signal, since it's free to do
        // anything in its process. Just drop the report and move on.
        return;
    }

    InputEventTimeline& timeline = it->second;
    const auto connectionIt = timeline.connectionTimelines.find(connectionToken);
    if (connectionIt == timeline.connectionTimelines.end()) {
        // Most likely case: app calls 'finishInputEvent' before it reports the graphics timeline
        timeline.connectionTimelines.emplace(connectionToken,
                                             ConnectionTimeline{deliveryTime, consumeTime,
                                                                finishTime});
    } else {
        // Already have a record for this connectionToken
        ConnectionTimeline& connectionTimeline = connectionIt->second;
        const bool success =
                connectionTimeline.setDispatchTimeline(deliveryTime, consumeTime, finishTime);
        if (!success) {
            // We are receiving unreliable data from the app. Just delete the entire connection
            // timeline for this event
            timeline.connectionTimelines.erase(connectionIt);
        }
    }
}

void LatencyTracker::trackGraphicsLatency(
        int32_t inputEventId, const sp<IBinder>& connectionToken,
        std::array<nsecs_t, GraphicsTimeline::SIZE> graphicsTimeline) {
    const auto it = mTimelines.find(inputEventId);
    if (it == mTimelines.end()) {
        // This could happen if we erased this event when duplicate events were detected. It's
        // also possible that an app sent a bad (or late) 'Timeline' signal, since it's free to do
        // anything in its process. Just drop the report and move on.
        return;
    }

    InputEventTimeline& timeline = it->second;
    const auto connectionIt = timeline.connectionTimelines.find(connectionToken);
    if (connectionIt == timeline.connectionTimelines.end()) {
        timeline.connectionTimelines.emplace(connectionToken, std::move(graphicsTimeline));
    } else {
        // Most likely case
        ConnectionTimeline& connectionTimeline = connectionIt->second;
        const bool success = connectionTimeline.setGraphicsTimeline(std::move(graphicsTimeline));
        if (!success) {
            // We are receiving unreliable data from the app. Just delete the entire connection
            // timeline for this event
            timeline.connectionTimelines.erase(connectionIt);
        }
    }
}

/**
 * We should use the current time 'now()' here to determine the age of the event, but instead we
 * are using the latest 'eventTime' for efficiency since this time is already acquired, and
 * 'trackListener' should happen soon after the event occurs.
 */
void LatencyTracker::reportAndPruneMatureRecords(nsecs_t newEventTime) {
    while (!mEventTimes.empty()) {
        const auto& [oldestEventTime, oldestInputEventId] = *mEventTimes.begin();
        if (isMatureEvent(oldestEventTime, /*now=*/newEventTime)) {
            // Report and drop this event
            const auto it = mTimelines.find(oldestInputEventId);
            LOG_ALWAYS_FATAL_IF(it == mTimelines.end(),
                                "Event %" PRId32 " is in mEventTimes, but not in mTimelines",
                                oldestInputEventId);
            const InputEventTimeline& timeline = it->second;
            mTimelineProcessor->processTimeline(timeline);
            mTimelines.erase(it);
            mEventTimes.erase(mEventTimes.begin());
        } else {
            // If the oldest event does not need to be pruned, no events should be pruned.
            return;
        }
    }
}

std::string LatencyTracker::dump(const char* prefix) const {
    return StringPrintf("%sLatencyTracker:\n", prefix) +
            StringPrintf("%s  mTimelines.size() = %zu\n", prefix, mTimelines.size()) +
            StringPrintf("%s  mEventTimes.size() = %zu\n", prefix, mEventTimes.size());
}

} // namespace android::inputdispatcher
