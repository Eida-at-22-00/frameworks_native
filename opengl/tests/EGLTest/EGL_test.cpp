/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <SurfaceFlingerProperties.h>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>

#include <configstore/Utils.h>
#include <utils/String8.h>

#include <EGL/egl.h>
#include <gui/Surface.h>
#include <gui/IConsumerListener.h>
#include <gui/IProducerListener.h>
#include <gui/IGraphicBufferConsumer.h>
#include <gui/BufferQueue.h>

#include "egl_display.h"

bool hasEglExtension(EGLDisplay dpy, const char* extensionName) {
    const char* exts = eglQueryString(dpy, EGL_EXTENSIONS);
    size_t cropExtLen = strlen(extensionName);
    size_t extsLen = strlen(exts);
    bool equal = !strcmp(extensionName, exts);
    android::String8 extString(extensionName);
    android::String8 space(" ");
    bool atStart = !strncmp(extString + space, exts, cropExtLen + 1);
    bool atEnd = (cropExtLen + 1) < extsLen &&
            !strcmp(space + extString, exts + extsLen - (cropExtLen + 1));
    bool inMiddle = strstr(exts, space + extString + space);
    return equal || atStart || atEnd || inMiddle;
}

namespace android {

#define EGL_UNSIGNED_TRUE static_cast<EGLBoolean>(EGL_TRUE)

// retrieve wide-color setting from configstore
using namespace android::hardware::configstore;
using namespace android::hardware::configstore::V1_0;

#define METADATA_SCALE(x) (static_cast<EGLint>(x * EGL_METADATA_SCALING_EXT))

static bool hasWideColorDisplay = android::sysprop::has_wide_color_display(false);

static bool hasHdrDisplay = android::sysprop::has_HDR_display(false);

class EGLTest : public ::testing::Test {
public:
    void get8BitConfig(EGLConfig& config);
    void setSurfaceSmpteMetadata(EGLSurface surface);
    void checkSurfaceSmpteMetadata(EGLSurface eglSurface);

protected:
    EGLDisplay mEglDisplay;

protected:
    EGLTest() :
            mEglDisplay(EGL_NO_DISPLAY) {
    }

    virtual void SetUp() {
        mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        ASSERT_NE(EGL_NO_DISPLAY, mEglDisplay);
        ASSERT_EQ(EGL_SUCCESS, eglGetError());

        EGLint majorVersion;
        EGLint minorVersion;
        EXPECT_TRUE(eglInitialize(mEglDisplay, &majorVersion, &minorVersion));
        ASSERT_EQ(EGL_SUCCESS, eglGetError());
        RecordProperty("EglVersionMajor", majorVersion);
        RecordProperty("EglVersionMajor", minorVersion);
    }

    virtual void TearDown() {
        EGLBoolean success = eglTerminate(mEglDisplay);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(EGL_SUCCESS, eglGetError());
    }
};

TEST_F(EGLTest, DISABLED_EGLConfigEightBitFirst) {

    EGLint numConfigs;
    EGLConfig config;
    EGLBoolean success;
    EGLint attrs[] = {
            EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
            EGL_NONE
    };

    success = eglChooseConfig(mEglDisplay, attrs, &config, 1, &numConfigs);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_GE(numConfigs, 1);

    EGLint components[3];

    success = eglGetConfigAttrib(mEglDisplay, config, EGL_RED_SIZE, &components[0]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_GREEN_SIZE, &components[1]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_BLUE_SIZE, &components[2]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());

    EXPECT_GE(components[0], 8);
    EXPECT_GE(components[1], 8);
    EXPECT_GE(components[2], 8);
}

TEST_F(EGLTest, EGLTerminateSucceedsWithRemainingObjects) {
    EGLint numConfigs;
    EGLConfig config;
    EGLint attrs[] = {
        EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,           8,
        EGL_GREEN_SIZE,         8,
        EGL_BLUE_SIZE,          8,
        EGL_ALPHA_SIZE,         8,
        EGL_NONE
    };
    EXPECT_TRUE(eglChooseConfig(mEglDisplay, attrs, &config, 1, &numConfigs));

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config,
                                mANW.get(), NULL);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface) ;

    // do not destroy eglSurface
    // eglTerminate is called in the tear down and should destroy it for us
}

