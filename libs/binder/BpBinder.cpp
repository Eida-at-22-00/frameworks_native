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

#define LOG_TAG "BpBinder"
//#define LOG_NDEBUG 0

#include <binder/BpBinder.h>

#include <binder/IPCThreadState.h>
#include <binder/IResultReceiver.h>
#include <binder/RpcSession.h>
#include <binder/Stability.h>
#include <binder/Trace.h>

#include <stdio.h>

#include "BuildFlags.h"
#include "Constants.h"
#include "file.h"

//#undef ALOGV
//#define ALOGV(...) fprintf(stderr, __VA_ARGS__)

namespace android {

using android::binder::unique_fd;

// ---------------------------------------------------------------------------

RpcMutex BpBinder::sTrackingLock;
std::unordered_map<int32_t, uint32_t> BpBinder::sTrackingMap;
std::unordered_map<int32_t, uint32_t> BpBinder::sLastLimitCallbackMap;
int BpBinder::sNumTrackedUids = 0;
std::atomic_bool BpBinder::sCountByUidEnabled(false);
binder_proxy_limit_callback BpBinder::sLimitCallback;
binder_proxy_warning_callback BpBinder::sWarningCallback;
bool BpBinder::sBinderProxyThrottleCreate = false;

static StaticString16 kDescriptorUninit(u"");

// Arbitrarily high value that probably distinguishes a bad behaving app
uint32_t BpBinder::sBinderProxyCountHighWatermark = 2500;
// Another arbitrary value a binder count needs to drop below before another callback will be called
uint32_t BpBinder::sBinderProxyCountLowWatermark = 2000;
// Arbitrary value between low and high watermark on a bad behaving app to
// trigger a warning callback.
uint32_t BpBinder::sBinderProxyCountWarningWatermark = 2250;

std::atomic<uint32_t> BpBinder::sBinderProxyCount(0);
std::atomic<uint32_t> BpBinder::sBinderProxyCountWarned(0);

static constexpr uint32_t kBinderProxyCountWarnInterval = 5000;

enum {
    LIMIT_REACHED_MASK = 0x80000000,        // A flag denoting that the limit has been reached
    WARNING_REACHED_MASK = 0x40000000,      // A flag denoting that the warning has been reached
    COUNTING_VALUE_MASK = 0x3FFFFFFF,       // A mask of the remaining bits for the count value
};

BpBinder::ObjectManager::ObjectManager()
{
}

BpBinder::ObjectManager::~ObjectManager()
{
    const size_t N = mObjects.size();
    ALOGV("Killing %zu objects in manager %p", N, this);
    for (auto i : mObjects) {
        const entry_t& e = i.second;
        if (e.func != nullptr) {
            e.func(i.first, e.object, e.cleanupCookie);
        }
    }

    mObjects.clear();
}

void* BpBinder::ObjectManager::attach(const void* objectID, void* object, void* cleanupCookie,
                                      IBinder::object_cleanup_func func) {
    entry_t e;
    e.object = object;
    e.cleanupCookie = cleanupCookie;
    e.func = func;

    if (mObjects.find(objectID) != mObjects.end()) {
        ALOGI("Trying to attach object ID %p to binder ObjectManager %p with object %p, but object "
              "ID already in use",
              objectID, this, object);
        return mObjects[objectID].object;
    }

    mObjects.insert({objectID, e});
    return nullptr;
}

void* BpBinder::ObjectManager::find(const void* objectID) const
{
    auto i = mObjects.find(objectID);
    if (i == mObjects.end()) return nullptr;
    return i->second.object;
}

void* BpBinder::ObjectManager::detach(const void* objectID) {
    auto i = mObjects.find(objectID);
    if (i == mObjects.end()) return nullptr;
    void* value = i->second.object;
    mObjects.erase(i);
    return value;
}

namespace {
struct Tag {
    wp<IBinder> binder;
};
} // namespace

static void cleanWeak(const void* /* id */, void* obj, void* /* cookie */) {
    delete static_cast<Tag*>(obj);
}

sp<IBinder> BpBinder::ObjectManager::lookupOrCreateWeak(const void* objectID, object_make_func make,
                                                        const void* makeArgs) {
    entry_t& e = mObjects[objectID];
    if (e.object != nullptr) {
        if (auto attached = static_cast<Tag*>(e.object)->binder.promote()) {
            return attached;
        }
    } else {
        e.object = new Tag;
        LOG_ALWAYS_FATAL_IF(!e.object, "no more memory");
    }
    sp<IBinder> newObj = make(makeArgs);

    static_cast<Tag*>(e.object)->binder = newObj;
    e.cleanupCookie = nullptr;
    e.func = cleanWeak;

    return newObj;
}

// ---------------------------------------------------------------------------

sp<BpBinder> BpBinder::create(int32_t handle, std::function<void()>* postTask) {
    if constexpr (!kEnableKernelIpc) {
        LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
        return nullptr;
    }
    LOG_ALWAYS_FATAL_IF(postTask == nullptr, "BAD STATE");

    int32_t trackedUid = -1;
    if (sCountByUidEnabled) {
        trackedUid = IPCThreadState::self()->getCallingUid();
        RpcMutexUniqueLock _l(sTrackingLock);
        uint32_t trackedValue = sTrackingMap[trackedUid];
        if (trackedValue & LIMIT_REACHED_MASK) [[unlikely]] {
            if (sBinderProxyThrottleCreate) {
                return nullptr;
            }
            trackedValue = trackedValue & COUNTING_VALUE_MASK;
            uint32_t lastLimitCallbackAt = sLastLimitCallbackMap[trackedUid];

            if (trackedValue > lastLimitCallbackAt &&
                (trackedValue - lastLimitCallbackAt > sBinderProxyCountHighWatermark)) {
                ALOGE("Still too many binder proxy objects sent to uid %d from uid %d (%d proxies "
                      "held)",
                      getuid(), trackedUid, trackedValue);

                if (sLimitCallback) {
                    *postTask = [=]() { sLimitCallback(trackedUid); };
                }

                sLastLimitCallbackMap[trackedUid] = trackedValue;
            }
        } else {
            uint32_t currentValue = trackedValue & COUNTING_VALUE_MASK;
            if (currentValue >= sBinderProxyCountWarningWatermark
                    && currentValue < sBinderProxyCountHighWatermark
                    && ((trackedValue & WARNING_REACHED_MASK) == 0)) [[unlikely]] {
                sTrackingMap[trackedUid] |= WARNING_REACHED_MASK;
                if (sWarningCallback) {
                    *postTask = [=]() { sWarningCallback(trackedUid); };
                }
            } else if (currentValue >= sBinderProxyCountHighWatermark) {
                ALOGE("Too many binder proxy objects sent to uid %d from uid %d (%d proxies held)",
                      getuid(), trackedUid, trackedValue);
                sTrackingMap[trackedUid] |= LIMIT_REACHED_MASK;

                if (sLimitCallback) {
                    *postTask = [=]() { sLimitCallback(trackedUid); };
                }

                sLastLimitCallbackMap[trackedUid] = trackedValue & COUNTING_VALUE_MASK;
                if (sBinderProxyThrottleCreate) {
                    ALOGI("Throttling binder proxy creates from uid %d in uid %d until binder proxy"
                          " count drops below %d",
                          trackedUid, getuid(), sBinderProxyCountLowWatermark);
                    return nullptr;
                }
            }
        }
        sTrackingMap[trackedUid]++;
    }
    uint32_t numProxies = sBinderProxyCount.fetch_add(1, std::memory_order_relaxed);
    binder::os::trace_int(ATRACE_TAG_AIDL, "binder_proxies", numProxies);
    uint32_t numLastWarned = sBinderProxyCountWarned.load(std::memory_order_relaxed);
    uint32_t numNextWarn = numLastWarned + kBinderProxyCountWarnInterval;
    if (numProxies >= numNextWarn) {
        // Multiple threads can get here, make sure only one of them gets to
        // update the warn counter.
        if (sBinderProxyCountWarned.compare_exchange_strong(numLastWarned,
                                                            numNextWarn,
                                                            std::memory_order_relaxed)) {
            ALOGW("Unexpectedly many live BinderProxies: %d\n", numProxies);
        }
    }
    return sp<BpBinder>::make(BinderHandle{handle}, trackedUid);
}

sp<BpBinder> BpBinder::create(const sp<RpcSession>& session, uint64_t address) {
    LOG_ALWAYS_FATAL_IF(session == nullptr, "BpBinder::create null session");

    // These are not currently tracked, since there is no UID or other
    // identifier to track them with. However, if similar functionality is
    // needed, session objects keep track of all BpBinder objects on a
    // per-session basis.

    return sp<BpBinder>::make(RpcHandle{session, address});
}

BpBinder::BpBinder(Handle&& handle)
      : mStability(0),
        mHandle(handle),
        mAlive(true),
        mObitsSent(false),
        mObituaries(nullptr),
        mDescriptorCache(kDescriptorUninit),
        mTrackedUid(-1) {
    extendObjectLifetime(OBJECT_LIFETIME_WEAK);
}

BpBinder::BpBinder(BinderHandle&& handle, int32_t trackedUid) : BpBinder(Handle(handle)) {
    if constexpr (!kEnableKernelIpc) {
        LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
        return;
    }

    mTrackedUid = trackedUid;

    ALOGV("Creating BpBinder %p handle %d\n", this, this->binderHandle());

    IPCThreadState::self()->incWeakHandle(this->binderHandle(), this);
}

BpBinder::BpBinder(RpcHandle&& handle) : BpBinder(Handle(handle)) {
    LOG_ALWAYS_FATAL_IF(rpcSession() == nullptr, "BpBinder created w/o session object");
}

bool BpBinder::isRpcBinder() const {
    return std::holds_alternative<RpcHandle>(mHandle);
}

uint64_t BpBinder::rpcAddress() const {
    return std::get<RpcHandle>(mHandle).address;
}

const sp<RpcSession>& BpBinder::rpcSession() const {
    return std::get<RpcHandle>(mHandle).session;
}

int32_t BpBinder::binderHandle() const {
    return std::get<BinderHandle>(mHandle).handle;
}

std::optional<int32_t> BpBinder::getDebugBinderHandle() const {
    if (!isRpcBinder()) {
        return binderHandle();
    } else {
        return std::nullopt;
    }
}

bool BpBinder::isDescriptorCached() const {
    RpcMutexUniqueLock _l(mLock);
    return mDescriptorCache.c_str() != kDescriptorUninit.c_str();
}

const String16& BpBinder::getInterfaceDescriptor() const
{
    if (!isDescriptorCached()) {
        sp<BpBinder> thiz = sp<BpBinder>::fromExisting(const_cast<BpBinder*>(this));

        Parcel data;
        data.markForBinder(thiz);
        Parcel reply;
        // do the IPC without a lock held.
        status_t err = thiz->transact(INTERFACE_TRANSACTION, data, &reply);
        if (err == NO_ERROR) {
            String16 res(reply.readString16());
            RpcMutexUniqueLock _l(mLock);
            // mDescriptorCache could have been assigned while the lock was
            // released.
            if (mDescriptorCache.c_str() == kDescriptorUninit.c_str()) mDescriptorCache = res;
        }
    }

    // we're returning a reference to a non-static object here. Usually this
    // is not something smart to do, however, with binder objects it is
    // (usually) safe because they are reference-counted.

    return mDescriptorCache;
}

bool BpBinder::isBinderAlive() const
{
    return mAlive != 0;
}

status_t BpBinder::pingBinder()
{
    Parcel data;
    data.markForBinder(sp<BpBinder>::fromExisting(this));
    Parcel reply;
    return transact(PING_TRANSACTION, data, &reply);
}

status_t BpBinder::startRecordingBinder(const unique_fd& fd) {
    Parcel send, reply;
    send.writeUniqueFileDescriptor(fd);
    return transact(START_RECORDING_TRANSACTION, send, &reply);
}

status_t BpBinder::stopRecordingBinder() {
    Parcel data, reply;
    data.markForBinder(sp<BpBinder>::fromExisting(this));
    return transact(STOP_RECORDING_TRANSACTION, data, &reply);
}

status_t BpBinder::dump(int fd, const Vector<String16>& args)
{
    Parcel send;
    Parcel reply;
    send.writeFileDescriptor(fd);
    const size_t numArgs = args.size();
    send.writeInt32(numArgs);
    for (size_t i = 0; i < numArgs; i++) {
        send.writeString16(args[i]);
    }
    status_t err = transact(DUMP_TRANSACTION, send, &reply);
    return err;
}

// NOLINTNEXTLINE(google-default-arguments)
status_t BpBinder::transact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    // Once a binder has died, it will never come back to life.
    if (mAlive) {
        bool privateVendor = flags & FLAG_PRIVATE_VENDOR;
        // don't send userspace flags to the kernel
        flags = flags & ~static_cast<uint32_t>(FLAG_PRIVATE_VENDOR);

        // user transactions require a given stability level
        if (code >= FIRST_CALL_TRANSACTION && code <= LAST_CALL_TRANSACTION) {
            using android::internal::Stability;

            int16_t stability = Stability::getRepr(this);
            Stability::Level required = privateVendor ? Stability::VENDOR
                : Stability::getLocalLevel();

            if (!Stability::check(stability, required)) [[unlikely]] {
                ALOGE("Cannot do a user transaction on a %s binder (%s) in a %s context.",
                      Stability::levelString(stability).c_str(),
                      String8(getInterfaceDescriptor()).c_str(),
                      Stability::levelString(required).c_str());
                return BAD_TYPE;
            }
        }

        status_t status;
        if (isRpcBinder()) [[unlikely]] {
            status = rpcSession()->transact(sp<IBinder>::fromExisting(this), code, data, reply,
                                            flags);
        } else {
            if constexpr (!kEnableKernelIpc) {
                LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
                return INVALID_OPERATION;
            }

            status = IPCThreadState::self()->transact(binderHandle(), code, data, reply, flags);
        }

        if (data.dataSize() > binder::kLogTransactionsOverBytes) {
            RpcMutexUniqueLock _l(mLock);
            ALOGW("Large outgoing transaction of %zu bytes, interface descriptor %s, code %d was "
                  "sent",
                  data.dataSize(), String8(mDescriptorCache).c_str(), code);
        }

        if (status == DEAD_OBJECT) mAlive = 0;

        return status;
    }

