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

#ifndef ANDROID_GUI_IGRAPHICBUFFERPRODUCER_H
#define ANDROID_GUI_IGRAPHICBUFFERPRODUCER_H

#include <stdint.h>
#include <sys/types.h>
#include <optional>

#include <utils/Errors.h>
#include <utils/RefBase.h>

#include <binder/IInterface.h>

#include <ui/BufferQueueDefs.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <ui/PictureProfileHandle.h>
#include <ui/Rect.h>
#include <ui/Region.h>

#include <gui/AdditionalOptions.h>
#include <gui/FrameTimestamps.h>
#include <gui/HdrMetadata.h>

#include <hidl/HybridInterface.h>
#include <android/hardware/graphics/bufferqueue/1.0/IGraphicBufferProducer.h>
#include <android/hardware/graphics/bufferqueue/2.0/IGraphicBufferProducer.h>

#include <optional>
#include <vector>

#include <com_android_graphics_libgui_flags.h>

namespace android {
// ----------------------------------------------------------------------------

class IProducerListener;
class NativeHandle;
class Surface;

using HGraphicBufferProducerV1_0 =
        ::android::hardware::graphics::bufferqueue::V1_0::
        IGraphicBufferProducer;
using HGraphicBufferProducerV2_0 =
        ::android::hardware::graphics::bufferqueue::V2_0::
        IGraphicBufferProducer;

/*
 * This class defines the Binder IPC interface for the producer side of
 * a queue of graphics buffers.  It's used to send graphics data from one
 * component to another.  For example, a class that decodes video for
 * playback might use this to provide frames.  This is typically done
 * indirectly, through Surface.
 *
 * The underlying mechanism is a BufferQueue, which implements
 * BnGraphicBufferProducer.  In normal operation, the producer calls
 * dequeueBuffer() to get an empty buffer, fills it with data, then
 * calls queueBuffer() to make it available to the consumer.
 *
 * BufferQueues have a size, which we'll refer to in other comments as
 * SLOT_COUNT. Its default is 64 (NUM_BUFFER_SLOTS). It can be adjusted by
 * the IGraphicBufferConsumer::setMaxBufferCount, or when
 * IGraphicBufferConsumer::allowUnlimitedSlots is set to true, by
 * IGraphicBufferProducer::extendSlotCount. The actual number of buffers in use
 * is a function of various configurations, including whether we're in single
 * buffer mode, the maximum dequeuable/aquirable buffers, and SLOT_COUNT.
 *
 * This class was previously called ISurfaceTexture.
 */
#ifndef NO_BINDER
class IGraphicBufferProducer : public IInterface {
    DECLARE_HYBRID_META_INTERFACE(GraphicBufferProducer,
                                  HGraphicBufferProducerV1_0,
                                  HGraphicBufferProducerV2_0)
#else
class IGraphicBufferProducer : public RefBase {
#endif
public:
    enum {
        // A flag returned by dequeueBuffer when the client needs to call
        // requestBuffer immediately thereafter.
        BUFFER_NEEDS_REALLOCATION = BufferQueueDefs::BUFFER_NEEDS_REALLOCATION,
        // A flag returned by dequeueBuffer when all mirrored slots should be
        // released by the client. This flag should always be processed first.
        RELEASE_ALL_BUFFERS       = BufferQueueDefs::RELEASE_ALL_BUFFERS,
    };

    enum {
        // A parcelable magic indicates using Binder BufferQueue as transport
        // backend.
        USE_BUFFER_QUEUE = 0x62717565, // 'bque'
        // A parcelable magic indicates using BufferHub as transport backend.
        USE_BUFFER_HUB = 0x62687562, // 'bhub'
    };

    // requestBuffer requests a new buffer for the given index. The server (i.e.
    // the IGraphicBufferProducer implementation) assigns the newly created
    // buffer to the given slot index, and the client is expected to mirror the
    // slot->buffer mapping so that it's not necessary to transfer a
    // GraphicBuffer for every dequeue operation.
    //
    // The slot must be in the range of [0, SLOT_COUNT).
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned or the producer is not
    //             connected.
    // * BAD_VALUE - one of the two conditions occurred:
    //              * slot was out of range (see above)
    //              * buffer specified by the slot is not dequeued
    virtual status_t requestBuffer(int slot, sp<GraphicBuffer>* buf) = 0;

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
    // extendSlotCount sets the maximum slot count (SLOT_COUNT) to the given
    //  size. This feature must be enabled by the consumer to function via
    // IGraphicBufferConsumer::allowUnlimitedSlots. This must be called before
    // the producer connects.
    //
    // After calling this, any slot can be returned in the [0, size) range.
    // Callers are responsible for the allocation of the appropriate slots
    // array for their own buffer cache.
    //
    // On success, the consumer is notified (so that it can increase its own
    // slot cache).
    //
    // Return of a value other than NO_ERROR means that an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned
    // * INVALID_OPERATION - one of the following conditions has occurred:
    //                     *  The producer is connected already
    //                     *  The consumer didn't call allowUnlimitedSlots
    // * BAD_VALUE - The value is smaller than the previous max size
    //               (initialized to 64, then whatever the last call to this
    //               was)
    virtual status_t extendSlotCount(int size);
#endif

