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

#define LOG_TAG "ProcessState"

#include <binder/ProcessState.h>

#include <android-base/strings.h>
#include <binder/BpBinder.h>
#include <binder/Functional.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Stability.h>
#include <utils/AndroidThreads.h>
#include <utils/String8.h>
#include <utils/Thread.h>

#include "Static.h"
#include "Utils.h"
#include "binder_module.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <mutex>

#define BINDER_VM_SIZE ((1 * 1024 * 1024) - sysconf(_SC_PAGE_SIZE) * 2)
#define DEFAULT_MAX_BINDER_THREADS 15
#define DEFAULT_ENABLE_ONEWAY_SPAM_DETECTION 1

#if defined(__ANDROID__) || defined(__Fuchsia__)
#define EXPECT_BINDER_OPEN_SUCCESS
#endif

#ifdef __ANDROID_VNDK__
const char* kDefaultDriver = "/dev/vndbinder";
#else
const char* kDefaultDriver = "/dev/binder";
#endif

#ifdef BINDER_WITH_KERNEL_IPC
static constexpr inline int schedPolicyMask(int policy, int priority) {
    return (priority & FLAT_BINDER_FLAG_PRIORITY_MASK) | ((policy & 3) << FLAT_BINDER_FLAG_SCHED_POLICY_SHIFT);
}
#endif // BINDER_WITH_KERNEL_IPC

// -------------------------------------------------------------------------

namespace {
bool readDriverFeatureFile(const char* filename) {
    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    char on;
    if (fd == -1) {
        ALOGE_IF(errno != ENOENT, "%s: cannot open %s: %s", __func__, filename, strerror(errno));
        return false;
    }
    if (read(fd, &on, sizeof(on)) == -1) {
        ALOGE("%s: error reading to %s: %s", __func__, filename, strerror(errno));
        close(fd);
        return false;
    }
    close(fd);
    return on == '1';
}

} // namespace

namespace android {

using namespace android::binder::impl;
using android::binder::unique_fd;

class PoolThread : public Thread
{
public:
    explicit PoolThread(bool isMain)
        : mIsMain(isMain)
    {
    }

protected:
    virtual bool threadLoop()
    {
        IPCThreadState::self()->joinThreadPool(mIsMain);
        return false;
    }