TEST_F(EGLTest, EGLConfigRGBA8888First) {

    EGLint numConfigs;
    EGLConfig config;
    EGLBoolean success;
    EGLint attrs[] = {
            EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,           8,
            EGL_GREEN_SIZE,         8,
            EGL_BLUE_SIZE,          8,
            EGL_ALPHA_SIZE,         8,
            EGL_NONE
    };

    success = eglChooseConfig(mEglDisplay, attrs, &config, 1, &numConfigs);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_GE(numConfigs, 1);

    EGLint components[4];

    success = eglGetConfigAttrib(mEglDisplay, config, EGL_RED_SIZE, &components[0]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_GREEN_SIZE, &components[1]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_BLUE_SIZE, &components[2]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_ALPHA_SIZE, &components[3]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());

    EXPECT_GE(components[0], 8);
    EXPECT_GE(components[1], 8);
    EXPECT_GE(components[2], 8);
    EXPECT_GE(components[3], 8);
}

TEST_F(EGLTest, EGLDisplayP3) {
    EGLint numConfigs;
    EGLConfig config;
    EGLBoolean success;

    if (!hasWideColorDisplay) {
        // skip this test if device does not have wide-color display
        std::cerr << "[          ] Device does not support wide-color, test skipped" << std::endl;
        return;
    }

    // Test that display-p3 extensions exist
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_display_p3"));
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_display_p3_linear"));
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_display_p3_passthrough"));

    // Use 8-bit to keep forcus on Display-P3 aspect
    EGLint attrs[] = {
            // clang-format off
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,          EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RED_SIZE,                 8,
            EGL_GREEN_SIZE,               8,
            EGL_BLUE_SIZE,                8,
            EGL_ALPHA_SIZE,               8,
            EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FIXED_EXT,
            EGL_NONE,                     EGL_NONE
            // clang-format on
    };
    success = eglChooseConfig(mEglDisplay, attrs, &config, 1, &numConfigs);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(1, numConfigs);

    EGLint components[4];
    EGLint value;
    eglGetConfigAttrib(mEglDisplay, config, EGL_CONFIG_ID, &value);

    success = eglGetConfigAttrib(mEglDisplay, config, EGL_RED_SIZE, &components[0]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_GREEN_SIZE, &components[1]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_BLUE_SIZE, &components[2]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_ALPHA_SIZE, &components[3]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());

    EXPECT_EQ(components[0], 8);
    EXPECT_EQ(components[1], 8);
    EXPECT_EQ(components[2], 8);
    EXPECT_EQ(components[3], 8);

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;
    EGLint winAttrs[] = {
            // clang-format off
            EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_DISPLAY_P3_EXT,
            EGL_NONE,              EGL_NONE
            // clang-format on
    };

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    success = eglQuerySurface(mEglDisplay, eglSurface, EGL_GL_COLORSPACE_KHR, &value);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_GL_COLORSPACE_DISPLAY_P3_EXT, value);

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));
}

TEST_F(EGLTest, EGLDisplayP3Passthrough) {
    EGLConfig config;
    EGLBoolean success;

    if (!hasWideColorDisplay) {
        // skip this test if device does not have wide-color display
        std::cerr << "[          ] Device does not support wide-color, test skipped" << std::endl;
        return;
    }

    // Test that display-p3 extensions exist
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_display_p3"));
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_display_p3_linear"));
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_display_p3_passthrough"));

    get8BitConfig(config);

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;
    EGLint winAttrs[] = {
            // clang-format off
            EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_DISPLAY_P3_PASSTHROUGH_EXT,
            EGL_NONE,              EGL_NONE
            // clang-format on
    };

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    android_dataspace dataspace =
        static_cast<android_dataspace>(ANativeWindow_getBuffersDataSpace(mANW.get()));
    ASSERT_EQ(dataspace, HAL_DATASPACE_DISPLAY_P3);

    EGLint value;
    success = eglQuerySurface(mEglDisplay, eglSurface, EGL_GL_COLORSPACE_KHR, &value);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_GL_COLORSPACE_DISPLAY_P3_PASSTHROUGH_EXT, value);

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));
}