    // setMaxDequeuedBufferCount sets the maximum number of buffers that can be
    // dequeued by the producer at one time. If this method succeeds, any new
    // buffer slots will be both unallocated and owned by the BufferQueue object
    // (i.e. they are not owned by the producer or consumer). Calling this may
    // also cause some buffer slots to be emptied. If the caller is caching the
    // contents of the buffer slots, it should empty that cache after calling
    // this method.
    //
    // This function should not be called with a value of maxDequeuedBuffers
    // that is less than the number of currently dequeued buffer slots. Doing so
    // will result in a BAD_VALUE error.
    //
    // The buffer count should be at least 1 (inclusive), but at most
    // (SLOT_COUNT - the minimum undequeued buffer count) (exclusive). The
    // minimum undequeued buffer count can be obtained by calling
    // query(NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS).
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned.
    // * BAD_VALUE - one of the below conditions occurred:
    //     * bufferCount was out of range (see above).
    //     * client would have more than the requested number of dequeued
    //       buffers after this call.
    //     * this call would cause the maxBufferCount value to be exceeded.
    //     * failure to adjust the number of available slots.
    virtual status_t setMaxDequeuedBufferCount(int maxDequeuedBuffers) = 0;

    // Set the async flag if the producer intends to asynchronously queue
    // buffers without blocking. Typically this is used for triple-buffering
    // and/or when the swap interval is set to zero.
    //
    // Enabling async mode will internally allocate an additional buffer to
    // allow for the asynchronous behavior. If it is not enabled queue/dequeue
    // calls may block.
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned.
    // * BAD_VALUE - one of the following has occurred:
    //             * this call would cause the maxBufferCount value to be
    //               exceeded
    //             * failure to adjust the number of available slots.
    virtual status_t setAsyncMode(bool async) = 0;

    // dequeueBuffer requests a new buffer slot for the client to use. Ownership
    // of the slot is transfered to the client, meaning that the server will not
    // use the contents of the buffer associated with that slot.
    //
    // The slot index returned may or may not contain a buffer (client-side).
    // If the slot is empty the client should call requestBuffer to assign a new
    // buffer to that slot.
    //
    // Once the client is done filling this buffer, it is expected to transfer
    // buffer ownership back to the server with either cancelBuffer on
    // the dequeued slot or to fill in the contents of its associated buffer
    // contents and call queueBuffer.
    //
    // If dequeueBuffer returns the BUFFER_NEEDS_REALLOCATION flag, the client is
    // expected to call requestBuffer immediately.
    //
    // If dequeueBuffer returns the RELEASE_ALL_BUFFERS flag, the client is
    // expected to release all of the mirrored slot->buffer mappings.
    //
    // The fence parameter will be updated to hold the fence associated with
    // the buffer. The contents of the buffer must not be overwritten until the
    // fence signals. If the fence is Fence::NO_FENCE, the buffer may be written
    // immediately.
    //
    // The width and height parameters must be no greater than the minimum of
    // GL_MAX_VIEWPORT_DIMS and GL_MAX_TEXTURE_SIZE (see: glGetIntegerv).
    // An error due to invalid dimensions might not be reported until
    // updateTexImage() is called.  If width and height are both zero, the
    // default values specified by setDefaultBufferSize() are used instead.
    //
    // If the format is 0, the default format will be used.
    //
    // The usage argument specifies gralloc buffer usage flags.  The values
    // are enumerated in <gralloc.h>, e.g. GRALLOC_USAGE_HW_RENDER.  These
    // will be merged with the usage flags specified by
    // IGraphicBufferConsumer::setConsumerUsageBits.
    //
    // This call will block until a buffer is available to be dequeued. If
    // both the producer and consumer are controlled by the app, then this call
    // can never block and will return WOULD_BLOCK if no buffer is available.
    //
    // A non-negative value with flags set (see above) will be returned upon
    // success.
    //
    // Return of a negative means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned or the producer is not
    //             connected.
    // * BAD_VALUE - both in async mode and buffer count was less than the
    //               max numbers of buffers that can be allocated at once.
    // * INVALID_OPERATION - cannot attach the buffer because it would cause
    //                       too many buffers to be dequeued, either because
    //                       the producer already has a single buffer dequeued
    //                       and did not set a buffer count, or because a
    //                       buffer count was set and this call would cause
    //                       it to be exceeded.
    // * WOULD_BLOCK - no buffer is currently available, and blocking is disabled
    //                 since both the producer/consumer are controlled by app
    // * NO_MEMORY - out of memory, cannot allocate the graphics buffer.
    // * TIMED_OUT - the timeout set by setDequeueTimeout was exceeded while
    //               waiting for a buffer to become available.
    //
    // All other negative values are an unknown error returned downstream
    // from the graphics allocator (typically errno).
    virtual status_t dequeueBuffer(int* slot, sp<Fence>* fence, uint32_t w, uint32_t h,
                                   PixelFormat format, uint64_t usage, uint64_t* outBufferAge,
                                   FrameEventHistoryDelta* outTimestamps) = 0;

