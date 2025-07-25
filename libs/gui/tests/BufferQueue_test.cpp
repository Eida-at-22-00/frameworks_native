/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "BufferQueue_test"
//#define LOG_NDEBUG 0

#include "Constants.h"
#include "MockConsumer.h"

#include <EGL/egl.h>

#include <gui/BufferItem.h>
#include <gui/BufferItemConsumer.h>
#include <gui/BufferQueue.h>
#include <gui/IProducerListener.h>
#include <gui/Surface.h>

#include <ui/GraphicBuffer.h>
#include <ui/PictureProfileHandle.h>

#include <android-base/properties.h>
#include <android-base/unique_fd.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>

#include <utils/String8.h>
#include <utils/threads.h>

#include <system/window.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <csignal>
#include <future>
#include <optional>
#include <thread>
#include <unordered_map>

#include <com_android_graphics_libgui_flags.h>

using namespace std::chrono_literals;

static bool IsCuttlefish() {
    return ::android::base::GetProperty("ro.product.board", "") == "cutf";
}

namespace android {
using namespace com::android::graphics::libgui;

class BufferQueueTest : public ::testing::Test {

public:
protected:
    void TearDown() override {
        std::vector<std::function<void()>> teardownFns;
        teardownFns.swap(mTeardownFns);

        for (auto& fn : teardownFns) {
            fn();
        }
    }

    void GetMinUndequeuedBufferCount(int* bufferCount) {
        ASSERT_TRUE(bufferCount != nullptr);
        ASSERT_EQ(OK, mProducer->query(NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
                    bufferCount));
        ASSERT_GE(*bufferCount, 0);
    }

    void createBufferQueue() {
        BufferQueue::createBufferQueue(&mProducer, &mConsumer);
    }

    void testBufferItem(const IGraphicBufferProducer::QueueBufferInput& input,
            const BufferItem& item) {
        int64_t timestamp;
        bool isAutoTimestamp;
        android_dataspace dataSpace;
        Rect crop;
        int scalingMode;
        uint32_t transform;
        sp<Fence> fence;

        input.deflate(&timestamp, &isAutoTimestamp, &dataSpace, &crop,
                &scalingMode, &transform, &fence, nullptr);
        ASSERT_EQ(timestamp, item.mTimestamp);
        ASSERT_EQ(isAutoTimestamp, item.mIsAutoTimestamp);
        ASSERT_EQ(dataSpace, item.mDataSpace);
        ASSERT_EQ(crop, item.mCrop);
        ASSERT_EQ(static_cast<uint32_t>(scalingMode), item.mScalingMode);
        ASSERT_EQ(transform, item.mTransform);
        ASSERT_EQ(fence, item.mFence);
    }

    sp<IGraphicBufferProducer> mProducer;
    sp<IGraphicBufferConsumer> mConsumer;
    std::vector<std::function<void()>> mTeardownFns;
};

static const uint32_t TEST_DATA = 0x12345678u;

// XXX: Tests that fork a process to hold the BufferQueue must run before tests
// that use a local BufferQueue, or else Binder will get unhappy
//
// TODO(b/392945118): In one instance this was a crash in the createBufferQueue
// where the binder call to create a buffer allocator apparently got garbage
// back. See b/36592665.
TEST_F(BufferQueueTest, DISABLED_BufferQueueInAnotherProcess) {
    const String16 PRODUCER_NAME = String16("BQTestProducer");

    base::unique_fd readfd, writefd;
    ASSERT_TRUE(base::Pipe(&readfd, &writefd));

    pid_t forkPid = fork();
    ASSERT_NE(forkPid, -1);

    if (forkPid == 0) {
        // Child process
        sp<IGraphicBufferProducer> producer;
        sp<IGraphicBufferConsumer> consumer;
        BufferQueue::createBufferQueue(&producer, &consumer);
        sp<IServiceManager> serviceManager = defaultServiceManager();
        serviceManager->addService(PRODUCER_NAME, IInterface::asBinder(producer));

        class ChildConsumerListener : public IConsumerListener {
        public:
            ChildConsumerListener(const sp<IGraphicBufferConsumer>& consumer,
                                  base::unique_fd&& writeFd)
                  : mConsumer(consumer), mWriteFd(std::move(writeFd)) {}

            virtual void onFrameAvailable(const BufferItem&) override {
                BufferItem item;
                ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));

                uint32_t* dataOut;
                ASSERT_EQ(OK,
                          item.mGraphicBuffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN,
                                                    reinterpret_cast<void**>(&dataOut)));
                ASSERT_EQ(*dataOut, TEST_DATA);
                ASSERT_EQ(OK, item.mGraphicBuffer->unlock());

                bool isOk = true;
                write(mWriteFd, &isOk, sizeof(bool));
            }
            virtual void onBuffersReleased() override {}
            virtual void onSidebandStreamChanged() override {}

        private:
            sp<IGraphicBufferConsumer> mConsumer;
            base::unique_fd mWriteFd;
        };

        sp<ChildConsumerListener> mc =
                sp<ChildConsumerListener>::make(consumer, std::move(writefd));
        ASSERT_EQ(OK, consumer->consumerConnect(mc, false));

        ProcessState::self()->startThreadPool();
        IPCThreadState::self()->joinThreadPool();
        LOG_ALWAYS_FATAL("Shouldn't be here");
    } else {
        mTeardownFns.emplace_back([forkPid]() { kill(forkPid, SIGTERM); });
    }

    sp<IServiceManager> serviceManager = defaultServiceManager();
    sp<IBinder> binderProducer = serviceManager->waitForService(PRODUCER_NAME);
    mProducer = interface_cast<IGraphicBufferProducer>(binderProducer);
    EXPECT_TRUE(mProducer != nullptr);

    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
            mProducer->connect(nullptr, NATIVE_WINDOW_API_CPU, false, &output));

    int slot;
    sp<Fence> fence;
    sp<GraphicBuffer> buffer;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                       nullptr, nullptr));
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));

    uint32_t* dataIn;
    ASSERT_EQ(OK, buffer->lock(GraphicBuffer::USAGE_SW_WRITE_OFTEN,
            reinterpret_cast<void**>(&dataIn)));
    *dataIn = TEST_DATA;
    ASSERT_EQ(OK, buffer->unlock());

    IGraphicBufferProducer::QueueBufferInput input(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

    bool isOk;
    read(readfd, &isOk, sizeof(bool));
    ASSERT_TRUE(isOk);
}

TEST_F(BufferQueueTest, GetMaxBufferCountInQueueBufferOutput_Succeeds) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    mConsumer->consumerConnect(mc, false);
    int bufferCount = 50;
    mConsumer->setMaxBufferCount(bufferCount);

    IGraphicBufferProducer::QueueBufferOutput output;
    mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &output);
    ASSERT_EQ(output.maxBufferCount, bufferCount);
}

TEST_F(BufferQueueTest, AcquireBuffer_ExceedsMaxAcquireCount_Fails) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    mConsumer->consumerConnect(mc, false);
    IGraphicBufferProducer::QueueBufferOutput qbo;
    mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &qbo);
    mProducer->setMaxDequeuedBufferCount(3);

    int slot;
    sp<Fence> fence;
    sp<GraphicBuffer> buf;
    IGraphicBufferProducer::QueueBufferInput qbi(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    BufferItem item;

    for (int i = 0; i < 2; i++) {
        ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
                  mProducer->dequeueBuffer(&slot, &fence, 1, 1, 0, GRALLOC_USAGE_SW_READ_OFTEN,
                                           nullptr, nullptr));
        ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buf));
        ASSERT_EQ(OK, mProducer->queueBuffer(slot, qbi, &qbo));
        ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    }

    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 1, 1, 0, GRALLOC_USAGE_SW_READ_OFTEN,
                                       nullptr, nullptr));
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buf));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, qbi, &qbo));

    // Acquire the third buffer, which should fail.
    ASSERT_EQ(INVALID_OPERATION, mConsumer->acquireBuffer(&item, 0));
}

