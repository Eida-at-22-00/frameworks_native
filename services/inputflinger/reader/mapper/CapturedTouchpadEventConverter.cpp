/*
 * Copyright 2023 The Android Open Source Project
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

#include "CapturedTouchpadEventConverter.h"

#include <optional>
#include <sstream>

#include <android-base/stringprintf.h>
#include <com_android_input_flags.h>
#include <input/PrintTools.h>
#include <linux/input-event-codes.h>
#include <log/log_main.h>

namespace android {

namespace {

static constexpr uint32_t SOURCE = AINPUT_SOURCE_TOUCHPAD;

int32_t actionWithIndex(int32_t action, int32_t index) {
    return action | (index << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
}

template <typename T>
size_t firstUnmarkedBit(T set) {
    // TODO: replace with std::countr_one from <bit> when that's available
    LOG_ALWAYS_FATAL_IF(set.all());
    size_t i = 0;
    while (set.test(i)) {
        i++;
    }
    return i;
}

void addRawMotionRange(InputDeviceInfo& deviceInfo, int32_t androidAxis,
                       RawAbsoluteAxisInfo& evdevAxis) {
    deviceInfo.addMotionRange(androidAxis, SOURCE, evdevAxis.minValue, evdevAxis.maxValue,
                              evdevAxis.flat, evdevAxis.fuzz, evdevAxis.resolution);
}

} // namespace

CapturedTouchpadEventConverter::CapturedTouchpadEventConverter(
        InputReaderContext& readerContext, const InputDeviceContext& deviceContext,
        MultiTouchMotionAccumulator& motionAccumulator, int32_t deviceId)
      : mDeviceId(deviceId),
        mReaderContext(readerContext),
        mDeviceContext(deviceContext),
        mMotionAccumulator(motionAccumulator),
        mHasTouchMinor(deviceContext.hasAbsoluteAxis(ABS_MT_TOUCH_MINOR)),
        mHasToolMinor(deviceContext.hasAbsoluteAxis(ABS_MT_WIDTH_MINOR)) {
    if (std::optional<RawAbsoluteAxisInfo> orientation =
                deviceContext.getAbsoluteAxisInfo(ABS_MT_ORIENTATION);
        orientation) {
        if (orientation->maxValue > 0) {
            mOrientationScale = M_PI_2 / orientation->maxValue;
        } else if (orientation->minValue < 0) {
            mOrientationScale = -M_PI_2 / orientation->minValue;
        }
    }

    // TODO(b/275369880): support touch.pressure.calibration and .scale properties when captured.
    if (std::optional<RawAbsoluteAxisInfo> pressure =
                deviceContext.getAbsoluteAxisInfo(ABS_MT_PRESSURE);
        pressure && pressure->maxValue > 0) {
        mPressureScale = 1.0 / pressure->maxValue;
    }

    std::optional<RawAbsoluteAxisInfo> touchMajor =
            deviceContext.getAbsoluteAxisInfo(ABS_MT_TOUCH_MAJOR);
    std::optional<RawAbsoluteAxisInfo> toolMajor =
            deviceContext.getAbsoluteAxisInfo(ABS_MT_WIDTH_MAJOR);
    mHasTouchMajor = touchMajor.has_value();
    mHasToolMajor = toolMajor.has_value();
    if (mHasTouchMajor && touchMajor->maxValue != 0) {
        mSizeScale = 1.0f / touchMajor->maxValue;
    } else if (mHasToolMajor && toolMajor->maxValue != 0) {
        mSizeScale = 1.0f / toolMajor->maxValue;
    }
}

std::string CapturedTouchpadEventConverter::dump() const {
    std::stringstream out;
    out << "Orientation scale: " << mOrientationScale << "\n";
    out << "Pressure scale: " << mPressureScale << "\n";
    out << "Size scale: " << mSizeScale << "\n";

    out << "Dimension axes:";
    if (mHasTouchMajor) out << " touch major";
    if (mHasTouchMinor) out << ", touch minor";
    if (mHasToolMajor) out << ", tool major";
    if (mHasToolMinor) out << ", tool minor";
    out << "\n";

    out << "Down time: " << mDownTime << "\n";
    out << StringPrintf("Button state: 0x%08x\n", mButtonState);

    out << StringPrintf("Pointer IDs in use: %s\n", mPointerIdsInUse.to_string().c_str());

    out << "Pointer IDs for slot numbers:\n";
    out << addLinePrefix(dumpMap(mPointerIdForSlotNumber), "  ") << "\n";
    return out.str();
}

void CapturedTouchpadEventConverter::populateMotionRanges(InputDeviceInfo& info) const {
    tryAddRawMotionRangeWithRelative(/*byref*/ info, AMOTION_EVENT_AXIS_X,
                                     AMOTION_EVENT_AXIS_RELATIVE_X, ABS_MT_POSITION_X);
    tryAddRawMotionRangeWithRelative(/*byref*/ info, AMOTION_EVENT_AXIS_Y,
                                     AMOTION_EVENT_AXIS_RELATIVE_Y, ABS_MT_POSITION_Y);
    tryAddRawMotionRange(/*byref*/ info, AMOTION_EVENT_AXIS_TOUCH_MAJOR, ABS_MT_TOUCH_MAJOR);
    tryAddRawMotionRange(/*byref*/ info, AMOTION_EVENT_AXIS_TOUCH_MINOR, ABS_MT_TOUCH_MINOR);
    tryAddRawMotionRange(/*byref*/ info, AMOTION_EVENT_AXIS_TOOL_MAJOR, ABS_MT_WIDTH_MAJOR);
    tryAddRawMotionRange(/*byref*/ info, AMOTION_EVENT_AXIS_TOOL_MINOR, ABS_MT_WIDTH_MINOR);

    if (mDeviceContext.hasAbsoluteAxis(ABS_MT_PRESSURE)) {
        info.addMotionRange(AMOTION_EVENT_AXIS_PRESSURE, SOURCE, 0, 1, 0, 0, 0);
    }

    if (std::optional<RawAbsoluteAxisInfo> orientation =
                mDeviceContext.getAbsoluteAxisInfo(ABS_MT_ORIENTATION);
        orientation && (orientation->maxValue > 0 || orientation->minValue < 0)) {
        info.addMotionRange(AMOTION_EVENT_AXIS_ORIENTATION, SOURCE, -M_PI_2, M_PI_2, 0, 0, 0);
    }

    if (mHasTouchMajor || mHasToolMajor) {
        info.addMotionRange(AMOTION_EVENT_AXIS_SIZE, SOURCE, 0, 1, 0, 0, 0);
    }
}