    // detachBuffer attempts to remove all ownership of the buffer in the given
    // slot from the buffer queue. If this call succeeds, the slot will be
    // freed, and there will be no way to obtain the buffer from this interface.
    // The freed slot will remain unallocated until either it is selected to
    // hold a freshly allocated buffer in dequeueBuffer or a buffer is attached
    // to the slot. The buffer must have already been dequeued, and the caller
    // must already possesses the sp<GraphicBuffer> (i.e., must have called
    // requestBuffer).
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned or the producer is not
    //             connected.
    // * BAD_VALUE - the given slot number is invalid, either because it is
    //               out of the range [0, SLOT_COUNT), or because the slot it
    //               refers to is not currently dequeued and requested.
    virtual status_t detachBuffer(int slot) = 0;

    // detachNextBuffer is equivalent to calling dequeueBuffer, requestBuffer,
    // and detachBuffer in sequence, except for two things:
    //
    // 1) It is unnecessary to know the dimensions, format, or usage of the
    //    next buffer.
    // 2) It will not block, since if it cannot find an appropriate buffer to
    //    return, it will return an error instead.
    //
    // Only slots that are free but still contain a GraphicBuffer will be
    // considered, and the oldest of those will be returned. outBuffer is
    // equivalent to outBuffer from the requestBuffer call, and outFence is
    // equivalent to fence from the dequeueBuffer call.
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned or the producer is not
    //             connected.
    // * BAD_VALUE - either outBuffer or outFence were NULL.
    // * NO_MEMORY - no slots were found that were both free and contained a
    //               GraphicBuffer.
    virtual status_t detachNextBuffer(sp<GraphicBuffer>* outBuffer,
            sp<Fence>* outFence) = 0;

    // attachBuffer attempts to transfer ownership of a buffer to the buffer
    // queue. If this call succeeds, it will be as if this buffer was dequeued
    // from the returned slot number. As such, this call will fail if attaching
    // this buffer would cause too many buffers to be simultaneously dequeued.
    //
    // If attachBuffer returns the RELEASE_ALL_BUFFERS flag, the caller is
    // expected to release all of the mirrored slot->buffer mappings.
    //
    // A non-negative value with flags set (see above) will be returned upon
    // success.
    //
    // Return of a negative value means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned or the producer is not
    //             connected.
    // * BAD_VALUE - outSlot or buffer were NULL, invalid combination of
    //               async mode and buffer count override, or the generation
    //               number of the buffer did not match the buffer queue.
    // * INVALID_OPERATION - cannot attach the buffer because it would cause
    //                       too many buffers to be dequeued, either because
    //                       the producer already has a single buffer dequeued
    //                       and did not set a buffer count, or because a
    //                       buffer count was set and this call would cause
    //                       it to be exceeded.
    // * WOULD_BLOCK - no buffer slot is currently available, and blocking is
    //                 disabled since both the producer/consumer are
    //                 controlled by the app.
    // * TIMED_OUT - the timeout set by setDequeueTimeout was exceeded while
    //               waiting for a slot to become available.
    virtual status_t attachBuffer(int* outSlot,
            const sp<GraphicBuffer>& buffer) = 0;

    struct QueueBufferInput : public Flattenable<QueueBufferInput> {
        explicit inline QueueBufferInput(const Parcel& parcel) {
            parcel.read(*this);
        }