TEST_F(BufferQueueTest, SetMaxAcquiredBufferCountWithIllegalValues_ReturnsError) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    mConsumer->consumerConnect(mc, false);

    EXPECT_EQ(OK, mConsumer->setMaxBufferCount(10));
    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxAcquiredBufferCount(10));

    IGraphicBufferProducer::QueueBufferOutput qbo;
    mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &qbo);
    mProducer->setMaxDequeuedBufferCount(3);

    int minBufferCount;
    ASSERT_NO_FATAL_FAILURE(GetMinUndequeuedBufferCount(&minBufferCount));
    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxAcquiredBufferCount(
                minBufferCount - 1));

    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxAcquiredBufferCount(0));
    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxAcquiredBufferCount(-3));
    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxAcquiredBufferCount(
            BufferQueue::MAX_MAX_ACQUIRED_BUFFERS+1));
    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxAcquiredBufferCount(100));

    int slot;
    sp<Fence> fence;
    sp<GraphicBuffer> buf;
    IGraphicBufferProducer::QueueBufferInput qbi(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    BufferItem item;
    EXPECT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(3));
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
                  mProducer->dequeueBuffer(&slot, &fence, 1, 1, 0, GRALLOC_USAGE_SW_READ_OFTEN,
                                           nullptr, nullptr));
        ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buf));
        ASSERT_EQ(OK, mProducer->queueBuffer(slot, qbi, &qbo));
        ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    }

    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxAcquiredBufferCount(2));
}

TEST_F(BufferQueueTest, SetMaxAcquiredBufferCountWithLegalValues_Succeeds) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    mConsumer->consumerConnect(mc, false);

    IGraphicBufferProducer::QueueBufferOutput qbo;
    mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &qbo);
    mProducer->setMaxDequeuedBufferCount(2);

    int minBufferCount;
    ASSERT_NO_FATAL_FAILURE(GetMinUndequeuedBufferCount(&minBufferCount));

    EXPECT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(1));
    EXPECT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(2));
    EXPECT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(minBufferCount));

    int slot;
    sp<Fence> fence;
    sp<GraphicBuffer> buf;
    IGraphicBufferProducer::QueueBufferInput qbi(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    BufferItem item;

    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 1, 1, 0, GRALLOC_USAGE_SW_READ_OFTEN,
                                       nullptr, nullptr));
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buf));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, qbi, &qbo));
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));

    EXPECT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(3));

    for (int i = 0; i < 2; i++) {
        ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
                  mProducer->dequeueBuffer(&slot, &fence, 1, 1, 0, GRALLOC_USAGE_SW_READ_OFTEN,
                                           nullptr, nullptr));
        ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buf));
        ASSERT_EQ(OK, mProducer->queueBuffer(slot, qbi, &qbo));
        ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    }

    EXPECT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(
            BufferQueue::MAX_MAX_ACQUIRED_BUFFERS));
}

TEST_F(BufferQueueTest, SetMaxBufferCountWithLegalValues_Succeeds) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    mConsumer->consumerConnect(mc, false);

    // Test shared buffer mode
    EXPECT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(1));
}

TEST_F(BufferQueueTest, SetMaxBufferCountWithIllegalValues_ReturnsError) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    mConsumer->consumerConnect(mc, false);

    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxBufferCount(0));
    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxBufferCount(
            BufferQueue::NUM_BUFFER_SLOTS + 1));

    EXPECT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(5));
    EXPECT_EQ(BAD_VALUE, mConsumer->setMaxBufferCount(3));
}

TEST_F(BufferQueueTest, DetachAndReattachOnProducerSide) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, false));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &output));

    ASSERT_EQ(BAD_VALUE, mProducer->detachBuffer(-1)); // Index too low
    ASSERT_EQ(BAD_VALUE, mProducer->detachBuffer(
                BufferQueueDefs::NUM_BUFFER_SLOTS)); // Index too high
    ASSERT_EQ(BAD_VALUE, mProducer->detachBuffer(0)); // Not dequeued

    int slot;
    sp<Fence> fence;
    sp<GraphicBuffer> buffer;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                       nullptr, nullptr));
    ASSERT_EQ(BAD_VALUE, mProducer->detachBuffer(slot)); // Not requested
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));
    ASSERT_EQ(OK, mProducer->detachBuffer(slot));
    ASSERT_EQ(BAD_VALUE, mProducer->detachBuffer(slot)); // Not dequeued

    sp<GraphicBuffer> safeToClobberBuffer;
    // Can no longer request buffer from this slot
    ASSERT_EQ(BAD_VALUE, mProducer->requestBuffer(slot, &safeToClobberBuffer));

    uint32_t* dataIn;
    ASSERT_EQ(OK, buffer->lock(GraphicBuffer::USAGE_SW_WRITE_OFTEN,
            reinterpret_cast<void**>(&dataIn)));
    *dataIn = TEST_DATA;
    ASSERT_EQ(OK, buffer->unlock());

    int newSlot;
    ASSERT_EQ(BAD_VALUE, mProducer->attachBuffer(nullptr, safeToClobberBuffer));
    ASSERT_EQ(BAD_VALUE, mProducer->attachBuffer(&newSlot, nullptr));

    ASSERT_EQ(OK, mProducer->attachBuffer(&newSlot, buffer));
    IGraphicBufferProducer::QueueBufferInput input(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    ASSERT_EQ(OK, mProducer->queueBuffer(newSlot, input, &output));

    BufferItem item;
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, static_cast<nsecs_t>(0)));

    uint32_t* dataOut;
    ASSERT_EQ(OK, item.mGraphicBuffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN,
            reinterpret_cast<void**>(&dataOut)));
    ASSERT_EQ(*dataOut, TEST_DATA);
    ASSERT_EQ(OK, item.mGraphicBuffer->unlock());
}

TEST_F(BufferQueueTest, DetachAndReattachOnConsumerSide) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, false));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &output));

    int slot;
    sp<Fence> fence;
    sp<GraphicBuffer> buffer;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                       nullptr, nullptr));
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));
    IGraphicBufferProducer::QueueBufferInput input(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

    ASSERT_EQ(BAD_VALUE, mConsumer->detachBuffer(-1)); // Index too low
    ASSERT_EQ(BAD_VALUE, mConsumer->detachBuffer(
            BufferQueueDefs::NUM_BUFFER_SLOTS)); // Index too high
    ASSERT_EQ(BAD_VALUE, mConsumer->detachBuffer(0)); // Not acquired

    BufferItem item;
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, static_cast<nsecs_t>(0)));

    ASSERT_EQ(OK, mConsumer->detachBuffer(item.mSlot));
    ASSERT_EQ(BAD_VALUE, mConsumer->detachBuffer(item.mSlot)); // Not acquired

    uint32_t* dataIn;
    ASSERT_EQ(OK, item.mGraphicBuffer->lock(
            GraphicBuffer::USAGE_SW_WRITE_OFTEN,
            reinterpret_cast<void**>(&dataIn)));
    *dataIn = TEST_DATA;
    ASSERT_EQ(OK, item.mGraphicBuffer->unlock());

    int newSlot;
    sp<GraphicBuffer> safeToClobberBuffer;
    ASSERT_EQ(BAD_VALUE, mConsumer->attachBuffer(nullptr, safeToClobberBuffer));
    ASSERT_EQ(BAD_VALUE, mConsumer->attachBuffer(&newSlot, nullptr));
    ASSERT_EQ(OK, mConsumer->attachBuffer(&newSlot, item.mGraphicBuffer));

    ASSERT_EQ(OK, mConsumer->releaseBuffer(newSlot, 0, Fence::NO_FENCE));

    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                       nullptr, nullptr));
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));

    uint32_t* dataOut;
    ASSERT_EQ(OK, buffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN,
            reinterpret_cast<void**>(&dataOut)));
    ASSERT_EQ(*dataOut, TEST_DATA);
    ASSERT_EQ(OK, buffer->unlock());
}

TEST_F(BufferQueueTest, MoveFromConsumerToProducer) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, false));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &output));

    int slot;
    sp<Fence> fence;
    sp<GraphicBuffer> buffer;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                       nullptr, nullptr));
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));

    uint32_t* dataIn;
    ASSERT_EQ(OK, buffer->lock(GraphicBuffer::USAGE_SW_WRITE_OFTEN,
            reinterpret_cast<void**>(&dataIn)));
    *dataIn = TEST_DATA;
    ASSERT_EQ(OK, buffer->unlock());

    IGraphicBufferProducer::QueueBufferInput input(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

    BufferItem item;
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, static_cast<nsecs_t>(0)));
    ASSERT_EQ(OK, mConsumer->detachBuffer(item.mSlot));

    int newSlot;
    ASSERT_EQ(OK, mProducer->attachBuffer(&newSlot, item.mGraphicBuffer));
    ASSERT_EQ(OK, mProducer->queueBuffer(newSlot, input, &output));
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, static_cast<nsecs_t>(0)));

    uint32_t* dataOut;
    ASSERT_EQ(OK, item.mGraphicBuffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN,
            reinterpret_cast<void**>(&dataOut)));
    ASSERT_EQ(*dataOut, TEST_DATA);
    ASSERT_EQ(OK, item.mGraphicBuffer->unlock());
}