void CapturedTouchpadEventConverter::tryAddRawMotionRange(InputDeviceInfo& deviceInfo,
                                                          int32_t androidAxis,
                                                          int32_t evdevAxis) const {
    std::optional<RawAbsoluteAxisInfo> info = mDeviceContext.getAbsoluteAxisInfo(evdevAxis);
    if (info) {
        addRawMotionRange(/*byref*/ deviceInfo, androidAxis, *info);
    }
}

void CapturedTouchpadEventConverter::tryAddRawMotionRangeWithRelative(InputDeviceInfo& deviceInfo,
                                                                      int32_t androidAxis,
                                                                      int32_t androidRelativeAxis,
                                                                      int32_t evdevAxis) const {
    std::optional<RawAbsoluteAxisInfo> axisInfo = mDeviceContext.getAbsoluteAxisInfo(evdevAxis);
    if (axisInfo) {
        addRawMotionRange(/*byref*/ deviceInfo, androidAxis, *axisInfo);

        // The largest movement we could possibly report on a relative axis is from the minimum to
        // the maximum (or vice versa) of the absolute axis.
        float range = axisInfo->maxValue - axisInfo->minValue;
        deviceInfo.addMotionRange(androidRelativeAxis, SOURCE, -range, range, axisInfo->flat,
                                  axisInfo->fuzz, axisInfo->resolution);
    }
}

void CapturedTouchpadEventConverter::reset() {
    mCursorButtonAccumulator.reset(mDeviceContext);
    mDownTime = 0;
    mPointerIdsInUse.reset();
    mPointerIdForSlotNumber.clear();
}

std::list<NotifyArgs> CapturedTouchpadEventConverter::process(const RawEvent& rawEvent) {
    std::list<NotifyArgs> out;
    if (rawEvent.type == EV_SYN && rawEvent.code == SYN_REPORT) {
        out = sync(rawEvent.when, rawEvent.readTime);
        mMotionAccumulator.finishSync();
    }

    mCursorButtonAccumulator.process(rawEvent);
    mMotionAccumulator.process(rawEvent);
    return out;
}

