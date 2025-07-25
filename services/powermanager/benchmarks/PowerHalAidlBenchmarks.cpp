/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *            http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "PowerHalAidlBenchmarks"

#include <aidl/android/hardware/power/Boost.h>
#include <aidl/android/hardware/power/IPower.h>
#include <aidl/android/hardware/power/IPowerHintSession.h>
#include <aidl/android/hardware/power/Mode.h>
#include <aidl/android/hardware/power/WorkDuration.h>
#include <benchmark/benchmark.h>
#include <binder/IServiceManager.h>
#include <binder/Status.h>
#include <powermanager/PowerHalLoader.h>
#include <testUtil.h>
#include <chrono>

using aidl::android::hardware::power::Boost;
using aidl::android::hardware::power::IPower;
using aidl::android::hardware::power::IPowerHintSession;
using aidl::android::hardware::power::Mode;
using aidl::android::hardware::power::WorkDuration;
using android::power::PowerHalLoader;
using std::chrono::microseconds;

using namespace android;
using namespace std::chrono_literals;

// Values from Boost.aidl and Mode.aidl.
static constexpr int64_t FIRST_BOOST = static_cast<int64_t>(*ndk::enum_range<Boost>().begin());
static constexpr int64_t LAST_BOOST = static_cast<int64_t>(*(ndk::enum_range<Boost>().end()-1));
static constexpr int64_t FIRST_MODE = static_cast<int64_t>(*ndk::enum_range<Mode>().begin());
static constexpr int64_t LAST_MODE = static_cast<int64_t>(*(ndk::enum_range<Mode>().end()-1));

class DurationWrapper : public WorkDuration {
public:
    DurationWrapper(int64_t dur, int64_t time) {
        durationNanos = dur;
        timeStampNanos = time;
    }
};

static const std::vector<WorkDuration> DURATIONS = {
        DurationWrapper(1L, 1L),
        DurationWrapper(1000L, 2L),
        DurationWrapper(1000000L, 3L),
        DurationWrapper(1000000000L, 4L),
};

// Delay between oneway method calls to avoid overflowing the binder buffers.
static constexpr microseconds ONEWAY_API_DELAY = 100us;

template <class R, class... Args0, class... Args1>
static void runBenchmark(benchmark::State& state, microseconds delay, R (IPower::*fn)(Args0...),
                         Args1&&... args1) {
    std::shared_ptr<IPower> hal = PowerHalLoader::loadAidl();

    if (hal == nullptr) {
        ALOGV("Power HAL not available, skipping test...");
        state.SkipWithMessage("Power HAL unavailable");
        return;
    }

    ndk::ScopedAStatus ret = (*hal.*fn)(std::forward<Args1>(args1)...);
    if (ret.getExceptionCode() == binder::Status::EX_UNSUPPORTED_OPERATION) {
        ALOGV("Power HAL does not support this operation, skipping test...");
        state.SkipWithMessage("operation unsupported");
        return;
    }

    for (auto _ : state) {
        ret = (*hal.*fn)(std::forward<Args1>(args1)...);
        if (!ret.isOk()) {
            state.SkipWithError(ret.getDescription().c_str());
            break;
        }
        if (delay > 0us) {
            state.PauseTiming();
            testDelaySpin(std::chrono::duration_cast<std::chrono::duration<float>>(delay).count());
            state.ResumeTiming();
        }
    }
}

template <class R, class... Args0, class... Args1>
static void runSessionBenchmark(benchmark::State& state, R (IPowerHintSession::*fn)(Args0...),
                                Args1&&... args1) {
    std::shared_ptr<IPower> hal = PowerHalLoader::loadAidl();

    if (hal == nullptr) {
        ALOGV("Power HAL not available, skipping test...");
        state.SkipWithMessage("Power HAL unavailable");
        return;
    }

    // do not use tid from the benchmark process, use 1 for init
    std::vector<int32_t> threadIds{1};
    int64_t durationNanos = 16666666L;
    std::shared_ptr<IPowerHintSession> session;

    auto status = hal->createHintSession(1, 0, threadIds, durationNanos, &session);

    if (session == nullptr) {
        ALOGV("Power HAL doesn't support session, skipping test...");
        state.SkipWithMessage("operation unsupported");
        return;
    }

    ndk::ScopedAStatus ret = (*session.*fn)(std::forward<Args1>(args1)...);
    if (ret.getExceptionCode() == binder::Status::EX_UNSUPPORTED_OPERATION) {
        ALOGV("Power HAL does not support this operation, skipping test...");
        state.SkipWithMessage("operation unsupported");
        return;
    }

    for (auto _ : state) {
        ret = (*session.*fn)(std::forward<Args1>(args1)...);
        if (!ret.isOk()) {
            state.SkipWithError(ret.getDescription().c_str());
            break;
        }
        state.PauseTiming();
        testDelaySpin(std::chrono::duration_cast<std::chrono::duration<float>>(ONEWAY_API_DELAY)
                                .count());
        state.ResumeTiming();
    }
    session->close();
}