TEST_F(BufferQueueTest, TestDisallowingAllocation) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, true, &output));

    static const uint32_t WIDTH = 320;
    static const uint32_t HEIGHT = 240;

    ASSERT_EQ(OK, mConsumer->setDefaultBufferSize(WIDTH, HEIGHT));

    int slot;
    sp<Fence> fence;
    sp<GraphicBuffer> buffer;
    // This should return an error since it would require an allocation
    ASSERT_EQ(OK, mProducer->allowAllocation(false));
    ASSERT_EQ(WOULD_BLOCK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                       nullptr, nullptr));

    // This should succeed, now that we've lifted the prohibition
    ASSERT_EQ(OK, mProducer->allowAllocation(true));
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                       nullptr, nullptr));

    // Release the previous buffer back to the BufferQueue
    mProducer->cancelBuffer(slot, fence);

    // This should fail since we're requesting a different size
    ASSERT_EQ(OK, mProducer->allowAllocation(false));
    ASSERT_EQ(WOULD_BLOCK,
              mProducer->dequeueBuffer(&slot, &fence, WIDTH * 2, HEIGHT * 2, 0,
                                       GRALLOC_USAGE_SW_WRITE_OFTEN, nullptr, nullptr));
}

TEST_F(BufferQueueTest, TestGenerationNumbers) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, true, &output));

    ASSERT_EQ(OK, mProducer->setGenerationNumber(1));

    // Get one buffer to play with
    int slot;
    sp<Fence> fence;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));

    sp<GraphicBuffer> buffer;
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));

    // Ensure that the generation number we set propagates to allocated buffers
    ASSERT_EQ(1U, buffer->getGenerationNumber());

    ASSERT_EQ(OK, mProducer->detachBuffer(slot));

    ASSERT_EQ(OK, mProducer->setGenerationNumber(2));

    // These should fail, since we've changed the generation number on the queue
    int outSlot;
    ASSERT_EQ(BAD_VALUE, mProducer->attachBuffer(&outSlot, buffer));
    ASSERT_EQ(BAD_VALUE, mConsumer->attachBuffer(&outSlot, buffer));

    buffer->setGenerationNumber(2);

    // This should succeed now that we've changed the buffer's generation number
    ASSERT_EQ(OK, mProducer->attachBuffer(&outSlot, buffer));

    ASSERT_EQ(OK, mProducer->detachBuffer(outSlot));

    // This should also succeed with the new generation number
    ASSERT_EQ(OK, mConsumer->attachBuffer(&outSlot, buffer));
}

TEST_F(BufferQueueTest, TestSharedBufferModeWithoutAutoRefresh) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, true, &output));

    ASSERT_EQ(OK, mProducer->setSharedBufferMode(true));

    // Get a buffer
    int sharedSlot;
    sp<Fence> fence;
    sp<GraphicBuffer> buffer;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&sharedSlot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                       nullptr, nullptr));
    ASSERT_EQ(OK, mProducer->requestBuffer(sharedSlot, &buffer));

    // Queue the buffer
    IGraphicBufferProducer::QueueBufferInput input(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    ASSERT_EQ(OK, mProducer->queueBuffer(sharedSlot, input, &output));

    // Repeatedly queue and dequeue a buffer from the producer side, it should
    // always return the same one. And we won't run out of buffers because it's
    // always the same one and because async mode gets enabled.
    int slot;
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(OK,
                  mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                           nullptr, nullptr));
        ASSERT_EQ(sharedSlot, slot);
        ASSERT_EQ(OK, mProducer->queueBuffer(sharedSlot, input, &output));
    }

    // acquire the buffer
    BufferItem item;
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(sharedSlot, item.mSlot);
    testBufferItem(input, item);
    ASSERT_EQ(true, item.mQueuedBuffer);
    ASSERT_EQ(false, item.mAutoRefresh);

    ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));

    // attempt to acquire a second time should return no buffer available
    ASSERT_EQ(IGraphicBufferConsumer::NO_BUFFER_AVAILABLE,
            mConsumer->acquireBuffer(&item, 0));
}

TEST_F(BufferQueueTest, TestSharedBufferModeWithAutoRefresh) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, true, &output));

    ASSERT_EQ(OK, mProducer->setSharedBufferMode(true));
    ASSERT_EQ(OK, mProducer->setAutoRefresh(true));

    // Get a buffer
    int sharedSlot;
    sp<Fence> fence;
    sp<GraphicBuffer> buffer;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&sharedSlot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                       nullptr, nullptr));
    ASSERT_EQ(OK, mProducer->requestBuffer(sharedSlot, &buffer));

    // Queue the buffer
    IGraphicBufferProducer::QueueBufferInput input(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    ASSERT_EQ(OK, mProducer->queueBuffer(sharedSlot, input, &output));

    // Repeatedly acquire and release a buffer from the consumer side, it should
    // always return the same one.
    BufferItem item;
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
        ASSERT_EQ(sharedSlot, item.mSlot);
        testBufferItem(input, item);
        ASSERT_EQ(i == 0, item.mQueuedBuffer);
        ASSERT_EQ(true, item.mAutoRefresh);

        ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));
    }

    // Repeatedly queue and dequeue a buffer from the producer side, it should
    // always return the same one.
    int slot;
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(OK,
                  mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                           nullptr, nullptr));
        ASSERT_EQ(sharedSlot, slot);
        ASSERT_EQ(OK, mProducer->queueBuffer(sharedSlot, input, &output));
    }

    // Repeatedly acquire and release a buffer from the consumer side, it should
    // always return the same one. First grabbing them from the queue and then
    // when the queue is empty, returning the shared buffer.
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
        ASSERT_EQ(sharedSlot, item.mSlot);
        ASSERT_EQ(0, item.mTimestamp);
        ASSERT_EQ(false, item.mIsAutoTimestamp);
        ASSERT_EQ(HAL_DATASPACE_UNKNOWN, item.mDataSpace);
        ASSERT_EQ(Rect(0, 0, 1, 1), item.mCrop);
        ASSERT_EQ(NATIVE_WINDOW_SCALING_MODE_FREEZE, item.mScalingMode);
        ASSERT_EQ(0u, item.mTransform);
        ASSERT_EQ(Fence::NO_FENCE, item.mFence);
        ASSERT_EQ(i == 0, item.mQueuedBuffer);
        ASSERT_EQ(true, item.mAutoRefresh);

        ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));
    }
}

TEST_F(BufferQueueTest, TestSharedBufferModeUsingAlreadyDequeuedBuffer) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, true, &output));

    // Dequeue a buffer
    int sharedSlot;
    sp<Fence> fence;
    sp<GraphicBuffer> buffer;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&sharedSlot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                       nullptr, nullptr));
    ASSERT_EQ(OK, mProducer->requestBuffer(sharedSlot, &buffer));

    // Enable shared buffer mode
    ASSERT_EQ(OK, mProducer->setSharedBufferMode(true));

    // Queue the buffer
    IGraphicBufferProducer::QueueBufferInput input(0, false,
            HAL_DATASPACE_UNKNOWN, Rect(0, 0, 1, 1),
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    ASSERT_EQ(OK, mProducer->queueBuffer(sharedSlot, input, &output));

    // Repeatedly queue and dequeue a buffer from the producer side, it should
    // always return the same one. And we won't run out of buffers because it's
    // always the same one and because async mode gets enabled.
    int slot;
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(OK,
                  mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                           nullptr, nullptr));
        ASSERT_EQ(sharedSlot, slot);
        ASSERT_EQ(OK, mProducer->queueBuffer(sharedSlot, input, &output));
    }

    // acquire the buffer
    BufferItem item;
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(sharedSlot, item.mSlot);
    testBufferItem(input, item);
    ASSERT_EQ(true, item.mQueuedBuffer);
    ASSERT_EQ(false, item.mAutoRefresh);

    ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));

    // attempt to acquire a second time should return no buffer available
    ASSERT_EQ(IGraphicBufferConsumer::NO_BUFFER_AVAILABLE,
            mConsumer->acquireBuffer(&item, 0));
}