    return DEAD_OBJECT;
}

// NOLINTNEXTLINE(google-default-arguments)
status_t BpBinder::linkToDeath(
    const sp<DeathRecipient>& recipient, void* cookie, uint32_t flags)
{
    if (isRpcBinder()) {
        if (rpcSession()->getMaxIncomingThreads() < 1) {
            ALOGE("Cannot register a DeathRecipient without any incoming threads. Need to set max "
                  "incoming threads to a value greater than 0 before calling linkToDeath.");
            return INVALID_OPERATION;
        }
    } else if constexpr (!kEnableKernelIpc) {
        LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
        return INVALID_OPERATION;
    } else {
        if (ProcessState::self()->getThreadPoolMaxTotalThreadCount() == 0) {
            ALOGW("Linking to death on %s but there are no threads (yet?) listening to incoming "
                  "transactions. See ProcessState::startThreadPool and "
                  "ProcessState::setThreadPoolMaxThreadCount. Generally you should setup the "
                  "binder "
                  "threadpool before other initialization steps.",
                  String8(getInterfaceDescriptor()).c_str());
        }
    }

    Obituary ob;
    ob.recipient = recipient;
    ob.cookie = cookie;
    ob.flags = flags;

    LOG_ALWAYS_FATAL_IF(recipient == nullptr,
                        "linkToDeath(): recipient must be non-NULL");

    {
        RpcMutexUniqueLock _l(mLock);

        if (!mObitsSent) {
            if (!mObituaries) {
                mObituaries = new Vector<Obituary>;
                if (!mObituaries) {
                    return NO_MEMORY;
                }
                ALOGV("Requesting death notification: %p handle %d\n", this, binderHandle());
                if (!isRpcBinder()) {
                    if constexpr (kEnableKernelIpc) {
                        getWeakRefs()->incWeak(this);
                        IPCThreadState* self = IPCThreadState::self();
                        self->requestDeathNotification(binderHandle(), this);
                        self->flushCommands();
                    }
                }
            }
            ssize_t res = mObituaries->add(ob);
            return res >= (ssize_t)NO_ERROR ? (status_t)NO_ERROR : res;
        }
    }

    return DEAD_OBJECT;
}

