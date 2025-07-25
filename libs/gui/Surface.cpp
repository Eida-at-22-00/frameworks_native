/*
 * Copyright (C) 2010 The Android Open Source Project
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

#define LOG_TAG "Surface"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#include <gui/Surface.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include <inttypes.h>

#include <android/gui/DisplayStatInfo.h>
#include <android/native_window.h>

#include <gui/FenceMonitor.h>
#include <gui/TraceUtils.h>
#include <utils/Log.h>
#include <utils/NativeHandle.h>
#include <utils/Trace.h>

#include <ui/BufferQueueDefs.h>
#include <ui/DynamicDisplayInfo.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <ui/Region.h>

#include <gui/AidlUtil.h>
#include <gui/BufferItem.h>

#include <gui/ISurfaceComposer.h>
#include <gui/LayerState.h>
#include <private/gui/ComposerService.h>
#include <private/gui/ComposerServiceAIDL.h>

#include <com_android_graphics_libgui_flags.h>

namespace android {

using namespace com::android::graphics::libgui;
using gui::aidl_utils::statusTFromBinderStatus;
using ui::Dataspace;

namespace {

enum {
    // moved from nativewindow/include/system/window.h, to be removed
    NATIVE_WINDOW_GET_WIDE_COLOR_SUPPORT = 28,
    NATIVE_WINDOW_GET_HDR_SUPPORT = 29,
};

bool isInterceptorRegistrationOp(int op) {
    return op == NATIVE_WINDOW_SET_CANCEL_INTERCEPTOR ||
            op == NATIVE_WINDOW_SET_DEQUEUE_INTERCEPTOR ||
            op == NATIVE_WINDOW_SET_PERFORM_INTERCEPTOR ||
            op == NATIVE_WINDOW_SET_QUEUE_INTERCEPTOR ||
            op == NATIVE_WINDOW_SET_QUERY_INTERCEPTOR;
}

} // namespace

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
Surface::ProducerDeathListenerProxy::ProducerDeathListenerProxy(wp<SurfaceListener> surfaceListener)
      : mSurfaceListener(surfaceListener) {}

void Surface::ProducerDeathListenerProxy::binderDied(const wp<IBinder>&) {
    sp<SurfaceListener> surfaceListener = mSurfaceListener.promote();
    if (!surfaceListener) {
        return;
    }

    if (surfaceListener->needsDeathNotify()) {
        surfaceListener->onRemoteDied();
    }
}
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)

Surface::Surface(const sp<IGraphicBufferProducer>& bufferProducer, bool controlledByApp,
                 const sp<IBinder>& surfaceControlHandle)
      : mGraphicBufferProducer(bufferProducer),
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
        mSurfaceDeathListener(nullptr),
#endif
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
        mSlots(NUM_BUFFER_SLOTS),
#endif
        mCrop(Rect::EMPTY_RECT),
        mBufferAge(0),
        mGenerationNumber(0),
        mSharedBufferMode(false),
        mAutoRefresh(false),
        mAutoPrerotation(false),
        mSharedBufferSlot(BufferItem::INVALID_BUFFER_SLOT),
        mSharedBufferHasBeenQueued(false),
        mQueriedSupportedTimestamps(false),
        mFrameTimestampsSupportsPresent(false),
        mEnableFrameTimestamps(false),
        mFrameEventHistory(std::make_unique<ProducerFrameEventHistory>()) {
    // Initialize the ANativeWindow function pointers.
    ANativeWindow::setSwapInterval  = hook_setSwapInterval;
    ANativeWindow::dequeueBuffer    = hook_dequeueBuffer;
    ANativeWindow::cancelBuffer     = hook_cancelBuffer;
    ANativeWindow::queueBuffer      = hook_queueBuffer;
    ANativeWindow::query            = hook_query;
    ANativeWindow::perform          = hook_perform;

    ANativeWindow::dequeueBuffer_DEPRECATED = hook_dequeueBuffer_DEPRECATED;
    ANativeWindow::cancelBuffer_DEPRECATED  = hook_cancelBuffer_DEPRECATED;
    ANativeWindow::lockBuffer_DEPRECATED    = hook_lockBuffer_DEPRECATED;
    ANativeWindow::queueBuffer_DEPRECATED   = hook_queueBuffer_DEPRECATED;

    const_cast<int&>(ANativeWindow::minSwapInterval) = 0;
    const_cast<int&>(ANativeWindow::maxSwapInterval) = 1;

    mReqWidth = 0;
    mReqHeight = 0;
    mReqFormat = 0;
    mReqUsage = 0;
    mTimestamp = NATIVE_WINDOW_TIMESTAMP_AUTO;
    mDataSpace = Dataspace::UNKNOWN;
    mScalingMode = NATIVE_WINDOW_SCALING_MODE_FREEZE;
    mTransform = 0;
    mStickyTransform = 0;
    mDefaultWidth = 0;
    mDefaultHeight = 0;
    mUserWidth = 0;
    mUserHeight = 0;
    mTransformHint = 0;
    mConsumerRunningBehind = false;
    mConnectedToCpu = false;
    mProducerControlledByApp = controlledByApp;
    mSwapIntervalZero = false;
    mMaxBufferCount = NUM_BUFFER_SLOTS;
    mSurfaceControlHandle = surfaceControlHandle;
}

Surface::~Surface() {
    if (mConnectedToCpu) {
        Surface::disconnect(NATIVE_WINDOW_API_CPU);
    }
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
    if (mSurfaceDeathListener != nullptr) {
        IInterface::asBinder(mGraphicBufferProducer)->unlinkToDeath(mSurfaceDeathListener);
        mSurfaceDeathListener = nullptr;
    }
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
}

sp<ISurfaceComposer> Surface::composerService() const {
    return ComposerService::getComposerService();
}

sp<gui::ISurfaceComposer> Surface::composerServiceAIDL() const {
    return ComposerServiceAIDL::getComposerService();
}

nsecs_t Surface::now() const {
    return systemTime();
}

sp<IGraphicBufferProducer> Surface::getIGraphicBufferProducer() const {
    return mGraphicBufferProducer;
}

void Surface::setSidebandStream(const sp<NativeHandle>& stream) {
    mGraphicBufferProducer->setSidebandStream(stream);
}

void Surface::allocateBuffers() {
    uint32_t reqWidth = mReqWidth ? mReqWidth : mUserWidth;
    uint32_t reqHeight = mReqHeight ? mReqHeight : mUserHeight;
    mGraphicBufferProducer->allocateBuffers(reqWidth, reqHeight,
            mReqFormat, mReqUsage);
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
status_t Surface::allowAllocation(bool allowAllocation) {
    return mGraphicBufferProducer->allowAllocation(allowAllocation);
}
#endif

status_t Surface::setGenerationNumber(uint32_t generation) {
    status_t result = mGraphicBufferProducer->setGenerationNumber(generation);
    if (result == NO_ERROR) {
        mGenerationNumber = generation;
    }
    return result;
}

uint64_t Surface::getNextFrameNumber() const {
    Mutex::Autolock lock(mMutex);
    return mNextFrameNumber;
}

String8 Surface::getConsumerName() const {
    return mGraphicBufferProducer->getConsumerName();
}

status_t Surface::setDequeueTimeout(nsecs_t timeout) {
    return mGraphicBufferProducer->setDequeueTimeout(timeout);
}

status_t Surface::getLastQueuedBuffer(sp<GraphicBuffer>* outBuffer,
        sp<Fence>* outFence, float outTransformMatrix[16]) {
    return mGraphicBufferProducer->getLastQueuedBuffer(outBuffer, outFence,
            outTransformMatrix);
}

status_t Surface::getDisplayRefreshCycleDuration(nsecs_t* outRefreshDuration) {
    ATRACE_CALL();

    gui::DisplayStatInfo stats;
    binder::Status status = composerServiceAIDL()->getDisplayStats(nullptr, &stats);
    if (!status.isOk()) {
        return statusTFromBinderStatus(status);
    }

    *outRefreshDuration = stats.vsyncPeriod;

    return NO_ERROR;
}

void Surface::enableFrameTimestamps(bool enable) {
    Mutex::Autolock lock(mMutex);
    // If going from disabled to enabled, get the initial values for
    // compositor and display timing.
    if (!mEnableFrameTimestamps && enable) {
        FrameEventHistoryDelta delta;
        mGraphicBufferProducer->getFrameTimestamps(&delta);
        mFrameEventHistory->applyDelta(delta);
    }
    mEnableFrameTimestamps = enable;
}

status_t Surface::getCompositorTiming(
        nsecs_t* compositeDeadline, nsecs_t* compositeInterval,
        nsecs_t* compositeToPresentLatency) {
    Mutex::Autolock lock(mMutex);
    if (!mEnableFrameTimestamps) {
        return INVALID_OPERATION;
    }

    if (compositeDeadline != nullptr) {
        *compositeDeadline =
                mFrameEventHistory->getNextCompositeDeadline(now());
    }
    if (compositeInterval != nullptr) {
        *compositeInterval = mFrameEventHistory->getCompositeInterval();
    }
    if (compositeToPresentLatency != nullptr) {
        *compositeToPresentLatency =
                mFrameEventHistory->getCompositeToPresentLatency();
    }
    return NO_ERROR;
}

static bool checkConsumerForUpdates(
        const FrameEvents* e, const uint64_t lastFrameNumber,
        const nsecs_t* outLatchTime,
        const nsecs_t* outFirstRefreshStartTime,
        const nsecs_t* outLastRefreshStartTime,
        const nsecs_t* outGpuCompositionDoneTime,
        const nsecs_t* outDisplayPresentTime,
        const nsecs_t* outDequeueReadyTime,
        const nsecs_t* outReleaseTime) {
    bool checkForLatch = (outLatchTime != nullptr) && !e->hasLatchInfo();
    bool checkForFirstRefreshStart = (outFirstRefreshStartTime != nullptr) &&
            !e->hasFirstRefreshStartInfo();
    bool checkForGpuCompositionDone = (outGpuCompositionDoneTime != nullptr) &&
            !e->hasGpuCompositionDoneInfo();
    bool checkForDisplayPresent = (outDisplayPresentTime != nullptr) &&
            !e->hasDisplayPresentInfo();

    // LastRefreshStart, DequeueReady, and Release are never available for the
    // last frame.
    bool checkForLastRefreshStart = (outLastRefreshStartTime != nullptr) &&
            !e->hasLastRefreshStartInfo() &&
            (e->frameNumber != lastFrameNumber);
    bool checkForDequeueReady = (outDequeueReadyTime != nullptr) &&
            !e->hasDequeueReadyInfo() && (e->frameNumber != lastFrameNumber);
    bool checkForRelease = (outReleaseTime != nullptr) &&
            !e->hasReleaseInfo() && (e->frameNumber != lastFrameNumber);

    // RequestedPresent and Acquire info are always available producer-side.
    return checkForLatch || checkForFirstRefreshStart ||
            checkForLastRefreshStart || checkForGpuCompositionDone ||
            checkForDisplayPresent || checkForDequeueReady || checkForRelease;
}

static void getFrameTimestamp(nsecs_t *dst, const nsecs_t& src) {
    if (dst != nullptr) {
        // We always get valid timestamps for these eventually.
        *dst = (src == FrameEvents::TIMESTAMP_PENDING) ?
                NATIVE_WINDOW_TIMESTAMP_PENDING : src;
    }
}

static void getFrameTimestampFence(nsecs_t *dst,
        const std::shared_ptr<FenceTime>& src, bool fenceShouldBeKnown) {
    if (dst != nullptr) {
        if (!fenceShouldBeKnown) {
            *dst = NATIVE_WINDOW_TIMESTAMP_PENDING;
            return;
        }

        nsecs_t signalTime = src->getSignalTime();
        *dst = (signalTime == Fence::SIGNAL_TIME_PENDING) ?
                    NATIVE_WINDOW_TIMESTAMP_PENDING :
                (signalTime == Fence::SIGNAL_TIME_INVALID) ?
                    NATIVE_WINDOW_TIMESTAMP_INVALID :
                signalTime;
    }
}

status_t Surface::getFrameTimestamps(uint64_t frameNumber,
        nsecs_t* outRequestedPresentTime, nsecs_t* outAcquireTime,
        nsecs_t* outLatchTime, nsecs_t* outFirstRefreshStartTime,
        nsecs_t* outLastRefreshStartTime, nsecs_t* outGpuCompositionDoneTime,
        nsecs_t* outDisplayPresentTime, nsecs_t* outDequeueReadyTime,
        nsecs_t* outReleaseTime) {
    ATRACE_CALL();

    Mutex::Autolock lock(mMutex);

    if (!mEnableFrameTimestamps) {
        return INVALID_OPERATION;
    }

    // Verify the requested timestamps are supported.
    querySupportedTimestampsLocked();
    if (outDisplayPresentTime != nullptr && !mFrameTimestampsSupportsPresent) {
        return BAD_VALUE;
    }

    FrameEvents* events = mFrameEventHistory->getFrame(frameNumber);
    if (events == nullptr) {
        // If the entry isn't available in the producer, it's definitely not
        // available in the consumer.
        return NAME_NOT_FOUND;
    }

    // Update our cache of events if the requested events are not available.
    if (checkConsumerForUpdates(events, mLastFrameNumber,
            outLatchTime, outFirstRefreshStartTime, outLastRefreshStartTime,
            outGpuCompositionDoneTime, outDisplayPresentTime,
            outDequeueReadyTime, outReleaseTime)) {
        FrameEventHistoryDelta delta;
        mGraphicBufferProducer->getFrameTimestamps(&delta);
        mFrameEventHistory->applyDelta(delta);
        events = mFrameEventHistory->getFrame(frameNumber);
    }

    if (events == nullptr) {
        // The entry was available before the update, but was overwritten
        // after the update. Make sure not to send the wrong frame's data.
        return NAME_NOT_FOUND;
    }

    getFrameTimestamp(outRequestedPresentTime, events->requestedPresentTime);
    getFrameTimestamp(outLatchTime, events->latchTime);

    nsecs_t firstRefreshStartTime = NATIVE_WINDOW_TIMESTAMP_INVALID;
    getFrameTimestamp(&firstRefreshStartTime, events->firstRefreshStartTime);
    if (outFirstRefreshStartTime) {
        *outFirstRefreshStartTime = firstRefreshStartTime;
    }

    getFrameTimestamp(outLastRefreshStartTime, events->lastRefreshStartTime);
    getFrameTimestamp(outDequeueReadyTime, events->dequeueReadyTime);

    nsecs_t acquireTime = NATIVE_WINDOW_TIMESTAMP_INVALID;
    getFrameTimestampFence(&acquireTime, events->acquireFence,
            events->hasAcquireInfo());
    if (outAcquireTime != nullptr) {
        *outAcquireTime = acquireTime;
    }

    getFrameTimestampFence(outGpuCompositionDoneTime,
            events->gpuCompositionDoneFence,
            events->hasGpuCompositionDoneInfo());
    getFrameTimestampFence(outDisplayPresentTime, events->displayPresentFence,
            events->hasDisplayPresentInfo());
    getFrameTimestampFence(outReleaseTime, events->releaseFence,
            events->hasReleaseInfo());

    // Fix up the GPU completion fence at this layer -- eglGetFrameTimestampsANDROID() expects
    // that EGL_FIRST_COMPOSITION_GPU_FINISHED_TIME_ANDROID > EGL_RENDERING_COMPLETE_TIME_ANDROID.
    // This is typically true, but SurfaceFlinger may opt to cache prior GPU composition results,
    // which breaks that assumption, so zero out GPU composition time.
    if (outGpuCompositionDoneTime != nullptr
            && *outGpuCompositionDoneTime > 0 && (acquireTime > 0 || firstRefreshStartTime > 0)
            && *outGpuCompositionDoneTime <= std::max(acquireTime, firstRefreshStartTime)) {
        *outGpuCompositionDoneTime = 0;
    }

    return NO_ERROR;
}

// Deprecated(b/242763577): to be removed, this method should not be used
// The reason this method still exists here is to support compiled vndk
// Surface support should not be tied to the display
// Return true since most displays should have this support
status_t Surface::getWideColorSupport(bool* supported) {
    ATRACE_CALL();

    *supported = true;
    return NO_ERROR;
}

// Deprecated(b/242763577): to be removed, this method should not be used
// The reason this method still exists here is to support compiled vndk
// Surface support should not be tied to the display
// Return true since most displays should have this support
status_t Surface::getHdrSupport(bool* supported) {
    ATRACE_CALL();

    *supported = true;
    return NO_ERROR;
}

int Surface::hook_setSwapInterval(ANativeWindow* window, int interval) {
    Surface* c = getSelf(window);
    return c->setSwapInterval(interval);
}

int Surface::hook_dequeueBuffer(ANativeWindow* window,
        ANativeWindowBuffer** buffer, int* fenceFd) {
    Surface* c = getSelf(window);
    {
        std::shared_lock<std::shared_mutex> lock(c->mInterceptorMutex);
        if (c->mDequeueInterceptor != nullptr) {
            auto interceptor = c->mDequeueInterceptor;
            auto data = c->mDequeueInterceptorData;
            return interceptor(window, Surface::dequeueBufferInternal, data, buffer, fenceFd);
        }
    }
    return c->dequeueBuffer(buffer, fenceFd);
}

int Surface::dequeueBufferInternal(ANativeWindow* window, ANativeWindowBuffer** buffer,
                                   int* fenceFd) {
    Surface* c = getSelf(window);
    return c->dequeueBuffer(buffer, fenceFd);
}

int Surface::hook_cancelBuffer(ANativeWindow* window,
        ANativeWindowBuffer* buffer, int fenceFd) {
    Surface* c = getSelf(window);
    {
        std::shared_lock<std::shared_mutex> lock(c->mInterceptorMutex);
        if (c->mCancelInterceptor != nullptr) {
            auto interceptor = c->mCancelInterceptor;
            auto data = c->mCancelInterceptorData;
            return interceptor(window, Surface::cancelBufferInternal, data, buffer, fenceFd);
        }
    }
    return c->cancelBuffer(buffer, fenceFd);
}

int Surface::cancelBufferInternal(ANativeWindow* window, ANativeWindowBuffer* buffer, int fenceFd) {
    Surface* c = getSelf(window);
    return c->cancelBuffer(buffer, fenceFd);
}

int Surface::hook_queueBuffer(ANativeWindow* window,
        ANativeWindowBuffer* buffer, int fenceFd) {
    Surface* c = getSelf(window);
    {
        std::shared_lock<std::shared_mutex> lock(c->mInterceptorMutex);
        if (c->mQueueInterceptor != nullptr) {
            auto interceptor = c->mQueueInterceptor;
            auto data = c->mQueueInterceptorData;
            return interceptor(window, Surface::queueBufferInternal, data, buffer, fenceFd);
        }
    }
    return c->queueBuffer(buffer, fenceFd);
}

int Surface::queueBufferInternal(ANativeWindow* window, ANativeWindowBuffer* buffer, int fenceFd) {
    Surface* c = getSelf(window);
    return c->queueBuffer(buffer, fenceFd);
}

int Surface::hook_dequeueBuffer_DEPRECATED(ANativeWindow* window,
        ANativeWindowBuffer** buffer) {
    Surface* c = getSelf(window);
    ANativeWindowBuffer* buf;
    int fenceFd = -1;
    int result = c->dequeueBuffer(&buf, &fenceFd);
    if (result != OK) {
        return result;
    }
    sp<Fence> fence = sp<Fence>::make(fenceFd);
    int waitResult = fence->waitForever("dequeueBuffer_DEPRECATED");
    if (waitResult != OK) {
        ALOGE("dequeueBuffer_DEPRECATED: Fence::wait returned an error: %d",
                waitResult);
        c->cancelBuffer(buf, -1);
        return waitResult;
    }
    *buffer = buf;
    return result;
}

int Surface::hook_cancelBuffer_DEPRECATED(ANativeWindow* window,
        ANativeWindowBuffer* buffer) {
    Surface* c = getSelf(window);
    return c->cancelBuffer(buffer, -1);
}

int Surface::hook_lockBuffer_DEPRECATED(ANativeWindow* window,
        ANativeWindowBuffer* buffer) {
    Surface* c = getSelf(window);
    return c->lockBuffer_DEPRECATED(buffer);
}

int Surface::hook_queueBuffer_DEPRECATED(ANativeWindow* window,
        ANativeWindowBuffer* buffer) {
    Surface* c = getSelf(window);
    return c->queueBuffer(buffer, -1);
}

int Surface::hook_perform(ANativeWindow* window, int operation, ...) {
    va_list args;
    va_start(args, operation);
    Surface* c = getSelf(window);
    int result;
    // Don't acquire shared ownership of the interceptor mutex if we're going to
    // do interceptor registration, as otherwise we'll deadlock on acquiring
    // exclusive ownership.
    if (!isInterceptorRegistrationOp(operation)) {
        std::shared_lock<std::shared_mutex> lock(c->mInterceptorMutex);
        if (c->mPerformInterceptor != nullptr) {
            result = c->mPerformInterceptor(window, Surface::performInternal,
                                            c->mPerformInterceptorData, operation, args);
            va_end(args);
            return result;
        }
    }
    result = c->perform(operation, args);
    va_end(args);
    return result;
}

int Surface::performInternal(ANativeWindow* window, int operation, va_list args) {
    Surface* c = getSelf(window);
    return c->perform(operation, args);
}

int Surface::hook_query(const ANativeWindow* window, int what, int* value) {
    const Surface* c = getSelf(window);
    {
        std::shared_lock<std::shared_mutex> lock(c->mInterceptorMutex);
        if (c->mQueryInterceptor != nullptr) {
            auto interceptor = c->mQueryInterceptor;
            auto data = c->mQueryInterceptorData;
            return interceptor(window, Surface::queryInternal, data, what, value);
        }
    }
    return c->query(what, value);
}

int Surface::queryInternal(const ANativeWindow* window, int what, int* value) {
    const Surface* c = getSelf(window);
    return c->query(what, value);
}

int Surface::setSwapInterval(int interval) {
    ATRACE_CALL();
    // EGL specification states:
    //  interval is silently clamped to minimum and maximum implementation
    //  dependent values before being stored.

    if (interval < minSwapInterval)
        interval = minSwapInterval;

    if (interval > maxSwapInterval)
        interval = maxSwapInterval;

    const bool wasSwapIntervalZero = mSwapIntervalZero;
    mSwapIntervalZero = (interval == 0);

    if (mSwapIntervalZero != wasSwapIntervalZero) {
        mGraphicBufferProducer->setAsyncMode(mSwapIntervalZero);
    }

    return NO_ERROR;
}

void Surface::getDequeueBufferInputLocked(
        IGraphicBufferProducer::DequeueBufferInput* dequeueInput) {
    LOG_ALWAYS_FATAL_IF(dequeueInput == nullptr, "input is null");

    dequeueInput->width = mReqWidth ? mReqWidth : mUserWidth;
    dequeueInput->height = mReqHeight ? mReqHeight : mUserHeight;

    dequeueInput->format = mReqFormat;
    dequeueInput->usage = mReqUsage;

    dequeueInput->getTimestamps = mEnableFrameTimestamps;
}

int Surface::dequeueBuffer(android_native_buffer_t** buffer, int* fenceFd) {
    ATRACE_FORMAT("dequeueBuffer - %s", getDebugName());
    ALOGV("Surface::dequeueBuffer");

    IGraphicBufferProducer::DequeueBufferInput dqInput;
    {
        Mutex::Autolock lock(mMutex);
        if (mReportRemovedBuffers) {
            mRemovedBuffers.clear();
        }

        getDequeueBufferInputLocked(&dqInput);

        if (mSharedBufferMode && mAutoRefresh && mSharedBufferSlot !=
                BufferItem::INVALID_BUFFER_SLOT) {
            sp<GraphicBuffer>& gbuf(mSlots[mSharedBufferSlot].buffer);
            if (gbuf != nullptr) {
                *buffer = gbuf.get();
                *fenceFd = -1;
                return OK;
            }
        }
    } // Drop the lock so that we can still touch the Surface while blocking in IGBP::dequeueBuffer

    int buf = -1;
    sp<Fence> fence;
    nsecs_t startTime = systemTime();

    FrameEventHistoryDelta frameTimestamps;
    status_t result = mGraphicBufferProducer->dequeueBuffer(&buf, &fence, dqInput.width,
                                                            dqInput.height, dqInput.format,
                                                            dqInput.usage, &mBufferAge,
                                                            dqInput.getTimestamps ?
                                                                    &frameTimestamps : nullptr);
    mLastDequeueDuration = systemTime() - startTime;

    if (result < 0) {
        ALOGV("dequeueBuffer: IGraphicBufferProducer::dequeueBuffer"
                "(%d, %d, %d, %#" PRIx64 ") failed: %d",
                dqInput.width, dqInput.height, dqInput.format, dqInput.usage, result);
        return result;
    }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    if (buf < 0 || buf >= (int)mSlots.size()) {
#else
    if (buf < 0 || buf >= NUM_BUFFER_SLOTS) {
#endif
        ALOGE("dequeueBuffer: IGraphicBufferProducer returned invalid slot number %d", buf);
        android_errorWriteLog(0x534e4554, "36991414"); // SafetyNet logging
        return FAILED_TRANSACTION;
    }

    Mutex::Autolock lock(mMutex);

    // Write this while holding the mutex
    mLastDequeueStartTime = startTime;

    sp<GraphicBuffer>& gbuf(mSlots[buf].buffer);

    // this should never happen
    ALOGE_IF(fence == nullptr, "Surface::dequeueBuffer: received null Fence! buf=%d", buf);

    if (CC_UNLIKELY(atrace_is_tag_enabled(ATRACE_TAG_GRAPHICS))) {
        static gui::FenceMonitor hwcReleaseThread("HWC release");
        hwcReleaseThread.queueFence(fence);
    }

    if (result & IGraphicBufferProducer::RELEASE_ALL_BUFFERS) {
        freeAllBuffers();
    }

    if (dqInput.getTimestamps) {
         mFrameEventHistory->applyDelta(frameTimestamps);
    }

    if ((result & IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION) || gbuf == nullptr) {
        if (mReportRemovedBuffers && (gbuf != nullptr)) {
            mRemovedBuffers.push_back(gbuf);
        }
        result = mGraphicBufferProducer->requestBuffer(buf, &gbuf);
        if (result != NO_ERROR) {
            ALOGE("dequeueBuffer: IGraphicBufferProducer::requestBuffer failed: %d", result);
            mGraphicBufferProducer->cancelBuffer(buf, fence);
            return result;
        }
    }

    if (fence->isValid()) {
        *fenceFd = fence->dup();
        if (*fenceFd == -1) {
            ALOGE("dequeueBuffer: error duping fence: %d", errno);
            // dup() should never fail; something is badly wrong. Soldier on
            // and hope for the best; the worst that should happen is some
            // visible corruption that lasts until the next frame.
        }
    } else {
        *fenceFd = -1;
    }

    *buffer = gbuf.get();

    if (mSharedBufferMode && mAutoRefresh) {
        mSharedBufferSlot = buf;
        mSharedBufferHasBeenQueued = false;
    } else if (mSharedBufferSlot == buf) {
        mSharedBufferSlot = BufferItem::INVALID_BUFFER_SLOT;
        mSharedBufferHasBeenQueued = false;
    }

    mDequeuedSlots.insert(buf);

    return OK;
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)

status_t Surface::dequeueBuffer(sp<GraphicBuffer>* buffer, sp<Fence>* outFence) {
    if (buffer == nullptr || outFence == nullptr) {
        return BAD_VALUE;
    }

    android_native_buffer_t* anb;
    int fd = -1;
    status_t res = dequeueBuffer(&anb, &fd);
    *buffer = GraphicBuffer::from(anb);
    *outFence = sp<Fence>::make(fd);
    return res;
}

status_t Surface::queueBuffer(const sp<GraphicBuffer>& buffer, const sp<Fence>& fd,
                              SurfaceQueueBufferOutput* output) {
    if (buffer == nullptr) {
        return BAD_VALUE;
    }
    return queueBuffer(buffer.get(), fd ? fd->get() : -1, output);
}

status_t Surface::detachBuffer(const sp<GraphicBuffer>& buffer) {
    if (nullptr == buffer) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mMutex);

    uint64_t bufferId = buffer->getId();
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    for (int slot = 0; slot < (int)mSlots.size(); ++slot) {
#else
    for (int slot = 0; slot < Surface::NUM_BUFFER_SLOTS; ++slot) {
#endif
        auto& bufferSlot = mSlots[slot];
        if (bufferSlot.buffer != nullptr && bufferSlot.buffer->getId() == bufferId) {
            bufferSlot.buffer = nullptr;
            bufferSlot.dirtyRegion = Region::INVALID_REGION;
            return mGraphicBufferProducer->detachBuffer(slot);
        }
    }

    return BAD_VALUE;
}

#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)

int Surface::dequeueBuffers(std::vector<BatchBuffer>* buffers) {
    using DequeueBufferInput = IGraphicBufferProducer::DequeueBufferInput;
    using DequeueBufferOutput = IGraphicBufferProducer::DequeueBufferOutput;
    using CancelBufferInput = IGraphicBufferProducer::CancelBufferInput;
    using RequestBufferOutput = IGraphicBufferProducer::RequestBufferOutput;

    ATRACE_CALL();
    ALOGV("Surface::dequeueBuffers");

    if (buffers->size() == 0) {
        ALOGE("%s: must dequeue at least 1 buffer!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mSharedBufferMode) {
        ALOGE("%s: batch operation is not supported in shared buffer mode!",
                __FUNCTION__);
        return INVALID_OPERATION;
    }

    size_t numBufferRequested = buffers->size();
    DequeueBufferInput input;

    {
        Mutex::Autolock lock(mMutex);
        if (mReportRemovedBuffers) {
            mRemovedBuffers.clear();
        }

        getDequeueBufferInputLocked(&input);
    } // Drop the lock so that we can still touch the Surface while blocking in IGBP::dequeueBuffers

    std::vector<DequeueBufferInput> dequeueInput(numBufferRequested, input);
    std::vector<DequeueBufferOutput> dequeueOutput;

    nsecs_t startTime = systemTime();

    status_t result = mGraphicBufferProducer->dequeueBuffers(dequeueInput, &dequeueOutput);

    mLastDequeueDuration = systemTime() - startTime;

    if (result < 0) {
        ALOGV("%s: IGraphicBufferProducer::dequeueBuffers"
                "(%d, %d, %d, %#" PRIx64 ") failed: %d",
                __FUNCTION__, input.width, input.height, input.format, input.usage, result);
        return result;
    }

    std::vector<CancelBufferInput> cancelBufferInputs;
    cancelBufferInputs.reserve(numBufferRequested);
    std::vector<status_t> cancelBufferOutputs;
    for (size_t i = 0; i < numBufferRequested; i++) {
        if (dequeueOutput[i].result >= 0) {
            CancelBufferInput& input = cancelBufferInputs.emplace_back();
            input.slot = dequeueOutput[i].slot;
            input.fence = dequeueOutput[i].fence;
        }
    }

    for (const auto& output : dequeueOutput) {
        if (output.result < 0) {
            mGraphicBufferProducer->cancelBuffers(cancelBufferInputs, &cancelBufferOutputs);
            ALOGV("%s: IGraphicBufferProducer::dequeueBuffers"
                    "(%d, %d, %d, %#" PRIx64 ") failed: %d",
                    __FUNCTION__, input.width, input.height, input.format, input.usage,
                    output.result);
            return output.result;
        }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
        if (output.slot < 0 || output.slot >= (int)mSlots.size()) {
#else
        if (output.slot < 0 || output.slot >= NUM_BUFFER_SLOTS) {
#endif
            mGraphicBufferProducer->cancelBuffers(cancelBufferInputs, &cancelBufferOutputs);
            ALOGE("%s: IGraphicBufferProducer returned invalid slot number %d",
                    __FUNCTION__, output.slot);
            android_errorWriteLog(0x534e4554, "36991414"); // SafetyNet logging
            return FAILED_TRANSACTION;
        }

        if (input.getTimestamps && !output.timestamps.has_value()) {
            mGraphicBufferProducer->cancelBuffers(cancelBufferInputs, &cancelBufferOutputs);
            ALOGE("%s: no frame timestamp returns!", __FUNCTION__);
            return FAILED_TRANSACTION;
        }

        // this should never happen
        ALOGE_IF(output.fence == nullptr,
                "%s: received null Fence! slot=%d", __FUNCTION__, output.slot);
    }

    Mutex::Autolock lock(mMutex);

    // Write this while holding the mutex
    mLastDequeueStartTime = startTime;

    std::vector<int32_t> requestBufferSlots;
    requestBufferSlots.reserve(numBufferRequested);
    // handle release all buffers and request buffers
    for (const auto& output : dequeueOutput) {
        if (output.result & IGraphicBufferProducer::RELEASE_ALL_BUFFERS) {
            ALOGV("%s: RELEASE_ALL_BUFFERS during batch operation", __FUNCTION__);
            freeAllBuffers();
            break;
        }
    }

    for (const auto& output : dequeueOutput) {
        // Collect slots that needs requesting buffer
        sp<GraphicBuffer>& gbuf(mSlots[output.slot].buffer);
        if ((result & IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION) || gbuf == nullptr) {
            if (mReportRemovedBuffers && (gbuf != nullptr)) {
                mRemovedBuffers.push_back(gbuf);
            }
            requestBufferSlots.push_back(output.slot);
        }
    }

    // Batch request Buffer
    std::vector<RequestBufferOutput> reqBufferOutput;
    if (requestBufferSlots.size() > 0) {
        result = mGraphicBufferProducer->requestBuffers(requestBufferSlots, &reqBufferOutput);
        if (result != NO_ERROR) {
            ALOGE("%s: IGraphicBufferProducer::requestBuffers failed: %d",
                    __FUNCTION__, result);
            mGraphicBufferProducer->cancelBuffers(cancelBufferInputs, &cancelBufferOutputs);
            return result;
        }

        // Check if we have any single failure
        for (size_t i = 0; i < requestBufferSlots.size(); i++) {
            if (reqBufferOutput[i].result != OK) {
                ALOGE("%s: IGraphicBufferProducer::requestBuffers failed at %zu-th buffer, slot %d",
                        __FUNCTION__, i, requestBufferSlots[i]);
                mGraphicBufferProducer->cancelBuffers(cancelBufferInputs, &cancelBufferOutputs);
                return reqBufferOutput[i].result;
            }
        }

        // Fill request buffer results to mSlots
        for (size_t i = 0; i < requestBufferSlots.size(); i++) {
            mSlots[requestBufferSlots[i]].buffer = reqBufferOutput[i].buffer;
        }
    }

    for (size_t batchIdx = 0; batchIdx < numBufferRequested; batchIdx++) {
        const auto& output = dequeueOutput[batchIdx];
        int slot = output.slot;
        sp<GraphicBuffer>& gbuf(mSlots[slot].buffer);

        if (CC_UNLIKELY(atrace_is_tag_enabled(ATRACE_TAG_GRAPHICS))) {
            static gui::FenceMonitor hwcReleaseThread("HWC release");
            hwcReleaseThread.queueFence(output.fence);
        }

        if (input.getTimestamps) {
             mFrameEventHistory->applyDelta(output.timestamps.value());
        }

        if (output.fence->isValid()) {
            buffers->at(batchIdx).fenceFd = output.fence->dup();
            if (buffers->at(batchIdx).fenceFd == -1) {
                ALOGE("%s: error duping fence: %d", __FUNCTION__, errno);
                // dup() should never fail; something is badly wrong. Soldier on
                // and hope for the best; the worst that should happen is some
                // visible corruption that lasts until the next frame.
            }
        } else {
            buffers->at(batchIdx).fenceFd = -1;
        }

        buffers->at(batchIdx).buffer = gbuf.get();
        mDequeuedSlots.insert(slot);
    }
    return OK;
}

int Surface::cancelBuffer(android_native_buffer_t* buffer,
        int fenceFd) {
    ATRACE_CALL();
    ALOGV("Surface::cancelBuffer");
    Mutex::Autolock lock(mMutex);
    int i = getSlotFromBufferLocked(buffer);
    if (i < 0) {
        if (fenceFd >= 0) {
            close(fenceFd);
        }
        return i;
    }
    if (mSharedBufferSlot == i && mSharedBufferHasBeenQueued) {
        if (fenceFd >= 0) {
            close(fenceFd);
        }
        return OK;
    }
    sp<Fence> fence(fenceFd >= 0 ? sp<Fence>::make(fenceFd) : Fence::NO_FENCE);
    mGraphicBufferProducer->cancelBuffer(i, fence);

    if (mSharedBufferMode && mAutoRefresh && mSharedBufferSlot == i) {
        mSharedBufferHasBeenQueued = true;
    }

    mDequeuedSlots.erase(i);

    return OK;
}

int Surface::cancelBuffers(const std::vector<BatchBuffer>& buffers) {
    using CancelBufferInput = IGraphicBufferProducer::CancelBufferInput;
    ATRACE_CALL();
    ALOGV("Surface::cancelBuffers");

    if (mSharedBufferMode) {
        ALOGE("%s: batch operation is not supported in shared buffer mode!",
                __FUNCTION__);
        return INVALID_OPERATION;
    }

    size_t numBuffers = buffers.size();
    std::vector<CancelBufferInput> cancelBufferInputs(numBuffers);
    std::vector<status_t> cancelBufferOutputs;
    size_t numBuffersCancelled = 0;
    int badSlotResult = 0;
    for (size_t i = 0; i < numBuffers; i++) {
        int slot = getSlotFromBufferLocked(buffers[i].buffer);
        int fenceFd = buffers[i].fenceFd;
        if (slot < 0) {
            if (fenceFd >= 0) {
                close(fenceFd);
            }
            ALOGE("%s: cannot find slot number for cancelled buffer", __FUNCTION__);
            badSlotResult = slot;
        } else {
            sp<Fence> fence(fenceFd >= 0 ? sp<Fence>::make(fenceFd) : Fence::NO_FENCE);
            cancelBufferInputs[numBuffersCancelled].slot = slot;
            cancelBufferInputs[numBuffersCancelled++].fence = fence;
        }
    }
    cancelBufferInputs.resize(numBuffersCancelled);
    mGraphicBufferProducer->cancelBuffers(cancelBufferInputs, &cancelBufferOutputs);


    for (size_t i = 0; i < numBuffersCancelled; i++) {
        mDequeuedSlots.erase(cancelBufferInputs[i].slot);
    }

    if (badSlotResult != 0) {
        return badSlotResult;
    }
    return OK;
}

int Surface::getSlotFromBufferLocked(
        android_native_buffer_t* buffer) const {
    if (buffer == nullptr) {
        ALOGE("%s: input buffer is null!", __FUNCTION__);
        return BAD_VALUE;
    }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    for (int i = 0; i < (int)mSlots.size(); i++) {
#else
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
#endif
        if (mSlots[i].buffer != nullptr &&
                mSlots[i].buffer->handle == buffer->handle) {
            return i;
        }
    }
    ALOGE("%s: unknown buffer: %p", __FUNCTION__, buffer->handle);
    return BAD_VALUE;
}

int Surface::lockBuffer_DEPRECATED(android_native_buffer_t* buffer __attribute__((unused))) {
    ALOGV("Surface::lockBuffer");
    Mutex::Autolock lock(mMutex);
    return OK;
}

void Surface::getQueueBufferInputLocked(android_native_buffer_t* buffer, int fenceFd,
        nsecs_t timestamp, IGraphicBufferProducer::QueueBufferInput* out) {
    bool isAutoTimestamp = false;

    if (timestamp == NATIVE_WINDOW_TIMESTAMP_AUTO) {
        timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
        isAutoTimestamp = true;
        ALOGV("Surface::queueBuffer making up timestamp: %.2f ms",
            timestamp / 1000000.0);
    }

    // Make sure the crop rectangle is entirely inside the buffer.
    Rect crop(Rect::EMPTY_RECT);
    mCrop.intersect(Rect(buffer->width, buffer->height), &crop);

    sp<Fence> fence(fenceFd >= 0 ? sp<Fence>::make(fenceFd) : Fence::NO_FENCE);
    IGraphicBufferProducer::QueueBufferInput input(timestamp, isAutoTimestamp,
            static_cast<android_dataspace>(mDataSpace), crop, mScalingMode,
            mTransform ^ mStickyTransform, fence, mStickyTransform,
            mEnableFrameTimestamps);

    // we should send HDR metadata as needed if this becomes a bottleneck
    input.setHdrMetadata(mHdrMetadata);

    if (mConnectedToCpu || mDirtyRegion.bounds() == Rect::INVALID_RECT) {
        input.setSurfaceDamage(Region::INVALID_REGION);
    } else {
        // Here we do two things:
        // 1) The surface damage was specified using the OpenGL ES convention of
        //    the origin being in the bottom-left corner. Here we flip to the
        //    convention that the rest of the system uses (top-left corner) by
        //    subtracting all top/bottom coordinates from the buffer height.
        // 2) If the buffer is coming in rotated (for example, because the EGL
        //    implementation is reacting to the transform hint coming back from
        //    SurfaceFlinger), the surface damage needs to be rotated the
        //    opposite direction, since it was generated assuming an unrotated
        //    buffer (the app doesn't know that the EGL implementation is
        //    reacting to the transform hint behind its back). The
        //    transformations in the switch statement below apply those
        //    complementary rotations (e.g., if 90 degrees, rotate 270 degrees).

        int width = buffer->width;
        int height = buffer->height;
        bool rotated90 = (mTransform ^ mStickyTransform) &
                NATIVE_WINDOW_TRANSFORM_ROT_90;
        if (rotated90) {
            std::swap(width, height);
        }

        Region flippedRegion;
        for (auto rect : mDirtyRegion) {
            int left = rect.left;
            int right = rect.right;
            int top = height - rect.bottom; // Flip from OpenGL convention
            int bottom = height - rect.top; // Flip from OpenGL convention
            switch (mTransform ^ mStickyTransform) {
                case NATIVE_WINDOW_TRANSFORM_ROT_90: {
                    // Rotate 270 degrees
                    Rect flippedRect{top, width - right, bottom, width - left};
                    flippedRegion.orSelf(flippedRect);
                    break;
                }
                case NATIVE_WINDOW_TRANSFORM_ROT_180: {
                    // Rotate 180 degrees
                    Rect flippedRect{width - right, height - bottom,
                            width - left, height - top};
                    flippedRegion.orSelf(flippedRect);
                    break;
                }
                case NATIVE_WINDOW_TRANSFORM_ROT_270: {
                    // Rotate 90 degrees
                    Rect flippedRect{height - bottom, left,
                            height - top, right};
                    flippedRegion.orSelf(flippedRect);
                    break;
                }
                default: {
                    Rect flippedRect{left, top, right, bottom};
                    flippedRegion.orSelf(flippedRect);
                    break;
                }
            }
        }

        input.setSurfaceDamage(flippedRegion);
    }
    *out = input;
}

void Surface::applyGrallocMetadataLocked(
        android_native_buffer_t* buffer,
        const IGraphicBufferProducer::QueueBufferInput& queueBufferInput) {
    ATRACE_CALL();
    auto& mapper = GraphicBufferMapper::get();
    mapper.setDataspace(buffer->handle, static_cast<ui::Dataspace>(queueBufferInput.dataSpace));
    if (mHdrMetadataIsSet & HdrMetadata::SMPTE2086)
        mapper.setSmpte2086(buffer->handle, queueBufferInput.getHdrMetadata().getSmpte2086());
    if (mHdrMetadataIsSet & HdrMetadata::CTA861_3)
        mapper.setCta861_3(buffer->handle, queueBufferInput.getHdrMetadata().getCta8613());
    if (mHdrMetadataIsSet & HdrMetadata::HDR10PLUS)
        mapper.setSmpte2094_40(buffer->handle, queueBufferInput.getHdrMetadata().getHdr10Plus());
}

void Surface::onBufferQueuedLocked(int slot, sp<Fence> fence,
        const IGraphicBufferProducer::QueueBufferOutput& output) {
    mDequeuedSlots.erase(slot);

    if (mEnableFrameTimestamps) {
        mFrameEventHistory->applyDelta(output.frameTimestamps);
        // Update timestamps with the local acquire fence.
        // The consumer doesn't send it back to prevent us from having two
        // file descriptors of the same fence.
        mFrameEventHistory->updateAcquireFence(mNextFrameNumber,
                std::make_shared<FenceTime>(fence));

        // Cache timestamps of signaled fences so we can close their file
        // descriptors.
        mFrameEventHistory->updateSignalTimes();
    }

    mLastFrameNumber = mNextFrameNumber;

    mDefaultWidth = output.width;
    mDefaultHeight = output.height;
    mNextFrameNumber = output.nextFrameNumber;

    // Ignore transform hint if sticky transform is set or transform to display inverse flag is
    // set.
    if (mStickyTransform == 0 && !transformToDisplayInverse()) {
        mTransformHint = output.transformHint;
    }

    mConsumerRunningBehind = (output.numPendingBuffers >= 2);

    if (!mConnectedToCpu) {
        // Clear surface damage back to full-buffer
        mDirtyRegion = Region::INVALID_REGION;
    }

    if (mSharedBufferMode && mAutoRefresh && mSharedBufferSlot == slot) {
        mSharedBufferHasBeenQueued = true;
    }

    mQueueBufferCondition.broadcast();

    if (CC_UNLIKELY(atrace_is_tag_enabled(ATRACE_TAG_GRAPHICS))) {
        static gui::FenceMonitor gpuCompletionThread("GPU completion");
        gpuCompletionThread.queueFence(fence);
    }
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)

int Surface::queueBuffer(android_native_buffer_t* buffer, int fenceFd,
                         SurfaceQueueBufferOutput* surfaceOutput) {
    ATRACE_CALL();
    ALOGV("Surface::queueBuffer");

    IGraphicBufferProducer::QueueBufferOutput output;
    IGraphicBufferProducer::QueueBufferInput input;
    int slot;
    sp<Fence> fence;
    {
        Mutex::Autolock lock(mMutex);

        slot = getSlotFromBufferLocked(buffer);
        if (slot < 0) {
            if (fenceFd >= 0) {
                close(fenceFd);
            }
            return slot;
        }
        if (mSharedBufferSlot == slot && mSharedBufferHasBeenQueued) {
            if (fenceFd >= 0) {
                close(fenceFd);
            }
            return OK;
        }

        getQueueBufferInputLocked(buffer, fenceFd, mTimestamp, &input);
        applyGrallocMetadataLocked(buffer, input);
        fence = input.fence;
    }
    nsecs_t now = systemTime();
    // Drop the lock temporarily while we touch the underlying producer. In the case of a local
    // BufferQueue, the following should be allowable:
    //
    //    Surface::queueBuffer
    // -> IConsumerListener::onFrameAvailable callback triggers automatically
    // ->   implementation calls IGraphicBufferConsumer::acquire/release immediately
    // -> SurfaceListener::onBufferRelesed callback triggers automatically
    // ->   implementation calls Surface::dequeueBuffer
    status_t err = mGraphicBufferProducer->queueBuffer(slot, input, &output);
    {
        Mutex::Autolock lock(mMutex);

        mLastQueueDuration = systemTime() - now;
        if (err != OK) {
            ALOGE("queueBuffer: error queuing buffer, %d", err);
        }

        onBufferQueuedLocked(slot, fence, output);
    }

    if (surfaceOutput != nullptr) {
        *surfaceOutput = {.bufferReplaced = output.bufferReplaced};
    }

    return err;
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
int Surface::queueBuffers(const std::vector<BatchQueuedBuffer>& buffers,
                          std::vector<SurfaceQueueBufferOutput>* queueBufferOutputs)
#else
int Surface::queueBuffers(const std::vector<BatchQueuedBuffer>& buffers)
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
{
    ATRACE_CALL();
    ALOGV("Surface::queueBuffers");

    size_t numBuffers = buffers.size();
    std::vector<IGraphicBufferProducer::QueueBufferInput> igbpQueueBufferInputs(numBuffers);
    std::vector<IGraphicBufferProducer::QueueBufferOutput> igbpQueueBufferOutputs;
    std::vector<int> bufferSlots(numBuffers, -1);
    std::vector<sp<Fence>> bufferFences(numBuffers);

    int err;
    {
        Mutex::Autolock lock(mMutex);

        if (mSharedBufferMode) {
            ALOGE("%s: batched operation is not supported in shared buffer mode", __FUNCTION__);
            return INVALID_OPERATION;
        }

        for (size_t batchIdx = 0; batchIdx < numBuffers; batchIdx++) {
            int i = getSlotFromBufferLocked(buffers[batchIdx].buffer);
            if (i < 0) {
                if (buffers[batchIdx].fenceFd >= 0) {
                    close(buffers[batchIdx].fenceFd);
                }
                return i;
            }
            bufferSlots[batchIdx] = i;

            IGraphicBufferProducer::QueueBufferInput input;
            getQueueBufferInputLocked(buffers[batchIdx].buffer, buffers[batchIdx].fenceFd,
                                      buffers[batchIdx].timestamp, &input);
            input.slot = i;
            bufferFences[batchIdx] = input.fence;
            igbpQueueBufferInputs[batchIdx] = input;
        }
    }
    nsecs_t now = systemTime();
    err = mGraphicBufferProducer->queueBuffers(igbpQueueBufferInputs, &igbpQueueBufferOutputs);
    {
        Mutex::Autolock lock(mMutex);
        mLastQueueDuration = systemTime() - now;
        if (err != OK) {
            ALOGE("%s: error queuing buffer, %d", __FUNCTION__, err);
        }

        for (size_t batchIdx = 0; batchIdx < numBuffers; batchIdx++) {
            onBufferQueuedLocked(bufferSlots[batchIdx], bufferFences[batchIdx],
                                 igbpQueueBufferOutputs[batchIdx]);
        }
    }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
    if (queueBufferOutputs != nullptr) {
        queueBufferOutputs->clear();
        queueBufferOutputs->resize(numBuffers);
        for (size_t batchIdx = 0; batchIdx < numBuffers; batchIdx++) {
            (*queueBufferOutputs)[batchIdx].bufferReplaced =
                    igbpQueueBufferOutputs[batchIdx].bufferReplaced;
        }
    }
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)

    return err;
}

#else

int Surface::queueBuffer(android_native_buffer_t* buffer, int fenceFd) {
    ATRACE_CALL();
    ALOGV("Surface::queueBuffer");
    Mutex::Autolock lock(mMutex);

    int i = getSlotFromBufferLocked(buffer);
    if (i < 0) {
        if (fenceFd >= 0) {
            close(fenceFd);
        }
        return i;
    }
    if (mSharedBufferSlot == i && mSharedBufferHasBeenQueued) {
        if (fenceFd >= 0) {
            close(fenceFd);
        }
        return OK;
    }

    IGraphicBufferProducer::QueueBufferOutput output;
    IGraphicBufferProducer::QueueBufferInput input;
    getQueueBufferInputLocked(buffer, fenceFd, mTimestamp, &input);
    applyGrallocMetadataLocked(buffer, input);
    sp<Fence> fence = input.fence;

    nsecs_t now = systemTime();

    status_t err = mGraphicBufferProducer->queueBuffer(i, input, &output);
    mLastQueueDuration = systemTime() - now;
    if (err != OK)  {
        ALOGE("queueBuffer: error queuing buffer, %d", err);
    }

    onBufferQueuedLocked(i, fence, output);
    return err;
}

int Surface::queueBuffers(const std::vector<BatchQueuedBuffer>& buffers) {
    ATRACE_CALL();
    ALOGV("Surface::queueBuffers");
    Mutex::Autolock lock(mMutex);

    if (mSharedBufferMode) {
        ALOGE("%s: batched operation is not supported in shared buffer mode", __FUNCTION__);
        return INVALID_OPERATION;
    }

    size_t numBuffers = buffers.size();
    std::vector<IGraphicBufferProducer::QueueBufferInput> queueBufferInputs(numBuffers);
    std::vector<IGraphicBufferProducer::QueueBufferOutput> queueBufferOutputs;
    std::vector<int> bufferSlots(numBuffers, -1);
    std::vector<sp<Fence>> bufferFences(numBuffers);

    for (size_t batchIdx = 0; batchIdx < numBuffers; batchIdx++) {
        int i = getSlotFromBufferLocked(buffers[batchIdx].buffer);
        if (i < 0) {
            if (buffers[batchIdx].fenceFd >= 0) {
                close(buffers[batchIdx].fenceFd);
            }
            return i;
        }
        bufferSlots[batchIdx] = i;

        IGraphicBufferProducer::QueueBufferInput input;
        getQueueBufferInputLocked(
                buffers[batchIdx].buffer, buffers[batchIdx].fenceFd, buffers[batchIdx].timestamp,
                &input);
        bufferFences[batchIdx] = input.fence;
        queueBufferInputs[batchIdx] = input;
    }

    nsecs_t now = systemTime();
    status_t err = mGraphicBufferProducer->queueBuffers(queueBufferInputs, &queueBufferOutputs);
    mLastQueueDuration = systemTime() - now;
    if (err != OK)  {
        ALOGE("%s: error queuing buffer, %d", __FUNCTION__, err);
    }


    for (size_t batchIdx = 0; batchIdx < numBuffers; batchIdx++) {
        onBufferQueuedLocked(bufferSlots[batchIdx], bufferFences[batchIdx],
                queueBufferOutputs[batchIdx]);
    }

    return err;
}

#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)

void Surface::querySupportedTimestampsLocked() const {
    // mMutex must be locked when calling this method.

    if (mQueriedSupportedTimestamps) {
        return;
    }
    mQueriedSupportedTimestamps = true;

    std::vector<FrameEvent> supportedFrameTimestamps;
    binder::Status status =
            composerServiceAIDL()->getSupportedFrameTimestamps(&supportedFrameTimestamps);

    if (!status.isOk()) {
        return;
    }

    for (auto sft : supportedFrameTimestamps) {
        if (sft == FrameEvent::DISPLAY_PRESENT) {
            mFrameTimestampsSupportsPresent = true;
        }
    }
}

int Surface::query(int what, int* value) const {
    ATRACE_CALL();
    ALOGV("Surface::query");
    if ((what == NATIVE_WINDOW_WIDTH) || (what == NATIVE_WINDOW_HEIGHT)) {
        return mGraphicBufferProducer->query(what, value);
    }

    { // scope for the lock
        Mutex::Autolock lock(mMutex);
        switch (what) {
            case NATIVE_WINDOW_FORMAT:
                if (mReqFormat) {
                    *value = static_cast<int>(mReqFormat);
                    return NO_ERROR;
                }
                break;
            case NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER: {
                status_t err = mGraphicBufferProducer->query(what, value);
                if (err == NO_ERROR) {
                    return NO_ERROR;
                }
                sp<gui::ISurfaceComposer> surfaceComposer = composerServiceAIDL();
                if (surfaceComposer == nullptr) {
                    return -EPERM; // likely permissions error
                }
                // ISurfaceComposer no longer supports authenticateSurfaceTexture
                *value = 0;
                return NO_ERROR;
            }
            case NATIVE_WINDOW_CONCRETE_TYPE:
                *value = NATIVE_WINDOW_SURFACE;
                return NO_ERROR;
            case NATIVE_WINDOW_DEFAULT_WIDTH:
                *value = static_cast<int>(
                        mUserWidth ? mUserWidth : mDefaultWidth);
                return NO_ERROR;
            case NATIVE_WINDOW_DEFAULT_HEIGHT:
                *value = static_cast<int>(
                        mUserHeight ? mUserHeight : mDefaultHeight);
                return NO_ERROR;
            case NATIVE_WINDOW_TRANSFORM_HINT:
                *value = static_cast<int>(getTransformHint());
                return NO_ERROR;
            case NATIVE_WINDOW_CONSUMER_RUNNING_BEHIND: {
                status_t err = NO_ERROR;
                if (!mConsumerRunningBehind) {
                    *value = 0;
                } else {
                    err = mGraphicBufferProducer->query(what, value);
                    if (err == NO_ERROR) {
                        mConsumerRunningBehind = *value;
                    }
                }
                return err;
            }
            case NATIVE_WINDOW_BUFFER_AGE: {
                if (mBufferAge > INT32_MAX) {
                    *value = 0;
                } else {
                    *value = static_cast<int32_t>(mBufferAge);
                }
                return NO_ERROR;
            }
            case NATIVE_WINDOW_LAST_DEQUEUE_DURATION: {
                int64_t durationUs = mLastDequeueDuration / 1000;
                *value = durationUs > std::numeric_limits<int>::max() ?
                        std::numeric_limits<int>::max() :
                        static_cast<int>(durationUs);
                return NO_ERROR;
            }
            case NATIVE_WINDOW_LAST_QUEUE_DURATION: {
                int64_t durationUs = mLastQueueDuration / 1000;
                *value = durationUs > std::numeric_limits<int>::max() ?
                        std::numeric_limits<int>::max() :
                        static_cast<int>(durationUs);
                return NO_ERROR;
            }
            case NATIVE_WINDOW_FRAME_TIMESTAMPS_SUPPORTS_PRESENT: {
                querySupportedTimestampsLocked();
                *value = mFrameTimestampsSupportsPresent ? 1 : 0;
                return NO_ERROR;
            }
            case NATIVE_WINDOW_IS_VALID: {
                *value = mGraphicBufferProducer != nullptr ? 1 : 0;
                return NO_ERROR;
            }
            case NATIVE_WINDOW_DATASPACE: {
                *value = static_cast<int>(mDataSpace);
                return NO_ERROR;
            }
            case NATIVE_WINDOW_MAX_BUFFER_COUNT: {
                *value = mMaxBufferCount;
                return NO_ERROR;
            }
        }
    }
    return mGraphicBufferProducer->query(what, value);
}

int Surface::perform(int operation, va_list args)
{
    int res = NO_ERROR;
    switch (operation) {
    case NATIVE_WINDOW_CONNECT:
        // deprecated. must return NO_ERROR.
        break;
    case NATIVE_WINDOW_DISCONNECT:
        // deprecated. must return NO_ERROR.
        break;
    case NATIVE_WINDOW_SET_USAGE:
        res = dispatchSetUsage(args);
        break;
    case NATIVE_WINDOW_SET_CROP:
        res = dispatchSetCrop(args);
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        res = dispatchSetBufferCount(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        res = dispatchSetBuffersGeometry(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
        res = dispatchSetBuffersTransform(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_STICKY_TRANSFORM:
        res = dispatchSetBuffersStickyTransform(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
        res = dispatchSetBuffersTimestamp(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
        res = dispatchSetBuffersDimensions(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        res = dispatchSetBuffersUserDimensions(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
        res = dispatchSetBuffersFormat(args);
        break;
    case NATIVE_WINDOW_LOCK:
        res = dispatchLock(args);
        break;
    case NATIVE_WINDOW_UNLOCK_AND_POST:
        res = dispatchUnlockAndPost(args);
        break;
    case NATIVE_WINDOW_SET_SCALING_MODE:
        res = dispatchSetScalingMode(args);
        break;
    case NATIVE_WINDOW_API_CONNECT:
        res = dispatchConnect(args);
        break;
    case NATIVE_WINDOW_API_DISCONNECT:
        res = dispatchDisconnect(args);
        break;
    case NATIVE_WINDOW_SET_SIDEBAND_STREAM:
        res = dispatchSetSidebandStream(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DATASPACE:
        res = dispatchSetBuffersDataSpace(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_SMPTE2086_METADATA:
        res = dispatchSetBuffersSmpte2086Metadata(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_CTA861_3_METADATA:
        res = dispatchSetBuffersCta8613Metadata(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_HDR10_PLUS_METADATA:
        res = dispatchSetBuffersHdr10PlusMetadata(args);
        break;
    case NATIVE_WINDOW_SET_SURFACE_DAMAGE:
        res = dispatchSetSurfaceDamage(args);
        break;
    case NATIVE_WINDOW_SET_SHARED_BUFFER_MODE:
        res = dispatchSetSharedBufferMode(args);
        break;
    case NATIVE_WINDOW_SET_AUTO_REFRESH:
        res = dispatchSetAutoRefresh(args);
        break;
    case NATIVE_WINDOW_GET_REFRESH_CYCLE_DURATION:
        res = dispatchGetDisplayRefreshCycleDuration(args);
        break;
    case NATIVE_WINDOW_GET_NEXT_FRAME_ID:
        res = dispatchGetNextFrameId(args);
        break;
    case NATIVE_WINDOW_ENABLE_FRAME_TIMESTAMPS:
        res = dispatchEnableFrameTimestamps(args);
        break;
    case NATIVE_WINDOW_GET_COMPOSITOR_TIMING:
        res = dispatchGetCompositorTiming(args);
        break;
    case NATIVE_WINDOW_GET_FRAME_TIMESTAMPS:
        res = dispatchGetFrameTimestamps(args);
        break;
    case NATIVE_WINDOW_GET_WIDE_COLOR_SUPPORT:
        res = dispatchGetWideColorSupport(args);
        break;
    case NATIVE_WINDOW_GET_HDR_SUPPORT:
        res = dispatchGetHdrSupport(args);
        break;
    case NATIVE_WINDOW_SET_USAGE64:
        res = dispatchSetUsage64(args);
        break;
    case NATIVE_WINDOW_GET_CONSUMER_USAGE64:
        res = dispatchGetConsumerUsage64(args);
        break;
    case NATIVE_WINDOW_SET_AUTO_PREROTATION:
        res = dispatchSetAutoPrerotation(args);
        break;
    case NATIVE_WINDOW_GET_LAST_DEQUEUE_START:
        res = dispatchGetLastDequeueStartTime(args);
        break;
    case NATIVE_WINDOW_SET_DEQUEUE_TIMEOUT:
        res = dispatchSetDequeueTimeout(args);
        break;
    case NATIVE_WINDOW_GET_LAST_DEQUEUE_DURATION:
        res = dispatchGetLastDequeueDuration(args);
        break;
    case NATIVE_WINDOW_GET_LAST_QUEUE_DURATION:
        res = dispatchGetLastQueueDuration(args);
        break;
    case NATIVE_WINDOW_SET_FRAME_RATE:
        res = dispatchSetFrameRate(args);
        break;
    case NATIVE_WINDOW_SET_CANCEL_INTERCEPTOR:
        res = dispatchAddCancelInterceptor(args);
        break;
    case NATIVE_WINDOW_SET_DEQUEUE_INTERCEPTOR:
        res = dispatchAddDequeueInterceptor(args);
        break;
    case NATIVE_WINDOW_SET_PERFORM_INTERCEPTOR:
        res = dispatchAddPerformInterceptor(args);
        break;
    case NATIVE_WINDOW_SET_QUEUE_INTERCEPTOR:
        res = dispatchAddQueueInterceptor(args);
        break;
    case NATIVE_WINDOW_SET_QUERY_INTERCEPTOR:
        res = dispatchAddQueryInterceptor(args);
        break;
    case NATIVE_WINDOW_ALLOCATE_BUFFERS:
        allocateBuffers();
        res = NO_ERROR;
        break;
    case NATIVE_WINDOW_GET_LAST_QUEUED_BUFFER:
        res = dispatchGetLastQueuedBuffer(args);
        break;
    case NATIVE_WINDOW_GET_LAST_QUEUED_BUFFER2:
        res = dispatchGetLastQueuedBuffer2(args);
        break;
    case NATIVE_WINDOW_SET_FRAME_TIMELINE_INFO:
        res = dispatchSetFrameTimelineInfo(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_ADDITIONAL_OPTIONS:
        res = dispatchSetAdditionalOptions(args);
        break;
    default:
        res = NAME_NOT_FOUND;
        break;
    }
    return res;
}

int Surface::dispatchConnect(va_list args) {
    int api = va_arg(args, int);
    return connect(api);
}

int Surface::dispatchDisconnect(va_list args) {
    int api = va_arg(args, int);
    return disconnect(api);
}

int Surface::dispatchSetUsage(va_list args) {
    uint64_t usage = va_arg(args, uint32_t);
    return setUsage(usage);
}

int Surface::dispatchSetUsage64(va_list args) {
    uint64_t usage = va_arg(args, uint64_t);
    return setUsage(usage);
}

int Surface::dispatchSetCrop(va_list args) {
    android_native_rect_t const* rect = va_arg(args, android_native_rect_t*);
    return setCrop(reinterpret_cast<Rect const*>(rect));
}

int Surface::dispatchSetBufferCount(va_list args) {
    size_t bufferCount = va_arg(args, size_t);
    return setBufferCount(static_cast<int32_t>(bufferCount));
}

int Surface::dispatchSetBuffersGeometry(va_list args) {
    uint32_t width = va_arg(args, uint32_t);
    uint32_t height = va_arg(args, uint32_t);
    PixelFormat format = va_arg(args, PixelFormat);
    int err = setBuffersDimensions(width, height);
    if (err != 0) {
        return err;
    }
    return setBuffersFormat(format);
}

int Surface::dispatchSetBuffersDimensions(va_list args) {
    uint32_t width = va_arg(args, uint32_t);
    uint32_t height = va_arg(args, uint32_t);
    return setBuffersDimensions(width, height);
}

int Surface::dispatchSetBuffersUserDimensions(va_list args) {
    uint32_t width = va_arg(args, uint32_t);
    uint32_t height = va_arg(args, uint32_t);
    return setBuffersUserDimensions(width, height);
}

int Surface::dispatchSetBuffersFormat(va_list args) {
    PixelFormat format = va_arg(args, PixelFormat);
    return setBuffersFormat(format);
}

int Surface::dispatchSetScalingMode(va_list args) {
    int mode = va_arg(args, int);
    return setScalingMode(mode);
}

int Surface::dispatchSetBuffersTransform(va_list args) {
    uint32_t transform = va_arg(args, uint32_t);
    return setBuffersTransform(transform);
}

int Surface::dispatchSetBuffersStickyTransform(va_list args) {
    uint32_t transform = va_arg(args, uint32_t);
    return setBuffersStickyTransform(transform);
}

int Surface::dispatchSetBuffersTimestamp(va_list args) {
    int64_t timestamp = va_arg(args, int64_t);
    return setBuffersTimestamp(timestamp);
}

int Surface::dispatchLock(va_list args) {
    ANativeWindow_Buffer* outBuffer = va_arg(args, ANativeWindow_Buffer*);
    ARect* inOutDirtyBounds = va_arg(args, ARect*);
    return lock(outBuffer, inOutDirtyBounds);
}

int Surface::dispatchUnlockAndPost(va_list args __attribute__((unused))) {
    return unlockAndPost();
}

int Surface::dispatchSetSidebandStream(va_list args) {
    native_handle_t* sH = va_arg(args, native_handle_t*);
    sp<NativeHandle> sidebandHandle = NativeHandle::create(sH, false);
    setSidebandStream(sidebandHandle);
    return OK;
}

int Surface::dispatchSetBuffersDataSpace(va_list args) {
    Dataspace dataspace = static_cast<Dataspace>(va_arg(args, int));
    return setBuffersDataSpace(dataspace);
}

int Surface::dispatchSetBuffersSmpte2086Metadata(va_list args) {
    const android_smpte2086_metadata* metadata =
        va_arg(args, const android_smpte2086_metadata*);
    return setBuffersSmpte2086Metadata(metadata);
}

int Surface::dispatchSetBuffersCta8613Metadata(va_list args) {
    const android_cta861_3_metadata* metadata =
        va_arg(args, const android_cta861_3_metadata*);
    return setBuffersCta8613Metadata(metadata);
}

int Surface::dispatchSetBuffersHdr10PlusMetadata(va_list args) {
    const size_t size = va_arg(args, size_t);
    const uint8_t* metadata = va_arg(args, const uint8_t*);
    return setBuffersHdr10PlusMetadata(size, metadata);
}

int Surface::dispatchSetSurfaceDamage(va_list args) {
    android_native_rect_t* rects = va_arg(args, android_native_rect_t*);
    size_t numRects = va_arg(args, size_t);
    setSurfaceDamage(rects, numRects);
    return NO_ERROR;
}

int Surface::dispatchSetSharedBufferMode(va_list args) {
    bool sharedBufferMode = va_arg(args, int);
    return setSharedBufferMode(sharedBufferMode);
}

int Surface::dispatchSetAutoRefresh(va_list args) {
    bool autoRefresh = va_arg(args, int);
    return setAutoRefresh(autoRefresh);
}

int Surface::dispatchGetDisplayRefreshCycleDuration(va_list args) {
    nsecs_t* outRefreshDuration = va_arg(args, int64_t*);
    return getDisplayRefreshCycleDuration(outRefreshDuration);
}

int Surface::dispatchGetNextFrameId(va_list args) {
    uint64_t* nextFrameId = va_arg(args, uint64_t*);
    *nextFrameId = getNextFrameNumber();
    return NO_ERROR;
}

int Surface::dispatchEnableFrameTimestamps(va_list args) {
    bool enable = va_arg(args, int);
    enableFrameTimestamps(enable);
    return NO_ERROR;
}

int Surface::dispatchGetCompositorTiming(va_list args) {
    nsecs_t* compositeDeadline = va_arg(args, int64_t*);
    nsecs_t* compositeInterval = va_arg(args, int64_t*);
    nsecs_t* compositeToPresentLatency = va_arg(args, int64_t*);
    return getCompositorTiming(compositeDeadline, compositeInterval,
            compositeToPresentLatency);
}

int Surface::dispatchGetFrameTimestamps(va_list args) {
    uint64_t frameId = va_arg(args, uint64_t);
    nsecs_t* outRequestedPresentTime = va_arg(args, int64_t*);
    nsecs_t* outAcquireTime = va_arg(args, int64_t*);
    nsecs_t* outLatchTime = va_arg(args, int64_t*);
    nsecs_t* outFirstRefreshStartTime = va_arg(args, int64_t*);
    nsecs_t* outLastRefreshStartTime = va_arg(args, int64_t*);
    nsecs_t* outGpuCompositionDoneTime = va_arg(args, int64_t*);
    nsecs_t* outDisplayPresentTime = va_arg(args, int64_t*);
    nsecs_t* outDequeueReadyTime = va_arg(args, int64_t*);
    nsecs_t* outReleaseTime = va_arg(args, int64_t*);
    return getFrameTimestamps(frameId,
            outRequestedPresentTime, outAcquireTime, outLatchTime,
            outFirstRefreshStartTime, outLastRefreshStartTime,
            outGpuCompositionDoneTime, outDisplayPresentTime,
            outDequeueReadyTime, outReleaseTime);
}

int Surface::dispatchGetWideColorSupport(va_list args) {
    bool* outSupport = va_arg(args, bool*);
    return getWideColorSupport(outSupport);
}

int Surface::dispatchGetHdrSupport(va_list args) {
    bool* outSupport = va_arg(args, bool*);
    return getHdrSupport(outSupport);
}

int Surface::dispatchGetConsumerUsage64(va_list args) {
    uint64_t* usage = va_arg(args, uint64_t*);
    return getConsumerUsage(usage);
}

int Surface::dispatchSetAutoPrerotation(va_list args) {
    bool autoPrerotation = va_arg(args, int);
    return setAutoPrerotation(autoPrerotation);
}

int Surface::dispatchGetLastDequeueStartTime(va_list args) {
    int64_t* lastDequeueStartTime = va_arg(args, int64_t*);
    *lastDequeueStartTime = mLastDequeueStartTime;
    return NO_ERROR;
}

int Surface::dispatchSetDequeueTimeout(va_list args) {
    nsecs_t timeout = va_arg(args, int64_t);
    return setDequeueTimeout(timeout);
}

int Surface::dispatchGetLastDequeueDuration(va_list args) {
    int64_t* lastDequeueDuration = va_arg(args, int64_t*);
    *lastDequeueDuration = mLastDequeueDuration;
    return NO_ERROR;
}

int Surface::dispatchGetLastQueueDuration(va_list args) {
    int64_t* lastQueueDuration = va_arg(args, int64_t*);
    *lastQueueDuration = mLastQueueDuration;
    return NO_ERROR;
}

int Surface::dispatchSetFrameRate(va_list args) {
    float frameRate = static_cast<float>(va_arg(args, double));
    int8_t compatibility = static_cast<int8_t>(va_arg(args, int));
    int8_t changeFrameRateStrategy = static_cast<int8_t>(va_arg(args, int));
    return setFrameRate(frameRate, compatibility, changeFrameRateStrategy);
}

int Surface::dispatchAddCancelInterceptor(va_list args) {
    ANativeWindow_cancelBufferInterceptor interceptor =
            va_arg(args, ANativeWindow_cancelBufferInterceptor);
    void* data = va_arg(args, void*);
    std::lock_guard<std::shared_mutex> lock(mInterceptorMutex);
    mCancelInterceptor = interceptor;
    mCancelInterceptorData = data;
    return NO_ERROR;
}

int Surface::dispatchAddDequeueInterceptor(va_list args) {
    ANativeWindow_dequeueBufferInterceptor interceptor =
            va_arg(args, ANativeWindow_dequeueBufferInterceptor);
    void* data = va_arg(args, void*);
    std::lock_guard<std::shared_mutex> lock(mInterceptorMutex);
    mDequeueInterceptor = interceptor;
    mDequeueInterceptorData = data;
    return NO_ERROR;
}

int Surface::dispatchAddPerformInterceptor(va_list args) {
    ANativeWindow_performInterceptor interceptor = va_arg(args, ANativeWindow_performInterceptor);
    void* data = va_arg(args, void*);
    std::lock_guard<std::shared_mutex> lock(mInterceptorMutex);
    mPerformInterceptor = interceptor;
    mPerformInterceptorData = data;
    return NO_ERROR;
}

int Surface::dispatchAddQueueInterceptor(va_list args) {
    ANativeWindow_queueBufferInterceptor interceptor =
            va_arg(args, ANativeWindow_queueBufferInterceptor);
    void* data = va_arg(args, void*);
    std::lock_guard<std::shared_mutex> lock(mInterceptorMutex);
    mQueueInterceptor = interceptor;
    mQueueInterceptorData = data;
    return NO_ERROR;
}

int Surface::dispatchAddQueryInterceptor(va_list args) {
    ANativeWindow_queryInterceptor interceptor = va_arg(args, ANativeWindow_queryInterceptor);
    void* data = va_arg(args, void*);
    std::lock_guard<std::shared_mutex> lock(mInterceptorMutex);
    mQueryInterceptor = interceptor;
    mQueryInterceptorData = data;
    return NO_ERROR;
}

int Surface::dispatchGetLastQueuedBuffer(va_list args) {
    AHardwareBuffer** buffer = va_arg(args, AHardwareBuffer**);
    int* fence = va_arg(args, int*);
    float* matrix = va_arg(args, float*);
    sp<GraphicBuffer> graphicBuffer;
    sp<Fence> spFence;

    int result = mGraphicBufferProducer->getLastQueuedBuffer(&graphicBuffer, &spFence, matrix);

    if (graphicBuffer != nullptr) {
        *buffer = graphicBuffer->toAHardwareBuffer();
        AHardwareBuffer_acquire(*buffer);
    } else {
        *buffer = nullptr;
    }

    if (spFence != nullptr) {
        *fence = spFence->dup();
    } else {
        *fence = -1;
    }
    return result;
}

int Surface::dispatchGetLastQueuedBuffer2(va_list args) {
    AHardwareBuffer** buffer = va_arg(args, AHardwareBuffer**);
    int* fence = va_arg(args, int*);
    ARect* crop = va_arg(args, ARect*);
    uint32_t* transform = va_arg(args, uint32_t*);
    sp<GraphicBuffer> graphicBuffer;
    sp<Fence> spFence;

    Rect r;
    int result =
            mGraphicBufferProducer->getLastQueuedBuffer(&graphicBuffer, &spFence, &r, transform);

    if (graphicBuffer != nullptr) {
        *buffer = graphicBuffer->toAHardwareBuffer();
        AHardwareBuffer_acquire(*buffer);

        // Avoid setting crop* unless buffer is valid (matches IGBP behavior)
        crop->left = r.left;
        crop->top = r.top;
        crop->right = r.right;
        crop->bottom = r.bottom;
    } else {
        *buffer = nullptr;
    }

    if (spFence != nullptr) {
        *fence = spFence->dup();
    } else {
        *fence = -1;
    }
    return result;
}

int Surface::dispatchSetFrameTimelineInfo(va_list args) {
    ATRACE_CALL();
    ALOGV("Surface::%s", __func__);

    const auto nativeWindowFtlInfo = static_cast<ANativeWindowFrameTimelineInfo>(
            va_arg(args, ANativeWindowFrameTimelineInfo));

    FrameTimelineInfo ftlInfo;
    ftlInfo.vsyncId = nativeWindowFtlInfo.frameTimelineVsyncId;
    ftlInfo.inputEventId = nativeWindowFtlInfo.inputEventId;
    ftlInfo.startTimeNanos = nativeWindowFtlInfo.startTimeNanos;
    ftlInfo.useForRefreshRateSelection = nativeWindowFtlInfo.useForRefreshRateSelection;
    ftlInfo.skippedFrameVsyncId = nativeWindowFtlInfo.skippedFrameVsyncId;
    ftlInfo.skippedFrameStartTimeNanos = nativeWindowFtlInfo.skippedFrameStartTimeNanos;

    return setFrameTimelineInfo(nativeWindowFtlInfo.frameNumber, ftlInfo);
}

int Surface::dispatchSetAdditionalOptions(va_list args) {
    ATRACE_CALL();

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_EXTENDEDALLOCATE)
    const AHardwareBufferLongOptions* opts = va_arg(args, const AHardwareBufferLongOptions*);
    const size_t optsSize = va_arg(args, size_t);
    std::vector<gui::AdditionalOptions> convertedOpts;
    convertedOpts.reserve(optsSize);
    for (size_t i = 0; i < optsSize; i++) {
        convertedOpts.emplace_back(opts[i].name, opts[i].value);
    }
    return setAdditionalOptions(convertedOpts);
#else
    (void)args;
    return INVALID_OPERATION;
#endif
}

bool Surface::transformToDisplayInverse() const {
    return (mTransform & NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY) ==
            NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY;
}

int Surface::connect(int api) {
    static sp<SurfaceListener> listener = sp<StubSurfaceListener>::make();
    return connect(api, listener);
}

int Surface::connect(int api, const sp<SurfaceListener>& listener, bool reportBufferRemoval) {
    ATRACE_CALL();
    ALOGV("Surface::connect");
    Mutex::Autolock lock(mMutex);
    IGraphicBufferProducer::QueueBufferOutput output;
    mReportRemovedBuffers = reportBufferRemoval;

    if (listener != nullptr) {
        mListenerProxy = sp<ProducerListenerProxy>::make(this, listener);
    }

    int err =
            mGraphicBufferProducer->connect(mListenerProxy, api, mProducerControlledByApp, &output);
    if (err == NO_ERROR) {
        mDefaultWidth = output.width;
        mDefaultHeight = output.height;
        mNextFrameNumber = output.nextFrameNumber;
        mMaxBufferCount = output.maxBufferCount;
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
        mIsSlotExpansionAllowed = output.isSlotExpansionAllowed;
#endif

        // Ignore transform hint if sticky transform is set or transform to display inverse flag is
        // set. Transform hint should be ignored if the client is expected to always submit buffers
        // in the same orientation.
        if (mStickyTransform == 0 && !transformToDisplayInverse()) {
            mTransformHint = output.transformHint;
        }

        mConsumerRunningBehind = (output.numPendingBuffers >= 2);

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
        if (listener && listener->needsDeathNotify()) {
            mSurfaceDeathListener = sp<ProducerDeathListenerProxy>::make(listener);
            IInterface::asBinder(mGraphicBufferProducer)->linkToDeath(mSurfaceDeathListener);
        }
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
    }
    if (!err && api == NATIVE_WINDOW_API_CPU) {
        mConnectedToCpu = true;
        // Clear the dirty region in case we're switching from a non-CPU API
        mDirtyRegion.clear();
    } else if (!err) {
        // Initialize the dirty region for tracking surface damage
        mDirtyRegion = Region::INVALID_REGION;
    }

    return err;
}

int Surface::disconnect(int api, IGraphicBufferProducer::DisconnectMode mode) {
    ATRACE_CALL();
    ALOGV("Surface::disconnect");
    Mutex::Autolock lock(mMutex);
    mRemovedBuffers.clear();
    mSharedBufferSlot = BufferItem::INVALID_BUFFER_SLOT;
    mSharedBufferHasBeenQueued = false;
    freeAllBuffers();
    int err = mGraphicBufferProducer->disconnect(api, mode);
    if (!err) {
        mReqFormat = 0;
        mReqWidth = 0;
        mReqHeight = 0;
        mReqUsage = 0;
        mCrop.clear();
        mDataSpace = Dataspace::UNKNOWN;
        mScalingMode = NATIVE_WINDOW_SCALING_MODE_FREEZE;
        mTransform = 0;
        mStickyTransform = 0;
        mAutoPrerotation = false;
        mEnableFrameTimestamps = false;
        mMaxBufferCount = NUM_BUFFER_SLOTS;

        if (api == NATIVE_WINDOW_API_CPU) {
            mConnectedToCpu = false;
        }
    }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
    if (mSurfaceDeathListener != nullptr) {
        IInterface::asBinder(mGraphicBufferProducer)->unlinkToDeath(mSurfaceDeathListener);
        mSurfaceDeathListener = nullptr;
    }
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)

    return err;
}

// MIUI ADD: START
void Surface::releaseSlot(int slot) {
    Mutex::Autolock lock(mMutex);
    if(mDequeuedSlots.count(slot) <= 0) {
        ALOGV("Surface releaseSlot %d",slot);
        if (mReportRemovedBuffers && (mSlots[slot].buffer != nullptr)) {
            mRemovedBuffers.push_back(mSlots[slot].buffer);
        }
        mSlots[slot].buffer = nullptr;
    }
}
// MIUI ADD: END

int Surface::detachNextBuffer(sp<GraphicBuffer>* outBuffer,
        sp<Fence>* outFence) {
    ATRACE_CALL();
    ALOGV("Surface::detachNextBuffer");

    if (outBuffer == nullptr || outFence == nullptr) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mMutex);
    if (mReportRemovedBuffers) {
        mRemovedBuffers.clear();
    }

    sp<GraphicBuffer> buffer(nullptr);
    sp<Fence> fence(nullptr);
    status_t result = mGraphicBufferProducer->detachNextBuffer(
            &buffer, &fence);
    if (result != NO_ERROR) {
        return result;
    }

    *outBuffer = buffer;
    if (fence != nullptr && fence->isValid()) {
        *outFence = fence;
    } else {
        *outFence = Fence::NO_FENCE;
    }

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    for (int i = 0; i < (int)mSlots.size(); i++) {
#else
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
#endif
        if (mSlots[i].buffer != nullptr &&
                mSlots[i].buffer->getId() == buffer->getId()) {
            if (mReportRemovedBuffers) {
                mRemovedBuffers.push_back(mSlots[i].buffer);
            }
            mSlots[i].buffer = nullptr;
        }
    }

    return NO_ERROR;
}

int Surface::isBufferOwned(const sp<GraphicBuffer>& buffer, bool* outIsOwned) const {
    ATRACE_CALL();

    if (buffer == nullptr) {
        ALOGE("%s: Bad input, buffer was null", __FUNCTION__);
        return BAD_VALUE;
    }
    if (outIsOwned == nullptr) {
        ALOGE("%s: Bad input, output was null", __FUNCTION__);
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mMutex);

    int slot = this->getSlotFromBufferLocked(buffer->getNativeBuffer());
    if (slot == BAD_VALUE) {
        ALOGV("%s: Buffer %" PRIu64 " is not owned", __FUNCTION__, buffer->getId());
        *outIsOwned = false;
        return NO_ERROR;
    } else if (slot < 0) {
        ALOGV("%s: Buffer %" PRIu64 " look up failed (%d)", __FUNCTION__, buffer->getId(), slot);
        *outIsOwned = false;
        return slot;
    }

    *outIsOwned = true;
    return NO_ERROR;
}

int Surface::attachBuffer(ANativeWindowBuffer* buffer)
{
    ATRACE_CALL();
    sp<GraphicBuffer> graphicBuffer(static_cast<GraphicBuffer*>(buffer));

    ALOGV("Surface::attachBuffer bufferId=%" PRIu64, graphicBuffer->getId());

    Mutex::Autolock lock(mMutex);
    if (mReportRemovedBuffers) {
        mRemovedBuffers.clear();
    }

    uint32_t priorGeneration = graphicBuffer->mGenerationNumber;
    graphicBuffer->mGenerationNumber = mGenerationNumber;
    int32_t attachedSlot = -1;
    status_t result = mGraphicBufferProducer->attachBuffer(&attachedSlot, graphicBuffer);
    if (result != NO_ERROR) {
        ALOGE("attachBuffer: IGraphicBufferProducer call failed (%d)", result);
        graphicBuffer->mGenerationNumber = priorGeneration;
        return result;
    }
    if (mReportRemovedBuffers && (mSlots[attachedSlot].buffer != nullptr)) {
        mRemovedBuffers.push_back(mSlots[attachedSlot].buffer);
    }
    mSlots[attachedSlot].buffer = graphicBuffer;
    mDequeuedSlots.insert(attachedSlot);

    return NO_ERROR;
}

int Surface::setUsage(uint64_t reqUsage)
{
    ALOGV("Surface::setUsage");
    Mutex::Autolock lock(mMutex);
    if (reqUsage != mReqUsage) {
        mSharedBufferSlot = BufferItem::INVALID_BUFFER_SLOT;
    }
    mReqUsage = reqUsage;
    return OK;
}

int Surface::setCrop(Rect const* rect)
{
    ATRACE_CALL();

    Rect realRect(Rect::EMPTY_RECT);
    if (rect == nullptr || rect->isEmpty()) {
        realRect.clear();
    } else {
        realRect = *rect;
    }

    ALOGV("Surface::setCrop rect=[%d %d %d %d]",
            realRect.left, realRect.top, realRect.right, realRect.bottom);

    Mutex::Autolock lock(mMutex);
    mCrop = realRect;
    return NO_ERROR;
}

int Surface::setBufferCount(int bufferCount)
{
    ATRACE_CALL();
    ALOGV("Surface::setBufferCount");
    Mutex::Autolock lock(mMutex);

    status_t err = NO_ERROR;
    if (bufferCount == 0) {
        err = mGraphicBufferProducer->setMaxDequeuedBufferCount(1);
    } else {
        int minUndequeuedBuffers = 0;
        err = mGraphicBufferProducer->query(
                NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBuffers);
        if (err == NO_ERROR) {
            err = mGraphicBufferProducer->setMaxDequeuedBufferCount(
                    bufferCount - minUndequeuedBuffers);
        }
    }

    ALOGE_IF(err, "IGraphicBufferProducer::setBufferCount(%d) returned %s",
             bufferCount, strerror(-err));

    return err;
}

int Surface::setMaxDequeuedBufferCount(int maxDequeuedBuffers) {
    ATRACE_CALL();
    ALOGV("Surface::setMaxDequeuedBufferCount");
    Mutex::Autolock lock(mMutex);

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    if (maxDequeuedBuffers > BufferQueueDefs::NUM_BUFFER_SLOTS && !mIsSlotExpansionAllowed) {
        return BAD_VALUE;
    }

    int minUndequeuedBuffers = 0;
    status_t err = mGraphicBufferProducer->query(NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
                                                 &minUndequeuedBuffers);
    if (err != OK) {
        ALOGE("IGraphicBufferProducer::query(NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS) returned %s",
              strerror(-err));
        return err;
    }

    if (maxDequeuedBuffers > (int)mSlots.size()) {
        int newSlotCount = minUndequeuedBuffers + maxDequeuedBuffers;
        err = mGraphicBufferProducer->extendSlotCount(newSlotCount);
        if (err != OK) {
            ALOGE("IGraphicBufferProducer::extendSlotCount(%d) returned %s", newSlotCount,
                  strerror(-err));
            return err;
        }

        mSlots.resize(newSlotCount);
    }
    err = mGraphicBufferProducer->setMaxDequeuedBufferCount(maxDequeuedBuffers);
#else
    status_t err = mGraphicBufferProducer->setMaxDequeuedBufferCount(maxDequeuedBuffers);
#endif
    ALOGE_IF(err, "IGraphicBufferProducer::setMaxDequeuedBufferCount(%d) "
            "returned %s", maxDequeuedBuffers, strerror(-err));

    return err;
}

int Surface::setAsyncMode(bool async) {
    ATRACE_CALL();
    ALOGV("Surface::setAsyncMode");
    Mutex::Autolock lock(mMutex);

    status_t err = mGraphicBufferProducer->setAsyncMode(async);
    ALOGE_IF(err, "IGraphicBufferProducer::setAsyncMode(%d) returned %s",
            async, strerror(-err));

    return err;
}

int Surface::setSharedBufferMode(bool sharedBufferMode) {
    ATRACE_CALL();
    ALOGV("Surface::setSharedBufferMode (%d)", sharedBufferMode);
    Mutex::Autolock lock(mMutex);

    status_t err = mGraphicBufferProducer->setSharedBufferMode(
            sharedBufferMode);
    if (err == NO_ERROR) {
        mSharedBufferMode = sharedBufferMode;
    }
    ALOGE_IF(err, "IGraphicBufferProducer::setSharedBufferMode(%d) returned"
            "%s", sharedBufferMode, strerror(-err));

    return err;
}

int Surface::setAutoRefresh(bool autoRefresh) {
    ATRACE_CALL();
    ALOGV("Surface::setAutoRefresh (%d)", autoRefresh);
    Mutex::Autolock lock(mMutex);

    status_t err = mGraphicBufferProducer->setAutoRefresh(autoRefresh);
    if (err == NO_ERROR) {
        mAutoRefresh = autoRefresh;
    }
    ALOGE_IF(err, "IGraphicBufferProducer::setAutoRefresh(%d) returned %s",
            autoRefresh, strerror(-err));
    return err;
}

int Surface::setBuffersDimensions(uint32_t width, uint32_t height)
{
    ATRACE_CALL();
    ALOGV("Surface::setBuffersDimensions");

    if ((width && !height) || (!width && height))
        return BAD_VALUE;

    Mutex::Autolock lock(mMutex);
    if (width != mReqWidth || height != mReqHeight) {
        mSharedBufferSlot = BufferItem::INVALID_BUFFER_SLOT;
    }
    mReqWidth = width;
    mReqHeight = height;
    return NO_ERROR;
}

int Surface::setBuffersUserDimensions(uint32_t width, uint32_t height)
{
    ATRACE_CALL();
    ALOGV("Surface::setBuffersUserDimensions");

    if ((width && !height) || (!width && height))
        return BAD_VALUE;

    Mutex::Autolock lock(mMutex);
    if (width != mUserWidth || height != mUserHeight) {
        mSharedBufferSlot = BufferItem::INVALID_BUFFER_SLOT;
    }
    mUserWidth = width;
    mUserHeight = height;
    return NO_ERROR;
}

int Surface::setBuffersFormat(PixelFormat format)
{
    ALOGV("Surface::setBuffersFormat");

    Mutex::Autolock lock(mMutex);
    if (format != mReqFormat) {
        mSharedBufferSlot = BufferItem::INVALID_BUFFER_SLOT;
    }
    mReqFormat = format;
    return NO_ERROR;
}

int Surface::setScalingMode(int mode)
{
    ATRACE_CALL();
    ALOGV("Surface::setScalingMode(%d)", mode);

    switch (mode) {
        case NATIVE_WINDOW_SCALING_MODE_FREEZE:
        case NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW:
        case NATIVE_WINDOW_SCALING_MODE_SCALE_CROP:
        case NATIVE_WINDOW_SCALING_MODE_NO_SCALE_CROP:
            break;
        default:
            ALOGE("unknown scaling mode: %d", mode);
            return BAD_VALUE;
    }

    Mutex::Autolock lock(mMutex);
    mScalingMode = mode;
    return NO_ERROR;
}

int Surface::setBuffersTransform(uint32_t transform)
{
    ATRACE_CALL();
    ALOGV("Surface::setBuffersTransform");
    Mutex::Autolock lock(mMutex);
    // Ensure NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY is sticky. If the client sets the flag, do not
    // override it until the surface is disconnected. This is a temporary workaround for camera
    // until they switch to using Buffer State Layers. Currently if client sets the buffer transform
    // it may be overriden by the buffer producer when the producer sets the buffer transform.
    if (transformToDisplayInverse()) {
        transform |= NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY;
    }
    mTransform = transform;
    return NO_ERROR;
}

int Surface::setBuffersStickyTransform(uint32_t transform)
{
    ATRACE_CALL();
    ALOGV("Surface::setBuffersStickyTransform");
    Mutex::Autolock lock(mMutex);
    mStickyTransform = transform;
    return NO_ERROR;
}

int Surface::setBuffersTimestamp(int64_t timestamp)
{
    ALOGV("Surface::setBuffersTimestamp");
    Mutex::Autolock lock(mMutex);
    mTimestamp = timestamp;
    return NO_ERROR;
}

int Surface::setBuffersDataSpace(Dataspace dataSpace)
{
    ALOGV("Surface::setBuffersDataSpace");
    Mutex::Autolock lock(mMutex);
    mDataSpace = dataSpace;
    return NO_ERROR;
}

int Surface::setBuffersSmpte2086Metadata(const android_smpte2086_metadata* metadata) {
    ALOGV("Surface::setBuffersSmpte2086Metadata");
    Mutex::Autolock lock(mMutex);
    mHdrMetadataIsSet |= HdrMetadata::SMPTE2086;
    if (metadata) {
        mHdrMetadata.smpte2086 = *metadata;
        mHdrMetadata.validTypes |= HdrMetadata::SMPTE2086;
    } else {
        mHdrMetadata.validTypes &= ~HdrMetadata::SMPTE2086;
    }
    return NO_ERROR;
}

int Surface::setBuffersCta8613Metadata(const android_cta861_3_metadata* metadata) {
    ALOGV("Surface::setBuffersCta8613Metadata");
    Mutex::Autolock lock(mMutex);
    mHdrMetadataIsSet |= HdrMetadata::CTA861_3;
    if (metadata) {
        mHdrMetadata.cta8613 = *metadata;
        mHdrMetadata.validTypes |= HdrMetadata::CTA861_3;
    } else {
        mHdrMetadata.validTypes &= ~HdrMetadata::CTA861_3;
    }
    return NO_ERROR;
}

int Surface::setBuffersHdr10PlusMetadata(const size_t size, const uint8_t* metadata) {
    ALOGV("Surface::setBuffersBlobMetadata");
    Mutex::Autolock lock(mMutex);
    mHdrMetadataIsSet |= HdrMetadata::HDR10PLUS;
    if (size > 0) {
        mHdrMetadata.hdr10plus.assign(metadata, metadata + size);
        mHdrMetadata.validTypes |= HdrMetadata::HDR10PLUS;
    } else {
        mHdrMetadata.validTypes &= ~HdrMetadata::HDR10PLUS;
        mHdrMetadata.hdr10plus.clear();
    }
    return NO_ERROR;
}

Dataspace Surface::getBuffersDataSpace() {
    ALOGV("Surface::getBuffersDataSpace");
    Mutex::Autolock lock(mMutex);
    return mDataSpace;
}

void Surface::freeAllBuffers() {
    if (!mDequeuedSlots.empty()) {
        ALOGE("%s: %zu buffers were freed while being dequeued!",
                __FUNCTION__, mDequeuedSlots.size());
    }
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    for (int i = 0; i < (int)mSlots.size(); i++) {
#else
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
#endif
        mSlots[i].buffer = nullptr;
    }
}

status_t Surface::getAndFlushBuffersFromSlots(const std::vector<int32_t>& slots,
        std::vector<sp<GraphicBuffer>>* outBuffers) {
    ALOGV("Surface::getAndFlushBuffersFromSlots");
    for (int32_t i : slots) {
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
        if (i < 0 || i >= (int)mSlots.size()) {
#else
        if (i < 0 || i >= NUM_BUFFER_SLOTS) {
#endif
            ALOGE("%s: Invalid slotIndex: %d", __FUNCTION__, i);
            return BAD_VALUE;
        }
    }

    Mutex::Autolock lock(mMutex);
    for (int32_t i : slots) {
        if (mSlots[i].buffer == nullptr) {
            ALOGW("%s: Discarded slot %d doesn't contain buffer!", __FUNCTION__, i);
            continue;
        }
        // Don't flush currently dequeued buffers
        if (mDequeuedSlots.count(i) > 0) {
            continue;
        }
        outBuffers->push_back(mSlots[i].buffer);
        mSlots[i].buffer = nullptr;
    }
    return OK;
}

void Surface::setSurfaceDamage(android_native_rect_t* rects, size_t numRects) {
    ATRACE_CALL();
    ALOGV("Surface::setSurfaceDamage");
    Mutex::Autolock lock(mMutex);

    if (mConnectedToCpu || numRects == 0) {
        mDirtyRegion = Region::INVALID_REGION;
        return;
    }

    mDirtyRegion.clear();
    for (size_t r = 0; r < numRects; ++r) {
        // We intentionally flip top and bottom here, since because they're
        // specified with a bottom-left origin, top > bottom, which fails
        // validation in the Region class. We will fix this up when we flip to a
        // top-left origin in queueBuffer.
        Rect rect(rects[r].left, rects[r].bottom, rects[r].right, rects[r].top);
        mDirtyRegion.orSelf(rect);
    }
}

// ----------------------------------------------------------------------
// the lock/unlock APIs must be used from the same thread

static status_t copyBlt(
        const sp<GraphicBuffer>& dst,
        const sp<GraphicBuffer>& src,
        const Region& reg,
        int *dstFenceFd)
{
    if (dst->getId() == src->getId())
        return OK;

    // src and dst with, height and format must be identical. no verification
    // is done here.
    status_t err;
    uint8_t* src_bits = nullptr;
    err = src->lock(GRALLOC_USAGE_SW_READ_OFTEN, reg.bounds(),
            reinterpret_cast<void**>(&src_bits));
    ALOGE_IF(err, "error locking src buffer %s", strerror(-err));

    uint8_t* dst_bits = nullptr;
    err = dst->lockAsync(GRALLOC_USAGE_SW_WRITE_OFTEN, reg.bounds(),
            reinterpret_cast<void**>(&dst_bits), *dstFenceFd);
    ALOGE_IF(err, "error locking dst buffer %s", strerror(-err));
    *dstFenceFd = -1;

    Region::const_iterator head(reg.begin());
    Region::const_iterator tail(reg.end());
    if (head != tail && src_bits && dst_bits) {
        const size_t bpp = bytesPerPixel(src->format);
        const size_t dbpr = static_cast<uint32_t>(dst->stride) * bpp;
        const size_t sbpr = static_cast<uint32_t>(src->stride) * bpp;

        while (head != tail) {
            const Rect& r(*head++);
            int32_t h = r.height();
            if (h <= 0) continue;
            size_t size = static_cast<uint32_t>(r.width()) * bpp;
            uint8_t const * s = src_bits +
                    static_cast<uint32_t>(r.left + src->stride * r.top) * bpp;
            uint8_t       * d = dst_bits +
                    static_cast<uint32_t>(r.left + dst->stride * r.top) * bpp;
            if (dbpr==sbpr && size==sbpr) {
                size *= static_cast<size_t>(h);
                h = 1;
            }
            do {
                memcpy(d, s, size);
                d += dbpr;
                s += sbpr;
            } while (--h > 0);
        }
    }

    if (src_bits)
        src->unlock();

    if (dst_bits)
        dst->unlockAsync(dstFenceFd);

    return err;
}

// ----------------------------------------------------------------------------

status_t Surface::lock(
        ANativeWindow_Buffer* outBuffer, ARect* inOutDirtyBounds)
{
    if (mLockedBuffer != nullptr) {
        ALOGE("Surface::lock failed, already locked");
        return INVALID_OPERATION;
    }

    if (!mConnectedToCpu) {
        int err = Surface::connect(NATIVE_WINDOW_API_CPU);
        if (err) {
            return err;
        }
        // we're intending to do software rendering from this point
        setUsage(GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
    }

    ANativeWindowBuffer* out;
    int fenceFd = -1;
    status_t err = dequeueBuffer(&out, &fenceFd);
    ALOGE_IF(err, "dequeueBuffer failed (%s)", strerror(-err));
    if (err == NO_ERROR) {
        sp<GraphicBuffer> backBuffer(GraphicBuffer::getSelf(out));
        const Rect bounds(backBuffer->width, backBuffer->height);

        Region newDirtyRegion;
        if (inOutDirtyBounds) {
            newDirtyRegion.set(static_cast<Rect const&>(*inOutDirtyBounds));
            newDirtyRegion.andSelf(bounds);
        } else {
            newDirtyRegion.set(bounds);
        }

        // figure out if we can copy the frontbuffer back
        const sp<GraphicBuffer>& frontBuffer(mPostedBuffer);
        const bool canCopyBack = (frontBuffer != nullptr &&
                backBuffer->width  == frontBuffer->width &&
                backBuffer->height == frontBuffer->height &&
                backBuffer->format == frontBuffer->format);

        if (canCopyBack) {
            // copy the area that is invalid and not repainted this round
            const Region copyback(mDirtyRegion.subtract(newDirtyRegion));
            if (!copyback.isEmpty()) {
                copyBlt(backBuffer, frontBuffer, copyback, &fenceFd);
            }
        } else {
            // if we can't copy-back anything, modify the user's dirty
            // region to make sure they redraw the whole buffer
            newDirtyRegion.set(bounds);
            mDirtyRegion.clear();
            Mutex::Autolock lock(mMutex);
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
            for (int i = 0; i < (int)mSlots.size(); i++) {
#else
            for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
#endif
                mSlots[i].dirtyRegion.clear();
            }
        }


        { // scope for the lock
            Mutex::Autolock lock(mMutex);
            int backBufferSlot(getSlotFromBufferLocked(backBuffer.get()));
            if (backBufferSlot >= 0) {
                Region& dirtyRegion(mSlots[backBufferSlot].dirtyRegion);
                mDirtyRegion.subtract(dirtyRegion);
                dirtyRegion = newDirtyRegion;
            }
        }

        mDirtyRegion.orSelf(newDirtyRegion);
        if (inOutDirtyBounds) {
            *inOutDirtyBounds = newDirtyRegion.getBounds();
        }

        void* vaddr;
        status_t res = backBuffer->lockAsync(
                GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
                newDirtyRegion.bounds(), &vaddr, fenceFd);

        ALOGW_IF(res, "failed locking buffer (handle = %p)",
                backBuffer->handle);

        if (res != 0) {
            err = INVALID_OPERATION;
        } else {
            mLockedBuffer = backBuffer;
            outBuffer->width  = backBuffer->width;
            outBuffer->height = backBuffer->height;
            outBuffer->stride = backBuffer->stride;
            outBuffer->format = backBuffer->format;
            outBuffer->bits   = vaddr;
        }
    }
    return err;
}

status_t Surface::unlockAndPost()
{
    if (mLockedBuffer == nullptr) {
        ALOGE("Surface::unlockAndPost failed, no locked buffer");
        return INVALID_OPERATION;
    }

    int fd = -1;
    status_t err = mLockedBuffer->unlockAsync(&fd);
    ALOGE_IF(err, "failed unlocking buffer (%p)", mLockedBuffer->handle);

    err = queueBuffer(mLockedBuffer.get(), fd);
    ALOGE_IF(err, "queueBuffer (handle=%p) failed (%s)",
            mLockedBuffer->handle, strerror(-err));

    mPostedBuffer = mLockedBuffer;
    mLockedBuffer = nullptr;
    return err;
}

bool Surface::waitForNextFrame(uint64_t lastFrame, nsecs_t timeout) {
    Mutex::Autolock lock(mMutex);
    if (mLastFrameNumber > lastFrame) {
        return true;
    }
    return mQueueBufferCondition.waitRelative(mMutex, timeout) == OK;
}

status_t Surface::getUniqueId(uint64_t* outId) const {
    Mutex::Autolock lock(mMutex);
    return mGraphicBufferProducer->getUniqueId(outId);
}

int Surface::getConsumerUsage(uint64_t* outUsage) const {
    Mutex::Autolock lock(mMutex);
    return mGraphicBufferProducer->getConsumerUsage(outUsage);
}

status_t Surface::getAndFlushRemovedBuffers(std::vector<sp<GraphicBuffer>>* out) {
    if (out == nullptr) {
        ALOGE("%s: out must not be null!", __FUNCTION__);
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mMutex);
    *out = mRemovedBuffers;
    mRemovedBuffers.clear();
    return OK;
}

status_t Surface::attachAndQueueBufferWithDataspace(Surface* surface, sp<GraphicBuffer> buffer,
                                                    Dataspace dataspace) {
    if (buffer == nullptr) {
        return BAD_VALUE;
    }
    int err = static_cast<ANativeWindow*>(surface)->perform(surface, NATIVE_WINDOW_API_CONNECT,
                                                            NATIVE_WINDOW_API_CPU);
    if (err != OK) {
        return err;
    }
    ui::Dataspace tmpDataspace = surface->getBuffersDataSpace();
    err = surface->setBuffersDataSpace(dataspace);
    if (err != OK) {
        return err;
    }
    err = surface->attachBuffer(buffer->getNativeBuffer());
    if (err != OK) {
        return err;
    }
    err = static_cast<ANativeWindow*>(surface)->queueBuffer(surface, buffer->getNativeBuffer(), -1);
    if (err != OK) {
        return err;
    }
    err = surface->setBuffersDataSpace(tmpDataspace);
    if (err != OK) {
        return err;
    }
    err = surface->disconnect(NATIVE_WINDOW_API_CPU);
    return err;
}

int Surface::setAutoPrerotation(bool autoPrerotation) {
    ATRACE_CALL();
    ALOGV("Surface::setAutoPrerotation (%d)", autoPrerotation);
    Mutex::Autolock lock(mMutex);

    if (mAutoPrerotation == autoPrerotation) {
        return OK;
    }

    status_t err = mGraphicBufferProducer->setAutoPrerotation(autoPrerotation);
    if (err == NO_ERROR) {
        mAutoPrerotation = autoPrerotation;
    }
    ALOGE_IF(err, "IGraphicBufferProducer::setAutoPrerotation(%d) returned %s", autoPrerotation,
             strerror(-err));
    return err;
}

void Surface::ProducerListenerProxy::onBuffersDiscarded(const std::vector<int32_t>& slots) {
    ATRACE_CALL();
    sp<Surface> parent = mParent.promote();
    if (parent == nullptr) {
        return;
    }

    std::vector<sp<GraphicBuffer>> discardedBufs;
    status_t res = parent->getAndFlushBuffersFromSlots(slots, &discardedBufs);
    if (res != OK) {
        ALOGE("%s: Failed to get buffers from slots: %s(%d)", __FUNCTION__,
                strerror(-res), res);
        return;
    }

    mSurfaceListener->onBuffersDiscarded(discardedBufs);
}

status_t Surface::setFrameRate(float frameRate, int8_t compatibility,
                               int8_t changeFrameRateStrategy) {
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_SETFRAMERATE)
    if (flags::bq_setframerate()) {
        status_t err = mGraphicBufferProducer->setFrameRate(frameRate, compatibility,
                                                            changeFrameRateStrategy);
        ALOGE_IF(err, "IGraphicBufferProducer::setFrameRate(%.2f) returned %s", frameRate,
                 strerror(-err));
        return err;
    }
#else
    static_cast<void>(frameRate);
    static_cast<void>(compatibility);
    static_cast<void>(changeFrameRateStrategy);
#endif

    ALOGI("Surface::setFrameRate is deprecated, setFrameRate hint is dropped as destination is not "
          "SurfaceFlinger");
    // ISurfaceComposer no longer supports setFrameRate, we will return NO_ERROR when the api is
    // called to avoid apps crashing, as BAD_VALUE can generate fatal exception in apps.
    return NO_ERROR;
}

status_t Surface::setFrameTimelineInfo(uint64_t /*frameNumber*/,
                                       const FrameTimelineInfo& /*frameTimelineInfo*/) {
    // ISurfaceComposer no longer supports setFrameTimelineInfo
    return BAD_VALUE;
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_EXTENDEDALLOCATE)
status_t Surface::setAdditionalOptions(const std::vector<gui::AdditionalOptions>& options) {
    if (!GraphicBufferAllocator::get().supportsAdditionalOptions()) {
        return INVALID_OPERATION;
    }

    Mutex::Autolock lock(mMutex);
    return mGraphicBufferProducer->setAdditionalOptions(options);
}
#endif

sp<IBinder> Surface::getSurfaceControlHandle() const {
    Mutex::Autolock lock(mMutex);
    return mSurfaceControlHandle;
}

void Surface::destroy() {
    Mutex::Autolock lock(mMutex);
    mSurfaceControlHandle = nullptr;
}

const char* Surface::getDebugName() {
    std::unique_lock lock{mNameMutex};
    if (mName.empty()) {
        mName = getConsumerName();
    }
    return mName.c_str();
}

}; // namespace android