TEST_F(BufferQueueTest, TestTimeouts) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, true, &output));

    // Fill up the queue. Since the controlledByApp flags are set to true, this
    // queue should be in non-blocking mode, and we should be recycling the same
    // two buffers
    for (int i = 0; i < 5; ++i) {
        int slot = BufferQueue::INVALID_BUFFER_SLOT;
        sp<Fence> fence = Fence::NO_FENCE;
        auto result = mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                               nullptr, nullptr);
        if (i < 2) {
            ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
                    result);
        } else {
            ASSERT_EQ(OK, result);
        }
        sp<GraphicBuffer> buffer;
        ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));
        IGraphicBufferProducer::QueueBufferInput input(0ull, true,
                HAL_DATASPACE_UNKNOWN, Rect::INVALID_RECT,
                NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
        IGraphicBufferProducer::QueueBufferOutput output{};
        ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    }

    const auto TIMEOUT = ms2ns(250);
    mProducer->setDequeueTimeout(TIMEOUT);

    // Setting a timeout will change the BufferQueue into blocking mode (with
    // one droppable buffer in the queue and one free from the previous
    // dequeue/queues), so dequeue and queue two more buffers: one to replace
    // the current droppable buffer, and a second to max out the buffer count
    sp<GraphicBuffer> buffer; // Save a buffer to attach later
    for (int i = 0; i < 2; ++i) {
        int slot = BufferQueue::INVALID_BUFFER_SLOT;
        sp<Fence> fence = Fence::NO_FENCE;
        ASSERT_EQ(OK,
                  mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                           nullptr, nullptr));
        ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));
        IGraphicBufferProducer::QueueBufferInput input(0ull, true,
                HAL_DATASPACE_UNKNOWN, Rect::INVALID_RECT,
                NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
        ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    }

    int slot = BufferQueue::INVALID_BUFFER_SLOT;
    sp<Fence> fence = Fence::NO_FENCE;
    auto startTime = systemTime();
    ASSERT_EQ(TIMED_OUT,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_GE(systemTime() - startTime, TIMEOUT);

    // We're technically attaching the same buffer multiple times (since we
    // queued it previously), but that doesn't matter for this test
    startTime = systemTime();
    ASSERT_EQ(TIMED_OUT, mProducer->attachBuffer(&slot, buffer));
    ASSERT_GE(systemTime() - startTime, TIMEOUT);
}

TEST_F(BufferQueueTest, CanAttachWhileDisallowingAllocation) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, true, &output));

    int slot = BufferQueue::INVALID_BUFFER_SLOT;
    sp<Fence> sourceFence;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &sourceFence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                       nullptr, nullptr));
    sp<GraphicBuffer> buffer;
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));
    ASSERT_EQ(OK, mProducer->detachBuffer(slot));

    ASSERT_EQ(OK, mProducer->allowAllocation(false));

    slot = BufferQueue::INVALID_BUFFER_SLOT;
    ASSERT_EQ(OK, mProducer->attachBuffer(&slot, buffer));
}

TEST_F(BufferQueueTest, CanRetrieveLastQueuedBuffer) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, false));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &output));

    // Dequeue and queue the first buffer, storing the handle
    int slot = BufferQueue::INVALID_BUFFER_SLOT;
    sp<Fence> fence;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    sp<GraphicBuffer> firstBuffer;
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &firstBuffer));

    IGraphicBufferProducer::QueueBufferInput input(0ull, true,
        HAL_DATASPACE_UNKNOWN, Rect::INVALID_RECT,
        NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

    // Dequeue a second buffer
    slot = BufferQueue::INVALID_BUFFER_SLOT;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    sp<GraphicBuffer> secondBuffer;
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &secondBuffer));

    // Ensure it's a new buffer
    ASSERT_NE(firstBuffer->getNativeBuffer()->handle,
            secondBuffer->getNativeBuffer()->handle);

    // Queue the second buffer
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

    // Acquire and release both buffers
    for (size_t i = 0; i < 2; ++i) {
        BufferItem item;
        ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
        ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));
    }

    // Make sure we got the second buffer back
    sp<GraphicBuffer> returnedBuffer;
    sp<Fence> returnedFence;
    float transform[16];
    ASSERT_EQ(OK,
            mProducer->getLastQueuedBuffer(&returnedBuffer, &returnedFence,
            transform));
    ASSERT_EQ(secondBuffer->getNativeBuffer()->handle,
            returnedBuffer->getNativeBuffer()->handle);
}

TEST_F(BufferQueueTest, TestOccupancyHistory) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, false));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &output));

    int slot = BufferQueue::INVALID_BUFFER_SLOT;
    sp<Fence> fence = Fence::NO_FENCE;
    sp<GraphicBuffer> buffer = nullptr;
    IGraphicBufferProducer::QueueBufferInput input(0ull, true,
        HAL_DATASPACE_UNKNOWN, Rect::INVALID_RECT,
        NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    BufferItem item{};

    // Preallocate, dequeue, request, and cancel 3 buffers so we don't get
    // BUFFER_NEEDS_REALLOCATION below
    int slots[3] = {};
    mProducer->setMaxDequeuedBufferCount(3);
    for (size_t i = 0; i < 3; ++i) {
        status_t result = mProducer->dequeueBuffer(&slots[i], &fence, 0, 0, 0,
                                                   TEST_PRODUCER_USAGE_BITS, nullptr, nullptr);
        ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, result);
        ASSERT_EQ(OK, mProducer->requestBuffer(slots[i], &buffer));
    }
    for (size_t i = 0; i < 3; ++i) {
        ASSERT_EQ(OK, mProducer->cancelBuffer(slots[i], Fence::NO_FENCE));
    }

    // Create 3 segments

    // The first segment is a two-buffer segment, so we only put one buffer into
    // the queue at a time
    for (size_t i = 0; i < 5; ++i) {
        ASSERT_EQ(OK,
                  mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                           nullptr, nullptr));
        ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
        ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
        ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));
        std::this_thread::sleep_for(16ms);
    }

    // Sleep between segments
    std::this_thread::sleep_for(500ms);

    // The second segment is a double-buffer segment. It starts the same as the
    // two-buffer segment, but then at the end, we put two buffers in the queue
    // at the same time before draining it.
    for (size_t i = 0; i < 5; ++i) {
        ASSERT_EQ(OK,
                  mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                           nullptr, nullptr));
        ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
        ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
        ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));
        std::this_thread::sleep_for(16ms);
    }
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));
    std::this_thread::sleep_for(16ms);
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));

    // Sleep between segments
    std::this_thread::sleep_for(500ms);

    // The third segment is a triple-buffer segment, so the queue is switching
    // between one buffer and two buffers deep.
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    for (size_t i = 0; i < 5; ++i) {
        ASSERT_EQ(OK,
                  mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                           nullptr, nullptr));
        ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
        ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
        ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));
        std::this_thread::sleep_for(16ms);
    }
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));

    // Now we read the segments
    std::vector<OccupancyTracker::Segment> history;
    ASSERT_EQ(OK, mConsumer->getOccupancyHistory(false, &history));

    // Since we didn't force a flush, we should only get the first two segments
    // (since the third segment hasn't been closed out by the appearance of a
    // new segment yet)
    ASSERT_EQ(2u, history.size());

    // The first segment (which will be history[1], since the newest segment
    // should be at the front of the vector) should be a two-buffer segment,
    // which implies that the occupancy average should be between 0 and 1, and
    // usedThirdBuffer should be false
    const auto& firstSegment = history[1];
    ASSERT_EQ(5u, firstSegment.numFrames);
    ASSERT_LT(0, firstSegment.occupancyAverage);
    ASSERT_GT(1, firstSegment.occupancyAverage);
    ASSERT_EQ(false, firstSegment.usedThirdBuffer);

    // The second segment should be a double-buffered segment, which implies that
    // the occupancy average should be between 0 and 1, but usedThirdBuffer
    // should be true
    const auto& secondSegment = history[0];
    ASSERT_EQ(7u, secondSegment.numFrames);
    ASSERT_LT(0, secondSegment.occupancyAverage);
    ASSERT_GT(1, secondSegment.occupancyAverage);
    ASSERT_EQ(true, secondSegment.usedThirdBuffer);

    // If we read the segments again without flushing, we shouldn't get any new
    // segments
    ASSERT_EQ(OK, mConsumer->getOccupancyHistory(false, &history));
    ASSERT_EQ(0u, history.size());

    // Read the segments again, this time forcing a flush so we get the third
    // segment
    ASSERT_EQ(OK, mConsumer->getOccupancyHistory(true, &history));
    ASSERT_EQ(1u, history.size());

    // This segment should be a triple-buffered segment, which implies that the
    // occupancy average should be between 1 and 2, and usedThirdBuffer should
    // be true
    const auto& thirdSegment = history[0];
    ASSERT_EQ(6u, thirdSegment.numFrames);
    ASSERT_LT(1, thirdSegment.occupancyAverage);
    ASSERT_GT(2, thirdSegment.occupancyAverage);
    ASSERT_EQ(true, thirdSegment.usedThirdBuffer);
}