// NOLINTNEXTLINE(google-default-arguments)
status_t BpBinder::unlinkToDeath(
    const wp<DeathRecipient>& recipient, void* cookie, uint32_t flags,
    wp<DeathRecipient>* outRecipient)
{
    if (!kEnableKernelIpc && !isRpcBinder()) {
        LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
        return INVALID_OPERATION;
    }

    RpcMutexUniqueLock _l(mLock);

    if (mObitsSent) {
        return DEAD_OBJECT;
    }

    const size_t N = mObituaries ? mObituaries->size() : 0;
    for (size_t i=0; i<N; i++) {
        const Obituary& obit = mObituaries->itemAt(i);
        if ((obit.recipient == recipient
                    || (recipient == nullptr && obit.cookie == cookie))
                && obit.flags == flags) {
            if (outRecipient != nullptr) {
                *outRecipient = mObituaries->itemAt(i).recipient;
            }
            mObituaries->removeAt(i);
            if (mObituaries->size() == 0) {
                ALOGV("Clearing death notification: %p handle %d\n", this, binderHandle());
                if (!isRpcBinder()) {
                    if constexpr (kEnableKernelIpc) {
                        IPCThreadState* self = IPCThreadState::self();
                        self->clearDeathNotification(binderHandle(), this);
                        self->flushCommands();
                    }
                }
                delete mObituaries;
                mObituaries = nullptr;
            }
            return NO_ERROR;
        }
    }

    return NAME_NOT_FOUND;
}

