/*
 * Copyright 2019 The Android Open Source Project
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

#include <cstdint>

#include <android/gui/BorderSettings.h>
#include <android/gui/CachingHint.h>
#include <gui/DisplayLuts.h>
#include <gui/HdrMetadata.h>
#include <math/mat4.h>
#include <ui/BlurRegion.h>
#include <ui/FloatRect.h>
#include <ui/LayerStack.h>
#include <ui/PictureProfileHandle.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <ui/ShadowSettings.h>
#include <ui/Transform.h>

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wextra"

#include <gui/BufferQueue.h>
#include <ui/EdgeExtensionEffect.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicTypes.h>
#include <ui/StretchEffect.h>

#include "DisplayHardware/Hal.h"

#include <aidl/android/hardware/graphics/composer3/Composition.h>

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion -Wextra"

namespace android::compositionengine {

namespace hal = android::hardware::graphics::composer::hal;

// More complex metadata for this layer
struct GenericLayerMetadataEntry {
    // True if the metadata may affect the composed result.
    // See setLayerGenericMetadata in IComposerClient.hal
    bool mandatory;

    // Byte blob or parcel
    std::vector<uint8_t> value;

    std::string dumpAsString() const;

    struct Hasher {
        size_t operator()(const GenericLayerMetadataEntry& entry) const {
            size_t hash = 0;
            for (const auto value : entry.value) {
                hashCombineSingleHashed(hash, value);
            }
            return hash;
        }
    };
};

inline bool operator==(const GenericLayerMetadataEntry& lhs, const GenericLayerMetadataEntry& rhs) {
    return lhs.mandatory == rhs.mandatory && lhs.value == rhs.value;
}

// Defining PrintTo helps with Google Tests.
inline void PrintTo(const GenericLayerMetadataEntry& v, ::std::ostream* os) {
    *os << v.dumpAsString();
}

using GenericLayerMetadataMap = std::unordered_map<std::string, GenericLayerMetadataEntry>;

/*
 * Used by LayerFE::getCompositionState
 * Note that fields that affect HW composer state may need to be mirrored into
 * android::compositionengine::impl::planner::LayerState
 */
struct LayerFECompositionState {
    // If set to true, forces client composition on all output layers until
    // the next geometry change.
    bool forceClientComposition{false};

    // TODO(b/121291683): Reorganize and rename the contents of this structure

    /*
     * Visibility state
     */

    // The filter that determines which outputs include this layer
    ui::LayerFilter outputFilter;

    // If false, this layer should not be considered visible
    bool isVisible{true};

    // True if the layer is completely opaque
    bool isOpaque{true};

    // If true, invalidates the entire visible region
    bool contentDirty{false};

    // The alpha value for this layer
    float alpha{1.f};

    // Background blur in pixels
    int backgroundBlurRadius{0};

    // The transform from layer local coordinates to composition coordinates
    ui::Transform geomLayerTransform;

    // The inverse of the layer transform
    ui::Transform geomInverseLayerTransform;

    // The hint from the layer producer as to what portion of the layer is
    // transparent.
    Region transparentRegionHint;

    // The blend mode for this layer
    hal::BlendMode blendMode{hal::BlendMode::INVALID};

    // The bounds of the layer in layer local coordinates
    FloatRect geomLayerBounds;

    // The crop to apply to the layer in layer local coordinates
    FloatRect geomLayerCrop;

    ShadowSettings shadowSettings;

    // The settings to configure the outline of a layer.
    gui::BorderSettings borderSettings;

    // List of regions that require blur
    std::vector<BlurRegion> blurRegions;

    StretchEffect stretchEffect;
    EdgeExtensionEffect edgeExtensionEffect;

    /*
     * Geometry state
     */

    bool isSecure{false};
    bool geomUsesSourceCrop{false};
    bool geomBufferUsesDisplayInverseTransform{false};
    uint32_t geomBufferTransform{0};
    Rect geomBufferSize;
    Rect geomContentCrop;
    FloatRect geomCrop;

    GenericLayerMetadataMap metadata;

    /*
     * Per-frame content
     */

    // The type of composition for this layer
    aidl::android::hardware::graphics::composer3::Composition compositionType{
            aidl::android::hardware::graphics::composer3::Composition::INVALID};

    // The buffer and related state
    sp<GraphicBuffer> buffer;
    sp<Fence> acquireFence = Fence::NO_FENCE;
    Region surfaceDamage;
    uint64_t frameNumber = 0;

    // The handle to use for a sideband stream for this layer
    sp<NativeHandle> sidebandStream;
    // If true, this sideband layer has a frame update
    bool sidebandStreamHasFrame{false};

    // The color for this layer
    half4 color;

    /*
     * Per-frame presentation state
     */

    // If true, this layer will use the dataspace chosen for the output and
    // ignore the dataspace value just below
    bool isColorspaceAgnostic{false};

    // The dataspace for this layer
    ui::Dataspace dataspace{ui::Dataspace::UNKNOWN};

    // The metadata for this layer
    HdrMetadata hdrMetadata;

    // The color transform
    mat4 colorTransform;
    bool colorTransformIsIdentity{true};

    // True if the layer has protected content
    bool hasProtectedContent{false};

    /*
     * Cursor state
     */

    // The output-independent frame for the cursor
    Rect cursorFrame;

    // framerate of the layer as measured by LayerHistory
    float fps;

    // The dimming flag
    bool dimmingEnabled{true};

    float currentHdrSdrRatio = 1.f;
    float desiredHdrSdrRatio = 1.f;

    // A picture profile handle refers to a PictureProfile configured on the display, which is a
    // set of parameters that configures the picture processing hardware that is used to enhance
    // the quality of buffer contents.
    PictureProfileHandle pictureProfileHandle{PictureProfileHandle::NONE};

    // A layer's priority in terms of limited picture processing pipeline utilization.
    int64_t pictureProfilePriority;

    gui::CachingHint cachingHint = gui::CachingHint::Enabled;

    std::shared_ptr<gui::DisplayLuts> luts;

    virtual ~LayerFECompositionState();

    // Debugging
    virtual void dump(std::string& out) const;
};

} // namespace android::compositionengine