struct BufferDiscardedListener : public BnProducerListener {
public:
    BufferDiscardedListener() = default;
    virtual ~BufferDiscardedListener() = default;

    virtual void onBufferReleased() {}
    virtual bool needsReleaseNotify() { return false; }
    virtual void onBuffersDiscarded(const std::vector<int32_t>& slots) {
        mDiscardedSlots.insert(mDiscardedSlots.end(), slots.begin(), slots.end());
    }

    const std::vector<int32_t>& getDiscardedSlots() const { return mDiscardedSlots; }
private:
    // No need to use lock given the test triggers the listener in the same
    // thread context.
    std::vector<int32_t> mDiscardedSlots;
};

TEST_F(BufferQueueTest, TestDiscardFreeBuffers) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, false));
    IGraphicBufferProducer::QueueBufferOutput output;
    sp<BufferDiscardedListener> pl(new BufferDiscardedListener);
    ASSERT_EQ(OK, mProducer->connect(pl,
            NATIVE_WINDOW_API_CPU, false, &output));

    int slot = BufferQueue::INVALID_BUFFER_SLOT;
    sp<Fence> fence = Fence::NO_FENCE;
    sp<GraphicBuffer> buffer = nullptr;
    IGraphicBufferProducer::QueueBufferInput input(0ull, true,
        HAL_DATASPACE_UNKNOWN, Rect::INVALID_RECT,
        NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    BufferItem item{};

    // Preallocate, dequeue, request, and cancel 4 buffers so we don't get
    // BUFFER_NEEDS_REALLOCATION below
    int slots[4] = {};
    mProducer->setMaxDequeuedBufferCount(4);
    for (size_t i = 0; i < 4; ++i) {
        status_t result = mProducer->dequeueBuffer(&slots[i], &fence, 0, 0, 0,
                                                   TEST_PRODUCER_USAGE_BITS, nullptr, nullptr);
        ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, result);
        ASSERT_EQ(OK, mProducer->requestBuffer(slots[i], &buffer));
    }
    for (size_t i = 0; i < 4; ++i) {
        ASSERT_EQ(OK, mProducer->cancelBuffer(slots[i], Fence::NO_FENCE));
    }

    // Get buffers in all states: dequeued, filled, acquired, free

    // Fill 3 buffers
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    // Dequeue 1 buffer
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));

    // Acquire and free 1 buffer
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));
    int releasedSlot = item.mSlot;

    // Acquire 1 buffer, leaving 1 filled buffer in queue
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));

    // Now discard the free buffers
    ASSERT_EQ(OK, mConsumer->discardFreeBuffers());

    // Check onBuffersDiscarded is called with correct slots
    auto buffersDiscarded = pl->getDiscardedSlots();
    ASSERT_EQ(buffersDiscarded.size(), 1u);
    ASSERT_EQ(buffersDiscarded[0], releasedSlot);

    // Check no free buffers in dump
    String8 dumpString;
    mConsumer->dumpState(String8{}, &dumpString);

    // Parse the dump to ensure that all buffer slots that are FREE also
    // have a null GraphicBuffer
    // Fragile - assumes the following format for the dump for a buffer entry:
    // ":%p\][^:]*state=FREE" where %p is the buffer pointer in hex.
    ssize_t idx = dumpString.find("state=FREE");
    while (idx != -1) {
        ssize_t bufferPtrIdx = idx - 1;
        while (bufferPtrIdx > 0) {
            if (dumpString[bufferPtrIdx] == ':') {
                bufferPtrIdx++;
                break;
            }
            bufferPtrIdx--;
        }
        ASSERT_GT(bufferPtrIdx, 0) << "Can't parse queue dump to validate";
        ssize_t nullPtrIdx = dumpString.find("0x0]", bufferPtrIdx);
        ASSERT_EQ(bufferPtrIdx, nullPtrIdx) << "Free buffer not discarded";
        idx = dumpString.find("FREE", idx + 1);
    }
}

TEST_F(BufferQueueTest, TestBufferReplacedInQueueBuffer) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(OK,
              mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, true, &output));
    ASSERT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(1));

    int slot = BufferQueue::INVALID_BUFFER_SLOT;
    sp<Fence> fence = Fence::NO_FENCE;
    sp<GraphicBuffer> buffer = nullptr;
    IGraphicBufferProducer::QueueBufferInput input(0ull, true,
        HAL_DATASPACE_UNKNOWN, Rect::INVALID_RECT,
        NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    BufferItem item{};

    // Preallocate, dequeue, request, and cancel 2 buffers so we don't get
    // BUFFER_NEEDS_REALLOCATION below
    int slots[2] = {};
    ASSERT_EQ(OK, mProducer->setMaxDequeuedBufferCount(2));
    for (size_t i = 0; i < 2; ++i) {
        status_t result = mProducer->dequeueBuffer(&slots[i], &fence, 0, 0, 0,
                                                   TEST_PRODUCER_USAGE_BITS, nullptr, nullptr);
        ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, result);
        ASSERT_EQ(OK, mProducer->requestBuffer(slots[i], &buffer));
    }
    for (size_t i = 0; i < 2; ++i) {
        ASSERT_EQ(OK, mProducer->cancelBuffer(slots[i], Fence::NO_FENCE));
    }

    // Fill 2 buffers without consumer consuming them. Verify that all
    // queued buffer returns proper bufferReplaced flag
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    ASSERT_EQ(false, output.bufferReplaced);
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));
    ASSERT_EQ(true, output.bufferReplaced);
}

struct BufferDetachedListener : public BnProducerListener {
public:
    BufferDetachedListener() = default;
    virtual ~BufferDetachedListener() = default;

    virtual void onBufferReleased() {}
    virtual bool needsReleaseNotify() { return true; }
    virtual void onBufferDetached(int slot) {
        mDetachedSlots.push_back(slot);
    }
    const std::vector<int>& getDetachedSlots() const { return mDetachedSlots; }
private:
    std::vector<int> mDetachedSlots;
};

TEST_F(BufferQueueTest, TestConsumerDetachProducerListener) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    sp<BufferDetachedListener> pl(new BufferDetachedListener);
    ASSERT_EQ(OK, mProducer->connect(pl, NATIVE_WINDOW_API_CPU, true, &output));
    ASSERT_EQ(OK, mProducer->setDequeueTimeout(0));
    ASSERT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(1));

    sp<Fence> fence = Fence::NO_FENCE;
    sp<GraphicBuffer> buffer = nullptr;
    IGraphicBufferProducer::QueueBufferInput input(0ull, true,
        HAL_DATASPACE_UNKNOWN, Rect::INVALID_RECT,
        NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);

    int slots[2] = {};
    status_t result = OK;
    ASSERT_EQ(OK, mProducer->setMaxDequeuedBufferCount(2));

    result = mProducer->dequeueBuffer(&slots[0], &fence, 0, 0, 0,
                                      GRALLOC_USAGE_SW_READ_RARELY, nullptr, nullptr);
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, result);
    ASSERT_EQ(OK, mProducer->requestBuffer(slots[0], &buffer));

    result = mProducer->dequeueBuffer(&slots[1], &fence, 0, 0, 0,
                                      GRALLOC_USAGE_SW_READ_RARELY, nullptr, nullptr);
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, result);
    ASSERT_EQ(OK, mProducer->requestBuffer(slots[1], &buffer));

    // Queue & detach one from two dequeued buffes.
    ASSERT_EQ(OK, mProducer->queueBuffer(slots[1], input, &output));
    BufferItem item{};
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(OK, mConsumer->detachBuffer(item.mSlot));

    // Check whether the slot from IProducerListener is same to the detached slot.
    ASSERT_EQ(pl->getDetachedSlots().size(), 1u);
    ASSERT_EQ(pl->getDetachedSlots()[0], slots[1]);

    // Dequeue another buffer.
    int slot;
    result = mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0,
                                      GRALLOC_USAGE_SW_READ_RARELY, nullptr, nullptr);
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, result);
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));

    // Dequeue should fail here, since we dequeued 3 buffers and one buffer was
    // detached from consumer(Two buffers are dequeued, and the current max
    // dequeued buffer count is two).
    result = mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0,
                                      GRALLOC_USAGE_SW_READ_RARELY, nullptr, nullptr);
    ASSERT_TRUE(result == WOULD_BLOCK || result == TIMED_OUT || result == INVALID_OPERATION);
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(BQ_CONSUMER_ATTACH_CALLBACK)
struct BufferAttachedListener : public BnProducerListener {
public:
    BufferAttachedListener(bool enable) : mEnabled(enable), mAttached(0) {}
    virtual ~BufferAttachedListener() = default;

