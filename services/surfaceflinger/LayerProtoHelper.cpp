/*
 * Copyright (C) 2017 The Android Open Source Project
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

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#include "FrontEnd/LayerCreationArgs.h"
#include "FrontEnd/LayerSnapshot.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wextra"

#include "LayerProtoHelper.h"

namespace android {

using gui::WindowInfo;

namespace surfaceflinger {

void LayerProtoHelper::writePositionToProto(
        const float x, const float y,
        std::function<perfetto::protos::PositionProto*()> getPositionProto) {
    if (x != 0 || y != 0) {
        // Use a lambda do avoid writing the object header when the object is empty
        perfetto::protos::PositionProto* position = getPositionProto();
        position->set_x(x);
        position->set_y(y);
    }
}

void LayerProtoHelper::writeSizeToProto(
        const uint32_t w, const uint32_t h,
        std::function<perfetto::protos::SizeProto*()> getSizeProto) {
    if (w != 0 || h != 0) {
        // Use a lambda do avoid writing the object header when the object is empty
        perfetto::protos::SizeProto* size = getSizeProto();
        size->set_w(w);
        size->set_h(h);
    }
}

void LayerProtoHelper::writeToProto(
        const Region& region, std::function<perfetto::protos::RegionProto*()> getRegionProto) {
    if (region.isEmpty()) {
        return;
    }

    writeToProto(region, getRegionProto());
}

void LayerProtoHelper::writeToProto(const Region& region,
                                    perfetto::protos::RegionProto* regionProto) {
    if (region.isEmpty()) {
        return;
    }

    Region::const_iterator head = region.begin();
    Region::const_iterator const tail = region.end();
    // Use a lambda do avoid writing the object header when the object is empty
    while (head != tail) {
        writeToProto(*head, regionProto->add_rect());
        head++;
    }
}

void LayerProtoHelper::readFromProto(const perfetto::protos::RegionProto& regionProto,
                                     Region& outRegion) {
    for (int i = 0; i < regionProto.rect_size(); i++) {
        Rect rect;
        readFromProto(regionProto.rect(i), rect);
        outRegion.orSelf(rect);
    }
}

void LayerProtoHelper::writeToProto(const Rect& rect,
                                    std::function<perfetto::protos::RectProto*()> getRectProto) {
    if (rect.left != 0 || rect.right != 0 || rect.top != 0 || rect.bottom != 0) {
        // Use a lambda do avoid writing the object header when the object is empty
        writeToProto(rect, getRectProto());
    }
}

void LayerProtoHelper::writeToProto(const Rect& rect, perfetto::protos::RectProto* rectProto) {
    rectProto->set_left(rect.left);
    rectProto->set_top(rect.top);
    rectProto->set_bottom(rect.bottom);
    rectProto->set_right(rect.right);
}

void LayerProtoHelper::readFromProto(const perfetto::protos::RectProto& proto, Rect& outRect) {
    outRect.left = proto.left();
    outRect.top = proto.top();
    outRect.bottom = proto.bottom();
    outRect.right = proto.right();
}

void LayerProtoHelper::readFromProto(const perfetto::protos::RectProto& proto, FloatRect& outRect) {
    outRect.left = proto.left();
    outRect.top = proto.top();
    outRect.bottom = proto.bottom();
    outRect.right = proto.right();
}

void LayerProtoHelper::writeToProto(
        const FloatRect& rect,
        std::function<perfetto::protos::FloatRectProto*()> getFloatRectProto) {
    if (rect.left != 0 || rect.right != 0 || rect.top != 0 || rect.bottom != 0) {
        // Use a lambda do avoid writing the object header when the object is empty
        perfetto::protos::FloatRectProto* rectProto = getFloatRectProto();
        rectProto->set_left(rect.left);
        rectProto->set_top(rect.top);
        rectProto->set_bottom(rect.bottom);
        rectProto->set_right(rect.right);
    }
}

void LayerProtoHelper::writeToProto(const half4 color,
                                    std::function<perfetto::protos::ColorProto*()> getColorProto) {
    if (color.r != 0 || color.g != 0 || color.b != 0 || color.a != 0) {
        // Use a lambda do avoid writing the object header when the object is empty
        perfetto::protos::ColorProto* colorProto = getColorProto();
        colorProto->set_r(color.r);
        colorProto->set_g(color.g);
        colorProto->set_b(color.b);
        colorProto->set_a(color.a);
    }
}

void LayerProtoHelper::writeToProtoDeprecated(const ui::Transform& transform,
                                              perfetto::protos::TransformProto* transformProto) {
    const uint32_t type = transform.getType() | (transform.getOrientation() << 8);
    transformProto->set_type(type);

    // Rotations that are 90/180/270 have their own type so the transform matrix can be
    // reconstructed later. All other rotation have the type UKNOWN so we need to save the transform
    // values in that case.
    if (type & (ui::Transform::SCALE | ui::Transform::UNKNOWN)) {
        transformProto->set_dsdx(transform[0][0]);
        transformProto->set_dtdx(transform[0][1]);
        transformProto->set_dsdy(transform[1][0]);
        transformProto->set_dtdy(transform[1][1]);
    }
}

void LayerProtoHelper::writeTransformToProto(const ui::Transform& transform,
                                             perfetto::protos::TransformProto* transformProto) {
    const uint32_t type = transform.getType() | (transform.getOrientation() << 8);
    transformProto->set_type(type);

    // Rotations that are 90/180/270 have their own type so the transform matrix can be
    // reconstructed later. All other rotation have the type UNKNOWN so we need to save the
    // transform values in that case.
    if (type & (ui::Transform::SCALE | ui::Transform::UNKNOWN)) {
        transformProto->set_dsdx(transform.dsdx());
        transformProto->set_dtdx(transform.dtdx());
        transformProto->set_dtdy(transform.dtdy());
        transformProto->set_dsdy(transform.dsdy());
    }
}

void LayerProtoHelper::writeToProto(
        const renderengine::ExternalTexture& buffer,
        std::function<perfetto::protos::ActiveBufferProto*()> getActiveBufferProto) {
    if (buffer.getWidth() != 0 || buffer.getHeight() != 0 || buffer.getUsage() != 0 ||
        buffer.getPixelFormat() != 0) {
        // Use a lambda do avoid writing the object header when the object is empty
        auto* activeBufferProto = getActiveBufferProto();
        activeBufferProto->set_width(buffer.getWidth());
        activeBufferProto->set_height(buffer.getHeight());
        activeBufferProto->set_stride(buffer.getUsage());
        activeBufferProto->set_format(buffer.getPixelFormat());
    }
}

void LayerProtoHelper::writeToProto(
        const WindowInfo& inputInfo,
        std::function<perfetto::protos::InputWindowInfoProto*()> getInputWindowInfoProto) {
    perfetto::protos::InputWindowInfoProto* proto = getInputWindowInfoProto();
    proto->set_layout_params_flags(inputInfo.layoutParamsFlags.get());
    proto->set_input_config(inputInfo.inputConfig.get());
    using U = std::underlying_type_t<WindowInfo::Type>;
    // TODO(b/129481165): This static assert can be safely removed once conversion warnings
    // are re-enabled.
    static_assert(std::is_same_v<U, int32_t>);
    proto->set_layout_params_type(static_cast<U>(inputInfo.layoutParamsType));

    LayerProtoHelper::writeToProto({inputInfo.frame.left, inputInfo.frame.top,
                                    inputInfo.frame.right, inputInfo.frame.bottom},
                                   [&]() { return proto->mutable_frame(); });
    LayerProtoHelper::writeToProto(inputInfo.touchableRegion,
                                   [&]() { return proto->mutable_touchable_region(); });

    proto->set_surface_inset(inputInfo.surfaceInset);
    using InputConfig = gui::WindowInfo::InputConfig;
    proto->set_visible(!inputInfo.inputConfig.test(InputConfig::NOT_VISIBLE));
    proto->set_focusable(!inputInfo.inputConfig.test(InputConfig::NOT_FOCUSABLE));
    proto->set_has_wallpaper(inputInfo.inputConfig.test(InputConfig::DUPLICATE_TOUCH_TO_WALLPAPER));

    proto->set_global_scale_factor(inputInfo.globalScaleFactor);
    LayerProtoHelper::writeToProtoDeprecated(inputInfo.transform, proto->mutable_transform());
    proto->set_replace_touchable_region_with_crop(inputInfo.replaceTouchableRegionWithCrop);
}

void LayerProtoHelper::writeToProto(const mat4 matrix,
                                    perfetto::protos::ColorTransformProto* colorTransformProto) {
    for (int i = 0; i < mat4::ROW_SIZE; i++) {
        for (int j = 0; j < mat4::COL_SIZE; j++) {
            colorTransformProto->add_val(matrix[i][j]);
        }
    }
}

void LayerProtoHelper::readFromProto(
        const perfetto::protos::ColorTransformProto& colorTransformProto, mat4& matrix) {
    for (int i = 0; i < mat4::ROW_SIZE; i++) {
        for (int j = 0; j < mat4::COL_SIZE; j++) {
            matrix[i][j] = colorTransformProto.val(i * mat4::COL_SIZE + j);
        }
    }
}

void LayerProtoHelper::writeToProto(const android::BlurRegion region,
                                    perfetto::protos::BlurRegion* proto) {
    proto->set_blur_radius(region.blurRadius);
    proto->set_corner_radius_tl(region.cornerRadiusTL);
    proto->set_corner_radius_tr(region.cornerRadiusTR);
    proto->set_corner_radius_bl(region.cornerRadiusBL);
    proto->set_corner_radius_br(region.cornerRadiusBR);
    proto->set_alpha(region.alpha);
    proto->set_left(region.left);
    proto->set_top(region.top);
    proto->set_right(region.right);
    proto->set_bottom(region.bottom);
}

void LayerProtoHelper::readFromProto(const perfetto::protos::BlurRegion& proto,
                                     android::BlurRegion& outRegion) {
    outRegion.blurRadius = proto.blur_radius();
    outRegion.cornerRadiusTL = proto.corner_radius_tl();
    outRegion.cornerRadiusTR = proto.corner_radius_tr();
    outRegion.cornerRadiusBL = proto.corner_radius_bl();
    outRegion.cornerRadiusBR = proto.corner_radius_br();
    outRegion.alpha = proto.alpha();
    outRegion.left = proto.left();
    outRegion.top = proto.top();
    outRegion.right = proto.right();
    outRegion.bottom = proto.bottom();
}

LayerProtoFromSnapshotGenerator& LayerProtoFromSnapshotGenerator::with(
        const frontend::LayerHierarchy& root) {
    mLayersProto.clear_layers();
    mVisitedLayers.clear();
    std::unordered_set<uint64_t> stackIdsToSkip;
    if ((mTraceFlags & LayerTracing::TRACE_VIRTUAL_DISPLAYS) == 0) {
        for (const auto& [layerStack, displayInfo] : mDisplayInfos) {
            if (displayInfo.isVirtual) {
                stackIdsToSkip.insert(layerStack.id);
            }
        }
    }

    frontend::LayerHierarchy::TraversalPath path = frontend::LayerHierarchy::TraversalPath::ROOT;
    for (auto& [child, variant] : root.mChildren) {
        if (variant != frontend::LayerHierarchy::Variant::Attached ||
            stackIdsToSkip.find(child->getLayer()->layerStack.id) != stackIdsToSkip.end()) {
            continue;
        }
        LayerProtoFromSnapshotGenerator::writeHierarchyToProto(*child,
                                                               path.makeChild(child->getLayer()->id,
                                                                              variant));
    }

    // fill in relative and parent info
    for (int i = 0; i < mLayersProto.layers_size(); i++) {
        auto layerProto = mLayersProto.mutable_layers()->Mutable(i);
        auto it = mChildToRelativeParent.find(layerProto->id());
        if (it == mChildToRelativeParent.end()) {
            layerProto->set_z_order_relative_of(-1);
        } else {
            layerProto->set_z_order_relative_of(it->second);
        }
        it = mChildToParent.find(layerProto->id());
        if (it == mChildToParent.end()) {
            layerProto->set_parent(-1);
        } else {
            layerProto->set_parent(it->second);
        }
    }

    return *this;
}

LayerProtoFromSnapshotGenerator& LayerProtoFromSnapshotGenerator::withOffscreenLayers(
        const frontend::LayerHierarchy& offscreenRoot) {
    // Add a fake invisible root layer to the proto output and parent all the offscreen layers to
    // it.
    perfetto::protos::LayerProto* rootProto = mLayersProto.add_layers();
    const int32_t offscreenRootLayerId = INT32_MAX - 2;
    rootProto->set_id(offscreenRootLayerId);
    rootProto->set_name("Offscreen Root");
    rootProto->set_parent(-1);

    perfetto::protos::LayersProto offscreenLayers =
            LayerProtoFromSnapshotGenerator(mSnapshotBuilder, mDisplayInfos, mLegacyLayers,
                                            mTraceFlags)
                    .with(offscreenRoot)
                    .generate();

    for (int i = 0; i < offscreenLayers.layers_size(); i++) {
        perfetto::protos::LayerProto* layerProto = offscreenLayers.mutable_layers()->Mutable(i);
        if (layerProto->parent() == -1) {
            layerProto->set_parent(offscreenRootLayerId);
            // Add layer as child of the fake root
            rootProto->add_children(layerProto->id());
        }
    }

    mLayersProto.mutable_layers()->Reserve(mLayersProto.layers_size() +
                                           offscreenLayers.layers_size());
    std::copy(offscreenLayers.layers().begin(), offscreenLayers.layers().end(),
              RepeatedFieldBackInserter(mLayersProto.mutable_layers()));

    return *this;
}

frontend::LayerSnapshot* LayerProtoFromSnapshotGenerator::getSnapshot(
        const frontend::LayerHierarchy::TraversalPath& path,
        const frontend::RequestedLayerState& layer) {
    frontend::LayerSnapshot* snapshot = mSnapshotBuilder.getSnapshot(path);
    if (snapshot) {
        return snapshot;
    } else {
        mDefaultSnapshots[path] = frontend::LayerSnapshot(layer, path);
        return &mDefaultSnapshots[path];
    }
}

void LayerProtoFromSnapshotGenerator::writeHierarchyToProto(
        const frontend::LayerHierarchy& root, const frontend::LayerHierarchy::TraversalPath& path) {
    using Variant = frontend::LayerHierarchy::Variant;
    perfetto::protos::LayerProto* layerProto = mLayersProto.add_layers();
    const frontend::RequestedLayerState& layer = *root.getLayer();
    frontend::LayerSnapshot* snapshot = getSnapshot(path, layer);
    if (mVisitedLayers.find(snapshot->uniqueSequence) != mVisitedLayers.end()) {
        TransactionTraceWriter::getInstance().invoke("DuplicateLayer", /* overwrite= */ false);
        return;
    }
    mVisitedLayers.insert(snapshot->uniqueSequence);
    LayerProtoHelper::writeSnapshotToProto(layerProto, layer, *snapshot, mTraceFlags);

    for (const auto& [child, variant] : root.mChildren) {
        frontend::LayerSnapshot* childSnapshot =
                getSnapshot(path.makeChild(child->getLayer()->id, variant), layer);
        if (variant == Variant::Attached || variant == Variant::Detached ||
            frontend::LayerHierarchy::isMirror(variant)) {
            mChildToParent[childSnapshot->uniqueSequence] = snapshot->uniqueSequence;
            layerProto->add_children(childSnapshot->uniqueSequence);
        } else if (variant == Variant::Relative) {
            mChildToRelativeParent[childSnapshot->uniqueSequence] = snapshot->uniqueSequence;
            layerProto->add_relatives(childSnapshot->uniqueSequence);
        }
    }

    if (mTraceFlags & LayerTracing::TRACE_COMPOSITION) {
        auto it = mLegacyLayers.find(layer.id);
        if (it != mLegacyLayers.end()) {
            it->second->writeCompositionStateToProto(layerProto, snapshot->outputFilter.layerStack);
        }
    }

    for (const auto& [child, variant] : root.mChildren) {
        // avoid visiting relative layers twice
        if (variant == Variant::Detached) {
            continue;
        }
        writeHierarchyToProto(*child, path.makeChild(child->getLayer()->id, variant));
    }
}

