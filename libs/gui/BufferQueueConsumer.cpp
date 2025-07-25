/*
 * Copyright 2014 The Android Open Source Project
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

#define LOG_TAG "BufferQueueConsumer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#if DEBUG_ONLY_CODE
#define VALIDATE_CONSISTENCY() do { mCore->validateConsistencyLocked(); } while (0)
#else
#define VALIDATE_CONSISTENCY()
#endif

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <gui/BufferItem.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/IConsumerListener.h>
#include <gui/IProducerListener.h>
#include <gui/TraceUtils.h>

#include <private/gui/BufferQueueThreadState.h>
#if !defined(__ANDROID_VNDK__) && !defined(NO_BINDER)
#include <binder/PermissionCache.h>
#endif

#include <system/window.h>

#include <com_android_graphics_libgui_flags.h>

#include <inttypes.h>
#include <pwd.h>
#include <sys/types.h>
#include <optional>

namespace android {

// Macros for include BufferQueueCore information in log messages
#define BQ_LOGV(x, ...)                                                                           \
    ALOGV("[%s](id:%" PRIx64 ",api:%d,p:%d,c:%" PRIu64 ") " x, mConsumerName.c_str(),             \
          mCore->mUniqueId, mCore->mConnectedApi, mCore->mConnectedPid, (mCore->mUniqueId) >> 32, \
          ##__VA_ARGS__)
#define BQ_LOGD(x, ...)                                                                           \
    ALOGD("[%s](id:%" PRIx64 ",api:%d,p:%d,c:%" PRIu64 ") " x, mConsumerName.c_str(),             \
          mCore->mUniqueId, mCore->mConnectedApi, mCore->mConnectedPid, (mCore->mUniqueId) >> 32, \
          ##__VA_ARGS__)
#define BQ_LOGI(x, ...)                                                                           \
    ALOGI("[%s](id:%" PRIx64 ",api:%d,p:%d,c:%" PRIu64 ") " x, mConsumerName.c_str(),             \
          mCore->mUniqueId, mCore->mConnectedApi, mCore->mConnectedPid, (mCore->mUniqueId) >> 32, \
          ##__VA_ARGS__)
#define BQ_LOGW(x, ...)                                                                           \
    ALOGW("[%s](id:%" PRIx64 ",api:%d,p:%d,c:%" PRIu64 ") " x, mConsumerName.c_str(),             \
          mCore->mUniqueId, mCore->mConnectedApi, mCore->mConnectedPid, (mCore->mUniqueId) >> 32, \
          ##__VA_ARGS__)
#define BQ_LOGE(x, ...)                                                                           \
    ALOGE("[%s](id:%" PRIx64 ",api:%d,p:%d,c:%" PRIu64 ") " x, mConsumerName.c_str(),             \
          mCore->mUniqueId, mCore->mConnectedApi, mCore->mConnectedPid, (mCore->mUniqueId) >> 32, \
          ##__VA_ARGS__)

ConsumerListener::~ConsumerListener() = default;

BufferQueueConsumer::BufferQueueConsumer(const sp<BufferQueueCore>& core) :
    mCore(core),
    mSlots(core->mSlots),
    mConsumerName() {}

BufferQueueConsumer::~BufferQueueConsumer() {}

status_t BufferQueueConsumer::acquireBuffer(BufferItem* outBuffer,
        nsecs_t expectedPresent, uint64_t maxFrameNumber) {
    ATRACE_CALL();

    int numDroppedBuffers = 0;
    sp<IProducerListener> listener;
    {
        std::unique_lock<std::mutex> lock(mCore->mMutex);

        // Check that the consumer doesn't currently have the maximum number of
        // buffers acquired. We allow the max buffer count to be exceeded by one
        // buffer so that the consumer can successfully set up the newly acquired
        // buffer before releasing the old one.
        int numAcquiredBuffers = 0;
        for (int s : mCore->mActiveBuffers) {
            if (mSlots[s].mBufferState.isAcquired()) {
                ++numAcquiredBuffers;
            }
        }
        const bool acquireNonDroppableBuffer = mCore->mAllowExtraAcquire &&
                numAcquiredBuffers == mCore->mMaxAcquiredBufferCount + 1;
        if (numAcquiredBuffers >= mCore->mMaxAcquiredBufferCount + 1 &&
            !acquireNonDroppableBuffer) {
            BQ_LOGE("acquireBuffer: max acquired buffer count reached: %d (max %d)",
                    numAcquiredBuffers, mCore->mMaxAcquiredBufferCount);
            return INVALID_OPERATION;
        }

        bool sharedBufferAvailable = mCore->mSharedBufferMode &&
                mCore->mAutoRefresh && mCore->mSharedBufferSlot !=
                BufferQueueCore::INVALID_BUFFER_SLOT;

        // In asynchronous mode the list is guaranteed to be one buffer deep,
        // while in synchronous mode we use the oldest buffer.
        if (mCore->mQueue.empty() && !sharedBufferAvailable) {
            return NO_BUFFER_AVAILABLE;
        }

        BufferQueueCore::Fifo::iterator front(mCore->mQueue.begin());

        // If expectedPresent is specified, we may not want to return a buffer yet.
        // If it's specified and there's more than one buffer queued, we may want
        // to drop a buffer.
        // Skip this if we're in shared buffer mode and the queue is empty,
        // since in that case we'll just return the shared buffer.
        if (expectedPresent != 0 && !mCore->mQueue.empty()) {
            // The 'expectedPresent' argument indicates when the buffer is expected
            // to be presented on-screen. If the buffer's desired present time is
            // earlier (less) than expectedPresent -- meaning it will be displayed
            // on time or possibly late if we show it as soon as possible -- we
            // acquire and return it. If we don't want to display it until after the
            // expectedPresent time, we return PRESENT_LATER without acquiring it.
            //
            // To be safe, we don't defer acquisition if expectedPresent is more
            // than one second in the future beyond the desired present time
            // (i.e., we'd be holding the buffer for a long time).
            //
            // NOTE: Code assumes monotonic time values from the system clock
            // are positive.

            // Start by checking to see if we can drop frames. We skip this check if
            // the timestamps are being auto-generated by Surface. If the app isn't
            // generating timestamps explicitly, it probably doesn't want frames to
            // be discarded based on them.
            while (mCore->mQueue.size() > 1 && !mCore->mQueue[0].mIsAutoTimestamp) {
                const BufferItem& bufferItem(mCore->mQueue[1]);

                // If dropping entry[0] would leave us with a buffer that the
                // consumer is not yet ready for, don't drop it.
                if (maxFrameNumber && bufferItem.mFrameNumber > maxFrameNumber) {
                    break;
                }

                // If entry[1] is timely, drop entry[0] (and repeat). We apply an
                // additional criterion here: we only drop the earlier buffer if our
                // desiredPresent falls within +/- 1 second of the expected present.
                // Otherwise, bogus desiredPresent times (e.g., 0 or a small
                // relative timestamp), which normally mean "ignore the timestamp
                // and acquire immediately", would cause us to drop frames.
                //
                // We may want to add an additional criterion: don't drop the
                // earlier buffer if entry[1]'s fence hasn't signaled yet.
                nsecs_t desiredPresent = bufferItem.mTimestamp;
                if (desiredPresent < expectedPresent - MAX_REASONABLE_NSEC ||
                        desiredPresent > expectedPresent) {
                    // This buffer is set to display in the near future, or
                    // desiredPresent is garbage. Either way we don't want to drop
                    // the previous buffer just to get this on the screen sooner.
                    BQ_LOGV("acquireBuffer: nodrop desire=%" PRId64 " expect=%"
                            PRId64 " (%" PRId64 ") now=%" PRId64,
                            desiredPresent, expectedPresent,
                            desiredPresent - expectedPresent,
                            systemTime(CLOCK_MONOTONIC));
                    break;
                }

                BQ_LOGV("acquireBuffer: drop desire=%" PRId64 " expect=%" PRId64
                        " size=%zu",
                        desiredPresent, expectedPresent, mCore->mQueue.size());

                if (!front->mIsStale) {
                    // Front buffer is still in mSlots, so mark the slot as free
                    mSlots[front->mSlot].mBufferState.freeQueued();

                    // After leaving shared buffer mode, the shared buffer will
                    // still be around. Mark it as no longer shared if this
                    // operation causes it to be free.
                    if (!mCore->mSharedBufferMode &&
                            mSlots[front->mSlot].mBufferState.isFree()) {
                        mSlots[front->mSlot].mBufferState.mShared = false;
                    }

                    // Don't put the shared buffer on the free list
                    if (!mSlots[front->mSlot].mBufferState.isShared()) {
                        mCore->mActiveBuffers.erase(front->mSlot);
                        mCore->mFreeBuffers.push_back(front->mSlot);
                    }

                    if (mCore->mBufferReleasedCbEnabled) {
                        listener = mCore->mConnectedProducerListener;
                    }
                    ++numDroppedBuffers;
                }

                mCore->mQueue.erase(front);
                front = mCore->mQueue.begin();
            }

            // See if the front buffer is ready to be acquired
            nsecs_t desiredPresent = front->mTimestamp;
            bool bufferIsDue = desiredPresent <= expectedPresent ||
                    desiredPresent > expectedPresent + MAX_REASONABLE_NSEC;
            bool consumerIsReady = maxFrameNumber > 0 ?
                    front->mFrameNumber <= maxFrameNumber : true;
            if (!bufferIsDue || !consumerIsReady) {
                BQ_LOGV("acquireBuffer: defer desire=%" PRId64 " expect=%" PRId64
                        " (%" PRId64 ") now=%" PRId64 " frame=%" PRIu64
                        " consumer=%" PRIu64,
                        desiredPresent, expectedPresent,
                        desiredPresent - expectedPresent,
                        systemTime(CLOCK_MONOTONIC),
                        front->mFrameNumber, maxFrameNumber);
                ATRACE_NAME("PRESENT_LATER");
                return PRESENT_LATER;
            }

            BQ_LOGV("acquireBuffer: accept desire=%" PRId64 " expect=%" PRId64 " "
                    "(%" PRId64 ") now=%" PRId64, desiredPresent, expectedPresent,
                    desiredPresent - expectedPresent,
                    systemTime(CLOCK_MONOTONIC));
        }

        int slot = BufferQueueCore::INVALID_BUFFER_SLOT;

        if (sharedBufferAvailable && mCore->mQueue.empty()) {
            // make sure the buffer has finished allocating before acquiring it
            mCore->waitWhileAllocatingLocked(lock);

            slot = mCore->mSharedBufferSlot;

            // Recreate the BufferItem for the shared buffer from the data that
            // was cached when it was last queued.
            outBuffer->mGraphicBuffer = mSlots[slot].mGraphicBuffer;
            outBuffer->mFence = Fence::NO_FENCE;
            outBuffer->mFenceTime = FenceTime::NO_FENCE;
            outBuffer->mCrop = mCore->mSharedBufferCache.crop;
            outBuffer->mTransform = mCore->mSharedBufferCache.transform &
                    ~static_cast<uint32_t>(
                    NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY);
            outBuffer->mScalingMode = mCore->mSharedBufferCache.scalingMode;
            outBuffer->mDataSpace = mCore->mSharedBufferCache.dataspace;
            outBuffer->mFrameNumber = mCore->mFrameCounter;
            outBuffer->mSlot = slot;
            outBuffer->mAcquireCalled = mSlots[slot].mAcquireCalled;
            outBuffer->mTransformToDisplayInverse =
                    (mCore->mSharedBufferCache.transform &
                    NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY) != 0;
            outBuffer->mSurfaceDamage = Region::INVALID_REGION;
            outBuffer->mQueuedBuffer = false;
            outBuffer->mIsStale = false;
            outBuffer->mAutoRefresh = mCore->mSharedBufferMode &&
                    mCore->mAutoRefresh;
        } else if (acquireNonDroppableBuffer && front->mIsDroppable) {
            BQ_LOGV("acquireBuffer: front buffer is not droppable");
            return NO_BUFFER_AVAILABLE;
        } else {
            slot = front->mSlot;
            *outBuffer = *front;
        }

        ATRACE_BUFFER_INDEX(slot);

        BQ_LOGV("acquireBuffer: acquiring { slot=%d/%" PRIu64 " buffer=%p }",
                slot, outBuffer->mFrameNumber, outBuffer->mGraphicBuffer->handle);

        if (!outBuffer->mIsStale) {
            mSlots[slot].mAcquireCalled = true;
            // Don't decrease the queue count if the BufferItem wasn't
            // previously in the queue. This happens in shared buffer mode when
            // the queue is empty and the BufferItem is created above.
            if (mCore->mQueue.empty()) {
                mSlots[slot].mBufferState.acquireNotInQueue();
            } else {
                mSlots[slot].mBufferState.acquire();
            }
            mSlots[slot].mFence = Fence::NO_FENCE;
        }

        // If the buffer has previously been acquired by the consumer, set
        // mGraphicBuffer to NULL to avoid unnecessarily remapping this buffer
        // on the consumer side
        if (outBuffer->mAcquireCalled) {
            outBuffer->mGraphicBuffer = nullptr;
        }

        mCore->mQueue.erase(front);

        // We might have freed a slot while dropping old buffers, or the producer
        // may be blocked waiting for the number of buffers in the queue to
        // decrease.
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
        mCore->notifyBufferReleased();
#else
        mCore->mDequeueCondition.notify_all();
#endif

        ATRACE_INT(mCore->mConsumerName.c_str(), static_cast<int32_t>(mCore->mQueue.size()));
#ifndef NO_BINDER
        mCore->mOccupancyTracker.registerOccupancyChange(mCore->mQueue.size());
#endif
        VALIDATE_CONSISTENCY();
    }

    if (listener != nullptr) {
        for (int i = 0; i < numDroppedBuffers; ++i) {
            listener->onBufferReleased();
        }
    }

    return NO_ERROR;
}

status_t BufferQueueConsumer::detachBuffer(int slot) {
    ATRACE_CALL();
    ATRACE_BUFFER_INDEX(slot);
    BQ_LOGV("detachBuffer: slot %d", slot);
    sp<IProducerListener> listener;
    {
        std::lock_guard<std::mutex> lock(mCore->mMutex);

        if (mCore->mIsAbandoned) {
            BQ_LOGE("detachBuffer: BufferQueue has been abandoned");
            return NO_INIT;
        }

        if (mCore->mSharedBufferMode || slot == mCore->mSharedBufferSlot) {
            BQ_LOGE("detachBuffer: detachBuffer not allowed in shared buffer mode");
            return BAD_VALUE;
        }

        const int totalSlotCount = mCore->getTotalSlotCountLocked();
        if (slot < 0 || slot >= totalSlotCount) {
            BQ_LOGE("detachBuffer: slot index %d out of range [0, %d)", slot, totalSlotCount);
            return BAD_VALUE;
        } else if (!mSlots[slot].mBufferState.isAcquired()) {
            BQ_LOGE("detachBuffer: slot %d is not owned by the consumer "
                    "(state = %s)", slot, mSlots[slot].mBufferState.string());
            return BAD_VALUE;
        }
        if (mCore->mBufferReleasedCbEnabled) {
            listener = mCore->mConnectedProducerListener;
        }

        mSlots[slot].mBufferState.detachConsumer();
        mCore->mActiveBuffers.erase(slot);
        mCore->mFreeSlots.insert(slot);
        mCore->clearBufferSlotLocked(slot);
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
        mCore->notifyBufferReleased();
#else
        mCore->mDequeueCondition.notify_all();
#endif

        VALIDATE_CONSISTENCY();
    }

    if (listener) {
        listener->onBufferDetached(slot);
    }
    return NO_ERROR;
}

status_t BufferQueueConsumer::attachBuffer(int* outSlot,
        const sp<android::GraphicBuffer>& buffer) {
    ATRACE_CALL();

    if (outSlot == nullptr) {
        BQ_LOGE("attachBuffer: outSlot must not be NULL");
        return BAD_VALUE;
    } else if (buffer == nullptr) {
        BQ_LOGE("attachBuffer: cannot attach NULL buffer");
        return BAD_VALUE;
    }

    sp<IProducerListener> listener;
    {
        std::lock_guard<std::mutex> lock(mCore->mMutex);

        if (mCore->mSharedBufferMode) {
            BQ_LOGE("attachBuffer: cannot attach a buffer in shared buffer mode");
            return BAD_VALUE;
        }

        // Make sure we don't have too many acquired buffers
        int numAcquiredBuffers = 0;
        for (int s : mCore->mActiveBuffers) {
            if (mSlots[s].mBufferState.isAcquired()) {
                ++numAcquiredBuffers;
            }
        }

        if (numAcquiredBuffers >= mCore->mMaxAcquiredBufferCount + 1) {
            BQ_LOGE("attachBuffer: max acquired buffer count reached: %d "
                    "(max %d)", numAcquiredBuffers,
                    mCore->mMaxAcquiredBufferCount);
            return INVALID_OPERATION;
        }

        if (buffer->getGenerationNumber() != mCore->mGenerationNumber) {
            BQ_LOGE("attachBuffer: generation number mismatch [buffer %u] "
                    "[queue %u]", buffer->getGenerationNumber(),
                    mCore->mGenerationNumber);
            return BAD_VALUE;
        }

        // Find a free slot to put the buffer into
        int found = BufferQueueCore::INVALID_BUFFER_SLOT;
        if (!mCore->mFreeSlots.empty()) {
            auto slot = mCore->mFreeSlots.begin();
            found = *slot;
            mCore->mFreeSlots.erase(slot);
        } else if (!mCore->mFreeBuffers.empty()) {
            found = mCore->mFreeBuffers.front();
            mCore->mFreeBuffers.remove(found);
        }
        if (found == BufferQueueCore::INVALID_BUFFER_SLOT) {
            BQ_LOGE("attachBuffer: could not find free buffer slot");
            return NO_MEMORY;
        }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_CONSUMER_ATTACH_CALLBACK)
        if (mCore->mBufferAttachedCbEnabled) {
            listener = mCore->mConnectedProducerListener;
        }
#endif

        mCore->mActiveBuffers.insert(found);
        *outSlot = found;
        ATRACE_BUFFER_INDEX(*outSlot);
        BQ_LOGV("attachBuffer: returning slot %d", *outSlot);

        mSlots[*outSlot].mGraphicBuffer = buffer;
        mSlots[*outSlot].mBufferState.attachConsumer();
        mSlots[*outSlot].mNeedsReallocation = true;
        mSlots[*outSlot].mFence = Fence::NO_FENCE;
        mSlots[*outSlot].mFrameNumber = 0;

        // mAcquireCalled tells BufferQueue that it doesn't need to send a valid
        // GraphicBuffer pointer on the next acquireBuffer call, which decreases
        // Binder traffic by not un/flattening the GraphicBuffer. However, it
        // requires that the consumer maintain a cached copy of the slot <--> buffer
        // mappings, which is why the consumer doesn't need the valid pointer on
        // acquire.
        //
        // The StreamSplitter is one of the primary users of the attach/detach
        // logic, and while it is running, all buffers it acquires are immediately
        // detached, and all buffers it eventually releases are ones that were
        // attached (as opposed to having been obtained from acquireBuffer), so it
        // doesn't make sense to maintain the slot/buffer mappings, which would
        // become invalid for every buffer during detach/attach. By setting this to
        // false, the valid GraphicBuffer pointer will always be sent with acquire
        // for attached buffers.
        mSlots[*outSlot].mAcquireCalled = false;

        VALIDATE_CONSISTENCY();
    }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_CONSUMER_ATTACH_CALLBACK)
    if (listener != nullptr) {
        listener->onBufferAttached();
    }
#endif

    return NO_ERROR;
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_GL_FENCE_CLEANUP)
status_t BufferQueueConsumer::releaseBuffer(int slot, uint64_t frameNumber,
                                            const sp<Fence>& releaseFence) {
#else
status_t BufferQueueConsumer::releaseBuffer(int slot, uint64_t frameNumber,
        const sp<Fence>& releaseFence, EGLDisplay eglDisplay,
        EGLSyncKHR eglFence) {
#endif
    ATRACE_CALL();
    ATRACE_BUFFER_INDEX(slot);

    const int totalSlotCount = mCore->getTotalSlotCountLocked();
    if (slot < 0 || slot >= totalSlotCount) {
        BQ_LOGE("releaseBuffer: slot index %d out of range [0, %d)", slot, totalSlotCount);
        return BAD_VALUE;
    }
    if (releaseFence == nullptr) {
        BQ_LOGE("releaseBuffer: slot %d fence %p NULL", slot, releaseFence.get());
        return BAD_VALUE;
    }

    sp<IProducerListener> listener;
    { // Autolock scope
        std::lock_guard<std::mutex> lock(mCore->mMutex);

        const int totalSlotCount = mCore->getTotalSlotCountLocked();
        if (slot < 0 || slot >= totalSlotCount || releaseFence == nullptr) {
            BQ_LOGE("releaseBuffer: slot %d out of range [0, %d) or fence %p NULL", slot,
                    totalSlotCount, releaseFence.get());
            return BAD_VALUE;
        }

        // If the frame number has changed because the buffer has been reallocated,
        // we can ignore this releaseBuffer for the old buffer.
        // Ignore this for the shared buffer where the frame number can easily
        // get out of sync due to the buffer being queued and acquired at the
        // same time.
        if (frameNumber != mSlots[slot].mFrameNumber &&
                !mSlots[slot].mBufferState.isShared()) {
            return STALE_BUFFER_SLOT;
        }

        if (!mSlots[slot].mBufferState.isAcquired()) {
            BQ_LOGE("releaseBuffer: attempted to release buffer slot %d "
                    "but its state was %s", slot,
                    mSlots[slot].mBufferState.string());
            return BAD_VALUE;
        }

#if !COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_GL_FENCE_CLEANUP)
        mSlots[slot].mEglDisplay = eglDisplay;
        mSlots[slot].mEglFence = eglFence;
#endif
        mSlots[slot].mFence = releaseFence;
        mSlots[slot].mBufferState.release();

        // After leaving shared buffer mode, the shared buffer will
        // still be around. Mark it as no longer shared if this
        // operation causes it to be free.
        if (!mCore->mSharedBufferMode && mSlots[slot].mBufferState.isFree()) {
            mSlots[slot].mBufferState.mShared = false;
        }
        // Don't put the shared buffer on the free list.
        if (!mSlots[slot].mBufferState.isShared()) {
            mCore->mActiveBuffers.erase(slot);
            mCore->mFreeBuffers.push_back(slot);
        }

        if (mCore->mBufferReleasedCbEnabled) {
            listener = mCore->mConnectedProducerListener;
        }
        BQ_LOGV("releaseBuffer: releasing slot %d", slot);

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
        mCore->notifyBufferReleased();
#else
        mCore->mDequeueCondition.notify_all();
#endif

        VALIDATE_CONSISTENCY();
    } // Autolock scope

    // Call back without lock held
    if (listener != nullptr) {
        listener->onBufferReleased();
    }

    return NO_ERROR;
}

status_t BufferQueueConsumer::connect(
        const sp<IConsumerListener>& consumerListener, bool controlledByApp) {
    ATRACE_CALL();

    if (consumerListener == nullptr) {
        BQ_LOGE("connect: consumerListener may not be NULL");
        return BAD_VALUE;
    }

    BQ_LOGV("connect: controlledByApp=%s",
            controlledByApp ? "true" : "false");

    std::lock_guard<std::mutex> lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("connect: BufferQueue has been abandoned");
        return NO_INIT;
    }

    mCore->mConsumerListener = consumerListener;
    mCore->mConsumerControlledByApp = controlledByApp;

    return NO_ERROR;
}

status_t BufferQueueConsumer::disconnect() {
    ATRACE_CALL();

    BQ_LOGV("disconnect");

    std::lock_guard<std::mutex> lock(mCore->mMutex);

    if (mCore->mConsumerListener == nullptr) {
        BQ_LOGE("disconnect: no consumer is connected");
        return BAD_VALUE;
    }

    mCore->mIsAbandoned = true;
    mCore->mConsumerListener = nullptr;
    mCore->mQueue.clear();
    mCore->freeAllBuffersLocked();
    mCore->mSharedBufferSlot = BufferQueueCore::INVALID_BUFFER_SLOT;
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BUFFER_RELEASE_CHANNEL)
    mCore->notifyBufferReleased();
#else
    mCore->mDequeueCondition.notify_all();
#endif
    return NO_ERROR;
}

status_t BufferQueueConsumer::getReleasedBuffers(uint64_t *outSlotMask) {
    ATRACE_CALL();

    if (outSlotMask == nullptr) {
        BQ_LOGE("getReleasedBuffers: outSlotMask may not be NULL");
        return BAD_VALUE;
    }

    std::lock_guard<std::mutex> lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("getReleasedBuffers: BufferQueue has been abandoned");
        return NO_INIT;
    }

    uint64_t mask = 0;
    for (int s = 0; s < BufferQueueDefs::NUM_BUFFER_SLOTS; ++s) {
        if (!mSlots[s].mAcquireCalled) {
            mask |= (1ULL << s);
        }
    }

    // Remove from the mask queued buffers for which acquire has been called,
    // since the consumer will not receive their buffer addresses and so must
    // retain their cached information
    BufferQueueCore::Fifo::iterator current(mCore->mQueue.begin());
    while (current != mCore->mQueue.end()) {
        if (current->mAcquireCalled) {
            mask &= ~(1ULL << current->mSlot);
        }
        ++current;
    }

    BQ_LOGV("getReleasedBuffers: returning mask %#" PRIx64, mask);
    *outSlotMask = mask;
    return NO_ERROR;
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
status_t BufferQueueConsumer::getReleasedBuffersExtended(std::vector<bool>* outSlotMask) {
    ATRACE_CALL();

    if (outSlotMask == nullptr) {
        BQ_LOGE("getReleasedBuffersExtended: outSlotMask may not be NULL");
        return BAD_VALUE;
    }

    std::lock_guard<std::mutex> lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("getReleasedBuffersExtended: BufferQueue has been abandoned");
        return NO_INIT;
    }

    const int totalSlotCount = mCore->getTotalSlotCountLocked();
    outSlotMask->resize(totalSlotCount);
    for (int s = 0; s < totalSlotCount; ++s) {
        (*outSlotMask)[s] = !mSlots[s].mAcquireCalled;
    }

    // Remove from the mask queued buffers for which acquire has been called,
    // since the consumer will not receive their buffer addresses and so must
    // retain their cached information
    BufferQueueCore::Fifo::iterator current(mCore->mQueue.begin());
    while (current != mCore->mQueue.end()) {
        if (current->mAcquireCalled) {
            (*outSlotMask)[current->mSlot] = false;
        }
        ++current;
    }

    return NO_ERROR;
}
#endif

status_t BufferQueueConsumer::setDefaultBufferSize(uint32_t width,
        uint32_t height) {
    ATRACE_CALL();

    if (width == 0 || height == 0) {
        BQ_LOGV("setDefaultBufferSize: dimensions cannot be 0 (width=%u "
                "height=%u)", width, height);
        return BAD_VALUE;
    }

    BQ_LOGV("setDefaultBufferSize: width=%u height=%u", width, height);

    std::lock_guard<std::mutex> lock(mCore->mMutex);
    mCore->mDefaultWidth = width;
    mCore->mDefaultHeight = height;
    return NO_ERROR;
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
status_t BufferQueueConsumer::allowUnlimitedSlots(bool allowUnlimitedSlots) {
    ATRACE_CALL();
    BQ_LOGV("allowUnlimitedSlots: %d", allowUnlimitedSlots);
    std::lock_guard<std::mutex> lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("allowUnlimitedSlots: BufferQueue has been abandoned");
        return NO_INIT;
    }

    if (mCore->mConnectedApi != BufferQueueCore::NO_CONNECTED_API) {
        BQ_LOGE("allowUnlimitedSlots: BufferQueue already connected");
        return INVALID_OPERATION;
    }

    mCore->mAllowExtendedSlotCount = allowUnlimitedSlots;

    return OK;
}
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)

status_t BufferQueueConsumer::setMaxBufferCount(int bufferCount) {
    ATRACE_CALL();

    if (bufferCount < 1 || bufferCount > BufferQueueDefs::NUM_BUFFER_SLOTS) {
        BQ_LOGE("setMaxBufferCount: invalid count %d", bufferCount);
        return BAD_VALUE;
    }

    std::lock_guard<std::mutex> lock(mCore->mMutex);

    if (mCore->mConnectedApi != BufferQueueCore::NO_CONNECTED_API) {
        BQ_LOGE("setMaxBufferCount: producer is already connected");
        return INVALID_OPERATION;
    }

    if (bufferCount < mCore->mMaxAcquiredBufferCount) {
        BQ_LOGE("setMaxBufferCount: invalid buffer count (%d) less than"
                "mMaxAcquiredBufferCount (%d)", bufferCount,
                mCore->mMaxAcquiredBufferCount);
        return BAD_VALUE;
    }

    int delta = mCore->getMaxBufferCountLocked(mCore->mAsyncMode,
            mCore->mDequeueBufferCannotBlock, bufferCount) -
            mCore->getMaxBufferCountLocked();
    if (!mCore->adjustAvailableSlotsLocked(delta)) {
        BQ_LOGE("setMaxBufferCount: BufferQueue failed to adjust the number of "
                "available slots. Delta = %d", delta);
        return BAD_VALUE;
    }

    mCore->mMaxBufferCount = bufferCount;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setMaxAcquiredBufferCount(int maxAcquiredBuffers) {
    return setMaxAcquiredBufferCount(maxAcquiredBuffers, std::nullopt);
}

status_t BufferQueueConsumer::setMaxAcquiredBufferCount(
        int maxAcquiredBuffers, std::optional<OnBufferReleasedCallback> onBuffersReleasedCallback) {
    ATRACE_FORMAT("%s(%d)", __func__, maxAcquiredBuffers);

    std::optional<OnBufferReleasedCallback> callback;
    { // Autolock scope
        std::unique_lock<std::mutex> lock(mCore->mMutex);

        // We reserve two slots in order to guarantee that the producer and
        // consumer can run asynchronously.
        int maxMaxAcquiredBuffers =
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
                mCore->getTotalSlotCountLocked() - 2;
#else
                BufferQueueCore::MAX_MAX_ACQUIRED_BUFFERS;
#endif
        if (maxAcquiredBuffers < 1 || maxAcquiredBuffers > maxMaxAcquiredBuffers) {
            BQ_LOGE("setMaxAcquiredBufferCount: invalid count %d", maxAcquiredBuffers);
            return BAD_VALUE;
        }

        mCore->waitWhileAllocatingLocked(lock);

        if (mCore->mIsAbandoned) {
            BQ_LOGE("setMaxAcquiredBufferCount: consumer is abandoned");
            return NO_INIT;
        }

        if (maxAcquiredBuffers == mCore->mMaxAcquiredBufferCount) {
            return NO_ERROR;
        }

        // The new maxAcquiredBuffers count should not be violated by the number
        // of currently acquired buffers
        int acquiredCount = 0;
        for (int slot : mCore->mActiveBuffers) {
            if (mSlots[slot].mBufferState.isAcquired()) {
                acquiredCount++;
            }
        }
        if (acquiredCount > maxAcquiredBuffers) {
            BQ_LOGE("setMaxAcquiredBufferCount: the requested maxAcquiredBuffer"
                    "count (%d) exceeds the current acquired buffer count (%d)",
                    maxAcquiredBuffers, acquiredCount);
            return BAD_VALUE;
        }

        if ((maxAcquiredBuffers + mCore->mMaxDequeuedBufferCount +
                (mCore->mAsyncMode || mCore->mDequeueBufferCannotBlock ? 1 : 0))
                > mCore->mMaxBufferCount) {
            BQ_LOGE("setMaxAcquiredBufferCount: %d acquired buffers would "
                    "exceed the maxBufferCount (%d) (maxDequeued %d async %d)",
                    maxAcquiredBuffers, mCore->mMaxBufferCount,
                    mCore->mMaxDequeuedBufferCount, mCore->mAsyncMode ||
                    mCore->mDequeueBufferCannotBlock);
            return BAD_VALUE;
        }

        int delta = maxAcquiredBuffers - mCore->mMaxAcquiredBufferCount;
        if (!mCore->adjustAvailableSlotsLocked(delta)) {
            return BAD_VALUE;
        }

        BQ_LOGV("setMaxAcquiredBufferCount: %d", maxAcquiredBuffers);
        mCore->mMaxAcquiredBufferCount = maxAcquiredBuffers;
        VALIDATE_CONSISTENCY();
        if (delta < 0) {
            if (onBuffersReleasedCallback) {
                callback = std::move(onBuffersReleasedCallback);
            } else if (mCore->mBufferReleasedCbEnabled) {
                callback = [listener = mCore->mConsumerListener]() {
                    listener->onBuffersReleased();
                };
            }
        }
    }

    // Call back without lock held
    if (callback) {
        (*callback)();
    }

    return NO_ERROR;
}

status_t BufferQueueConsumer::setConsumerName(const String8& name) {
    ATRACE_CALL();
    BQ_LOGV("setConsumerName: '%s'", name.c_str());
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    mCore->mConsumerName = name;
    mConsumerName = name;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setDefaultBufferFormat(PixelFormat defaultFormat) {
    ATRACE_CALL();
    BQ_LOGV("setDefaultBufferFormat: %u", defaultFormat);
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    mCore->mDefaultBufferFormat = defaultFormat;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setDefaultBufferDataSpace(
        android_dataspace defaultDataSpace) {
    ATRACE_CALL();
    BQ_LOGV("setDefaultBufferDataSpace: %u", defaultDataSpace);
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    mCore->mDefaultBufferDataSpace = defaultDataSpace;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setConsumerUsageBits(uint64_t usage) {
    ATRACE_CALL();
    BQ_LOGV("setConsumerUsageBits: %#" PRIx64, usage);
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    mCore->mConsumerUsageBits = usage;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setConsumerIsProtected(bool isProtected) {
    ATRACE_CALL();
    BQ_LOGV("setConsumerIsProtected: %s", isProtected ? "true" : "false");
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    mCore->mConsumerIsProtected = isProtected;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setTransformHint(uint32_t hint) {
    ATRACE_CALL();
    BQ_LOGV("setTransformHint: %#x", hint);
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    mCore->mTransformHint = hint;
    return NO_ERROR;
}

status_t BufferQueueConsumer::getSidebandStream(sp<NativeHandle>* outStream) const {
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    *outStream = mCore->mSidebandStream;
    return NO_ERROR;
}

status_t BufferQueueConsumer::getOccupancyHistory(bool forceFlush,
        std::vector<OccupancyTracker::Segment>* outHistory) {
    std::lock_guard<std::mutex> lock(mCore->mMutex);
#ifndef NO_BINDER
    *outHistory = mCore->mOccupancyTracker.getSegmentHistory(forceFlush);
#else
    (void)forceFlush;
    outHistory->clear();
#endif
    return NO_ERROR;
}

status_t BufferQueueConsumer::discardFreeBuffers() {
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    mCore->discardFreeBuffersLocked();
    return NO_ERROR;
}

status_t BufferQueueConsumer::dumpState(const String8& prefix, String8* outResult) const {
    struct passwd* pwd = getpwnam("shell");
    uid_t shellUid = pwd ? pwd->pw_uid : 0;
    if (!shellUid) {
        int savedErrno = errno;
        BQ_LOGE("Cannot get AID_SHELL");
        return savedErrno ? -savedErrno : UNKNOWN_ERROR;
    }

    bool denied = false;
    const uid_t uid = BufferQueueThreadState::getCallingUid();
#if !defined(__ANDROID_VNDK__) && !defined(NO_BINDER)
    // permission check can't be done for vendors as vendors have no access to
    // the PermissionController.
    const pid_t pid = BufferQueueThreadState::getCallingPid();
    if ((uid != shellUid) &&
        !PermissionCache::checkPermission(String16("android.permission.DUMP"), pid, uid)) {
        outResult->appendFormat("Permission Denial: can't dump BufferQueueConsumer "
                                "from pid=%d, uid=%d\n",
                                pid, uid);
        denied = true;
    }
#else
    if (uid != shellUid) {
        denied = true;
    }
#endif
    if (denied) {
        android_errorWriteWithInfoLog(0x534e4554, "27046057",
                static_cast<int32_t>(uid), nullptr, 0);
        return PERMISSION_DENIED;
    }

    mCore->dumpState(prefix, outResult);
    return NO_ERROR;
}

void BufferQueueConsumer::setAllowExtraAcquire(bool allow) {
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    mCore->mAllowExtraAcquire = allow;
}

} // namespace android
