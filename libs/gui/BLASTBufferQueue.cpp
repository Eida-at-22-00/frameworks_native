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

#undef LOG_TAG
#define LOG_TAG "BLASTBufferQueue"

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#include <com_android_graphics_libgui_flags.h>
#include <cutils/atomic.h>
#include <ftl/fake_guard.h>
#include <gui/BLASTBufferQueue.h>
#include <gui/BufferItemConsumer.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <gui/FrameRateUtils.h>
#include <gui/GLConsumer.h>
#include <gui/IProducerListener.h>
#include <gui/Surface.h>
#include <gui/TraceUtils.h>
#include <utils/Singleton.h>
#include <utils/Trace.h>

#include <private/gui/ComposerService.h>
#include <private/gui/ComposerServiceAIDL.h>

#include <android-base/thread_annotations.h>

#include <com_android_graphics_libgui_flags.h>

using namespace com::android::graphics::libgui;
using namespace std::chrono_literals;

namespace {

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
template <class Mutex>
class UnlockGuard {
public:
    explicit UnlockGuard(Mutex& lock) : mLock{lock} { mLock.unlock(); }

    ~UnlockGuard() { mLock.lock(); }

    UnlockGuard(const UnlockGuard&) = delete;
    UnlockGuard& operator=(const UnlockGuard&) = delete;

private:
    Mutex& mLock;
};
#endif

inline const char* boolToString(bool b) {
    return b ? "true" : "false";
}

} // namespace

