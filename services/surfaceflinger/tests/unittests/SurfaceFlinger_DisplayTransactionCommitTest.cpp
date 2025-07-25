/*
 * Copyright 2020 The Android Open Source Project
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

namespace android {
namespace {

struct DisplayTransactionCommitTest : DisplayTransactionTest {
    template <typename Case>
    void setupCommonPreconditions();

    template <typename Case, bool connected>
    static void expectHotplugReceived(mock::EventThread*);

    template <typename Case>
    void setupCommonCallExpectationsForConnectProcessing();

    template <typename Case>
    void setupCommonCallExpectationsForDisconnectProcessing();

    template <typename Case>
    void processesHotplugConnectCommon();

    template <typename Case>
    void ignoresHotplugConnectCommon();

    template <typename Case>
    void processesHotplugDisconnectCommon();

    template <typename Case>
    void verifyDisplayIsConnected(const sp<IBinder>& displayToken);

    template <typename Case>
    void verifyPhysicalDisplayIsConnected();

    void verifyDisplayIsNotConnected(const sp<IBinder>& displayToken);
};

template <typename Case>
void DisplayTransactionCommitTest::setupCommonPreconditions() {
    // Wide color displays support is configured appropriately
    Case::WideColorSupport::injectConfigChange(this);

    // SurfaceFlinger will use a test-controlled factory for BufferQueues
    injectFakeBufferQueueFactory();

    // SurfaceFlinger will use a test-controlled factory for native window
    // surfaces.
    injectFakeNativeWindowSurfaceFactory();
}

template <typename Case, bool connected>
void DisplayTransactionCommitTest::expectHotplugReceived(mock::EventThread* eventThread) {
    const auto physicalDisplayId = asPhysicalDisplayId(Case::Display::DISPLAY_ID::get());
    ASSERT_TRUE(physicalDisplayId);
    EXPECT_CALL(*eventThread, onHotplugReceived(*physicalDisplayId, connected)).Times(1);
}

template <typename Case>
void DisplayTransactionCommitTest::setupCommonCallExpectationsForConnectProcessing() {
    Case::Display::setupHwcHotplugCallExpectations(this);

    Case::Display::setupFramebufferConsumerBufferQueueCallExpectations(this);
    Case::Display::setupFramebufferProducerBufferQueueCallExpectations(this);
    Case::Display::setupNativeWindowSurfaceCreationCallExpectations(this);
    Case::Display::setupHwcGetActiveConfigCallExpectations(this);

    Case::WideColorSupport::setupComposerCallExpectations(this);
    Case::HdrSupport::setupComposerCallExpectations(this);
    Case::PerFrameMetadataSupport::setupComposerCallExpectations(this);

    expectHotplugReceived<Case, true>(mEventThread);
    expectHotplugReceived<Case, true>(mSFEventThread);
}

template <typename Case>
void DisplayTransactionCommitTest::setupCommonCallExpectationsForDisconnectProcessing() {
    expectHotplugReceived<Case, false>(mEventThread);
    expectHotplugReceived<Case, false>(mSFEventThread);
}

template <typename Case>
void DisplayTransactionCommitTest::verifyDisplayIsConnected(const sp<IBinder>& displayToken) {
    // The display device should have been set up in the list of displays.
    ASSERT_TRUE(hasDisplayDevice(displayToken));
    const auto& display = getDisplayDevice(displayToken);

    EXPECT_EQ(static_cast<bool>(Case::Display::SECURE), display.isSecure());
    EXPECT_EQ(static_cast<bool>(Case::Display::PRIMARY), display.isPrimary());

    std::optional<DisplayDeviceState::Physical> expectedPhysical;
    if (Case::Display::CONNECTION_TYPE::value) {
        const auto displayId = asPhysicalDisplayId(Case::Display::DISPLAY_ID::get());
        ASSERT_TRUE(displayId);
        const auto hwcDisplayId = Case::Display::HWC_DISPLAY_ID_OPT::value;
        ASSERT_TRUE(hwcDisplayId);
        expectedPhysical = {.id = *displayId, .hwcDisplayId = *hwcDisplayId};
    }

    // The display should have been set up in the current display state
    ASSERT_TRUE(hasCurrentDisplayState(displayToken));
    const auto& current = getCurrentDisplayState(displayToken);
    EXPECT_EQ(static_cast<bool>(Case::Display::VIRTUAL), current.isVirtual());
    EXPECT_EQ(expectedPhysical, current.physical);

    // The display should have been set up in the drawing display state
    ASSERT_TRUE(hasDrawingDisplayState(displayToken));
    const auto& draw = getDrawingDisplayState(displayToken);
    EXPECT_EQ(static_cast<bool>(Case::Display::VIRTUAL), draw.isVirtual());
    EXPECT_EQ(expectedPhysical, draw.physical);
}

template <typename Case>
void DisplayTransactionCommitTest::verifyPhysicalDisplayIsConnected() {
    // HWComposer should have an entry for the display
    EXPECT_TRUE(hasPhysicalHwcDisplay(Case::Display::HWC_DISPLAY_ID));

    // SF should have a display token.
    const auto displayIdOpt = asPhysicalDisplayId(Case::Display::DISPLAY_ID::get());
    ASSERT_TRUE(displayIdOpt);

    const auto displayOpt = mFlinger.mutablePhysicalDisplays().get(*displayIdOpt);
    ASSERT_TRUE(displayOpt);

    const auto& display = displayOpt->get();
    EXPECT_EQ(Case::Display::CONNECTION_TYPE::value, display.snapshot().connectionType());

    verifyDisplayIsConnected<Case>(display.token());
}

void DisplayTransactionCommitTest::verifyDisplayIsNotConnected(const sp<IBinder>& displayToken) {
    EXPECT_FALSE(hasDisplayDevice(displayToken));
    EXPECT_FALSE(hasCurrentDisplayState(displayToken));
    EXPECT_FALSE(hasDrawingDisplayState(displayToken));
}

template <typename Case>
void DisplayTransactionCommitTest::processesHotplugConnectCommon() {
    // --------------------------------------------------------------------
    // Preconditions

    setupCommonPreconditions<Case>();

    // A hotplug connect event is enqueued for a display
    Case::Display::injectPendingHotplugEvent(this, HWComposer::HotplugEvent::Connected);

    // --------------------------------------------------------------------
    // Call Expectations

    setupCommonCallExpectationsForConnectProcessing<Case>();

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.configureAndCommit();

    // --------------------------------------------------------------------
    // Postconditions

    verifyPhysicalDisplayIsConnected<Case>();

    // --------------------------------------------------------------------
    // Cleanup conditions

    EXPECT_CALL(*mComposer,
                setVsyncEnabled(Case::Display::HWC_DISPLAY_ID, IComposerClient::Vsync::DISABLE))
            .WillOnce(Return(Error::NONE));
    EXPECT_CALL(*mConsumer, consumerDisconnect()).WillOnce(Return(NO_ERROR));
}

template <typename Case>
void DisplayTransactionCommitTest::ignoresHotplugConnectCommon() {
    // --------------------------------------------------------------------
    // Preconditions

    setupCommonPreconditions<Case>();

    // A hotplug connect event is enqueued for a display
    Case::Display::injectPendingHotplugEvent(this, HWComposer::HotplugEvent::Connected);

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.configureAndCommit();

    // --------------------------------------------------------------------
    // Postconditions

    // HWComposer should not have an entry for the display
    EXPECT_FALSE(hasPhysicalHwcDisplay(Case::Display::HWC_DISPLAY_ID));
}

template <typename Case>
void DisplayTransactionCommitTest::processesHotplugDisconnectCommon() {
    // --------------------------------------------------------------------
    // Preconditions

    setupCommonPreconditions<Case>();

    // A hotplug disconnect event is enqueued for a display
    Case::Display::injectPendingHotplugEvent(this, HWComposer::HotplugEvent::Disconnected);

    // The display is already completely set up.
    Case::Display::injectHwcDisplay(this);
    auto existing = Case::Display::makeFakeExistingDisplayInjector(this);
    existing.inject();

    // --------------------------------------------------------------------
    // Call Expectations

    EXPECT_CALL(*mComposer, getDisplayIdentificationData(Case::Display::HWC_DISPLAY_ID, _, _))
            .Times(0);

    setupCommonCallExpectationsForDisconnectProcessing<Case>();

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.configureAndCommit();

    // --------------------------------------------------------------------
    // Postconditions

    // HWComposer should not have an entry for the display
    EXPECT_FALSE(hasPhysicalHwcDisplay(Case::Display::HWC_DISPLAY_ID));

    // SF should not have a PhysicalDisplay.
    const auto physicalDisplayIdOpt = asPhysicalDisplayId(Case::Display::DISPLAY_ID::get());
    ASSERT_TRUE(physicalDisplayIdOpt);
    ASSERT_FALSE(mFlinger.mutablePhysicalDisplays().contains(*physicalDisplayIdOpt));

    // The existing token should have been removed.
    verifyDisplayIsNotConnected(existing.token());
}

TEST_F(DisplayTransactionCommitTest, processesHotplugConnectPrimaryDisplay) {
    processesHotplugConnectCommon<SimplePrimaryDisplayCase>();
}

TEST_F(DisplayTransactionCommitTest, processesHotplugConnectExternalDisplay) {
    // Inject a primary display.
    PrimaryDisplayVariant::injectHwcDisplay(this);

    processesHotplugConnectCommon<SimpleExternalDisplayCase>();
}

TEST_F(DisplayTransactionCommitTest, processesHotplugConnectNonSecureExternalDisplay) {
    // Inject a primary display.
    PrimaryDisplayVariant::injectHwcDisplay(this);

    processesHotplugConnectCommon<SimpleExternalDisplayNonSecureCase>();
}

TEST_F(DisplayTransactionCommitTest, ignoresHotplugConnectIfPrimaryAndExternalAlreadyConnected) {
    // Inject both a primary and external display.
    PrimaryDisplayVariant::injectHwcDisplay(this);
    ExternalDisplayVariant::injectHwcDisplay(this);

    // TODO: This is an unnecessary call.
    EXPECT_CALL(*mComposer,
                getDisplayIdentificationData(TertiaryDisplayVariant::HWC_DISPLAY_ID, _, _))
            .WillOnce(DoAll(SetArgPointee<1>(TertiaryDisplay<kSecure>::PORT),
                            SetArgPointee<2>(TertiaryDisplay<kSecure>::GET_IDENTIFICATION_DATA()),
                            Return(Error::NONE)));

    ignoresHotplugConnectCommon<SimpleTertiaryDisplayCase>();
}

TEST_F(DisplayTransactionCommitTest,
       ignoresHotplugConnectNonSecureIfPrimaryAndExternalAlreadyConnected) {
    // Inject both a primary and external display.
    PrimaryDisplayVariant::injectHwcDisplay(this);
    ExternalDisplayVariant::injectHwcDisplay(this);

    // TODO: This is an unnecessary call.
    EXPECT_CALL(*mComposer,
                getDisplayIdentificationData(TertiaryDisplayVariant::HWC_DISPLAY_ID, _, _))
            .WillOnce(DoAll(SetArgPointee<1>(TertiaryDisplay<kSecure>::PORT),
                            SetArgPointee<2>(TertiaryDisplay<kSecure>::GET_IDENTIFICATION_DATA()),
                            Return(Error::NONE)));

    ignoresHotplugConnectCommon<SimpleTertiaryDisplayNonSecureCase>();
}

TEST_F(DisplayTransactionCommitTest, processesHotplugDisconnectPrimaryDisplay) {
    EXPECT_EXIT(processesHotplugDisconnectCommon<SimplePrimaryDisplayCase>(),
                testing::KilledBySignal(SIGABRT), "Primary display cannot be disconnected.");
}

TEST_F(DisplayTransactionCommitTest, processesHotplugDisconnectExternalDisplay) {
    processesHotplugDisconnectCommon<SimpleExternalDisplayCase>();
}

TEST_F(DisplayTransactionCommitTest, processesHotplugDisconnectNonSecureExternalDisplay) {
    processesHotplugDisconnectCommon<SimpleExternalDisplayNonSecureCase>();
}

TEST_F(DisplayTransactionCommitTest, processesHotplugConnectThenDisconnectPrimary) {
    EXPECT_EXIT(
            [this] {
                using Case = SimplePrimaryDisplayCase;

                // --------------------------------------------------------------------
                // Preconditions

                setupCommonPreconditions<Case>();

                // A hotplug connect event is enqueued for a display
                Case::Display::injectPendingHotplugEvent(this, HWComposer::HotplugEvent::Connected);
                // A hotplug disconnect event is also enqueued for the same display
                Case::Display::injectPendingHotplugEvent(this,
                                                         HWComposer::HotplugEvent::Disconnected);

                // --------------------------------------------------------------------
                // Call Expectations

                setupCommonCallExpectationsForConnectProcessing<Case>();
                setupCommonCallExpectationsForDisconnectProcessing<Case>();

                EXPECT_CALL(*mComposer,
                            setVsyncEnabled(Case::Display::HWC_DISPLAY_ID,
                                            IComposerClient::Vsync::DISABLE))
                        .WillOnce(Return(Error::NONE));
                EXPECT_CALL(*mConsumer, consumerDisconnect()).WillOnce(Return(NO_ERROR));

                // --------------------------------------------------------------------
                // Invocation

                mFlinger.configureAndCommit();

                // --------------------------------------------------------------------
                // Postconditions

                // HWComposer should not have an entry for the display
                EXPECT_FALSE(hasPhysicalHwcDisplay(Case::Display::HWC_DISPLAY_ID));

                // SF should not have a PhysicalDisplay.
                const auto physicalDisplayIdOpt =
                        asPhysicalDisplayId(Case::Display::DISPLAY_ID::get());
                ASSERT_TRUE(physicalDisplayIdOpt);
                ASSERT_FALSE(mFlinger.mutablePhysicalDisplays().contains(*physicalDisplayIdOpt));
            }(),
            testing::KilledBySignal(SIGABRT), "Primary display cannot be disconnected.");
}

TEST_F(DisplayTransactionCommitTest, processesHotplugDisconnectThenConnectPrimary) {
    EXPECT_EXIT(
            [this] {
                using Case = SimplePrimaryDisplayCase;

                // --------------------------------------------------------------------
                // Preconditions

                setupCommonPreconditions<Case>();

                // The display is already completely set up.
                Case::Display::injectHwcDisplay(this);
                auto existing = Case::Display::makeFakeExistingDisplayInjector(this);
                existing.inject();

                // A hotplug disconnect event is enqueued for a display
                Case::Display::injectPendingHotplugEvent(this,
                                                         HWComposer::HotplugEvent::Disconnected);
                // A hotplug connect event is also enqueued for the same display
                Case::Display::injectPendingHotplugEvent(this, HWComposer::HotplugEvent::Connected);

                // --------------------------------------------------------------------
                // Call Expectations

                setupCommonCallExpectationsForConnectProcessing<Case>();
                setupCommonCallExpectationsForDisconnectProcessing<Case>();

                // --------------------------------------------------------------------
                // Invocation

                mFlinger.configureAndCommit();

                // --------------------------------------------------------------------
                // Postconditions

                // The existing token should have been removed.
                verifyDisplayIsNotConnected(existing.token());
                const auto physicalDisplayIdOpt =
                        asPhysicalDisplayId(Case::Display::DISPLAY_ID::get());
                ASSERT_TRUE(physicalDisplayIdOpt);

                const auto displayOpt =
                        mFlinger.mutablePhysicalDisplays().get(*physicalDisplayIdOpt);
                ASSERT_TRUE(displayOpt);
                EXPECT_NE(existing.token(), displayOpt->get().token());

                // A new display should be connected in its place.
                verifyPhysicalDisplayIsConnected<Case>();

                // --------------------------------------------------------------------
                // Cleanup conditions

                EXPECT_CALL(*mComposer,
                            setVsyncEnabled(Case::Display::HWC_DISPLAY_ID,
                                            IComposerClient::Vsync::DISABLE))
                        .WillOnce(Return(Error::NONE));
                EXPECT_CALL(*mConsumer, consumerDisconnect()).WillOnce(Return(NO_ERROR));
            }(),
            testing::KilledBySignal(SIGABRT), "Primary display cannot be disconnected.");
}

TEST_F(DisplayTransactionCommitTest, processesVirtualDisplayAdded) {
    using Case = HwcVirtualDisplayCase;

    // --------------------------------------------------------------------
    // Preconditions

    // The HWC supports at least one virtual display
    injectMockComposer(1);

    setupCommonPreconditions<Case>();

    // A virtual display was added to the current state, and it has a
    // surface(producer)
    sp<BBinder> displayToken = sp<BBinder>::make();

    DisplayDeviceState state;
    state.isSecure = static_cast<bool>(Case::Display::SECURE);

    sp<mock::GraphicBufferProducer> surface{sp<mock::GraphicBufferProducer>::make()};
    state.surface = surface;
    mFlinger.mutableCurrentState().displays.add(displayToken, state);

    // --------------------------------------------------------------------
    // Call Expectations

    Case::Display::setupFramebufferConsumerBufferQueueCallExpectations(this);
    Case::Display::setupNativeWindowSurfaceCreationCallExpectations(this);

    EXPECT_CALL(*surface, query(NATIVE_WINDOW_WIDTH, _))
            .WillRepeatedly(DoAll(SetArgPointee<1>(Case::Display::WIDTH), Return(NO_ERROR)));
    EXPECT_CALL(*surface, query(NATIVE_WINDOW_HEIGHT, _))
            .WillRepeatedly(DoAll(SetArgPointee<1>(Case::Display::HEIGHT), Return(NO_ERROR)));
    EXPECT_CALL(*surface, query(NATIVE_WINDOW_FORMAT, _))
            .WillRepeatedly(DoAll(SetArgPointee<1>(DEFAULT_VIRTUAL_DISPLAY_SURFACE_FORMAT),
                                  Return(NO_ERROR)));
    EXPECT_CALL(*surface, query(NATIVE_WINDOW_CONSUMER_USAGE_BITS, _))
            .WillRepeatedly(DoAll(SetArgPointee<1>(0), Return(NO_ERROR)));

    EXPECT_CALL(*surface, setAsyncMode(true)).Times(1);

    EXPECT_CALL(*mProducer, connect(_, NATIVE_WINDOW_API_EGL, false, _)).Times(1);
    EXPECT_CALL(*mProducer, disconnect(_, _)).Times(1);

    Case::Display::setupHwcVirtualDisplayCreationCallExpectations(this);
    Case::WideColorSupport::setupComposerCallExpectations(this);
    Case::HdrSupport::setupComposerCallExpectations(this);
    Case::PerFrameMetadataSupport::setupComposerCallExpectations(this);

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);

    // --------------------------------------------------------------------
    // Postconditions

    // The display device should have been set up in the list of displays.
    verifyDisplayIsConnected<Case>(displayToken);

    // --------------------------------------------------------------------
    // Cleanup conditions

    EXPECT_CALL(*mComposer, destroyVirtualDisplay(Case::Display::HWC_DISPLAY_ID))
            .WillOnce(Return(Error::NONE));
    EXPECT_CALL(*mConsumer, consumerDisconnect()).WillOnce(Return(NO_ERROR));

    // Cleanup
    mFlinger.mutableCurrentState().displays.removeItem(displayToken);
    mFlinger.mutableDrawingState().displays.removeItem(displayToken);
}

TEST_F(DisplayTransactionCommitTest, processesVirtualDisplayAddedWithNoSurface) {
    using Case = HwcVirtualDisplayCase;

    // --------------------------------------------------------------------
    // Preconditions

    // The HWC supports at least one virtual display
    injectMockComposer(1);

    setupCommonPreconditions<Case>();

    // A virtual display was added to the current state, but it does not have a
    // surface.
    sp<BBinder> displayToken = sp<BBinder>::make();

    DisplayDeviceState state;
    state.isSecure = static_cast<bool>(Case::Display::SECURE);

    mFlinger.mutableCurrentState().displays.add(displayToken, state);

    // --------------------------------------------------------------------
    // Call Expectations

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);

    // --------------------------------------------------------------------
    // Postconditions

    // There will not be a display device set up.
    EXPECT_FALSE(hasDisplayDevice(displayToken));

    // The drawing display state will be set from the current display state.
    ASSERT_TRUE(hasDrawingDisplayState(displayToken));
    const auto& draw = getDrawingDisplayState(displayToken);
    EXPECT_EQ(static_cast<bool>(Case::Display::VIRTUAL), draw.isVirtual());
}

TEST_F(DisplayTransactionCommitTest, processesVirtualDisplayRemoval) {
    using Case = HwcVirtualDisplayCase;

    // --------------------------------------------------------------------
    // Preconditions

    // A virtual display is set up but is removed from the current state.
    const auto displayId = asHalDisplayId(Case::Display::DISPLAY_ID::get());
    ASSERT_TRUE(displayId);
    mFlinger.mutableHwcDisplayData().try_emplace(*displayId);
    Case::Display::injectHwcDisplay(this);
    auto existing = Case::Display::makeFakeExistingDisplayInjector(this);
    existing.inject();
    mFlinger.mutableCurrentState().displays.removeItem(existing.token());

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);

    // --------------------------------------------------------------------
    // Postconditions

    // The existing token should have been removed
    verifyDisplayIsNotConnected(existing.token());
}

TEST_F(DisplayTransactionCommitTest, processesDisplayLayerStackChanges) {
    using Case = NonHwcVirtualDisplayCase;

    constexpr ui::LayerStack oldLayerStack = ui::DEFAULT_LAYER_STACK;
    constexpr ui::LayerStack newLayerStack{123u};

    // --------------------------------------------------------------------
    // Preconditions

    // A display is set up
    auto display = Case::Display::makeFakeExistingDisplayInjector(this);
    display.inject();

    // There is a change to the layerStack state
    display.mutableDrawingDisplayState().layerStack = oldLayerStack;
    display.mutableCurrentDisplayState().layerStack = newLayerStack;

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);

    // --------------------------------------------------------------------
    // Postconditions

    EXPECT_EQ(newLayerStack, display.mutableDisplayDevice()->getLayerStack());
}

TEST_F(DisplayTransactionCommitTest, processesDisplayTransformChanges) {
    using Case = NonHwcVirtualDisplayCase;

    constexpr ui::Rotation oldTransform = ui::ROTATION_0;
    constexpr ui::Rotation newTransform = ui::ROTATION_180;

    // --------------------------------------------------------------------
    // Preconditions

    // A display is set up
    auto display = Case::Display::makeFakeExistingDisplayInjector(this);
    display.inject();

    // There is a change to the orientation state
    display.mutableDrawingDisplayState().orientation = oldTransform;
    display.mutableCurrentDisplayState().orientation = newTransform;

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);

    // --------------------------------------------------------------------
    // Postconditions

    EXPECT_EQ(newTransform, display.mutableDisplayDevice()->getOrientation());
}

TEST_F(DisplayTransactionCommitTest, processesDisplayLayerStackRectChanges) {
    using Case = NonHwcVirtualDisplayCase;

    const Rect oldLayerStackRect(0, 0, 0, 0);
    const Rect newLayerStackRect(0, 0, 123, 456);

    // --------------------------------------------------------------------
    // Preconditions

    // A display is set up
    auto display = Case::Display::makeFakeExistingDisplayInjector(this);
    display.inject();

    // There is a change to the layerStackSpaceRect state
    display.mutableDrawingDisplayState().layerStackSpaceRect = oldLayerStackRect;
    display.mutableCurrentDisplayState().layerStackSpaceRect = newLayerStackRect;

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);

    // --------------------------------------------------------------------
    // Postconditions

    EXPECT_EQ(newLayerStackRect, display.mutableDisplayDevice()->getLayerStackSpaceRect());
}

TEST_F(DisplayTransactionCommitTest, processesDisplayRectChanges) {
    using Case = NonHwcVirtualDisplayCase;

    const Rect oldDisplayRect(0, 0);
    const Rect newDisplayRect(123, 456);

    // --------------------------------------------------------------------
    // Preconditions

    // A display is set up
    auto display = Case::Display::makeFakeExistingDisplayInjector(this);
    display.inject();

    // There is a change to the layerStackSpaceRect state
    display.mutableDrawingDisplayState().orientedDisplaySpaceRect = oldDisplayRect;
    display.mutableCurrentDisplayState().orientedDisplaySpaceRect = newDisplayRect;

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);

    // --------------------------------------------------------------------
    // Postconditions

    EXPECT_EQ(newDisplayRect, display.mutableDisplayDevice()->getOrientedDisplaySpaceRect());
}

TEST_F(DisplayTransactionCommitTest, processesDisplayWidthChanges) {
    using Case = NonHwcVirtualDisplayCase;

    constexpr int oldWidth = 0;
    constexpr int oldHeight = 10;
    constexpr int newWidth = 123;

    // --------------------------------------------------------------------
    // Preconditions

    // A display is set up
    auto nativeWindow = sp<mock::NativeWindow>::make();
    auto displaySurface = sp<compositionengine::mock::DisplaySurface>::make();
    sp<GraphicBuffer> buf =

            sp<GraphicBuffer>::make();
    auto display = Case::Display::makeFakeExistingDisplayInjector(this);
    display.setNativeWindow(nativeWindow);
    display.setDisplaySurface(displaySurface);
    // Setup injection expectations
    EXPECT_CALL(*nativeWindow, query(NATIVE_WINDOW_WIDTH, _))
            .WillOnce(DoAll(SetArgPointee<1>(oldWidth), Return(0)));
    EXPECT_CALL(*nativeWindow, query(NATIVE_WINDOW_HEIGHT, _))
            .WillOnce(DoAll(SetArgPointee<1>(oldHeight), Return(0)));
    EXPECT_CALL(*nativeWindow, perform(NATIVE_WINDOW_SET_BUFFERS_FORMAT)).Times(1);
    EXPECT_CALL(*nativeWindow, perform(NATIVE_WINDOW_API_CONNECT)).Times(1);
    EXPECT_CALL(*nativeWindow, perform(NATIVE_WINDOW_SET_USAGE64)).Times(1);
    EXPECT_CALL(*nativeWindow, perform(NATIVE_WINDOW_API_DISCONNECT)).Times(1);
    display.inject();

    // There is a change to the layerStackSpaceRect state
    display.mutableDrawingDisplayState().width = oldWidth;
    display.mutableDrawingDisplayState().height = oldHeight;
    display.mutableCurrentDisplayState().width = newWidth;
    display.mutableCurrentDisplayState().height = oldHeight;

    // --------------------------------------------------------------------
    // Call Expectations

    EXPECT_CALL(*displaySurface, resizeBuffers(ui::Size(newWidth, oldHeight))).Times(1);

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);
}

TEST_F(DisplayTransactionCommitTest, processesDisplayHeightChanges) {
    using Case = NonHwcVirtualDisplayCase;

    constexpr int oldWidth = 0;
    constexpr int oldHeight = 10;
    constexpr int newHeight = 123;

    // --------------------------------------------------------------------
    // Preconditions

    // A display is set up
    auto nativeWindow = sp<mock::NativeWindow>::make();
    auto displaySurface = sp<compositionengine::mock::DisplaySurface>::make();
    sp<GraphicBuffer> buf = sp<GraphicBuffer>::make();
    auto display = Case::Display::makeFakeExistingDisplayInjector(this);
    display.setNativeWindow(nativeWindow);
    display.setDisplaySurface(displaySurface);
    // Setup injection expectations
    EXPECT_CALL(*nativeWindow, query(NATIVE_WINDOW_WIDTH, _))
            .WillOnce(DoAll(SetArgPointee<1>(oldWidth), Return(0)));
    EXPECT_CALL(*nativeWindow, query(NATIVE_WINDOW_HEIGHT, _))
            .WillOnce(DoAll(SetArgPointee<1>(oldHeight), Return(0)));
    EXPECT_CALL(*nativeWindow, perform(NATIVE_WINDOW_SET_BUFFERS_FORMAT)).Times(1);
    EXPECT_CALL(*nativeWindow, perform(NATIVE_WINDOW_API_CONNECT)).Times(1);
    EXPECT_CALL(*nativeWindow, perform(NATIVE_WINDOW_SET_USAGE64)).Times(1);
    EXPECT_CALL(*nativeWindow, perform(NATIVE_WINDOW_API_DISCONNECT)).Times(1);
    display.inject();

    // There is a change to the layerStackSpaceRect state
    display.mutableDrawingDisplayState().width = oldWidth;
    display.mutableDrawingDisplayState().height = oldHeight;
    display.mutableCurrentDisplayState().width = oldWidth;
    display.mutableCurrentDisplayState().height = newHeight;

    // --------------------------------------------------------------------
    // Call Expectations

    EXPECT_CALL(*displaySurface, resizeBuffers(ui::Size(oldWidth, newHeight))).Times(1);

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);
}

TEST_F(DisplayTransactionCommitTest, processesDisplaySizeDisplayRectAndLayerStackRectChanges) {
    using Case = NonHwcVirtualDisplayCase;

    constexpr uint32_t kOldWidth = 567;
    constexpr uint32_t kOldHeight = 456;
    const auto kOldSize = Rect(kOldWidth, kOldHeight);

    constexpr uint32_t kNewWidth = 234;
    constexpr uint32_t kNewHeight = 123;
    const auto kNewSize = Rect(kNewWidth, kNewHeight);

    // --------------------------------------------------------------------
    // Preconditions

    // A display is set up
    auto nativeWindow = sp<mock::NativeWindow>::make();
    auto displaySurface = sp<compositionengine::mock::DisplaySurface>::make();
    sp<GraphicBuffer> buf = sp<GraphicBuffer>::make();
    auto display = Case::Display::makeFakeExistingDisplayInjector(this);
    display.setNativeWindow(nativeWindow);
    display.setDisplaySurface(displaySurface);
    // Setup injection expectations
    EXPECT_CALL(*nativeWindow, query(NATIVE_WINDOW_WIDTH, _))
            .WillOnce(DoAll(SetArgPointee<1>(kOldWidth), Return(0)));
    EXPECT_CALL(*nativeWindow, query(NATIVE_WINDOW_HEIGHT, _))
            .WillOnce(DoAll(SetArgPointee<1>(kOldHeight), Return(0)));
    display.inject();

    // There is a change to the layerStackSpaceRect state
    display.mutableDrawingDisplayState().width = kOldWidth;
    display.mutableDrawingDisplayState().height = kOldHeight;
    display.mutableDrawingDisplayState().layerStackSpaceRect = kOldSize;
    display.mutableDrawingDisplayState().orientedDisplaySpaceRect = kOldSize;

    display.mutableCurrentDisplayState().width = kNewWidth;
    display.mutableCurrentDisplayState().height = kNewHeight;
    display.mutableCurrentDisplayState().layerStackSpaceRect = kNewSize;
    display.mutableCurrentDisplayState().orientedDisplaySpaceRect = kNewSize;

    // --------------------------------------------------------------------
    // Call Expectations

    EXPECT_CALL(*displaySurface, resizeBuffers(kNewSize.getSize())).Times(1);

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.commitTransactionsLocked(eDisplayTransactionNeeded);

    EXPECT_EQ(display.mutableDisplayDevice()->getBounds(), kNewSize);
    EXPECT_EQ(display.mutableDisplayDevice()->getWidth(), kNewWidth);
    EXPECT_EQ(display.mutableDisplayDevice()->getHeight(), kNewHeight);
    EXPECT_EQ(display.mutableDisplayDevice()->getOrientedDisplaySpaceRect(), kNewSize);
    EXPECT_EQ(display.mutableDisplayDevice()->getLayerStackSpaceRect(), kNewSize);
}

} // namespace
} // namespace android