        // timestamp - a monotonically increasing value in nanoseconds
        // isAutoTimestamp - if the timestamp was synthesized at queue time
        // dataSpace - description of the contents, interpretation depends on format
        // crop - a crop rectangle that's used as a hint to the consumer
        // scalingMode - a set of flags from NATIVE_WINDOW_SCALING_* in <window.h>
        // transform - a set of flags from NATIVE_WINDOW_TRANSFORM_* in <window.h>
        // fence - a fence that the consumer must wait on before reading the buffer,
        //         set this to Fence::NO_FENCE if the buffer is ready immediately
        // sticky - the sticky transform set in Surface (only used by the LEGACY
        //          camera mode).
        // getFrameTimestamps - whether or not the latest frame timestamps
        //                      should be retrieved from the consumer.
        // slot - the slot index to queue. This is used only by queueBuffers().
        //        queueBuffer() ignores this value and uses the argument `slot`
        //        instead.
        inline QueueBufferInput(int64_t _timestamp, bool _isAutoTimestamp,
                android_dataspace _dataSpace, const Rect& _crop,
                int _scalingMode, uint32_t _transform, const sp<Fence>& _fence,
                uint32_t _sticky = 0, bool _getFrameTimestamps = false,
                int _slot = -1)
                : timestamp(_timestamp), isAutoTimestamp(_isAutoTimestamp),
                  dataSpace(_dataSpace), crop(_crop), scalingMode(_scalingMode),
                  transform(_transform), stickyTransform(_sticky),
                  fence(_fence), surfaceDamage(),
                  getFrameTimestamps(_getFrameTimestamps), slot(_slot) { }

        QueueBufferInput() = default;

        inline void deflate(int64_t* outTimestamp, bool* outIsAutoTimestamp,
                android_dataspace* outDataSpace,
                Rect* outCrop, int* outScalingMode,
                uint32_t* outTransform, sp<Fence>* outFence,
                uint32_t* outStickyTransform = nullptr,
                bool* outGetFrameTimestamps = nullptr,
                int* outSlot = nullptr) const {
            *outTimestamp = timestamp;
            *outIsAutoTimestamp = bool(isAutoTimestamp);
            *outDataSpace = dataSpace;
            *outCrop = crop;
            *outScalingMode = scalingMode;
            *outTransform = transform;
            *outFence = fence;
            if (outStickyTransform != nullptr) {
                *outStickyTransform = stickyTransform;
            }
            if (outGetFrameTimestamps) {
                *outGetFrameTimestamps = getFrameTimestamps;
            }
            if (outSlot) {
                *outSlot = slot;
            }
        }

        // Flattenable protocol
        static constexpr size_t minFlattenedSize();
        size_t getFlattenedSize() const;
        size_t getFdCount() const;
        status_t flatten(void*& buffer, size_t& size, int*& fds, size_t& count) const;
        status_t unflatten(void const*& buffer, size_t& size, int const*& fds, size_t& count);

        const Region& getSurfaceDamage() const { return surfaceDamage; }
        void setSurfaceDamage(const Region& damage) { surfaceDamage = damage; }

        const HdrMetadata& getHdrMetadata() const { return hdrMetadata; }
        void setHdrMetadata(const HdrMetadata& metadata) { hdrMetadata = metadata; }

        const std::optional<PictureProfileHandle>& getPictureProfileHandle() const {
            return pictureProfileHandle;
        }
        void setPictureProfileHandle(const PictureProfileHandle& profile) {
            pictureProfileHandle = profile;
        }
        void clearPictureProfileHandle() { pictureProfileHandle = std::nullopt; }

        int64_t timestamp{0};
        int isAutoTimestamp{0};
        android_dataspace dataSpace{HAL_DATASPACE_UNKNOWN};
        Rect crop;
        int scalingMode{0};
        uint32_t transform{0};
        uint32_t stickyTransform{0};
        sp<Fence> fence;
        Region surfaceDamage;
        bool getFrameTimestamps{false};
        int slot{-1};
        HdrMetadata hdrMetadata;
        std::optional<PictureProfileHandle> pictureProfileHandle;
    };

    struct QueueBufferOutput : public Flattenable<QueueBufferOutput> {
        QueueBufferOutput() = default;

        // Moveable.
        QueueBufferOutput(QueueBufferOutput&& src) = default;
        QueueBufferOutput& operator=(QueueBufferOutput&& src) = default;
        // Not copyable.
        QueueBufferOutput(const QueueBufferOutput& src) = delete;
        QueueBufferOutput& operator=(const QueueBufferOutput& src) = delete;

        // Flattenable protocol
        static constexpr size_t minFlattenedSize();
        size_t getFlattenedSize() const;
        size_t getFdCount() const;
        status_t flatten(void*& buffer, size_t& size, int*& fds, size_t& count) const;
        status_t unflatten(void const*& buffer, size_t& size, int const*& fds, size_t& count);

        uint32_t width{0};
        uint32_t height{0};
        uint32_t transformHint{0};
        uint32_t numPendingBuffers{0};
        uint64_t nextFrameNumber{0};
        FrameEventHistoryDelta frameTimestamps;
        bool bufferReplaced{false};
        int maxBufferCount{BufferQueueDefs::NUM_BUFFER_SLOTS};
        bool isSlotExpansionAllowed{false};
        status_t result{NO_ERROR};
    };