    virtual void onBufferReleased() {}
    virtual bool needsReleaseNotify() { return true; }
    virtual void onBufferAttached() {
        ++mAttached;
    }
    virtual bool needsAttachNotify() { return mEnabled; }

    int getNumAttached() const { return mAttached; }
private:
    const bool mEnabled;
    int mAttached;
};

TEST_F(BufferQueueTest, TestConsumerAttachProducerListener) {
    createBufferQueue();
    sp<MockConsumer> mc1(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc1, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    // Do not enable attach callback.
    sp<BufferAttachedListener> pl1(new BufferAttachedListener(false));
    ASSERT_EQ(OK, mProducer->connect(pl1, NATIVE_WINDOW_API_CPU, true, &output));
    ASSERT_EQ(OK, mProducer->setDequeueTimeout(0));
    ASSERT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(1));

    sp<Fence> fence = Fence::NO_FENCE;
    sp<GraphicBuffer> buffer = nullptr;

    int slot;
    status_t result = OK;

    ASSERT_EQ(OK, mProducer->setMaxDequeuedBufferCount(1));

    result = mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0,
                                      GRALLOC_USAGE_SW_READ_RARELY, nullptr, nullptr);
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, result);
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));
    ASSERT_EQ(OK, mProducer->detachBuffer(slot));

    // Check # of attach is zero.
    ASSERT_EQ(0, pl1->getNumAttached());

    // Attach a buffer and check the callback was not called.
    ASSERT_EQ(OK, mConsumer->attachBuffer(&slot, buffer));
    ASSERT_EQ(0, pl1->getNumAttached());

    mProducer = nullptr;
    mConsumer = nullptr;
    createBufferQueue();

    sp<MockConsumer> mc2(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc2, true));
    // Enable attach callback.
    sp<BufferAttachedListener> pl2(new BufferAttachedListener(true));
    ASSERT_EQ(OK, mProducer->connect(pl2, NATIVE_WINDOW_API_CPU, true, &output));
    ASSERT_EQ(OK, mProducer->setDequeueTimeout(0));
    ASSERT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(1));

    fence = Fence::NO_FENCE;
    buffer = nullptr;

    result = OK;

    ASSERT_EQ(OK, mProducer->setMaxDequeuedBufferCount(1));

    result = mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0,
                                      GRALLOC_USAGE_SW_READ_RARELY, nullptr, nullptr);
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, result);
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));
    ASSERT_EQ(OK, mProducer->detachBuffer(slot));

    // Check # of attach is zero.
    ASSERT_EQ(0, pl2->getNumAttached());

    // Attach a buffer and check the callback was called.
    ASSERT_EQ(OK, mConsumer->attachBuffer(&slot, buffer));
    ASSERT_EQ(1, pl2->getNumAttached());
}
#endif

TEST_F(BufferQueueTest, TestStaleBufferHandleSentAfterDisconnect) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    sp<IProducerListener> fakeListener(new StubProducerListener);
    ASSERT_EQ(OK, mProducer->connect(fakeListener, NATIVE_WINDOW_API_CPU, true, &output));

    int slot = BufferQueue::INVALID_BUFFER_SLOT;
    sp<Fence> fence = Fence::NO_FENCE;
    sp<GraphicBuffer> buffer = nullptr;
    IGraphicBufferProducer::QueueBufferInput input(0ull, true,
            HAL_DATASPACE_UNKNOWN, Rect::INVALID_RECT,
            NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);

    // Dequeue, request, and queue one buffer
    status_t result = mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS,
                                               nullptr, nullptr);
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, result);
    ASSERT_EQ(OK, mProducer->requestBuffer(slot, &buffer));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

    // Acquire and release the buffer. Upon acquiring, the buffer handle should
    // be non-null since this is the first time we've acquired this slot.
    BufferItem item;
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(slot, item.mSlot);
    ASSERT_NE(nullptr, item.mGraphicBuffer.get());
    ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));

    // Dequeue and queue the buffer again
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

    // Acquire and release the buffer again. Upon acquiring, the buffer handle
    // should be null since this is not the first time we've acquired this slot.
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(slot, item.mSlot);
    ASSERT_EQ(nullptr, item.mGraphicBuffer.get());
    ASSERT_EQ(OK, mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber, Fence::NO_FENCE));

    // Dequeue and queue the buffer again
    ASSERT_EQ(OK,
              mProducer->dequeueBuffer(&slot, &fence, 0, 0, 0, TEST_PRODUCER_USAGE_BITS, nullptr,
                                       nullptr));
    ASSERT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

    // Disconnect the producer end. This should clear all of the slots and mark
    // the buffer in the queue as stale.
    ASSERT_EQ(OK, mProducer->disconnect(NATIVE_WINDOW_API_CPU));

    // Acquire the buffer again. Upon acquiring, the buffer handle should not be
    // null since the queued buffer should have been marked as stale, which
    // should trigger the BufferQueue to resend the buffer handle.
    ASSERT_EQ(OK, mConsumer->acquireBuffer(&item, 0));
    ASSERT_EQ(slot, item.mSlot);
    ASSERT_NE(nullptr, item.mGraphicBuffer.get());
}

TEST_F(BufferQueueTest, TestProducerConnectDisconnect) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    ASSERT_EQ(OK, mConsumer->consumerConnect(mc, true));
    IGraphicBufferProducer::QueueBufferOutput output;
    sp<IProducerListener> fakeListener(new StubProducerListener);
    ASSERT_EQ(NO_INIT, mProducer->disconnect(NATIVE_WINDOW_API_CPU));
    ASSERT_EQ(OK, mProducer->connect(fakeListener, NATIVE_WINDOW_API_CPU, true, &output));
    ASSERT_EQ(BAD_VALUE, mProducer->connect(fakeListener, NATIVE_WINDOW_API_MEDIA, true, &output));

    ASSERT_EQ(BAD_VALUE, mProducer->disconnect(NATIVE_WINDOW_API_MEDIA));
    ASSERT_EQ(OK, mProducer->disconnect(NATIVE_WINDOW_API_CPU));
    ASSERT_EQ(NO_INIT, mProducer->disconnect(NATIVE_WINDOW_API_CPU));
}

struct BufferItemConsumerSetFrameRateListener : public BufferItemConsumer {
    BufferItemConsumerSetFrameRateListener() : BufferItemConsumer(GRALLOC_USAGE_SW_READ_OFTEN, 1) {}

    MOCK_METHOD(void, onSetFrameRate, (float, int8_t, int8_t), (override));
};

TEST_F(BufferQueueTest, TestSetFrameRate) {
    sp<BufferItemConsumerSetFrameRateListener> bufferConsumer =
            sp<BufferItemConsumerSetFrameRateListener>::make();
    sp<IGraphicBufferProducer> producer = bufferConsumer->getSurface()->getIGraphicBufferProducer();

    EXPECT_CALL(*bufferConsumer, onSetFrameRate(12.34f, 1, 0)).Times(1);
    producer->setFrameRate(12.34f, 1, 0);
}

class Latch {
public:
    explicit Latch(int expected) : mExpected(expected) {}
    Latch(const Latch&) = delete;
    Latch& operator=(const Latch&) = delete;

    void CountDown() {
        std::unique_lock<std::mutex> lock(mLock);
        mExpected--;
        if (mExpected <= 0) {
            mCV.notify_all();
        }
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(mLock);
        mCV.wait(lock, [&] { return mExpected == 0; });
    }

private:
    int mExpected;
    std::mutex mLock;
    std::condition_variable mCV;
};

struct OneshotOnDequeuedListener final : public BufferItemConsumer::FrameAvailableListener {
    OneshotOnDequeuedListener(std::function<void()>&& oneshot)
          : mOneshotRunnable(std::move(oneshot)) {}