std::list<NotifyArgs> CapturedTouchpadEventConverter::sync(nsecs_t when, nsecs_t readTime) {
    std::list<NotifyArgs> out;
    std::vector<PointerCoords> coords;
    std::vector<PointerProperties> properties;
    std::map<size_t /*slotNumber*/, size_t /*coordsIndex*/> coordsIndexForSlotNumber;

    // For all the touches that were already down, send a MOVE event with their updated coordinates.
    // A convention of the MotionEvent API is that pointer coordinates in UP events match the
    // pointer's coordinates from the previous MOVE, so we still include touches here even if
    // they've been lifted in this evdev frame.
    if (!mPointerIdForSlotNumber.empty()) {
        for (const auto [slotNumber, pointerId] : mPointerIdForSlotNumber) {
            // Note that we don't check whether the touch has actually moved — it's rare for a touch
            // to stay perfectly still between frames, and if it does the worst that can happen is
            // an extra MOVE event, so it's not worth the overhead of checking for changes.
            coordsIndexForSlotNumber[slotNumber] = coords.size();
            coords.push_back(makePointerCoordsForSlot(slotNumber));
            properties.push_back({.id = pointerId, .toolType = ToolType::FINGER});
        }
        out.push_back(
                makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_MOVE, coords, properties));
        // For any further events we send from this sync, the pointers won't have moved relative to
        // the positions we just reported in this MOVE event, so zero out the relative axes.
        for (PointerCoords& pointer : coords) {
            pointer.setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_X, 0);
            pointer.setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_Y, 0);
        }
    }

    std::vector<size_t> upSlots, downSlots;
    for (size_t i = 0; i < mMotionAccumulator.getSlotCount(); i++) {
        const MultiTouchMotionAccumulator::Slot& slot = mMotionAccumulator.getSlot(i);
        // Some touchpads continue to report contacts even after they've identified them as palms.
        // We don't currently have a way to mark these as palms when reporting to apps, so don't
        // report them at all.
        const bool isInUse = slot.isInUse() && slot.getToolType() != ToolType::PALM;
        const bool wasInUse = mPointerIdForSlotNumber.find(i) != mPointerIdForSlotNumber.end();
        if (isInUse && !wasInUse) {
            downSlots.push_back(i);
        } else if (!isInUse && wasInUse) {
            upSlots.push_back(i);
        }
    }

    // Send BUTTON_RELEASE events. (This has to happen before any UP events to avoid sending
    // BUTTON_RELEASE events without any pointers.)
    uint32_t newButtonState;
    if (coords.size() - upSlots.size() + downSlots.size() == 0) {
        // If there won't be any pointers down after this evdev sync, we won't be able to send
        // button updates on their own, as motion events without pointers are invalid. To avoid
        // erroneously reporting buttons being held for long periods, send BUTTON_RELEASE events for
        // all pressed buttons when the last pointer is lifted.
        //
        // This also prevents us from sending BUTTON_PRESS events too early in the case of touchpads
        // which report a button press one evdev sync before reporting a touch going down.
        newButtonState = 0;
    } else {
        newButtonState = mCursorButtonAccumulator.getButtonState();
    }
    for (uint32_t button = 1; button <= AMOTION_EVENT_BUTTON_FORWARD; button <<= 1) {
        if (!(newButtonState & button) && mButtonState & button) {
            mButtonState &= ~button;
            out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_BUTTON_RELEASE,
                                         coords, properties, /*actionButton=*/button));
        }
    }

    // For any touches that were lifted, send UP or POINTER_UP events.
    for (size_t slotNumber : upSlots) {
        const size_t indexToRemove = coordsIndexForSlotNumber.at(slotNumber);
        const bool cancel = mMotionAccumulator.getSlot(slotNumber).getToolType() == ToolType::PALM;
        int32_t action;
        if (coords.size() == 1) {
            action = cancel ? AMOTION_EVENT_ACTION_CANCEL : AMOTION_EVENT_ACTION_UP;
        } else {
            action = actionWithIndex(AMOTION_EVENT_ACTION_POINTER_UP, indexToRemove);
        }
        out.push_back(makeMotionArgs(when, readTime, action, coords, properties, /*actionButton=*/0,
                                     /*flags=*/cancel ? AMOTION_EVENT_FLAG_CANCELED : 0));

        freePointerIdForSlot(slotNumber);
        mPreviousCoordsForSlotNumber.erase(slotNumber);
        coords.erase(coords.begin() + indexToRemove);
        properties.erase(properties.begin() + indexToRemove);
        // Now that we've removed some coords and properties, we might have to update the slot
        // number to coords index mapping.
        coordsIndexForSlotNumber.erase(slotNumber);
        for (auto& [_, index] : coordsIndexForSlotNumber) {
            if (index > indexToRemove) {
                index--;
            }
        }
    }

    // For new touches, send DOWN or POINTER_DOWN events.
    for (size_t slotNumber : downSlots) {
        const size_t coordsIndex = coords.size();
        const int32_t action = coords.empty()
                ? AMOTION_EVENT_ACTION_DOWN
                : actionWithIndex(AMOTION_EVENT_ACTION_POINTER_DOWN, coordsIndex);

        coordsIndexForSlotNumber[slotNumber] = coordsIndex;
        coords.push_back(makePointerCoordsForSlot(slotNumber));
        properties.push_back(
                {.id = allocatePointerIdToSlot(slotNumber), .toolType = ToolType::FINGER});

        out.push_back(makeMotionArgs(when, readTime, action, coords, properties));
    }

    for (uint32_t button = 1; button <= AMOTION_EVENT_BUTTON_FORWARD; button <<= 1) {
        if (newButtonState & button && !(mButtonState & button)) {
            mButtonState |= button;
            out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_BUTTON_PRESS, coords,
                                         properties, /*actionButton=*/button));
        }
    }
    return out;
}