    // queueBuffer indicates that the client has finished filling in the
    // contents of the buffer associated with slot and transfers ownership of
    // that slot back to the server.
    //
    // It is not valid to call queueBuffer on a slot that is not owned
    // by the client or one for which a buffer associated via requestBuffer
    // (an attempt to do so will fail with a return value of BAD_VALUE).
    //
    // In addition, the input must be described by the client (as documented
    // below). Any other properties (zero point, etc)
    // are client-dependent, and should be documented by the client.
    //
    // The slot must be in the range of [0, SLOT_COUNT).
    //
    // Upon success, the output will be filled with meaningful values
    // (refer to the documentation below).
    //
    // Note: QueueBufferInput::slot was added to QueueBufferInput to be used by
    // queueBuffers(), the batched version of queueBuffer(). The non-batched
    // method (queueBuffer()) uses `slot` and ignores `input.slot`.
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned or the producer is not
    //             connected.
    // * BAD_VALUE - one of the below conditions occurred:
    //              * fence was NULL
    //              * scaling mode was unknown
    //              * both in async mode and buffer count was less than the
    //                max numbers of buffers that can be allocated at once
    //              * slot index was out of range (see above).
    //              * the slot was not in the dequeued state
    //              * the slot was enqueued without requesting a buffer
    //              * crop rect is out of bounds of the buffer dimensions
    virtual status_t queueBuffer(int slot, const QueueBufferInput& input,
            QueueBufferOutput* output) = 0;

    // cancelBuffer indicates that the client does not wish to fill in the
    // buffer associated with slot and transfers ownership of the slot back to
    // the server.
    //
    // The buffer is not queued for use by the consumer.
    //
    // The slot must be in the range of [0, SLOT_COUNT).
    //
    // The buffer will not be overwritten until the fence signals.  The fence
    // will usually be the one obtained from dequeueBuffer.
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned or the producer is not
    //             connected.
    // * BAD_VALUE - one of the below conditions occurred:
    //              * fence was NULL
    //              * slot index was out of range (see above).
    //              * the slot was not in the dequeued state
    virtual status_t cancelBuffer(int slot, const sp<Fence>& fence) = 0;

    // query retrieves some information for this surface
    // 'what' tokens allowed are that of NATIVE_WINDOW_* in <window.h>
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - the buffer queue has been abandoned.
    // * BAD_VALUE - what was out of range
    virtual int query(int what, int* value) = 0;

    // connect attempts to connect a client API to the IGraphicBufferProducer.
    // This must be called before any other IGraphicBufferProducer methods are
    // called except for getAllocator. A consumer must be already connected.
    //
    // This method will fail if the connect was previously called on the
    // IGraphicBufferProducer and no corresponding disconnect call was made.
    //
    // The listener is an optional binder callback object that can be used if
    // the producer wants to be notified when the consumer releases a buffer
    // back to the BufferQueue. It is also used to detect the death of the
    // producer. If only the latter functionality is desired, there is a
    // StubProducerListener class in IProducerListener.h that can be used.
    //
    // The api should be one of the NATIVE_WINDOW_API_* values in <window.h>
    //
    // The producerControlledByApp should be set to true if the producer is hosted
    // by an untrusted process (typically app_process-forked processes). If both
    // the producer and the consumer are app-controlled then all buffer queues
    // will operate in async mode regardless of the async flag.
    //
    // Upon success, the output will be filled with meaningful data
    // (refer to QueueBufferOutput documentation above).
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - one of the following occurred:
    //             * the buffer queue was abandoned
    //             * no consumer has yet connected
    // * BAD_VALUE - one of the following has occurred:
    //             * the producer is already connected
    //             * api was out of range (see above).
    //             * output was NULL.
    //             * Failure to adjust the number of available slots. This can
    //               happen because of trying to allocate/deallocate the async
    //               buffer in response to the value of producerControlledByApp.
    // * DEAD_OBJECT - the token is hosted by an already-dead process
    //
    // Additional negative errors may be returned by the internals, they
    // should be treated as opaque fatal unrecoverable errors.
    virtual status_t connect(const sp<IProducerListener>& listener,
            int api, bool producerControlledByApp, QueueBufferOutput* output) = 0;

    enum class DisconnectMode {
        // Disconnect only the specified API.
        Api,
        // Disconnect any API originally connected from the process calling disconnect.
        AllLocal
    };

