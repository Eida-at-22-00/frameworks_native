/*
 * Copyright 2013 The Android Open Source Project
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

#include <optional>
#include <string>

#include <compositionengine/DisplaySurface.h>
#include <gui/BufferQueue.h>
#include <gui/ConsumerBase.h>
#include <gui/IGraphicBufferProducer.h>
#include <ui/DisplayId.h>

#include <ui/DisplayIdentification.h>

namespace android {

class HWComposer;
class IProducerListener;

/* This DisplaySurface implementation supports virtual displays, where GPU
 * and/or HWC compose into a buffer that is then passed to an arbitrary
 * consumer (the sink) running in another process.
 *
 * The simplest case is when the virtual display will never use the h/w
 * composer -- either the h/w composer doesn't support writing to buffers, or
 * there are more virtual displays than it supports simultaneously. In this
 * case, the GPU driver works directly with the output buffer queue, and
 * calls to the VirtualDisplay from SurfaceFlinger and DisplayHardware do
 * nothing.
 *
 * If h/w composer might be used, then each frame will fall into one of three
 * configurations: GPU-only, HWC-only, and MIXED composition. In all of these,
 * we must provide a FB target buffer and output buffer for the HWC set() call.
 *
 * In GPU-only composition, the GPU driver is given a buffer from the sink to
 * render into. When the GPU driver queues the buffer to the
 * VirtualDisplaySurface, the VirtualDisplaySurface holds onto it instead of
 * immediately queueing it to the sink. The buffer is used as both the FB
 * target and output buffer for HWC, though on these frames the HWC doesn't
 * do any work for this display and doesn't write to the output buffer. After
 * composition is complete, the buffer is queued to the sink.
 *
 * In HWC-only composition, the VirtualDisplaySurface dequeues a buffer from
 * the sink and passes it to HWC as both the FB target buffer and output
 * buffer. The HWC doesn't need to read from the FB target buffer, but does
 * write to the output buffer. After composition is complete, the buffer is
 * queued to the sink.
 *
 * On MIXED frames, things become more complicated, since some h/w composer
 * implementations can't read from and write to the same buffer. This class has
 * an internal BufferQueue that it uses as a scratch buffer pool. The GPU
 * driver is given a scratch buffer to render into. When it finishes rendering,
 * the buffer is queued and then immediately acquired by the
 * VirtualDisplaySurface. The scratch buffer is then used as the FB target
 * buffer for HWC, and a separate buffer is dequeued from the sink and used as
 * the HWC output buffer. When HWC composition is complete, the scratch buffer
 * is released and the output buffer is queued to the sink.
 */