TEST_F(EGLTest, EGLDisplayP31010102) {
    // This test has been failing since:
    // libEGL: When driver doesn't understand P3, map sRGB-encoded P3 to sRGB
    // https://android-review.git.corp.google.com/c/platform/frameworks/native/+/793504
    GTEST_SKIP() << "Skipping broken test. See b/120714942 and b/117104367";

    EGLint numConfigs;
    EGLConfig config;
    EGLBoolean success;

    if (!hasWideColorDisplay) {
        // skip this test if device does not have wide-color display
        std::cerr << "[          ] Device does not support wide-color, test skipped" << std::endl;
        return;
    }

    // Test that display-p3 extensions exist
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_display_p3"));
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_display_p3_linear"));
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_display_p3_passthrough"));

    // Use 8-bit to keep forcus on Display-P3 aspect
    EGLint attrs[] = {
            // clang-format off
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,          EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RED_SIZE,                 10,
            EGL_GREEN_SIZE,               10,
            EGL_BLUE_SIZE,                10,
            EGL_ALPHA_SIZE,               2,
            EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FIXED_EXT,
            EGL_NONE,                     EGL_NONE
            // clang-format on
    };
    success = eglChooseConfig(mEglDisplay, attrs, &config, 1, &numConfigs);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(1, numConfigs);

    EGLint components[4];
    EGLint value;
    eglGetConfigAttrib(mEglDisplay, config, EGL_CONFIG_ID, &value);

    success = eglGetConfigAttrib(mEglDisplay, config, EGL_RED_SIZE, &components[0]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_GREEN_SIZE, &components[1]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_BLUE_SIZE, &components[2]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_ALPHA_SIZE, &components[3]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());

    EXPECT_EQ(components[0], 10);
    EXPECT_EQ(components[1], 10);
    EXPECT_EQ(components[2], 10);
    EXPECT_EQ(components[3], 2);

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;
    EGLint winAttrs[] = {
            // clang-format off
            EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_DISPLAY_P3_EXT,
            EGL_NONE,              EGL_NONE
            // clang-format on
    };

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    success = eglQuerySurface(mEglDisplay, eglSurface, EGL_GL_COLORSPACE_KHR, &value);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_GL_COLORSPACE_DISPLAY_P3_EXT, value);

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));
}

void EGLTest::get8BitConfig(EGLConfig& config) {
    EGLint numConfigs;
    EGLBoolean success;

    // Use 8-bit to keep focus on colorspace aspect
    const EGLint attrs[] = {
            // clang-format off
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,          EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RED_SIZE,                 8,
            EGL_GREEN_SIZE,               8,
            EGL_BLUE_SIZE,                8,
            EGL_ALPHA_SIZE,               8,
            EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FIXED_EXT,
            EGL_NONE,
            // clang-format on
    };
    success = eglChooseConfig(mEglDisplay, attrs, &config, 1, &numConfigs);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(1, numConfigs);

    EGLint components[4];
    EGLint value;
    eglGetConfigAttrib(mEglDisplay, config, EGL_CONFIG_ID, &value);

    success = eglGetConfigAttrib(mEglDisplay, config, EGL_RED_SIZE, &components[0]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_GREEN_SIZE, &components[1]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_BLUE_SIZE, &components[2]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_ALPHA_SIZE, &components[3]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());

    // Verify component sizes on config match what was asked for.
    EXPECT_EQ(components[0], 8);
    EXPECT_EQ(components[1], 8);
    EXPECT_EQ(components[2], 8);
    EXPECT_EQ(components[3], 8);
}