void BpBinder::sendObituary()
{
    if (!kEnableKernelIpc && !isRpcBinder()) {
        LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
        return;
    }

    ALOGV("Sending obituary for proxy %p handle %d, mObitsSent=%s\n", this, binderHandle(),
          mObitsSent ? "true" : "false");

    mAlive = 0;
    if (mObitsSent) return;

    mLock.lock();
    Vector<Obituary>* obits = mObituaries;
    if(obits != nullptr) {
        ALOGV("Clearing sent death notification: %p handle %d\n", this, binderHandle());
        if (!isRpcBinder()) {
            if constexpr (kEnableKernelIpc) {
                IPCThreadState* self = IPCThreadState::self();
                self->clearDeathNotification(binderHandle(), this);
                self->flushCommands();
            }
        }
        mObituaries = nullptr;
    }
    mObitsSent = 1;
    mLock.unlock();

    ALOGV("Reporting death of proxy %p for %zu recipients\n",
        this, obits ? obits->size() : 0U);

    if (obits != nullptr) {
        const size_t N = obits->size();
        for (size_t i=0; i<N; i++) {
            reportOneDeath(obits->itemAt(i));
        }

        delete obits;
    }
}

status_t BpBinder::addFrozenStateChangeCallback(const wp<FrozenStateChangeCallback>& callback) {
    LOG_ALWAYS_FATAL_IF(isRpcBinder(),
                        "addFrozenStateChangeCallback() is not supported for RPC Binder.");
    LOG_ALWAYS_FATAL_IF(!kEnableKernelIpc, "Binder kernel driver disabled at build time");
    LOG_ALWAYS_FATAL_IF(ProcessState::self()->getThreadPoolMaxTotalThreadCount() == 0,
                        "addFrozenStateChangeCallback on %s but there are no threads "
                        "(yet?) listening to incoming transactions. See "
                        "ProcessState::startThreadPool "
                        "and ProcessState::setThreadPoolMaxThreadCount. Generally you should "
                        "setup the binder threadpool before other initialization steps.",
                        String8(getInterfaceDescriptor()).c_str());
    LOG_ALWAYS_FATAL_IF(callback == nullptr,
                        "addFrozenStateChangeCallback(): callback must be non-NULL");

    const sp<FrozenStateChangeCallback> strongCallback = callback.promote();
    if (strongCallback == nullptr) {
        return BAD_VALUE;
    }

    {
        RpcMutexUniqueLock _l(mLock);
        if (!mFrozen) {
            ALOGV("Requesting freeze notification: %p handle %d\n", this, binderHandle());
            IPCThreadState* self = IPCThreadState::self();
            status_t status = self->addFrozenStateChangeCallback(binderHandle(), this);
            if (status != NO_ERROR) {
                // Avoids logspam if kernel does not support freeze
                // notification.
                if (status != INVALID_OPERATION) {
                    ALOGE("IPCThreadState.addFrozenStateChangeCallback "
                          "failed with %s. %p handle %d\n",
                          statusToString(status).c_str(), this, binderHandle());
                }
                return status;
            }
            mFrozen = std::make_unique<FrozenStateChange>();
            if (!mFrozen) {
                std::ignore =
                        IPCThreadState::self()->removeFrozenStateChangeCallback(binderHandle(),
                                                                                this);
                return NO_MEMORY;
            }
        }
        if (mFrozen->initialStateReceived) {
            strongCallback->onStateChanged(wp<BpBinder>::fromExisting(this),
                                           mFrozen->isFrozen
                                                   ? FrozenStateChangeCallback::State::FROZEN
                                                   : FrozenStateChangeCallback::State::UNFROZEN);
        }
        ssize_t res = mFrozen->callbacks.add(callback);
        if (res < 0) {
            return res;
        }
        return NO_ERROR;
    }
}

