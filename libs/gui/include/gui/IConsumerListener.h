/*
 * Copyright (C) 2013 The Android Open Source Project
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

#pragma once

#include <utils/Errors.h>
#include <utils/RefBase.h>

#include <cstdint>

#include <com_android_graphics_libgui_flags.h>

namespace android {

class BufferItem;
class FrameEventHistoryDelta;
struct NewFrameEventsEntry;

// ConsumerListener is the interface through which the BufferQueue notifies the consumer of events
// that the consumer may wish to react to. Because the consumer will generally have a mutex that is
// locked during calls from the consumer to the BufferQueue, these calls from the BufferQueue to the
// consumer *MUST* be called only when the BufferQueue mutex is NOT locked.

class ConsumerListener : public virtual RefBase {
public:
    ConsumerListener() {}
    virtual ~ConsumerListener();

    // onDisconnect is called when a producer disconnects from the BufferQueue.
    virtual void onDisconnect() {} /* Asynchronous */

    // onFrameDequeued is called when a call to the BufferQueueProducer::dequeueBuffer successfully
    // returns a slot from the BufferQueue.
    virtual void onFrameDequeued(const uint64_t) {}

    // onFrameCancelled is called when the client calls cancelBuffer, thereby releasing the slot
    // back to the BufferQueue.
    virtual void onFrameCancelled(const uint64_t) {}

    // onFrameDetached is called after a successful detachBuffer() call while in asynchronous mode.
    virtual void onFrameDetached(const uint64_t) {}

    // onFrameAvailable is called from queueBuffer each time an additional frame becomes available
    // for consumption. This means that frames that are queued while in asynchronous mode only
    // trigger the callback if no previous frames are pending. Frames queued while in synchronous
    // mode always trigger the callback. The item passed to the callback will contain all of the
    // information about the queued frame except for its GraphicBuffer pointer, which will always be
    // null (except if the consumer is SurfaceFlinger).
    //
    // This is called without any lock held and can be called concurrently by multiple threads.
    virtual void onFrameAvailable(const BufferItem& item) = 0; /* Asynchronous */

    // onFrameReplaced is called from queueBuffer if the frame being queued is replacing an existing
    // slot in the queue. Any call to queueBuffer that doesn't call onFrameAvailable will call this
    // callback instead. The item passed to the callback will contain all of the information about
    // the queued frame except for its GraphicBuffer pointer, which will always be null.
    //
    // This is called without any lock held and can be called concurrently by multiple threads.
    virtual void onFrameReplaced(const BufferItem& /* item */) {} /* Asynchronous */

    // onBuffersReleased is called to notify the buffer consumer that the BufferQueue has released
    // its references to one or more GraphicBuffers contained in its slots. The buffer consumer
    // should then call BufferQueue::getReleasedBuffers to retrieve the list of buffers.
    //
    // This is called without any lock held and can be called concurrently by multiple threads.
    virtual void onBuffersReleased() = 0; /* Asynchronous */

    // onSidebandStreamChanged is called to notify the buffer consumer that the BufferQueue's
    // sideband buffer stream has changed. This is called when a stream is first attached and when
    // it is either detached or replaced by a different stream.
    virtual void onSidebandStreamChanged() = 0; /* Asynchronous */

    // Notifies the consumer of any new producer-side timestamps and returns the combined frame
    // history that hasn't already been retrieved.
    //
    // WARNING: This method can only be called when the BufferQueue is in the consumer's process.
    virtual void addAndGetFrameTimestamps(const NewFrameEventsEntry* /*newTimestamps*/,
                                          FrameEventHistoryDelta* /*outDelta*/) {}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_SETFRAMERATE)
    // Notifies the consumer of a setFrameRate call from the producer side.
    virtual void onSetFrameRate(float /*frameRate*/, int8_t /*compatibility*/,
                                int8_t /*changeFrameRateStrategy*/) {}
#endif

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    // Notifies the consumer that IGraphicBufferProducer::extendSlotCount has
    // been called and the total slot count has increased.
    //
    // This will only ever be called if
    // IGraphicBufferConsumer::allowUnlimitedSlots has been called on the
    // consumer.
    virtual void onSlotCountChanged(int /* slotCount */) {}
#endif
};

class IConsumerListener : public ConsumerListener {};

} // namespace android
