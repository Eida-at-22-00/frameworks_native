/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <gmock/gmock.h>

#include "Scheduler/EventThread.h"

namespace android::mock {

class EventThread : public android::EventThread {
public:
    EventThread();
    ~EventThread() override;

    MOCK_METHOD(sp<EventThreadConnection>, createEventConnection, (EventRegistrationFlags),
                (const, override));
    MOCK_METHOD(void, enableSyntheticVsync, (bool), (override));
    MOCK_METHOD(void, omitVsyncDispatching, (bool), (override));
    MOCK_METHOD(void, onHotplugReceived, (PhysicalDisplayId, bool), (override));
    MOCK_METHOD(void, onHotplugConnectionError, (int32_t), (override));
    MOCK_METHOD(void, onModeChanged, (const scheduler::FrameRateMode&), (override));
    MOCK_METHOD(void, onModeRejected, (PhysicalDisplayId displayId, DisplayModeId modeId),
                (override));
    MOCK_METHOD(void, onFrameRateOverridesChanged,
                (PhysicalDisplayId, std::vector<FrameRateOverride>), (override));
    MOCK_METHOD(void, dump, (std::string&), (const, override));
    MOCK_METHOD(void, setDuration,
                (std::chrono::nanoseconds workDuration, std::chrono::nanoseconds readyDuration),
                (override));
    MOCK_METHOD(status_t, registerDisplayEventConnection,
                (const sp<android::EventThreadConnection>&), (override));
    MOCK_METHOD(void, setVsyncRate, (uint32_t, const sp<android::EventThreadConnection>&),
                (override));
    MOCK_METHOD(void, requestNextVsync, (const sp<android::EventThreadConnection>&), (override));
    MOCK_METHOD(VsyncEventData, getLatestVsyncEventData,
                (const sp<android::EventThreadConnection>&, nsecs_t), (const, override));
    MOCK_METHOD(void, requestLatestConfig, (const sp<android::EventThreadConnection>&));
    MOCK_METHOD(void, pauseVsyncCallback, (bool));
    MOCK_METHOD(void, onNewVsyncSchedule, (std::shared_ptr<scheduler::VsyncSchedule>), (override));
    MOCK_METHOD(void, onHdcpLevelsChanged,
                (PhysicalDisplayId displayId, int32_t connectedLevel, int32_t maxLevel),
                (override));
};

} // namespace android::mock