status_t BpBinder::removeFrozenStateChangeCallback(const wp<FrozenStateChangeCallback>& callback) {
    LOG_ALWAYS_FATAL_IF(isRpcBinder(),
                        "removeFrozenStateChangeCallback() is not supported for RPC Binder.");
    LOG_ALWAYS_FATAL_IF(!kEnableKernelIpc, "Binder kernel driver disabled at build time");

    RpcMutexUniqueLock _l(mLock);

    const size_t N = mFrozen ? mFrozen->callbacks.size() : 0;
    for (size_t i = 0; i < N; i++) {
        if (mFrozen->callbacks.itemAt(i) == callback) {
            mFrozen->callbacks.removeAt(i);
            if (mFrozen->callbacks.size() == 0) {
                ALOGV("Clearing freeze notification: %p handle %d\n", this, binderHandle());
                status_t status =
                        IPCThreadState::self()->removeFrozenStateChangeCallback(binderHandle(),
                                                                                this);
                if (status != NO_ERROR) {
                    ALOGE("Unexpected error from "
                          "IPCThreadState.removeFrozenStateChangeCallback: %s. "
                          "%p handle %d\n",
                          statusToString(status).c_str(), this, binderHandle());
                }
                mFrozen.reset();
            }
            return NO_ERROR;
        }
    }

    return NAME_NOT_FOUND;
}

