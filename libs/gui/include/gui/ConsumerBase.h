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

#ifndef ANDROID_GUI_CONSUMERBASE_H
#define ANDROID_GUI_CONSUMERBASE_H

#include <com_android_graphics_libgui_flags.h>
#include <gui/BufferQueueDefs.h>
#include <gui/IConsumerListener.h>
#include <gui/IGraphicBufferConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/OccupancyTracker.h>
#include <ui/PixelFormat.h>
#include <utils/String8.h>
#include <utils/Vector.h>
#include <utils/threads.h>

namespace android {
// ----------------------------------------------------------------------------

class String8;
class GraphicBuffer;

// ConsumerBase is a base class for BufferQueue consumer end-points. It
// handles common tasks like management of the connection to the BufferQueue
// and the buffer pool.
class ConsumerBase : public virtual RefBase,
        protected ConsumerListener {
public:
    struct FrameAvailableListener : public virtual RefBase {
        // See IConsumerListener::onFrame{Available,Replaced}
        virtual void onFrameAvailable(const BufferItem& item) = 0;
        virtual void onFrameReplaced(const BufferItem& /* item */) {}
        virtual void onFrameDequeued(const uint64_t){};
        virtual void onFrameCancelled(const uint64_t){};
        virtual void onFrameDetached(const uint64_t){};
    };

    ~ConsumerBase() override;

    // abandon frees all the buffers and puts the ConsumerBase into the
    // 'abandoned' state.  Once put in this state the ConsumerBase can never
    // leave it.  When in the 'abandoned' state, all methods of the
    // IGraphicBufferProducer interface will fail with the NO_INIT error.
    //
    // Note that while calling this method causes all the buffers to be freed
    // from the perspective of the the ConsumerBase, if there are additional
    // references on the buffers (e.g. if a buffer is referenced by a client
    // or by OpenGL ES as a texture) then those buffer will remain allocated.
    void abandon();

    // Returns true if the ConsumerBase is in the 'abandoned' state
    bool isAbandoned();

    // set the name of the ConsumerBase that will be used to identify it in
    // log messages.
    void setName(const String8& name);

    // dumpState writes the current state to a string. Child classes should add
    // their state to the dump by overriding the dumpLocked method, which is
    // called by these methods after locking the mutex.
    void dumpState(String8& result) const;
    void dumpState(String8& result, const char* prefix) const;

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
    // Returns a Surface that can be used as the producer for this consumer.
    sp<Surface> getSurface() const;

    // DEPRECATED, DO NOT USE. Returns the underlying IGraphicBufferConsumer
    // that backs this ConsumerBase.
    sp<IGraphicBufferConsumer> getIGraphicBufferConsumer() const
            __attribute((deprecated("DO NOT USE: Temporary hack for refactoring")));
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)

    // setFrameAvailableListener sets the listener object that will be notified
    // when a new frame becomes available.
    void setFrameAvailableListener(const wp<FrameAvailableListener>& listener);

    // See IGraphicBufferConsumer::detachBuffer
    status_t detachBuffer(int slot) __attribute((
            deprecated("Please use the GraphicBuffer variant--slots are deprecated.")));

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)
    // See IGraphicBufferConsumer::detachBuffer
    status_t detachBuffer(const sp<GraphicBuffer>& buffer);
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_PLATFORM_API_IMPROVEMENTS)

    status_t addReleaseFence(const sp<GraphicBuffer> buffer, const sp<Fence>& fence);

    // See IGraphicBufferConsumer::setDefaultBufferSize
    status_t setDefaultBufferSize(uint32_t width, uint32_t height);

    // See IGraphicBufferConsumer::setDefaultBufferFormat
    status_t setDefaultBufferFormat(PixelFormat defaultFormat);

    // See IGraphicBufferConsumer::setDefaultBufferDataSpace
    status_t setDefaultBufferDataSpace(android_dataspace defaultDataSpace);

    // See IGraphicBufferConsumer::setConsumerUsageBits
    status_t setConsumerUsageBits(uint64_t usage);

    // See IGraphicBufferConsumer::setTransformHint
    status_t setTransformHint(uint32_t hint);

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
    // See IGraphicBufferConsumer::setMaxBufferCount
    status_t setMaxBufferCount(int bufferCount);
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)

    // See IGraphicBufferConsumer::setMaxAcquiredBufferCount
    status_t setMaxAcquiredBufferCount(int maxAcquiredBuffers);

    status_t setConsumerIsProtected(bool isProtected);

    // See IGraphicBufferConsumer::getSidebandStream
    sp<NativeHandle> getSidebandStream() const;

    // See IGraphicBufferConsumer::getOccupancyHistory
    status_t getOccupancyHistory(bool forceFlush,
            std::vector<OccupancyTracker::Segment>* outHistory);

    // See IGraphicBufferConsumer::discardFreeBuffers
    status_t discardFreeBuffers();