void EGLTest::setSurfaceSmpteMetadata(EGLSurface surface) {
    if (hasEglExtension(mEglDisplay, "EGL_EXT_surface_SMPTE2086_metadata")) {
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT,
                         METADATA_SCALE(0.640));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT,
                         METADATA_SCALE(0.330));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT,
                         METADATA_SCALE(0.290));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT,
                         METADATA_SCALE(0.600));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT,
                         METADATA_SCALE(0.150));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT,
                         METADATA_SCALE(0.060));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_WHITE_POINT_X_EXT,
                         METADATA_SCALE(0.3127));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_WHITE_POINT_Y_EXT,
                         METADATA_SCALE(0.3290));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_MAX_LUMINANCE_EXT,
                         METADATA_SCALE(300));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_SMPTE2086_MIN_LUMINANCE_EXT,
                         METADATA_SCALE(0.7));
    }

    if (hasEglExtension(mEglDisplay, "EGL_EXT_surface_CTA861_3_metadata")) {
        eglSurfaceAttrib(mEglDisplay, surface, EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT,
                         METADATA_SCALE(300));
        eglSurfaceAttrib(mEglDisplay, surface, EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT,
                         METADATA_SCALE(75));
    }
}

void EGLTest::checkSurfaceSmpteMetadata(EGLSurface eglSurface) {
    EGLBoolean success;
    EGLint value;

    if (hasEglExtension(mEglDisplay, "EGL_EXT_surface_SMPTE2086_metadata")) {
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(0.640), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(0.330), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(0.290), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(0.600), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(0.150), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(0.060), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_WHITE_POINT_X_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(0.3127), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_WHITE_POINT_Y_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(0.3290), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_MAX_LUMINANCE_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(300.0), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_SMPTE2086_MIN_LUMINANCE_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(0.7), value);
    }

    if (hasEglExtension(mEglDisplay, "EGL_EXT_surface_CTA861_3_metadata")) {
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(300.0), value);
        success = eglQuerySurface(mEglDisplay, eglSurface, EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT, &value);
        ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
        ASSERT_EQ(METADATA_SCALE(75.0), value);
    }
}

TEST_F(EGLTest, EGLBT2020Linear) {
    EGLConfig config;
    EGLBoolean success;

    if (!hasHdrDisplay) {
        // skip this test if device does not have HDR display
        RecordProperty("hasHdrDisplay", false);
        return;
    }

    // Test that bt2020 linear extension exists
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_bt2020_linear"))
            << "EGL_EXT_gl_colorspace_bt2020_linear extension not available";

    ASSERT_NO_FATAL_FAILURE(get8BitConfig(config));

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;

    std::vector<EGLint> winAttrs;
    winAttrs.push_back(EGL_GL_COLORSPACE_KHR);
    winAttrs.push_back(EGL_GL_COLORSPACE_BT2020_PQ_EXT);

    winAttrs.push_back(EGL_NONE);

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs.data());
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    EGLint value;
    success = eglQuerySurface(mEglDisplay, eglSurface, EGL_GL_COLORSPACE_KHR, &value);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_GL_COLORSPACE_BT2020_PQ_EXT, value);

    ASSERT_NO_FATAL_FAILURE(setSurfaceSmpteMetadata(eglSurface));

    ASSERT_NO_FATAL_FAILURE(checkSurfaceSmpteMetadata(eglSurface));

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));
}

TEST_F(EGLTest, EGLBT2020PQ) {
    EGLConfig config;
    EGLBoolean success;

    if (!hasHdrDisplay) {
        // skip this test if device does not have HDR display
        RecordProperty("hasHdrDisplay", false);
        return;
    }

    // Test that bt2020-pq extension exists
    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_gl_colorspace_bt2020_pq"))
            << "EGL_EXT_gl_colorspace_bt2020_pq extension not available";

    ASSERT_NO_FATAL_FAILURE(get8BitConfig(config));

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;
    std::vector<EGLint> winAttrs;
    winAttrs.push_back(EGL_GL_COLORSPACE_KHR);
    winAttrs.push_back(EGL_GL_COLORSPACE_BT2020_PQ_EXT);
    winAttrs.push_back(EGL_NONE);

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs.data());
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    EGLint value;
    success = eglQuerySurface(mEglDisplay, eglSurface, EGL_GL_COLORSPACE_KHR, &value);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_GL_COLORSPACE_BT2020_PQ_EXT, value);

    ASSERT_NO_FATAL_FAILURE(setSurfaceSmpteMetadata(eglSurface));

    ASSERT_NO_FATAL_FAILURE(checkSurfaceSmpteMetadata(eglSurface));

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));
}

