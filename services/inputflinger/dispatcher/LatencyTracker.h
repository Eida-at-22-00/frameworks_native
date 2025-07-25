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

#pragma once

#include "../InputDeviceMetricsSource.h"

#include <map>
#include <unordered_map>
#include <vector>

#include <binder/IBinder.h>
#include <input/Input.h>
#include <input/InputDevice.h>

#include "InputEventTimeline.h"
#include "NotifyArgs.h"

namespace android::inputdispatcher {

/**
 * Maintain a record for input events that are received by InputDispatcher, sent out to the apps,
 * and processed by the apps. Once an event becomes "mature" (older than the ANR timeout), report
 * the entire input event latency history to the reporting function.
 *
 * All calls to LatencyTracker should come from the same thread. It is not thread-safe.
 */
class LatencyTracker {
public:
    /**
     * Create a LatencyTracker.
     * param reportingFunction: the function that will be called in order to report full latency.
     * param inputDevices: input devices relevant for tracking.
     */
    LatencyTracker(InputEventTimelineProcessor& processor,
                   std::vector<InputDeviceInfo>& inputDevices);
    /**
     * Start keeping track of an event identified by the args. This must be called first.
     * If duplicate events are encountered (events that have the same eventId), none of them will be
     * tracked. This is because there is not enough information to correctly track them. It is
     * always possible that two different events are generated with the same inputEventId and the
     * same eventTime, so there aren't ways to distinguish those. Therefore, we must drop all
     * duplicate data.
     * For that reason, the APIs 'trackFinishedEvent' and 'trackGraphicsLatency' only receive the
     * inputEventId as input.
     */
    void trackListener(const NotifyArgs& args);
    void trackFinishedEvent(int32_t inputEventId, const sp<IBinder>& connectionToken,
                            nsecs_t deliveryTime, nsecs_t consumeTime, nsecs_t finishTime);
    void trackGraphicsLatency(int32_t inputEventId, const sp<IBinder>& connectionToken,
                              std::array<nsecs_t, GraphicsTimeline::SIZE> timeline);

    std::string dump(const char* prefix) const;

private:
    /**
     * A collection of InputEventTimelines keyed by inputEventId. An InputEventTimeline is first
     * created when 'trackListener' is called.
     * When either 'trackFinishedEvent' or 'trackGraphicsLatency' is called for this input event,
     * the corresponding InputEventTimeline will be updated for that token.
     */
    std::unordered_map<int32_t /*inputEventId*/, InputEventTimeline> mTimelines;
    /**
     * The collection of eventTimes will help us quickly find the events that we should prune
     * from the 'mTimelines'. Since 'mTimelines' is keyed by inputEventId, it would be inefficient
     * to walk through it directly to find the oldest input events to get rid of.
     * There is a 1:1 mapping between 'mTimelines' and 'mEventTimes'.
     * We are using 'multimap' instead of 'map' because there could be more than 1 event with the
     * same eventTime.
     */
    std::multimap<nsecs_t /*eventTime*/, int32_t /*inputEventId*/> mEventTimes;

    InputEventTimelineProcessor* mTimelineProcessor;
    std::vector<InputDeviceInfo>& mInputDevices;

    void trackListener(int32_t inputEventId, nsecs_t eventTime, nsecs_t readTime, DeviceId deviceId,
                       const std::set<InputDeviceUsageSource>& sources, int32_t inputEventAction,
                       InputEventType inputEventType);
    void reportAndPruneMatureRecords(nsecs_t newEventTime);
};

} // namespace android::inputdispatcher
