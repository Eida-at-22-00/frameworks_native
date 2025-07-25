/**
 * Copyright 2024 The Android Open Source Project
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

#define LOG_TAG "InputConsumerNoResampling"
#define ATRACE_TAG ATRACE_TAG_INPUT

#include <inttypes.h>
#include <set>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <cutils/properties.h>
#include <ftl/enum.h>
#include <utils/Trace.h>

#include <com_android_input_flags.h>
#include <input/InputConsumerNoResampling.h>
#include <input/PrintTools.h>
#include <input/TraceTools.h>

namespace android {

namespace {

using std::chrono::nanoseconds;

using android::base::Result;

/**
 * Log debug messages relating to the consumer end of the transport channel.
 * Enable this via "adb shell setprop log.tag.InputTransportConsumer DEBUG" (requires restart)
 */
const bool DEBUG_TRANSPORT_CONSUMER =
        __android_log_is_loggable(ANDROID_LOG_DEBUG, LOG_TAG "Consumer", ANDROID_LOG_INFO);

std::unique_ptr<KeyEvent> createKeyEvent(const InputMessage& msg) {
    std::unique_ptr<KeyEvent> event = std::make_unique<KeyEvent>();
    event->initialize(msg.body.key.eventId, msg.body.key.deviceId, msg.body.key.source,
                      ui::LogicalDisplayId{msg.body.key.displayId}, msg.body.key.hmac,
                      msg.body.key.action, msg.body.key.flags, msg.body.key.keyCode,
                      msg.body.key.scanCode, msg.body.key.metaState, msg.body.key.repeatCount,
                      msg.body.key.downTime, msg.body.key.eventTime);
    return event;
}

std::unique_ptr<FocusEvent> createFocusEvent(const InputMessage& msg) {
    std::unique_ptr<FocusEvent> event = std::make_unique<FocusEvent>();
    event->initialize(msg.body.focus.eventId, msg.body.focus.hasFocus);
    return event;
}

std::unique_ptr<CaptureEvent> createCaptureEvent(const InputMessage& msg) {
    std::unique_ptr<CaptureEvent> event = std::make_unique<CaptureEvent>();
    event->initialize(msg.body.capture.eventId, msg.body.capture.pointerCaptureEnabled);
    return event;
}

std::unique_ptr<DragEvent> createDragEvent(const InputMessage& msg) {
    std::unique_ptr<DragEvent> event = std::make_unique<DragEvent>();
    event->initialize(msg.body.drag.eventId, msg.body.drag.x, msg.body.drag.y,
                      msg.body.drag.isExiting);
    return event;
}

std::unique_ptr<MotionEvent> createMotionEvent(const InputMessage& msg) {
    std::unique_ptr<MotionEvent> event = std::make_unique<MotionEvent>();
    const uint32_t pointerCount = msg.body.motion.pointerCount;
    std::vector<PointerProperties> pointerProperties;
    pointerProperties.reserve(pointerCount);
    std::vector<PointerCoords> pointerCoords;
    pointerCoords.reserve(pointerCount);
    for (uint32_t i = 0; i < pointerCount; i++) {
        pointerProperties.push_back(msg.body.motion.pointers[i].properties);
        pointerCoords.push_back(msg.body.motion.pointers[i].coords);
    }

    ui::Transform transform;
    transform.set({msg.body.motion.dsdx, msg.body.motion.dtdx, msg.body.motion.tx,
                   msg.body.motion.dtdy, msg.body.motion.dsdy, msg.body.motion.ty, 0, 0, 1});
    ui::Transform displayTransform;
    displayTransform.set({msg.body.motion.dsdxRaw, msg.body.motion.dtdxRaw, msg.body.motion.txRaw,
                          msg.body.motion.dtdyRaw, msg.body.motion.dsdyRaw, msg.body.motion.tyRaw,
                          0, 0, 1});
    event->initialize(msg.body.motion.eventId, msg.body.motion.deviceId, msg.body.motion.source,
                      ui::LogicalDisplayId{msg.body.motion.displayId}, msg.body.motion.hmac,
                      msg.body.motion.action, msg.body.motion.actionButton, msg.body.motion.flags,
                      msg.body.motion.edgeFlags, msg.body.motion.metaState,
                      msg.body.motion.buttonState, msg.body.motion.classification, transform,
                      msg.body.motion.xPrecision, msg.body.motion.yPrecision,
                      msg.body.motion.xCursorPosition, msg.body.motion.yCursorPosition,
                      displayTransform, msg.body.motion.downTime, msg.body.motion.eventTime,
                      pointerCount, pointerProperties.data(), pointerCoords.data());
    return event;
}