TEST_F(EGLTest, EGLConfigFP16) {
    EGLint numConfigs;
    EGLConfig config;
    EGLBoolean success;

    if (!hasWideColorDisplay) {
        // skip this test if device does not have wide-color display
        RecordProperty("hasWideColorDisplay", false);
        return;
    }

    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_EXT_pixel_format_float"));

    const EGLint attrs[] = {
            // clang-format off
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,          EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,                 16,
            EGL_GREEN_SIZE,               16,
            EGL_BLUE_SIZE,                16,
            EGL_ALPHA_SIZE,               16,
            EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT,
            EGL_NONE,
            // clang-format on
    };
    success = eglChooseConfig(mEglDisplay, attrs, &config, 1, &numConfigs);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(1, numConfigs);

    EGLint components[4];

    success = eglGetConfigAttrib(mEglDisplay, config, EGL_RED_SIZE, &components[0]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_GREEN_SIZE, &components[1]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_BLUE_SIZE, &components[2]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_ALPHA_SIZE, &components[3]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());

    EXPECT_GE(components[0], 16);
    EXPECT_GE(components[1], 16);
    EXPECT_GE(components[2], 16);
    EXPECT_GE(components[3], 16);

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), NULL);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));
}

TEST_F(EGLTest, EGLNoConfigContext) {
    if (!hasWideColorDisplay) {
        // skip this test if device does not have wide-color display
        RecordProperty("hasWideColorDisplay", false);
        return;
    }

    ASSERT_TRUE(hasEglExtension(mEglDisplay, "EGL_KHR_no_config_context"));

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    std::vector<EGLint> contextAttributes;
    contextAttributes.reserve(4);
    contextAttributes.push_back(EGL_CONTEXT_CLIENT_VERSION);
    contextAttributes.push_back(2);
    contextAttributes.push_back(EGL_NONE);
    contextAttributes.push_back(EGL_NONE);

    EGLContext eglContext = eglCreateContext(mEglDisplay, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
                                             contextAttributes.data());
    EXPECT_NE(EGL_NO_CONTEXT, eglContext);
    EXPECT_EQ(EGL_SUCCESS, eglGetError());

    if (eglContext != EGL_NO_CONTEXT) {
        eglDestroyContext(mEglDisplay, eglContext);
    }
}

// Verify that eglCreateContext works when EGL_TELEMETRY_HINT_ANDROID is used with
// NO_HINT = 0, SKIP_TELEMETRY = 1 and an invalid of value of 2
TEST_F(EGLTest, EGLContextTelemetryHintExt) {
    for (int i = 0; i < 3; i++) {
        EGLConfig config;
        get8BitConfig(config);
        std::vector<EGLint> contextAttributes;
        contextAttributes.reserve(4);
        contextAttributes.push_back(EGL_TELEMETRY_HINT_ANDROID);
        contextAttributes.push_back(i);
        contextAttributes.push_back(EGL_NONE);
        contextAttributes.push_back(EGL_NONE);

        EGLContext eglContext = eglCreateContext(mEglDisplay, config, EGL_NO_CONTEXT,
                                                 contextAttributes.data());
        EXPECT_NE(EGL_NO_CONTEXT, eglContext);
        EXPECT_EQ(EGL_SUCCESS, eglGetError());

        if (eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(mEglDisplay, eglContext);
        }
    }
}

