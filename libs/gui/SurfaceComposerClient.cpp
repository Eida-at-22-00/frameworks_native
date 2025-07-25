/*
 * Copyright (C) 2007 The Android Open Source Project
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

#define LOG_TAG "SurfaceComposerClient"

#include <semaphore.h>
#include <stdint.h>
#include <sys/types.h>
#include <algorithm>

#include <android/gui/BnWindowInfosReportedListener.h>
#include <android/gui/DisplayState.h>
#include <android/gui/EdgeExtensionParameters.h>
#include <android/gui/ISurfaceComposerClient.h>
#include <android/gui/IWindowInfosListener.h>
#include <android/gui/TrustedPresentationThresholds.h>
#include <android/os/IInputConstants.h>
#include <com_android_graphics_libgui_flags.h>
#include <gui/DisplayLuts.h>
#include <gui/FrameRateUtils.h>
#include <gui/TraceUtils.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/SortedVector.h>
#include <utils/String8.h>
#include <utils/threads.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>

#include <system/graphics.h>

#include <gui/AidlUtil.h>
#include <gui/BufferItemConsumer.h>
#include <gui/CpuConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/ISurfaceComposer.h>
#include <gui/LayerState.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/WindowInfo.h>
#include <private/gui/ParcelUtils.h>
#include <ui/DisplayMode.h>
#include <ui/DisplayState.h>
#include <ui/DynamicDisplayInfo.h>
#include <ui/FrameRateCategoryRate.h>

#include <android-base/thread_annotations.h>
#include <gui/LayerStatePermissions.h>
#include <gui/ScreenCaptureResults.h>
#include <private/gui/ComposerService.h>
#include <private/gui/ComposerServiceAIDL.h>

// This server size should always be smaller than the server cache size
#define BUFFER_CACHE_MAX_SIZE 4096

namespace android {

using aidl::android::hardware::graphics::common::DisplayDecorationSupport;
using gui::FocusRequest;
using gui::IRegionSamplingListener;
using gui::TrustedPresentationThresholds;
using gui::WindowInfo;
using gui::WindowInfoHandle;
using gui::WindowInfosListener;
using gui::aidl_utils::statusTFromBinderStatus;
using ui::ColorMode;
// ---------------------------------------------------------------------------

ANDROID_SINGLETON_STATIC_INSTANCE(ComposerService);
ANDROID_SINGLETON_STATIC_INSTANCE(ComposerServiceAIDL);

namespace {
// Initialize transaction id counter used to generate transaction ids
std::atomic<uint32_t> idCounter = 0;
int64_t generateId() {
    return (((int64_t)getpid()) << 32) | ++idCounter;
}

constexpr int64_t INVALID_VSYNC = -1;
const constexpr char* LOG_SURFACE_CONTROL_REGISTRY = "SurfaceControlRegistry";

} // namespace

const std::string SurfaceComposerClient::kEmpty{};

ComposerService::ComposerService()
: Singleton<ComposerService>() {
    Mutex::Autolock _l(mLock);
    connectLocked();
}

bool ComposerService::connectLocked() {
    const String16 name("SurfaceFlinger");
    mComposerService = waitForService<ISurfaceComposer>(name);
    if (mComposerService == nullptr) {
        return false; // fatal error or permission problem
    }

    // Create the death listener.
    class DeathObserver : public IBinder::DeathRecipient {
        ComposerService& mComposerService;
        virtual void binderDied(const wp<IBinder>& who) {
            ALOGW("ComposerService remote (surfaceflinger) died [%p]",
                  who.unsafe_get());
            mComposerService.composerServiceDied();
        }
     public:
        explicit DeathObserver(ComposerService& mgr) : mComposerService(mgr) { }
    };

    mDeathObserver = sp<DeathObserver>::make(*const_cast<ComposerService*>(this));
    IInterface::asBinder(mComposerService)->linkToDeath(mDeathObserver);
    return true;
}

/*static*/ sp<ISurfaceComposer> ComposerService::getComposerService() {
    ComposerService& instance = ComposerService::getInstance();
    Mutex::Autolock _l(instance.mLock);
    if (instance.mComposerService == nullptr) {
        if (ComposerService::getInstance().connectLocked()) {
            ALOGD("ComposerService reconnected");
        }
    }
    return instance.mComposerService;
}

void ComposerService::composerServiceDied()
{
    Mutex::Autolock _l(mLock);
    mComposerService = nullptr;
    mDeathObserver = nullptr;
}

ComposerServiceAIDL::ComposerServiceAIDL() : Singleton<ComposerServiceAIDL>() {
    std::scoped_lock lock(mMutex);
    connectLocked();
}

bool ComposerServiceAIDL::connectLocked() {
    const String16 name("SurfaceFlingerAIDL");
    mComposerService = waitForService<gui::ISurfaceComposer>(name);
    if (mComposerService == nullptr) {
        return false; // fatal error or permission problem
    }

    // Create the death listener.
    class DeathObserver : public IBinder::DeathRecipient {
        ComposerServiceAIDL& mComposerService;
        virtual void binderDied(const wp<IBinder>& who) {
            ALOGW("ComposerService aidl remote (surfaceflinger) died [%p]", who.unsafe_get());
            mComposerService.composerServiceDied();
        }

    public:
        explicit DeathObserver(ComposerServiceAIDL& mgr) : mComposerService(mgr) {}
    };

    mDeathObserver = sp<DeathObserver>::make(*const_cast<ComposerServiceAIDL*>(this));
    IInterface::asBinder(mComposerService)->linkToDeath(mDeathObserver);
    return true;
}

/*static*/ sp<gui::ISurfaceComposer> ComposerServiceAIDL::getComposerService() {
    ComposerServiceAIDL& instance = ComposerServiceAIDL::getInstance();
    std::scoped_lock lock(instance.mMutex);
    if (instance.mComposerService == nullptr) {
        if (ComposerServiceAIDL::getInstance().connectLocked()) {
            ALOGD("ComposerServiceAIDL reconnected");
            WindowInfosListenerReporter::getInstance()->reconnect(instance.mComposerService);
        }
    }
    return instance.mComposerService;
}

void ComposerServiceAIDL::composerServiceDied() {
    std::scoped_lock lock(mMutex);
    mComposerService = nullptr;
    mDeathObserver = nullptr;
}

class DefaultComposerClient: public Singleton<DefaultComposerClient> {
    Mutex mLock;
    sp<SurfaceComposerClient> mClient;
    friend class Singleton<ComposerService>;
public:
    static sp<SurfaceComposerClient> getComposerClient() {
        DefaultComposerClient& dc = DefaultComposerClient::getInstance();
        Mutex::Autolock _l(dc.mLock);
        if (dc.mClient == nullptr) {
            dc.mClient = sp<SurfaceComposerClient>::make();
        }
        return dc.mClient;
    }
};
ANDROID_SINGLETON_STATIC_INSTANCE(DefaultComposerClient);


sp<SurfaceComposerClient> SurfaceComposerClient::getDefault() {
    return DefaultComposerClient::getComposerClient();
}

// ---------------------------------------------------------------------------

JankDataListener::~JankDataListener() {
}

status_t JankDataListener::flushJankData() {
    if (mLayerId == -1) {
        return INVALID_OPERATION;
    }

    binder::Status status = ComposerServiceAIDL::getComposerService()->flushJankData(mLayerId);
    return statusTFromBinderStatus(status);
}

std::mutex JankDataListenerFanOut::sFanoutInstanceMutex;
std::unordered_map<int32_t, sp<JankDataListenerFanOut>> JankDataListenerFanOut::sFanoutInstances;

binder::Status JankDataListenerFanOut::onJankData(const std::vector<gui::JankData>& jankData) {
    // Find the highest VSync ID.
    int64_t lastVsync = jankData.empty()
            ? 0
            : std::max_element(jankData.begin(), jankData.end(),
                               [](const gui::JankData& jd1, const gui::JankData& jd2) {
                                   return jd1.frameVsyncId < jd2.frameVsyncId;
                               })
                      ->frameVsyncId;

    // Fan out the jank data callback.
    std::vector<wp<JankDataListener>> listenersToRemove;
    for (auto listener : getActiveListeners()) {
        if (!listener->onJankDataAvailable(jankData) ||
            (listener->mRemoveAfter >= 0 && listener->mRemoveAfter <= lastVsync)) {
            listenersToRemove.push_back(listener);
        }
    }

    return removeListeners(listenersToRemove)
            ? binder::Status::ok()
            : binder::Status::fromExceptionCode(binder::Status::EX_NULL_POINTER);
}

status_t JankDataListenerFanOut::addListener(sp<SurfaceControl> sc, sp<JankDataListener> listener) {
    sp<IBinder> layer = sc->getHandle();
    if (layer == nullptr) {
        return UNEXPECTED_NULL;
    }
    int32_t layerId = sc->getLayerId();

    sFanoutInstanceMutex.lock();
    auto it = sFanoutInstances.find(layerId);
    bool registerNeeded = it == sFanoutInstances.end();
    sp<JankDataListenerFanOut> fanout;
    if (registerNeeded) {
        fanout = sp<JankDataListenerFanOut>::make(layerId);
        sFanoutInstances.insert({layerId, fanout});
    } else {
        fanout = it->second;
    }

    fanout->mMutex.lock();
    fanout->mListeners.insert(listener);
    fanout->mMutex.unlock();

    sFanoutInstanceMutex.unlock();

    if (registerNeeded) {
        binder::Status status =
                ComposerServiceAIDL::getComposerService()->addJankListener(layer, fanout);
        return statusTFromBinderStatus(status);
    }
    return OK;
}

status_t JankDataListenerFanOut::removeListener(sp<JankDataListener> listener) {
    int32_t layerId = listener->mLayerId;
    if (layerId == -1) {
        return INVALID_OPERATION;
    }

    int64_t removeAfter = INVALID_VSYNC;
    sp<JankDataListenerFanOut> fanout;

    sFanoutInstanceMutex.lock();
    auto it = sFanoutInstances.find(layerId);
    if (it != sFanoutInstances.end()) {
        fanout = it->second;
        removeAfter = fanout->updateAndGetRemovalVSync();
    }

    if (removeAfter != INVALID_VSYNC) {
        // Remove this instance from the map, so that no new listeners are added
        // while we're scheduled to be removed.
        sFanoutInstances.erase(layerId);
    }
    sFanoutInstanceMutex.unlock();

    if (removeAfter < 0) {
        return OK;
    }

    binder::Status status =
            ComposerServiceAIDL::getComposerService()->removeJankListener(layerId, fanout,
                                                                          removeAfter);
    return statusTFromBinderStatus(status);
}

std::vector<sp<JankDataListener>> JankDataListenerFanOut::getActiveListeners() {
    std::scoped_lock<std::mutex> lock(mMutex);

    std::vector<sp<JankDataListener>> listeners;
    for (auto it = mListeners.begin(); it != mListeners.end();) {
        auto listener = it->promote();
        if (!listener) {
            it = mListeners.erase(it);
        } else {
            listeners.push_back(std::move(listener));
            it++;
        }
    }
    return listeners;
}

bool JankDataListenerFanOut::removeListeners(const std::vector<wp<JankDataListener>>& listeners) {
    std::scoped_lock<std::mutex> fanoutLock(sFanoutInstanceMutex);
    std::scoped_lock<std::mutex> listenersLock(mMutex);

    for (auto listener : listeners) {
        mListeners.erase(listener);
    }

    if (mListeners.empty()) {
        sFanoutInstances.erase(mLayerId);
        return false;
    }
    return true;
}

int64_t JankDataListenerFanOut::updateAndGetRemovalVSync() {
    std::scoped_lock<std::mutex> lock(mMutex);
    if (mRemoveAfter >= 0) {
        // We've already been scheduled to be removed. Don't schedule again.
        return INVALID_VSYNC;
    }

    int64_t removeAfter = 0;
    for (auto it = mListeners.begin(); it != mListeners.end();) {
        auto listener = it->promote();
        if (!listener) {
            it = mListeners.erase(it);
        } else if (listener->mRemoveAfter < 0) {
            // We have at least one listener that's still interested. Don't remove.
            return INVALID_VSYNC;
        } else {
            removeAfter = std::max(removeAfter, listener->mRemoveAfter);
            it++;
        }
    }

    mRemoveAfter = removeAfter;
    return removeAfter;
}

// ---------------------------------------------------------------------------

// TransactionCompletedListener does not use ANDROID_SINGLETON_STATIC_INSTANCE because it needs
// to be able to return a sp<> to its instance to pass to SurfaceFlinger.
// ANDROID_SINGLETON_STATIC_INSTANCE only allows a reference to an instance.

// 0 is an invalid callback id
TransactionCompletedListener::TransactionCompletedListener() : mCallbackIdCounter(1) {}

int64_t TransactionCompletedListener::getNextIdLocked() {
    return mCallbackIdCounter++;
}

sp<TransactionCompletedListener> TransactionCompletedListener::sInstance = nullptr;
static std::mutex sListenerInstanceMutex;

void TransactionCompletedListener::setInstance(const sp<TransactionCompletedListener>& listener) {
    sInstance = listener;
}

