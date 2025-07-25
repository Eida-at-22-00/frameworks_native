/*
 * Copyright 2021 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "LibSurfaceFlingerUnittests"

#include "DisplayTransactionTestHelpers.h"
#include "mock/DisplayHardware/MockDisplayMode.h"
#include "mock/MockDisplayModeSpecs.h"

#include <com_android_graphics_surfaceflinger_flags.h>
#include <common/test/FlagUtils.h>
#include <ftl/fake_guard.h>
#include <scheduler/Fps.h>

using namespace com::android::graphics::surfaceflinger;

#define EXPECT_SET_ACTIVE_CONFIG(displayId, modeId)                                 \
    EXPECT_CALL(*mComposer,                                                         \
                setActiveConfigWithConstraints(displayId,                           \
                                               static_cast<hal::HWConfigId>(        \
                                                       ftl::to_underlying(modeId)), \
                                               _, _))                               \
            .WillOnce(DoAll(SetArgPointee<3>(timeline), Return(hal::Error::NONE)))

namespace android {
namespace {

using android::hardware::graphics::composer::V2_4::Error;
using android::hardware::graphics::composer::V2_4::VsyncPeriodChangeTimeline;

MATCHER_P2(ModeSettledTo, dmc, modeId, "") {
    const auto displayId = arg->getPhysicalId();

    if (const auto desiredOpt = dmc->getDesiredMode(displayId)) {
        *result_listener << "Unsettled desired mode "
                         << ftl::to_underlying(desiredOpt->mode.modePtr->getId());
        return false;
    }

    if (dmc->getActiveMode(displayId).modePtr->getId() != modeId) {
        *result_listener << "Settled to unexpected active mode " << ftl::to_underlying(modeId);
        return false;
    }

    return true;
}

MATCHER_P2(ModeSwitchingTo, flinger, modeId, "") {
    const auto displayId = arg->getPhysicalId();
    auto& dmc = flinger->mutableDisplayModeController();

    if (!dmc.getDesiredMode(displayId)) {
        *result_listener << "No desired mode";
        return false;
    }

    if (dmc.getDesiredMode(displayId)->mode.modePtr->getId() != modeId) {
        *result_listener << "Unexpected desired mode " << ftl::to_underlying(modeId);
        return false;
    }

    // VsyncModulator should react to mode switches on the pacesetter display.
    if (displayId == flinger->scheduler()->pacesetterDisplayId() &&
        !flinger->scheduler()->vsyncModulator().isVsyncConfigEarly()) {
        *result_listener << "VsyncModulator did not shift to early phase";
        return false;
    }

    return true;
}

class DisplayModeSwitchingTest : public DisplayTransactionTest {
public:
    void SetUp() override {
        injectFakeBufferQueueFactory();
        injectFakeNativeWindowSurfaceFactory();

        PrimaryDisplayVariant::setupHwcHotplugCallExpectations(this);
        PrimaryDisplayVariant::setupFramebufferConsumerBufferQueueCallExpectations(this);
        PrimaryDisplayVariant::setupFramebufferProducerBufferQueueCallExpectations(this);
        PrimaryDisplayVariant::setupNativeWindowSurfaceCreationCallExpectations(this);
        PrimaryDisplayVariant::setupHwcGetActiveConfigCallExpectations(this);

        auto selectorPtr = std::make_shared<scheduler::RefreshRateSelector>(kModes, kModeId60);

        setupScheduler(selectorPtr);

        mFlinger.onComposerHalHotplugEvent(kInnerDisplayHwcId, DisplayHotplugEvent::CONNECTED);
        mFlinger.configureAndCommit();

        auto vsyncController = std::make_unique<mock::VsyncController>();
        auto vsyncTracker = std::make_shared<mock::VSyncTracker>();

        EXPECT_CALL(*vsyncTracker, nextAnticipatedVSyncTimeFrom(_, _)).WillRepeatedly(Return(0));
        EXPECT_CALL(*vsyncTracker, currentPeriod())
                .WillRepeatedly(Return(
                        TestableSurfaceFlinger::FakeHwcDisplayInjector::DEFAULT_VSYNC_PERIOD));
        EXPECT_CALL(*vsyncTracker, minFramePeriod())
                .WillRepeatedly(Return(Period::fromNs(
                        TestableSurfaceFlinger::FakeHwcDisplayInjector::DEFAULT_VSYNC_PERIOD)));

        mDisplay = PrimaryDisplayVariant::makeFakeExistingDisplayInjector(this)
                           .setRefreshRateSelector(std::move(selectorPtr))
                           .inject(std::move(vsyncController), std::move(vsyncTracker));
        mDisplayId = mDisplay->getPhysicalId();

        // isVsyncPeriodSwitchSupported should return true, otherwise the SF's HWC proxy
        // will call setActiveConfig instead of setActiveConfigWithConstraints.
        ON_CALL(*mComposer, isSupported(Hwc2::Composer::OptionalFeature::RefreshRateSwitching))
                .WillByDefault(Return(true));
    }

    static constexpr HWDisplayId kInnerDisplayHwcId = PrimaryDisplayVariant::HWC_DISPLAY_ID;
    static constexpr HWDisplayId kOuterDisplayHwcId = kInnerDisplayHwcId + 1;
    static constexpr uint8_t kOuterDisplayPort = 254u;
    static constexpr PhysicalDisplayId kOuterDisplayId =
            PhysicalDisplayId::fromPort(kOuterDisplayPort);

    auto injectOuterDisplay() {
        // For the inner display, this is handled by setupHwcHotplugCallExpectations.
        EXPECT_CALL(*mComposer, getDisplayConnectionType(kOuterDisplayHwcId, _))
                .WillOnce(DoAll(SetArgPointee<1>(IComposerClient::DisplayConnectionType::INTERNAL),
                                Return(hal::V2_4::Error::NONE)));

        constexpr bool kIsPrimary = false;
        TestableSurfaceFlinger::FakeHwcDisplayInjector(kOuterDisplayId, hal::DisplayType::PHYSICAL,
                                                       kIsPrimary)
                .setHwcDisplayId(kOuterDisplayHwcId)
                .setPowerMode(hal::PowerMode::OFF)
                .inject(&mFlinger, mComposer);

        mOuterDisplay = mFakeDisplayInjector.injectInternalDisplay(
                [&](FakeDisplayDeviceInjector& injector) {
                    injector.setPowerMode(hal::PowerMode::OFF);
                    injector.setDisplayModes(mock::cloneForDisplay(kOuterDisplayId, kModes),
                                             kModeId120);
                },
                {.displayId = kOuterDisplayId,
                 .port = kOuterDisplayPort,
                 .hwcDisplayId = kOuterDisplayHwcId,
                 .isPrimary = kIsPrimary});

        return std::forward_as_tuple(mDisplay, mOuterDisplay);
    }

protected:
    void setupScheduler(std::shared_ptr<scheduler::RefreshRateSelector>);

    auto& dmc() { return mFlinger.mutableDisplayModeController(); }

    sp<DisplayDevice> mDisplay, mOuterDisplay;
    PhysicalDisplayId mDisplayId;

    mock::EventThread* mAppEventThread;

    static constexpr DisplayModeId kModeId60{0};
    static constexpr DisplayModeId kModeId90{1};
    static constexpr DisplayModeId kModeId120{2};
    static constexpr DisplayModeId kModeId90_4K{3};
    static constexpr DisplayModeId kModeId60_8K{4};

    static inline const DisplayModePtr kMode60 = createDisplayMode(kModeId60, 60_Hz, 0);
    static inline const DisplayModePtr kMode90 = createDisplayMode(kModeId90, 90_Hz, 1);
    static inline const DisplayModePtr kMode120 = createDisplayMode(kModeId120, 120_Hz, 2);

    static constexpr ui::Size kResolution4K{3840, 2160};
    static constexpr ui::Size kResolution8K{7680, 4320};

    static inline const DisplayModePtr kMode90_4K =
            createDisplayMode(kModeId90_4K, 90_Hz, 3, kResolution4K);
    static inline const DisplayModePtr kMode60_8K =
            createDisplayMode(kModeId60_8K, 60_Hz, 4, kResolution8K);

    static inline const DisplayModes kModes =
            makeModes(kMode60, kMode90, kMode120, kMode90_4K, kMode60_8K);
};

void DisplayModeSwitchingTest::setupScheduler(
        std::shared_ptr<scheduler::RefreshRateSelector> selectorPtr) {
    auto eventThread = std::make_unique<mock::EventThread>();
    mAppEventThread = eventThread.get();
    auto sfEventThread = std::make_unique<mock::EventThread>();

    auto vsyncController = std::make_unique<mock::VsyncController>();
    auto vsyncTracker = std::make_shared<mock::VSyncTracker>();

    EXPECT_CALL(*vsyncTracker, nextAnticipatedVSyncTimeFrom(_, _)).WillRepeatedly(Return(0));
    EXPECT_CALL(*vsyncTracker, currentPeriod())
            .WillRepeatedly(
                    Return(TestableSurfaceFlinger::FakeHwcDisplayInjector::DEFAULT_VSYNC_PERIOD));
    EXPECT_CALL(*vsyncTracker, minFramePeriod())
            .WillRepeatedly(Return(Period::fromNs(
                    TestableSurfaceFlinger::FakeHwcDisplayInjector::DEFAULT_VSYNC_PERIOD)));
    EXPECT_CALL(*vsyncTracker, nextAnticipatedVSyncTimeFrom(_, _)).WillRepeatedly(Return(0));
    mFlinger.setupScheduler(std::move(vsyncController), std::move(vsyncTracker),
                            std::move(eventThread), std::move(sfEventThread),
                            std::move(selectorPtr),
                            TestableSurfaceFlinger::SchedulerCallbackImpl::kNoOp);
}

TEST_F(DisplayModeSwitchingTest, changeRefreshRateWithRefreshRequired) {
    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId60));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId90, 120_Hz)));

    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId90));

    // Verify that next commit will call setActiveConfigWithConstraints in HWC
    const VsyncPeriodChangeTimeline timeline{.refreshRequired = true};
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId90);

    mFlinger.commit();
    Mock::VerifyAndClearExpectations(mComposer);

    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId90));

    // Verify that the next commit will complete the mode change and send
    // a onModeChanged event to the framework.

    EXPECT_CALL(*mAppEventThread,
                onModeChanged(scheduler::FrameRateMode{90_Hz, ftl::as_non_null(kMode90)}));

    mFlinger.commit();
    Mock::VerifyAndClearExpectations(mAppEventThread);

    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId90));
}

TEST_F(DisplayModeSwitchingTest, changeRefreshRateWithoutRefreshRequired) {
    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId60));

    constexpr bool kAllowGroupSwitching = true;
    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(
                      mDisplay->getDisplayToken().promote(),
                      mock::createDisplayModeSpecs(kModeId90, 120_Hz, kAllowGroupSwitching)));

    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId90));

    // Verify that next commit will call setActiveConfigWithConstraints in HWC
    // and complete the mode change.
    const VsyncPeriodChangeTimeline timeline{.refreshRequired = false};
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId90);

    EXPECT_CALL(*mAppEventThread,
                onModeChanged(scheduler::FrameRateMode{90_Hz, ftl::as_non_null(kMode90)}));

    mFlinger.commit();

    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId90));
}

TEST_F(DisplayModeSwitchingTest, changeRefreshRateOnTwoDisplaysWithoutRefreshRequired) {
    const auto [innerDisplay, outerDisplay] = injectOuterDisplay();

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId120));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(innerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId90, 120_Hz,
                                                                               true)));
    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(outerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId60, 60_Hz,
                                                                               true)));

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));

    // Verify that next commit will call setActiveConfigWithConstraints in HWC
    // and complete the mode change.
    const VsyncPeriodChangeTimeline timeline{.refreshRequired = false};
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId90);
    EXPECT_SET_ACTIVE_CONFIG(kOuterDisplayHwcId, kModeId60);

    EXPECT_CALL(*mAppEventThread, onModeChanged(_)).Times(2);

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId60));
}

TEST_F(DisplayModeSwitchingTest, twoConsecutiveSetDesiredDisplayModeSpecs) {
    // Test that if we call setDesiredDisplayModeSpecs while a previous mode change
    // is still being processed the later call will be respected.

    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId60));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId90, 120_Hz)));

    const VsyncPeriodChangeTimeline timeline{.refreshRequired = true};
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId90);

    mFlinger.commit();

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId120,
                                                                               180_Hz)));

    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId120));

    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId120);

    mFlinger.commit();

    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId120));

    mFlinger.commit();

    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId120));
}

TEST_F(DisplayModeSwitchingTest, changeResolutionWithoutRefreshRequired) {
    SET_FLAG_FOR_TEST(flags::synced_resolution_switch, false);

    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId60));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId90_4K,
                                                                               120_Hz)));

    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId90_4K));

    // Verify that next commit will call setActiveConfigWithConstraints in HWC
    // and complete the mode change.
    const VsyncPeriodChangeTimeline timeline{.refreshRequired = false};
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId90_4K);

    EXPECT_CALL(*mAppEventThread, onHotplugReceived(mDisplayId, true));

    // Override expectations set up by PrimaryDisplayVariant.
    EXPECT_CALL(*mConsumer,
                setDefaultBufferSize(static_cast<uint32_t>(kResolution4K.getWidth()),
                                     static_cast<uint32_t>(kResolution4K.getHeight())))
            .WillOnce(Return(NO_ERROR));
    EXPECT_CALL(*mConsumer, consumerConnect(_, false)).WillOnce(Return(NO_ERROR));
    EXPECT_CALL(*mComposer, setClientTargetSlotCount(_)).WillOnce(Return(hal::Error::NONE));

    // Create a new native surface to be used by the recreated display.
    mNativeWindowSurface = nullptr;
    injectFakeNativeWindowSurfaceFactory();
    PrimaryDisplayVariant::setupNativeWindowSurfaceCreationCallExpectations(this);

    mFlinger.commit();

    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId90_4K));
}

TEST_F(DisplayModeSwitchingTest, changeResolutionSynced) {
    SET_FLAG_FOR_TEST(flags::synced_resolution_switch, true);

    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId60));

    // PrimaryDisplayVariant has a 4K size, so switch to 8K.
    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId60_8K,
                                                                               60_Hz)));

    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId60_8K));

    // The mode should not be set until the commit that resizes the display.
    mFlinger.commit();
    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId60_8K));
    mFlinger.commit();
    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId60_8K));

    // Set the display size to match the resolution.
    DisplayState state;
    state.what = DisplayState::eDisplaySizeChanged;
    state.token = mDisplay->getDisplayToken().promote();
    state.width = static_cast<uint32_t>(kResolution8K.width);
    state.height = static_cast<uint32_t>(kResolution8K.height);

    // The next commit should set the mode and resize the framebuffer.
    const VsyncPeriodChangeTimeline timeline{.refreshRequired = false};
    EXPECT_CALL(*mDisplaySurface, resizeBuffers(kResolution8K));
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId60_8K);

    constexpr bool kModeset = true;
    mFlinger.setDisplayStateLocked(state);
    mFlinger.configureAndCommit(kModeset);

    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId60_8K));
}

TEST_F(DisplayModeSwitchingTest, innerXorOuterDisplay) {
    const auto [innerDisplay, outerDisplay] = injectOuterDisplay();

    EXPECT_TRUE(innerDisplay->isPoweredOn());
    EXPECT_FALSE(outerDisplay->isPoweredOn());

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId120));

    mFlinger.setPhysicalDisplayPowerMode(outerDisplay, hal::PowerMode::OFF);
    mFlinger.setPhysicalDisplayPowerMode(innerDisplay, hal::PowerMode::ON);

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId120));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(innerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId90, 120_Hz)));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(outerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId60, 120_Hz)));

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));

    const VsyncPeriodChangeTimeline timeline{.refreshRequired = true};
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId90);
    EXPECT_SET_ACTIVE_CONFIG(kOuterDisplayHwcId, kModeId60);

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId60));

    mFlinger.setPhysicalDisplayPowerMode(innerDisplay, hal::PowerMode::OFF);
    mFlinger.setPhysicalDisplayPowerMode(outerDisplay, hal::PowerMode::ON);

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId60));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(innerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId60, 120_Hz)));

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId60);

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId60));

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId60));
}

TEST_F(DisplayModeSwitchingTest, innerAndOuterDisplay) {
    const auto [innerDisplay, outerDisplay] = injectOuterDisplay();

    EXPECT_TRUE(innerDisplay->isPoweredOn());
    EXPECT_FALSE(outerDisplay->isPoweredOn());

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId120));

    mFlinger.setPhysicalDisplayPowerMode(innerDisplay, hal::PowerMode::ON);
    mFlinger.setPhysicalDisplayPowerMode(outerDisplay, hal::PowerMode::ON);

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId120));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(innerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId90, 120_Hz)));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(outerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId60, 120_Hz)));

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));

    const VsyncPeriodChangeTimeline timeline{.refreshRequired = true};
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId90);
    EXPECT_SET_ACTIVE_CONFIG(kOuterDisplayHwcId, kModeId60);

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId60));
}

TEST_F(DisplayModeSwitchingTest, powerOffDuringModeSet) {
    EXPECT_TRUE(mDisplay->isPoweredOn());
    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId60));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId90, 120_Hz)));

    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId90));

    // Power off the display before the mode has been set.
    mFlinger.setPhysicalDisplayPowerMode(mDisplay, hal::PowerMode::OFF);

    const VsyncPeriodChangeTimeline timeline{.refreshRequired = true};
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId90);

    mFlinger.commit();

    // Powering off should not abort the mode set.
    EXPECT_FALSE(mDisplay->isPoweredOn());
    EXPECT_THAT(mDisplay, ModeSwitchingTo(&mFlinger, kModeId90));

    mFlinger.commit();

    EXPECT_THAT(mDisplay, ModeSettledTo(&dmc(), kModeId90));
}

TEST_F(DisplayModeSwitchingTest, powerOffDuringConcurrentModeSet) {
    const auto [innerDisplay, outerDisplay] = injectOuterDisplay();

    EXPECT_TRUE(innerDisplay->isPoweredOn());
    EXPECT_FALSE(outerDisplay->isPoweredOn());

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId120));

    mFlinger.setPhysicalDisplayPowerMode(innerDisplay, hal::PowerMode::ON);
    mFlinger.setPhysicalDisplayPowerMode(outerDisplay, hal::PowerMode::ON);

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId120));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(innerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId90, 120_Hz)));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(outerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId60, 120_Hz)));

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));

    // Power off the outer display before the mode has been set.
    mFlinger.setPhysicalDisplayPowerMode(outerDisplay, hal::PowerMode::OFF);

    const VsyncPeriodChangeTimeline timeline{.refreshRequired = true};
    EXPECT_SET_ACTIVE_CONFIG(kInnerDisplayHwcId, kModeId90);
    EXPECT_SET_ACTIVE_CONFIG(kOuterDisplayHwcId, kModeId60);

    mFlinger.commit();

    // Powering off the inactive display should not abort the mode set.
    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId60));

    mFlinger.setPhysicalDisplayPowerMode(innerDisplay, hal::PowerMode::OFF);
    mFlinger.setPhysicalDisplayPowerMode(outerDisplay, hal::PowerMode::ON);

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(outerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId120,
                                                                               120_Hz)));

    EXPECT_SET_ACTIVE_CONFIG(kOuterDisplayHwcId, kModeId120);

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId120));

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(&dmc(), kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(&dmc(), kModeId120));
}

} // namespace
} // namespace android