void addSample(MotionEvent& event, const InputMessage& msg) {
    uint32_t pointerCount = msg.body.motion.pointerCount;
    std::vector<PointerCoords> pointerCoords;
    pointerCoords.reserve(pointerCount);
    for (uint32_t i = 0; i < pointerCount; i++) {
        pointerCoords.push_back(msg.body.motion.pointers[i].coords);
    }

    // TODO(b/329770983): figure out if it's safe to combine events with mismatching metaState
    event.setMetaState(event.getMetaState() | msg.body.motion.metaState);
    event.addSample(msg.body.motion.eventTime, pointerCoords.data(), msg.body.motion.eventId);
}

std::unique_ptr<TouchModeEvent> createTouchModeEvent(const InputMessage& msg) {
    std::unique_ptr<TouchModeEvent> event = std::make_unique<TouchModeEvent>();
    event->initialize(msg.body.touchMode.eventId, msg.body.touchMode.isInTouchMode);
    return event;
}

std::string outboundMessageToString(const InputMessage& outboundMsg) {
    switch (outboundMsg.header.type) {
        case InputMessage::Type::FINISHED: {
            return android::base::StringPrintf("  Finish: seq=%" PRIu32 " handled=%s",
                                               outboundMsg.header.seq,
                                               toString(outboundMsg.body.finished.handled));
        }
        case InputMessage::Type::TIMELINE: {
            return android::base::
                    StringPrintf("  Timeline: inputEventId=%" PRId32 " gpuCompletedTime=%" PRId64
                                 ", presentTime=%" PRId64,
                                 outboundMsg.body.timeline.eventId,
                                 outboundMsg.body.timeline
                                         .graphicsTimeline[GraphicsTimeline::GPU_COMPLETED_TIME],
                                 outboundMsg.body.timeline
                                         .graphicsTimeline[GraphicsTimeline::PRESENT_TIME]);
        }
        default: {
            LOG(FATAL) << "Outbound message must be FINISHED or TIMELINE, got "
                       << ftl::enum_string(outboundMsg.header.type);
            return "Unreachable";
        }
    }
}

InputMessage createFinishedMessage(uint32_t seq, bool handled, nsecs_t consumeTime) {
    InputMessage msg;
    msg.header.type = InputMessage::Type::FINISHED;
    msg.header.seq = seq;
    msg.body.finished.handled = handled;
    msg.body.finished.consumeTime = consumeTime;
    return msg;
}

InputMessage createTimelineMessage(int32_t inputEventId, nsecs_t gpuCompletedTime,
                                   nsecs_t presentTime) {
    InputMessage msg;
    msg.header.type = InputMessage::Type::TIMELINE;
    msg.header.seq = 0;
    msg.body.timeline.eventId = inputEventId;
    msg.body.timeline.graphicsTimeline[GraphicsTimeline::GPU_COMPLETED_TIME] = gpuCompletedTime;
    msg.body.timeline.graphicsTimeline[GraphicsTimeline::PRESENT_TIME] = presentTime;
    return msg;
}

} // namespace

// --- InputConsumerNoResampling ---