void BpBinder::onFrozenStateChanged(bool isFrozen) {
    LOG_ALWAYS_FATAL_IF(isRpcBinder(), "onFrozenStateChanged is not supported for RPC Binder.");
    LOG_ALWAYS_FATAL_IF(!kEnableKernelIpc, "Binder kernel driver disabled at build time");

    ALOGV("Sending frozen state change notification for proxy %p handle %d, isFrozen=%s\n", this,
          binderHandle(), isFrozen ? "true" : "false");

    RpcMutexUniqueLock _l(mLock);
    if (!mFrozen) {
        return;
    }
    bool stateChanged = !mFrozen->initialStateReceived || mFrozen->isFrozen != isFrozen;
    if (stateChanged) {
        mFrozen->isFrozen = isFrozen;
        mFrozen->initialStateReceived = true;
        for (size_t i = 0; i < mFrozen->callbacks.size();) {
            sp<FrozenStateChangeCallback> callback = mFrozen->callbacks.itemAt(i).promote();
            if (callback != nullptr) {
                callback->onStateChanged(wp<BpBinder>::fromExisting(this),
                                         isFrozen ? FrozenStateChangeCallback::State::FROZEN
                                                  : FrozenStateChangeCallback::State::UNFROZEN);
                i++;
            } else {
                mFrozen->callbacks.removeItemsAt(i);
            }
        }
    }
}

