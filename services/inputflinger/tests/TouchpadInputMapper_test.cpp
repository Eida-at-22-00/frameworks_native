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

#include "TouchpadInputMapper.h"

#include <android-base/logging.h>
#include <gtest/gtest.h>
#include <input/AccelerationCurve.h>

#include <log/log.h>
#include <thread>
#include "InputMapperTest.h"
#include "InterfaceMocks.h"
#include "TestConstants.h"
#include "TestEventMatchers.h"

#define TAG "TouchpadInputMapper_test"

namespace android {

using testing::Return;
using testing::VariantWith;
constexpr auto ACTION_DOWN = AMOTION_EVENT_ACTION_DOWN;
constexpr auto ACTION_UP = AMOTION_EVENT_ACTION_UP;
constexpr auto BUTTON_PRESS = AMOTION_EVENT_ACTION_BUTTON_PRESS;
constexpr auto BUTTON_RELEASE = AMOTION_EVENT_ACTION_BUTTON_RELEASE;
constexpr auto HOVER_MOVE = AMOTION_EVENT_ACTION_HOVER_MOVE;
constexpr auto HOVER_ENTER = AMOTION_EVENT_ACTION_HOVER_ENTER;
constexpr auto HOVER_EXIT = AMOTION_EVENT_ACTION_HOVER_EXIT;
constexpr ui::LogicalDisplayId DISPLAY_ID = ui::LogicalDisplayId::DEFAULT;
constexpr int32_t DISPLAY_WIDTH = 480;
constexpr int32_t DISPLAY_HEIGHT = 800;
constexpr std::optional<uint8_t> NO_PORT = std::nullopt; // no physical port is specified

/**
 * Unit tests for TouchpadInputMapper.
 */
class TouchpadInputMapperTest : public InputMapperUnitTest {
protected:
    void SetUp() override {
        InputMapperUnitTest::SetUp();

        // Present scan codes: BTN_TOUCH and BTN_TOOL_FINGER
        expectScanCodes(/*present=*/true,
                        {BTN_LEFT, BTN_RIGHT, BTN_TOOL_FINGER, BTN_TOOL_QUINTTAP, BTN_TOUCH,
                         BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP});
        // Missing scan codes that the mapper checks for.
        expectScanCodes(/*present=*/false,
                        {BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOOL_BRUSH, BTN_TOOL_PENCIL,
                         BTN_TOOL_AIRBRUSH});

        // Current scan code state - all keys are UP by default
        setScanCodeState(KeyState::UP, {BTN_TOUCH,          BTN_STYLUS,
                                        BTN_STYLUS2,        BTN_0,
                                        BTN_TOOL_FINGER,    BTN_TOOL_PEN,
                                        BTN_TOOL_RUBBER,    BTN_TOOL_BRUSH,
                                        BTN_TOOL_PENCIL,    BTN_TOOL_AIRBRUSH,
                                        BTN_TOOL_MOUSE,     BTN_TOOL_LENS,
                                        BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                        BTN_TOOL_QUADTAP,   BTN_TOOL_QUINTTAP,
                                        BTN_LEFT,           BTN_RIGHT,
                                        BTN_MIDDLE,         BTN_BACK,
                                        BTN_SIDE,           BTN_FORWARD,
                                        BTN_EXTRA,          BTN_TASK});

        setKeyCodeState(KeyState::UP,
                        {AKEYCODE_STYLUS_BUTTON_PRIMARY, AKEYCODE_STYLUS_BUTTON_SECONDARY});

        // Key mappings
        EXPECT_CALL(mMockEventHub,
                    mapKey(EVENTHUB_ID, BTN_LEFT, /*usageCode=*/0, /*metaState=*/0, testing::_,
                           testing::_, testing::_))
                .WillRepeatedly(Return(NAME_NOT_FOUND));

        // Input properties - only INPUT_PROP_BUTTONPAD
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_BUTTONPAD))
                .WillRepeatedly(Return(true));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_SEMI_MT))
                .WillRepeatedly(Return(false));

        // Axes that the device has
        setupAxis(ABS_MT_SLOT, /*valid=*/true, /*min=*/0, /*max=*/4, /*resolution=*/0);
        setupAxis(ABS_MT_POSITION_X, /*valid=*/true, /*min=*/0, /*max=*/2000, /*resolution=*/24);
        setupAxis(ABS_MT_POSITION_Y, /*valid=*/true, /*min=*/0, /*max=*/1000, /*resolution=*/24);
        setupAxis(ABS_MT_PRESSURE, /*valid=*/true, /*min*/ 0, /*max=*/255, /*resolution=*/0);
        // Axes that the device does not have
        setupAxis(ABS_MT_ORIENTATION, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOUCH_MAJOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOUCH_MINOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_WIDTH_MAJOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_WIDTH_MINOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TRACKING_ID, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_DISTANCE, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOOL_TYPE, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);

        EXPECT_CALL(mMockEventHub, getAbsoluteAxisValue(EVENTHUB_ID, ABS_MT_SLOT))
                .WillRepeatedly(Return(0));
        EXPECT_CALL(mMockEventHub, getMtSlotValues(EVENTHUB_ID, testing::_, testing::_))
                .WillRepeatedly([]() -> base::Result<std::vector<int32_t>> {
                    return base::ResultError("Axis not supported", NAME_NOT_FOUND);
                });
        mMapper = createInputMapper<TouchpadInputMapper>(*mDeviceContext, mReaderConfiguration);
    }
};