InputConsumerNoResampling::InputConsumerNoResampling(
        const std::shared_ptr<InputChannel>& channel, sp<Looper> looper,
        InputConsumerCallbacks& callbacks,
        std::function<std::unique_ptr<Resampler>()> resamplerCreator)
      : mChannel{channel},
        mLooper{looper},
        mCallbacks{callbacks},
        mResamplerCreator{std::move(resamplerCreator)},
        mFdEvents(0) {
    LOG_ALWAYS_FATAL_IF(mLooper == nullptr);
    mCallback = sp<LooperEventCallback>::make(
            std::bind(&InputConsumerNoResampling::handleReceiveCallback, this,
                      std::placeholders::_1));
    // In the beginning, there are no pending outbounds events; we only care about receiving
    // incoming data.
    setFdEvents(ALOOPER_EVENT_INPUT);
}

InputConsumerNoResampling::~InputConsumerNoResampling() {
    ensureCalledOnLooperThread(__func__);
    // If there are any remaining unread batches, send an ack for them and don't deliver
    // them to callbacks.
    for (auto& [_, batches] : mBatches) {
        while (!batches.empty()) {
            finishInputEvent(batches.front().header.seq, /*handled=*/false);
            batches.pop();
        }
    }

    while (!mOutboundQueue.empty()) {
        processOutboundEvents();
        // This is our last chance to ack the events. If we don't ack them here, we will get an ANR,
        // so keep trying to send the events as long as they are present in the queue.
    }
    // However, it is still up to the app to finish any events that have already been delivered
    // to the callbacks. If we wanted to change that behaviour and auto-finish all unfinished events
    // that were already sent to callbacks, we could potentially loop through "mConsumeTimes"
    // instead. We can't use "mBatchedSequenceNumbers" for this purpose, because it only contains
    // batchable (i.e., ACTION_MOVE) events that were sent to the callbacks.
    const size_t unfinishedEvents = mConsumeTimes.size();
    LOG_IF(INFO, unfinishedEvents != 0)
            << getName() << " has " << unfinishedEvents << " unfinished event(s)";
    // Remove the fd from epoll, so that Looper does not call 'handleReceiveCallback' anymore.
    // This must be done at the end of the destructor; otherwise, some of the other functions may
    // call 'setFdEvents' as a side-effect, thus adding the fd back to the epoll set of the looper.
    setFdEvents(0);
}

int InputConsumerNoResampling::handleReceiveCallback(int events) {
    // Allowed return values of this function as documented in LooperCallback::handleEvent
    constexpr int REMOVE_CALLBACK = 0;
    constexpr int KEEP_CALLBACK = 1;

    if (events & (ALOOPER_EVENT_ERROR | ALOOPER_EVENT_HANGUP)) {
        // This error typically occurs when the publisher has closed the input channel
        // as part of removing a window or finishing an IME session, in which case
        // the consumer will soon be disposed as well.
        if (DEBUG_TRANSPORT_CONSUMER) {
            LOG(INFO) << "The channel was hung up or an error occurred: " << mChannel->getName();
        }
        return REMOVE_CALLBACK;
    }

    int handledEvents = 0;
    if (events & ALOOPER_EVENT_INPUT) {
        handleMessages(readAllMessages());
        handledEvents |= ALOOPER_EVENT_INPUT;
    }

    if (events & ALOOPER_EVENT_OUTPUT) {
        processOutboundEvents();
        handledEvents |= ALOOPER_EVENT_OUTPUT;
    }
    if (handledEvents != events) {
        LOG(FATAL) << "Mismatch: handledEvents=" << handledEvents << ", events=" << events;
    }
    return KEEP_CALLBACK;
}