private:
    ConsumerBase(const ConsumerBase&);
    void operator=(const ConsumerBase&);

    void initialize(bool controlledByApp);

protected:
    // ConsumerBase constructs a new ConsumerBase object to consume image
    // buffers from the given IGraphicBufferConsumer.
    // The controlledByApp flag indicates that this consumer is under the application's
    // control.
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
    explicit ConsumerBase(bool controlledByApp = false, bool consumerIsSurfaceFlinger = false);
    explicit ConsumerBase(const sp<IGraphicBufferProducer>& producer,
                          const sp<IGraphicBufferConsumer>& consumer, bool controlledByApp = false);

    explicit ConsumerBase(const sp<IGraphicBufferConsumer>& consumer, bool controlledByApp = false)
            __attribute((deprecated("ConsumerBase should own its own producer, and constructing it "
                                    "without one is fragile! This method is going away soon.")));
#else
    explicit ConsumerBase(const sp<IGraphicBufferConsumer>& consumer, bool controlledByApp = false);
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)

    // onLastStrongRef gets called by RefBase just before the dtor of the most
    // derived class.  It is used to clean up the buffers so that ConsumerBase
    // can coordinate the clean-up by calling into virtual methods implemented
    // by the derived classes.  This would not be possible from the
    // ConsuemrBase dtor because by the time that gets called the derived
    // classes have already been destructed.
    //
    // This methods should not need to be overridden by derived classes, but
    // if they are overridden the ConsumerBase implementation must be called
    // from the derived class.
    virtual void onLastStrongRef(const void* id);

    // Implementation of the IConsumerListener interface.  These
    // calls are used to notify the ConsumerBase of asynchronous events in the
    // BufferQueue.  The onFrameAvailable, onFrameReplaced, and
    // onBuffersReleased methods should not need to be overridden by derived
    // classes, but if they are overridden the ConsumerBase implementation must
    // be called from the derived class. The ConsumerBase version of
    // onSidebandStreamChanged does nothing and can be overriden by derived
    // classes if they want the notification.
    virtual void onFrameAvailable(const BufferItem& item) override;
    virtual void onFrameReplaced(const BufferItem& item) override;
    virtual void onFrameDequeued(const uint64_t bufferId) override;
    virtual void onFrameCancelled(const uint64_t bufferId) override;
    virtual void onFrameDetached(const uint64_t bufferId) override;
    virtual void onBuffersReleased() override;
    virtual void onSidebandStreamChanged() override;
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    virtual void onSlotCountChanged(int slotCount) override;
#endif
    virtual int getSlotForBufferLocked(const sp<GraphicBuffer>& buffer);

    virtual void onBuffersReleasedLocked();

    virtual status_t detachBufferLocked(int slotIndex);

    // freeBufferLocked frees up the given buffer slot.  If the slot has been
    // initialized this will release the reference to the GraphicBuffer in that
    // slot.  Otherwise it has no effect.
    //
    // Derived classes should override this method to clean up any state they
    // keep per slot.  If it is overridden, the derived class's implementation
    // must call ConsumerBase::freeBufferLocked.
    //
    // This method must be called with mMutex locked.
    virtual void freeBufferLocked(int slotIndex);

    // abandonLocked puts the BufferQueue into the abandoned state, causing
    // all future operations on it to fail. This method rather than the public
    // abandon method should be overridden by child classes to add abandon-
    // time behavior.
    //
    // Derived classes should override this method to clean up any object
    // state they keep (as opposed to per-slot state).  If it is overridden,
    // the derived class's implementation must call ConsumerBase::abandonLocked.
    //
    // This method must be called with mMutex locked.
    virtual void abandonLocked();

    // dumpLocked dumps the current state of the ConsumerBase object to the
    // result string.  Each line is prefixed with the string pointed to by the
    // prefix argument.  The buffer argument points to a buffer that may be
    // used for intermediate formatting data, and the size of that buffer is
    // indicated by the size argument.
    //
    // Derived classes should override this method to dump their internal
    // state.  If this method is overridden the derived class's implementation
    // should call ConsumerBase::dumpLocked.
    //
    // This method must be called with mMutex locked.
    virtual void dumpLocked(String8& result, const char* prefix) const;

    // acquireBufferLocked fetches the next buffer from the BufferQueue and
    // updates the buffer slot for the buffer returned.
    //
    // Derived classes should override this method to perform any
    // initialization that must take place the first time a buffer is assigned
    // to a slot.  If it is overridden the derived class's implementation must
    // call ConsumerBase::acquireBufferLocked.
    virtual status_t acquireBufferLocked(BufferItem *item, nsecs_t presentWhen,
            uint64_t maxFrameNumber = 0);

    // releaseBufferLocked relinquishes control over a buffer, returning that
    // control to the BufferQueue.
    //
    // Derived classes should override this method to perform any cleanup that
    // must take place when a buffer is released back to the BufferQueue.  If
    // it is overridden the derived class's implementation must call
    // ConsumerBase::releaseBufferLocked.
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_GL_FENCE_CLEANUP)
    virtual status_t releaseBufferLocked(int slot, const sp<GraphicBuffer> graphicBuffer);
