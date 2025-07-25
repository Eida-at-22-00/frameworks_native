/*
 * Copyright 2022 The Android Open Source Project
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

#include <gmock/gmock.h>

#include "TestableSurfaceFlinger.h"
#include "mock/PowerAdvisor/MockPowerAdvisor.h"
#include "mock/system/window/MockNativeWindow.h"

namespace android {

static constexpr uint8_t kDefaultPort = 255u;

using FakeDisplayDeviceInjector = TestableSurfaceFlinger::FakeDisplayDeviceInjector;
using android::adpf::mock::PowerAdvisor;
using android::hardware::graphics::composer::hal::HWDisplayId;

struct FakeDisplayInjectorArgs {
    PhysicalDisplayId displayId = PhysicalDisplayId::fromPort(kDefaultPort);
    uint8_t port = kDefaultPort;
    HWDisplayId hwcDisplayId = 0;
    bool isPrimary = true;
};

class FakeDisplayInjector {
public:
    FakeDisplayInjector(TestableSurfaceFlinger& flinger, PowerAdvisor& powerAdvisor,
                        sp<mock::NativeWindow> nativeWindow)
          : mFlinger(flinger), mPowerAdvisor(powerAdvisor), mNativeWindow(nativeWindow) {}

    sp<DisplayDevice> injectInternalDisplay(
            const std::function<void(FakeDisplayDeviceInjector&)>& injectExtra,
            FakeDisplayInjectorArgs args = {}) {
        using testing::_;
        using testing::AnyNumber;
        using testing::DoAll;
        using testing::Mock;
        using testing::Return;
        using testing::SetArgPointee;

        constexpr ui::Size kResolution = {1080, 1920};

        // The DisplayDevice is required to have a framebuffer (behind the
        // ANativeWindow interface) which uses the actual hardware display
        // size.
        EXPECT_CALL(*mNativeWindow, query(NATIVE_WINDOW_WIDTH, _))
                .WillRepeatedly(DoAll(SetArgPointee<1>(kResolution.getWidth()), Return(0)));
        EXPECT_CALL(*mNativeWindow, query(NATIVE_WINDOW_HEIGHT, _))
                .WillRepeatedly(DoAll(SetArgPointee<1>(kResolution.getHeight()), Return(0)));
        EXPECT_CALL(*mNativeWindow, perform(NATIVE_WINDOW_SET_BUFFERS_FORMAT));
        EXPECT_CALL(*mNativeWindow, perform(NATIVE_WINDOW_API_CONNECT));
        EXPECT_CALL(*mNativeWindow, perform(NATIVE_WINDOW_SET_USAGE64));
        EXPECT_CALL(*mNativeWindow, perform(NATIVE_WINDOW_API_DISCONNECT)).Times(AnyNumber());

        auto compositionDisplay = compositionengine::impl::
                createDisplay(mFlinger.getCompositionEngine(),
                              compositionengine::DisplayCreationArgsBuilder()
                                      .setId(args.displayId)
                                      .setPixels(kResolution)
                                      .setPowerAdvisor(&mPowerAdvisor)
                                      .build());

        auto injector = FakeDisplayDeviceInjector(mFlinger, compositionDisplay,
                                                  ui::DisplayConnectionType::Internal, args.port,
                                                  args.hwcDisplayId, args.isPrimary);

        injector.setNativeWindow(mNativeWindow);
        if (injectExtra) {
            injectExtra(injector);
        }

        auto displayDevice = injector.inject();

        Mock::VerifyAndClear(mNativeWindow.get());

        return displayDevice;
    }

    TestableSurfaceFlinger& mFlinger;
    PowerAdvisor& mPowerAdvisor;
    sp<mock::NativeWindow> mNativeWindow;
};

} // namespace android