// Emulate what a native application would do to create a
// 10:10:10:2 surface.
TEST_F(EGLTest, EGLConfig1010102) {
    EGLint numConfigs;
    EGLConfig config;
    EGLBoolean success;

    if (!hasWideColorDisplay) {
        // skip this test if device does not have wide-color display
        RecordProperty("hasWideColorDisplay", false);
        return;
    }

    const EGLint attrs[] = {
            // clang-format off
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,          EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RED_SIZE,                 10,
            EGL_GREEN_SIZE,               10,
            EGL_BLUE_SIZE,                10,
            EGL_ALPHA_SIZE,               2,
            EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FIXED_EXT,
            EGL_NONE,                     EGL_NONE
            // clang-format on
    };
    success = eglChooseConfig(mEglDisplay, attrs, &config, 1, &numConfigs);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(1, numConfigs);

    EGLint components[4];
    EGLint value;
    eglGetConfigAttrib(mEglDisplay, config, EGL_CONFIG_ID, &value);

    success = eglGetConfigAttrib(mEglDisplay, config, EGL_RED_SIZE, &components[0]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_GREEN_SIZE, &components[1]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_BLUE_SIZE, &components[2]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    success = eglGetConfigAttrib(mEglDisplay, config, EGL_ALPHA_SIZE, &components[3]);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());

    EXPECT_EQ(components[0], 10);
    EXPECT_EQ(components[1], 10);
    EXPECT_EQ(components[2], 10);
    EXPECT_EQ(components[3], 2);

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), NULL);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));
}

TEST_F(EGLTest, EGLInvalidColorspaceAttribute) {
    EGLConfig config;

    ASSERT_NO_FATAL_FAILURE(get8BitConfig(config));

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;

    EGLint winAttrs[] = {
            // clang-format off
            EGL_GL_COLORSPACE_KHR, EGL_BACK_BUFFER,
            EGL_NONE,
            // clang-format on
    };

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs);
    ASSERT_EQ(EGL_BAD_ATTRIBUTE, eglGetError());
    ASSERT_EQ(EGL_NO_SURFACE, eglSurface);
}

TEST_F(EGLTest, EGLUnsupportedColorspaceFormatCombo) {
    EGLint numConfigs;
    EGLConfig config;
    EGLBoolean success;

    if (!hasWideColorDisplay) {
        // skip this test if device does not have wide-color display
        RecordProperty("hasWideColorDisplay", false);
        return;
    }

    const EGLint attrs[] = {
            // clang-format off
            EGL_SURFACE_TYPE,             EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,          EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,                 16,
            EGL_GREEN_SIZE,               16,
            EGL_BLUE_SIZE,                16,
            EGL_ALPHA_SIZE,               16,
            EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT,
            EGL_NONE,
            // clang-format on
    };
    success = eglChooseConfig(mEglDisplay, attrs, &config, 1, &numConfigs);
    ASSERT_EQ(EGL_UNSIGNED_TRUE, success);
    ASSERT_EQ(1, numConfigs);

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;

    const EGLint winAttrs[] = {
            // clang-format off
            EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_DISPLAY_P3_EXT,
            EGL_NONE,
            // clang-format on
    };

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs);
    ASSERT_EQ(EGL_BAD_MATCH, eglGetError());
    ASSERT_EQ(EGL_NO_SURFACE, eglSurface);
}

TEST_F(EGLTest, EGLCreateWindowFailAndSucceed) {
    EGLConfig config;

    ASSERT_NO_FATAL_FAILURE(get8BitConfig(config));

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;

    EGLint winAttrs[] = {
            // clang-format off
            EGL_GL_COLORSPACE_KHR, EGL_BACK_BUFFER,
            EGL_NONE,
            // clang-format on
    };

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs);
    ASSERT_EQ(EGL_BAD_ATTRIBUTE, eglGetError());
    ASSERT_EQ(EGL_NO_SURFACE, eglSurface);

    // Now recreate surface with a valid colorspace. Ensure proper cleanup is done
    // in the first failed attempt (e.g. native_window_api_disconnect).
    winAttrs[1] = EGL_GL_COLORSPACE_LINEAR_KHR;
    eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));
}