void BpBinder::reportOneDeath(const Obituary& obit)
{
    sp<DeathRecipient> recipient = obit.recipient.promote();
    ALOGV("Reporting death to recipient: %p\n", recipient.get());
    if (recipient == nullptr) return;

    recipient->binderDied(wp<BpBinder>::fromExisting(this));
}

void* BpBinder::attachObject(const void* objectID, void* object, void* cleanupCookie,
                             object_cleanup_func func) {
    RpcMutexUniqueLock _l(mLock);
    ALOGV("Attaching object %p to binder %p (manager=%p)", object, this, &mObjectMgr);
    return mObjectMgr.attach(objectID, object, cleanupCookie, func);
}

void* BpBinder::findObject(const void* objectID) const
{
    RpcMutexUniqueLock _l(mLock);
    return mObjectMgr.find(objectID);
}

void* BpBinder::detachObject(const void* objectID) {
    RpcMutexUniqueLock _l(mLock);
    return mObjectMgr.detach(objectID);
}

void BpBinder::withLock(const std::function<void()>& doWithLock) {
    RpcMutexUniqueLock _l(mLock);
    doWithLock();
}

sp<IBinder> BpBinder::lookupOrCreateWeak(const void* objectID, object_make_func make,
                                         const void* makeArgs) {
    RpcMutexUniqueLock _l(mLock);
    return mObjectMgr.lookupOrCreateWeak(objectID, make, makeArgs);
}

BpBinder* BpBinder::remoteBinder()
{
    return this;
}

BpBinder::~BpBinder() {
    if (isRpcBinder()) [[unlikely]] {
        return;
    }

    if constexpr (!kEnableKernelIpc) {
        LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
        return;
    }

    ALOGV("Destroying BpBinder %p handle %d\n", this, binderHandle());

    IPCThreadState* ipc = IPCThreadState::self();

    if (mTrackedUid >= 0) {
        RpcMutexUniqueLock _l(sTrackingLock);
        uint32_t trackedValue = sTrackingMap[mTrackedUid];
        if ((trackedValue & COUNTING_VALUE_MASK) == 0) [[unlikely]] {
            ALOGE("Unexpected Binder Proxy tracking decrement in %p handle %d\n", this,
                  binderHandle());
        } else {
            auto countingValue = trackedValue & COUNTING_VALUE_MASK;
            if ((trackedValue & (LIMIT_REACHED_MASK | WARNING_REACHED_MASK)) &&
                (countingValue <= sBinderProxyCountLowWatermark)) [[unlikely]] {
                ALOGI("Limit reached bit reset for uid %d (fewer than %d proxies from uid %d held)",
                      getuid(), sBinderProxyCountLowWatermark, mTrackedUid);
                sTrackingMap[mTrackedUid] &= ~(LIMIT_REACHED_MASK | WARNING_REACHED_MASK);
                sLastLimitCallbackMap.erase(mTrackedUid);
            }
            if (--sTrackingMap[mTrackedUid] == 0) {
                sTrackingMap.erase(mTrackedUid);
            }
        }
    }
    uint32_t numProxies = --sBinderProxyCount;
    binder::os::trace_int(ATRACE_TAG_AIDL, "binder_proxies", numProxies);
    if (ipc) {
        ipc->expungeHandle(binderHandle(), this);
        ipc->decWeakHandle(binderHandle());
    }
}