void InputConsumerNoResampling::processOutboundEvents() {
    while (!mOutboundQueue.empty()) {
        const InputMessage& outboundMsg = mOutboundQueue.front();

        const status_t result = mChannel->sendMessage(&outboundMsg);
        if (result == OK) {
            if (outboundMsg.header.type == InputMessage::Type::FINISHED) {
                ATRACE_ASYNC_END("InputConsumer processing", /*cookie=*/outboundMsg.header.seq);
            }
            // Successful send. Erase the entry and keep trying to send more
            mOutboundQueue.pop();
            continue;
        }

        // Publisher is busy, try again later. Keep this entry (do not erase)
        if (result == WOULD_BLOCK) {
            setFdEvents(ALOOPER_EVENT_INPUT | ALOOPER_EVENT_OUTPUT);
            return; // try again later
        }

        if (result == DEAD_OBJECT) {
            // If there's no one to receive events in the channel, there's no point in sending them.
            // Drop all outbound events.
            LOG(INFO) << "Channel " << mChannel->getName() << " died. Dropping outbound event "
                      << outboundMsg;
            mOutboundQueue.pop();
            setFdEvents(0);
            continue;
        }
        // Some other error. Give up
        LOG(FATAL) << "Failed to send outbound event on channel '" << mChannel->getName()
                   << "'.  status=" << statusToString(result) << "(" << result << ")";
    }

    // The queue is now empty. Tell looper there's no more output to expect.
    setFdEvents(ALOOPER_EVENT_INPUT);
}

void InputConsumerNoResampling::finishInputEvent(uint32_t seq, bool handled) {
    ensureCalledOnLooperThread(__func__);
    mOutboundQueue.push(createFinishedMessage(seq, handled, popConsumeTime(seq)));
    // also produce finish events for all batches for this seq (if any)
    const auto it = mBatchedSequenceNumbers.find(seq);
    if (it != mBatchedSequenceNumbers.end()) {
        for (uint32_t subSeq : it->second) {
            mOutboundQueue.push(createFinishedMessage(subSeq, handled, popConsumeTime(subSeq)));
        }
        mBatchedSequenceNumbers.erase(it);
    }
    processOutboundEvents();
}

bool InputConsumerNoResampling::probablyHasInput() const {
    // Ideally, this would only be allowed to run on the looper thread, and in production, it will.
    // However, for testing, it's convenient to call this while the looper thread is blocked, so
    // we do not call ensureCalledOnLooperThread here.
    return (!mBatches.empty()) || mChannel->probablyHasInput();
}

void InputConsumerNoResampling::reportTimeline(int32_t inputEventId, nsecs_t gpuCompletedTime,
                                               nsecs_t presentTime) {
    ensureCalledOnLooperThread(__func__);
    mOutboundQueue.push(createTimelineMessage(inputEventId, gpuCompletedTime, presentTime));
    processOutboundEvents();
}

nsecs_t InputConsumerNoResampling::popConsumeTime(uint32_t seq) {
    auto it = mConsumeTimes.find(seq);
    // Consume time will be missing if either 'finishInputEvent' is called twice, or if it was
    // called for the wrong (synthetic?) input event. Either way, it is a bug that should be fixed.
    LOG_ALWAYS_FATAL_IF(it == mConsumeTimes.end(), "Could not find consume time for seq=%" PRIu32,
                        seq);
    nsecs_t consumeTime = it->second;
    mConsumeTimes.erase(it);
    return consumeTime;
}

void InputConsumerNoResampling::setFdEvents(int events) {
    if (mFdEvents != events) {
        mFdEvents = events;
        if (events != 0) {
            mLooper->addFd(mChannel->getFd(), 0, events, mCallback, nullptr);
        } else {
            mLooper->removeFd(mChannel->getFd());
        }
    }
}