sp<TransactionCompletedListener> TransactionCompletedListener::getInstance() {
    std::lock_guard<std::mutex> lock(sListenerInstanceMutex);
    if (sInstance == nullptr) {
        sInstance = sp<TransactionCompletedListener>::make();
    }
    return sInstance;
}

sp<ITransactionCompletedListener> TransactionCompletedListener::getIInstance() {
    return static_cast<sp<ITransactionCompletedListener>>(getInstance());
}

void TransactionCompletedListener::startListeningLocked() {
    if (mListening) {
        return;
    }
    ProcessState::self()->startThreadPool();
    mListening = true;
}

CallbackId TransactionCompletedListener::addCallbackFunction(
        const TransactionCompletedCallback& callbackFunction,
        const std::unordered_set<sp<SurfaceControl>, SurfaceComposerClient::SCHash>&
                surfaceControls,
        CallbackId::Type callbackType) {
    std::lock_guard<std::mutex> lock(mMutex);
    startListeningLocked();

    CallbackId callbackId(getNextIdLocked(), callbackType);
    mCallbacks[callbackId].callbackFunction = callbackFunction;
    auto& callbackSurfaceControls = mCallbacks[callbackId].surfaceControls;

    for (const auto& surfaceControl : surfaceControls) {
        callbackSurfaceControls[surfaceControl->getHandle()] = surfaceControl;
    }

    return callbackId;
}

void TransactionCompletedListener::setReleaseBufferCallback(const ReleaseCallbackId& callbackId,
                                                            ReleaseBufferCallback listener) {
    std::scoped_lock<std::mutex> lock(mMutex);
    mReleaseBufferCallbacks[callbackId] = listener;
}

void TransactionCompletedListener::addSurfaceStatsListener(void* context, void* cookie,
        sp<SurfaceControl> surfaceControl, SurfaceStatsCallback listener) {
    std::scoped_lock<std::recursive_mutex> lock(mSurfaceStatsListenerMutex);
    mSurfaceStatsListeners.insert(
            {surfaceControl->getLayerId(), SurfaceStatsCallbackEntry(context, cookie, listener)});
}

void TransactionCompletedListener::removeSurfaceStatsListener(void* context, void* cookie) {
    std::scoped_lock<std::recursive_mutex> lock(mSurfaceStatsListenerMutex);
    for (auto it = mSurfaceStatsListeners.begin(); it != mSurfaceStatsListeners.end();) {
        auto [itContext, itCookie, itListener] = it->second;
        if (itContext == context && itCookie == cookie) {
            it = mSurfaceStatsListeners.erase(it);
        } else {
            it++;
        }
    }
}

void TransactionCompletedListener::addSurfaceControlToCallbacks(
        const sp<SurfaceControl>& surfaceControl,
        const std::unordered_set<CallbackId, CallbackIdHash>& callbackIds) {
    std::lock_guard<std::mutex> lock(mMutex);

    for (auto callbackId : callbackIds) {
        mCallbacks[callbackId].surfaceControls.emplace(std::piecewise_construct,
                                                       std::forward_as_tuple(
                                                               surfaceControl->getHandle()),
                                                       std::forward_as_tuple(surfaceControl));
    }
}

void TransactionCompletedListener::onTransactionCompleted(ListenerStats listenerStats) {
    std::unordered_map<CallbackId, CallbackTranslation, CallbackIdHash> callbacksMap;
    {
        std::lock_guard<std::mutex> lock(mMutex);

        /* This listener knows all the sp<IBinder> to sp<SurfaceControl> for all its registered
         * callbackIds, except for when Transactions are merged together. This probably cannot be
         * solved before this point because the Transactions could be merged together and applied in
         * a different process.
         *
         * Fortunately, we get all the callbacks for this listener for the same frame together at
         * the same time. This means if any Transactions were merged together, we will get their
         * callbacks at the same time. We can combine all the sp<IBinder> to sp<SurfaceControl> maps
         * for all the callbackIds to generate one super map that contains all the sp<IBinder> to
         * sp<SurfaceControl> that could possibly exist for the callbacks.
         */
        callbacksMap = mCallbacks;
        for (const auto& transactionStats : listenerStats.transactionStats) {
            for (auto& callbackId : transactionStats.callbackIds) {
                mCallbacks.erase(callbackId);
            }
        }
    }
    for (const auto& transactionStats : listenerStats.transactionStats) {
        // handle on commit callbacks
        for (auto callbackId : transactionStats.callbackIds) {
            if (callbackId.type != CallbackId::Type::ON_COMMIT) {
                continue;
            }
            auto& [callbackFunction, callbackSurfaceControls] = callbacksMap[callbackId];
            if (!callbackFunction) {
                continue;
            }
            std::vector<SurfaceControlStats> surfaceControlStats;
            for (const auto& surfaceStats : transactionStats.surfaceStats) {
                surfaceControlStats
                        .emplace_back(callbacksMap[callbackId]
                                              .surfaceControls[surfaceStats.surfaceControl],
                                      transactionStats.latchTime, surfaceStats.acquireTimeOrFence,
                                      transactionStats.presentFence,
                                      surfaceStats.previousReleaseFence, surfaceStats.transformHint,
                                      surfaceStats.eventStats,
                                      surfaceStats.currentMaxAcquiredBufferCount);
            }

            callbackFunction(transactionStats.latchTime, transactionStats.presentFence,
                             surfaceControlStats);

            // More than one transaction may contain the same callback id. Erase the callback from
            // the map to ensure that it is only called once. This can happen if transactions are
            // parcelled out of process and applied in both processes.
            callbacksMap.erase(callbackId);
        }

        // handle on complete callbacks
        for (auto callbackId : transactionStats.callbackIds) {
            if (callbackId.type != CallbackId::Type::ON_COMPLETE) {
                continue;
            }
            auto& [callbackFunction, callbackSurfaceControls] = callbacksMap[callbackId];
            if (!callbackFunction) {
                ALOGE("cannot call null callback function, skipping");
                continue;
            }
            std::vector<SurfaceControlStats> surfaceControlStats;
            for (const auto& surfaceStats : transactionStats.surfaceStats) {
                surfaceControlStats
                        .emplace_back(callbacksMap[callbackId]
                                              .surfaceControls[surfaceStats.surfaceControl],
                                      transactionStats.latchTime, surfaceStats.acquireTimeOrFence,
                                      transactionStats.presentFence,
                                      surfaceStats.previousReleaseFence, surfaceStats.transformHint,
                                      surfaceStats.eventStats,
                                      surfaceStats.currentMaxAcquiredBufferCount);
                if (callbacksMap[callbackId].surfaceControls[surfaceStats.surfaceControl] &&
                    surfaceStats.transformHint.has_value()) {
                    callbacksMap[callbackId]
                            .surfaceControls[surfaceStats.surfaceControl]
                            ->setTransformHint(*surfaceStats.transformHint);
                }
                // If there is buffer id set, we look up any pending client release buffer callbacks
                // and call them. This is a performance optimization when we have a transaction
                // callback and a release buffer callback happening at the same time to avoid an
                // additional ipc call from the server.
                if (surfaceStats.previousReleaseCallbackId != ReleaseCallbackId::INVALID_ID) {
                    ReleaseBufferCallback callback;
                    {
                        std::scoped_lock<std::mutex> lock(mMutex);
                        callback = popReleaseBufferCallbackLocked(
                                surfaceStats.previousReleaseCallbackId);
                    }
                    if (callback) {
                        callback(surfaceStats.previousReleaseCallbackId,
                                 surfaceStats.previousReleaseFence
                                         ? surfaceStats.previousReleaseFence
                                         : Fence::NO_FENCE,
                                 surfaceStats.currentMaxAcquiredBufferCount);
                    }
                }
            }

            callbackFunction(transactionStats.latchTime, transactionStats.presentFence,
                             surfaceControlStats);
        }
    }

    for (const auto& transactionStats : listenerStats.transactionStats) {
        for (const auto& surfaceStats : transactionStats.surfaceStats) {
            // The callbackMap contains the SurfaceControl object, which we need to look up the
            // layerId. Since we don't know which callback contains the SurfaceControl, iterate
            // through all until the SC is found.
            int32_t layerId = -1;
            for (auto callbackId : transactionStats.callbackIds) {
                if (callbackId.type != CallbackId::Type::ON_COMPLETE) {
                    // We only want to run the stats callback for ON_COMPLETE
                    continue;
                }
                sp<SurfaceControl> sc =
                        callbacksMap[callbackId].surfaceControls[surfaceStats.surfaceControl];
                if (sc != nullptr) {
                    layerId = sc->getLayerId();
                    break;
                }
            }

            if (layerId != -1) {
                // Acquire surface stats listener lock such that we guarantee that after calling
                // unregister, there won't be any further callback.
                std::scoped_lock<std::recursive_mutex> lock(mSurfaceStatsListenerMutex);
                auto listenerRange = mSurfaceStatsListeners.equal_range(layerId);
                for (auto it = listenerRange.first; it != listenerRange.second; it++) {
                    auto entry = it->second;
                    entry.callback(entry.context, transactionStats.latchTime,
                        transactionStats.presentFence, surfaceStats);
                }
            }
        }
    }
}

void TransactionCompletedListener::onTransactionQueueStalled(const String8& reason) {
    std::unordered_map<void*, std::function<void(const std::string&)>> callbackCopy;
    {
        std::scoped_lock<std::mutex> lock(mMutex);
        callbackCopy = mQueueStallListeners;
    }
    for (auto const& it : callbackCopy) {
        it.second(reason.c_str());
    }
}

void TransactionCompletedListener::addQueueStallListener(
        std::function<void(const std::string&)> stallListener, void* id) {
    std::scoped_lock<std::mutex> lock(mMutex);
    mQueueStallListeners[id] = stallListener;
}

void TransactionCompletedListener::removeQueueStallListener(void* id) {
    std::scoped_lock<std::mutex> lock(mMutex);
    mQueueStallListeners.erase(id);
}

void TransactionCompletedListener::onReleaseBuffer(ReleaseCallbackId callbackId,
                                                   sp<Fence> releaseFence,
                                                   uint32_t currentMaxAcquiredBufferCount) {
    ReleaseBufferCallback callback;
    {
        std::scoped_lock<std::mutex> lock(mMutex);
        callback = popReleaseBufferCallbackLocked(callbackId);
    }
    if (!callback) {
        ALOGE("Could not call release buffer callback, buffer not found %s",
              callbackId.to_string().c_str());
        return;
    }
    std::optional<uint32_t> optionalMaxAcquiredBufferCount =
            currentMaxAcquiredBufferCount == UINT_MAX
            ? std::nullopt
            : std::make_optional<uint32_t>(currentMaxAcquiredBufferCount);
    callback(callbackId, releaseFence, optionalMaxAcquiredBufferCount);
}

ReleaseBufferCallback TransactionCompletedListener::popReleaseBufferCallbackLocked(
        const ReleaseCallbackId& callbackId) {
    ReleaseBufferCallback callback;
    auto itr = mReleaseBufferCallbacks.find(callbackId);
    if (itr == mReleaseBufferCallbacks.end()) {
        return nullptr;
    }
    callback = itr->second;
    mReleaseBufferCallbacks.erase(itr);
    return callback;
}

void TransactionCompletedListener::removeReleaseBufferCallback(
        const ReleaseCallbackId& callbackId) {
    {
        std::scoped_lock<std::mutex> lock(mMutex);
        popReleaseBufferCallbackLocked(callbackId);
    }
}

SurfaceComposerClient::PresentationCallbackRAII::PresentationCallbackRAII(
        TransactionCompletedListener* tcl, int id) {
    mTcl = tcl;
    mId = id;
}

SurfaceComposerClient::PresentationCallbackRAII::~PresentationCallbackRAII() {
    mTcl->clearTrustedPresentationCallback(mId);
}

sp<SurfaceComposerClient::PresentationCallbackRAII>
TransactionCompletedListener::addTrustedPresentationCallback(TrustedPresentationCallback tpc,
                                                             int id, void* context) {
    std::scoped_lock<std::mutex> lock(mMutex);
    mTrustedPresentationCallbacks[id] =
            std::tuple<TrustedPresentationCallback, void*>(tpc, context);
    return sp<SurfaceComposerClient::PresentationCallbackRAII>::make(this, id);
}

void TransactionCompletedListener::clearTrustedPresentationCallback(int id) {
    std::scoped_lock<std::mutex> lock(mMutex);
    mTrustedPresentationCallbacks.erase(id);
}

void TransactionCompletedListener::onTrustedPresentationChanged(int id,
                                                                bool presentedWithinThresholds) {
    TrustedPresentationCallback tpc;
    void* context;
    {
        std::scoped_lock<std::mutex> lock(mMutex);
        auto it = mTrustedPresentationCallbacks.find(id);
        if (it == mTrustedPresentationCallbacks.end()) {
            return;
        }
        std::tie(tpc, context) = it->second;
    }
    tpc(context, presentedWithinThresholds);
}

// ---------------------------------------------------------------------------

void removeDeadBufferCallback(void* /*context*/, uint64_t graphicBufferId);

