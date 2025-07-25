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

#pragma once

#include "CursorButtonAccumulator.h"
#include "CursorScrollAccumulator.h"
#include "InputMapper.h"

#include <input/VelocityControl.h>
#include <ui/Rotation.h>

namespace android {

/* Keeps track of cursor movements. */
class CursorMotionAccumulator {
public:
    CursorMotionAccumulator();
    void reset(InputDeviceContext& deviceContext);

    void process(const RawEvent& rawEvent);
    void finishSync();

    inline int32_t getRelativeX() const { return mRelX; }
    inline int32_t getRelativeY() const { return mRelY; }

private:
    int32_t mRelX;
    int32_t mRelY;

    void clearRelativeAxes();
};

class CursorInputMapper : public InputMapper {
public:
    template <class T, class... Args>
    friend std::unique_ptr<T> createInputMapper(InputDeviceContext& deviceContext,
                                                const InputReaderConfiguration& readerConfig,
                                                Args... args);
    virtual ~CursorInputMapper() = default;

    virtual uint32_t getSources() const override;
    virtual void populateDeviceInfo(InputDeviceInfo& deviceInfo) override;
    virtual void dump(std::string& dump) override;
    [[nodiscard]] std::list<NotifyArgs> reconfigure(nsecs_t when,
                                                    const InputReaderConfiguration& readerConfig,
                                                    ConfigurationChanges changes) override;
    [[nodiscard]] std::list<NotifyArgs> reset(nsecs_t when) override;
    [[nodiscard]] std::list<NotifyArgs> process(const RawEvent& rawEvent) override;

    virtual int32_t getScanCodeState(uint32_t sourceMask, int32_t scanCode) override;

    virtual std::optional<ui::LogicalDisplayId> getAssociatedDisplayId() const override;

private:
    // Amount that trackball needs to move in order to generate a key event.
    static const int32_t TRACKBALL_MOVEMENT_THRESHOLD = 6;

    // Immutable configuration parameters.
    struct Parameters {
        enum class Mode {
            // In POINTER mode, the device is a mouse that controls the mouse cursor on the screen,
            // reporting absolute screen locations using SOURCE_MOUSE.
            POINTER,
            // A mouse device in POINTER mode switches to the POINTER_RELATIVE mode when Pointer
            // Capture is enabled, and reports relative values only using SOURCE_MOUSE_RELATIVE.
            POINTER_RELATIVE,
            // A device in NAVIGATION mode emits relative values using SOURCE_TRACKBALL.
            NAVIGATION,

            ftl_last = NAVIGATION,
        };

        Mode mode;
        bool hasAssociatedDisplay;
        bool orientationAware;
    } mParameters;

    CursorButtonAccumulator mCursorButtonAccumulator;
    CursorMotionAccumulator mCursorMotionAccumulator;
    CursorScrollAccumulator mCursorScrollAccumulator;

    int32_t mSource;
    float mXScale;
    float mYScale;
    float mXPrecision;
    float mYPrecision;

    float mVWheelScale;
    float mHWheelScale;

    // Velocity controls for mouse pointer and wheel movements.
    // The controls for X and Y wheel movements are separate to keep them decoupled.
    CurvedVelocityControl mPointerVelocityControl;
    SimpleVelocityControl mWheelXVelocityControl;
    SimpleVelocityControl mWheelYVelocityControl;

    // The display that events generated by this mapper should target. This can be set to
    // LogicalDisplayId::INVALID to target the focused display. If there is no display target (i.e.
    // std::nullopt), all events will be ignored.
    std::optional<ui::LogicalDisplayId> mDisplayId;
    ui::Rotation mOrientation{ui::ROTATION_0};
    FloatRect mBoundsInLogicalDisplay{};

    // The button state as of the last sync.
    int32_t mButtonState;
    nsecs_t mDownTime;
    nsecs_t mLastEventTime;

    bool mMouseReverseVerticalScrolling = false;

    explicit CursorInputMapper(InputDeviceContext& deviceContext,
                               const InputReaderConfiguration& readerConfig);
    void dumpParameters(std::string& dump);
    void configureBasicParams();
    void configureOnPointerCapture(const InputReaderConfiguration& config);
    void configureOnChangePointerSpeed(const InputReaderConfiguration& config);
    void configureOnChangeDisplayInfo(const InputReaderConfiguration& config);
    void configureOnChangeMouseSettings(const InputReaderConfiguration& config);

    [[nodiscard]] std::list<NotifyArgs> sync(nsecs_t when, nsecs_t readTime);

    static Parameters computeParameters(const InputDeviceContext& deviceContext);
};

} // namespace android