void InputConsumerNoResampling::handleMessages(std::vector<InputMessage>&& messages) {
    for (const InputMessage& msg : messages) {
        if (msg.header.type == InputMessage::Type::MOTION) {
            const int32_t action = msg.body.motion.action;
            const DeviceId deviceId = msg.body.motion.deviceId;
            const int32_t source = msg.body.motion.source;
            const bool batchableEvent = (action == AMOTION_EVENT_ACTION_MOVE ||
                                         action == AMOTION_EVENT_ACTION_HOVER_MOVE) &&
                    (isFromSource(source, AINPUT_SOURCE_CLASS_POINTER) ||
                     isFromSource(source, AINPUT_SOURCE_CLASS_JOYSTICK));

            const bool canResample = (mResamplerCreator != nullptr) &&
                    (isFromSource(source, AINPUT_SOURCE_CLASS_POINTER));
            if (canResample) {
                if (action == AMOTION_EVENT_ACTION_DOWN) {
                    if (std::unique_ptr<Resampler> resampler = mResamplerCreator();
                        resampler != nullptr) {
                        const auto [_, inserted] =
                                mResamplers.insert(std::pair(deviceId, std::move(resampler)));
                        LOG_IF(WARNING, !inserted) << deviceId << "already exists in mResamplers";
                    }
                }
            }

            if (batchableEvent) {
                // add it to batch
                mBatches[deviceId].emplace(msg);
            } else {
                // consume all pending batches for this device immediately
                consumeBatchedInputEvents(deviceId, /*requestedFrameTime=*/
                                          std::numeric_limits<nsecs_t>::max());
                if (canResample &&
                    (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL)) {
                    LOG_IF(INFO, mResamplers.erase(deviceId) == 0)
                            << deviceId << "does not exist in mResamplers";
                }
                handleMessage(msg);
            }
        } else {
            // Non-motion events shouldn't force the consumption of pending batched events
            handleMessage(msg);
        }
    }
    // At the end of this, if we still have pending batches, notify the receiver about it.

    // We need to carefully notify the InputConsumerCallbacks about the pending batch. The receiver
    // could choose to consume all events when notified about the batch. That means that the
    // "mBatches" variable could change when 'InputConsumerCallbacks::onBatchedInputEventPending' is
    // invoked. We also can't notify the InputConsumerCallbacks in a while loop until mBatches is
    // empty, because the receiver could choose to not consume the batch immediately.
    std::set<int32_t> pendingBatchSources;
    for (const auto& [_, pendingMessages] : mBatches) {
        // Assume that all messages for a given device has the same source.
        pendingBatchSources.insert(pendingMessages.front().body.motion.source);
    }
    for (const int32_t source : pendingBatchSources) {
        const bool sourceStillRemaining =
                std::any_of(mBatches.begin(), mBatches.end(), [=](const auto& pair) {
                    return pair.second.front().body.motion.source == source;
                });
        if (sourceStillRemaining) {
            mCallbacks.onBatchedInputEventPending(source);
        }
    }
}

std::vector<InputMessage> InputConsumerNoResampling::readAllMessages() {
    std::vector<InputMessage> messages;
    while (true) {
        android::base::Result<InputMessage> result = mChannel->receiveMessage();
        if (result.ok()) {
            const InputMessage& msg = *result;
            const auto [_, inserted] =
                    mConsumeTimes.emplace(msg.header.seq, systemTime(SYSTEM_TIME_MONOTONIC));
            LOG_ALWAYS_FATAL_IF(!inserted, "Already have a consume time for seq=%" PRIu32,
                                msg.header.seq);

            // Trace the event processing timeline - event was just read from the socket
            // TODO(b/329777420): distinguish between multiple instances of InputConsumer
            // in the same process.
            ATRACE_ASYNC_BEGIN("InputConsumer processing", /*cookie=*/msg.header.seq);
            messages.push_back(msg);
        } else { // !result.ok()
            switch (result.error().code()) {
                case WOULD_BLOCK: {
                    return messages;
                }
                case DEAD_OBJECT: {
                    LOG(FATAL) << "Got a dead object for " << mChannel->getName();
                    break;
                }
                case BAD_VALUE: {
                    LOG(FATAL) << "Got a bad value for " << mChannel->getName();
                    break;
                }
                default: {
                    LOG(FATAL) << "Unexpected error: " << result.error().message();
                    break;
                }
            }
        }
    }
}