NotifyMotionArgs CapturedTouchpadEventConverter::makeMotionArgs(
        nsecs_t when, nsecs_t readTime, int32_t action, const std::vector<PointerCoords>& coords,
        const std::vector<PointerProperties>& properties, int32_t actionButton, int32_t flags) {
    LOG_ALWAYS_FATAL_IF(coords.size() != properties.size(),
                        "Mismatched coords and properties arrays.");
    return NotifyMotionArgs(mReaderContext.getNextId(), when, readTime, mDeviceId, SOURCE,
                            ui::LogicalDisplayId::INVALID, /*policyFlags=*/POLICY_FLAG_WAKE, action,
                            /*actionButton=*/actionButton, flags,
                            mReaderContext.getGlobalMetaState(), mButtonState,
                            MotionClassification::NONE, AMOTION_EVENT_EDGE_FLAG_NONE, coords.size(),
                            properties.data(), coords.data(), /*xPrecision=*/1.0f,
                            /*yPrecision=*/1.0f, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                            AMOTION_EVENT_INVALID_CURSOR_POSITION, mDownTime, /*videoFrames=*/{});
}

PointerCoords CapturedTouchpadEventConverter::makePointerCoordsForSlot(size_t slotNumber) {
    const MultiTouchMotionAccumulator::Slot& slot = mMotionAccumulator.getSlot(slotNumber);
    PointerCoords coords;
    coords.clear();
    coords.setAxisValue(AMOTION_EVENT_AXIS_X, slot.getX());
    coords.setAxisValue(AMOTION_EVENT_AXIS_Y, slot.getY());
    if (auto it = mPreviousCoordsForSlotNumber.find(slotNumber);
        it != mPreviousCoordsForSlotNumber.end()) {
        auto [oldX, oldY] = it->second;
        coords.setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_X, slot.getX() - oldX);
        coords.setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_Y, slot.getY() - oldY);
    }
    mPreviousCoordsForSlotNumber[slotNumber] = std::make_pair(slot.getX(), slot.getY());

    coords.setAxisValue(AMOTION_EVENT_AXIS_TOUCH_MAJOR, slot.getTouchMajor());
    coords.setAxisValue(AMOTION_EVENT_AXIS_TOUCH_MINOR, slot.getTouchMinor());
    coords.setAxisValue(AMOTION_EVENT_AXIS_TOOL_MAJOR, slot.getToolMajor());
    coords.setAxisValue(AMOTION_EVENT_AXIS_TOOL_MINOR, slot.getToolMinor());
    coords.setAxisValue(AMOTION_EVENT_AXIS_ORIENTATION, slot.getOrientation() * mOrientationScale);
    coords.setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, slot.getPressure() * mPressureScale);
    float size = 0;
    // TODO(b/275369880): support touch.size.calibration and .isSummed properties when captured.
    if (mHasTouchMajor) {
        size = mHasTouchMinor ? (slot.getTouchMajor() + slot.getTouchMinor()) / 2
                              : slot.getTouchMajor();
    } else if (mHasToolMajor) {
        size = mHasToolMinor ? (slot.getToolMajor() + slot.getToolMinor()) / 2
                             : slot.getToolMajor();
    }
    coords.setAxisValue(AMOTION_EVENT_AXIS_SIZE, size * mSizeScale);
    return coords;
}

int32_t CapturedTouchpadEventConverter::allocatePointerIdToSlot(size_t slotNumber) {
    const int32_t pointerId = firstUnmarkedBit(mPointerIdsInUse);
    mPointerIdsInUse.set(pointerId);
    mPointerIdForSlotNumber[slotNumber] = pointerId;
    return pointerId;
}

void CapturedTouchpadEventConverter::freePointerIdForSlot(size_t slotNumber) {
    mPointerIdsInUse.reset(mPointerIdForSlotNumber.at(slotNumber));
    mPointerIdForSlotNumber.erase(slotNumber);
}

} // namespace android