/**
 * We use the BufferCache to reduce the overhead of exchanging GraphicBuffers with
 * the server. If we were to simply parcel the GraphicBuffer we would pay two overheads
 *     1. Cost of sending the FD
 *     2. Cost of importing the GraphicBuffer with the mapper in the receiving process.
 * To ease this cost we implement the following scheme of caching buffers to integers,
 * or said-otherwise, naming them with integers. This is the scheme known as slots in
 * the legacy BufferQueue system.
 *     1. When sending Buffers to SurfaceFlinger we look up the Buffer in the cache.
 *     2. If there is a cache-hit we remove the Buffer from the Transaction and instead
 *        send the cached integer.
 *     3. If there is a cache miss, we cache the new buffer and send the integer
 *        along with the Buffer, SurfaceFlinger on it's side creates a new cache
 *        entry, and we use the integer for further communication.
 * A few details about lifetime:
 *     1. The cache evicts by LRU. The server side cache is keyed by BufferCache::getToken
 *        which is per process Unique. The server side cache is larger than the client side
 *        cache so that the server will never evict entries before the client.
 *     2. When the client evicts an entry it notifies the server via an uncacheBuffer
 *        transaction.
 *     3. The client only references the Buffers by ID, and uses buffer->addDeathCallback
 *        to auto-evict destroyed buffers.
 */
class BufferCache : public Singleton<BufferCache> {
public:
    BufferCache() : token(sp<BBinder>::make()) {}

    sp<IBinder> getToken() {
        return IInterface::asBinder(TransactionCompletedListener::getIInstance());
    }

    status_t getCacheId(const sp<GraphicBuffer>& buffer, uint64_t* cacheId) {
        std::lock_guard<std::mutex> lock(mMutex);

        auto itr = mBuffers.find(buffer->getId());
        if (itr == mBuffers.end()) {
            return BAD_VALUE;
        }
        itr->second = getCounter();
        *cacheId = buffer->getId();
        return NO_ERROR;
    }

    uint64_t cache(const sp<GraphicBuffer>& buffer,
                   std::optional<client_cache_t>& outUncacheBuffer) {
        std::lock_guard<std::mutex> lock(mMutex);

        if (mBuffers.size() >= BUFFER_CACHE_MAX_SIZE) {
            outUncacheBuffer = findLeastRecentlyUsedBuffer();
            mBuffers.erase(outUncacheBuffer->id);
        }

        buffer->addDeathCallback(removeDeadBufferCallback, nullptr);

        mBuffers[buffer->getId()] = getCounter();
        return buffer->getId();
    }

    void uncache(uint64_t cacheId) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mBuffers.erase(cacheId)) {
            SurfaceComposerClient::doUncacheBufferTransaction(cacheId);
        }
    }

private:
    client_cache_t findLeastRecentlyUsedBuffer() REQUIRES(mMutex) {
        auto itr = mBuffers.begin();
        uint64_t minCounter = itr->second;
        auto minBuffer = itr;
        itr++;

        while (itr != mBuffers.end()) {
            uint64_t counter = itr->second;
            if (counter < minCounter) {
                minCounter = counter;
                minBuffer = itr;
            }
            itr++;
        }

        return {.token = getToken(), .id = minBuffer->first};
    }

    uint64_t getCounter() REQUIRES(mMutex) {
        static uint64_t counter = 0;
        return counter++;
    }

    std::mutex mMutex;
    std::map<uint64_t /*Cache id*/, uint64_t /*counter*/> mBuffers GUARDED_BY(mMutex);

    // Used by ISurfaceComposer to identify which process is sending the cached buffer.
    sp<IBinder> token;
};

ANDROID_SINGLETON_STATIC_INSTANCE(BufferCache);

void removeDeadBufferCallback(void* /*context*/, uint64_t graphicBufferId) {
    // GraphicBuffer id's are used as the cache ids.
    BufferCache::getInstance().uncache(graphicBufferId);
}

// ---------------------------------------------------------------------------

SurfaceComposerClient::Transaction::Transaction() {
    mState.mId = generateId();
    mTransactionCompletedListener = TransactionCompletedListener::getInstance();
}

SurfaceComposerClient::Transaction::Transaction(Transaction&& other)
      : mTransactionCompletedListener(TransactionCompletedListener::getInstance()),
        mState(std::move(other.mState)),
        mListenerCallbacks(std::move(other.mListenerCallbacks)) {}

void SurfaceComposerClient::Transaction::sanitize(int pid, int uid) {
    uint32_t permissions = LayerStatePermissions::getTransactionPermissions(pid, uid);
    for (auto& composerState : mState.mComposerStates) {
        composerState.state.sanitize(permissions);
    }
    if (!mState.mInputWindowCommands.empty() &&
        (permissions & layer_state_t::Permission::ACCESS_SURFACE_FLINGER) == 0) {
        ALOGE("Only privileged callers are allowed to send input commands.");
        mState.mInputWindowCommands.clear();
    }
}

std::unique_ptr<SurfaceComposerClient::Transaction>
SurfaceComposerClient::Transaction::createFromParcel(const Parcel* parcel) {
    auto transaction = std::make_unique<Transaction>();
    if (transaction->readFromParcel(parcel) == NO_ERROR) {
        return transaction;
    }
    return nullptr;
}


status_t SurfaceComposerClient::Transaction::readFromParcel(const Parcel* parcel) {
    TransactionState tmpState;
    SAFE_PARCEL(tmpState.readFromParcel, parcel);

    size_t count = static_cast<size_t>(parcel->readUint32());
    if (count > parcel->dataSize()) {
        return BAD_VALUE;
    }
    std::unordered_map<sp<ITransactionCompletedListener>, CallbackInfo, TCLHash> listenerCallbacks;
    listenerCallbacks.reserve(count);
    for (size_t i = 0; i < count; i++) {
        sp<ITransactionCompletedListener> listener =
                interface_cast<ITransactionCompletedListener>(parcel->readStrongBinder());
        size_t numCallbackIds = parcel->readUint32();
        if (numCallbackIds > parcel->dataSize()) {
            return BAD_VALUE;
        }
        for (size_t j = 0; j < numCallbackIds; j++) {
            CallbackId id;
            parcel->readParcelable(&id);
            listenerCallbacks[listener].callbackIds.insert(id);
        }
        size_t numSurfaces = parcel->readUint32();
        if (numSurfaces > parcel->dataSize()) {
            return BAD_VALUE;
        }
        for (size_t j = 0; j < numSurfaces; j++) {
            sp<SurfaceControl> surface;
            SAFE_PARCEL(SurfaceControl::readFromParcel, *parcel, &surface);
            listenerCallbacks[listener].surfaceControls.insert(surface);
        }
    }

    mState = std::move(tmpState);
    mListenerCallbacks = std::move(listenerCallbacks);
    return NO_ERROR;
}

status_t SurfaceComposerClient::Transaction::writeToParcel(Parcel* parcel) const {
    // If we write the Transaction to a parcel, we want to ensure the Buffers are cached
    // before crossing the IPC boundary. Otherwise the receiving party will cache the buffers
    // but is unlikely to use them again as they are owned by the other process.
    // You may be asking yourself, is this const cast safe? Const cast is safe up
    // until the point where you try and write to an object that was originally const at which
    // point we enter undefined behavior. In this case we are safe though, because there are
    // two possibilities:
    //    1. The SurfaceComposerClient::Transaction was originally non-const. Safe.
    //    2. It was originall const! In this case not only was it useless, but it by definition
    //       contains no composer states and so cacheBuffers will not perform any writes.

    const_cast<SurfaceComposerClient::Transaction*>(this)->cacheBuffers();

    SAFE_PARCEL(mState.writeToParcel, parcel);

    parcel->writeUint32(static_cast<uint32_t>(mListenerCallbacks.size()));
    for (auto const& [listener, callbackInfo] : mListenerCallbacks) {
        parcel->writeStrongBinder(ITransactionCompletedListener::asBinder(listener));
        parcel->writeUint32(static_cast<uint32_t>(callbackInfo.callbackIds.size()));
        for (auto callbackId : callbackInfo.callbackIds) {
            parcel->writeParcelable(callbackId);
        }
        parcel->writeUint32(static_cast<uint32_t>(callbackInfo.surfaceControls.size()));
        for (auto surfaceControl : callbackInfo.surfaceControls) {
            SAFE_PARCEL(surfaceControl->writeToParcel, *parcel);
        }
    }

    return NO_ERROR;
}