static void BM_PowerHalAidlBenchmarks_isBoostSupported(benchmark::State& state) {
    bool isSupported;
    Boost boost = static_cast<Boost>(state.range(0));
    runBenchmark(state, 0us, &IPower::isBoostSupported, boost, &isSupported);
}

static void BM_PowerHalAidlBenchmarks_isModeSupported(benchmark::State& state) {
    bool isSupported;
    Mode mode = static_cast<Mode>(state.range(0));
    runBenchmark(state, 0us, &IPower::isModeSupported, mode, &isSupported);
}

static void BM_PowerHalAidlBenchmarks_setBoost(benchmark::State& state) {
    Boost boost = static_cast<Boost>(state.range(0));
    bool isSupported;
    std::shared_ptr<IPower> hal = PowerHalLoader::loadAidl();

    if (hal == nullptr) {
        ALOGV("Power HAL not available, skipping test...");
        state.SkipWithMessage("Power HAL unavailable");
        return;
    }

    ndk::ScopedAStatus ret = hal->isBoostSupported(boost, &isSupported);
    if (!ret.isOk() || !isSupported) {
        state.SkipWithMessage("operation unsupported");
        return;
    }

    runBenchmark(state, ONEWAY_API_DELAY, &IPower::setBoost, boost, 1);
}

static void BM_PowerHalAidlBenchmarks_setMode(benchmark::State& state) {
    Mode mode = static_cast<Mode>(state.range(0));
    bool isSupported;
    std::shared_ptr<IPower> hal = PowerHalLoader::loadAidl();

    if (hal == nullptr) {
        ALOGV("Power HAL not available, skipping test...");
        state.SkipWithMessage("Power HAL unavailable");
        return;
    }

    ndk::ScopedAStatus ret = hal->isModeSupported(mode, &isSupported);
    if (!ret.isOk() || !isSupported) {
        state.SkipWithMessage("operation unsupported");
        return;
    }

    runBenchmark(state, ONEWAY_API_DELAY, &IPower::setMode, mode, false);
}

static void BM_PowerHalAidlBenchmarks_createHintSession(benchmark::State& state) {
    std::vector<int32_t> threadIds{static_cast<int32_t>(state.range(0))};
    int64_t durationNanos = 16666666L;
    int32_t tgid = 999;
    int32_t uid = 1001;
    std::shared_ptr<IPowerHintSession> appSession;
    std::shared_ptr<IPower> hal = PowerHalLoader::loadAidl();

    if (hal == nullptr) {
        ALOGV("Power HAL not available, skipping test...");
        state.SkipWithMessage("Power HAL unavailable");
        return;
    }

    ndk::ScopedAStatus ret =
            hal->createHintSession(tgid, uid, threadIds, durationNanos, &appSession);
    if (ret.getExceptionCode() == binder::Status::EX_UNSUPPORTED_OPERATION) {
        ALOGV("Power HAL does not support this operation, skipping test...");
        state.SkipWithMessage("operation unsupported");
        return;
    } else if (!ret.isOk()) {
        state.SkipWithError(ret.getDescription().c_str());
        return;
    } else {
        appSession->close();
    }

    for (auto _ : state) {
        ret = hal->createHintSession(tgid, uid, threadIds, durationNanos, &appSession);
        if (!ret.isOk()) {
            state.SkipWithError(ret.getDescription().c_str());
            break;
        }
        state.PauseTiming();
        appSession->close();
        state.ResumeTiming();
    }
}

static void BM_PowerHalAidlBenchmarks_getHintSessionPreferredRate(benchmark::State& state) {
    int64_t rate;
    runBenchmark(state, 0us, &IPower::getHintSessionPreferredRate, &rate);
}

static void BM_PowerHalAidlBenchmarks_updateTargetWorkDuration(benchmark::State& state) {
    int64_t duration = 1000;
    runSessionBenchmark(state, &IPowerHintSession::updateTargetWorkDuration, duration);
}

static void BM_PowerHalAidlBenchmarks_reportActualWorkDuration(benchmark::State& state) {
    runSessionBenchmark(state, &IPowerHintSession::reportActualWorkDuration, DURATIONS);
}

BENCHMARK(BM_PowerHalAidlBenchmarks_isBoostSupported)->DenseRange(FIRST_BOOST, LAST_BOOST, 1);
BENCHMARK(BM_PowerHalAidlBenchmarks_isModeSupported)->DenseRange(FIRST_MODE, LAST_MODE, 1);
BENCHMARK(BM_PowerHalAidlBenchmarks_setBoost)->DenseRange(FIRST_BOOST, LAST_BOOST, 1);
BENCHMARK(BM_PowerHalAidlBenchmarks_setMode)->DenseRange(FIRST_MODE, LAST_MODE, 1);
BENCHMARK(BM_PowerHalAidlBenchmarks_createHintSession)->Arg(1);
BENCHMARK(BM_PowerHalAidlBenchmarks_getHintSessionPreferredRate);
BENCHMARK(BM_PowerHalAidlBenchmarks_updateTargetWorkDuration);
BENCHMARK(BM_PowerHalAidlBenchmarks_reportActualWorkDuration);