    std::function<void()> mOneshotRunnable;

    void run() {
        if (mOneshotRunnable) {
            mOneshotRunnable();
            mOneshotRunnable = nullptr;
        }
    }

    void onFrameDequeued(const uint64_t) override { run(); }

    void onFrameAvailable(const BufferItem&) override {}
};

// See b/270004534
TEST(BufferQueueThreading, TestProducerDequeueConsumerDestroy) {
    sp<BufferItemConsumer> bufferConsumer =
            sp<BufferItemConsumer>::make(GRALLOC_USAGE_SW_READ_OFTEN, 2);
    ASSERT_NE(nullptr, bufferConsumer.get());
    sp<Surface> surface = bufferConsumer->getSurface();
    native_window_set_buffers_format(surface.get(), PIXEL_FORMAT_RGBA_8888);
    native_window_set_buffers_dimensions(surface.get(), 100, 100);

    Latch triggerDisconnect(1);
    Latch resumeCallback(1);
    auto luckyListener = sp<OneshotOnDequeuedListener>::make([&]() {
        triggerDisconnect.CountDown();
        resumeCallback.Wait();
    });
    bufferConsumer->setFrameAvailableListener(luckyListener);

    std::future<void> disconnecter = std::async(std::launch::async, [&]() {
        triggerDisconnect.Wait();
        luckyListener = nullptr;
        bufferConsumer = nullptr;
        resumeCallback.CountDown();
    });

    std::future<void> render = std::async(std::launch::async, [=]() {
        ANativeWindow_Buffer buffer;
        surface->lock(&buffer, nullptr);
        surface->unlockAndPost();
    });

    ASSERT_EQ(std::future_status::ready, render.wait_for(1s));
    EXPECT_EQ(nullptr, luckyListener.get());
    EXPECT_EQ(nullptr, bufferConsumer.get());
}

TEST_F(BufferQueueTest, TestAdditionalOptions) {
    sp<BufferItemConsumer> bufferConsumer =
            sp<BufferItemConsumer>::make(GRALLOC_USAGE_SW_READ_OFTEN, 2);
    ASSERT_NE(nullptr, bufferConsumer.get());
    sp<Surface> surface = bufferConsumer->getSurface();
    native_window_set_buffers_format(surface.get(), PIXEL_FORMAT_RGBA_8888);
    native_window_set_buffers_dimensions(surface.get(), 100, 100);

    std::array<AHardwareBufferLongOptions, 1> extras = {{
            {.name = "android.hardware.graphics.common.Dataspace", ADATASPACE_DISPLAY_P3},
    }};

    auto status = native_window_set_buffers_additional_options(surface.get(), extras.data(),
                                                               extras.size());
    if (flags::bq_extendedallocate()) {
        ASSERT_EQ(NO_INIT, status);
    } else {
        ASSERT_EQ(INVALID_OPERATION, status);
        GTEST_SKIP() << "Flag bq_extendedallocate not enabled";
    }

    if (!IsCuttlefish()) {
        GTEST_SKIP() << "Not cuttlefish";
    }

    ASSERT_EQ(OK, native_window_api_connect(surface.get(), NATIVE_WINDOW_API_CPU));
    ASSERT_EQ(OK,
              native_window_set_buffers_additional_options(surface.get(), extras.data(),
                                                           extras.size()));

    ANativeWindowBuffer* windowBuffer = nullptr;
    int fence = -1;
    ASSERT_EQ(OK, ANativeWindow_dequeueBuffer(surface.get(), &windowBuffer, &fence));

    AHardwareBuffer* buffer = ANativeWindowBuffer_getHardwareBuffer(windowBuffer);
    ASSERT_TRUE(buffer);
    ADataSpace dataSpace = AHardwareBuffer_getDataSpace(buffer);
    EXPECT_EQ(ADATASPACE_DISPLAY_P3, dataSpace);

    ANativeWindow_cancelBuffer(surface.get(), windowBuffer, -1);

    // Check that reconnecting properly clears the options
    ASSERT_EQ(OK, native_window_api_disconnect(surface.get(), NATIVE_WINDOW_API_CPU));
    ASSERT_EQ(OK, native_window_api_connect(surface.get(), NATIVE_WINDOW_API_CPU));

    ASSERT_EQ(OK, ANativeWindow_dequeueBuffer(surface.get(), &windowBuffer, &fence));
    buffer = ANativeWindowBuffer_getHardwareBuffer(windowBuffer);
    ASSERT_TRUE(buffer);
    dataSpace = AHardwareBuffer_getDataSpace(buffer);
    EXPECT_EQ(ADATASPACE_UNKNOWN, dataSpace);
}

TEST_F(BufferQueueTest, PassesThroughPictureProfileHandle) {
    createBufferQueue();
    sp<MockConsumer> mc(new MockConsumer);
    mConsumer->consumerConnect(mc, false);

    IGraphicBufferProducer::QueueBufferOutput qbo;
    mProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &qbo);
    mProducer->setMaxDequeuedBufferCount(2);
    mConsumer->setMaxAcquiredBufferCount(2);

    // First try to pass a valid picture profile handle
    {
        int slot;
        sp<Fence> fence;
        sp<GraphicBuffer> buf;
        IGraphicBufferProducer::QueueBufferInput qbi(0, false, HAL_DATASPACE_UNKNOWN,
                                                     Rect(0, 0, 1, 1),
                                                     NATIVE_WINDOW_SCALING_MODE_FREEZE, 0,
                                                     Fence::NO_FENCE);
        qbi.setPictureProfileHandle(PictureProfileHandle(1));

        EXPECT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
                  mProducer->dequeueBuffer(&slot, &fence, 1, 1, 0, GRALLOC_USAGE_SW_READ_OFTEN,
                                           nullptr, nullptr));
        EXPECT_EQ(OK, mProducer->requestBuffer(slot, &buf));
        EXPECT_EQ(OK, mProducer->queueBuffer(slot, qbi, &qbo));

        BufferItem item;
        EXPECT_EQ(OK, mConsumer->acquireBuffer(&item, 0));

        ASSERT_TRUE(item.mPictureProfileHandle.has_value());
        ASSERT_EQ(item.mPictureProfileHandle, PictureProfileHandle(1));
    }

    // Then validate that the picture profile handle isn't sticky and is reset for the next buffer
    {
        int slot;
        sp<Fence> fence;
        sp<GraphicBuffer> buf;
        IGraphicBufferProducer::QueueBufferInput qbi(0, false, HAL_DATASPACE_UNKNOWN,
                                                     Rect(0, 0, 1, 1),
                                                     NATIVE_WINDOW_SCALING_MODE_FREEZE, 0,
                                                     Fence::NO_FENCE);

        EXPECT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION,
                  mProducer->dequeueBuffer(&slot, &fence, 1, 1, 0, GRALLOC_USAGE_SW_READ_OFTEN,
                                           nullptr, nullptr));
        EXPECT_EQ(OK, mProducer->requestBuffer(slot, &buf));
        EXPECT_EQ(OK, mProducer->queueBuffer(slot, qbi, &qbo));

        BufferItem item;
        EXPECT_EQ(OK, mConsumer->acquireBuffer(&item, 0));

        ASSERT_FALSE(item.mPictureProfileHandle.has_value());
    }
}

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
struct MockUnlimitedSlotConsumer : public MockConsumer {
    virtual void onSlotCountChanged(int size) override { mSize = size; }

    std::optional<int> mSize;
};

TEST_F(BufferQueueTest, UnlimitedSlots_FailsWhenNotAllowed) {
    createBufferQueue();

    sp<MockUnlimitedSlotConsumer> mc = sp<MockUnlimitedSlotConsumer>::make();
    EXPECT_EQ(OK, mConsumer->consumerConnect(mc, false));

    EXPECT_EQ(INVALID_OPERATION, mProducer->extendSlotCount(64));
    EXPECT_EQ(INVALID_OPERATION, mProducer->extendSlotCount(32));
    EXPECT_EQ(INVALID_OPERATION, mProducer->extendSlotCount(128));

    EXPECT_EQ(std::nullopt, mc->mSize);
}