void InputConsumerNoResampling::handleMessage(const InputMessage& msg) const {
    switch (msg.header.type) {
        case InputMessage::Type::KEY: {
            std::unique_ptr<KeyEvent> keyEvent = createKeyEvent(msg);
            mCallbacks.onKeyEvent(std::move(keyEvent), msg.header.seq);
            break;
        }

        case InputMessage::Type::MOTION: {
            std::unique_ptr<MotionEvent> motionEvent = createMotionEvent(msg);
            mCallbacks.onMotionEvent(std::move(motionEvent), msg.header.seq);
            break;
        }

        case InputMessage::Type::FINISHED:
        case InputMessage::Type::TIMELINE: {
            LOG(FATAL) << "Consumed a " << ftl::enum_string(msg.header.type)
                       << " message, which should never be seen by InputConsumer on "
                       << mChannel->getName();
            break;
        }

        case InputMessage::Type::FOCUS: {
            std::unique_ptr<FocusEvent> focusEvent = createFocusEvent(msg);
            mCallbacks.onFocusEvent(std::move(focusEvent), msg.header.seq);
            break;
        }

        case InputMessage::Type::CAPTURE: {
            std::unique_ptr<CaptureEvent> captureEvent = createCaptureEvent(msg);
            mCallbacks.onCaptureEvent(std::move(captureEvent), msg.header.seq);
            break;
        }

        case InputMessage::Type::DRAG: {
            std::unique_ptr<DragEvent> dragEvent = createDragEvent(msg);
            mCallbacks.onDragEvent(std::move(dragEvent), msg.header.seq);
            break;
        }

        case InputMessage::Type::TOUCH_MODE: {
            std::unique_ptr<TouchModeEvent> touchModeEvent = createTouchModeEvent(msg);
            mCallbacks.onTouchModeEvent(std::move(touchModeEvent), msg.header.seq);
            break;
        }
    }
}

std::pair<std::unique_ptr<MotionEvent>, std::optional<uint32_t>>
InputConsumerNoResampling::createBatchedMotionEvent(const std::optional<nsecs_t> requestedFrameTime,
                                                    std::queue<InputMessage>& messages) {
    std::unique_ptr<MotionEvent> motionEvent;
    std::optional<uint32_t> firstSeqForBatch;

    LOG_IF(FATAL, messages.empty()) << "messages queue is empty!";
    const DeviceId deviceId = messages.front().body.motion.deviceId;
    const auto resampler = mResamplers.find(deviceId);
    const nanoseconds resampleLatency = (resampler != mResamplers.cend())
            ? resampler->second->getResampleLatency()
            : nanoseconds{0};
    // When batching is not enabled, we want to consume all events. That's equivalent to having an
    // infinite requestedFrameTime.
    const nanoseconds adjustedFrameTime = (requestedFrameTime.has_value())
            ? (nanoseconds{*requestedFrameTime} - resampleLatency)
            : nanoseconds{std::numeric_limits<nsecs_t>::max()};

    while (!messages.empty() &&
           (messages.front().body.motion.eventTime <= adjustedFrameTime.count())) {
        if (motionEvent == nullptr) {
            motionEvent = createMotionEvent(messages.front());
            firstSeqForBatch = messages.front().header.seq;
            const auto [_, inserted] = mBatchedSequenceNumbers.insert({*firstSeqForBatch, {}});
            LOG_IF(FATAL, !inserted)
                    << "The sequence " << messages.front().header.seq << " was already present!";
        } else {
            addSample(*motionEvent, messages.front());
            mBatchedSequenceNumbers[*firstSeqForBatch].push_back(messages.front().header.seq);
        }
        messages.pop();
    }

    // Check if resampling should be performed.
    InputMessage* futureSample = nullptr;
    if (!messages.empty()) {
        futureSample = &messages.front();
    }
    if ((motionEvent != nullptr) && (resampler != mResamplers.cend()) &&
        (requestedFrameTime.has_value())) {
        resampler->second->resampleMotionEvent(nanoseconds{*requestedFrameTime}, *motionEvent,
                                               futureSample);
    }

    return std::make_pair(std::move(motionEvent), firstSeqForBatch);
}

