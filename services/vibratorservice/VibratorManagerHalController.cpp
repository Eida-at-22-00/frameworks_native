/*
 * Copyright (C) 2020 The Android Open Source Project
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

#define LOG_TAG "VibratorManagerHalController"

#include <utils/Log.h>

#include <vibratorservice/VibratorManagerHalController.h>

using aidl::android::hardware::vibrator::IVibrationSession;
using aidl::android::hardware::vibrator::IVibrator;
using aidl::android::hardware::vibrator::IVibratorManager;
using aidl::android::hardware::vibrator::VibrationSessionConfig;

namespace android {

namespace vibrator {

std::shared_ptr<ManagerHalWrapper> connectManagerHal(std::shared_ptr<CallbackScheduler> scheduler) {
    static bool gHalExists = true;
    if (gHalExists) {
        auto serviceName = std::string(IVibratorManager::descriptor) + "/default";
        if (AServiceManager_isDeclared(serviceName.c_str())) {
            std::shared_ptr<IVibratorManager> hal = IVibratorManager::fromBinder(
                    ndk::SpAIBinder(AServiceManager_checkService(serviceName.c_str())));
            if (hal) {
                ALOGV("Successfully connected to VibratorManager HAL AIDL service.");
                return std::make_shared<AidlManagerHalWrapper>(std::move(scheduler),
                                                               std::move(hal));
            }
        }
    }

    ALOGV("VibratorManager HAL service not available.");
    gHalExists = false;
    return std::make_shared<LegacyManagerHalWrapper>();
}

static constexpr int MAX_RETRIES = 1;

template <typename T>
HalResult<T> ManagerHalController::processHalResult(HalResult<T> result, const char* functionName) {
    if (result.isFailed()) {
        ALOGE("VibratorManager HAL %s failed: %s", functionName, result.errorMessage());
    }
    return result;
}

template <typename T>
HalResult<T> ManagerHalController::apply(ManagerHalController::hal_fn<T>& halFn,
                                         const char* functionName) {
    std::shared_ptr<ManagerHalWrapper> hal = nullptr;
    {
        std::lock_guard<std::mutex> lock(mConnectedHalMutex);
        if (mConnectedHal == nullptr) {
            // Init was never called, so connect to HAL for the first time during this call.
            mConnectedHal = mConnector(mCallbackScheduler);

            if (mConnectedHal == nullptr) {
                ALOGV("Skipped %s because VibratorManager HAL is not available", functionName);
                return HalResult<T>::unsupported();
            }
        }
        hal = mConnectedHal;
    }

    HalResult<T> result = processHalResult(halFn(hal), functionName);
    for (int i = 0; i < MAX_RETRIES && result.shouldRetry(); i++) {
        {
            std::lock_guard<std::mutex> lock(mConnectedHalMutex);
            mConnectedHal->tryReconnect();
        }
        result = processHalResult(halFn(hal), functionName);
    }

    return result;
}

// -------------------------------------------------------------------------------------------------

void ManagerHalController::init() {
    std::lock_guard<std::mutex> lock(mConnectedHalMutex);
    if (mConnectedHal == nullptr) {
        mConnectedHal = mConnector(mCallbackScheduler);
    }
}

HalResult<void> ManagerHalController::ping() {
    hal_fn<void> pingFn = [](std::shared_ptr<ManagerHalWrapper> hal) { return hal->ping(); };
    return apply(pingFn, "ping");
}

void ManagerHalController::tryReconnect() {
    std::lock_guard<std::mutex> lock(mConnectedHalMutex);
    if (mConnectedHal == nullptr) {
        mConnectedHal = mConnector(mCallbackScheduler);
    } else {
        mConnectedHal->tryReconnect();
    }
}

HalResult<ManagerCapabilities> ManagerHalController::getCapabilities() {
    hal_fn<ManagerCapabilities> getCapabilitiesFn = [](std::shared_ptr<ManagerHalWrapper> hal) {
        return hal->getCapabilities();
    };
    return apply(getCapabilitiesFn, "getCapabilities");
}

HalResult<std::vector<int32_t>> ManagerHalController::getVibratorIds() {
    hal_fn<std::vector<int32_t>> getVibratorIdsFn = [](std::shared_ptr<ManagerHalWrapper> hal) {
        return hal->getVibratorIds();
    };
    return apply(getVibratorIdsFn, "getVibratorIds");
}

HalResult<std::shared_ptr<HalController>> ManagerHalController::getVibrator(int32_t id) {
    hal_fn<std::shared_ptr<HalController>> getVibratorFn =
            [&](std::shared_ptr<ManagerHalWrapper> hal) { return hal->getVibrator(id); };
    return apply(getVibratorFn, "getVibrator");
}

HalResult<void> ManagerHalController::prepareSynced(const std::vector<int32_t>& ids) {
    hal_fn<void> prepareSyncedFn = [&](std::shared_ptr<ManagerHalWrapper> hal) {
        return hal->prepareSynced(ids);
    };
    return apply(prepareSyncedFn, "prepareSynced");
}

HalResult<void> ManagerHalController::triggerSynced(
        const std::function<void()>& completionCallback) {
    hal_fn<void> triggerSyncedFn = [&](std::shared_ptr<ManagerHalWrapper> hal) {
        return hal->triggerSynced(completionCallback);
    };
    return apply(triggerSyncedFn, "triggerSynced");
}

HalResult<void> ManagerHalController::cancelSynced() {
    hal_fn<void> cancelSyncedFn = [](std::shared_ptr<ManagerHalWrapper> hal) {
        return hal->cancelSynced();
    };
    return apply(cancelSyncedFn, "cancelSynced");
}

HalResult<std::shared_ptr<IVibrationSession>> ManagerHalController::startSession(
        const std::vector<int32_t>& ids, const VibrationSessionConfig& config,
        const std::function<void()>& completionCallback) {
    hal_fn<std::shared_ptr<IVibrationSession>> startSessionFn =
            [&](std::shared_ptr<ManagerHalWrapper> hal) {
                return hal->startSession(ids, config, completionCallback);
            };
    return apply(startSessionFn, "startSession");
}

HalResult<void> ManagerHalController::clearSessions() {
    hal_fn<void> clearSessionsFn = [](std::shared_ptr<ManagerHalWrapper> hal) {
        return hal->clearSessions();
    };
    return apply(clearSessionsFn, "clearSessions");
}

}; // namespace vibrator

}; // namespace android
