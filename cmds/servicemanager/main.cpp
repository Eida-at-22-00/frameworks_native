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

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/Status.h>
#include <sys/timerfd.h>
#include <utils/Looper.h>
#include <utils/StrongPointer.h>

#include "Access.h"
#include "ServiceManager.h"

#if !defined(VENDORSERVICEMANAGER) && !defined(__ANDROID_RECOVERY__)

#include <perfetto/public/producer.h>
#include <perfetto/public/te_category_macros.h>
#include <perfetto/public/te_macros.h>
#include <perfetto/public/track_event.h>

namespace android {

static void register_perfetto_te_categories() {
    struct PerfettoProducerInitArgs perfetto_args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
    perfetto_args.backends = PERFETTO_BACKEND_SYSTEM;
    PerfettoProducerInit(perfetto_args);
    PerfettoTeInit();
    PERFETTO_TE_REGISTER_CATEGORIES(PERFETTO_SM_CATEGORIES);
}
} // namespace android

#endif // !defined(VENDORSERVICEMANAGER) && !defined(__ANDROID_RECOVERY__)

using ::android::Access;
using ::android::IPCThreadState;
using ::android::Looper;
using ::android::LooperCallback;
using ::android::ProcessState;
using ::android::ServiceManager;
using ::android::sp;
using ::android::base::SetProperty;
using ::android::os::IServiceManager;

class BinderCallback : public LooperCallback {
public:
    static sp<BinderCallback> setupTo(const sp<Looper>& looper) {
        sp<BinderCallback> cb = sp<BinderCallback>::make();
        cb->mLooper = looper;

        IPCThreadState::self()->setupPolling(&cb->mBinderFd);
        LOG_ALWAYS_FATAL_IF(cb->mBinderFd < 0, "Failed to setupPolling: %d", cb->mBinderFd);

        int ret = looper->addFd(cb->mBinderFd, Looper::POLL_CALLBACK, Looper::EVENT_INPUT, cb,
                                nullptr /*data*/);
        LOG_ALWAYS_FATAL_IF(ret != 1, "Failed to add binder FD to Looper");

        return cb;
    }

    int handleEvent(int /* fd */, int /* events */, void* /* data */) override {
        IPCThreadState::self()->handlePolledCommands();
        return 1;  // Continue receiving callbacks.
    }

    void repoll() {
        if (!mLooper->repoll(mBinderFd)) {
            ALOGE("Failed to repoll binder FD.");
        }
    }

private:
    sp<Looper> mLooper;
    int mBinderFd = -1;
};

// LooperCallback for IClientCallback
class ClientCallbackCallback : public LooperCallback {
public:
    static sp<ClientCallbackCallback> setupTo(const sp<Looper>& looper,
                                              const sp<ServiceManager>& manager,
                                              sp<BinderCallback> binderCallback) {
        sp<ClientCallbackCallback> cb = sp<ClientCallbackCallback>::make(manager);
        cb->mBinderCallback = binderCallback;

        int fdTimer = timerfd_create(CLOCK_MONOTONIC, 0 /*flags*/);
        LOG_ALWAYS_FATAL_IF(fdTimer < 0, "Failed to timerfd_create: fd: %d err: %d", fdTimer, errno);

        itimerspec timespec {
            .it_interval = {
                .tv_sec = 5,
                .tv_nsec = 0,
            },
            .it_value = {
                .tv_sec = 5,
                .tv_nsec = 0,
            },
        };

        int timeRes = timerfd_settime(fdTimer, 0 /*flags*/, &timespec, nullptr);
        LOG_ALWAYS_FATAL_IF(timeRes < 0, "Failed to timerfd_settime: res: %d err: %d", timeRes, errno);

        int addRes = looper->addFd(fdTimer,
                                   Looper::POLL_CALLBACK,
                                   Looper::EVENT_INPUT,
                                   cb,
                                   nullptr);
        LOG_ALWAYS_FATAL_IF(addRes != 1, "Failed to add client callback FD to Looper");

        return cb;
    }

    int handleEvent(int fd, int /*events*/, void* /*data*/) override {
        uint64_t expirations;
        int ret = read(fd, &expirations, sizeof(expirations));
        if (ret != sizeof(expirations)) {
            ALOGE("Read failed to callback FD: ret: %d err: %d", ret, errno);
        }

        mManager->handleClientCallbacks();
        mBinderCallback->repoll(); // b/316829336

        return 1;  // Continue receiving callbacks.
    }
private:
    friend sp<ClientCallbackCallback>;
    ClientCallbackCallback(const sp<ServiceManager>& manager) : mManager(manager) {}
    sp<ServiceManager> mManager;
    sp<BinderCallback> mBinderCallback;
};

int main(int argc, char** argv) {
    android::base::InitLogging(argv, android::base::KernelLogger);

    if (argc > 2) {
        LOG(FATAL) << "usage: " << argv[0] << " [binder driver]";
    }

    const char* driver = argc == 2 ? argv[1] : "/dev/binder";

#if !defined(VENDORSERVICEMANAGER) && !defined(__ANDROID_RECOVERY__)
    android::register_perfetto_te_categories();
#endif // !defined(VENDORSERVICEMANAGER) && !defined(__ANDROID_RECOVERY__)

    LOG(INFO) << "Starting sm instance on " << driver;

    int sched_policy = SCHED_FIFO;
    int newPriority = sched_get_priority_min(sched_policy);

    struct sched_param param;
    param.sched_priority = newPriority;

    if (sched_setscheduler(0, sched_policy, &param)) {
       LOG(ERROR) << "Failed to set ServiceManager priority to SCHED_FIFO";
    }

    sp<ProcessState> ps = ProcessState::initWithDriver(driver);
    ps->setThreadPoolMaxThreadCount(0);
    ps->setCallRestriction(ProcessState::CallRestriction::FATAL_IF_NOT_ONEWAY);

    IPCThreadState::self()->disableBackgroundScheduling(true);

    sp<ServiceManager> manager = sp<ServiceManager>::make(std::make_unique<Access>());
    manager->setRequestingSid(true);
    if (!manager->addService("manager", manager, false /*allowIsolated*/, IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk()) {
        LOG(ERROR) << "Could not self register servicemanager";
    }

    IPCThreadState::self()->setTheContextObject(manager);
    if (!ps->becomeContextManager()) {
        LOG(FATAL) << "Could not become context manager";
    }

    sp<Looper> looper = Looper::prepare(false /*allowNonCallbacks*/);

    sp<BinderCallback> binderCallback = BinderCallback::setupTo(looper);
    ClientCallbackCallback::setupTo(looper, manager, binderCallback);

#ifndef VENDORSERVICEMANAGER
    if (!SetProperty("servicemanager.ready", "true")) {
        LOG(ERROR) << "Failed to set servicemanager ready property";
    }
#endif

    while(true) {
        looper->pollAll(-1);
    }

    // should not be reached
    return EXIT_FAILURE;
}