    // disconnect attempts to disconnect a client API from the
    // IGraphicBufferProducer.  Calling this method will cause any subsequent
    // calls to other IGraphicBufferProducer methods to fail except for
    // getAllocator and connect.  Successfully calling connect after this will
    // allow the other methods to succeed again.
    //
    // The api should be one of the NATIVE_WINDOW_API_* values in <window.h>
    //
    // Alternatively if mode is AllLocal, then the API value is ignored, and any API
    // connected from the same PID calling disconnect will be disconnected.
    //
    // Disconnecting from an abandoned IGraphicBufferProducer is legal and
    // is considered a no-op.
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * NO_INIT - the producer is not connected
    // * BAD_VALUE - one of the following has occurred:
    //             * the api specified does not match the one that was connected
    //             * api was out of range (see above).
    // * DEAD_OBJECT - the token is hosted by an already-dead process
    virtual status_t disconnect(int api, DisconnectMode mode = DisconnectMode::Api) = 0;

    // Attaches a sideband buffer stream to the IGraphicBufferProducer.
    //
    // A sideband stream is a device-specific mechanism for passing buffers
    // from the producer to the consumer without using dequeueBuffer/
    // queueBuffer. If a sideband stream is present, the consumer can choose
    // whether to acquire buffers from the sideband stream or from the queued
    // buffers.
    //
    // Passing NULL or a different stream handle will detach the previous
    // handle if any.
    virtual status_t setSidebandStream(const sp<NativeHandle>& stream) = 0;

    // Allocates buffers based on the given dimensions/format.
    //
    // This function will allocate up to the maximum number of buffers
    // permitted by the current BufferQueue configuration. It will use the
    // given format, dimensions, and usage bits, which are interpreted in the
    // same way as for dequeueBuffer, and the async flag must be set the same
    // way as for dequeueBuffer to ensure that the correct number of buffers are
    // allocated. This is most useful to avoid an allocation delay during
    // dequeueBuffer. If there are already the maximum number of buffers
    // allocated, this function has no effect.
    virtual void allocateBuffers(uint32_t width, uint32_t height,
            PixelFormat format, uint64_t usage) = 0;

    // Sets whether dequeueBuffer is allowed to allocate new buffers.
    //
    // Normally dequeueBuffer does not discriminate between free slots which
    // already have an allocated buffer and those which do not, and will
    // allocate a new buffer if the slot doesn't have a buffer or if the slot's
    // buffer doesn't match the requested size, format, or usage. This method
    // allows the producer to restrict the eligible slots to those which already
    // have an allocated buffer of the correct size, format, and usage. If no
    // eligible slot is available, dequeueBuffer will block or return an error
    // as usual.
    virtual status_t allowAllocation(bool allow) = 0;

    // Sets the current generation number of the BufferQueue.
    //
    // This generation number will be inserted into any buffers allocated by the
    // BufferQueue, and any attempts to attach a buffer with a different
    // generation number will fail. Buffers already in the queue are not
    // affected and will retain their current generation number. The generation
    // number defaults to 0.
    virtual status_t setGenerationNumber(uint32_t generationNumber) = 0;

    // Returns the name of the connected consumer.
    virtual String8 getConsumerName() const = 0;

    // Used to enable/disable shared buffer mode.
    //
    // When shared buffer mode is enabled the first buffer that is queued or
    // dequeued will be cached and returned to all subsequent calls to
    // dequeueBuffer and acquireBuffer. This allows the producer and consumer to
    // simultaneously access the same buffer.
    virtual status_t setSharedBufferMode(bool sharedBufferMode) = 0;

    // Used to enable/disable auto-refresh.
    //
    // Auto refresh has no effect outside of shared buffer mode. In shared
    // buffer mode, when enabled, it indicates to the consumer that it should
    // attempt to acquire buffers even if it is not aware of any being
    // available.
    virtual status_t setAutoRefresh(bool autoRefresh) = 0;

    // Sets how long dequeueBuffer will wait for a buffer to become available
    // before returning an error (TIMED_OUT).
    //
    // This timeout also affects the attachBuffer call, which will block if
    // there is not a free slot available into which the attached buffer can be
    // placed.
    //
    // By default, the BufferQueue will wait forever, which is indicated by a
    // timeout of -1. If set (to a value other than -1), this will disable
    // non-blocking mode and its corresponding spare buffer (which is used to
    // ensure a buffer is always available).
    //
    // Note well: queueBuffer will stop buffer dropping behavior if timeout is
    // strictly positive. If timeout is zero or negative, previous buffer
    // dropping behavior will not be changed.
    //
    // Return of a value other than NO_ERROR means an error has occurred:
    // * BAD_VALUE - Failure to adjust the number of available slots. This can
    //               happen because of trying to allocate/deallocate the async
    //               buffer.
    virtual status_t setDequeueTimeout(nsecs_t timeout) = 0;

    // Used to enable/disable buffer drop behavior of queueBuffer.
    // If it's not used, legacy drop behavior will be retained.
    virtual status_t setLegacyBufferDrop(bool drop);