TEST_F(BufferQueueTest, UnlimitedSlots_OnlyAllowedForExtensions) {
    createBufferQueue();

    sp<MockUnlimitedSlotConsumer> consumerListener = sp<MockUnlimitedSlotConsumer>::make();
    EXPECT_EQ(OK, mConsumer->consumerConnect(consumerListener, false));
    EXPECT_EQ(OK, mConsumer->allowUnlimitedSlots(true));

    EXPECT_EQ(BAD_VALUE, mProducer->extendSlotCount(32));
    EXPECT_EQ(OK, mProducer->extendSlotCount(64));
    EXPECT_EQ(OK, mProducer->extendSlotCount(128));
    EXPECT_EQ(128, *consumerListener->mSize);

    EXPECT_EQ(OK, mProducer->extendSlotCount(128));
    EXPECT_EQ(BAD_VALUE, mProducer->extendSlotCount(127));
}

class BufferQueueUnlimitedTest : public BufferQueueTest {
protected:
    static constexpr auto kMaxBufferCount = 128;
    static constexpr auto kAcquirableBufferCount = 2;
    static constexpr auto kDequeableBufferCount = kMaxBufferCount - kAcquirableBufferCount;

    virtual void SetUp() override {
        BufferQueueTest::SetUp();

        createBufferQueue();
        setUpConsumer();
        setUpProducer();
    }

    void setUpConsumer() {
        EXPECT_EQ(OK, mConsumer->consumerConnect(mConsumerListener, false));

#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
        EXPECT_EQ(OK, mConsumer->allowUnlimitedSlots(true));
#endif
        EXPECT_EQ(OK, mConsumer->setConsumerUsageBits(GraphicBuffer::USAGE_SW_READ_OFTEN));
        EXPECT_EQ(OK, mConsumer->setDefaultBufferSize(10, 10));
        EXPECT_EQ(OK, mConsumer->setDefaultBufferFormat(PIXEL_FORMAT_RGBA_8888));
        EXPECT_EQ(OK, mConsumer->setMaxAcquiredBufferCount(kAcquirableBufferCount));
    }

    void setUpProducer() {
        EXPECT_EQ(OK, mProducer->extendSlotCount(kMaxBufferCount));

        IGraphicBufferProducer::QueueBufferOutput output;
        EXPECT_EQ(OK,
                  mProducer->connect(mProducerListener, NATIVE_WINDOW_API_CPU,
                                     /*producerControlledByApp*/ true, &output));
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
        ASSERT_TRUE(output.isSlotExpansionAllowed);
#endif
        ASSERT_EQ(OK, mProducer->setMaxDequeuedBufferCount(kDequeableBufferCount));
        ASSERT_EQ(OK, mProducer->allowAllocation(true));
    }

    std::unordered_map<int, sp<Fence>> dequeueAll() {
        std::unordered_map<int, sp<Fence>> slotsToFences;

        for (int i = 0; i < kDequeableBufferCount; ++i) {
            int slot;
            sp<Fence> fence;
            sp<GraphicBuffer> buffer;

            status_t ret =
                    mProducer->dequeueBuffer(&slot, &fence, /*w*/ 0, /*h*/ 0, /*format*/ 0,
                                             /*uint64_t*/ 0,
                                             /*outBufferAge*/ nullptr, /*outTimestamps*/ nullptr);
            if (ret & IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION) {
                EXPECT_EQ(OK, mProducer->requestBuffer(slot, &buffer))
                        << "Unable to request buffer for slot " << slot;
            }
            EXPECT_FALSE(slotsToFences.contains(slot));
            slotsToFences.emplace(slot, fence);
        }
        EXPECT_EQ(kDequeableBufferCount, (int)slotsToFences.size());
        return slotsToFences;
    }

    sp<MockUnlimitedSlotConsumer> mConsumerListener = sp<MockUnlimitedSlotConsumer>::make();
    sp<StubProducerListener> mProducerListener = sp<StubProducerListener>::make();
};

TEST_F(BufferQueueUnlimitedTest, ExpandOverridesConsumerMaxBuffers) {
    createBufferQueue();
    setUpConsumer();
    EXPECT_EQ(OK, mConsumer->setMaxBufferCount(10));

    setUpProducer();

    EXPECT_EQ(kDequeableBufferCount, (int)dequeueAll().size());
}

TEST_F(BufferQueueUnlimitedTest, CanDetachAll) {
    auto slotsToFences = dequeueAll();
    for (auto& [slot, fence] : slotsToFences) {
        EXPECT_EQ(OK, mProducer->detachBuffer(slot));
    }
}

TEST_F(BufferQueueUnlimitedTest, CanCancelAll) {
    auto slotsToFences = dequeueAll();
    for (auto& [slot, fence] : slotsToFences) {
        EXPECT_EQ(OK, mProducer->cancelBuffer(slot, fence));
    }
}

TEST_F(BufferQueueUnlimitedTest, CanAcquireAndReleaseAll) {
    auto slotsToFences = dequeueAll();
    for (auto& [slot, fence] : slotsToFences) {
        IGraphicBufferProducer::QueueBufferInput input;
        input.fence = fence;

        IGraphicBufferProducer::QueueBufferOutput output;
        EXPECT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

        BufferItem buffer;
        EXPECT_EQ(OK, mConsumer->acquireBuffer(&buffer, 0));
        EXPECT_EQ(OK,
                  mConsumer->releaseBuffer(buffer.mSlot, buffer.mFrameNumber, EGL_NO_DISPLAY,
                                           EGL_NO_SYNC, buffer.mFence));
    }
}

TEST_F(BufferQueueUnlimitedTest, CanAcquireAndDetachAll) {
    auto slotsToFences = dequeueAll();
    for (auto& [slot, fence] : slotsToFences) {
        IGraphicBufferProducer::QueueBufferInput input;
        input.fence = fence;

        IGraphicBufferProducer::QueueBufferOutput output;
        EXPECT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

        BufferItem buffer;
        EXPECT_EQ(OK, mConsumer->acquireBuffer(&buffer, 0));
        EXPECT_EQ(OK, mConsumer->detachBuffer(buffer.mSlot));
    }
}

TEST_F(BufferQueueUnlimitedTest, GetReleasedBuffersExtended) {
    // First, acquire and release all the buffers so the consumer "knows" about
    // them
    auto slotsToFences = dequeueAll();

    std::vector<bool> releasedSlots;
    EXPECT_EQ(OK, mConsumer->getReleasedBuffersExtended(&releasedSlots));
    for (auto& [slot, _] : slotsToFences) {
        EXPECT_TRUE(releasedSlots[slot])
                << "Slots that haven't been acquired will show up as released.";
    }
    for (auto& [slot, fence] : slotsToFences) {
        IGraphicBufferProducer::QueueBufferInput input;
        input.fence = fence;

        IGraphicBufferProducer::QueueBufferOutput output;
        EXPECT_EQ(OK, mProducer->queueBuffer(slot, input, &output));

        BufferItem buffer;
        EXPECT_EQ(OK, mConsumer->acquireBuffer(&buffer, 0));
        EXPECT_EQ(OK,
                  mConsumer->releaseBuffer(buffer.mSlot, buffer.mFrameNumber, EGL_NO_DISPLAY,
                                           EGL_NO_SYNC_KHR, buffer.mFence));
    }

    EXPECT_EQ(OK, mConsumer->getReleasedBuffersExtended(&releasedSlots));
    for (auto& [slot, _] : slotsToFences) {
        EXPECT_FALSE(releasedSlots[slot])
                << "Slots that have been acquired will show up as not released.";
    }

    // Then, alternatively cancel and detach (release) buffers. Only detached
    // buffers should be returned by getReleasedBuffersExtended
    slotsToFences = dequeueAll();
    std::set<int> cancelledSlots;
    std::set<int> detachedSlots;
    bool cancel;
    for (auto& [slot, fence] : slotsToFences) {
        if (cancel) {
            EXPECT_EQ(OK, mProducer->cancelBuffer(slot, fence));
            cancelledSlots.insert(slot);
        } else {
            EXPECT_EQ(OK, mProducer->detachBuffer(slot));
            detachedSlots.insert(slot);
        }
        cancel = !cancel;
    }

    EXPECT_EQ(OK, mConsumer->getReleasedBuffersExtended(&releasedSlots));
    for (int slot : detachedSlots) {
        EXPECT_TRUE(releasedSlots[slot]) << "Slots that are detached are released.";
    }
    for (int slot : cancelledSlots) {
        EXPECT_FALSE(releasedSlots[slot])
                << "Slots that are still held in the queue are not released.";
    }
}
#endif //  COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_UNLIMITED_SLOTS)
} // namespace android