void BpBinder::onFirstRef() {
    if (isRpcBinder()) [[unlikely]] {
        return;
    }

    if constexpr (!kEnableKernelIpc) {
        LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
        return;
    }

    ALOGV("onFirstRef BpBinder %p handle %d\n", this, binderHandle());
    IPCThreadState* ipc = IPCThreadState::self();
    if (ipc) ipc->incStrongHandle(binderHandle(), this);
}

void BpBinder::onLastStrongRef(const void* /*id*/) {
    if (isRpcBinder()) [[unlikely]] {
        (void)rpcSession()->sendDecStrong(this);
        return;
    }

    if constexpr (!kEnableKernelIpc) {
        LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
        return;
    }

    ALOGV("onLastStrongRef BpBinder %p handle %d\n", this, binderHandle());
    IF_ALOGV() {
        printRefs();
    }
    IPCThreadState* ipc = IPCThreadState::self();
    if (ipc) ipc->decStrongHandle(binderHandle());

    mLock.lock();
    Vector<Obituary>* obits = mObituaries;
    if(obits != nullptr) {
        if (!obits->isEmpty()) {
            ALOGV("onLastStrongRef automatically unlinking death recipients: %s",
                  String8(mDescriptorCache).c_str());
        }

        if (ipc) ipc->clearDeathNotification(binderHandle(), this);
        mObituaries = nullptr;
    }
    if (mFrozen != nullptr) {
        std::ignore = IPCThreadState::self()->removeFrozenStateChangeCallback(binderHandle(), this);
        mFrozen.reset();
    }
    mLock.unlock();

    if (obits != nullptr) {
        // XXX Should we tell any remaining DeathRecipient
        // objects that the last strong ref has gone away, so they
        // are no longer linked?
        delete obits;
    }
}

bool BpBinder::onIncStrongAttempted(uint32_t /*flags*/, const void* /*id*/)
{
    // RPC binder doesn't currently support inc from weak binders
    if (isRpcBinder()) [[unlikely]] {
        return false;
    }

    if constexpr (!kEnableKernelIpc) {
        LOG_ALWAYS_FATAL("Binder kernel driver disabled at build time");
        return false;
    }

    ALOGV("onIncStrongAttempted BpBinder %p handle %d\n", this, binderHandle());
    IPCThreadState* ipc = IPCThreadState::self();
    return ipc ? ipc->attemptIncStrongHandle(binderHandle()) == NO_ERROR : false;
}

uint32_t BpBinder::getBinderProxyCount(uint32_t uid)
{
    RpcMutexUniqueLock _l(sTrackingLock);
    auto it = sTrackingMap.find(uid);
    if (it != sTrackingMap.end()) {
        return it->second & COUNTING_VALUE_MASK;
    }
    return 0;
}

uint32_t BpBinder::getBinderProxyCount()
{
    return sBinderProxyCount.load();
}

void BpBinder::getCountByUid(Vector<uint32_t>& uids, Vector<uint32_t>& counts)
{
    RpcMutexUniqueLock _l(sTrackingLock);
    uids.setCapacity(sTrackingMap.size());
    counts.setCapacity(sTrackingMap.size());
    for (const auto& it : sTrackingMap) {
        uids.push_back(it.first);
        counts.push_back(it.second & COUNTING_VALUE_MASK);
    }
}

void BpBinder::enableCountByUid() { sCountByUidEnabled.store(true); }
void BpBinder::disableCountByUid() { sCountByUidEnabled.store(false); }
void BpBinder::setCountByUidEnabled(bool enable) { sCountByUidEnabled.store(enable); }

void BpBinder::setBinderProxyCountEventCallback(binder_proxy_limit_callback cbl,
                                                binder_proxy_warning_callback cbw) {
    RpcMutexUniqueLock _l(sTrackingLock);
    sLimitCallback = std::move(cbl);
    sWarningCallback = std::move(cbw);
}

void BpBinder::setBinderProxyCountWatermarks(int high, int low, int warning) {
    RpcMutexUniqueLock _l(sTrackingLock);
    sBinderProxyCountHighWatermark = high;
    sBinderProxyCountLowWatermark = low;
    sBinderProxyCountWarningWatermark = warning;
}

// ---------------------------------------------------------------------------

} // namespace android