namespace android {

// Macros to include adapter info in log messages
#define BQA_LOGD(x, ...) \
    ALOGD("[%s](f:%u,a:%u) " x, mName.c_str(), mNumFrameAvailable, mNumAcquired, ##__VA_ARGS__)
#define BQA_LOGV(x, ...) \
    ALOGV("[%s](f:%u,a:%u) " x, mName.c_str(), mNumFrameAvailable, mNumAcquired, ##__VA_ARGS__)
// enable logs for a single layer
//#define BQA_LOGV(x, ...) \
//    ALOGV_IF((strstr(mName.c_str(), "SurfaceView") != nullptr), "[%s](f:%u,a:%u) " x, \
//              mName.c_str(), mNumFrameAvailable, mNumAcquired, ##__VA_ARGS__)
#define BQA_LOGE(x, ...) \
    ALOGE("[%s](f:%u,a:%u) " x, mName.c_str(), mNumFrameAvailable, mNumAcquired, ##__VA_ARGS__)

#define BBQ_TRACE(x, ...)                                                                  \
    ATRACE_FORMAT("%s - %s(f:%u,a:%u)" x, __FUNCTION__, mName.c_str(), mNumFrameAvailable, \
                  mNumAcquired, ##__VA_ARGS__)

#define UNIQUE_LOCK_WITH_ASSERTION(mutex) \
    std::unique_lock _lock{mutex};        \
    base::ScopedLockAssertion assumeLocked(mutex);

void BLASTBufferItemConsumer::onDisconnect() {
    Mutex::Autolock lock(mMutex);
    mPreviouslyConnected = mCurrentlyConnected;
    mCurrentlyConnected = false;
    if (mPreviouslyConnected) {
        mDisconnectEvents.push(mCurrentFrameNumber);
    }
    mFrameEventHistory.onDisconnect();
}

void BLASTBufferItemConsumer::addAndGetFrameTimestamps(const NewFrameEventsEntry* newTimestamps,
                                                       FrameEventHistoryDelta* outDelta) {
    Mutex::Autolock lock(mMutex);
    if (newTimestamps) {
        // BufferQueueProducer only adds a new timestamp on
        // queueBuffer
        mCurrentFrameNumber = newTimestamps->frameNumber;
        mFrameEventHistory.addQueue(*newTimestamps);
    }
    if (outDelta) {
        // frame event histories will be processed
        // only after the producer connects and requests
        // deltas for the first time.  Forward this intent
        // to SF-side to turn event processing back on
        mPreviouslyConnected = mCurrentlyConnected;
        mCurrentlyConnected = true;
        mFrameEventHistory.getAndResetDelta(outDelta);
    }
}

void BLASTBufferItemConsumer::updateFrameTimestamps(
        uint64_t frameNumber, uint64_t previousFrameNumber, nsecs_t refreshStartTime,
        const sp<Fence>& glDoneFence, const sp<Fence>& presentFence,
        const sp<Fence>& prevReleaseFence, CompositorTiming compositorTiming, nsecs_t latchTime,
        nsecs_t dequeueReadyTime) {
    Mutex::Autolock lock(mMutex);

    // if the producer is not connected, don't bother updating,
    // the next producer that connects won't access this frame event
    if (!mCurrentlyConnected) return;
    std::shared_ptr<FenceTime> glDoneFenceTime = std::make_shared<FenceTime>(glDoneFence);
    std::shared_ptr<FenceTime> presentFenceTime = std::make_shared<FenceTime>(presentFence);
    std::shared_ptr<FenceTime> releaseFenceTime = std::make_shared<FenceTime>(prevReleaseFence);

    mFrameEventHistory.addLatch(frameNumber, latchTime);
    if (flags::frametimestamps_previousrelease()) {
        if (previousFrameNumber > 0) {
            mFrameEventHistory.addRelease(previousFrameNumber, dequeueReadyTime,
                                          std::move(releaseFenceTime));
        }
    } else {
        mFrameEventHistory.addRelease(frameNumber, dequeueReadyTime, std::move(releaseFenceTime));
    }

    mFrameEventHistory.addPreComposition(frameNumber, refreshStartTime);
    mFrameEventHistory.addPostComposition(frameNumber, glDoneFenceTime, presentFenceTime,
                                          compositorTiming);
}

void BLASTBufferItemConsumer::getConnectionEvents(uint64_t frameNumber, bool* needsDisconnect) {
    bool disconnect = false;
    Mutex::Autolock lock(mMutex);
    while (!mDisconnectEvents.empty() && mDisconnectEvents.front() <= frameNumber) {
        disconnect = true;
        mDisconnectEvents.pop();
    }
    if (needsDisconnect != nullptr) *needsDisconnect = disconnect;
}

void BLASTBufferItemConsumer::onSidebandStreamChanged() {
    sp<BLASTBufferQueue> bbq = mBLASTBufferQueue.promote();
    if (bbq != nullptr) {
        sp<NativeHandle> stream = getSidebandStream();
        bbq->setSidebandStream(stream);
    }
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_SETFRAMERATE)
void BLASTBufferItemConsumer::onSetFrameRate(float frameRate, int8_t compatibility,
                                             int8_t changeFrameRateStrategy) {
    sp<BLASTBufferQueue> bbq = mBLASTBufferQueue.promote();
    if (bbq != nullptr) {
        bbq->setFrameRate(frameRate, compatibility, changeFrameRateStrategy);
    }
}
#endif

void BLASTBufferItemConsumer::resizeFrameEventHistory(size_t newSize) {
    Mutex::Autolock lock(mMutex);
    mFrameEventHistory.resize(newSize);
}

BLASTBufferQueue::BLASTBufferQueue(const std::string& name, bool updateDestinationFrame)
      : mSurfaceControl(nullptr),
        mSize(1, 1),
        mRequestedSize(mSize),
        mFormat(PIXEL_FORMAT_RGBA_8888),
        mTransactionReadyCallback(nullptr),
        mSyncTransaction(nullptr),
        mUpdateDestinationFrame(updateDestinationFrame) {
    createBufferQueue(&mProducer, &mConsumer);
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
    mBufferItemConsumer = sp<BLASTBufferItemConsumer>::make(mProducer, mConsumer,
                                                            GraphicBuffer::USAGE_HW_COMPOSER |
                                                                    GraphicBuffer::USAGE_HW_TEXTURE,
                                                            1, false, this);
#else
    mBufferItemConsumer = sp<BLASTBufferItemConsumer>::make(mConsumer,
                                                            GraphicBuffer::USAGE_HW_COMPOSER |
                                                                    GraphicBuffer::USAGE_HW_TEXTURE,
                                                            1, false, this);
#endif //  COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
    // since the adapter is in the client process, set dequeue timeout
    // explicitly so that dequeueBuffer will block
    mProducer->setDequeueTimeout(std::numeric_limits<int64_t>::max());

    static std::atomic<uint32_t> nextId = 0;
    mProducerId = nextId++;
    mName = name + "#" + std::to_string(mProducerId);
    auto consumerName = mName + "(BLAST Consumer)" + std::to_string(mProducerId);
    mQueuedBufferTrace = "QueuedBuffer - " + mName + "BLAST#" + std::to_string(mProducerId);
    mBufferItemConsumer->setName(String8(consumerName.c_str()));
    mBufferItemConsumer->setFrameAvailableListener(this);

    ComposerServiceAIDL::getComposerService()->getMaxAcquiredBufferCount(&mMaxAcquiredBuffers);
    mBufferItemConsumer->setMaxAcquiredBufferCount(mMaxAcquiredBuffers);
    mCurrentMaxAcquiredBufferCount = mMaxAcquiredBuffers;

    TransactionCompletedListener::getInstance()->addQueueStallListener(
            [&](const std::string& reason) {
                std::function<void(const std::string&)> callbackCopy;
                {
                    std::unique_lock _lock{mMutex};
                    callbackCopy = mTransactionHangCallback;
                }
                if (callbackCopy) callbackCopy(reason);
            },
            this);

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
    gui::BufferReleaseChannel::open(mName, mBufferReleaseConsumer, mBufferReleaseProducer);
    mBufferReleaseReader.emplace(*this);
#endif

    BQA_LOGV("BLASTBufferQueue created");
}

BLASTBufferQueue::~BLASTBufferQueue() {
    TransactionCompletedListener::getInstance()->removeQueueStallListener(this);
    if (mPendingTransactions.empty()) {
        return;
    }
    BQA_LOGE("Applying pending transactions on dtor %d",
             static_cast<uint32_t>(mPendingTransactions.size()));
    SurfaceComposerClient::Transaction t;
    mergePendingTransactions(&t, std::numeric_limits<uint64_t>::max() /* frameNumber */);
    // All transactions on our apply token are one-way. See comment on mAppliedLastTransaction
    t.setApplyToken(mApplyToken).apply(false, true);

    if (mTransactionReadyCallback) {
        mTransactionReadyCallback(mSyncTransaction);
    }
}

void BLASTBufferQueue::onFirstRef() {
    // safe default, most producers are expected to override this
    mProducer->setMaxDequeuedBufferCount(2);
}

void BLASTBufferQueue::update(const sp<SurfaceControl>& surface, uint32_t width, uint32_t height,
                              int32_t format) {
    LOG_ALWAYS_FATAL_IF(surface == nullptr, "BLASTBufferQueue: mSurfaceControl must not be NULL");

    std::lock_guard _lock{mMutex};
    if (mFormat != format) {
        mFormat = format;
        mBufferItemConsumer->setDefaultBufferFormat(convertBufferFormat(format));
    }

    const bool surfaceControlChanged = !SurfaceControl::isSameSurface(mSurfaceControl, surface);
    if (surfaceControlChanged && mSurfaceControl != nullptr) {
        BQA_LOGD("Updating SurfaceControl without recreating BBQ");
    }

    // Always update the native object even though they might have the same layer handle, so we can
    // get the updated transform hint from WM.
    mSurfaceControl = surface;
    SurfaceComposerClient::Transaction t;
    bool applyTransaction = false;
    if (surfaceControlChanged) {
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
        updateBufferReleaseProducer();
#endif
        t.setFlags(mSurfaceControl, layer_state_t::eEnableBackpressure,
                   layer_state_t::eEnableBackpressure);
        // Migrate the picture profile handle to the new surface control.
#if COM_ANDROID_GRAPHICS_LIBUI_FLAGS_APPLY_PICTURE_PROFILES
        if (com_android_graphics_libgui_flags_apply_picture_profiles() &&
            mPictureProfileHandle.has_value()) {
            t.setPictureProfileHandle(mSurfaceControl, *mPictureProfileHandle);
        }
#endif // COM_ANDROID_GRAPHICS_LIBUI_FLAGS_APPLY_PICTURE_PROFILES
        applyTransaction = true;
    }
    mTransformHint = mSurfaceControl->getTransformHint();
    mBufferItemConsumer->setTransformHint(mTransformHint);
    BQA_LOGV("update width=%d height=%d format=%d mTransformHint=%d", width, height, format,
             mTransformHint);

    ui::Size newSize(width, height);
    if (mRequestedSize != newSize) {
        mRequestedSize.set(newSize);
        mBufferItemConsumer->setDefaultBufferSize(mRequestedSize.width, mRequestedSize.height);
        if (mLastBufferInfo.scalingMode != NATIVE_WINDOW_SCALING_MODE_FREEZE) {
            // If the buffer supports scaling, update the frame immediately since the client may
            // want to scale the existing buffer to the new size.
            mSize = mRequestedSize;
            if (mUpdateDestinationFrame) {
                t.setDestinationFrame(mSurfaceControl, Rect(newSize));
                applyTransaction = true;
            }
        }
    }
    if (applyTransaction) {
        // All transactions on our apply token are one-way. See comment on mAppliedLastTransaction
        t.setApplyToken(mApplyToken).apply(false /* synchronous */, true /* oneWay */);
    }
}

static std::optional<SurfaceControlStats> findMatchingStat(
        const std::vector<SurfaceControlStats>& stats, const sp<SurfaceControl>& sc) {
    for (auto stat : stats) {
        if (SurfaceControl::isSameSurface(sc, stat.surfaceControl)) {
            return stat;
        }
    }
    return std::nullopt;
}

TransactionCompletedCallbackTakesContext BLASTBufferQueue::makeTransactionCommittedCallbackThunk() {
    return [bbq = sp<BLASTBufferQueue>::fromExisting(
                    this)](void* /*context*/, nsecs_t latchTime, const sp<Fence>& presentFence,
                           const std::vector<SurfaceControlStats>& stats) {
        bbq->transactionCommittedCallback(latchTime, presentFence, stats);
    };
}

void BLASTBufferQueue::transactionCommittedCallback(nsecs_t /*latchTime*/,
                                                    const sp<Fence>& /*presentFence*/,
                                                    const std::vector<SurfaceControlStats>& stats) {
    {
        std::lock_guard _lock{mMutex};
        BBQ_TRACE();
        BQA_LOGV("transactionCommittedCallback");
        if (!mSurfaceControlsWithPendingCallback.empty()) {
            sp<SurfaceControl> pendingSC = mSurfaceControlsWithPendingCallback.front();
            std::optional<SurfaceControlStats> stat = findMatchingStat(stats, pendingSC);
            if (stat) {
                uint64_t currFrameNumber = stat->frameEventStats.frameNumber;

                // We need to check if we were waiting for a transaction callback in order to
                // process any pending buffers and unblock. It's possible to get transaction
                // callbacks for previous requests so we need to ensure that there are no pending
                // frame numbers that were in a sync. We remove the frame from mSyncedFrameNumbers
                // set and then check if it's empty. If there are no more pending syncs, we can
                // proceed with flushing the shadow queue.
                mSyncedFrameNumbers.erase(currFrameNumber);
                if (mSyncedFrameNumbers.empty()) {
                    flushShadowQueue();
                }
            } else {
                BQA_LOGE("Failed to find matching SurfaceControl in transactionCommittedCallback");
            }
        } else {
            BQA_LOGE("No matching SurfaceControls found: mSurfaceControlsWithPendingCallback was "
                     "empty.");
        }
    }
}

TransactionCompletedCallbackTakesContext BLASTBufferQueue::makeTransactionCallbackThunk() {
    return [bbq = sp<BLASTBufferQueue>::fromExisting(
                    this)](void* /*context*/, nsecs_t latchTime, const sp<Fence>& presentFence,
                           const std::vector<SurfaceControlStats>& stats) {
        bbq->transactionCallback(latchTime, presentFence, stats);
    };
}

void BLASTBufferQueue::transactionCallback(nsecs_t /*latchTime*/, const sp<Fence>& /*presentFence*/,
                                           const std::vector<SurfaceControlStats>& stats) {
    {
        std::lock_guard _lock{mMutex};
        BBQ_TRACE();
        BQA_LOGV("transactionCallback");

        if (!mSurfaceControlsWithPendingCallback.empty()) {
            sp<SurfaceControl> pendingSC = mSurfaceControlsWithPendingCallback.front();
            mSurfaceControlsWithPendingCallback.pop();
            std::optional<SurfaceControlStats> statsOptional = findMatchingStat(stats, pendingSC);
            if (statsOptional) {
                SurfaceControlStats stat = *statsOptional;
                if (stat.transformHint) {
                    mTransformHint = *stat.transformHint;
                    mBufferItemConsumer->setTransformHint(mTransformHint);
                    BQA_LOGV("updated mTransformHint=%d", mTransformHint);
                }
                // Update frametime stamps if the frame was latched and presented, indicated by a
                // valid latch time.
                if (stat.latchTime > 0) {
                    mBufferItemConsumer
                            ->updateFrameTimestamps(stat.frameEventStats.frameNumber,
                                                    stat.frameEventStats.previousFrameNumber,
                                                    stat.frameEventStats.refreshStartTime,
                                                    stat.frameEventStats.gpuCompositionDoneFence,
                                                    stat.presentFence, stat.previousReleaseFence,
                                                    stat.frameEventStats.compositorTiming,
                                                    stat.latchTime,
                                                    stat.frameEventStats.dequeueReadyTime);
                }
                auto currFrameNumber = stat.frameEventStats.frameNumber;
                // Release stale buffers.
                for (const auto& [key, _] : mSubmitted) {
                    if (currFrameNumber <= key.framenumber) {
                        continue; // not stale.
                    }
                    releaseBufferCallbackLocked(key,
                                                stat.previousReleaseFence
                                                        ? stat.previousReleaseFence
                                                        : Fence::NO_FENCE,
                                                stat.currentMaxAcquiredBufferCount,
                                                true /* fakeRelease */);
                }
            } else {
                BQA_LOGE("Failed to find matching SurfaceControl in transactionCallback");
            }
        } else {
            BQA_LOGE("No matching SurfaceControls found: mSurfaceControlsWithPendingCallback was "
                     "empty.");
        }
    }
}

void BLASTBufferQueue::flushShadowQueue() {
    BQA_LOGV("flushShadowQueue");
    int32_t numFramesToFlush = mNumFrameAvailable;
    while (numFramesToFlush > 0) {
        acquireNextBufferLocked(std::nullopt);
        numFramesToFlush--;
    }
}

// Unlike transactionCallbackThunk the release buffer callback does not extend the life of the
// BBQ. This is because if the BBQ is destroyed, then the buffers will be released by the client.
// So we pass in a weak pointer to the BBQ and if it still alive, then we release the buffer.
// Otherwise, this is a no-op.
ReleaseBufferCallback BLASTBufferQueue::makeReleaseBufferCallbackThunk() {
    return [weakBbq = wp<BLASTBufferQueue>::fromExisting(
                    this)](const ReleaseCallbackId& id, const sp<Fence>& releaseFence,
                           std::optional<uint32_t> currentMaxAcquiredBufferCount) {
        sp<BLASTBufferQueue> bbq = weakBbq.promote();
        if (!bbq) {
            ALOGV("releaseBufferCallbackThunk %s blastBufferQueue is dead", id.to_string().c_str());
            return;
        }
        bbq->releaseBufferCallback(id, releaseFence, currentMaxAcquiredBufferCount);
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
        bbq->drainBufferReleaseConsumer();
#endif
    };
}

void BLASTBufferQueue::releaseBufferCallback(
        const ReleaseCallbackId& id, const sp<Fence>& releaseFence,
        std::optional<uint32_t> currentMaxAcquiredBufferCount) {
    std::lock_guard _lock{mMutex};
    BBQ_TRACE();
    releaseBufferCallbackLocked(id, releaseFence, currentMaxAcquiredBufferCount,
                                false /* fakeRelease */);
}

void BLASTBufferQueue::releaseBufferCallbackLocked(
        const ReleaseCallbackId& id, const sp<Fence>& releaseFence,
        std::optional<uint32_t> currentMaxAcquiredBufferCount, bool fakeRelease) {
    ATRACE_CALL();
    BQA_LOGV("releaseBufferCallback %s", id.to_string().c_str());

    // Calculate how many buffers we need to hold before we release them back
    // to the buffer queue. This will prevent higher latency when we are running
    // on a lower refresh rate than the max supported. We only do that for EGL
    // clients as others don't care about latency
    const auto it = mSubmitted.find(id);
    const bool isEGL = it != mSubmitted.end() && it->second.mApi == NATIVE_WINDOW_API_EGL;

    if (currentMaxAcquiredBufferCount) {
        mCurrentMaxAcquiredBufferCount = *currentMaxAcquiredBufferCount;
    }

    const uint32_t numPendingBuffersToHold =
            isEGL ? std::max(0, mMaxAcquiredBuffers - (int32_t)mCurrentMaxAcquiredBufferCount) : 0;

    auto rb = ReleasedBuffer{id, releaseFence};
    if (std::find(mPendingRelease.begin(), mPendingRelease.end(), rb) == mPendingRelease.end()) {
        mPendingRelease.emplace_back(rb);
        if (fakeRelease) {
            BQA_LOGE("Faking releaseBufferCallback from transactionCompleteCallback %" PRIu64,
                     id.framenumber);
            BBQ_TRACE("FakeReleaseCallback");
        }
    }

    // Release all buffers that are beyond the ones that we need to hold
    while (mPendingRelease.size() > numPendingBuffersToHold) {
        const auto releasedBuffer = mPendingRelease.front();
        mPendingRelease.pop_front();
        releaseBuffer(releasedBuffer.callbackId, releasedBuffer.releaseFence);
        // Don't process the transactions here if mSyncedFrameNumbers is not empty. That means
        // are still transactions that have sync buffers in them that have not been applied or
        // dropped. Instead, let onFrameAvailable handle processing them since it will merge with
        // the syncTransaction.
        if (mSyncedFrameNumbers.empty()) {
            acquireNextBufferLocked(std::nullopt);
        }
    }

    ATRACE_INT("PendingRelease", mPendingRelease.size());
    ATRACE_INT(mQueuedBufferTrace.c_str(),
               mNumFrameAvailable + mNumAcquired - mPendingRelease.size());
    mCallbackCV.notify_all();
}

void BLASTBufferQueue::releaseBuffer(const ReleaseCallbackId& callbackId,
                                     const sp<Fence>& releaseFence) {
    auto it = mSubmitted.find(callbackId);
    if (it == mSubmitted.end()) {
        return;
    }
    mNumAcquired--;
    BBQ_TRACE("frame=%" PRIu64, callbackId.framenumber);
    BQA_LOGV("released %s", callbackId.to_string().c_str());
    mBufferItemConsumer->releaseBuffer(it->second, releaseFence);
    mSubmitted.erase(it);
    // Remove the frame number from mSyncedFrameNumbers since we can get a release callback
    // without getting a transaction committed if the buffer was dropped.
    mSyncedFrameNumbers.erase(callbackId.framenumber);
}

static ui::Size getBufferSize(const BufferItem& item) {
    uint32_t bufWidth = item.mGraphicBuffer->getWidth();
    uint32_t bufHeight = item.mGraphicBuffer->getHeight();

    // Take the buffer's orientation into account
    if (item.mTransform & ui::Transform::ROT_90) {
        std::swap(bufWidth, bufHeight);
    }
    return ui::Size(bufWidth, bufHeight);
}

status_t BLASTBufferQueue::acquireNextBufferLocked(
        const std::optional<SurfaceComposerClient::Transaction*> transaction) {
    // Check if we have frames available and we have not acquired the maximum number of buffers.
    // Even with this check, the consumer can fail to acquire an additional buffer if the consumer
    // has already acquired (mMaxAcquiredBuffers + 1) and the new buffer is not droppable. In this
    // case mBufferItemConsumer->acquireBuffer will return with NO_BUFFER_AVAILABLE.
    if (mNumFrameAvailable == 0) {
        BQA_LOGV("Can't acquire next buffer. No available frames");
        return BufferQueue::NO_BUFFER_AVAILABLE;
    }

    if (mNumAcquired >= (mMaxAcquiredBuffers + 2)) {
        BQA_LOGV("Can't acquire next buffer. Already acquired max frames %d max:%d + 2",
                 mNumAcquired, mMaxAcquiredBuffers);
        return BufferQueue::NO_BUFFER_AVAILABLE;
    }

    if (mSurfaceControl == nullptr) {
        BQA_LOGE("ERROR : surface control is null");
        return NAME_NOT_FOUND;
    }

    SurfaceComposerClient::Transaction localTransaction;
    bool applyTransaction = true;
    SurfaceComposerClient::Transaction* t = &localTransaction;
    if (transaction) {
        t = *transaction;
        applyTransaction = false;
    }

    BufferItem bufferItem;

    status_t status =
            mBufferItemConsumer->acquireBuffer(&bufferItem, 0 /* expectedPresent */, false);
    if (status == BufferQueue::NO_BUFFER_AVAILABLE) {
        BQA_LOGV("Failed to acquire a buffer, err=NO_BUFFER_AVAILABLE");
        return status;
    } else if (status != OK) {
        BQA_LOGE("Failed to acquire a buffer, err=%s", statusToString(status).c_str());
        return status;
    }

    auto buffer = bufferItem.mGraphicBuffer;
    mNumFrameAvailable--;
    BBQ_TRACE("frame=%" PRIu64, bufferItem.mFrameNumber);

    if (buffer == nullptr) {
        mBufferItemConsumer->releaseBuffer(bufferItem, Fence::NO_FENCE);
        BQA_LOGE("Buffer was empty");
        return BAD_VALUE;
    }

    if (rejectBuffer(bufferItem)) {
        BQA_LOGE("rejecting buffer:active_size=%dx%d, requested_size=%dx%d "
                 "buffer{size=%dx%d transform=%d}",
                 mSize.width, mSize.height, mRequestedSize.width, mRequestedSize.height,
                 buffer->getWidth(), buffer->getHeight(), bufferItem.mTransform);
        mBufferItemConsumer->releaseBuffer(bufferItem, Fence::NO_FENCE);
        return acquireNextBufferLocked(transaction);
    }

    mNumAcquired++;
    mLastAcquiredFrameNumber = bufferItem.mFrameNumber;
    ReleaseCallbackId releaseCallbackId(buffer->getId(), mLastAcquiredFrameNumber);
    mSubmitted.emplace_or_replace(releaseCallbackId, bufferItem);

    bool needsDisconnect = false;
    mBufferItemConsumer->getConnectionEvents(bufferItem.mFrameNumber, &needsDisconnect);

    // if producer disconnected before, notify SurfaceFlinger
    if (needsDisconnect) {
        t->notifyProducerDisconnect(mSurfaceControl);
    }

    // Only update mSize for destination bounds if the incoming buffer matches the requested size.
    // Otherwise, it could cause stretching since the destination bounds will update before the
    // buffer with the new size is acquired.
    if (mRequestedSize == getBufferSize(bufferItem) ||
        bufferItem.mScalingMode != NATIVE_WINDOW_SCALING_MODE_FREEZE) {
        mSize = mRequestedSize;
    }
    Rect crop = computeCrop(bufferItem);
    mLastBufferInfo.update(true /* hasBuffer */, bufferItem.mGraphicBuffer->getWidth(),
                           bufferItem.mGraphicBuffer->getHeight(), bufferItem.mTransform,
                           bufferItem.mScalingMode, crop);

    auto releaseBufferCallback = makeReleaseBufferCallbackThunk();
    sp<Fence> fence =
            bufferItem.mFence ? sp<Fence>::make(bufferItem.mFence->dup()) : Fence::NO_FENCE;

    nsecs_t dequeueTime = -1;
    {
        std::lock_guard _lock{mTimestampMutex};
        auto dequeueTimeIt = mDequeueTimestamps.find(buffer->getId());
        if (dequeueTimeIt != mDequeueTimestamps.end()) {
            dequeueTime = dequeueTimeIt->second;
            mDequeueTimestamps.erase(dequeueTimeIt);
        }
    }

    t->setBuffer(mSurfaceControl, buffer, fence, bufferItem.mFrameNumber, mProducerId,
                 releaseBufferCallback, dequeueTime);
    t->setDataspace(mSurfaceControl, static_cast<ui::Dataspace>(bufferItem.mDataSpace));
    t->setHdrMetadata(mSurfaceControl, bufferItem.mHdrMetadata);
    t->setSurfaceDamageRegion(mSurfaceControl, bufferItem.mSurfaceDamage);
    t->addTransactionCompletedCallback(makeTransactionCallbackThunk(), nullptr);

    mSurfaceControlsWithPendingCallback.push(mSurfaceControl);

    if (mUpdateDestinationFrame) {
        t->setDestinationFrame(mSurfaceControl, Rect(mSize));
    } else {
        const bool ignoreDestinationFrame =
                bufferItem.mScalingMode == NATIVE_WINDOW_SCALING_MODE_FREEZE;
        t->setFlags(mSurfaceControl,
                    ignoreDestinationFrame ? layer_state_t::eIgnoreDestinationFrame : 0,
                    layer_state_t::eIgnoreDestinationFrame);
    }
    t->setBufferCrop(mSurfaceControl, crop);
    t->setTransform(mSurfaceControl, bufferItem.mTransform);
    t->setTransformToDisplayInverse(mSurfaceControl, bufferItem.mTransformToDisplayInverse);
    t->setAutoRefresh(mSurfaceControl, bufferItem.mAutoRefresh);
    if (!bufferItem.mIsAutoTimestamp) {
        t->setDesiredPresentTime(bufferItem.mTimestamp);
    }
#if COM_ANDROID_GRAPHICS_LIBUI_FLAGS_APPLY_PICTURE_PROFILES
    if (com_android_graphics_libgui_flags_apply_picture_profiles() &&
        bufferItem.mPictureProfileHandle.has_value()) {
        t->setPictureProfileHandle(mSurfaceControl, *bufferItem.mPictureProfileHandle);
        // The current picture profile must be maintained in case the BBQ gets its
        // SurfaceControl switched out.
        mPictureProfileHandle = bufferItem.mPictureProfileHandle;
        // Clear out the picture profile if the requestor has asked for it to be cleared
        if (mPictureProfileHandle == PictureProfileHandle::NONE) {
            mPictureProfileHandle = std::nullopt;
        }
    }
#endif // COM_ANDROID_GRAPHICS_LIBUI_FLAGS_APPLY_PICTURE_PROFILES

    // Drop stale frame timeline infos
    while (!mPendingFrameTimelines.empty() &&
           mPendingFrameTimelines.front().first < bufferItem.mFrameNumber) {
        ATRACE_FORMAT_INSTANT("dropping stale frameNumber: %" PRIu64 " vsyncId: %" PRId64,
                              mPendingFrameTimelines.front().first,
                              mPendingFrameTimelines.front().second.vsyncId);
        mPendingFrameTimelines.pop();
    }

    if (!mPendingFrameTimelines.empty() &&
        mPendingFrameTimelines.front().first == bufferItem.mFrameNumber) {
        ATRACE_FORMAT_INSTANT("Transaction::setFrameTimelineInfo frameNumber: %" PRIu64
                              " vsyncId: %" PRId64,
                              bufferItem.mFrameNumber,
                              mPendingFrameTimelines.front().second.vsyncId);
        t->setFrameTimelineInfo(mPendingFrameTimelines.front().second);
        mPendingFrameTimelines.pop();
    }

    mergePendingTransactions(t, bufferItem.mFrameNumber);
    if (applyTransaction) {
        // All transactions on our apply token are one-way. See comment on mAppliedLastTransaction
        t->setApplyToken(mApplyToken).apply(false, true);
        mAppliedLastTransaction = true;
        mLastAppliedFrameNumber = bufferItem.mFrameNumber;
    } else {
        t->setBufferHasBarrier(mSurfaceControl, mLastAppliedFrameNumber);
        mAppliedLastTransaction = false;
    }

    BQA_LOGV("acquireNextBufferLocked size=%dx%d mFrameNumber=%" PRIu64
             " applyTransaction=%s mTimestamp=%" PRId64 "%s mPendingTransactions.size=%d"
             " graphicBufferId=%" PRIu64 "%s transform=%d",
             mSize.width, mSize.height, bufferItem.mFrameNumber, boolToString(applyTransaction),
             bufferItem.mTimestamp, bufferItem.mIsAutoTimestamp ? "(auto)" : "",
             static_cast<uint32_t>(mPendingTransactions.size()), bufferItem.mGraphicBuffer->getId(),
             bufferItem.mAutoRefresh ? " mAutoRefresh" : "", bufferItem.mTransform);
    return OK;
}

Rect BLASTBufferQueue::computeCrop(const BufferItem& item) {
    if (item.mScalingMode == NATIVE_WINDOW_SCALING_MODE_SCALE_CROP) {
        return GLConsumer::scaleDownCrop(item.mCrop, mSize.width, mSize.height);
    }
    return item.mCrop;
}

void BLASTBufferQueue::acquireAndReleaseBuffer() {
    BBQ_TRACE();
    BufferItem bufferItem;
    status_t status =
            mBufferItemConsumer->acquireBuffer(&bufferItem, 0 /* expectedPresent */, false);
    if (status != OK) {
        BQA_LOGE("Failed to acquire a buffer in acquireAndReleaseBuffer, err=%s",
                 statusToString(status).c_str());
        return;
    }
    mNumFrameAvailable--;
    mBufferItemConsumer->releaseBuffer(bufferItem, bufferItem.mFence);
}

void BLASTBufferQueue::onFrameAvailable(const BufferItem& item) {
    std::function<void(SurfaceComposerClient::Transaction*)> prevCallback = nullptr;
    SurfaceComposerClient::Transaction* prevTransaction = nullptr;

    {
        UNIQUE_LOCK_WITH_ASSERTION(mMutex);
        BBQ_TRACE();
        bool waitForTransactionCallback = !mSyncedFrameNumbers.empty();

        const bool syncTransactionSet = mTransactionReadyCallback != nullptr;
        BQA_LOGV("onFrameAvailable-start syncTransactionSet=%s", boolToString(syncTransactionSet));

        if (syncTransactionSet) {
            // If we are going to re-use the same mSyncTransaction, release the buffer that may
            // already be set in the Transaction. This is to allow us a free slot early to continue
            // processing a new buffer.
            if (!mAcquireSingleBuffer) {
                auto bufferData = mSyncTransaction->getAndClearBuffer(mSurfaceControl);
                if (bufferData) {
                    BQA_LOGD("Releasing previous buffer when syncing: framenumber=%" PRIu64,
                             bufferData->frameNumber);
                    releaseBuffer(bufferData->generateReleaseCallbackId(),
                                  bufferData->acquireFence);
                }
            }

            if (waitForTransactionCallback) {
                // We are waiting on a previous sync's transaction callback so allow another sync
                // transaction to proceed.
                //
                // We need to first flush out the transactions that were in between the two syncs.
                // We do this by merging them into mSyncTransaction so any buffer merging will get
                // a release callback invoked.
                while (mNumFrameAvailable > 0) {
                    // flush out the shadow queue
                    acquireAndReleaseBuffer();
                }
            } else {
                // Make sure the frame available count is 0 before proceeding with a sync to ensure
                // the correct frame is used for the sync. The only way mNumFrameAvailable would be
                // greater than 0 is if we already ran out of buffers previously. This means we
                // need to flush the buffers before proceeding with the sync.
                while (mNumFrameAvailable > 0) {
                    BQA_LOGD("waiting until no queued buffers");
                    mCallbackCV.wait(_lock);
                }
            }
        }

        // add to shadow queue
        mNumFrameAvailable++;
        if (waitForTransactionCallback && mNumFrameAvailable >= 2) {
            acquireAndReleaseBuffer();
        }
        ATRACE_INT(mQueuedBufferTrace.c_str(),
                   mNumFrameAvailable + mNumAcquired - mPendingRelease.size());

        BQA_LOGV("onFrameAvailable framenumber=%" PRIu64 " syncTransactionSet=%s",
                 item.mFrameNumber, boolToString(syncTransactionSet));

        if (syncTransactionSet) {
            // Add to mSyncedFrameNumbers before waiting in case any buffers are released
            // while waiting for a free buffer. The release and commit callback will try to
            // acquire buffers if there are any available, but we don't want it to acquire
            // in the case where a sync transaction wants the buffer.
            mSyncedFrameNumbers.emplace(item.mFrameNumber);
            // If there's no available buffer and we're in a sync transaction, we need to wait
            // instead of returning since we guarantee a buffer will be acquired for the sync.
            while (acquireNextBufferLocked(mSyncTransaction) == BufferQueue::NO_BUFFER_AVAILABLE) {
                BQA_LOGD("waiting for available buffer");
                mCallbackCV.wait(_lock);
            }

            // Only need a commit callback when syncing to ensure the buffer that's synced has been
            // sent to SF
            mSyncTransaction
                    ->addTransactionCommittedCallback(makeTransactionCommittedCallbackThunk(),
                                                      nullptr);
            if (mAcquireSingleBuffer) {
                prevCallback = mTransactionReadyCallback;
                prevTransaction = mSyncTransaction;
                mTransactionReadyCallback = nullptr;
                mSyncTransaction = nullptr;
            }
        } else if (!waitForTransactionCallback) {
            acquireNextBufferLocked(std::nullopt);
        }
    }
    if (prevCallback) {
        prevCallback(prevTransaction);
    }
}

void BLASTBufferQueue::onFrameReplaced(const BufferItem& item) {
    BQA_LOGV("onFrameReplaced framenumber=%" PRIu64, item.mFrameNumber);
    // Do nothing since we are not storing unacquired buffer items locally.
}

void BLASTBufferQueue::onFrameDequeued(const uint64_t bufferId) {
    std::lock_guard _lock{mTimestampMutex};
    mDequeueTimestamps.emplace_or_replace(bufferId, systemTime());
};

void BLASTBufferQueue::onFrameCancelled(const uint64_t bufferId) {
    std::lock_guard _lock{mTimestampMutex};
    mDequeueTimestamps.erase(bufferId);
}

bool BLASTBufferQueue::syncNextTransaction(
        std::function<void(SurfaceComposerClient::Transaction*)> callback,
        bool acquireSingleBuffer) {
    LOG_ALWAYS_FATAL_IF(!callback,
                        "BLASTBufferQueue: callback passed in to syncNextTransaction must not be "
                        "NULL");

    std::lock_guard _lock{mMutex};
    BBQ_TRACE();
    if (mTransactionReadyCallback) {
        ALOGW("Attempting to overwrite transaction callback in syncNextTransaction");
        return false;
    }

    mTransactionReadyCallback = callback;
    mSyncTransaction = new SurfaceComposerClient::Transaction();
    mAcquireSingleBuffer = acquireSingleBuffer;
    return true;
}

void BLASTBufferQueue::stopContinuousSyncTransaction() {
    std::function<void(SurfaceComposerClient::Transaction*)> prevCallback = nullptr;
    SurfaceComposerClient::Transaction* prevTransaction = nullptr;
    {
        std::lock_guard _lock{mMutex};
        if (mAcquireSingleBuffer || !mTransactionReadyCallback) {
            ALOGW("Attempting to stop continuous sync when none are active");
            return;
        }

        prevCallback = mTransactionReadyCallback;
        prevTransaction = mSyncTransaction;

        mTransactionReadyCallback = nullptr;
        mSyncTransaction = nullptr;
        mAcquireSingleBuffer = true;
    }

    if (prevCallback) {
        prevCallback(prevTransaction);
    }
}

void BLASTBufferQueue::clearSyncTransaction() {
    std::lock_guard _lock{mMutex};
    if (!mAcquireSingleBuffer) {
        ALOGW("Attempting to clear sync transaction when none are active");
        return;
    }

    mTransactionReadyCallback = nullptr;
    mSyncTransaction = nullptr;
}

bool BLASTBufferQueue::rejectBuffer(const BufferItem& item) {
    if (item.mScalingMode != NATIVE_WINDOW_SCALING_MODE_FREEZE) {
        // Only reject buffers if scaling mode is freeze.
        return false;
    }

    ui::Size bufferSize = getBufferSize(item);
    if (mRequestedSize != mSize && mRequestedSize == bufferSize) {
        return false;
    }

    // reject buffers if the buffer size doesn't match.
    return mSize != bufferSize;
}

class BBQSurface : public Surface {
private:
    std::mutex mMutex;
    sp<BLASTBufferQueue> mBbq GUARDED_BY(mMutex);
    bool mDestroyed GUARDED_BY(mMutex) = false;

public:
    BBQSurface(const sp<IGraphicBufferProducer>& igbp, bool controlledByApp,
               const sp<IBinder>& scHandle, const sp<BLASTBufferQueue>& bbq)
          : Surface(igbp, controlledByApp, scHandle), mBbq(bbq) {}

    void allocateBuffers() override {
        ATRACE_CALL();
        uint32_t reqWidth = mReqWidth ? mReqWidth : mUserWidth;
        uint32_t reqHeight = mReqHeight ? mReqHeight : mUserHeight;
        auto gbp = getIGraphicBufferProducer();
        std::thread allocateThread([reqWidth, reqHeight, gbp = getIGraphicBufferProducer(),
                                    reqFormat = mReqFormat, reqUsage = mReqUsage]() {
            if (com_android_graphics_libgui_flags_allocate_buffer_priority()) {
                androidSetThreadName("allocateBuffers");
                pid_t tid = gettid();
                androidSetThreadPriority(tid, ANDROID_PRIORITY_DISPLAY);
            }

            gbp->allocateBuffers(reqWidth, reqHeight,
                                 reqFormat, reqUsage);
        });
        allocateThread.detach();
    }

    status_t setFrameRate(float frameRate, int8_t compatibility,
                          int8_t changeFrameRateStrategy) override {
        if (flags::bq_setframerate()) {
            return Surface::setFrameRate(frameRate, compatibility, changeFrameRateStrategy);
        }

        std::lock_guard _lock{mMutex};
        if (mDestroyed) {
            return DEAD_OBJECT;
        }
        if (!ValidateFrameRate(frameRate, compatibility, changeFrameRateStrategy,
                               "BBQSurface::setFrameRate")) {
            return BAD_VALUE;
        }
        return mBbq->setFrameRate(frameRate, compatibility, changeFrameRateStrategy);
    }

    status_t setFrameTimelineInfo(uint64_t frameNumber,
                                  const FrameTimelineInfo& frameTimelineInfo) override {
        std::lock_guard _lock{mMutex};
        if (mDestroyed) {
            return DEAD_OBJECT;
        }
        return mBbq->setFrameTimelineInfo(frameNumber, frameTimelineInfo);
    }

    void destroy() override {
        Surface::destroy();

        std::lock_guard _lock{mMutex};
        mDestroyed = true;
        mBbq = nullptr;
    }
};

// TODO: Can we coalesce this with frame updates? Need to confirm
// no timing issues.
status_t BLASTBufferQueue::setFrameRate(float frameRate, int8_t compatibility,
                                        bool shouldBeSeamless) {
    std::lock_guard _lock{mMutex};
    SurfaceComposerClient::Transaction t;

    return t.setFrameRate(mSurfaceControl, frameRate, compatibility, shouldBeSeamless).apply();
}

status_t BLASTBufferQueue::setFrameTimelineInfo(uint64_t frameNumber,
                                                const FrameTimelineInfo& frameTimelineInfo) {
    ATRACE_FORMAT("%s(%s) frameNumber: %" PRIu64 " vsyncId: %" PRId64, __func__, mName.c_str(),
                  frameNumber, frameTimelineInfo.vsyncId);
    std::lock_guard _lock{mMutex};
    mPendingFrameTimelines.push({frameNumber, frameTimelineInfo});
    return OK;
}

void BLASTBufferQueue::setSidebandStream(const sp<NativeHandle>& stream) {
    std::lock_guard _lock{mMutex};
    SurfaceComposerClient::Transaction t;

    t.setSidebandStream(mSurfaceControl, stream).apply();
}

sp<Surface> BLASTBufferQueue::getSurface(bool includeSurfaceControlHandle) {
    std::lock_guard _lock{mMutex};
    sp<IBinder> scHandle = nullptr;
    if (includeSurfaceControlHandle && mSurfaceControl) {
        scHandle = mSurfaceControl->getHandle();
    }
    return sp<BBQSurface>::make(mProducer, true, scHandle, this);
}

void BLASTBufferQueue::mergeWithNextTransaction(SurfaceComposerClient::Transaction* t,
                                                uint64_t frameNumber) {
    std::lock_guard _lock{mMutex};
    if (mLastAcquiredFrameNumber >= frameNumber) {
        // Apply the transaction since we have already acquired the desired frame.
        t->setApplyToken(mApplyToken).apply();
    } else {
        mPendingTransactions.emplace_back(frameNumber, std::move(*t));
        // Clear the transaction so it can't be applied elsewhere.
        t->clear();
    }
}

void BLASTBufferQueue::applyPendingTransactions(uint64_t frameNumber) {
    std::lock_guard _lock{mMutex};

    SurfaceComposerClient::Transaction t;
    mergePendingTransactions(&t, frameNumber);
    // All transactions on our apply token are one-way. See comment on mAppliedLastTransaction
    t.setApplyToken(mApplyToken).apply(false, true);
}

void BLASTBufferQueue::mergePendingTransactions(SurfaceComposerClient::Transaction* t,
                                                uint64_t frameNumber) {
    auto mergeTransaction =
            [t, currentFrameNumber = frameNumber](
                    std::pair<uint64_t, SurfaceComposerClient::Transaction>& pendingTransaction) {
                auto& [targetFrameNumber, transaction] = pendingTransaction;
                if (currentFrameNumber < targetFrameNumber) {
                    return false;
                }
                t->merge(std::move(transaction));
                return true;
            };

    mPendingTransactions.erase(std::remove_if(mPendingTransactions.begin(),
                                              mPendingTransactions.end(), mergeTransaction),
                               mPendingTransactions.end());
}

SurfaceComposerClient::Transaction* BLASTBufferQueue::gatherPendingTransactions(
        uint64_t frameNumber) {
    std::lock_guard _lock{mMutex};
    SurfaceComposerClient::Transaction* t = new SurfaceComposerClient::Transaction();
    mergePendingTransactions(t, frameNumber);
    return t;
}

// Maintains a single worker thread per process that services a list of runnables.
class AsyncWorker : public Singleton<AsyncWorker> {
private:
    std::thread mThread;
    bool mDone = false;
    std::deque<std::function<void()>> mRunnables;
    std::mutex mMutex;
    std::condition_variable mCv;
    void run() {
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mDone) {
            while (!mRunnables.empty()) {
                std::deque<std::function<void()>> runnables = std::move(mRunnables);
                mRunnables.clear();
                lock.unlock();
                // Run outside the lock since the runnable might trigger another
                // post to the async worker.
                execute(runnables);
                lock.lock();
            }
            mCv.wait(lock);
        }
    }

    void execute(std::deque<std::function<void()>>& runnables) {
        while (!runnables.empty()) {
            std::function<void()> runnable = runnables.front();
            runnables.pop_front();
            runnable();
        }
    }

public:
    AsyncWorker() : Singleton<AsyncWorker>() { mThread = std::thread(&AsyncWorker::run, this); }

    ~AsyncWorker() {
        mDone = true;
        mCv.notify_all();
        if (mThread.joinable()) {
            mThread.join();
        }
    }

    void post(std::function<void()> runnable) {
        std::unique_lock<std::mutex> lock(mMutex);
        mRunnables.emplace_back(std::move(runnable));
        mCv.notify_one();
    }
};
ANDROID_SINGLETON_STATIC_INSTANCE(AsyncWorker);

// Asynchronously calls ProducerListener functions so we can emulate one way binder calls.
class AsyncProducerListener : public BnProducerListener {
private:
    const sp<IProducerListener> mListener;
    AsyncProducerListener(const sp<IProducerListener>& listener) : mListener(listener) {}
    friend class sp<AsyncProducerListener>;

public:
    void onBufferReleased() override {
        AsyncWorker::getInstance().post([listener = mListener]() { listener->onBufferReleased(); });
    }

    void onBuffersDiscarded(const std::vector<int32_t>& slots) override {
        AsyncWorker::getInstance().post(
                [listener = mListener, slots = slots]() { listener->onBuffersDiscarded(slots); });
    }

    void onBufferDetached(int slot) override {
        AsyncWorker::getInstance().post(
                [listener = mListener, slot = slot]() { listener->onBufferDetached(slot); });
    }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_CONSUMER_ATTACH_CALLBACK)
    void onBufferAttached() override {
        AsyncWorker::getInstance().post([listener = mListener]() { listener->onBufferAttached(); });
    }
#endif
};

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
class BBQBufferQueueCore : public BufferQueueCore {
public:
    explicit BBQBufferQueueCore(const wp<BLASTBufferQueue>& bbq) : mBLASTBufferQueue{bbq} {}

    void notifyBufferReleased() const override {
        sp<BLASTBufferQueue> bbq = mBLASTBufferQueue.promote();
        if (!bbq) {
            return;
        }
        bbq->mBufferReleaseReader->interruptBlockingRead();
    }

private:
    wp<BLASTBufferQueue> mBLASTBufferQueue;
};
#endif

// Extends the BufferQueueProducer to create a wrapper around the listener so the listener calls
// can be non-blocking when the producer is in the client process.
class BBQBufferQueueProducer : public BufferQueueProducer {
public:
    BBQBufferQueueProducer(const sp<BufferQueueCore>& core, const wp<BLASTBufferQueue>& bbq)
          : BufferQueueProducer(core, false /* consumerIsSurfaceFlinger*/),
            mBLASTBufferQueue(bbq) {}

    status_t connect(const sp<IProducerListener>& listener, int api, bool producerControlledByApp,
                     QueueBufferOutput* output) override {
        if (!listener) {
            return BufferQueueProducer::connect(listener, api, producerControlledByApp, output);
        }

        return BufferQueueProducer::connect(sp<AsyncProducerListener>::make(listener), api,
                                            producerControlledByApp, output);
    }

    // We want to resize the frame history when changing the size of the buffer queue
    status_t setMaxDequeuedBufferCount(int maxDequeuedBufferCount) override {
        int maxBufferCount;
        if (status_t status = BufferQueueProducer::setMaxDequeuedBufferCount(maxDequeuedBufferCount,
                                                                             &maxBufferCount);
            status != OK) {
            return status;
        }

        sp<BLASTBufferQueue> bbq = mBLASTBufferQueue.promote();
        if (!bbq) {
            return OK;
        }

        // if we can't determine the max buffer count, then just skip growing the history size
        size_t newFrameHistorySize = maxBufferCount + 2; // +2 because triple buffer rendering
        // optimize away resizing the frame history unless it will grow
        if (newFrameHistorySize > FrameEventHistory::INITIAL_MAX_FRAME_HISTORY) {
            ALOGV("increasing frame history size to %zu", newFrameHistorySize);
            bbq->resizeFrameEventHistory(newFrameHistorySize);
        }

        return OK;
    }

    int query(int what, int* value) override {
        if (what == NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER) {
            *value = 1;
            return OK;
        }
        return BufferQueueProducer::query(what, value);
    }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
    status_t waitForBufferRelease(std::unique_lock<std::mutex>& bufferQueueLock,
                                  nsecs_t timeout) const override {
        const auto startTime = std::chrono::steady_clock::now();
        sp<BLASTBufferQueue> bbq = mBLASTBufferQueue.promote();
        if (!bbq) {
            return OK;
        }

        // BufferQueue has already checked if we have a free buffer. If there's an unread interrupt,
        // we want to ignore it. This must be done before unlocking the BufferQueue lock to ensure
        // we don't miss an interrupt.
        bbq->mBufferReleaseReader->clearInterrupts();
        UnlockGuard unlockGuard{bufferQueueLock};

        ATRACE_FORMAT("waiting for free buffer");
        ReleaseCallbackId id;
        sp<Fence> fence;
        uint32_t maxAcquiredBufferCount;
        status_t status =
                bbq->mBufferReleaseReader->readBlocking(id, fence, maxAcquiredBufferCount, timeout);
        if (status == TIMED_OUT) {
            return TIMED_OUT;
        } else if (status != OK) {
            // Waiting was interrupted or an error occurred. BufferQueueProducer will check if we
            // have a free buffer and call this method again if not.
            return OK;
        }

        bbq->releaseBufferCallback(id, fence, maxAcquiredBufferCount);
        const nsecs_t durationNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - startTime)
                                              .count();
        // Provide a callback for Choreographer to start buffer stuffing recovery when blocked
        // on buffer release.
        std::function<void(const nsecs_t)> callbackCopy = bbq->getWaitForBufferReleaseCallback();
        if (callbackCopy) callbackCopy(durationNanos);

        return OK;
    }
#endif

private:
    const wp<BLASTBufferQueue> mBLASTBufferQueue;
};

// Similar to BufferQueue::createBufferQueue but creates an adapter specific bufferqueue producer.
// This BQP allows invoking client specified ProducerListeners and invoke them asynchronously,
// emulating one way binder call behavior. Without this, if the listener calls back into the queue,
// we can deadlock.
void BLASTBufferQueue::createBufferQueue(sp<IGraphicBufferProducer>* outProducer,
                                         sp<IGraphicBufferConsumer>* outConsumer) {
    LOG_ALWAYS_FATAL_IF(outProducer == nullptr, "BLASTBufferQueue: outProducer must not be NULL");
    LOG_ALWAYS_FATAL_IF(outConsumer == nullptr, "BLASTBufferQueue: outConsumer must not be NULL");

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
    auto core = sp<BBQBufferQueueCore>::make(this);
#else
    auto core = sp<BufferQueueCore>::make();
#endif
    LOG_ALWAYS_FATAL_IF(core == nullptr, "BLASTBufferQueue: failed to create BufferQueueCore");

    auto producer = sp<BBQBufferQueueProducer>::make(core, this);
    LOG_ALWAYS_FATAL_IF(producer == nullptr,
                        "BLASTBufferQueue: failed to create BBQBufferQueueProducer");

    auto consumer = sp<BufferQueueConsumer>::make(core);
    consumer->setAllowExtraAcquire(true);
    LOG_ALWAYS_FATAL_IF(consumer == nullptr,
                        "BLASTBufferQueue: failed to create BufferQueueConsumer");

    *outProducer = producer;
    *outConsumer = consumer;
}

void BLASTBufferQueue::resizeFrameEventHistory(size_t newSize) {
    // This can be null during creation of the buffer queue, but resizing won't do anything at that
    // point in time, so just ignore. This can go away once the class relationships and lifetimes of
    // objects are cleaned up with a major refactor of BufferQueue as a whole.
    if (mBufferItemConsumer != nullptr) {
        std::unique_lock _lock{mMutex};
        mBufferItemConsumer->resizeFrameEventHistory(newSize);
    }
}

PixelFormat BLASTBufferQueue::convertBufferFormat(PixelFormat& format) {
    PixelFormat convertedFormat = format;
    switch (format) {
        case PIXEL_FORMAT_TRANSPARENT:
        case PIXEL_FORMAT_TRANSLUCENT:
            convertedFormat = PIXEL_FORMAT_RGBA_8888;
            break;
        case PIXEL_FORMAT_OPAQUE:
            convertedFormat = PIXEL_FORMAT_RGBX_8888;
            break;
    }
    return convertedFormat;
}

uint32_t BLASTBufferQueue::getLastTransformHint() const {
    std::lock_guard _lock{mMutex};
    if (mSurfaceControl != nullptr) {
        return mSurfaceControl->getTransformHint();
    } else {
        return 0;
    }
}

uint64_t BLASTBufferQueue::getLastAcquiredFrameNum() {
    std::lock_guard _lock{mMutex};
    return mLastAcquiredFrameNumber;
}

bool BLASTBufferQueue::isSameSurfaceControl(const sp<SurfaceControl>& surfaceControl) const {
    std::lock_guard _lock{mMutex};
    return SurfaceControl::isSameSurface(mSurfaceControl, surfaceControl);
}

void BLASTBufferQueue::setTransactionHangCallback(
        std::function<void(const std::string&)> callback) {
    std::lock_guard _lock{mMutex};
    mTransactionHangCallback = std::move(callback);
}

void BLASTBufferQueue::setApplyToken(sp<IBinder> applyToken) {
    std::lock_guard _lock{mMutex};
    mApplyToken = std::move(applyToken);
}

void BLASTBufferQueue::setWaitForBufferReleaseCallback(
        std::function<void(const nsecs_t)> callback) {
    std::lock_guard _lock{mWaitForBufferReleaseMutex};
    mWaitForBufferReleaseCallback = std::move(callback);
}

std::function<void(const nsecs_t)> BLASTBufferQueue::getWaitForBufferReleaseCallback() const {
    std::lock_guard _lock{mWaitForBufferReleaseMutex};
    return mWaitForBufferReleaseCallback;
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)

void BLASTBufferQueue::updateBufferReleaseProducer() {
    // SELinux policy may prevent this process from sending the BufferReleaseChannel's file
    // descriptor to SurfaceFlinger, causing the entire transaction to be dropped. We send this
    // transaction independently of any other updates to ensure those updates aren't lost.
    SurfaceComposerClient::Transaction t;
    status_t status = t.setApplyToken(mApplyToken)
                              .setBufferReleaseChannel(mSurfaceControl, mBufferReleaseProducer)
                              .apply(false /* synchronous */, true /* oneWay */);
    if (status != OK) {
        ALOGW("[%s] %s - failed to set buffer release channel on %s", mName.c_str(),
              statusToString(status).c_str(), mSurfaceControl->getName().c_str());
    }
}

void BLASTBufferQueue::drainBufferReleaseConsumer() {
    ATRACE_CALL();
    while (true) {
        ReleaseCallbackId id;
        sp<Fence> fence;
        uint32_t maxAcquiredBufferCount;
        status_t status =
                mBufferReleaseConsumer->readReleaseFence(id, fence, maxAcquiredBufferCount);
        if (status != OK) {
            return;
        }
        releaseBufferCallback(id, fence, maxAcquiredBufferCount);
    }
}

BLASTBufferQueue::BufferReleaseReader::BufferReleaseReader(BLASTBufferQueue& bbq) : mBbq{bbq} {
    mEpollFd = android::base::unique_fd{epoll_create1(EPOLL_CLOEXEC)};
    LOG_ALWAYS_FATAL_IF(!mEpollFd.ok(),
                        "Failed to create buffer release epoll file descriptor. errno=%d "
                        "message='%s'",
                        errno, strerror(errno));

    epoll_event registerEndpointFd{};
    registerEndpointFd.events = EPOLLIN;
    registerEndpointFd.data.fd = mBbq.mBufferReleaseConsumer->getFd();
    status_t status = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, mBbq.mBufferReleaseConsumer->getFd(),
                                &registerEndpointFd);
    LOG_ALWAYS_FATAL_IF(status == -1,
                        "Failed to register buffer release consumer file descriptor with epoll. "
                        "errno=%d message='%s'",
                        errno, strerror(errno));

    mEventFd = android::base::unique_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    LOG_ALWAYS_FATAL_IF(!mEventFd.ok(),
                        "Failed to create buffer release event file descriptor. errno=%d "
                        "message='%s'",
                        errno, strerror(errno));

    epoll_event registerEventFd{};
    registerEventFd.events = EPOLLIN;
    registerEventFd.data.fd = mEventFd.get();
    status = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, mEventFd.get(), &registerEventFd);
    LOG_ALWAYS_FATAL_IF(status == -1,
                        "Failed to register buffer release event file descriptor with epoll. "
                        "errno=%d message='%s'",
                        errno, strerror(errno));
}