void LayerProtoHelper::writeSnapshotToProto(perfetto::protos::LayerProto* layerInfo,
                                            const frontend::RequestedLayerState& requestedState,
                                            const frontend::LayerSnapshot& snapshot,
                                            uint32_t traceFlags) {
    const ui::Transform transform = snapshot.geomLayerTransform;
    auto buffer = requestedState.externalTexture;
    if (buffer != nullptr) {
        LayerProtoHelper::writeToProto(*buffer,
                                       [&]() { return layerInfo->mutable_active_buffer(); });
        LayerProtoHelper::writeToProtoDeprecated(ui::Transform(requestedState.bufferTransform),
                                                 layerInfo->mutable_buffer_transform());
    }
    layerInfo->set_invalidate(snapshot.contentDirty);
    layerInfo->set_is_protected(snapshot.hasProtectedContent);
    layerInfo->set_dataspace(dataspaceDetails(static_cast<android_dataspace>(snapshot.dataspace)));
    layerInfo->set_curr_frame(requestedState.bufferData->frameNumber);
    layerInfo->set_requested_corner_radius(requestedState.cornerRadius);
    layerInfo->set_corner_radius(
            (snapshot.roundedCorner.radius.x + snapshot.roundedCorner.radius.y) / 2.0);
    layerInfo->set_background_blur_radius(snapshot.backgroundBlurRadius);
    layerInfo->set_is_trusted_overlay(snapshot.trustedOverlay == gui::TrustedOverlay::ENABLED);
    // TODO(b/339701674) update protos
    LayerProtoHelper::writeToProtoDeprecated(transform, layerInfo->mutable_transform());
    LayerProtoHelper::writePositionToProto(transform.tx(), transform.ty(),
                                           [&]() { return layerInfo->mutable_position(); });
    LayerProtoHelper::writeToProto(snapshot.geomLayerBounds,
                                   [&]() { return layerInfo->mutable_bounds(); });
    LayerProtoHelper::writeToProto(snapshot.surfaceDamage,
                                   [&]() { return layerInfo->mutable_damage_region(); });

    if (requestedState.hasColorTransform) {
        LayerProtoHelper::writeToProto(snapshot.colorTransform,
                                       layerInfo->mutable_color_transform());
    }

    LayerProtoHelper::writeToProto(snapshot.croppedBufferSize,
                                   [&]() { return layerInfo->mutable_source_bounds(); });
    LayerProtoHelper::writeToProto(snapshot.transformedBounds,
                                   [&]() { return layerInfo->mutable_screen_bounds(); });
    LayerProtoHelper::writeToProto(snapshot.roundedCorner.cropRect,
                                   [&]() { return layerInfo->mutable_corner_radius_crop(); });
    layerInfo->set_shadow_radius(snapshot.shadowSettings.length);

    layerInfo->set_id(snapshot.uniqueSequence);
    layerInfo->set_original_id(snapshot.sequence);
    if (!snapshot.path.isClone()) {
        layerInfo->set_name(requestedState.name);
    } else {
        layerInfo->set_name(requestedState.name + "(Mirror)");
    }
    layerInfo->set_type("Layer");

    LayerProtoHelper::writeToProto(requestedState.getTransparentRegion(),
                                   [&]() { return layerInfo->mutable_transparent_region(); });

    layerInfo->set_layer_stack(snapshot.outputFilter.layerStack.id);
    layerInfo->set_z(requestedState.z);

    ui::Transform requestedTransform = requestedState.getTransform(0);
    LayerProtoHelper::writePositionToProto(requestedTransform.tx(), requestedTransform.ty(), [&]() {
        return layerInfo->mutable_requested_position();
    });

    LayerProtoHelper::writeToProto(Rect(requestedState.crop),
                                   [&]() { return layerInfo->mutable_crop(); });

    layerInfo->set_is_opaque(snapshot.contentOpaque);
    if (requestedState.externalTexture)
        layerInfo->set_pixel_format(
                decodePixelFormat(requestedState.externalTexture->getPixelFormat()));
    LayerProtoHelper::writeToProto(snapshot.color, [&]() { return layerInfo->mutable_color(); });
    LayerProtoHelper::writeToProto(requestedState.color,
                                   [&]() { return layerInfo->mutable_requested_color(); });
    layerInfo->set_flags(requestedState.flags);

    LayerProtoHelper::writeToProtoDeprecated(requestedTransform,
                                             layerInfo->mutable_requested_transform());

    layerInfo->set_is_relative_of(requestedState.isRelativeOf);

    layerInfo->set_owner_uid(requestedState.ownerUid.val());

    if ((traceFlags & LayerTracing::TRACE_INPUT) && snapshot.hasInputInfo()) {
        LayerProtoHelper::writeToProto(snapshot.inputInfo,
                                       [&]() { return layerInfo->mutable_input_window_info(); });
    }

    if (traceFlags & LayerTracing::TRACE_EXTRA) {
        auto protoMap = layerInfo->mutable_metadata();
        for (const auto& entry : requestedState.metadata.mMap) {
            (*protoMap)[entry.first] = std::string(entry.second.cbegin(), entry.second.cend());
        }
    }

    LayerProtoHelper::writeToProto(requestedState.destinationFrame,
                                   [&]() { return layerInfo->mutable_destination_frame(); });
}

google::protobuf::RepeatedPtrField<perfetto::protos::DisplayProto>
LayerProtoHelper::writeDisplayInfoToProto(const frontend::DisplayInfos& displayInfos) {
    google::protobuf::RepeatedPtrField<perfetto::protos::DisplayProto> displays;
    displays.Reserve(displayInfos.size());
    for (const auto& [layerStack, displayInfo] : displayInfos) {
        auto displayProto = displays.Add();
        displayProto->set_id(displayInfo.info.displayId.val());
        displayProto->set_layer_stack(layerStack.id);
        displayProto->mutable_size()->set_w(displayInfo.info.logicalWidth);
        displayProto->mutable_size()->set_h(displayInfo.info.logicalHeight);
        writeTransformToProto(displayInfo.transform, displayProto->mutable_transform());
        displayProto->set_is_virtual(displayInfo.isVirtual);
    }
    return displays;
}

} // namespace surfaceflinger
} // namespace android

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion -Wextra"