/**
 * Start moving the finger and then click the left touchpad button. Check whether HOVER_EXIT is
 * generated when hovering stops. Currently, it is not.
 * In the current implementation, HOVER_MOVE and ACTION_DOWN events are not sent out right away,
 * but only after the button is released.
 */
TEST_F(TouchpadInputMapperTest, HoverAndLeftButtonPress) {
    mFakePolicy->setDefaultPointerDisplayId(DISPLAY_ID);
    DisplayViewport viewport =
            createViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, ui::ROTATION_0,
                           /*isActive=*/true, "local:0", NO_PORT, ViewportType::INTERNAL);
    mFakePolicy->addDisplayViewport(viewport);
    std::list<NotifyArgs> args;

    args += mMapper->reconfigure(systemTime(SYSTEM_TIME_MONOTONIC), mReaderConfiguration,
                                 InputReaderConfiguration::Change::DISPLAY_INFO);
    ASSERT_THAT(args, testing::IsEmpty());

    args += process(EV_ABS, ABS_MT_TRACKING_ID, 1);
    args += process(EV_KEY, BTN_TOUCH, 1);
    setScanCodeState(KeyState::DOWN, {BTN_TOOL_FINGER});
    args += process(EV_KEY, BTN_TOOL_FINGER, 1);
    args += process(EV_ABS, ABS_MT_POSITION_X, 50);
    args += process(EV_ABS, ABS_MT_POSITION_Y, 50);
    args += process(EV_ABS, ABS_MT_PRESSURE, 1);
    args += process(EV_SYN, SYN_REPORT, 0);
    ASSERT_THAT(args, testing::IsEmpty());

    // Without this sleep, the test fails.
    // TODO(b/284133337): Figure out whether this can be removed
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    args += process(EV_KEY, BTN_LEFT, 1);
    setScanCodeState(KeyState::DOWN, {BTN_LEFT});
    args += process(EV_SYN, SYN_REPORT, 0);

    args += process(EV_KEY, BTN_LEFT, 0);
    setScanCodeState(KeyState::UP, {BTN_LEFT});
    args += process(EV_SYN, SYN_REPORT, 0);
    ASSERT_THAT(args,
                ElementsAre(VariantWith<NotifyMotionArgs>(WithMotionAction(HOVER_ENTER)),
                            VariantWith<NotifyMotionArgs>(WithMotionAction(HOVER_MOVE)),
                            VariantWith<NotifyMotionArgs>(WithMotionAction(HOVER_EXIT)),
                            VariantWith<NotifyMotionArgs>(WithMotionAction(ACTION_DOWN)),
                            VariantWith<NotifyMotionArgs>(WithMotionAction(BUTTON_PRESS)),
                            VariantWith<NotifyMotionArgs>(WithMotionAction(BUTTON_RELEASE)),
                            VariantWith<NotifyMotionArgs>(WithMotionAction(ACTION_UP)),
                            VariantWith<NotifyMotionArgs>(WithMotionAction(HOVER_ENTER))));

    // Liftoff
    args.clear();
    args += process(EV_ABS, ABS_MT_PRESSURE, 0);
    args += process(EV_ABS, ABS_MT_TRACKING_ID, -1);
    args += process(EV_KEY, BTN_TOUCH, 0);
    setScanCodeState(KeyState::UP, {BTN_TOOL_FINGER});
    args += process(EV_KEY, BTN_TOOL_FINGER, 0);
    args += process(EV_SYN, SYN_REPORT, 0);
    ASSERT_THAT(args, testing::IsEmpty());
}

