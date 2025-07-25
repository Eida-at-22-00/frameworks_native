/*
 * Copyright (C) 2008 The Android Open Source Project
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

#define LOG_TAG "LayerState"

#include <cinttypes>

#include <android/gui/ISurfaceComposerClient.h>
#include <android/native_window.h>
#include <binder/Parcel.h>
#include <com_android_graphics_libgui_flags.h>
#include <gui/FrameRateUtils.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/LayerState.h>
#include <gui/SurfaceControl.h>
#include <private/gui/ParcelUtils.h>
#include <system/window.h>
#include <utils/Errors.h>

#define CHECK_DIFF(DIFF_RESULT, CHANGE_FLAG, OTHER, FIELD)          \
    {                                                               \
        if ((OTHER.what & CHANGE_FLAG) && (FIELD != OTHER.FIELD)) { \
            DIFF_RESULT |= CHANGE_FLAG;                             \
        }                                                           \
    }

#define CHECK_DIFF2(DIFF_RESULT, CHANGE_FLAG, OTHER, FIELD1, FIELD2) \
    {                                                                \
        CHECK_DIFF(DIFF_RESULT, CHANGE_FLAG, OTHER, FIELD1)          \
        CHECK_DIFF(DIFF_RESULT, CHANGE_FLAG, OTHER, FIELD2)          \
    }

#define CHECK_DIFF3(DIFF_RESULT, CHANGE_FLAG, OTHER, FIELD1, FIELD2, FIELD3) \
    {                                                                        \
        CHECK_DIFF(DIFF_RESULT, CHANGE_FLAG, OTHER, FIELD1)                  \
        CHECK_DIFF(DIFF_RESULT, CHANGE_FLAG, OTHER, FIELD2)                  \
        CHECK_DIFF(DIFF_RESULT, CHANGE_FLAG, OTHER, FIELD3)                  \
    }

namespace android {

using gui::FocusRequest;
using gui::WindowInfoHandle;

namespace {
bool isSameWindowHandle(const sp<WindowInfoHandle>& lhs, const sp<WindowInfoHandle>& rhs) {
    if (lhs == rhs) {
        return true;
    }

    if (!lhs || !rhs) {
        return false;
    }

    return *lhs->getInfo() == *rhs->getInfo();
};

bool isSameSurfaceControl(const sp<SurfaceControl>& lhs, const sp<SurfaceControl>& rhs) {
    if (lhs == rhs) {
        return true;
    }

    return SurfaceControl::isSameSurface(lhs, rhs);
};
} // namespace

layer_state_t::layer_state_t()
      : surface(nullptr),
        layerId(-1),
        what(0),
        x(0),
        y(0),
        z(0),
        flags(0),
        mask(0),
        reserved(0),
        cornerRadius(0.0f),
        clientDrawnCornerRadius(0.0f),
        backgroundBlurRadius(0),
        color(0),
        bufferTransform(0),
        transformToDisplayInverse(false),
        crop({0, 0, -1, -1}),
        dataspace(ui::Dataspace::UNKNOWN),
        api(-1),
        colorTransform(mat4()),
        bgColor(0),
        bgColorDataspace(ui::Dataspace::UNKNOWN),
        colorSpaceAgnostic(false),
        shadowRadius(0.0f),
        frameRateSelectionPriority(-1),
        frameRate(0.0f),
        frameRateCompatibility(ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT),
        changeFrameRateStrategy(ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS),
        defaultFrameRateCompatibility(ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT),
        frameRateCategory(ANATIVEWINDOW_FRAME_RATE_CATEGORY_DEFAULT),
        frameRateCategorySmoothSwitchOnly(false),
        frameRateSelectionStrategy(ANATIVEWINDOW_FRAME_RATE_SELECTION_STRATEGY_PROPAGATE),
        fixedTransformHint(ui::Transform::ROT_INVALID),
        autoRefresh(false),
        trustedOverlay(gui::TrustedOverlay::UNSET),
        bufferCrop(Rect::INVALID_RECT),
        destinationFrame(Rect::INVALID_RECT),
        dropInputMode(gui::DropInputMode::NONE),
        pictureProfileHandle(PictureProfileHandle::NONE),
        appContentPriority(0) {
    matrix.dsdx = matrix.dtdy = 1.0f;
    matrix.dsdy = matrix.dtdx = 0.0f;
    hdrMetadata.validTypes = 0;
}

status_t layer_state_t::write(Parcel& output) const
{
    SAFE_PARCEL(output.writeStrongBinder, surface);
    SAFE_PARCEL(output.writeInt32, layerId);
    SAFE_PARCEL(output.writeUint64, what);
    SAFE_PARCEL(output.writeFloat, x);
    SAFE_PARCEL(output.writeFloat, y);
    SAFE_PARCEL(output.writeInt32, z);
    SAFE_PARCEL(output.writeUint32, layerStack.id);
    SAFE_PARCEL(output.writeUint32, flags);
    SAFE_PARCEL(output.writeUint32, mask);
    SAFE_PARCEL(matrix.write, output);
    SAFE_PARCEL(output.writeFloat, crop.top);
    SAFE_PARCEL(output.writeFloat, crop.left);
    SAFE_PARCEL(output.writeFloat, crop.bottom);
    SAFE_PARCEL(output.writeFloat, crop.right);
    SAFE_PARCEL(SurfaceControl::writeNullableToParcel, output,
                mNotDefCmpState.relativeLayerSurfaceControl);
    SAFE_PARCEL(SurfaceControl::writeNullableToParcel, output,
                mNotDefCmpState.parentSurfaceControlForChild);
    SAFE_PARCEL(output.writeFloat, color.r);
    SAFE_PARCEL(output.writeFloat, color.g);
    SAFE_PARCEL(output.writeFloat, color.b);
    SAFE_PARCEL(output.writeFloat, color.a);
    SAFE_PARCEL(mNotDefCmpState.windowInfoHandle->writeToParcel, &output);
    SAFE_PARCEL(output.write, mNotDefCmpState.transparentRegion);
    SAFE_PARCEL(output.writeUint32, bufferTransform);
    SAFE_PARCEL(output.writeBool, transformToDisplayInverse);
    SAFE_PARCEL(output.writeUint32, static_cast<uint32_t>(dataspace));
    SAFE_PARCEL(output.write, hdrMetadata);
    SAFE_PARCEL(output.write, mNotDefCmpState.surfaceDamageRegion);
    SAFE_PARCEL(output.writeInt32, api);

    if (sidebandStream) {
        SAFE_PARCEL(output.writeBool, true);
        SAFE_PARCEL(output.writeNativeHandle, sidebandStream->handle());
    } else {
        SAFE_PARCEL(output.writeBool, false);
    }

    SAFE_PARCEL(output.write, colorTransform.asArray(), 16 * sizeof(float));
    SAFE_PARCEL(output.writeFloat, cornerRadius);
    SAFE_PARCEL(output.writeFloat, clientDrawnCornerRadius);
    SAFE_PARCEL(output.writeUint32, backgroundBlurRadius);
    SAFE_PARCEL(output.writeParcelable, metadata);
    SAFE_PARCEL(output.writeFloat, bgColor.r);
    SAFE_PARCEL(output.writeFloat, bgColor.g);
    SAFE_PARCEL(output.writeFloat, bgColor.b);
    SAFE_PARCEL(output.writeFloat, bgColor.a);
    SAFE_PARCEL(output.writeUint32, static_cast<uint32_t>(bgColorDataspace));
    SAFE_PARCEL(output.writeBool, colorSpaceAgnostic);
    SAFE_PARCEL(output.writeVectorSize, listeners);

    for (auto listener : listeners) {
        SAFE_PARCEL(output.writeStrongBinder, listener.transactionCompletedListener);
        SAFE_PARCEL(output.writeParcelableVector, listener.callbackIds);
    }
    SAFE_PARCEL(output.writeFloat, shadowRadius);
    SAFE_PARCEL(output.writeParcelable, borderSettings);
    SAFE_PARCEL(output.writeInt32, frameRateSelectionPriority);
    SAFE_PARCEL(output.writeFloat, frameRate);
    SAFE_PARCEL(output.writeByte, frameRateCompatibility);
    SAFE_PARCEL(output.writeByte, changeFrameRateStrategy);
    SAFE_PARCEL(output.writeByte, defaultFrameRateCompatibility);
    SAFE_PARCEL(output.writeByte, frameRateCategory);
    SAFE_PARCEL(output.writeBool, frameRateCategorySmoothSwitchOnly);
    SAFE_PARCEL(output.writeByte, frameRateSelectionStrategy);
    SAFE_PARCEL(output.writeUint32, fixedTransformHint);
    SAFE_PARCEL(output.writeBool, autoRefresh);
    SAFE_PARCEL(output.writeBool, dimmingEnabled);

    SAFE_PARCEL(output.writeUint32, blurRegions.size());
    for (auto region : blurRegions) {
        SAFE_PARCEL(output.writeUint32, region.blurRadius);
        SAFE_PARCEL(output.writeFloat, region.cornerRadiusTL);
        SAFE_PARCEL(output.writeFloat, region.cornerRadiusTR);
        SAFE_PARCEL(output.writeFloat, region.cornerRadiusBL);
        SAFE_PARCEL(output.writeFloat, region.cornerRadiusBR);
        SAFE_PARCEL(output.writeFloat, region.alpha);
        SAFE_PARCEL(output.writeInt32, region.left);
        SAFE_PARCEL(output.writeInt32, region.top);
        SAFE_PARCEL(output.writeInt32, region.right);
        SAFE_PARCEL(output.writeInt32, region.bottom);
    }

    SAFE_PARCEL(output.write, stretchEffect);
    SAFE_PARCEL(output.writeParcelable, edgeExtensionParameters);
    SAFE_PARCEL(output.write, bufferCrop);
    SAFE_PARCEL(output.write, destinationFrame);
    SAFE_PARCEL(output.writeInt32, static_cast<uint32_t>(trustedOverlay));

    SAFE_PARCEL(output.writeUint32, static_cast<uint32_t>(dropInputMode));

    const bool hasBufferData = (bufferData != nullptr);
    SAFE_PARCEL(output.writeBool, hasBufferData);
    if (hasBufferData) {
        SAFE_PARCEL(output.writeParcelable, *bufferData);
    }
    SAFE_PARCEL(output.writeParcelable, trustedPresentationThresholds);
    SAFE_PARCEL(output.writeParcelable, trustedPresentationListener);
    SAFE_PARCEL(output.writeFloat, currentHdrSdrRatio);
    SAFE_PARCEL(output.writeFloat, desiredHdrSdrRatio);
    SAFE_PARCEL(output.writeInt32, static_cast<int32_t>(cachingHint));

    const bool hasBufferReleaseChannel = (bufferReleaseChannel != nullptr);
    SAFE_PARCEL(output.writeBool, hasBufferReleaseChannel);
    if (hasBufferReleaseChannel) {
        SAFE_PARCEL(output.writeParcelable, *bufferReleaseChannel);
    }
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS_APPLY_PICTURE_PROFILES
    SAFE_PARCEL(output.writeInt64, pictureProfileHandle.getId());
    SAFE_PARCEL(output.writeInt32, appContentPriority);
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS_APPLY_PICTURE_PROFILES

    const bool hasLuts = (luts != nullptr);
    SAFE_PARCEL(output.writeBool, hasLuts);
    if (hasLuts) {
        SAFE_PARCEL(output.writeParcelable, *luts);
    }

    return NO_ERROR;
}

status_t layer_state_t::read(const Parcel& input)
{
    SAFE_PARCEL(input.readNullableStrongBinder, &surface);
    SAFE_PARCEL(input.readInt32, &layerId);
    SAFE_PARCEL(input.readUint64, &what);
    SAFE_PARCEL(input.readFloat, &x);
    SAFE_PARCEL(input.readFloat, &y);
    SAFE_PARCEL(input.readInt32, &z);
    SAFE_PARCEL(input.readUint32, &layerStack.id);

    SAFE_PARCEL(input.readUint32, &flags);

    SAFE_PARCEL(input.readUint32, &mask);

    SAFE_PARCEL(matrix.read, input);
    SAFE_PARCEL(input.readFloat, &crop.top);
    SAFE_PARCEL(input.readFloat, &crop.left);
    SAFE_PARCEL(input.readFloat, &crop.bottom);
    SAFE_PARCEL(input.readFloat, &crop.right);

    SAFE_PARCEL(SurfaceControl::readNullableFromParcel, input,
                &mNotDefCmpState.relativeLayerSurfaceControl);
    SAFE_PARCEL(SurfaceControl::readNullableFromParcel, input,
                &mNotDefCmpState.parentSurfaceControlForChild);

    float tmpFloat = 0;
    SAFE_PARCEL(input.readFloat, &tmpFloat);
    color.r = tmpFloat;
    SAFE_PARCEL(input.readFloat, &tmpFloat);
    color.g = tmpFloat;
    SAFE_PARCEL(input.readFloat, &tmpFloat);
    color.b = tmpFloat;
    SAFE_PARCEL(input.readFloat, &tmpFloat);
    color.a = tmpFloat;

    SAFE_PARCEL(mNotDefCmpState.windowInfoHandle->readFromParcel, &input);

    SAFE_PARCEL(input.read, mNotDefCmpState.transparentRegion);
    SAFE_PARCEL(input.readUint32, &bufferTransform);
    SAFE_PARCEL(input.readBool, &transformToDisplayInverse);

    uint32_t tmpUint32 = 0;
    SAFE_PARCEL(input.readUint32, &tmpUint32);
    dataspace = static_cast<ui::Dataspace>(tmpUint32);

    SAFE_PARCEL(input.read, hdrMetadata);
    SAFE_PARCEL(input.read, mNotDefCmpState.surfaceDamageRegion);
    SAFE_PARCEL(input.readInt32, &api);

    bool tmpBool = false;
    SAFE_PARCEL(input.readBool, &tmpBool);
    if (tmpBool) {
        sidebandStream = NativeHandle::create(input.readNativeHandle(), true);
    }

    SAFE_PARCEL(input.read, &colorTransform, 16 * sizeof(float));
    SAFE_PARCEL(input.readFloat, &cornerRadius);
    SAFE_PARCEL(input.readFloat, &clientDrawnCornerRadius);
    SAFE_PARCEL(input.readUint32, &backgroundBlurRadius);
    SAFE_PARCEL(input.readParcelable, &metadata);

    SAFE_PARCEL(input.readFloat, &tmpFloat);
    bgColor.r = tmpFloat;
    SAFE_PARCEL(input.readFloat, &tmpFloat);
    bgColor.g = tmpFloat;
    SAFE_PARCEL(input.readFloat, &tmpFloat);
    bgColor.b = tmpFloat;
    SAFE_PARCEL(input.readFloat, &tmpFloat);
    bgColor.a = tmpFloat;
    SAFE_PARCEL(input.readUint32, &tmpUint32);
    bgColorDataspace = static_cast<ui::Dataspace>(tmpUint32);
    SAFE_PARCEL(input.readBool, &colorSpaceAgnostic);

    int32_t numListeners = 0;
    SAFE_PARCEL_READ_SIZE(input.readInt32, &numListeners, input.dataSize());
    listeners.clear();
    for (int i = 0; i < numListeners; i++) {
        sp<IBinder> listener;
        std::vector<CallbackId> callbackIds;
        SAFE_PARCEL(input.readNullableStrongBinder, &listener);
        SAFE_PARCEL(input.readParcelableVector, &callbackIds);
        listeners.emplace_back(listener, callbackIds);
    }
    SAFE_PARCEL(input.readFloat, &shadowRadius);
    SAFE_PARCEL(input.readParcelable, &borderSettings);

    SAFE_PARCEL(input.readInt32, &frameRateSelectionPriority);
    SAFE_PARCEL(input.readFloat, &frameRate);
    SAFE_PARCEL(input.readByte, &frameRateCompatibility);
    SAFE_PARCEL(input.readByte, &changeFrameRateStrategy);
    SAFE_PARCEL(input.readByte, &defaultFrameRateCompatibility);
    SAFE_PARCEL(input.readByte, &frameRateCategory);
    SAFE_PARCEL(input.readBool, &frameRateCategorySmoothSwitchOnly);
    SAFE_PARCEL(input.readByte, &frameRateSelectionStrategy);
    SAFE_PARCEL(input.readUint32, &tmpUint32);
    fixedTransformHint = static_cast<ui::Transform::RotationFlags>(tmpUint32);
    SAFE_PARCEL(input.readBool, &autoRefresh);
    SAFE_PARCEL(input.readBool, &dimmingEnabled);

    uint32_t numRegions = 0;
    SAFE_PARCEL(input.readUint32, &numRegions);
    blurRegions.clear();
    for (uint32_t i = 0; i < numRegions; i++) {
        BlurRegion region;
        SAFE_PARCEL(input.readUint32, &region.blurRadius);
        SAFE_PARCEL(input.readFloat, &region.cornerRadiusTL);
        SAFE_PARCEL(input.readFloat, &region.cornerRadiusTR);
        SAFE_PARCEL(input.readFloat, &region.cornerRadiusBL);
        SAFE_PARCEL(input.readFloat, &region.cornerRadiusBR);
        SAFE_PARCEL(input.readFloat, &region.alpha);
        SAFE_PARCEL(input.readInt32, &region.left);
        SAFE_PARCEL(input.readInt32, &region.top);
        SAFE_PARCEL(input.readInt32, &region.right);
        SAFE_PARCEL(input.readInt32, &region.bottom);
        blurRegions.push_back(region);
    }

    SAFE_PARCEL(input.read, stretchEffect);
    SAFE_PARCEL(input.readParcelable, &edgeExtensionParameters);
    SAFE_PARCEL(input.read, bufferCrop);
    SAFE_PARCEL(input.read, destinationFrame);
    uint32_t trustedOverlayInt;
    SAFE_PARCEL(input.readUint32, &trustedOverlayInt);
    trustedOverlay = static_cast<gui::TrustedOverlay>(trustedOverlayInt);

    uint32_t mode;
    SAFE_PARCEL(input.readUint32, &mode);
    dropInputMode = static_cast<gui::DropInputMode>(mode);

    bool hasBufferData;
    SAFE_PARCEL(input.readBool, &hasBufferData);
    if (hasBufferData) {
        bufferData = std::make_shared<BufferData>();
        SAFE_PARCEL(input.readParcelable, bufferData.get());
    } else {
        bufferData = nullptr;
    }

    SAFE_PARCEL(input.readParcelable, &trustedPresentationThresholds);
    SAFE_PARCEL(input.readParcelable, &trustedPresentationListener);

    SAFE_PARCEL(input.readFloat, &tmpFloat);
    currentHdrSdrRatio = tmpFloat;
    SAFE_PARCEL(input.readFloat, &tmpFloat);
    desiredHdrSdrRatio = tmpFloat;

    int32_t tmpInt32;
    SAFE_PARCEL(input.readInt32, &tmpInt32);
    cachingHint = static_cast<gui::CachingHint>(tmpInt32);

    bool hasBufferReleaseChannel;
    SAFE_PARCEL(input.readBool, &hasBufferReleaseChannel);
    if (hasBufferReleaseChannel) {
        bufferReleaseChannel = std::make_shared<gui::BufferReleaseChannel::ProducerEndpoint>();
        SAFE_PARCEL(input.readParcelable, bufferReleaseChannel.get());
    }
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS_APPLY_PICTURE_PROFILES
    int64_t pictureProfileId;
    SAFE_PARCEL(input.readInt64, &pictureProfileId);
    pictureProfileHandle = PictureProfileHandle(pictureProfileId);
    SAFE_PARCEL(input.readInt32, &appContentPriority);
#endif // COM_ANDROID_GRAPHICS_LIBGUI_FLAGS_APPLY_PICTURE_PROFILES

    bool hasLuts;
    SAFE_PARCEL(input.readBool, &hasLuts);
    if (hasLuts) {
        luts = std::make_shared<gui::DisplayLuts>();
        SAFE_PARCEL(input.readParcelable, luts.get());
    } else {
        luts = nullptr;
    }

    return NO_ERROR;
}

status_t ComposerState::write(Parcel& output) const {
    return state.write(output);
}

status_t ComposerState::read(const Parcel& input) {
    return state.read(input);
}

DisplayState::DisplayState() = default;

status_t DisplayState::write(Parcel& output) const {
    SAFE_PARCEL(output.writeStrongBinder, token);
    SAFE_PARCEL(output.writeStrongBinder, IInterface::asBinder(surface));
    SAFE_PARCEL(output.writeUint32, what);
    SAFE_PARCEL(output.writeUint32, flags);
    SAFE_PARCEL(output.writeUint32, layerStack.id);
    SAFE_PARCEL(output.writeUint32, toRotationInt(orientation));
    SAFE_PARCEL(output.write, layerStackSpaceRect);
    SAFE_PARCEL(output.write, orientedDisplaySpaceRect);
    SAFE_PARCEL(output.writeUint32, width);
    SAFE_PARCEL(output.writeUint32, height);
    return NO_ERROR;
}

status_t DisplayState::read(const Parcel& input) {
    SAFE_PARCEL(input.readStrongBinder, &token);
    sp<IBinder> tmpBinder;
    SAFE_PARCEL(input.readNullableStrongBinder, &tmpBinder);
    surface = interface_cast<IGraphicBufferProducer>(tmpBinder);

    SAFE_PARCEL(input.readUint32, &what);
    SAFE_PARCEL(input.readUint32, &flags);
    SAFE_PARCEL(input.readUint32, &layerStack.id);
    uint32_t tmpUint = 0;
    SAFE_PARCEL(input.readUint32, &tmpUint);
    orientation = ui::toRotation(tmpUint);

    SAFE_PARCEL(input.read, layerStackSpaceRect);
    SAFE_PARCEL(input.read, orientedDisplaySpaceRect);
    SAFE_PARCEL(input.readUint32, &width);
    SAFE_PARCEL(input.readUint32, &height);
    return NO_ERROR;
}

void DisplayState::merge(const DisplayState& other) {
    if (other.what & eSurfaceChanged) {
        what |= eSurfaceChanged;
        surface = other.surface;
    }
    if (other.what & eLayerStackChanged) {
        what |= eLayerStackChanged;
        layerStack = other.layerStack;
    }
    if (other.what & eFlagsChanged) {
        what |= eFlagsChanged;
        flags = other.flags;
    }
    if (other.what & eDisplayProjectionChanged) {
        what |= eDisplayProjectionChanged;
        orientation = other.orientation;
        layerStackSpaceRect = other.layerStackSpaceRect;
        orientedDisplaySpaceRect = other.orientedDisplaySpaceRect;
    }
    if (other.what & eDisplaySizeChanged) {
        what |= eDisplaySizeChanged;
        width = other.width;
        height = other.height;
    }
}

void DisplayState::sanitize(int32_t permissions) {
    if (what & DisplayState::eLayerStackChanged) {
        if (!(permissions & layer_state_t::Permission::ACCESS_SURFACE_FLINGER)) {
            what &= ~DisplayState::eLayerStackChanged;
            ALOGE("Stripped attempt to set eLayerStackChanged in sanitize");
        }
    }
    if (what & DisplayState::eDisplayProjectionChanged) {
        if (!(permissions & layer_state_t::Permission::ACCESS_SURFACE_FLINGER)) {
            what &= ~DisplayState::eDisplayProjectionChanged;
            ALOGE("Stripped attempt to set eDisplayProjectionChanged in sanitize");
        }
    }
    if (what & DisplayState::eSurfaceChanged) {
        if (!(permissions & layer_state_t::Permission::ACCESS_SURFACE_FLINGER)) {
            what &= ~DisplayState::eSurfaceChanged;
            ALOGE("Stripped attempt to set eSurfaceChanged in sanitize");
        }
    }
}

void layer_state_t::sanitize(int32_t permissions) {
    // TODO: b/109894387
    //
    // SurfaceFlinger's renderer is not prepared to handle cropping in the face of arbitrary
    // rotation. To see the problem observe that if we have a square parent, and a child
    // of the same size, then we rotate the child 45 degrees around its center, the child
    // must now be cropped to a non rectangular 8 sided region.
    //
    // Of course we can fix this in the future. For now, we are lucky, SurfaceControl is
    // private API, and arbitrary rotation is used in limited use cases, for instance:
    // - WindowManager only uses rotation in one case, which is on a top level layer in which
    //   cropping is not an issue.
    // - Launcher, as a privileged app, uses this to transition an application to PiP
    //   (picture-in-picture) mode.
    //
    // However given that abuse of rotation matrices could lead to surfaces extending outside
    // of cropped areas, we need to prevent non-root clients without permission
    // ACCESS_SURFACE_FLINGER nor ROTATE_SURFACE_FLINGER
    // (a.k.a. everyone except WindowManager / tests / Launcher) from setting non rectangle
    // preserving transformations.
    if (what & eMatrixChanged) {
        if (!(permissions & Permission::ROTATE_SURFACE_FLINGER)) {
            ui::Transform t;
            t.set(matrix.dsdx, matrix.dtdy, matrix.dtdx, matrix.dsdy);
            if (!t.preserveRects()) {
                what &= ~eMatrixChanged;
                ALOGE("Stripped non rect preserving matrix in sanitize");
            }
        }
    }

    if (what & eFlagsChanged) {
        if ((flags & eLayerIsDisplayDecoration) &&
            !(permissions & Permission::INTERNAL_SYSTEM_WINDOW)) {
            flags &= ~eLayerIsDisplayDecoration;
            ALOGE("Stripped attempt to set LayerIsDisplayDecoration in sanitize");
        }
        if ((mask & eCanOccludePresentation) &&
            !(permissions & Permission::ACCESS_SURFACE_FLINGER)) {
            flags &= ~eCanOccludePresentation;
            mask &= ~eCanOccludePresentation;
            ALOGE("Stripped attempt to set eCanOccludePresentation in sanitize");
        }
    }

    if (what & layer_state_t::eInputInfoChanged) {
        if (!(permissions & Permission::ACCESS_SURFACE_FLINGER)) {
            what &= ~eInputInfoChanged;
            ALOGE("Stripped attempt to set eInputInfoChanged in sanitize");
        }
    }
    if (what & layer_state_t::eTrustedOverlayChanged) {
        if (!(permissions & Permission::ACCESS_SURFACE_FLINGER)) {
            what &= ~eTrustedOverlayChanged;
            ALOGE("Stripped attempt to set eTrustedOverlay in sanitize");
        }
    }
    if (what & layer_state_t::eDropInputModeChanged) {
        if (!(permissions & Permission::ACCESS_SURFACE_FLINGER)) {
            what &= ~eDropInputModeChanged;
            ALOGE("Stripped attempt to set eDropInputModeChanged in sanitize");
        }
    }
    if (what & layer_state_t::eFrameRateSelectionPriority) {
        if (!(permissions & Permission::ACCESS_SURFACE_FLINGER)) {
            what &= ~eFrameRateSelectionPriority;
            ALOGE("Stripped attempt to set eFrameRateSelectionPriority in sanitize");
        }
    }
    if (what & layer_state_t::eFrameRateChanged) {
        if (!ValidateFrameRate(frameRate, frameRateCompatibility,
                               changeFrameRateStrategy,
                               "layer_state_t::sanitize",
                               permissions & Permission::ACCESS_SURFACE_FLINGER)) {
            what &= ~eFrameRateChanged; // logged in ValidateFrameRate
        }
    }
}

void layer_state_t::merge(const layer_state_t& other) {
    if (other.what & ePositionChanged) {
        what |= ePositionChanged;
        x = other.x;
        y = other.y;
    }
    if (other.what & eLayerChanged) {
        what |= eLayerChanged;
        what &= ~eRelativeLayerChanged;
        z = other.z;
    }
    if (other.what & eAlphaChanged) {
        what |= eAlphaChanged;
        color.a = other.color.a;
    }
    if (other.what & eMatrixChanged) {
        what |= eMatrixChanged;
        matrix = other.matrix;
    }
    if (other.what & eTransparentRegionChanged) {
        what |= eTransparentRegionChanged;
        mNotDefCmpState.transparentRegion = other.mNotDefCmpState.transparentRegion;
    }
    if (other.what & eFlagsChanged) {
        what |= eFlagsChanged;
        flags &= ~other.mask;
        flags |= (other.flags & other.mask);
        mask |= other.mask;
    }
    if (other.what & eLayerStackChanged) {
        what |= eLayerStackChanged;
        layerStack = other.layerStack;
    }
    if (other.what & eCornerRadiusChanged) {
        what |= eCornerRadiusChanged;
        cornerRadius = other.cornerRadius;
    }
    if (other.what & eClientDrawnCornerRadiusChanged) {
        what |= eClientDrawnCornerRadiusChanged;
        clientDrawnCornerRadius = other.clientDrawnCornerRadius;
    }
    if (other.what & eBackgroundBlurRadiusChanged) {
        what |= eBackgroundBlurRadiusChanged;
        backgroundBlurRadius = other.backgroundBlurRadius;
    }
    if (other.what & eBlurRegionsChanged) {
        what |= eBlurRegionsChanged;
        blurRegions = other.blurRegions;
    }
    if (other.what & eRelativeLayerChanged) {
        what |= eRelativeLayerChanged;
        what &= ~eLayerChanged;
        z = other.z;
        mNotDefCmpState.relativeLayerSurfaceControl =
                other.mNotDefCmpState.relativeLayerSurfaceControl;
    }
    if (other.what & eReparent) {
        what |= eReparent;
        mNotDefCmpState.parentSurfaceControlForChild =
                other.mNotDefCmpState.parentSurfaceControlForChild;
    }
    if (other.what & eBufferTransformChanged) {
        what |= eBufferTransformChanged;
        bufferTransform = other.bufferTransform;
    }
    if (other.what & eTransformToDisplayInverseChanged) {
        what |= eTransformToDisplayInverseChanged;
        transformToDisplayInverse = other.transformToDisplayInverse;
    }
    if (other.what & eCropChanged) {
        what |= eCropChanged;
        crop = other.crop;
    }
    if (other.what & eBufferChanged) {
        what |= eBufferChanged;
        bufferData = other.bufferData;
    }
    if (other.what & eTrustedPresentationInfoChanged) {
        what |= eTrustedPresentationInfoChanged;
        trustedPresentationListener = other.trustedPresentationListener;
        trustedPresentationThresholds = other.trustedPresentationThresholds;
    }
    if (other.what & eDataspaceChanged) {
        what |= eDataspaceChanged;
        dataspace = other.dataspace;
    }
    if (other.what & eExtendedRangeBrightnessChanged) {
        what |= eExtendedRangeBrightnessChanged;
        desiredHdrSdrRatio = other.desiredHdrSdrRatio;
        currentHdrSdrRatio = other.currentHdrSdrRatio;
    }
    if (other.what & eDesiredHdrHeadroomChanged) {
        what |= eDesiredHdrHeadroomChanged;
        desiredHdrSdrRatio = other.desiredHdrSdrRatio;
    }
    if (other.what & eCachingHintChanged) {
        what |= eCachingHintChanged;
        cachingHint = other.cachingHint;
    }
    if (other.what & eHdrMetadataChanged) {
        what |= eHdrMetadataChanged;
        hdrMetadata = other.hdrMetadata;
    }
    if (other.what & eSurfaceDamageRegionChanged) {
        what |= eSurfaceDamageRegionChanged;
        mNotDefCmpState.surfaceDamageRegion = other.mNotDefCmpState.surfaceDamageRegion;
    }
    if (other.what & eApiChanged) {
        what |= eApiChanged;
        api = other.api;
    }
    if (other.what & eSidebandStreamChanged) {
        what |= eSidebandStreamChanged;
        sidebandStream = other.sidebandStream;
    }
    if (other.what & eColorTransformChanged) {
        what |= eColorTransformChanged;
        colorTransform = other.colorTransform;
    }
    if (other.what & eHasListenerCallbacksChanged) {
        what |= eHasListenerCallbacksChanged;
    }
    if (other.what & eInputInfoChanged) {
        what |= eInputInfoChanged;
        mNotDefCmpState.windowInfoHandle =
                sp<WindowInfoHandle>::make(*other.mNotDefCmpState.windowInfoHandle);
    }
    if (other.what & eBackgroundColorChanged) {
        what |= eBackgroundColorChanged;
        bgColor = other.bgColor;
        bgColorDataspace = other.bgColorDataspace;
    }
    if (other.what & eMetadataChanged) {
        what |= eMetadataChanged;
        metadata.merge(other.metadata);
    }
    if (other.what & eShadowRadiusChanged) {
        what |= eShadowRadiusChanged;
        shadowRadius = other.shadowRadius;
    }
    if (other.what & eBorderSettingsChanged) {
        what |= eBorderSettingsChanged;
        borderSettings = other.borderSettings;
    }
    if (other.what & eLutsChanged) {
        what |= eLutsChanged;
        luts = other.luts;
    }
    if (other.what & eDefaultFrameRateCompatibilityChanged) {
        what |= eDefaultFrameRateCompatibilityChanged;
        defaultFrameRateCompatibility = other.defaultFrameRateCompatibility;
    }
    if (other.what & eFrameRateSelectionPriority) {
        what |= eFrameRateSelectionPriority;
        frameRateSelectionPriority = other.frameRateSelectionPriority;
    }
    if (other.what & eFrameRateChanged) {
        what |= eFrameRateChanged;
        frameRate = other.frameRate;
        frameRateCompatibility = other.frameRateCompatibility;
        changeFrameRateStrategy = other.changeFrameRateStrategy;
    }
    if (other.what & eFrameRateCategoryChanged) {
        what |= eFrameRateCategoryChanged;
        frameRateCategory = other.frameRateCategory;
        frameRateCategorySmoothSwitchOnly = other.frameRateCategorySmoothSwitchOnly;
    }
    if (other.what & eFrameRateSelectionStrategyChanged) {
        what |= eFrameRateSelectionStrategyChanged;
        frameRateSelectionStrategy = other.frameRateSelectionStrategy;
    }
    if (other.what & eFixedTransformHintChanged) {
        what |= eFixedTransformHintChanged;
        fixedTransformHint = other.fixedTransformHint;
    }
    if (other.what & eAutoRefreshChanged) {
        what |= eAutoRefreshChanged;
        autoRefresh = other.autoRefresh;
    }
    if (other.what & eTrustedOverlayChanged) {
        what |= eTrustedOverlayChanged;
        trustedOverlay = other.trustedOverlay;
    }
    if (other.what & eStretchChanged) {
        what |= eStretchChanged;
        stretchEffect = other.stretchEffect;
    }
    if (other.what & eEdgeExtensionChanged) {
        what |= eEdgeExtensionChanged;
        edgeExtensionParameters = other.edgeExtensionParameters;
    }
    if (other.what & eBufferCropChanged) {
        what |= eBufferCropChanged;
        bufferCrop = other.bufferCrop;
    }
    if (other.what & eDestinationFrameChanged) {
        what |= eDestinationFrameChanged;
        destinationFrame = other.destinationFrame;
    }
    if (other.what & eProducerDisconnect) {
        what |= eProducerDisconnect;
    }
    if (other.what & eDropInputModeChanged) {
        what |= eDropInputModeChanged;
        dropInputMode = other.dropInputMode;
    }
    if (other.what & eColorChanged) {
        what |= eColorChanged;
        color.rgb = other.color.rgb;
    }
    if (other.what & eColorSpaceAgnosticChanged) {
        what |= eColorSpaceAgnosticChanged;
        colorSpaceAgnostic = other.colorSpaceAgnostic;
    }
    if (other.what & eDimmingEnabledChanged) {
        what |= eDimmingEnabledChanged;
        dimmingEnabled = other.dimmingEnabled;
    }
    if (other.what & eFlushJankData) {
        what |= eFlushJankData;
    }
    if (other.what & eBufferReleaseChannelChanged) {
        what |= eBufferReleaseChannelChanged;
        bufferReleaseChannel = other.bufferReleaseChannel;
    }
    if (com_android_graphics_libgui_flags_apply_picture_profiles()) {
        if (other.what & ePictureProfileHandleChanged) {
            what |= ePictureProfileHandleChanged;
            pictureProfileHandle = other.pictureProfileHandle;
        }
        if (other.what & eAppContentPriorityChanged) {
            what |= eAppContentPriorityChanged;
            appContentPriority = other.appContentPriority;
        }
    }
    if ((other.what & what) != other.what) {
        ALOGE("Unmerged SurfaceComposer Transaction properties. LayerState::merge needs updating? "
              "other.what=0x%" PRIX64 " what=0x%" PRIX64 " unmerged flags=0x%" PRIX64,
              other.what, what, (other.what & what) ^ other.what);
    }
}

uint64_t layer_state_t::diff(const layer_state_t& other) const {
    uint64_t diff = 0;
    CHECK_DIFF2(diff, ePositionChanged, other, x, y);
    if (other.what & eLayerChanged) {
        diff |= eLayerChanged;
        diff &= ~eRelativeLayerChanged;
    }
    CHECK_DIFF(diff, eAlphaChanged, other, color.a);
    CHECK_DIFF(diff, eMatrixChanged, other, matrix);
    if (other.what & eTransparentRegionChanged &&
        (!mNotDefCmpState.transparentRegion.hasSameRects(
                other.mNotDefCmpState.transparentRegion))) {
        diff |= eTransparentRegionChanged;
    }
    if (other.what & eFlagsChanged) {
        uint64_t changedFlags = (flags & other.mask) ^ (other.flags & other.mask);
        if (changedFlags) diff |= eFlagsChanged;
    }
    CHECK_DIFF(diff, eLayerStackChanged, other, layerStack);
    CHECK_DIFF(diff, eCornerRadiusChanged, other, cornerRadius);
    CHECK_DIFF(diff, eClientDrawnCornerRadiusChanged, other, clientDrawnCornerRadius);
    CHECK_DIFF(diff, eBackgroundBlurRadiusChanged, other, backgroundBlurRadius);
    if (other.what & eBlurRegionsChanged) diff |= eBlurRegionsChanged;
    if (other.what & eRelativeLayerChanged) {
        diff |= eRelativeLayerChanged;
        diff &= ~eLayerChanged;
    }
    if (other.what & eReparent &&
        !SurfaceControl::isSameSurface(mNotDefCmpState.parentSurfaceControlForChild,
                                       other.mNotDefCmpState.parentSurfaceControlForChild)) {
        diff |= eReparent;
    }
    CHECK_DIFF(diff, eBufferTransformChanged, other, bufferTransform);
    CHECK_DIFF(diff, eTransformToDisplayInverseChanged, other, transformToDisplayInverse);
    CHECK_DIFF(diff, eCropChanged, other, crop);
    if (other.what & eBufferChanged) diff |= eBufferChanged;
    CHECK_DIFF(diff, eDataspaceChanged, other, dataspace);
    CHECK_DIFF2(diff, eExtendedRangeBrightnessChanged, other, currentHdrSdrRatio,
                desiredHdrSdrRatio);
    CHECK_DIFF(diff, eDesiredHdrHeadroomChanged, other, desiredHdrSdrRatio);
    CHECK_DIFF(diff, eCachingHintChanged, other, cachingHint);
    CHECK_DIFF(diff, eHdrMetadataChanged, other, hdrMetadata);
    if (other.what & eSurfaceDamageRegionChanged &&
        (!mNotDefCmpState.surfaceDamageRegion.hasSameRects(
                other.mNotDefCmpState.surfaceDamageRegion))) {
        diff |= eSurfaceDamageRegionChanged;
    }
    CHECK_DIFF(diff, eApiChanged, other, api);
    if (other.what & eSidebandStreamChanged) diff |= eSidebandStreamChanged;
    CHECK_DIFF(diff, eApiChanged, other, api);
    CHECK_DIFF(diff, eColorTransformChanged, other, colorTransform);
    if (other.what & eHasListenerCallbacksChanged) diff |= eHasListenerCallbacksChanged;
    if (other.what & eInputInfoChanged) diff |= eInputInfoChanged;
    CHECK_DIFF2(diff, eBackgroundColorChanged, other, bgColor, bgColorDataspace);
    if (other.what & eMetadataChanged) diff |= eMetadataChanged;
    CHECK_DIFF(diff, eShadowRadiusChanged, other, shadowRadius);
    CHECK_DIFF(diff, eBorderSettingsChanged, other, borderSettings);
    CHECK_DIFF(diff, eDefaultFrameRateCompatibilityChanged, other, defaultFrameRateCompatibility);
    CHECK_DIFF(diff, eFrameRateSelectionPriority, other, frameRateSelectionPriority);
    CHECK_DIFF3(diff, eFrameRateChanged, other, frameRate, frameRateCompatibility,
                changeFrameRateStrategy);
    CHECK_DIFF2(diff, eFrameRateCategoryChanged, other, frameRateCategory,
                frameRateCategorySmoothSwitchOnly);
    CHECK_DIFF(diff, eFrameRateSelectionStrategyChanged, other, frameRateSelectionStrategy);
    CHECK_DIFF(diff, eFixedTransformHintChanged, other, fixedTransformHint);
    CHECK_DIFF(diff, eAutoRefreshChanged, other, autoRefresh);
    CHECK_DIFF(diff, eTrustedOverlayChanged, other, trustedOverlay);
    CHECK_DIFF(diff, eStretchChanged, other, stretchEffect);
    CHECK_DIFF(diff, eEdgeExtensionChanged, other, edgeExtensionParameters);
    CHECK_DIFF(diff, eBufferCropChanged, other, bufferCrop);
    CHECK_DIFF(diff, eDestinationFrameChanged, other, destinationFrame);
    if (other.what & eProducerDisconnect) diff |= eProducerDisconnect;
    CHECK_DIFF(diff, eDropInputModeChanged, other, dropInputMode);
    CHECK_DIFF(diff, eColorChanged, other, color.rgb);
    CHECK_DIFF(diff, eColorSpaceAgnosticChanged, other, colorSpaceAgnostic);
    CHECK_DIFF(diff, eDimmingEnabledChanged, other, dimmingEnabled);
    if (other.what & eBufferReleaseChannelChanged) diff |= eBufferReleaseChannelChanged;
    if (other.what & eLutsChanged) diff |= eLutsChanged;
    CHECK_DIFF(diff, ePictureProfileHandleChanged, other, pictureProfileHandle);
    CHECK_DIFF(diff, eAppContentPriorityChanged, other, appContentPriority);

    return diff;
}

bool layer_state_t::hasBufferChanges() const {
    return what & layer_state_t::eBufferChanged;
}

bool layer_state_t::hasValidBuffer() const {
    return bufferData && (bufferData->hasBuffer() || bufferData->cachedBuffer.isValid());
}

status_t layer_state_t::matrix22_t::write(Parcel& output) const {
    SAFE_PARCEL(output.writeFloat, dsdx);
    SAFE_PARCEL(output.writeFloat, dtdx);
    SAFE_PARCEL(output.writeFloat, dtdy);
    SAFE_PARCEL(output.writeFloat, dsdy);
    return NO_ERROR;
}

status_t layer_state_t::matrix22_t::read(const Parcel& input) {
    SAFE_PARCEL(input.readFloat, &dsdx);
    SAFE_PARCEL(input.readFloat, &dtdx);
    SAFE_PARCEL(input.readFloat, &dtdy);
    SAFE_PARCEL(input.readFloat, &dsdy);
    return NO_ERROR;
}
void layer_state_t::updateTransparentRegion(const Region& transparentRegion) {
    what |= eTransparentRegionChanged;
    mNotDefCmpState.transparentRegion = transparentRegion;
}
void layer_state_t::updateSurfaceDamageRegion(const Region& surfaceDamageRegion) {
    what |= eSurfaceDamageRegionChanged;
    mNotDefCmpState.surfaceDamageRegion = surfaceDamageRegion;
}
void layer_state_t::updateRelativeLayer(const sp<SurfaceControl>& relativeTo, int32_t z) {
    what |= layer_state_t::eRelativeLayerChanged;
    what &= ~layer_state_t::eLayerChanged;
    mNotDefCmpState.relativeLayerSurfaceControl = relativeTo;
    this->z = z;
}
void layer_state_t::updateParentLayer(const sp<SurfaceControl>& newParent) {
    what |= layer_state_t::eReparent;
    mNotDefCmpState.parentSurfaceControlForChild =
            newParent ? newParent->getParentingLayer() : nullptr;
}
void layer_state_t::updateInputWindowInfo(sp<gui::WindowInfoHandle>&& info) {
    what |= eInputInfoChanged;
    mNotDefCmpState.windowInfoHandle = std::move(info);
}

bool layer_state_t::NotDefaultComparableState::operator==(
        const NotDefaultComparableState& rhs) const {
    return transparentRegion.hasSameRects(rhs.transparentRegion) &&
            surfaceDamageRegion.hasSameRects(rhs.surfaceDamageRegion) &&
            isSameWindowHandle(windowInfoHandle, rhs.windowInfoHandle) &&
            isSameSurfaceControl(relativeLayerSurfaceControl, rhs.relativeLayerSurfaceControl) &&
            isSameSurfaceControl(parentSurfaceControlForChild, rhs.parentSurfaceControlForChild);
}

// ------------------------------- InputWindowCommands ----------------------------------------

bool InputWindowCommands::merge(const InputWindowCommands& other) {
    bool changes = false;
    changes |= !other.focusRequests.empty();
    focusRequests.insert(focusRequests.end(), std::make_move_iterator(other.focusRequests.begin()),
                         std::make_move_iterator(other.focusRequests.end()));
    changes |= !other.windowInfosReportedListeners.empty();
    windowInfosReportedListeners.insert(other.windowInfosReportedListeners.begin(),
                                        other.windowInfosReportedListeners.end());
    return changes;
}

bool InputWindowCommands::empty() const {
    return focusRequests.empty() && windowInfosReportedListeners.empty();
}

void InputWindowCommands::clear() {
    focusRequests.clear();
    windowInfosReportedListeners.clear();
}

status_t InputWindowCommands::write(Parcel& output) const {
    SAFE_PARCEL(output.writeParcelableVector, focusRequests);

    SAFE_PARCEL(output.writeInt32, windowInfosReportedListeners.size());
    for (const auto& listener : windowInfosReportedListeners) {
        SAFE_PARCEL(output.writeStrongBinder, listener);
    }

    return NO_ERROR;
}

status_t InputWindowCommands::read(const Parcel& input) {
    SAFE_PARCEL(input.readParcelableVector, &focusRequests);

    int listenerSize = 0;
    SAFE_PARCEL_READ_SIZE(input.readInt32, &listenerSize, input.dataSize());
    windowInfosReportedListeners.reserve(listenerSize);
    for (int i = 0; i < listenerSize; i++) {
        sp<gui::IWindowInfosReportedListener> listener;
        SAFE_PARCEL(input.readStrongBinder, &listener);
        windowInfosReportedListeners.insert(listener);
    }

    return NO_ERROR;
}

// ----------------------------------------------------------------------------

ReleaseCallbackId BufferData::generateReleaseCallbackId() const {
    uint64_t bufferId;
    if (buffer) {
        bufferId = buffer->getId();
    } else {
        bufferId = cachedBuffer.id;
    }
    return {bufferId, frameNumber};
}

status_t BufferData::writeToParcel(Parcel* output) const {
    SAFE_PARCEL(output->writeInt32, flags.get());

    if (buffer) {
        SAFE_PARCEL(output->writeBool, true);
        SAFE_PARCEL(output->write, *buffer);
    } else {
        SAFE_PARCEL(output->writeBool, false);
    }

    if (acquireFence) {
        SAFE_PARCEL(output->writeBool, true);
        SAFE_PARCEL(output->write, *acquireFence);
    } else {
        SAFE_PARCEL(output->writeBool, false);
    }

    SAFE_PARCEL(output->writeUint64, frameNumber);
    SAFE_PARCEL(output->writeStrongBinder, IInterface::asBinder(releaseBufferListener));
    SAFE_PARCEL(output->writeStrongBinder, releaseBufferEndpoint);

    SAFE_PARCEL(output->writeStrongBinder, cachedBuffer.token.promote());
    SAFE_PARCEL(output->writeUint64, cachedBuffer.id);
    SAFE_PARCEL(output->writeBool, hasBarrier);
    SAFE_PARCEL(output->writeUint64, barrierFrameNumber);
    SAFE_PARCEL(output->writeUint32, producerId);
    SAFE_PARCEL(output->writeInt64, dequeueTime);

    return NO_ERROR;
}

status_t BufferData::readFromParcel(const Parcel* input) {
    int32_t tmpInt32;
    SAFE_PARCEL(input->readInt32, &tmpInt32);
    flags = ftl::Flags<BufferDataChange>(tmpInt32);

    bool tmpBool = false;
    SAFE_PARCEL(input->readBool, &tmpBool);
    if (tmpBool) {
        buffer = sp<GraphicBuffer>::make();
        SAFE_PARCEL(input->read, *buffer);
    }

    SAFE_PARCEL(input->readBool, &tmpBool);
    if (tmpBool) {
        acquireFence = sp<Fence>::make();
        SAFE_PARCEL(input->read, *acquireFence);
    }

    SAFE_PARCEL(input->readUint64, &frameNumber);

    sp<IBinder> tmpBinder = nullptr;
    SAFE_PARCEL(input->readNullableStrongBinder, &tmpBinder);
    if (tmpBinder) {
        releaseBufferListener = checked_interface_cast<ITransactionCompletedListener>(tmpBinder);
    }
    SAFE_PARCEL(input->readNullableStrongBinder, &releaseBufferEndpoint);

    tmpBinder = nullptr;
    SAFE_PARCEL(input->readNullableStrongBinder, &tmpBinder);
    cachedBuffer.token = tmpBinder;
    SAFE_PARCEL(input->readUint64, &cachedBuffer.id);

    SAFE_PARCEL(input->readBool, &hasBarrier);
    SAFE_PARCEL(input->readUint64, &barrierFrameNumber);
    SAFE_PARCEL(input->readUint32, &producerId);
    SAFE_PARCEL(input->readInt64, &dequeueTime);

    return NO_ERROR;
}

status_t TrustedPresentationListener::writeToParcel(Parcel* parcel) const {
    SAFE_PARCEL(parcel->writeStrongBinder, mState.callbackInterface);
    SAFE_PARCEL(parcel->writeInt32, mState.callbackId);
    return NO_ERROR;
}

status_t TrustedPresentationListener::readFromParcel(const Parcel* parcel) {
    sp<IBinder> tmpBinder = nullptr;
    SAFE_PARCEL(parcel->readNullableStrongBinder, &tmpBinder);
    if (tmpBinder) {
        mState.callbackInterface = checked_interface_cast<ITransactionCompletedListener>(tmpBinder);
    }
    SAFE_PARCEL(parcel->readInt32, &mState.callbackId);
    return NO_ERROR;
}

}; // namespace android