status_t BLASTBufferQueue::BufferReleaseReader::readBlocking(ReleaseCallbackId& outId,
                                                             sp<Fence>& outFence,
                                                             uint32_t& outMaxAcquiredBufferCount,
                                                             nsecs_t timeout) {
    // TODO(b/363290953) epoll_wait only has millisecond timeout precision. If timeout is less than
    // 1ms, then we round timeout up to 1ms. Otherwise, we round timeout to the nearest
    // millisecond. Once epoll_pwait2 can be used in libgui, we can specify timeout with nanosecond
    // precision.
    int timeoutMs = -1;
    if (timeout == 0) {
        timeoutMs = 0;
    } else if (timeout > 0) {
        const int nsPerMs = 1000000;
        if (timeout < nsPerMs) {
            timeoutMs = 1;
        } else {
            timeoutMs = static_cast<int>(
                    std::chrono::round<std::chrono::milliseconds>(std::chrono::nanoseconds{timeout})
                            .count());
        }
    }

    epoll_event event{};
    int eventCount;
    do {
        eventCount = epoll_wait(mEpollFd.get(), &event, 1 /*maxevents*/, timeoutMs);
    } while (eventCount == -1 && errno != EINTR);

    if (eventCount == -1) {
        ALOGE("epoll_wait error while waiting for buffer release. errno=%d message='%s'", errno,
              strerror(errno));
        return UNKNOWN_ERROR;
    }

    if (eventCount == 0) {
        return TIMED_OUT;
    }

    if (event.data.fd == mEventFd.get()) {
        clearInterrupts();
        return WOULD_BLOCK;
    }

    return mBbq.mBufferReleaseConsumer->readReleaseFence(outId, outFence,
                                                         outMaxAcquiredBufferCount);
}

void BLASTBufferQueue::BufferReleaseReader::interruptBlockingRead() {
    if (eventfd_write(mEventFd.get(), 1) == -1) {
        ALOGE("failed to notify dequeue event. errno=%d message='%s'", errno, strerror(errno));
    }
}

void BLASTBufferQueue::BufferReleaseReader::clearInterrupts() {
    eventfd_t value;
    if (eventfd_read(mEventFd.get(), &value) == -1 && errno != EWOULDBLOCK) {
        ALOGE("error while reading from eventfd. errno=%d message='%s'", errno, strerror(errno));
    }
}

#endif

} // namespace android