TEST_F(TouchpadInputMapperTest, TouchpadHardwareState) {
    mReaderConfiguration.shouldNotifyTouchpadHardwareState = true;
    std::list<NotifyArgs> args =
            mMapper->reconfigure(ARBITRARY_TIME, mReaderConfiguration,
                                 InputReaderConfiguration::Change::TOUCHPAD_SETTINGS);

    args += process(EV_ABS, ABS_MT_TRACKING_ID, 1);
    args += process(EV_KEY, BTN_TOUCH, 1);
    setScanCodeState(KeyState::DOWN, {BTN_TOOL_FINGER});
    args += process(EV_KEY, BTN_TOOL_FINGER, 1);
    args += process(EV_ABS, ABS_MT_POSITION_X, 50);
    args += process(EV_ABS, ABS_MT_POSITION_Y, 50);
    args += process(EV_ABS, ABS_MT_PRESSURE, 1);
    args += process(EV_SYN, SYN_REPORT, 0);

    mFakePolicy->assertTouchpadHardwareStateNotified();
}

TEST_F(TouchpadInputMapperTest, TouchpadAccelerationDisabled) {
    mReaderConfiguration.touchpadAccelerationEnabled = false;
    mReaderConfiguration.touchpadPointerSpeed = 3;

    std::list<NotifyArgs> args =
            mMapper->reconfigure(ARBITRARY_TIME, mReaderConfiguration,
                                 InputReaderConfiguration::Change::TOUCHPAD_SETTINGS);
    auto* touchpadMapper = static_cast<TouchpadInputMapper*>(mMapper.get());

    const auto accelCurvePropsDisabled =
            touchpadMapper->getGesturePropertyForTesting("Pointer Accel Curve");
    ASSERT_TRUE(accelCurvePropsDisabled.has_value());
    std::vector<double> curveValuesDisabled = accelCurvePropsDisabled.value().getRealValues();
    std::vector<AccelerationCurveSegment> curve =
            createFlatAccelerationCurve(mReaderConfiguration.touchpadPointerSpeed);
    double expectedBaseGain = curve[0].baseGain;
    ASSERT_EQ(curveValuesDisabled[0], std::numeric_limits<double>::infinity());
    ASSERT_EQ(curveValuesDisabled[1], 0);
    ASSERT_NEAR(curveValuesDisabled[2], expectedBaseGain, EPSILON);
    ASSERT_EQ(curveValuesDisabled[3], 0);
}

TEST_F(TouchpadInputMapperTest, TouchpadAccelerationEnabled) {
    // Enable touchpad acceleration.
    mReaderConfiguration.touchpadAccelerationEnabled = true;
    mReaderConfiguration.touchpadPointerSpeed = 3;

    std::list<NotifyArgs> args =
            mMapper->reconfigure(ARBITRARY_TIME, mReaderConfiguration,
                                 InputReaderConfiguration::Change::TOUCHPAD_SETTINGS);
    ASSERT_THAT(args, testing::IsEmpty());

    auto* touchpadMapper = static_cast<TouchpadInputMapper*>(mMapper.get());

    // Get the acceleration curve properties when acceleration is enabled.
    const auto accelCurvePropsEnabled =
            touchpadMapper->getGesturePropertyForTesting("Pointer Accel Curve");
    ASSERT_TRUE(accelCurvePropsEnabled.has_value());

    // Get the curve values.
    std::vector<double> curveValuesEnabled = accelCurvePropsEnabled.value().getRealValues();

    // Use createAccelerationCurveForPointerSensitivity to get expected curve segments.
    std::vector<AccelerationCurveSegment> expectedCurveSegments =
            createAccelerationCurveForPointerSensitivity(mReaderConfiguration.touchpadPointerSpeed);

    // Iterate through the segments and compare the values.
    for (size_t i = 0; i < expectedCurveSegments.size(); ++i) {
        // Check max speed.
        if (std::isinf(expectedCurveSegments[i].maxPointerSpeedMmPerS)) {
            ASSERT_TRUE(std::isinf(curveValuesEnabled[i * 4 + 0]));
        } else {
            ASSERT_NEAR(curveValuesEnabled[i * 4 + 0],
                        expectedCurveSegments[i].maxPointerSpeedMmPerS, EPSILON);
        }

        // Check that the x^2 term is zero.
        ASSERT_NEAR(curveValuesEnabled[i * 4 + 1], 0, EPSILON);
        ASSERT_NEAR(curveValuesEnabled[i * 4 + 2], expectedCurveSegments[i].baseGain, EPSILON);
        ASSERT_NEAR(curveValuesEnabled[i * 4 + 3], expectedCurveSegments[i].reciprocal, EPSILON);
    }
}

} // namespace android