    const bool mIsMain;
};

sp<ProcessState> ProcessState::self()
{
    return init(kDefaultDriver, false /*requireDefault*/);
}

sp<ProcessState> ProcessState::initWithDriver(const char* driver)
{
    return init(driver, true /*requireDefault*/);
}

sp<ProcessState> ProcessState::selfOrNull()
{
    return init(nullptr, false /*requireDefault*/);
}

[[clang::no_destroy]] static sp<ProcessState> gProcess;
[[clang::no_destroy]] static std::mutex gProcessMutex;

static void verifyNotForked(bool forked) {
    LOG_ALWAYS_FATAL_IF(forked, "libbinder ProcessState can not be used after fork");
}

bool ProcessState::isVndservicemanagerEnabled() {
    return access("/vendor/bin/vndservicemanager", R_OK) == 0;
}

sp<ProcessState> ProcessState::init(const char* driver, bool requireDefault) {
    if (driver == nullptr) {
        std::lock_guard<std::mutex> l(gProcessMutex);
        if (gProcess) {
            verifyNotForked(gProcess->mForked);
        }
        return gProcess;
    }

    [[clang::no_destroy]] static std::once_flag gProcessOnce;
    std::call_once(gProcessOnce, [&](){
        if (access(driver, R_OK) == -1) {
            ALOGE("Binder driver %s is unavailable. Using /dev/binder instead.", driver);
            driver = "/dev/binder";
        }

        if (0 == strcmp(driver, "/dev/vndbinder") && !isVndservicemanagerEnabled()) {
            ALOGE("vndservicemanager is not started on this device, you can save resources/threads "
                  "by not initializing ProcessState with /dev/vndbinder.");
        }

        // we must install these before instantiating the gProcess object,
        // otherwise this would race with creating it, and there could be the
        // possibility of an invalid gProcess object forked by another thread
        // before these are installed
        int ret = pthread_atfork(ProcessState::onFork, ProcessState::parentPostFork,
                                 ProcessState::childPostFork);
        LOG_ALWAYS_FATAL_IF(ret != 0, "pthread_atfork error %s", strerror(ret));

        std::lock_guard<std::mutex> l(gProcessMutex);
        gProcess = sp<ProcessState>::make(driver);
    });

    if (requireDefault) {
        // Detect if we are trying to initialize with a different driver, and
        // consider that an error. ProcessState will only be initialized once above.
        LOG_ALWAYS_FATAL_IF(gProcess->getDriverName() != driver,
                            "ProcessState was already initialized with %s,"
                            " can't initialize with %s.",
                            gProcess->getDriverName().c_str(), driver);
    }

    verifyNotForked(gProcess->mForked);
    return gProcess;
}

sp<IBinder> ProcessState::getContextObject(const sp<IBinder>& /*caller*/)
{
    sp<IBinder> context = getStrongProxyForHandle(0);

    if (context) {
        // The root object is special since we get it directly from the driver, it is never
        // written by Parcell::writeStrongBinder.
        internal::Stability::markCompilationUnit(context.get());
    } else {
        ALOGW("Not able to get context object on %s.", mDriverName.c_str());
    }

    return context;
}

void ProcessState::onFork() {
    // make sure another thread isn't currently retrieving ProcessState
    gProcessMutex.lock();
}

void ProcessState::parentPostFork() {
    gProcessMutex.unlock();
}

void ProcessState::childPostFork() {
    // another thread might call fork before gProcess is instantiated, but after
    // the thread handler is installed
    if (gProcess) {
        gProcess->mForked = true;

        // "O_CLOFORK"
        close(gProcess->mDriverFD);
        gProcess->mDriverFD = -1;
    }
    gProcessMutex.unlock();
}

void ProcessState::startThreadPool()
{
    std::unique_lock<std::mutex> _l(mLock);
    if (!mThreadPoolStarted) {
        if (mMaxThreads == 0) {
            // see also getThreadPoolMaxTotalThreadCount
            ALOGW("Extra binder thread started, but 0 threads requested. Do not use "
                  "*startThreadPool when zero threads are requested.");
        }
        mThreadPoolStarted = true;
        spawnPooledThread(true);
    }
}

bool ProcessState::becomeContextManager()
{
    std::unique_lock<std::mutex> _l(mLock);

    int policy = SCHED_FIFO;
    int minPriority = sched_get_priority_min(policy);

    flat_binder_object obj {
        .flags = FLAT_BINDER_FLAG_TXN_SECURITY_CTX,
    };
    obj.flags |= schedPolicyMask(policy, minPriority);
    obj.flags |= FLAT_BINDER_FLAG_INHERIT_RT;

    int result = ioctl(mDriverFD, BINDER_SET_CONTEXT_MGR_EXT, &obj);

    // fallback to original method
    if (result != 0) {
        android_errorWriteLog(0x534e4554, "121035042");

        int unused = 0;
        result = ioctl(mDriverFD, BINDER_SET_CONTEXT_MGR, &unused);
    }

    if (result == -1) {
        ALOGE("Binder ioctl to become context manager failed: %s\n", strerror(errno));
    }

    return result == 0;
}

// Get references to userspace objects held by the kernel binder driver
// Writes up to count elements into buf, and returns the total number
// of references the kernel has, which may be larger than count.
// buf may be NULL if count is 0.  The pointers returned by this method
// should only be used for debugging and not dereferenced, they may
// already be invalid.
ssize_t ProcessState::getKernelReferences(size_t buf_count, uintptr_t* buf)
{
    binder_node_debug_info info = {};

    uintptr_t* end = buf ? buf + buf_count : nullptr;
    size_t count = 0;

    do {
        status_t result = ioctl(mDriverFD, BINDER_GET_NODE_DEBUG_INFO, &info);
        if (result < 0) {
            return -1;
        }
        if (info.ptr != 0) {
            if (buf && buf < end)
                *buf++ = info.ptr;
            count++;
            if (buf && buf < end)
                *buf++ = info.cookie;
            count++;
        }
    } while (info.ptr != 0);

    return count;
}

// Queries the driver for the current strong reference count of the node
// that the handle points to. Can only be used by the servicemanager.
//
// Returns -1 in case of failure, otherwise the strong reference count.
ssize_t ProcessState::getStrongRefCountForNode(const sp<BpBinder>& binder) {
    if (binder->isRpcBinder()) return -1;

    binder_node_info_for_ref info;
    memset(&info, 0, sizeof(binder_node_info_for_ref));

    info.handle = binder->getPrivateAccessor().binderHandle();

    status_t result = ioctl(mDriverFD, BINDER_GET_NODE_INFO_FOR_REF, &info);

    if (result != OK) {
        static bool logged = false;
        if (!logged) {
          ALOGW("Kernel does not support BINDER_GET_NODE_INFO_FOR_REF.");
          logged = true;
        }
        return -1;
    }

    return info.strong_count;
}

void ProcessState::setCallRestriction(CallRestriction restriction) {
    LOG_ALWAYS_FATAL_IF(IPCThreadState::selfOrNull() != nullptr,
        "Call restrictions must be set before the threadpool is started.");

    mCallRestriction = restriction;
}

ProcessState::handle_entry* ProcessState::lookupHandleLocked(int32_t handle)
{
    const size_t N=mHandleToObject.size();
    if (N <= (size_t)handle) {
        handle_entry e;
        e.binder = nullptr;
        e.refs = nullptr;
        status_t err = mHandleToObject.insertAt(e, N, handle+1-N);
        if (err < NO_ERROR) return nullptr;
    }
    return &mHandleToObject.editItemAt(handle);
}

// see b/166779391: cannot change the VNDK interface, so access like this
extern sp<BBinder> the_context_object;

sp<IBinder> ProcessState::getStrongProxyForHandle(int32_t handle)
{
    sp<IBinder> result;
    std::function<void()> postTask;

    std::unique_lock<std::mutex> _l(mLock);

    if (handle == 0 && the_context_object != nullptr) return the_context_object;

    handle_entry* e = lookupHandleLocked(handle);

    if (e != nullptr) {
        // We need to create a new BpBinder if there isn't currently one, OR we
        // are unable to acquire a weak reference on this current one.  The
        // attemptIncWeak() is safe because we know the BpBinder destructor will always
        // call expungeHandle(), which acquires the same lock we are holding now.
        // We need to do this because there is a race condition between someone
        // releasing a reference on this BpBinder, and a new reference on its handle
        // arriving from the driver.
        IBinder* b = e->binder;
        if (b == nullptr || !e->refs->attemptIncWeak(this)) {
            if (handle == 0) {
                // Special case for context manager...
                // The context manager is the only object for which we create
                // a BpBinder proxy without already holding a reference.
                // Perform a dummy transaction to ensure the context manager
                // is registered before we create the first local reference
                // to it (which will occur when creating the BpBinder).
                // If a local reference is created for the BpBinder when the
                // context manager is not present, the driver will fail to
                // provide a reference to the context manager, but the
                // driver API does not return status.
                //
                // Note that this is not race-free if the context manager
                // dies while this code runs.

                IPCThreadState* ipc = IPCThreadState::self();

                CallRestriction originalCallRestriction = ipc->getCallRestriction();
                ipc->setCallRestriction(CallRestriction::NONE);

                Parcel data;
                status_t status = ipc->transact(
                        0, IBinder::PING_TRANSACTION, data, nullptr, 0);

                ipc->setCallRestriction(originalCallRestriction);

                if (status == DEAD_OBJECT)
                   return nullptr;
            }

            sp<BpBinder> b = BpBinder::PrivateAccessor::create(handle, &postTask);
            e->binder = b.get();
            if (b) e->refs = b->getWeakRefs();
            result = b;
        } else {
            // This little bit of nastyness is to allow us to add a primary
            // reference to the remote proxy when this team doesn't have one
            // but another team is sending the handle to us.
            result.force_set(b);
            e->refs->decWeak(this);
        }
    }

    _l.unlock();

    if (postTask) postTask();

    return result;
}

void ProcessState::expungeHandle(int32_t handle, IBinder* binder)
{
    std::unique_lock<std::mutex> _l(mLock);

    handle_entry* e = lookupHandleLocked(handle);

    // This handle may have already been replaced with a new BpBinder
    // (if someone failed the AttemptIncWeak() above); we don't want
    // to overwrite it.
    if (e && e->binder == binder) e->binder = nullptr;
}

String8 ProcessState::makeBinderThreadName() {
    int32_t s = mThreadPoolSeq.fetch_add(1, std::memory_order_release);
    pid_t pid = getpid();

    std::string_view driverName = mDriverName.c_str();
    android::base::ConsumePrefix(&driverName, "/dev/");

    String8 name;
    name.appendFormat("%.*s:%d_%X", static_cast<int>(driverName.length()), driverName.data(), pid,
                      s);
    return name;
}

void ProcessState::spawnPooledThread(bool isMain)
{
    if (mThreadPoolStarted) {
        String8 name = makeBinderThreadName();
        ALOGV("Spawning new pooled thread, name=%s\n", name.c_str());
        sp<Thread> t = sp<PoolThread>::make(isMain);
        t->run(name.c_str());
        mKernelStartedThreads++;
    }
    // TODO: if startThreadPool is called on another thread after the process
    // starts up, the kernel might think that it already requested those
    // binder threads, and additional won't be started. This is likely to
    // cause deadlocks, and it will also cause getThreadPoolMaxTotalThreadCount
    // to return too high of a value.
}

status_t ProcessState::setThreadPoolMaxThreadCount(size_t maxThreads) {
    LOG_ALWAYS_FATAL_IF(mThreadPoolStarted && maxThreads < mMaxThreads,
           "Binder threadpool cannot be shrunk after starting");
    status_t result = NO_ERROR;
    if (ioctl(mDriverFD, BINDER_SET_MAX_THREADS, &maxThreads) != -1) {
        mMaxThreads = maxThreads;
    } else {
        result = -errno;
        ALOGE("Binder ioctl to set max threads failed: %s", strerror(-result));
    }
    return result;
}

size_t ProcessState::getThreadPoolMaxTotalThreadCount() const {
    // Need to read `mKernelStartedThreads` before `mThreadPoolStarted` (with
    // non-relaxed memory ordering) to avoid a race like the following:
    //
    // thread A: if (mThreadPoolStarted) { // evaluates false
    // thread B: mThreadPoolStarted = true;
    // thread B: mKernelStartedThreads++;
    // thread A: size_t kernelStarted = mKernelStartedThreads;
    // thread A: LOG_ALWAYS_FATAL_IF(kernelStarted != 0, ...);
    size_t kernelStarted = mKernelStartedThreads;

    if (mThreadPoolStarted) {
        size_t max = mMaxThreads;
        size_t current = mCurrentThreads;

        LOG_ALWAYS_FATAL_IF(kernelStarted > max + 1,
                            "too many kernel-started threads: %zu > %zu + 1", kernelStarted, max);

        // calling startThreadPool starts a thread
        size_t threads = 1;

        // the kernel is configured to start up to mMaxThreads more threads
        threads += max;

        // Users may call IPCThreadState::joinThreadPool directly. We don't
        // currently have a way to count this directly (it could be added by
        // adding a separate private joinKernelThread method in IPCThreadState).
        // So, if we are in a race between the kernel thread variable being
        // incremented in this file and mCurrentThreads being incremented
        // in IPCThreadState, temporarily forget about the extra join threads.
        // This is okay, because most callers of this method only care about
        // having 0, 1, or more threads.
        if (current > kernelStarted) {
            threads += current - kernelStarted;
        }

        return threads;
    }

    // must not be initialized or maybe has poll thread setup, we
    // currently don't track this in libbinder
    LOG_ALWAYS_FATAL_IF(kernelStarted != 0, "Expecting 0 kernel started threads but have %zu",
                        kernelStarted);
    return mCurrentThreads;
}

bool ProcessState::isThreadPoolStarted() const {
    return mThreadPoolStarted;
}

void ProcessState::checkExpectingThreadPoolStart() const {
    if (mThreadPoolStarted) return;

    // this is also racey, but you should setup the threadpool in the main thread. If that is an
    // issue, we can check if we are the process leader, but haven't seen the issue in practice.
    size_t requestedThreads = mMaxThreads.load();

    // if it's manually set to the default, we do ignore it here...
    if (requestedThreads == DEFAULT_MAX_BINDER_THREADS) return;
    if (requestedThreads == 0) return;

    ALOGW("Thread pool configuration of size %zu requested, but startThreadPool was not called.",
          requestedThreads);
}

#define DRIVER_FEATURES_PATH "/dev/binderfs/features/"
bool ProcessState::isDriverFeatureEnabled(const DriverFeature feature) {
    // Use static variable to cache the results.
    if (feature == DriverFeature::ONEWAY_SPAM_DETECTION) {
        static bool enabled = readDriverFeatureFile(DRIVER_FEATURES_PATH "oneway_spam_detection");
        return enabled;
    }
    if (feature == DriverFeature::EXTENDED_ERROR) {
        static bool enabled = readDriverFeatureFile(DRIVER_FEATURES_PATH "extended_error");
        return enabled;
    }
    if (feature == DriverFeature::FREEZE_NOTIFICATION) {
        static bool enabled = readDriverFeatureFile(DRIVER_FEATURES_PATH "freeze_notification");
        return enabled;
    }
    return false;
}

status_t ProcessState::enableOnewaySpamDetection(bool enable) {
    uint32_t enableDetection = enable ? 1 : 0;
    if (ioctl(mDriverFD, BINDER_ENABLE_ONEWAY_SPAM_DETECTION, &enableDetection) == -1) {
        ALOGI("Binder ioctl to enable oneway spam detection failed: %s", strerror(errno));
        return -errno;
    }
    return NO_ERROR;
}

void ProcessState::giveThreadPoolName() {
    androidSetThreadName(makeBinderThreadName().c_str());
}

String8 ProcessState::getDriverName() {
    return mDriverName;
}

static unique_fd open_driver(const char* driver, String8* error) {
    auto fd = unique_fd(open(driver, O_RDWR | O_CLOEXEC));
    if (!fd.ok()) {
        error->appendFormat("%d (%s) Opening '%s' failed", errno, strerror(errno), driver);
        return {};
    }
    int vers = 0;
    int result = ioctl(fd.get(), BINDER_VERSION, &vers);
    if (result == -1) {
        error->appendFormat("%d (%s) Binder ioctl to obtain version failed", errno,
                            strerror(errno));
        return {};
    }
    if (result != 0 || vers != BINDER_CURRENT_PROTOCOL_VERSION) {
        error->appendFormat("Binder driver protocol(%d) does not match user space protocol(%d)! "
                            "ioctl() return value: %d",
                            vers, BINDER_CURRENT_PROTOCOL_VERSION, result);
        return {};
    }
    size_t maxThreads = DEFAULT_MAX_BINDER_THREADS;
    result = ioctl(fd.get(), BINDER_SET_MAX_THREADS, &maxThreads);
    if (result == -1) {
        ALOGE("Binder ioctl to set max threads failed: %s", strerror(errno));
    }
    uint32_t enable = DEFAULT_ENABLE_ONEWAY_SPAM_DETECTION;
    result = ioctl(fd.get(), BINDER_ENABLE_ONEWAY_SPAM_DETECTION, &enable);
    if (result == -1) {
        ALOGE_IF(ProcessState::isDriverFeatureEnabled(
                     ProcessState::DriverFeature::ONEWAY_SPAM_DETECTION),
                 "Binder ioctl to enable oneway spam detection failed: %s", strerror(errno));
    }
    return fd;
}

ProcessState::ProcessState(const char* driver)
      : mDriverName(String8(driver)),
        mDriverFD(-1),
        mVMStart(MAP_FAILED),
        mExecutingThreadsCount(0),
        mMaxThreads(DEFAULT_MAX_BINDER_THREADS),
        mCurrentThreads(0),
        mKernelStartedThreads(0),
        mStarvationStartTime(never()),
        mForked(false),
        mThreadPoolStarted(false),
        mThreadPoolSeq(1),
        mCallRestriction(CallRestriction::NONE) {
    String8 error;
    unique_fd opened = open_driver(driver, &error);

    if (opened.ok()) {
        // mmap the binder, providing a chunk of virtual address space to receive transactions.
        mVMStart = mmap(nullptr, BINDER_VM_SIZE, PROT_READ, MAP_PRIVATE | MAP_NORESERVE,
                        opened.get(), 0);
        if (mVMStart == MAP_FAILED) {
            // *sigh*
            ALOGE("Using %s failed: unable to mmap transaction memory.", driver);
            opened.reset();
            mDriverName.clear();
        }
    }

#if defined(EXPECT_BINDER_OPEN_SUCCESS)
    LOG_ALWAYS_FATAL_IF(!opened.ok(),
                        "Binder driver '%s' could not be opened. Error: %s. Terminating.",
                        driver, error.c_str());
#endif

    if (opened.ok()) {
        mDriverFD = opened.release();
    }
}

ProcessState::~ProcessState()
{
    if (mDriverFD >= 0) {
        if (mVMStart != MAP_FAILED) {
            munmap(mVMStart, BINDER_VM_SIZE);
        }
        close(mDriverFD);
    }
    mDriverFD = -1;
}

} // namespace android