void SurfaceComposerClient::Transaction::releaseBufferIfOverwriting(const layer_state_t& state) {
    if (!(state.what & layer_state_t::eBufferChanged) || !state.bufferData->hasBuffer()) {
        return;
    }

    auto listener = state.bufferData->releaseBufferListener;
    sp<Fence> fence =
            state.bufferData->acquireFence ? state.bufferData->acquireFence : Fence::NO_FENCE;
    if (state.bufferData->releaseBufferEndpoint ==
        IInterface::asBinder(TransactionCompletedListener::getIInstance())) {
        // if the callback is in process, run on a different thread to avoid any lock contigency
        // issues in the client.
        SurfaceComposerClient::getDefault()
                ->mReleaseCallbackThread
                .addReleaseCallback(state.bufferData->generateReleaseCallbackId(), fence);
    } else {
        listener->onReleaseBuffer(state.bufferData->generateReleaseCallbackId(), fence, UINT_MAX);
    }
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::merge(Transaction&& other) {
    mState.merge(std::move(other.mState),
                 std::bind(&Transaction::releaseBufferIfOverwriting, this, std::placeholders::_1));
    for (const auto& [listener, callbackInfo] : other.mListenerCallbacks) {
        auto& [callbackIds, surfaceControls] = callbackInfo;
        mListenerCallbacks[listener].callbackIds.insert(std::make_move_iterator(
                                                                callbackIds.begin()),
                                                        std::make_move_iterator(callbackIds.end()));

        mListenerCallbacks[listener].surfaceControls.insert(surfaceControls.begin(),
                                                            surfaceControls.end());

        auto& currentProcessCallbackInfo =
                mListenerCallbacks[TransactionCompletedListener::getIInstance()];
        currentProcessCallbackInfo.surfaceControls
                .insert(std::make_move_iterator(surfaceControls.begin()),
                        std::make_move_iterator(surfaceControls.end()));

        // register all surface controls for all callbackIds for this listener that is merging
        for (const auto& surfaceControl : currentProcessCallbackInfo.surfaceControls) {
            mTransactionCompletedListener
                    ->addSurfaceControlToCallbacks(surfaceControl,
                                                   currentProcessCallbackInfo.callbackIds);
        }
    }

    other.clear();
    return *this;
}

void SurfaceComposerClient::Transaction::clear() {
    mState.clear();
    mListenerCallbacks.clear();
}

uint64_t SurfaceComposerClient::Transaction::getId() const {
    return mState.mId;
}

std::vector<uint64_t> SurfaceComposerClient::Transaction::getMergedTransactionIds() {
    return mState.mMergedTransactionIds;
}

void SurfaceComposerClient::doUncacheBufferTransaction(uint64_t cacheId) {
    sp<ISurfaceComposer> sf(ComposerService::getComposerService());

    client_cache_t uncacheBuffer;
    uncacheBuffer.token = BufferCache::getInstance().getToken();
    uncacheBuffer.id = cacheId;
    TransactionState state;
    state.mId = generateId();
    state.mApplyToken = Transaction::getDefaultApplyToken();
    state.mUncacheBuffers.emplace_back(std::move(uncacheBuffer));
    state.mFlags = ISurfaceComposer::eOneWay;
    state.mDesiredPresentTime = systemTime();
    status_t status = sf->setTransactionState(std::move(state));
    if (status != NO_ERROR) {
        ALOGE_AND_TRACE("SurfaceComposerClient::doUncacheBufferTransaction - %s",
                        strerror(-status));
    }
}

void SurfaceComposerClient::Transaction::cacheBuffers() {
    if (!mState.mMayContainBuffer) {
        return;
    }

    size_t count = 0;
    for (auto& cs : mState.mComposerStates) {
        layer_state_t* s = &cs.state;
        if (!(s->what & layer_state_t::eBufferChanged)) {
            continue;
        } else if (s->bufferData &&
                   s->bufferData->flags.test(BufferData::BufferDataChange::cachedBufferChanged)) {
            // If eBufferChanged and eCachedBufferChanged are both trued then that means
            // we already cached the buffer in a previous call to cacheBuffers, perhaps
            // from writeToParcel on a Transaction that was merged in to this one.
            continue;
        }

        // Don't try to cache a null buffer. Sending null buffers is cheap so we shouldn't waste
        // time trying to cache them.
        if (!s->bufferData || !s->bufferData->buffer) {
            continue;
        }

        uint64_t cacheId = 0;
        status_t ret = BufferCache::getInstance().getCacheId(s->bufferData->buffer, &cacheId);
        if (ret == NO_ERROR) {
            // Cache-hit. Strip the buffer and send only the id.
            s->bufferData->buffer = nullptr;
        } else {
            // Cache-miss. Include the buffer and send the new cacheId.
            std::optional<client_cache_t> uncacheBuffer;
            cacheId = BufferCache::getInstance().cache(s->bufferData->buffer, uncacheBuffer);
            if (uncacheBuffer) {
                mState.mUncacheBuffers.emplace_back(*uncacheBuffer);
            }
        }
        s->bufferData->flags |= BufferData::BufferDataChange::cachedBufferChanged;
        s->bufferData->cachedBuffer.token = BufferCache::getInstance().getToken();
        s->bufferData->cachedBuffer.id = cacheId;

        // If we have more buffers than the size of the cache, we should stop caching so we don't
        // evict other buffers in this transaction
        count++;
        if (count >= BUFFER_CACHE_MAX_SIZE) {
            break;
        }
    }
}

class SyncCallback {
public:
    static auto getCallback(std::shared_ptr<SyncCallback>& callbackContext) {
        return [callbackContext](void* /* unused context */, nsecs_t /* latchTime */,
                                 const sp<Fence>& /* presentFence */,
                                 const std::vector<SurfaceControlStats>& /* stats */) {
            if (!callbackContext) {
                ALOGE("failed to get callback context for SyncCallback");
                return;
            }
            LOG_ALWAYS_FATAL_IF(sem_post(&callbackContext->mSemaphore), "sem_post failed");
        };
    }
    ~SyncCallback() {
        if (mInitialized) {
            LOG_ALWAYS_FATAL_IF(sem_destroy(&mSemaphore), "sem_destroy failed");
        }
    }
    void init() {
        LOG_ALWAYS_FATAL_IF(clock_gettime(CLOCK_MONOTONIC, &mTimeoutTimespec) == -1,
                            "clock_gettime() fail! in SyncCallback::init");
        mTimeoutTimespec.tv_sec += 4;
        LOG_ALWAYS_FATAL_IF(sem_init(&mSemaphore, 0, 0), "sem_init failed");
        mInitialized = true;
    }
    void wait() {
        int result = sem_clockwait(&mSemaphore, CLOCK_MONOTONIC, &mTimeoutTimespec);
        if (result && errno != ETIMEDOUT && errno != EINTR) {
            LOG_ALWAYS_FATAL("sem_clockwait failed(%d)", errno);
        } else if (errno == ETIMEDOUT) {
            ALOGW("Sync transaction timed out waiting for commit callback.");
        }
    }
    void* getContext() { return static_cast<void*>(this); }

private:
    sem_t mSemaphore;
    bool mInitialized = false;
    timespec mTimeoutTimespec;
};

status_t SurfaceComposerClient::Transaction::apply(bool synchronous, bool oneWay) {
    if (mStatus != NO_ERROR) {
        return mStatus;
    }

    std::shared_ptr<SyncCallback> syncCallback = std::make_shared<SyncCallback>();
    if (synchronous) {
        syncCallback->init();
        addTransactionCommittedCallback(SyncCallback::getCallback(syncCallback),
                                        /*callbackContext=*/nullptr);
    }

    mState.mHasListenerCallbacks = !mListenerCallbacks.empty();
    // For every listener with registered callbacks
    for (const auto& [listener, callbackInfo] : mListenerCallbacks) {
        auto& [callbackIds, surfaceControls] = callbackInfo;
        if (callbackIds.empty()) {
            continue;
        }

        if (surfaceControls.empty()) {
            mState.mListenerCallbacks.emplace_back(IInterface::asBinder(listener),
                                                   std::move(callbackIds));
        } else {
            // If the listener has any SurfaceControls set on this Transaction update the surface
            // state
            for (const auto& surfaceControl : surfaceControls) {
                layer_state_t* s = getLayerState(surfaceControl);
                if (!s) {
                    ALOGE("failed to get layer state");
                    continue;
                }
                std::vector<CallbackId> callbacks(callbackIds.begin(), callbackIds.end());
                s->what |= layer_state_t::eHasListenerCallbacksChanged;
                s->listeners.emplace_back(IInterface::asBinder(listener), std::move(callbacks));
            }
        }
    }

    cacheBuffers();

    if (oneWay) {
        if (synchronous) {
            ALOGE("Transaction attempted to set synchronous and one way at the same time"
                  " this is an invalid request. Synchronous will win for safety");
        } else {
            mState.mFlags |= ISurfaceComposer::eOneWay;
        }
    }

    // If both ISurfaceComposer::eEarlyWakeupStart and ISurfaceComposer::eEarlyWakeupEnd are set
    // it is equivalent for none
    uint32_t wakeupFlags = ISurfaceComposer::eEarlyWakeupStart | ISurfaceComposer::eEarlyWakeupEnd;
    if ((mState.mFlags & wakeupFlags) == wakeupFlags) {
        mState.mFlags &= ~(wakeupFlags);
    }
    if (!mState.mApplyToken) mState.mApplyToken = getDefaultApplyToken();

    sp<ISurfaceComposer> sf(ComposerService::getComposerService());
    status_t binderStatus = sf->setTransactionState(std::move(mState));
    mState.mId = generateId();

    // Clear the current states and flags
    clear();

    if (synchronous && binderStatus == OK) {
        syncCallback->wait();
    }

    if (mState.mLogCallPoints) {
        ALOG(LOG_DEBUG, LOG_SURFACE_CONTROL_REGISTRY, "Transaction %" PRIu64 " applied", getId());
    }

    mStatus = NO_ERROR;
    return binderStatus;
}

sp<IBinder> SurfaceComposerClient::Transaction::sApplyToken = sp<BBinder>::make();

std::mutex SurfaceComposerClient::Transaction::sApplyTokenMutex;

sp<IBinder> SurfaceComposerClient::Transaction::getDefaultApplyToken() {
    std::scoped_lock lock{sApplyTokenMutex};
    return sApplyToken;
}

void SurfaceComposerClient::Transaction::setDefaultApplyToken(sp<IBinder> applyToken) {
    std::scoped_lock lock{sApplyTokenMutex};
    sApplyToken = std::move(applyToken);
}

status_t SurfaceComposerClient::Transaction::sendSurfaceFlushJankDataTransaction(
        const sp<SurfaceControl>& sc) {
    Transaction t;
    layer_state_t* s = t.getLayerState(sc);
    if (!s) {
        return BAD_INDEX;
    }

    s->what |= layer_state_t::eFlushJankData;
    t.registerSurfaceControlForCallback(sc);
    return t.apply(/*sync=*/false, /* oneWay=*/true);
}

void SurfaceComposerClient::Transaction::enableDebugLogCallPoints() {
    mState.mLogCallPoints = true;
}

// ---------------------------------------------------------------------------

sp<IBinder> SurfaceComposerClient::createVirtualDisplay(const std::string& displayName,
                                                        bool isSecure, bool optimizeForPower,
                                                        const std::string& uniqueId,
                                                        float requestedRefreshRate) {
    const gui::ISurfaceComposer::OptimizationPolicy optimizationPolicy = optimizeForPower
            ? gui::ISurfaceComposer::OptimizationPolicy::optimizeForPower
            : gui::ISurfaceComposer::OptimizationPolicy::optimizeForPerformance;
    sp<IBinder> display = nullptr;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->createVirtualDisplay(displayName, isSecure,
                                                                            optimizationPolicy,
                                                                            uniqueId,
                                                                            requestedRefreshRate,
                                                                            &display);
    return status.isOk() ? display : nullptr;
}

status_t SurfaceComposerClient::destroyVirtualDisplay(const sp<IBinder>& displayToken) {
    return ComposerServiceAIDL::getComposerService()
            ->destroyVirtualDisplay(displayToken)
            .transactionError();
}

sp<IBinder> SurfaceComposerClient::createDisplay(const String8& displayName, bool isSecure,
                                                 float requestedRefereshRate) {
    return SurfaceComposerClient::createVirtualDisplay(std::string(displayName.c_str()), isSecure, true, kEmpty, requestedRefereshRate);
}

void SurfaceComposerClient::destroyDisplay(const sp<IBinder>& displayToken) {
    SurfaceComposerClient::destroyVirtualDisplay(displayToken);
}

std::vector<PhysicalDisplayId> SurfaceComposerClient::getPhysicalDisplayIds() {
    std::vector<int64_t> displayIds;
    std::vector<PhysicalDisplayId> physicalDisplayIds;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getPhysicalDisplayIds(&displayIds);
    if (status.isOk()) {
        physicalDisplayIds.reserve(displayIds.size());
        for (auto id : displayIds) {
            physicalDisplayIds.push_back(PhysicalDisplayId::fromValue(static_cast<uint64_t>(id)));
        }
    }
    return physicalDisplayIds;
}

sp<IBinder> SurfaceComposerClient::getPhysicalDisplayToken(PhysicalDisplayId displayId) {
    sp<IBinder> display = nullptr;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getPhysicalDisplayToken(displayId.value,
                                                                               &display);
    return status.isOk() ? display : nullptr;
}

std::optional<gui::StalledTransactionInfo> SurfaceComposerClient::getStalledTransactionInfo(
        pid_t pid) {
    std::optional<gui::StalledTransactionInfo> result;
    ComposerServiceAIDL::getComposerService()->getStalledTransactionInfo(pid, &result);
    return result;
}

void SurfaceComposerClient::Transaction::setAnimationTransaction() {
    mState.mFlags |= ISurfaceComposer::eAnimation;
}

void SurfaceComposerClient::Transaction::setEarlyWakeupStart() {
    mState.mFlags |= ISurfaceComposer::eEarlyWakeupStart;
}

void SurfaceComposerClient::Transaction::setEarlyWakeupEnd() {
    mState.mFlags |= ISurfaceComposer::eEarlyWakeupEnd;
}

layer_state_t* SurfaceComposerClient::Transaction::getLayerState(const sp<SurfaceControl>& sc) {
    return mState.getLayerState(sc);
}

void SurfaceComposerClient::Transaction::registerSurfaceControlForCallback(
        const sp<SurfaceControl>& sc) {
    auto& callbackInfo = mListenerCallbacks[TransactionCompletedListener::getIInstance()];
    callbackInfo.surfaceControls.insert(sc);

    mTransactionCompletedListener->addSurfaceControlToCallbacks(sc, callbackInfo.callbackIds);
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setPosition(
        const sp<SurfaceControl>& sc, float x, float y) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::ePositionChanged;
    s->x = x;
    s->y = y;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::show(
        const sp<SurfaceControl>& sc) {
    return setFlags(sc, 0, layer_state_t::eLayerHidden);
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::hide(
        const sp<SurfaceControl>& sc) {
    return setFlags(sc, layer_state_t::eLayerHidden, layer_state_t::eLayerHidden);
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setLayer(
        const sp<SurfaceControl>& sc, int32_t z) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eLayerChanged;
    s->what &= ~layer_state_t::eRelativeLayerChanged;
    s->z = z;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setRelativeLayer(
        const sp<SurfaceControl>& sc, const sp<SurfaceControl>& relativeTo, int32_t z) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->updateRelativeLayer(relativeTo, z);
    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setFlags(
        const sp<SurfaceControl>& sc, uint32_t flags,
        uint32_t mask) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eFlagsChanged;
    s->flags &= ~mask;
    s->flags |= (flags & mask);
    s->mask |= mask;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setTransparentRegionHint(
        const sp<SurfaceControl>& sc,
        const Region& transparentRegion) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->updateTransparentRegion(transparentRegion);
    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setDimmingEnabled(
        const sp<SurfaceControl>& sc, bool dimmingEnabled) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eDimmingEnabledChanged;
    s->dimmingEnabled = dimmingEnabled;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setAlpha(
        const sp<SurfaceControl>& sc, float alpha) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    if (alpha < 0.0f || alpha > 1.0f) {
        ALOGE("SurfaceComposerClient::Transaction::setAlpha: invalid alpha %f, clamping", alpha);
    }
    s->what |= layer_state_t::eAlphaChanged;
    s->color.a = std::clamp(alpha, 0.f, 1.f);

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setLayerStack(
        const sp<SurfaceControl>& sc, ui::LayerStack layerStack) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eLayerStackChanged;
    s->layerStack = layerStack;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setMetadata(
        const sp<SurfaceControl>& sc, uint32_t key, const Parcel& p) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eMetadataChanged;

    s->metadata.mMap[key] = {p.data(), p.data() + p.dataSize()};

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setMatrix(
        const sp<SurfaceControl>& sc, float dsdx, float dtdx,
        float dtdy, float dsdy) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eMatrixChanged;
    layer_state_t::matrix22_t matrix;
    matrix.dsdx = dsdx;
    matrix.dtdx = dtdx;
    matrix.dsdy = dsdy;
    matrix.dtdy = dtdy;
    s->matrix = matrix;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setCrop(
        const sp<SurfaceControl>& sc, const Rect& crop) {
    return setCrop(sc, crop.toFloatRect());
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setCrop(
        const sp<SurfaceControl>& sc, const FloatRect& crop) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eCropChanged;
    s->crop = crop;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setCornerRadius(
        const sp<SurfaceControl>& sc, float cornerRadius) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eCornerRadiusChanged;
    s->cornerRadius = cornerRadius;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setClientDrawnCornerRadius(
        const sp<SurfaceControl>& sc, float clientDrawnCornerRadius) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eClientDrawnCornerRadiusChanged;
    s->clientDrawnCornerRadius = clientDrawnCornerRadius;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setBackgroundBlurRadius(
        const sp<SurfaceControl>& sc, int backgroundBlurRadius) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eBackgroundBlurRadiusChanged;
    s->backgroundBlurRadius = backgroundBlurRadius;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setBlurRegions(
        const sp<SurfaceControl>& sc, const std::vector<BlurRegion>& blurRegions) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eBlurRegionsChanged;
    s->blurRegions = blurRegions;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::reparent(
        const sp<SurfaceControl>& sc, const sp<SurfaceControl>& newParent) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    if (SurfaceControl::isSameSurface(sc, newParent)) {
        return *this;
    }
    s->updateParentLayer(newParent);
    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setColor(
        const sp<SurfaceControl>& sc,
        const half3& color) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eColorChanged;
    s->color.rgb = color;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setBackgroundColor(
        const sp<SurfaceControl>& sc, const half3& color, float alpha, ui::Dataspace dataspace) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eBackgroundColorChanged;
    s->bgColor.rgb = color;
    s->bgColor.a = alpha;
    s->bgColorDataspace = dataspace;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setTransform(
        const sp<SurfaceControl>& sc, uint32_t transform) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eBufferTransformChanged;
    s->bufferTransform = transform;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction&
SurfaceComposerClient::Transaction::setTransformToDisplayInverse(const sp<SurfaceControl>& sc,
                                                                 bool transformToDisplayInverse) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eTransformToDisplayInverseChanged;
    s->transformToDisplayInverse = transformToDisplayInverse;

    registerSurfaceControlForCallback(sc);
    return *this;
}

std::shared_ptr<BufferData> SurfaceComposerClient::Transaction::getAndClearBuffer(
        const sp<SurfaceControl>& sc) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        return nullptr;
    }
    if (!(s->what & layer_state_t::eBufferChanged)) {
        return nullptr;
    }

    std::shared_ptr<BufferData> bufferData = std::move(s->bufferData);

    mTransactionCompletedListener->removeReleaseBufferCallback(
            bufferData->generateReleaseCallbackId());
    s->what &= ~layer_state_t::eBufferChanged;
    s->bufferData = nullptr;

    return bufferData;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setBufferHasBarrier(
        const sp<SurfaceControl>& sc, uint64_t barrierFrameNumber) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->bufferData->hasBarrier = true;
    s->bufferData->barrierFrameNumber = barrierFrameNumber;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setBuffer(
        const sp<SurfaceControl>& sc, const sp<GraphicBuffer>& buffer,
        const std::optional<sp<Fence>>& fence, const std::optional<uint64_t>& optFrameNumber,
        uint32_t producerId, ReleaseBufferCallback callback, nsecs_t dequeueTime) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    releaseBufferIfOverwriting(*s);

    std::shared_ptr<BufferData> bufferData = std::make_shared<BufferData>();
    bufferData->buffer = buffer;
    if (buffer) {
        uint64_t frameNumber = sc->resolveFrameNumber(optFrameNumber);
        bufferData->frameNumber = frameNumber;
        bufferData->producerId = producerId;
        bufferData->flags |= BufferData::BufferDataChange::frameNumberChanged;
        bufferData->dequeueTime = dequeueTime;
        if (fence) {
            bufferData->acquireFence = *fence;
            bufferData->flags |= BufferData::BufferDataChange::fenceChanged;
        }
        bufferData->releaseBufferEndpoint = IInterface::asBinder(mTransactionCompletedListener);
        setReleaseBufferCallback(bufferData.get(), callback);
    }

    if (mState.mIsAutoTimestamp) {
        mState.mDesiredPresentTime = systemTime();
    }
    s->what |= layer_state_t::eBufferChanged;
    s->bufferData = std::move(bufferData);
    registerSurfaceControlForCallback(sc);

    // With the current infrastructure, a release callback will not be invoked if there's no
    // transaction callback in the case when a buffer is latched and not released early. This is
    // because the legacy implementation didn't have a release callback and sent releases in the
    // transaction callback. Because of this, we need to make sure to have a transaction callback
    // set up when a buffer is sent in a transaction to ensure the caller gets the release
    // callback, regardless if they set up a transaction callback.
    //
    // TODO (b/230380821): Remove when release callbacks are separated from transaction callbacks
    addTransactionCompletedCallback([](void*, nsecs_t, const sp<Fence>&,
                                       const std::vector<SurfaceControlStats>&) {},
                                    nullptr);

    mState.mMayContainBuffer = true;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::unsetBuffer(
        const sp<SurfaceControl>& sc) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    if (!(s->what & layer_state_t::eBufferChanged)) {
        return *this;
    }

    releaseBufferIfOverwriting(*s);

    s->what &= ~layer_state_t::eBufferChanged;
    s->bufferData = nullptr;
    return *this;
}

void SurfaceComposerClient::Transaction::setReleaseBufferCallback(BufferData* bufferData,
                                                                  ReleaseBufferCallback callback) {
    if (!callback) {
        return;
    }

    if (!bufferData->buffer) {
        ALOGW("Transaction::setReleaseBufferCallback"
              "ignored trying to set a callback on a null buffer.");
        return;
    }

    bufferData->releaseBufferListener =
            static_cast<sp<ITransactionCompletedListener>>(mTransactionCompletedListener);
    mTransactionCompletedListener->setReleaseBufferCallback(bufferData->generateReleaseCallbackId(),
                                                            callback);
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setDataspace(
        const sp<SurfaceControl>& sc, ui::Dataspace dataspace) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eDataspaceChanged;
    s->dataspace = dataspace;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setExtendedRangeBrightness(
        const sp<SurfaceControl>& sc, float currentBufferRatio, float desiredRatio) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eExtendedRangeBrightnessChanged;
    s->currentHdrSdrRatio = currentBufferRatio;
    s->desiredHdrSdrRatio = desiredRatio;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setDesiredHdrHeadroom(
        const sp<SurfaceControl>& sc, float desiredRatio) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eDesiredHdrHeadroomChanged;
    s->desiredHdrSdrRatio = desiredRatio;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setLuts(
        const sp<SurfaceControl>& sc, base::unique_fd&& lutFd, const std::vector<int32_t>& offsets,
        const std::vector<int32_t>& dimensions, const std::vector<int32_t>& sizes,
        const std::vector<int32_t>& samplingKeys) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eLutsChanged;
    if (lutFd.ok()) {
        s->luts = std::make_shared<gui::DisplayLuts>(std::move(lutFd), offsets, dimensions, sizes,
                                                     samplingKeys);
    } else {
        s->luts = nullptr;
    }

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setCachingHint(
        const sp<SurfaceControl>& sc, gui::CachingHint cachingHint) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eCachingHintChanged;
    s->cachingHint = cachingHint;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setHdrMetadata(
        const sp<SurfaceControl>& sc, const HdrMetadata& hdrMetadata) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eHdrMetadataChanged;
    s->hdrMetadata = hdrMetadata;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setSurfaceDamageRegion(
        const sp<SurfaceControl>& sc, const Region& surfaceDamageRegion) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->updateSurfaceDamageRegion(surfaceDamageRegion);
    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setApi(
        const sp<SurfaceControl>& sc, int32_t api) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eApiChanged;
    s->api = api;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setSidebandStream(
        const sp<SurfaceControl>& sc, const sp<NativeHandle>& sidebandStream) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eSidebandStreamChanged;
    s->sidebandStream = sidebandStream;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setDesiredPresentTime(
        nsecs_t desiredPresentTime) {
    mState.mDesiredPresentTime = desiredPresentTime;
    mState.mIsAutoTimestamp = false;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setColorSpaceAgnostic(
        const sp<SurfaceControl>& sc, const bool agnostic) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eColorSpaceAgnosticChanged;
    s->colorSpaceAgnostic = agnostic;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction&
SurfaceComposerClient::Transaction::setFrameRateSelectionPriority(const sp<SurfaceControl>& sc,
                                                                  int32_t priority) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eFrameRateSelectionPriority;
    s->frameRateSelectionPriority = priority;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::addTransactionCallback(
        TransactionCompletedCallbackTakesContext callback, void* callbackContext,
        CallbackId::Type callbackType) {
    auto callbackWithContext =
            std::bind(std::move(callback), callbackContext, std::placeholders::_1,
                      std::placeholders::_2, std::placeholders::_3);
    const auto& surfaceControls = mListenerCallbacks[mTransactionCompletedListener].surfaceControls;

    CallbackId callbackId =
            mTransactionCompletedListener->addCallbackFunction(callbackWithContext, surfaceControls,
                                                               callbackType);

    mListenerCallbacks[mTransactionCompletedListener].callbackIds.emplace(callbackId);
    return *this;
}

SurfaceComposerClient::Transaction&
SurfaceComposerClient::Transaction::addTransactionCompletedCallback(
        TransactionCompletedCallbackTakesContext callback, void* callbackContext) {
    return addTransactionCallback(std::move(callback), callbackContext,
                                  CallbackId::Type::ON_COMPLETE);
}

SurfaceComposerClient::Transaction&
SurfaceComposerClient::Transaction::addTransactionCommittedCallback(
        TransactionCompletedCallbackTakesContext callback, void* callbackContext) {
    return addTransactionCallback(std::move(callback), callbackContext,
                                  CallbackId::Type::ON_COMMIT);
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::notifyProducerDisconnect(
        const sp<SurfaceControl>& sc) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eProducerDisconnect;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setInputWindowInfo(
        const sp<SurfaceControl>& sc, sp<WindowInfoHandle> info) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->updateInputWindowInfo(std::move(info));
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setFocusedWindow(
        const FocusRequest& request) {
    mState.mInputWindowCommands.addFocusRequest(request);
    return *this;
}

SurfaceComposerClient::Transaction&
SurfaceComposerClient::Transaction::addWindowInfosReportedListener(
        sp<gui::IWindowInfosReportedListener> windowInfosReportedListener) {
    mState.mInputWindowCommands.addWindowInfosReportedListener(windowInfosReportedListener);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setColorTransform(
    const sp<SurfaceControl>& sc, const mat3& matrix, const vec3& translation) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eColorTransformChanged;
    s->colorTransform = mat4(matrix, translation);

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setGeometry(
        const sp<SurfaceControl>& sc, const Rect& source, const Rect& dst, int transform) {
    setCrop(sc, source);

    int x = dst.left;
    int y = dst.top;

    float sourceWidth = source.getWidth();
    float sourceHeight = source.getHeight();

    float xScale = sourceWidth < 0 ? 1.0f : dst.getWidth() / sourceWidth;
    float yScale = sourceHeight < 0 ? 1.0f : dst.getHeight() / sourceHeight;
    float matrix[4] = {1, 0, 0, 1};

    switch (transform) {
        case NATIVE_WINDOW_TRANSFORM_FLIP_H:
            matrix[0] = -xScale; matrix[1] = 0;
            matrix[2] = 0; matrix[3] = yScale;
            x += source.getWidth();
            break;
        case NATIVE_WINDOW_TRANSFORM_FLIP_V:
            matrix[0] = xScale; matrix[1] = 0;
            matrix[2] = 0; matrix[3] = -yScale;
            y += source.getHeight();
            break;
        case NATIVE_WINDOW_TRANSFORM_ROT_90:
            matrix[0] = 0; matrix[1] = -yScale;
            matrix[2] = xScale; matrix[3] = 0;
            x += source.getHeight();
            break;
        case NATIVE_WINDOW_TRANSFORM_ROT_180:
            matrix[0] = -xScale; matrix[1] = 0;
            matrix[2] = 0; matrix[3] = -yScale;
            x += source.getWidth();
            y += source.getHeight();
            break;
        case NATIVE_WINDOW_TRANSFORM_ROT_270:
            matrix[0] = 0; matrix[1] = yScale;
            matrix[2] = -xScale; matrix[3] = 0;
            y += source.getWidth();
            break;
        default:
            matrix[0] = xScale; matrix[1] = 0;
            matrix[2] = 0; matrix[3] = yScale;
            break;
    }
    setMatrix(sc, matrix[0], matrix[1], matrix[2], matrix[3]);
    float offsetX = xScale * source.left;
    float offsetY = yScale * source.top;
    setPosition(sc, x - offsetX, y - offsetY);

    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setShadowRadius(
        const sp<SurfaceControl>& sc, float shadowRadius) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eShadowRadiusChanged;
    s->shadowRadius = shadowRadius;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setBorderSettings(
        const sp<SurfaceControl>& sc, gui::BorderSettings settings) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eBorderSettingsChanged;
    s->borderSettings = settings;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setFrameRate(
        const sp<SurfaceControl>& sc, float frameRate, int8_t compatibility,
        int8_t changeFrameRateStrategy) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    // Allow privileged values as well here, those will be ignored by SF if
    // the caller is not privileged
    if (!ValidateFrameRate(frameRate, compatibility, changeFrameRateStrategy,
                           "Transaction::setFrameRate",
                           /*privileged=*/true)) {
        mStatus = BAD_VALUE;
        return *this;
    }
    s->what |= layer_state_t::eFrameRateChanged;
    s->frameRate = frameRate;
    s->frameRateCompatibility = compatibility;
    s->changeFrameRateStrategy = changeFrameRateStrategy;
    return *this;
}

SurfaceComposerClient::Transaction&
SurfaceComposerClient::Transaction::setDefaultFrameRateCompatibility(const sp<SurfaceControl>& sc,
                                                                     int8_t compatibility) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eDefaultFrameRateCompatibilityChanged;
    s->defaultFrameRateCompatibility = compatibility;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setFrameRateCategory(
        const sp<SurfaceControl>& sc, int8_t category, bool smoothSwitchOnly) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eFrameRateCategoryChanged;
    s->frameRateCategory = category;
    s->frameRateCategorySmoothSwitchOnly = smoothSwitchOnly;
    return *this;
}

SurfaceComposerClient::Transaction&
SurfaceComposerClient::Transaction::setFrameRateSelectionStrategy(const sp<SurfaceControl>& sc,
                                                                  int8_t strategy) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eFrameRateSelectionStrategyChanged;
    s->frameRateSelectionStrategy = strategy;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setFixedTransformHint(
        const sp<SurfaceControl>& sc, int32_t fixedTransformHint) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    const ui::Transform::RotationFlags transform = fixedTransformHint == -1
            ? ui::Transform::ROT_INVALID
            : ui::Transform::toRotationFlags(static_cast<ui::Rotation>(fixedTransformHint));
    s->what |= layer_state_t::eFixedTransformHintChanged;
    s->fixedTransformHint = transform;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setFrameTimelineInfo(
        const FrameTimelineInfo& frameTimelineInfo) {
    mState.mergeFrameTimelineInfo(frameTimelineInfo);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setAutoRefresh(
        const sp<SurfaceControl>& sc, bool autoRefresh) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eAutoRefreshChanged;
    s->autoRefresh = autoRefresh;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setTrustedOverlay(
        const sp<SurfaceControl>& sc, bool isTrustedOverlay) {
    return setTrustedOverlay(sc,
                             isTrustedOverlay ? gui::TrustedOverlay::ENABLED
                                              : gui::TrustedOverlay::UNSET);
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setTrustedOverlay(
        const sp<SurfaceControl>& sc, gui::TrustedOverlay trustedOverlay) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eTrustedOverlayChanged;
    s->trustedOverlay = trustedOverlay;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setApplyToken(
        const sp<IBinder>& applyToken) {
    mState.mApplyToken = applyToken;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setStretchEffect(
    const sp<SurfaceControl>& sc, const StretchEffect& stretchEffect) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eStretchChanged;
    s->stretchEffect = stretchEffect;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setEdgeExtensionEffect(
        const sp<SurfaceControl>& sc, const gui::EdgeExtensionParameters& effect) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eEdgeExtensionChanged;
    s->edgeExtensionParameters = effect;
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setBufferCrop(
        const sp<SurfaceControl>& sc, const Rect& bufferCrop) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eBufferCropChanged;
    s->bufferCrop = bufferCrop;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setDestinationFrame(
        const sp<SurfaceControl>& sc, const Rect& destinationFrame) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eDestinationFrameChanged;
    s->destinationFrame = destinationFrame;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setDropInputMode(
        const sp<SurfaceControl>& sc, gui::DropInputMode mode) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eDropInputModeChanged;
    s->dropInputMode = mode;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setBufferReleaseChannel(
        const sp<SurfaceControl>& sc,
        const std::shared_ptr<gui::BufferReleaseChannel::ProducerEndpoint>& channel) {
    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }

    s->what |= layer_state_t::eBufferReleaseChannelChanged;
    s->bufferReleaseChannel = channel;

    registerSurfaceControlForCallback(sc);
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setPictureProfileHandle(
        const sp<SurfaceControl>& sc, const PictureProfileHandle& pictureProfileHandle) {
    if (com_android_graphics_libgui_flags_apply_picture_profiles()) {
        layer_state_t* s = getLayerState(sc);
        if (!s) {
            mStatus = BAD_INDEX;
            return *this;
        }

        s->what |= layer_state_t::ePictureProfileHandleChanged;
        s->pictureProfileHandle = pictureProfileHandle;

        registerSurfaceControlForCallback(sc);
    }
    return *this;
}

SurfaceComposerClient::Transaction& SurfaceComposerClient::Transaction::setContentPriority(
        const sp<SurfaceControl>& sc, int32_t priority) {
    if (com_android_graphics_libgui_flags_apply_picture_profiles()) {
        layer_state_t* s = getLayerState(sc);
        if (!s) {
            mStatus = BAD_INDEX;
            return *this;
        }

        s->what |= layer_state_t::eAppContentPriorityChanged;
        s->appContentPriority = priority;

        registerSurfaceControlForCallback(sc);
    }
    return *this;
}

// ---------------------------------------------------------------------------

DisplayState& SurfaceComposerClient::Transaction::getDisplayState(const sp<IBinder>& token) {
    return mState.getDisplayState(token);
}

status_t SurfaceComposerClient::Transaction::setDisplaySurface(const sp<IBinder>& token,
        const sp<IGraphicBufferProducer>& bufferProducer) {
    if (bufferProducer.get() != nullptr) {
        // Make sure that composition can never be stalled by a virtual display
        // consumer that isn't processing buffers fast enough.
        status_t err = bufferProducer->setAsyncMode(true);
        if (err != NO_ERROR) {
            ALOGE("Composer::setDisplaySurface Failed to enable async mode on the "
                    "BufferQueue. This BufferQueue cannot be used for virtual "
                    "display. (%d)", err);
            return err;
        }
    }
    DisplayState& s(getDisplayState(token));
    s.surface = bufferProducer;
    s.what |= DisplayState::eSurfaceChanged;
    return NO_ERROR;
}

void SurfaceComposerClient::Transaction::setDisplayLayerStack(const sp<IBinder>& token,
                                                              ui::LayerStack layerStack) {
    DisplayState& s(getDisplayState(token));
    s.layerStack = layerStack;
    s.what |= DisplayState::eLayerStackChanged;
}

void SurfaceComposerClient::Transaction::setDisplayFlags(const sp<IBinder>& token, uint32_t flags) {
    DisplayState& s(getDisplayState(token));
    s.flags = flags;
    s.what |= DisplayState::eFlagsChanged;
}

void SurfaceComposerClient::Transaction::setDisplayProjection(const sp<IBinder>& token,
                                                              ui::Rotation orientation,
                                                              const Rect& layerStackRect,
                                                              const Rect& displayRect) {
    DisplayState& s(getDisplayState(token));
    s.orientation = orientation;
    s.layerStackSpaceRect = layerStackRect;
    s.orientedDisplaySpaceRect = displayRect;
    s.what |= DisplayState::eDisplayProjectionChanged;
}

void SurfaceComposerClient::Transaction::setDisplaySize(const sp<IBinder>& token, uint32_t width, uint32_t height) {
    DisplayState& s(getDisplayState(token));
    s.width = width;
    s.height = height;
    s.what |= DisplayState::eDisplaySizeChanged;
}

SurfaceComposerClient::Transaction&
SurfaceComposerClient::Transaction::setTrustedPresentationCallback(
        const sp<SurfaceControl>& sc, TrustedPresentationCallback cb,
        const TrustedPresentationThresholds& thresholds, void* context,
        sp<SurfaceComposerClient::PresentationCallbackRAII>& outCallbackRef) {
    outCallbackRef =
            mTransactionCompletedListener->addTrustedPresentationCallback(cb, sc->getLayerId(),
                                                                          context);

    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eTrustedPresentationInfoChanged;
    s->trustedPresentationThresholds = thresholds;
    s->trustedPresentationListener.configure(
            {.callbackInterface = TransactionCompletedListener::getIInstance(),
             .callbackId = sc->getLayerId()});

    return *this;
}

SurfaceComposerClient::Transaction&
SurfaceComposerClient::Transaction::clearTrustedPresentationCallback(const sp<SurfaceControl>& sc) {
    mTransactionCompletedListener->clearTrustedPresentationCallback(sc->getLayerId());

    layer_state_t* s = getLayerState(sc);
    if (!s) {
        mStatus = BAD_INDEX;
        return *this;
    }
    s->what |= layer_state_t::eTrustedPresentationInfoChanged;
    s->trustedPresentationThresholds = TrustedPresentationThresholds();
    s->trustedPresentationListener.clear();

    return *this;
}

// ---------------------------------------------------------------------------

SurfaceComposerClient::SurfaceComposerClient() : mStatus(NO_INIT) {}

SurfaceComposerClient::SurfaceComposerClient(const sp<ISurfaceComposerClient>& client)
      : mStatus(NO_ERROR), mClient(client) {}

void SurfaceComposerClient::onFirstRef() {
    sp<gui::ISurfaceComposer> sf(ComposerServiceAIDL::getComposerService());
    if (sf != nullptr && mStatus == NO_INIT) {
        sp<ISurfaceComposerClient> conn;
        binder::Status status = sf->createConnection(&conn);
        if (status.isOk() && conn != nullptr) {
            mClient = conn;
            mStatus = NO_ERROR;
        }
    }
}

SurfaceComposerClient::~SurfaceComposerClient() {
    dispose();
}

status_t SurfaceComposerClient::initCheck() const {
    return mStatus;
}

sp<IBinder> SurfaceComposerClient::connection() const {
    return IInterface::asBinder(mClient);
}

status_t SurfaceComposerClient::linkToComposerDeath(
        const sp<IBinder::DeathRecipient>& recipient,
        void* cookie, uint32_t flags) {
    sp<ISurfaceComposer> sf(ComposerService::getComposerService());
    return IInterface::asBinder(sf)->linkToDeath(recipient, cookie, flags);
}

void SurfaceComposerClient::dispose() {
    // this can be called more than once.
    sp<ISurfaceComposerClient> client;
    Mutex::Autolock _lm(mLock);
    if (mClient != nullptr) {
        client = mClient; // hold ref while lock is held
        mClient.clear();
    }
    mStatus = NO_INIT;
}

status_t SurfaceComposerClient::bootFinished() {
    sp<gui::ISurfaceComposer> sf(ComposerServiceAIDL::getComposerService());
    binder::Status status = sf->bootFinished();
    return statusTFromBinderStatus(status);
}

sp<SurfaceControl> SurfaceComposerClient::createSurface(const String8& name, uint32_t w, uint32_t h,
                                                        PixelFormat format, int32_t flags,
                                                        const sp<IBinder>& parentHandle,
                                                        LayerMetadata metadata,
                                                        uint32_t* outTransformHint) {
    sp<SurfaceControl> s;
    createSurfaceChecked(name, w, h, format, &s, flags, parentHandle, std::move(metadata),
                         outTransformHint);
    return s;
}

static std::string toString(const String16& string) {
    return std::string(String8(string).c_str());
}

status_t SurfaceComposerClient::createSurfaceChecked(const String8& name, uint32_t w, uint32_t h,
                                                     PixelFormat format,
                                                     sp<SurfaceControl>* outSurface, int32_t flags,
                                                     const sp<IBinder>& parentHandle,
                                                     LayerMetadata metadata,
                                                     uint32_t* outTransformHint) {
    status_t err = mStatus;

    if (mStatus == NO_ERROR) {
        gui::CreateSurfaceResult result;
        binder::Status status = mClient->createSurface(std::string(name.c_str()), flags,
                                                       parentHandle, std::move(metadata), &result);
        err = statusTFromBinderStatus(status);
        if (outTransformHint) {
            *outTransformHint = result.transformHint;
        }
        ALOGE_IF(err, "SurfaceComposerClient::createSurface error %s", strerror(-err));
        if (err == NO_ERROR) {
            *outSurface = sp<SurfaceControl>::make(this, result.handle, result.layerId,
                                                   toString(result.layerName), w, h, format,
                                                   result.transformHint, flags);
        }
    }
    return err;
}

sp<SurfaceControl> SurfaceComposerClient::mirrorSurface(SurfaceControl* mirrorFromSurface) {
    if (mirrorFromSurface == nullptr) {
        return nullptr;
    }

    sp<IBinder> mirrorFromHandle = mirrorFromSurface->getHandle();
    gui::CreateSurfaceResult result;
    const binder::Status status = mClient->mirrorSurface(mirrorFromHandle, &result);
    const status_t err = statusTFromBinderStatus(status);
    if (err == NO_ERROR) {
        return sp<SurfaceControl>::make(this, result.handle, result.layerId,
                                        toString(result.layerName));
    }
    return nullptr;
}

sp<SurfaceControl> SurfaceComposerClient::mirrorDisplay(DisplayId displayId) {
    gui::CreateSurfaceResult result;
    const binder::Status status = mClient->mirrorDisplay(displayId.value, &result);
    const status_t err = statusTFromBinderStatus(status);
    if (err == NO_ERROR) {
        return sp<SurfaceControl>::make(this, result.handle, result.layerId,
                                        toString(result.layerName));
    }
    return nullptr;
}

status_t SurfaceComposerClient::clearLayerFrameStats(const sp<IBinder>& token) const {
    if (mStatus != NO_ERROR) {
        return mStatus;
    }
    const binder::Status status = mClient->clearLayerFrameStats(token);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getLayerFrameStats(const sp<IBinder>& token,
        FrameStats* outStats) const {
    if (mStatus != NO_ERROR) {
        return mStatus;
    }
    gui::FrameStats stats;
    const binder::Status status = mClient->getLayerFrameStats(token, &stats);
    if (status.isOk()) {
        outStats->refreshPeriodNano = stats.refreshPeriodNano;
        outStats->desiredPresentTimesNano.setCapacity(stats.desiredPresentTimesNano.size());
        for (const auto& t : stats.desiredPresentTimesNano) {
            outStats->desiredPresentTimesNano.add(t);
        }
        outStats->actualPresentTimesNano.setCapacity(stats.actualPresentTimesNano.size());
        for (const auto& t : stats.actualPresentTimesNano) {
            outStats->actualPresentTimesNano.add(t);
        }
        outStats->frameReadyTimesNano.setCapacity(stats.frameReadyTimesNano.size());
        for (const auto& t : stats.frameReadyTimesNano) {
            outStats->frameReadyTimesNano.add(t);
        }
    }
    return statusTFromBinderStatus(status);
}

// ----------------------------------------------------------------------------

status_t SurfaceComposerClient::getDisplayState(const sp<IBinder>& display,
                                                ui::DisplayState* state) {
    gui::DisplayState ds;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getDisplayState(display, &ds);
    if (status.isOk()) {
        state->layerStack = ui::LayerStack::fromValue(ds.layerStack);
        state->orientation = static_cast<ui::Rotation>(ds.orientation);
        state->layerStackSpaceRect =
                ui::Size(ds.layerStackSpaceRect.width, ds.layerStackSpaceRect.height);
    }
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getStaticDisplayInfo(int64_t displayId,
                                                     ui::StaticDisplayInfo* outInfo) {
    using Tag = android::gui::DeviceProductInfo::ManufactureOrModelDate::Tag;
    gui::StaticDisplayInfo ginfo;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getStaticDisplayInfo(displayId, &ginfo);
    if (status.isOk()) {
        // convert gui::StaticDisplayInfo to ui::StaticDisplayInfo
        outInfo->connectionType = static_cast<ui::DisplayConnectionType>(ginfo.connectionType);
        outInfo->port = ginfo.port;
        outInfo->density = ginfo.density;
        outInfo->secure = ginfo.secure;
        outInfo->installOrientation = static_cast<ui::Rotation>(ginfo.installOrientation);

        if (const std::optional<gui::DeviceProductInfo> dpi = ginfo.deviceProductInfo) {
            DeviceProductInfo info;
            info.name = dpi->name;
            if (dpi->manufacturerPnpId.size() > 0) {
                // copid from PnpId = std::array<char, 4> in ui/DeviceProductInfo.h
                constexpr int kMaxPnpIdSize = 4;
                size_t count = std::max<size_t>(kMaxPnpIdSize, dpi->manufacturerPnpId.size());
                std::copy_n(dpi->manufacturerPnpId.begin(), count, info.manufacturerPnpId.begin());
            }
            if (dpi->relativeAddress.size() > 0) {
                std::copy(dpi->relativeAddress.begin(), dpi->relativeAddress.end(),
                          std::back_inserter(info.relativeAddress));
            }
            info.productId = dpi->productId;

            const gui::DeviceProductInfo::ManufactureOrModelDate& date =
                    dpi->manufactureOrModelDate;
            if (date.getTag() == Tag::modelYear) {
                DeviceProductInfo::ModelYear modelYear;
                modelYear.year = static_cast<uint32_t>(date.get<Tag::modelYear>().year);
                info.manufactureOrModelDate = modelYear;
            } else if (date.getTag() == Tag::manufactureYear) {
                DeviceProductInfo::ManufactureYear manufactureYear;
                manufactureYear.year = date.get<Tag::manufactureYear>().modelYear.year;
                info.manufactureOrModelDate = manufactureYear;
            } else if (date.getTag() == Tag::manufactureWeekAndYear) {
                DeviceProductInfo::ManufactureWeekAndYear weekAndYear;
                weekAndYear.year =
                        date.get<Tag::manufactureWeekAndYear>().manufactureYear.modelYear.year;
                weekAndYear.week = date.get<Tag::manufactureWeekAndYear>().week;
                info.manufactureOrModelDate = weekAndYear;
            }

            outInfo->deviceProductInfo = info;
        }
    }
    return statusTFromBinderStatus(status);
}

void SurfaceComposerClient::getDynamicDisplayInfoInternal(gui::DynamicDisplayInfo& ginfo,
                                                          ui::DynamicDisplayInfo*& outInfo) {
    // convert gui::DynamicDisplayInfo to ui::DynamicDisplayInfo
    outInfo->supportedDisplayModes.clear();
    outInfo->supportedDisplayModes.reserve(ginfo.supportedDisplayModes.size());
    for (const auto& mode : ginfo.supportedDisplayModes) {
        ui::DisplayMode outMode;
        outMode.id = mode.id;
        outMode.resolution.width = mode.resolution.width;
        outMode.resolution.height = mode.resolution.height;
        outMode.xDpi = mode.xDpi;
        outMode.yDpi = mode.yDpi;
        outMode.peakRefreshRate = mode.peakRefreshRate;
        outMode.vsyncRate = mode.vsyncRate;
        outMode.appVsyncOffset = mode.appVsyncOffset;
        outMode.sfVsyncOffset = mode.sfVsyncOffset;
        outMode.presentationDeadline = mode.presentationDeadline;
        outMode.group = mode.group;
        std::transform(mode.supportedHdrTypes.begin(), mode.supportedHdrTypes.end(),
                       std::back_inserter(outMode.supportedHdrTypes),
                       [](const int32_t& value) { return static_cast<ui::Hdr>(value); });
        outInfo->supportedDisplayModes.push_back(outMode);
    }

    outInfo->activeDisplayModeId = ginfo.activeDisplayModeId;
    outInfo->renderFrameRate = ginfo.renderFrameRate;

    outInfo->supportedColorModes.clear();
    outInfo->supportedColorModes.reserve(ginfo.supportedColorModes.size());
    for (const auto& cmode : ginfo.supportedColorModes) {
        outInfo->supportedColorModes.push_back(static_cast<ui::ColorMode>(cmode));
    }

    outInfo->activeColorMode = static_cast<ui::ColorMode>(ginfo.activeColorMode);

    std::vector<ui::Hdr> types;
    types.reserve(ginfo.hdrCapabilities.supportedHdrTypes.size());
    for (const auto& hdr : ginfo.hdrCapabilities.supportedHdrTypes) {
        types.push_back(static_cast<ui::Hdr>(hdr));
    }
    outInfo->hdrCapabilities = HdrCapabilities(types, ginfo.hdrCapabilities.maxLuminance,
                                               ginfo.hdrCapabilities.maxAverageLuminance,
                                               ginfo.hdrCapabilities.minLuminance);

    outInfo->autoLowLatencyModeSupported = ginfo.autoLowLatencyModeSupported;
    outInfo->gameContentTypeSupported = ginfo.gameContentTypeSupported;
    outInfo->preferredBootDisplayMode = ginfo.preferredBootDisplayMode;
    outInfo->hasArrSupport = ginfo.hasArrSupport;
    outInfo->frameRateCategoryRate = ui::FrameRateCategoryRate(ginfo.frameRateCategoryRate.normal,
                                                               ginfo.frameRateCategoryRate.high);
    outInfo->supportedRefreshRates.clear();
    outInfo->supportedRefreshRates.reserve(ginfo.supportedRefreshRates.size());
    for (const auto rate : ginfo.supportedRefreshRates) {
        outInfo->supportedRefreshRates.push_back(static_cast<float>(rate));
    }
}

status_t SurfaceComposerClient::getDynamicDisplayInfoFromId(int64_t displayId,
                                                            ui::DynamicDisplayInfo* outInfo) {
    gui::DynamicDisplayInfo ginfo;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getDynamicDisplayInfoFromId(displayId,
                                                                                   &ginfo);
    if (status.isOk()) {
        getDynamicDisplayInfoInternal(ginfo, outInfo);
    }
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getDynamicDisplayInfoFromToken(const sp<IBinder>& display,
                                                               ui::DynamicDisplayInfo* outInfo) {
    gui::DynamicDisplayInfo ginfo;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getDynamicDisplayInfoFromToken(display,
                                                                                      &ginfo);
    if (status.isOk()) {
        getDynamicDisplayInfoInternal(ginfo, outInfo);
    }
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getActiveDisplayMode(const sp<IBinder>& display,
                                                     ui::DisplayMode* mode) {
    ui::DynamicDisplayInfo info;

    status_t result = getDynamicDisplayInfoFromToken(display, &info);
    if (result != NO_ERROR) {
        return result;
    }

    if (const auto activeMode = info.getActiveDisplayMode()) {
        *mode = *activeMode;
        return NO_ERROR;
    }

    ALOGE("Active display mode not found.");
    return NAME_NOT_FOUND;
}

status_t SurfaceComposerClient::setDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                                           const gui::DisplayModeSpecs& specs) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->setDesiredDisplayModeSpecs(displayToken,
                                                                                  specs);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                                           gui::DisplayModeSpecs* outSpecs) {
    if (!outSpecs) {
        return BAD_VALUE;
    }
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getDesiredDisplayModeSpecs(displayToken,
                                                                                  outSpecs);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getDisplayNativePrimaries(const sp<IBinder>& display,
        ui::DisplayPrimaries& outPrimaries) {
    gui::DisplayPrimaries primaries;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getDisplayNativePrimaries(display,
                                                                                 &primaries);
    if (status.isOk()) {
        outPrimaries.red.X = primaries.red.X;
        outPrimaries.red.Y = primaries.red.Y;
        outPrimaries.red.Z = primaries.red.Z;

        outPrimaries.green.X = primaries.green.X;
        outPrimaries.green.Y = primaries.green.Y;
        outPrimaries.green.Z = primaries.green.Z;

        outPrimaries.blue.X = primaries.blue.X;
        outPrimaries.blue.Y = primaries.blue.Y;
        outPrimaries.blue.Z = primaries.blue.Z;

        outPrimaries.white.X = primaries.white.X;
        outPrimaries.white.Y = primaries.white.Y;
        outPrimaries.white.Z = primaries.white.Z;
    }
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::setActiveColorMode(const sp<IBinder>& display,
        ColorMode colorMode) {
    binder::Status status = ComposerServiceAIDL::getComposerService()
                                    ->setActiveColorMode(display, static_cast<int>(colorMode));
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getBootDisplayModeSupport(bool* support) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getBootDisplayModeSupport(support);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getOverlaySupport(gui::OverlayProperties* outProperties) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getOverlaySupport(outProperties);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::setBootDisplayMode(const sp<IBinder>& display,
                                                   ui::DisplayModeId displayModeId) {
    binder::Status status = ComposerServiceAIDL::getComposerService()
                                    ->setBootDisplayMode(display, static_cast<int>(displayModeId));
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::clearBootDisplayMode(const sp<IBinder>& display) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->clearBootDisplayMode(display);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getHdrConversionCapabilities(
        std::vector<gui::HdrConversionCapability>* hdrConversionCapabilities) {
    binder::Status status = ComposerServiceAIDL::getComposerService()->getHdrConversionCapabilities(
            hdrConversionCapabilities);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::setHdrConversionStrategy(
        gui::HdrConversionStrategy hdrConversionStrategy, ui::Hdr* outPreferredHdrOutputType) {
    int hdrType;
    binder::Status status = ComposerServiceAIDL::getComposerService()
                                    ->setHdrConversionStrategy(hdrConversionStrategy, &hdrType);
    *outPreferredHdrOutputType = static_cast<ui::Hdr>(hdrType);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getHdrOutputConversionSupport(bool* isSupported) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getHdrOutputConversionSupport(isSupported);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::setGameModeFrameRateOverride(uid_t uid, float frameRate) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->setGameModeFrameRateOverride(uid, frameRate);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::setGameDefaultFrameRateOverride(uid_t uid, float frameRate) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->setGameDefaultFrameRateOverride(uid,
                                                                                       frameRate);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::updateSmallAreaDetection(std::vector<int32_t>& appIds,
                                                         std::vector<float>& thresholds) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->updateSmallAreaDetection(appIds, thresholds);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::setSmallAreaDetectionThreshold(int32_t appId, float threshold) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->setSmallAreaDetectionThreshold(appId,
                                                                                      threshold);
    return statusTFromBinderStatus(status);
}

void SurfaceComposerClient::setAutoLowLatencyMode(const sp<IBinder>& display, bool on) {
    ComposerServiceAIDL::getComposerService()->setAutoLowLatencyMode(display, on);
}

void SurfaceComposerClient::setGameContentType(const sp<IBinder>& display, bool on) {
    ComposerServiceAIDL::getComposerService()->setGameContentType(display, on);
}

void SurfaceComposerClient::setDisplayPowerMode(const sp<IBinder>& token,
        int mode) {
    ComposerServiceAIDL::getComposerService()->setPowerMode(token, mode);
}

status_t SurfaceComposerClient::getMaxLayerPictureProfiles(const sp<IBinder>& token,
                                                           int32_t* outMaxProfiles) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getMaxLayerPictureProfiles(token,
                                                                                  outMaxProfiles);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getCompositionPreference(
        ui::Dataspace* defaultDataspace, ui::PixelFormat* defaultPixelFormat,
        ui::Dataspace* wideColorGamutDataspace, ui::PixelFormat* wideColorGamutPixelFormat) {
    gui::CompositionPreference pref;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getCompositionPreference(&pref);
    if (status.isOk()) {
        *defaultDataspace = static_cast<ui::Dataspace>(pref.defaultDataspace);
        *defaultPixelFormat = static_cast<ui::PixelFormat>(pref.defaultPixelFormat);
        *wideColorGamutDataspace = static_cast<ui::Dataspace>(pref.wideColorGamutDataspace);
        *wideColorGamutPixelFormat = static_cast<ui::PixelFormat>(pref.wideColorGamutPixelFormat);
    }
    return statusTFromBinderStatus(status);
}

bool SurfaceComposerClient::getProtectedContentSupport() {
    bool supported = false;
    ComposerServiceAIDL::getComposerService()->getProtectedContentSupport(&supported);
    return supported;
}

status_t SurfaceComposerClient::clearAnimationFrameStats() {
    binder::Status status = ComposerServiceAIDL::getComposerService()->clearAnimationFrameStats();
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getAnimationFrameStats(FrameStats* outStats) {
    gui::FrameStats stats;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getAnimationFrameStats(&stats);
    if (status.isOk()) {
        outStats->refreshPeriodNano = stats.refreshPeriodNano;
        outStats->desiredPresentTimesNano.setCapacity(stats.desiredPresentTimesNano.size());
        for (const auto& t : stats.desiredPresentTimesNano) {
            outStats->desiredPresentTimesNano.add(t);
        }
        outStats->actualPresentTimesNano.setCapacity(stats.actualPresentTimesNano.size());
        for (const auto& t : stats.actualPresentTimesNano) {
            outStats->actualPresentTimesNano.add(t);
        }
        outStats->frameReadyTimesNano.setCapacity(stats.frameReadyTimesNano.size());
        for (const auto& t : stats.frameReadyTimesNano) {
            outStats->frameReadyTimesNano.add(t);
        }
    }
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::overrideHdrTypes(const sp<IBinder>& display,
                                                 const std::vector<ui::Hdr>& hdrTypes) {
    std::vector<int32_t> hdrTypesVector;
    hdrTypesVector.reserve(hdrTypes.size());
    for (auto t : hdrTypes) {
        hdrTypesVector.push_back(static_cast<int32_t>(t));
    }

    binder::Status status =
            ComposerServiceAIDL::getComposerService()->overrideHdrTypes(display, hdrTypesVector);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::onPullAtom(const int32_t atomId, std::string* outData,
                                           bool* success) {
    gui::PullAtomData pad;
    binder::Status status = ComposerServiceAIDL::getComposerService()->onPullAtom(atomId, &pad);
    if (status.isOk()) {
        outData->assign(pad.data.begin(), pad.data.end());
        *success = pad.success;
    }
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getDisplayedContentSamplingAttributes(const sp<IBinder>& display,
                                                                      ui::PixelFormat* outFormat,
                                                                      ui::Dataspace* outDataspace,
                                                                      uint8_t* outComponentMask) {
    if (!outFormat || !outDataspace || !outComponentMask) {
        return BAD_VALUE;
    }

    gui::ContentSamplingAttributes attrs;
    binder::Status status = ComposerServiceAIDL::getComposerService()
                                    ->getDisplayedContentSamplingAttributes(display, &attrs);
    if (status.isOk()) {
        *outFormat = static_cast<ui::PixelFormat>(attrs.format);
        *outDataspace = static_cast<ui::Dataspace>(attrs.dataspace);
        *outComponentMask = static_cast<uint8_t>(attrs.componentMask);
    }
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::setDisplayContentSamplingEnabled(const sp<IBinder>& display,
                                                                 bool enable, uint8_t componentMask,
                                                                 uint64_t maxFrames) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()
                    ->setDisplayContentSamplingEnabled(display, enable,
                                                       static_cast<int8_t>(componentMask),
                                                       static_cast<int64_t>(maxFrames));
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::getDisplayedContentSample(const sp<IBinder>& display,
                                                          uint64_t maxFrames, uint64_t timestamp,
                                                          DisplayedFrameStats* outStats) {
    if (!outStats) {
        return BAD_VALUE;
    }

    gui::DisplayedFrameStats stats;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getDisplayedContentSample(display, maxFrames,
                                                                                 timestamp, &stats);
    if (status.isOk()) {
        // convert gui::DisplayedFrameStats to ui::DisplayedFrameStats
        outStats->numFrames = static_cast<uint64_t>(stats.numFrames);
        outStats->component_0_sample.reserve(stats.component_0_sample.size());
        for (const auto& s : stats.component_0_sample) {
            outStats->component_0_sample.push_back(static_cast<uint64_t>(s));
        }
        outStats->component_1_sample.reserve(stats.component_1_sample.size());
        for (const auto& s : stats.component_1_sample) {
            outStats->component_1_sample.push_back(static_cast<uint64_t>(s));
        }
        outStats->component_2_sample.reserve(stats.component_2_sample.size());
        for (const auto& s : stats.component_2_sample) {
            outStats->component_2_sample.push_back(static_cast<uint64_t>(s));
        }
        outStats->component_3_sample.reserve(stats.component_3_sample.size());
        for (const auto& s : stats.component_3_sample) {
            outStats->component_3_sample.push_back(static_cast<uint64_t>(s));
        }
    }
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::isWideColorDisplay(const sp<IBinder>& display,
                                                   bool* outIsWideColorDisplay) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->isWideColorDisplay(display,
                                                                          outIsWideColorDisplay);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::addRegionSamplingListener(
        const Rect& samplingArea, const sp<IBinder>& stopLayerHandle,
        const sp<IRegionSamplingListener>& listener) {
    gui::ARect rect;
    rect.left = samplingArea.left;
    rect.top = samplingArea.top;
    rect.right = samplingArea.right;
    rect.bottom = samplingArea.bottom;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->addRegionSamplingListener(rect,
                                                                                 stopLayerHandle,
                                                                                 listener);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::removeRegionSamplingListener(
        const sp<IRegionSamplingListener>& listener) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->removeRegionSamplingListener(listener);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::addFpsListener(int32_t taskId,
                                               const sp<gui::IFpsListener>& listener) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->addFpsListener(taskId, listener);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::removeFpsListener(const sp<gui::IFpsListener>& listener) {
    binder::Status status = ComposerServiceAIDL::getComposerService()->removeFpsListener(listener);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::addTunnelModeEnabledListener(
        const sp<gui::ITunnelModeEnabledListener>& listener) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->addTunnelModeEnabledListener(listener);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::removeTunnelModeEnabledListener(
        const sp<gui::ITunnelModeEnabledListener>& listener) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->removeTunnelModeEnabledListener(listener);
    return statusTFromBinderStatus(status);
}

bool SurfaceComposerClient::getDisplayBrightnessSupport(const sp<IBinder>& displayToken) {
    bool support = false;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getDisplayBrightnessSupport(displayToken,
                                                                                   &support);
    return status.isOk() ? support : false;
}

status_t SurfaceComposerClient::setDisplayBrightness(const sp<IBinder>& displayToken,
                                                     const gui::DisplayBrightness& brightness) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->setDisplayBrightness(displayToken,
                                                                            brightness);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::addHdrLayerInfoListener(
        const sp<IBinder>& displayToken, const sp<gui::IHdrLayerInfoListener>& listener) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->addHdrLayerInfoListener(displayToken,
                                                                               listener);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::removeHdrLayerInfoListener(
        const sp<IBinder>& displayToken, const sp<gui::IHdrLayerInfoListener>& listener) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->removeHdrLayerInfoListener(displayToken,
                                                                                  listener);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::addActivePictureListener(
        const sp<gui::IActivePictureListener>& listener) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->addActivePictureListener(listener);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::removeActivePictureListener(
        const sp<gui::IActivePictureListener>& listener) {
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->removeActivePictureListener(listener);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::notifyPowerBoost(int32_t boostId) {
    binder::Status status = ComposerServiceAIDL::getComposerService()->notifyPowerBoost(boostId);
    return statusTFromBinderStatus(status);
}

status_t SurfaceComposerClient::setGlobalShadowSettings(const half4& ambientColor,
                                                        const half4& spotColor, float lightPosY,
                                                        float lightPosZ, float lightRadius) {
    gui::Color ambientColorG, spotColorG;
    ambientColorG.r = ambientColor.r;
    ambientColorG.g = ambientColor.g;
    ambientColorG.b = ambientColor.b;
    ambientColorG.a = ambientColor.a;
    spotColorG.r = spotColor.r;
    spotColorG.g = spotColor.g;
    spotColorG.b = spotColor.b;
    spotColorG.a = spotColor.a;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->setGlobalShadowSettings(ambientColorG,
                                                                               spotColorG,
                                                                               lightPosY, lightPosZ,
                                                                               lightRadius);
    return statusTFromBinderStatus(status);
}

std::optional<DisplayDecorationSupport> SurfaceComposerClient::getDisplayDecorationSupport(
        const sp<IBinder>& displayToken) {
    std::optional<gui::DisplayDecorationSupport> gsupport;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getDisplayDecorationSupport(displayToken,
                                                                                   &gsupport);
    std::optional<DisplayDecorationSupport> support;
    if (status.isOk() && gsupport.has_value()) {
        support.emplace(DisplayDecorationSupport{
          .format =
                static_cast<aidl::android::hardware::graphics::common::PixelFormat>(
                gsupport->format),
          .alphaInterpretation =
                static_cast<aidl::android::hardware::graphics::common::AlphaInterpretation>(
                        gsupport->alphaInterpretation)
        });
    }
    return support;
}

int SurfaceComposerClient::getGpuContextPriority() {
    int priority;
    binder::Status status =
            ComposerServiceAIDL::getComposerService()->getGpuContextPriority(&priority);
    if (!status.isOk()) {
        status_t err = statusTFromBinderStatus(status);
        ALOGE("getGpuContextPriority failed to read data:  %s (%d)", strerror(-err), err);
        return 0;
    }
    return priority;
}

status_t SurfaceComposerClient::addWindowInfosListener(
        const sp<WindowInfosListener>& windowInfosListener,
        std::pair<std::vector<gui::WindowInfo>, std::vector<gui::DisplayInfo>>* outInitialInfo) {
    return WindowInfosListenerReporter::getInstance()
            ->addWindowInfosListener(windowInfosListener, ComposerServiceAIDL::getComposerService(),
                                     outInitialInfo);
}

status_t SurfaceComposerClient::removeWindowInfosListener(
        const sp<WindowInfosListener>& windowInfosListener) {
    return WindowInfosListenerReporter::getInstance()
            ->removeWindowInfosListener(windowInfosListener,
                                        ComposerServiceAIDL::getComposerService());
}

void SurfaceComposerClient::notifyShutdown() {
    ComposerServiceAIDL::getComposerService()->notifyShutdown();
}
// ----------------------------------------------------------------------------

status_t ScreenshotClient::captureDisplay(const DisplayCaptureArgs& captureArgs,
                                          const sp<IScreenCaptureListener>& captureListener) {
    sp<gui::ISurfaceComposer> s(ComposerServiceAIDL::getComposerService());
    if (s == nullptr) return NO_INIT;

    binder::Status status = s->captureDisplay(captureArgs, captureListener);
    return statusTFromBinderStatus(status);
}

status_t ScreenshotClient::captureDisplay(DisplayId displayId, const gui::CaptureArgs& captureArgs,
                                          const sp<IScreenCaptureListener>& captureListener) {
    sp<gui::ISurfaceComposer> s(ComposerServiceAIDL::getComposerService());
    if (s == nullptr) return NO_INIT;

    binder::Status status = s->captureDisplayById(displayId.value, captureArgs, captureListener);
    return statusTFromBinderStatus(status);
}

status_t ScreenshotClient::captureLayers(const LayerCaptureArgs& captureArgs,
                                         const sp<IScreenCaptureListener>& captureListener,
                                         bool sync) {
    sp<gui::ISurfaceComposer> s(ComposerServiceAIDL::getComposerService());
    if (s == nullptr) return NO_INIT;

    binder::Status status;
    if (sync) {
        gui::ScreenCaptureResults captureResults;
        status = s->captureLayersSync(captureArgs, &captureResults);
        captureListener->onScreenCaptureCompleted(captureResults);
    } else {
        status = s->captureLayers(captureArgs, captureListener);
    }
    return statusTFromBinderStatus(status);
}

// ---------------------------------------------------------------------------------

void ReleaseCallbackThread::addReleaseCallback(const ReleaseCallbackId callbackId,
                                               sp<Fence> releaseFence) {
    std::scoped_lock<std::mutex> lock(mMutex);
    if (!mStarted) {
        mThread = std::thread(&ReleaseCallbackThread::threadMain, this);
        mStarted = true;
    }

    mCallbackInfos.emplace(callbackId, std::move(releaseFence));
    mReleaseCallbackPending.notify_one();
}

void ReleaseCallbackThread::threadMain() {
    const auto listener = TransactionCompletedListener::getInstance();
    std::queue<std::tuple<const ReleaseCallbackId, const sp<Fence>>> callbackInfos;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            base::ScopedLockAssertion assumeLocked(mMutex);
            callbackInfos = std::move(mCallbackInfos);
            mCallbackInfos = {};
        }

        while (!callbackInfos.empty()) {
            auto [callbackId, releaseFence] = callbackInfos.front();
            listener->onReleaseBuffer(callbackId, std::move(releaseFence), UINT_MAX);
            callbackInfos.pop();
        }

        {
            std::unique_lock<std::mutex> lock(mMutex);
            base::ScopedLockAssertion assumeLocked(mMutex);
            if (mCallbackInfos.size() == 0) {
                mReleaseCallbackPending.wait(lock);
            }
        }
    }
}

} // namespace android
