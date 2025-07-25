/*
 * Copyright 2018 The Android Open Source Project
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

#include <common/trace.h>
#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/LayerFE.h>
#include <compositionengine/LayerFECompositionState.h>
#include <compositionengine/OutputLayer.h>
#include <compositionengine/impl/CompositionEngine.h>
#include <compositionengine/impl/Display.h>
#include <ui/DisplayMap.h>

#include <renderengine/RenderEngine.h>

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

#include "DisplayHardware/HWComposer.h"

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"

namespace android::compositionengine {

CompositionEngine::~CompositionEngine() = default;

namespace impl {

std::unique_ptr<compositionengine::CompositionEngine> createCompositionEngine() {
    return std::make_unique<CompositionEngine>();
}

CompositionEngine::CompositionEngine() = default;
CompositionEngine::~CompositionEngine() = default;

std::shared_ptr<compositionengine::Display> CompositionEngine::createDisplay(
        const DisplayCreationArgs& args) {
    return compositionengine::impl::createDisplay(*this, args);
}

std::unique_ptr<compositionengine::LayerFECompositionState>
CompositionEngine::createLayerFECompositionState() {
    return std::make_unique<compositionengine::LayerFECompositionState>();
}

HWComposer& CompositionEngine::getHwComposer() const {
    return *mHwComposer;
}

void CompositionEngine::setHwComposer(HWComposer* hwComposer) {
    mHwComposer = hwComposer;
}

renderengine::RenderEngine& CompositionEngine::getRenderEngine() const {
    return *mRenderEngine;
}

void CompositionEngine::setRenderEngine(renderengine::RenderEngine* renderEngine) {
    mRenderEngine = renderEngine;
}

TimeStats* CompositionEngine::getTimeStats() const {
    return mTimeStats.get();
}

void CompositionEngine::setTimeStats(const std::shared_ptr<TimeStats>& timeStats) {
    mTimeStats = timeStats;
}

bool CompositionEngine::needsAnotherUpdate() const {
    return mNeedsAnotherUpdate;
}

nsecs_t CompositionEngine::getLastFrameRefreshTimestamp() const {
    return mRefreshStartTime;
}

namespace {
void offloadOutputs(Outputs& outputs) {
    if (outputs.size() < 2) {
        return;
    }

    ui::PhysicalDisplayVector<compositionengine::Output*> outputsToOffload;
    for (const auto& output : outputs) {
        if (!output->getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>)) {
            // Not HWC-enabled, so it is always client-composited. No need to offload.
            continue;
        }
        if (!output->getState().isEnabled) {
            continue;
        }

        // Only run present in multiple threads if all HWC-enabled displays
        // being refreshed support it.
        if (!output->supportsOffloadPresent()) {
            return;
        }
        outputsToOffload.push_back(output.get());
    }

    if (outputsToOffload.size() < 2) {
        return;
    }

    // Leave the last eligible display on the main thread, which will
    // allow it to run concurrently without an extra thread hop.
    outputsToOffload.pop_back();

    for (compositionengine::Output* output : outputsToOffload) {
        output->offloadPresentNextFrame();
    }
}
} // namespace

void CompositionEngine::present(CompositionRefreshArgs& args) {
    SFTRACE_CALL();
    ALOGV(__FUNCTION__);

    preComposition(args);

    {
        // latchedLayers is used to track the set of front-end layer state that
        // has been latched across all outputs for the prepare step, and is not
        // needed for anything else.
        LayerFESet latchedLayers;

        for (const auto& output : args.outputs) {
            output->prepare(args, latchedLayers);
        }
    }

    // Offloading the HWC call for `present` allows us to simultaneously call it
    // on multiple displays. This is desirable because these calls block and can
    // be slow.
    offloadOutputs(args.outputs);

    ui::DisplayVector<ftl::Future<std::monostate>> presentFutures;
    for (const auto& output : args.outputs) {
        presentFutures.push_back(output->present(args));
    }

    {
        SFTRACE_NAME("Waiting on HWC");
        for (auto& future : presentFutures) {
            // TODO(b/185536303): Call ftl::Future::wait() once it exists, since
            // we do not need the return value of get().
            future.get();
        }
    }
    postComposition(args);
}

void CompositionEngine::updateCursorAsync(CompositionRefreshArgs& args) {

    for (const auto& output : args.outputs) {
        for (auto* layer : output->getOutputLayersOrderedByZ()) {
            if (layer->isHardwareCursor()) {
                layer->writeCursorPositionToHWC();
            }
        }
    }
}

void CompositionEngine::preComposition(CompositionRefreshArgs& args) {
    SFTRACE_CALL();
    ALOGV(__FUNCTION__);

    bool needsAnotherUpdate = false;

    mRefreshStartTime = args.refreshStartTime;

    for (auto& layer : args.layers) {
        if (layer->onPreComposition(args.updatingOutputGeometryThisFrame)) {
            needsAnotherUpdate = true;
        }
    }

    mNeedsAnotherUpdate = needsAnotherUpdate;
}

// If a buffer is latched but the layer is not presented, such as when
// obscured by another layer, the previous buffer needs to be released. We find
// these buffers and fire a NO_FENCE to release it. This ensures that all
// promises for buffer releases are fulfilled at the end of composition.
void CompositionEngine::postComposition(CompositionRefreshArgs& args) {
    SFTRACE_CALL();
    ALOGV(__FUNCTION__);

    for (auto& layerFE : args.layers) {
        if (layerFE->getReleaseFencePromiseStatus() ==
            LayerFE::ReleaseFencePromiseStatus::INITIALIZED) {
            layerFE->setReleaseFence(Fence::NO_FENCE);
        }
    }

    // List of layersWithQueuedFrames does not necessarily overlap with
    // list of layers, so those layersWithQueuedFrames also need any
    // unfulfilled promises to be resolved for completeness.
    for (auto& layerFE : args.layersWithQueuedFrames) {
        if (layerFE->getReleaseFencePromiseStatus() ==
            LayerFE::ReleaseFencePromiseStatus::INITIALIZED) {
            layerFE->setReleaseFence(Fence::NO_FENCE);
        }
    }
}

FeatureFlags CompositionEngine::getFeatureFlags() const {
    return {};
}

void CompositionEngine::dump(std::string&) const {
    // The base class has no state to dump, but derived classes might.
}

void CompositionEngine::setNeedsAnotherUpdateForTest(bool value) {
    mNeedsAnotherUpdate = value;
}

} // namespace impl
} // namespace android::compositionengine