TEST_F(EGLTest, EGLCreateWindowTwoColorspaces) {
    EGLConfig config;

    if (!hasWideColorDisplay) {
        // skip this test if device does not have wide-color display
        RecordProperty("hasWideColorDisplay", false);
        return;
    }

    ASSERT_NO_FATAL_FAILURE(get8BitConfig(config));

    struct MockConsumer : public IConsumerListener {
        void onFrameAvailable(const BufferItem& /* item */) override {}
        void onBuffersReleased() override {}
        void onSidebandStreamChanged() override {}
    };

    // Create a EGLSurface
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->consumerConnect(new MockConsumer, false);
    sp<Surface> mSTC = new Surface(producer);
    sp<ANativeWindow> mANW = mSTC;

    const EGLint winAttrs[] = {
            // clang-format off
            EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_DISPLAY_P3_EXT,
            EGL_NONE,
            // clang-format on
    };

    EGLSurface eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), winAttrs);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    android_dataspace dataspace = static_cast<android_dataspace>(ANativeWindow_getBuffersDataSpace(mANW.get()));
    ASSERT_EQ(dataspace, HAL_DATASPACE_DISPLAY_P3);

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));

    // Now create with default attribute (EGL_GL_COLORSPACE_LINEAR_KHR)
    eglSurface = eglCreateWindowSurface(mEglDisplay, config, mANW.get(), NULL);
    ASSERT_EQ(EGL_SUCCESS, eglGetError());
    ASSERT_NE(EGL_NO_SURFACE, eglSurface);

    dataspace = static_cast<android_dataspace>(ANativeWindow_getBuffersDataSpace(mANW.get()));
    // Make sure the dataspace has been reset to UNKNOWN
    ASSERT_NE(dataspace, HAL_DATASPACE_DISPLAY_P3);

    EXPECT_TRUE(eglDestroySurface(mEglDisplay, eglSurface));
}

TEST_F(EGLTest, EGLCheckExtensionString) {
    // check that the format of the extension string is correct

    egl_display_t* display = egl_display_t::get(mEglDisplay);
    ASSERT_NE(display, nullptr);

    std::string extensionStrRegex = "((EGL_ANDROID_front_buffer_auto_refresh|"
       "EGL_ANDROID_get_native_client_buffer|"
       "EGL_ANDROID_presentation_time|"
       "EGL_EXT_surface_CTA861_3_metadata|"
       "EGL_EXT_surface_SMPTE2086_metadata|"
       "EGL_KHR_get_all_proc_addresses|"
       "EGL_KHR_swap_buffers_with_damage|"
       "EGL_ANDROID_get_frame_timestamps|"
       "EGL_EXT_gl_colorspace_scrgb|"
       "EGL_EXT_gl_colorspace_scrgb_linear|"
       "EGL_EXT_gl_colorspace_display_p3_linear|"
       "EGL_EXT_gl_colorspace_display_p3|"
       "EGL_EXT_gl_colorspace_display_p3_passthrough|"
       "EGL_EXT_gl_colorspace_bt2020_hlg|"
       "EGL_EXT_gl_colorspace_bt2020_linear|"
       "EGL_EXT_gl_colorspace_bt2020_pq|"
       "EGL_ANDROID_image_native_buffer|"
       "EGL_ANDROID_native_fence_sync|"
       "EGL_ANDROID_recordable|"
       "EGL_EXT_create_context_robustness|"
       "EGL_EXT_image_gl_colorspace|"
       "EGL_EXT_pixel_format_float|"
       "EGL_EXT_protected_content|"
       "EGL_EXT_yuv_surface|"
       "EGL_IMG_context_priority|"
       "EGL_KHR_config_attribs|"
       "EGL_KHR_create_context|"
       "EGL_KHR_fence_sync|"
       "EGL_KHR_gl_colorspace|"
       "EGL_KHR_gl_renderbuffer_image|"
       "EGL_KHR_gl_texture_2D_image|"
       "EGL_KHR_gl_texture_3D_image|"
       "EGL_KHR_gl_texture_cubemap_image|"
       "EGL_KHR_image|"
       "EGL_KHR_image_base|"
       "EGL_KHR_mutable_render_buffer|"
       "EGL_KHR_no_config_context|"
       "EGL_KHR_partial_update|"
       "EGL_KHR_surfaceless_context|"
       "EGL_KHR_wait_sync|"
       "EGL_EXT_buffer_age|"
       "EGL_KHR_reusable_sync|"
       "EGL_NV_context_priority_realtime) )+";
    EXPECT_THAT(display->getExtensionString(), testing::MatchesRegex(extensionStrRegex));
}

}