    // Returns the last queued buffer along with a fence which must signal
    // before the contents of the buffer are read. If there are no buffers in
    // the queue, outBuffer will be populated with nullptr and outFence will be
    // populated with Fence::NO_FENCE
    //
    // outTransformMatrix is not modified if outBuffer is null.
    //
    // Returns NO_ERROR or the status of the Binder transaction
    virtual status_t getLastQueuedBuffer(sp<GraphicBuffer>* outBuffer,
            sp<Fence>* outFence, float outTransformMatrix[16]) = 0;

    // Returns the last queued buffer along with a fence which must signal
    // before the contents of the buffer are read. If there are no buffers in
    // the queue, outBuffer will be populated with nullptr and outFence will be
    // populated with Fence::NO_FENCE
    //
    // outRect & outTransform are not modified if outBuffer is null.
    //
    // Returns NO_ERROR or the status of the Binder transaction
    virtual status_t getLastQueuedBuffer([[maybe_unused]] sp<GraphicBuffer>* outBuffer,
                                         [[maybe_unused]] sp<Fence>* outFence,
                                         [[maybe_unused]] Rect* outRect,
                                         [[maybe_unused]] uint32_t* outTransform) {
        // Too many things implement IGraphicBufferProducer...
        return UNKNOWN_TRANSACTION;
    }

    // Gets the frame events that haven't already been retrieved.
    virtual void getFrameTimestamps(FrameEventHistoryDelta* /*outDelta*/) {}

    // Returns a unique id for this BufferQueue
    virtual status_t getUniqueId(uint64_t* outId) const = 0;

    // Returns the consumer usage flags for this BufferQueue. This returns the
    // full 64-bit usage flags, rather than the truncated 32-bit usage flags
    // returned by querying the now deprecated
    // NATIVE_WINDOW_CONSUMER_USAGE_BITS attribute.
    virtual status_t getConsumerUsage(uint64_t* outUsage) const = 0;

    // Enable/disable the auto prerotation at buffer allocation when the buffer
    // size is driven by the consumer.
    //
    // When buffer size is driven by the consumer and the transform hint
    // specifies a 90 or 270 degree rotation, if auto prerotation is enabled,
    // the width and height used for dequeueBuffer will be additionally swapped.
    virtual status_t setAutoPrerotation(bool autoPrerotation);

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_SETFRAMERATE)
    // Sets the apps intended frame rate.
    virtual status_t setFrameRate(float frameRate, int8_t compatibility,
                                  int8_t changeFrameRateStrategy);
#endif

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_EXTENDEDALLOCATE)
    virtual status_t setAdditionalOptions(const std::vector<gui::AdditionalOptions>& options);
#endif

    struct RequestBufferOutput : public Flattenable<RequestBufferOutput> {
        RequestBufferOutput() = default;

        // Flattenable protocol
        static constexpr size_t minFlattenedSize();
        size_t getFlattenedSize() const;
        size_t getFdCount() const;
        status_t flatten(void*& buffer, size_t& size, int*& fds, size_t& count) const;
        status_t unflatten(void const*& buffer, size_t& size, int const*& fds, size_t& count);

        status_t result;
        sp<GraphicBuffer> buffer;
    };

    // Batched version of requestBuffer().
    // This method behaves like a sequence of requestBuffer() calls.
    // The return value of the batched method will only be about the
    // transaction. For a local call, the return value will always be NO_ERROR.
    virtual status_t requestBuffers(
            const std::vector<int32_t>& slots,
            std::vector<RequestBufferOutput>* outputs);

    struct DequeueBufferInput : public LightFlattenable<DequeueBufferInput> {
        DequeueBufferInput() = default;

        // LightFlattenable protocol
        inline bool isFixedSize() const { return true; }
        size_t getFlattenedSize() const;
        status_t flatten(void* buffer, size_t size) const;
        status_t unflatten(void const* buffer, size_t size);

        uint32_t width;
        uint32_t height;
        PixelFormat format;
        uint64_t usage;
        bool getTimestamps;
    };

    struct DequeueBufferOutput : public Flattenable<DequeueBufferOutput> {
        DequeueBufferOutput() = default;

        // Flattenable protocol
        static constexpr size_t minFlattenedSize();
        size_t getFlattenedSize() const;
        size_t getFdCount() const;
        status_t flatten(void*& buffer, size_t& size, int*& fds, size_t& count) const;
        status_t unflatten(void const*& buffer, size_t& size, int const*& fds, size_t& count);

        status_t result;
        int slot = -1;
        sp<Fence> fence = Fence::NO_FENCE;
        uint64_t bufferAge;
        std::optional<FrameEventHistoryDelta> timestamps;
    };