#else
    virtual status_t releaseBufferLocked(int slot,
            const sp<GraphicBuffer> graphicBuffer,
            EGLDisplay display = EGL_NO_DISPLAY, EGLSyncKHR eglFence = EGL_NO_SYNC_KHR);
#endif
    // returns true iff the slot still has the graphicBuffer in it.
    bool stillTracking(int slot, const sp<GraphicBuffer> graphicBuffer);

    // addReleaseFence* adds the sync points associated with a fence to the set
    // of sync points that must be reached before the buffer in the given slot
    // may be used after the slot has been released.  This should be called by
    // derived classes each time some asynchronous work is kicked off that
    // references the buffer.
    status_t addReleaseFence(int slot,
            const sp<GraphicBuffer> graphicBuffer, const sp<Fence>& fence);
    status_t addReleaseFenceLocked(int slot,
            const sp<GraphicBuffer> graphicBuffer, const sp<Fence>& fence);

    // Slot contains the information and object references that
    // ConsumerBase maintains about a BufferQueue buffer slot.
    struct Slot {
        // mGraphicBuffer is the Gralloc buffer store in the slot or NULL if
        // no Gralloc buffer is in the slot.
        sp<GraphicBuffer> mGraphicBuffer;

        // mFence is a fence which will signal when the buffer associated with
        // this buffer slot is no longer being used by the consumer and can be
        // overwritten. The buffer can be dequeued before the fence signals;
        // the producer is responsible for delaying writes until it signals.
        sp<Fence> mFence;

        // the frame number of the last acquired frame for this slot
        uint64_t mFrameNumber;
    };

    // mSlots stores the buffers that have been allocated by the BufferQueue
    // for each buffer slot.  It is initialized to null pointers, and gets
    // filled in with the result of BufferQueue::acquire when the
    // client dequeues a buffer from a
    // slot that has not yet been used. The buffer allocated to a slot will also
    // be replaced if the requested buffer usage or geometry differs from that
    // of the buffer allocated to a slot.
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    std::vector<Slot> mSlots;
#else
    Slot mSlots[BufferQueueDefs::NUM_BUFFER_SLOTS];
#endif

    // mAbandoned indicates that the BufferQueue will no longer be used to
    // consume images buffers pushed to it using the IGraphicBufferProducer
    // interface. It is initialized to false, and set to true in the abandon
    // method.  A BufferQueue that has been abandoned will return the NO_INIT
    // error from all IConsumerBase methods capable of returning an error.
    bool mAbandoned;

    // mName is a string used to identify the ConsumerBase in log messages.
    // It can be set by the setName method.
    String8 mName;

    // mFrameAvailableListener is the listener object that will be called when a
    // new frame becomes available. If it is not NULL it will be called from
    // queueBuffer. The listener object is protected by mFrameAvailableMutex
    // (not mMutex).
    Mutex mFrameAvailableMutex;
    wp<FrameAvailableListener> mFrameAvailableListener;

    // The ConsumerBase has-a BufferQueue and is responsible for creating these
    // objects if not supplied.
    sp<IGraphicBufferConsumer> mConsumer;

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
    // This Surface wraps the IGraphicBufferConsumer created for this
    // ConsumerBase.
    sp<Surface> mSurface;
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)

    // The final release fence of the most recent buffer released by
    // releaseBufferLocked.
    sp<Fence> mPrevFinalReleaseFence;

    // mMutex is the mutex used to prevent concurrent access to the member
    // variables of ConsumerBase objects. It must be locked whenever the
    // member variables are accessed or when any of the *Locked methods are
    // called.
    //
    // This mutex is intended to be locked by derived classes.
    mutable Mutex mMutex;
};

// ----------------------------------------------------------------------------
}; // namespace android

#endif // ANDROID_GUI_CONSUMERBASE_H