bool InputConsumerNoResampling::consumeBatchedInputEvents(
        std::optional<DeviceId> deviceId, std::optional<nsecs_t> requestedFrameTime) {
    ensureCalledOnLooperThread(__func__);
    bool producedEvents = false;

    for (auto deviceIdIter = (deviceId.has_value()) ? (mBatches.find(*deviceId))
                                                    : (mBatches.begin());
         deviceIdIter != mBatches.cend(); ++deviceIdIter) {
        std::queue<InputMessage>& messages = deviceIdIter->second;
        auto [motion, firstSeqForBatch] = createBatchedMotionEvent(requestedFrameTime, messages);
        if (motion != nullptr) {
            LOG_ALWAYS_FATAL_IF(!firstSeqForBatch.has_value());
            mCallbacks.onMotionEvent(std::move(motion), *firstSeqForBatch);
            producedEvents = true;
        } else {
            // This is OK, it just means that the requestedFrameTime is too old (all events that we
            // have pending are in the future of the requestedFrameTime). Maybe print a warning? If
            // there are multiple devices active though, this might be normal and can just be
            // ignored, unless none of them resulted in any consumption (in that case, this function
            // would already return "false" so we could just leave it up to the caller).
        }

        if (deviceId.has_value()) {
            // We already consumed events for this device. Break here to prevent iterating over the
            // other devices.
            break;
        }
    }
    std::erase_if(mBatches, [](const auto& pair) { return pair.second.empty(); });
    return producedEvents;
}

bool InputConsumerNoResampling::consumeBatchedInputEvents(
        std::optional<nsecs_t> requestedFrameTime) {
    return consumeBatchedInputEvents(/*deviceId=*/std::nullopt, requestedFrameTime);
}

void InputConsumerNoResampling::ensureCalledOnLooperThread(const char* func) const {
    sp<Looper> callingThreadLooper = Looper::getForThread();
    if (callingThreadLooper != mLooper) {
        LOG(FATAL) << "The function " << func << " can only be called on the looper thread";
    }
}

std::string InputConsumerNoResampling::dump() const {
    ensureCalledOnLooperThread(__func__);
    std::string out;
    if (mOutboundQueue.empty()) {
        out += "mOutboundQueue: <empty>\n";
    } else {
        out += "mOutboundQueue:\n";
        // Make a copy of mOutboundQueue for printing destructively. Unfortunately std::queue
        // doesn't provide a good way to iterate over the entire container.
        std::queue<InputMessage> tmpQueue = mOutboundQueue;
        while (!tmpQueue.empty()) {
            out += std::string("  ") + outboundMessageToString(tmpQueue.front()) + "\n";
            tmpQueue.pop();
        }
    }

    if (mBatches.empty()) {
        out += "mBatches: <empty>\n";
    } else {
        out += "mBatches:\n";
        for (const auto& [deviceId, messages] : mBatches) {
            out += "  Device id ";
            out += std::to_string(deviceId);
            out += ":\n";
            // Make a copy of mOutboundQueue for printing destructively. Unfortunately std::queue
            // doesn't provide a good way to iterate over the entire container.
            std::queue<InputMessage> tmpQueue = messages;
            while (!tmpQueue.empty()) {
                LOG_ALWAYS_FATAL_IF(tmpQueue.front().header.type != InputMessage::Type::MOTION);
                std::unique_ptr<MotionEvent> motion = createMotionEvent(tmpQueue.front());
                out += std::string("    ") + streamableToString(*motion) + "\n";
                tmpQueue.pop();
            }
        }
    }

    return out;
}

} // namespace android