class VirtualDisplaySurface : public compositionengine::DisplaySurface,
                              public BnGraphicBufferProducer,
                              private ConsumerBase {
public:
    VirtualDisplaySurface(HWComposer&, VirtualDisplayIdVariant,
                          const sp<IGraphicBufferProducer>& sink,
                          const sp<IGraphicBufferProducer>& bqProducer,
                          const sp<IGraphicBufferConsumer>& bqConsumer,
                          const std::string& name, bool secure);

    //
    // DisplaySurface interface
    //
    virtual status_t beginFrame(bool mustRecompose);
    virtual status_t prepareFrame(CompositionType);
    virtual status_t advanceFrame(float hdrSdrRatio);
    virtual void onFrameCommitted();
    virtual void dumpAsString(String8& result) const;
    virtual void resizeBuffers(const ui::Size&) override;
    virtual const sp<Fence>& getClientTargetAcquireFence() const override;
    // Virtual display surface needs to prepare the frame based on composition type. Skip
    // any client composition prediction.
    virtual bool supportsCompositionStrategyPrediction() const override { return false; };

private:
    enum Source : size_t {
        SOURCE_SINK = 0,
        SOURCE_SCRATCH = 1,

        ftl_first = SOURCE_SINK,
        ftl_last = SOURCE_SCRATCH,
    };

    virtual ~VirtualDisplaySurface();

    //
    // IGraphicBufferProducer interface, used by the GPU driver.
    //
    virtual status_t requestBuffer(int pslot, sp<GraphicBuffer>* outBuf);
    virtual status_t setMaxDequeuedBufferCount(int maxDequeuedBuffers);
    virtual status_t setAsyncMode(bool async);
    virtual status_t dequeueBuffer(int* pslot, sp<Fence>*, uint32_t w, uint32_t h, PixelFormat,
                                   uint64_t usage, uint64_t* outBufferAge,
                                   FrameEventHistoryDelta* outTimestamps);
    virtual status_t detachBuffer(int slot);
    virtual status_t detachNextBuffer(sp<GraphicBuffer>* outBuffer, sp<Fence>* outFence);
    virtual status_t attachBuffer(int* slot, const sp<GraphicBuffer>&);
    virtual status_t queueBuffer(int pslot, const QueueBufferInput&, QueueBufferOutput*);
    virtual status_t cancelBuffer(int pslot, const sp<Fence>&);
    virtual int query(int what, int* value);
    virtual status_t connect(const sp<IProducerListener>&, int api, bool producerControlledByApp,
                             QueueBufferOutput*);
    virtual status_t disconnect(int api, DisconnectMode);
    virtual status_t setSidebandStream(const sp<NativeHandle>& stream);
    virtual void allocateBuffers(uint32_t width, uint32_t height, PixelFormat, uint64_t usage);
    virtual status_t allowAllocation(bool allow);
    virtual status_t setGenerationNumber(uint32_t);
    virtual String8 getConsumerName() const override;
    virtual status_t setSharedBufferMode(bool sharedBufferMode) override;
    virtual status_t setAutoRefresh(bool autoRefresh) override;
    virtual status_t setDequeueTimeout(nsecs_t timeout) override;
    virtual status_t getLastQueuedBuffer(sp<GraphicBuffer>* outBuffer,
            sp<Fence>* outFence, float outTransformMatrix[16]) override;
    virtual status_t getUniqueId(uint64_t* outId) const override;
    virtual status_t getConsumerUsage(uint64_t* outUsage) const override;
    virtual void setOutputUsage(uint64_t flag);

    //
    // Utility methods
    //
    static Source fbSourceForCompositionType(CompositionType);
    static std::string toString(CompositionType);

    status_t dequeueBuffer(Source, PixelFormat, uint64_t usage, int* sslot, sp<Fence>*);
    void updateQueueBufferOutput(QueueBufferOutput&&);
    void resetPerFrameState();
    status_t refreshOutputBuffer();
    bool isBackedByGpu() const;

    // Both the sink and scratch buffer pools have their own set of slots
    // ("source slots", or "sslot"). We have to merge these into the single
    // set of slots used by the graphics producer ("producer slots" or "pslot") and
    // internally in the VirtualDisplaySurface. To minimize the number of times
    // a producer slot switches which source it comes from, we map source slot
    // numbers to producer slot numbers differently for each source.
    static int mapSource2ProducerSlot(Source, int sslot);
    static int mapProducer2SourceSlot(Source, int pslot);

    //
    // Immutable after construction
    //
    HWComposer& mHwc;
    const VirtualDisplayIdVariant mVirtualIdVariant;
    const std::string mDisplayName;
    sp<IGraphicBufferProducer> mSource[2]; // indexed by SOURCE_*
    uint32_t mDefaultOutputFormat;

    // Buffers that HWC has seen before, indexed by HWC slot number.
    // NOTE: The BufferQueue slot number is the same as the HWC slot number.
    uint64_t mHwcBufferIds[BufferQueue::NUM_BUFFER_SLOTS];

    //
    // Inter-frame state
    //

    // To avoid buffer reallocations, we track the buffer usage and format
    // we used on the previous frame and use it again on the new frame. If
    // the composition type changes or the GPU driver starts requesting
    // different usage/format, we'll get a new buffer.
    uint32_t mOutputFormat;
    uint64_t mOutputUsage;

    // Since we present a single producer interface to the GPU driver, but
    // are internally muxing between the sink and scratch producers, we have
    // to keep track of which source last returned each producer slot from
    // dequeueBuffer. Each bit in mProducerSlotSource corresponds to a producer
    // slot. Both mProducerSlotSource and mProducerBuffers are indexed by a
    // "producer slot"; see the mapSlot*() functions.
    uint64_t mProducerSlotSource;
    sp<GraphicBuffer> mProducerBuffers[BufferQueueDefs::NUM_BUFFER_SLOTS];

    // Need to propagate reallocation to VDS consumer.
    // Each bit corresponds to a producer slot.
    uint64_t mProducerSlotNeedReallocation;

    // The QueueBufferOutput with the latest info from the sink, and with the
    // transform hint cleared. Since we defer queueBuffer from the GPU driver
    // to the sink, we have to return the previous version.
    // Moves instead of copies are performed to avoid duplicate
    // FrameEventHistoryDeltas.
    QueueBufferOutput mQueueBufferOutput;

    // Details of the current sink buffer. These become valid when a buffer is
    // dequeued from the sink, and are used when queueing the buffer.
    uint32_t mSinkBufferWidth, mSinkBufferHeight;

    //
    // Intra-frame state
    //

    // Composition type and graphics buffer source for the current frame.
    // Valid after prepareFrame(), cleared in onFrameCommitted.
    CompositionType mCompositionType = CompositionType::Unknown;

    // mFbFence is the fence HWC should wait for before reading the framebuffer
    // target buffer.
    sp<Fence> mFbFence;

    // mOutputFence is the fence HWC should wait for before writing to the
    // output buffer.
    sp<Fence> mOutputFence;

    // Producer slot numbers for the buffers to use for HWC framebuffer target
    // and output.
    int mFbProducerSlot;
    int mOutputProducerSlot;

    // Debug only -- track the sequence of events in each frame so we can make
    // sure they happen in the order we expect. This class implicitly models
    // a state machine; this enum/variable makes it explicit.
    //
    // +-----------+-------------------+-------------+
    // | State     | Event             || Next State |
    // +-----------+-------------------+-------------+
    // | Idle      | beginFrame        || Begun      |
    // | Begun     | prepareFrame      || Prepared   |
    // | Prepared  | dequeueBuffer [1] || Gpu        |
    // | Prepared  | advanceFrame [2]  || Hwc        |
    // | Gpu       | queueBuffer       || GpuDone    |
    // | GpuDone   | advanceFrame      || Hwc        |
    // | Hwc       | onFrameCommitted  || Idle       |
    // +-----------+-------------------++------------+
    // [1] CompositionType::Gpu and CompositionType::Mixed frames.
    // [2] CompositionType::Hwc frames.
    //
    enum class DebugState {
        // no buffer dequeued, don't know anything about the next frame
        Idle,
        // output buffer dequeued, framebuffer source not yet known
        Begun,
        // output buffer dequeued, framebuffer source known but not provided
        // to GPU yet.
        Prepared,
        // GPU driver has a buffer dequeued
        Gpu,
        // GPU driver has queued the buffer, we haven't sent it to HWC yet
        GpuDone,
        // HWC has the buffer for this frame
        Hwc,

        ftl_last = Hwc
    };
    DebugState mDebugState = DebugState::Idle;
    CompositionType mDebugLastCompositionType = CompositionType::Unknown;

    bool mMustRecompose = false;

    bool mForceHwcCopy;
    bool mSecure;
    int mSinkUsage;
};

} // namespace android
