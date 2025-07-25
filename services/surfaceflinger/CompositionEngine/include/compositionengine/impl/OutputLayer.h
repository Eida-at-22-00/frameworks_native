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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <compositionengine/LayerFE.h>
#include <compositionengine/OutputLayer.h>
#include <ui/FloatRect.h>
#include <ui/PictureProfileHandle.h>
#include <ui/Rect.h>

#include <ui/DisplayIdentification.h>

#include <aidl/android/hardware/graphics/composer3/Composition.h>

using aidl::android::hardware::graphics::composer3::LutProperties;

namespace android::compositionengine {

struct LayerFECompositionState;

namespace impl {

// The implementation class contains the common implementation, but does not
// actually contain the final layer state.
class OutputLayer : public virtual compositionengine::OutputLayer {
public:
    ~OutputLayer() override;

    void setHwcLayer(std::shared_ptr<HWC2::Layer>) override;

    void uncacheBuffers(const std::vector<uint64_t>& bufferIdsToUncache) override;
    int64_t getPictureProfilePriority() const override;
    const PictureProfileHandle& getPictureProfileHandle() const override;
    void commitPictureProfileToCompositionState() override;

    void updateCompositionState(bool includeGeometry, bool forceClientComposition,
                                ui::Transform::RotationFlags,
                                const std::optional<std::vector<std::optional<LutProperties>>>
                                        properties = std::nullopt) override;
    void writeStateToHWC(bool includeGeometry, bool skipLayer, uint32_t z, bool zIsOverridden,
                         bool isPeekingThrough, bool hasLutsProperties) override;
    void writeCursorPositionToHWC() const override;

    HWC2::Layer* getHwcLayer() const override;
    bool requiresClientComposition() const override;
    bool isHardwareCursor() const override;
    void applyDeviceCompositionTypeChange(
            aidl::android::hardware::graphics::composer3::Composition) override;
    void prepareForDeviceLayerRequests() override;
    void applyDeviceLayerRequest(Hwc2::IComposerClient::LayerRequest request) override;
    void applyDeviceLayerLut(::android::base::unique_fd,
                             std::vector<std::pair<int, LutProperties>>) override;
    bool needsFiltering() const override;
    std::optional<LayerFE::LayerSettings> getOverrideCompositionSettings() const override;

    void dump(std::string&) const override;
    virtual FloatRect calculateOutputSourceCrop(uint32_t internalDisplayRotationFlags) const;
    virtual Rect calculateOutputDisplayFrame() const;
    virtual uint32_t calculateOutputRelativeBufferTransform(
            uint32_t internalDisplayRotationFlags) const;

protected:
    // Implemented by the final implementation for the final state it uses.
    virtual void dumpState(std::string&) const = 0;

private:
    Rect calculateInitialCrop() const;
    void writeOutputDependentGeometryStateToHWC(
            HWC2::Layer*, aidl::android::hardware::graphics::composer3::Composition, uint32_t z);
    void writeOutputIndependentGeometryStateToHWC(HWC2::Layer*, const LayerFECompositionState&,
                                                  bool skipLayer);
    void writeOutputDependentPerFrameStateToHWC(HWC2::Layer*);
    void writeOutputIndependentPerFrameStateToHWC(
            HWC2::Layer*, const LayerFECompositionState&,
            aidl::android::hardware::graphics::composer3::Composition compositionType,
            bool skipLayer);
    void writeSolidColorStateToHWC(HWC2::Layer*, const LayerFECompositionState&);
    void writeSidebandStateToHWC(HWC2::Layer*, const LayerFECompositionState&);
    void writeBufferStateToHWC(HWC2::Layer*, const LayerFECompositionState&, bool skipLayer);
    void writeCompositionTypeToHWC(HWC2::Layer*,
                                   aidl::android::hardware::graphics::composer3::Composition,
                                   bool isPeekingThrough, bool skipLayer);
    void writeLutToHWC(HWC2::Layer*, const LayerFECompositionState&);
    void detectDisallowedCompositionTypeChange(
            aidl::android::hardware::graphics::composer3::Composition from,
            aidl::android::hardware::graphics::composer3::Composition to) const;
    bool isClientCompositionForced(bool isPeekingThrough) const;
    void updateLuts(const LayerFECompositionState&,
                    const std::optional<std::vector<std::optional<LutProperties>>>& properties);
};

// This template factory function standardizes the implementation details of the
// final class using the types actually required by the implementation. This is
// not possible to do in the base class as those types may not even be visible
// to the base code.
template <typename BaseOutputLayer>
std::unique_ptr<BaseOutputLayer> createOutputLayerTemplated(const Output& output,
                                                            sp<LayerFE> layerFE) {
    class OutputLayer final : public BaseOutputLayer {
    public:
// Clang incorrectly complains that these are unused.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-local-typedef"

        using OutputLayerCompositionState = std::remove_const_t<
                std::remove_reference_t<decltype(std::declval<BaseOutputLayer>().getState())>>;
        using Output = std::remove_const_t<
                std::remove_reference_t<decltype(std::declval<BaseOutputLayer>().getOutput())>>;
        using LayerFE =
                std::remove_reference_t<decltype(std::declval<BaseOutputLayer>().getLayerFE())>;

#pragma clang diagnostic pop

        OutputLayer(const Output& output, const sp<LayerFE>& layerFE)
              : mOutput(output), mLayerFE(layerFE) {}
        ~OutputLayer() override = default;

    private:
        // compositionengine::OutputLayer overrides
        const Output& getOutput() const override { return mOutput; }
        LayerFE& getLayerFE() const override { return *mLayerFE; }
        const OutputLayerCompositionState& getState() const override { return mState; }
        OutputLayerCompositionState& editState() override { return mState; }

        // compositionengine::impl::OutputLayer overrides
        void dumpState(std::string& out) const override { mState.dump(out); }

        const Output& mOutput;
        const sp<LayerFE> mLayerFE;
        OutputLayerCompositionState mState;
    };

    return std::make_unique<OutputLayer>(output, layerFE);
}

std::unique_ptr<OutputLayer> createOutputLayer(const compositionengine::Output&,
                                               const sp<LayerFE>&);

} // namespace impl
} // namespace android::compositionengine