    // Batched version of dequeueBuffer().
    // This method behaves like a sequence of dequeueBuffer() calls.
    // The return value of the batched method will only be about the
    // transaction. For a local call, the return value will always be NO_ERROR.
    virtual status_t dequeueBuffers(
            const std::vector<DequeueBufferInput>& inputs,
            std::vector<DequeueBufferOutput>* outputs);

    // Batched version of detachBuffer().
    // This method behaves like a sequence of detachBuffer() calls.
    // The return value of the batched method will only be about the
    // transaction. For a local call, the return value will always be NO_ERROR.
    virtual status_t detachBuffers(const std::vector<int32_t>& slots,
                                   std::vector<status_t>* results);


    struct AttachBufferOutput : public LightFlattenable<AttachBufferOutput> {
        AttachBufferOutput() = default;

        // LightFlattenable protocol
        inline bool isFixedSize() const { return true; }
        size_t getFlattenedSize() const;
        status_t flatten(void* buffer, size_t size) const;
        status_t unflatten(void const* buffer, size_t size);

        status_t result;
        int slot;
    };
    // Batched version of attachBuffer().
    // This method behaves like a sequence of attachBuffer() calls.
    // The return value of the batched method will only be about the
    // transaction. For a local call, the return value will always be NO_ERROR.
    virtual status_t attachBuffers(
            const std::vector<sp<GraphicBuffer>>& buffers,
            std::vector<AttachBufferOutput>* outputs);

    // Batched version of queueBuffer().
    // This method behaves like a sequence of queueBuffer() calls.
    // The return value of the batched method will only be about the
    // transaction. For a local call, the return value will always be NO_ERROR.
    //
    // Note: QueueBufferInput::slot was added to QueueBufferInput to include the
    // `slot` input argument of the non-batched method queueBuffer().
    virtual status_t queueBuffers(const std::vector<QueueBufferInput>& inputs,
                                  std::vector<QueueBufferOutput>* outputs);

    struct CancelBufferInput : public Flattenable<CancelBufferInput> {
        CancelBufferInput() = default;

        // Flattenable protocol
        static constexpr size_t minFlattenedSize();
        size_t getFlattenedSize() const;
        size_t getFdCount() const;
        status_t flatten(void*& buffer, size_t& size, int*& fds, size_t& count) const;
        status_t unflatten(void const*& buffer, size_t& size, int const*& fds, size_t& count);

        int slot;
        sp<Fence> fence;
    };
    // Batched version of cancelBuffer().
    // This method behaves like a sequence of cancelBuffer() calls.
    // The return value of the batched method will only be about the
    // transaction. For a local call, the return value will always be NO_ERROR.
    virtual status_t cancelBuffers(
            const std::vector<CancelBufferInput>& inputs,
            std::vector<status_t>* results);

    struct QueryOutput : public LightFlattenable<QueryOutput> {
        QueryOutput() = default;

        // LightFlattenable protocol
        inline bool isFixedSize() const { return true; }
        size_t getFlattenedSize() const;
        status_t flatten(void* buffer, size_t size) const;
        status_t unflatten(void const* buffer, size_t size);

        status_t result;
        int64_t value;
    };
    // Batched version of query().
    // This method behaves like a sequence of query() calls.
    // The return value of the batched method will only be about the
    // transaction. For a local call, the return value will always be NO_ERROR.
    virtual status_t query(const std::vector<int32_t> inputs,
                           std::vector<QueryOutput>* outputs);

#ifndef NO_BINDER
    // Static method exports any IGraphicBufferProducer object to a parcel. It
    // handles null producer as well.
    static status_t exportToParcel(const sp<IGraphicBufferProducer>& producer,
                                   Parcel* parcel);

    // Factory method that creates a new IBGP instance from the parcel.
    static sp<IGraphicBufferProducer> createFromParcel(const Parcel* parcel);

protected:
    // Exports the current producer as a binder parcelable object. Note that the
    // producer must be disconnected to be exportable. After successful export,
    // the producer queue can no longer be connected again. Returns NO_ERROR
    // when the export is successful and writes an implementation defined
    // parcelable object into the parcel. For traditional Android BufferQueue,
    // it writes a strong binder object; for BufferHub, it writes a
    // ProducerQueueParcelable object.
    virtual status_t exportToParcel(Parcel* parcel);
#endif
};

// ----------------------------------------------------------------------------
#ifndef NO_BINDER
class BnGraphicBufferProducer : public BnInterface<IGraphicBufferProducer>
{
public:
    virtual status_t    onTransact( uint32_t code,
                                    const Parcel& data,
                                    Parcel* reply,
                                    uint32_t flags = 0);
};
#else
class BnGraphicBufferProducer : public IGraphicBufferProducer {
};
#endif

// ----------------------------------------------------------------------------
} // namespace android

#endif // ANDROID_GUI_IGRAPHICBUFFERPRODUCER_H
