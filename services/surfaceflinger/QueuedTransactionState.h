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

#pragma once

#include <memory>
#include <vector>
#include "FrontEnd/LayerCreationArgs.h"
#include "renderengine/ExternalTexture.h"

#include <PowerAdvisor/Workload.h>
#include <common/FlagManager.h>
#include <ftl/flags.h>
#include <gui/LayerState.h>
#include <gui/TransactionState.h>
#include <system/window.h>

namespace android {

enum TraverseBuffersReturnValues {
    CONTINUE_TRAVERSAL,
    STOP_TRAVERSAL,
    DELETE_AND_CONTINUE_TRAVERSAL,
};

// Extends the client side composer state by resolving buffer.
class ResolvedComposerState : public ComposerState {
public:
    ResolvedComposerState() = default;
    ResolvedComposerState(ComposerState&& source) { state = std::move(source.state); }
    std::shared_ptr<renderengine::ExternalTexture> externalTexture;
    uint32_t layerId = UNASSIGNED_LAYER_ID;
    uint32_t parentId = UNASSIGNED_LAYER_ID;
    uint32_t relativeParentId = UNASSIGNED_LAYER_ID;
    uint32_t touchCropId = UNASSIGNED_LAYER_ID;
};

struct QueuedTransactionState {
    QueuedTransactionState() = default;

    QueuedTransactionState(TransactionState&& transactionState,
                           std::vector<ResolvedComposerState>&& composerStates,
                           std::vector<uint64_t>&& uncacheBufferIds, int64_t postTime,
                           int originPid, int originUid)
          : frameTimelineInfo(std::move(transactionState.mFrameTimelineInfo)),
            states(composerStates),
            displays(std::move(transactionState.mDisplayStates)),
            flags(transactionState.mFlags),
            applyToken(transactionState.mApplyToken),
            inputWindowCommands(std::move(transactionState.mInputWindowCommands)),
            desiredPresentTime(transactionState.mDesiredPresentTime),
            isAutoTimestamp(transactionState.mIsAutoTimestamp),
            uncacheBufferIds(std::move(uncacheBufferIds)),
            postTime(postTime),
            hasListenerCallbacks(transactionState.mHasListenerCallbacks),
            listenerCallbacks(std::move(transactionState.mListenerCallbacks)),
            originPid(originPid),
            originUid(originUid),
            id(transactionState.getId()),
            mergedTransactionIds(std::move(transactionState.mMergedTransactionIds)) {}

    // Invokes `void(const layer_state_t&)` visitor for matching layers.
    template <typename Visitor>
    void traverseStatesWithBuffers(Visitor&& visitor) const {
        for (const auto& state : states) {
            if (state.state.hasBufferChanges() && state.externalTexture && state.state.surface) {
                visitor(state.state);
            }
        }
    }

    template <typename Visitor>
    void traverseStatesWithBuffersWhileTrue(Visitor&& visitor) NO_THREAD_SAFETY_ANALYSIS {
        for (auto state = states.begin(); state != states.end();) {
            if (state->state.hasBufferChanges() && state->externalTexture && state->state.surface) {
                int result = visitor(*state);
                if (result == STOP_TRAVERSAL) return;
                if (result == DELETE_AND_CONTINUE_TRAVERSAL) {
                    state = states.erase(state);
                    continue;
                }
            }
            state++;
        }
    }

    // TODO(b/185535769): Remove FrameHint. Instead, reset the idle timer (of the relevant physical
    // display) on the main thread if commit leads to composite. Then, RefreshRateOverlay should be
    // able to setFrameRate once, rather than for each transaction.
    bool isFrameActive() const {
        if (!displays.empty()) return true;

        for (const auto& state : states) {
            const bool frameRateChanged = state.state.what & layer_state_t::eFrameRateChanged;
            if (FlagManager::getInstance().vrr_bugfix_24q4()) {
                const bool frameRateIsNoVote = frameRateChanged &&
                        state.state.frameRateCompatibility == ANATIVEWINDOW_FRAME_RATE_NO_VOTE;
                const bool frameRateCategoryChanged =
                        state.state.what & layer_state_t::eFrameRateCategoryChanged;
                const bool frameRateCategoryIsNoPreference = frameRateCategoryChanged &&
                        state.state.frameRateCategory ==
                                ANATIVEWINDOW_FRAME_RATE_CATEGORY_NO_PREFERENCE;
                if (!frameRateIsNoVote && !frameRateCategoryIsNoPreference) {
                    return true;
                }
            } else {
                if (!frameRateChanged ||
                    state.state.frameRateCompatibility != ANATIVEWINDOW_FRAME_RATE_NO_VOTE) {
                    return true;
                }
            }
        }

        return false;
    }

    FrameTimelineInfo frameTimelineInfo;
    std::vector<ResolvedComposerState> states;
    std::vector<DisplayState> displays;
    uint32_t flags;
    sp<IBinder> applyToken;
    InputWindowCommands inputWindowCommands;
    int64_t desiredPresentTime;
    bool isAutoTimestamp;
    std::vector<uint64_t> uncacheBufferIds;
    int64_t postTime;
    bool hasListenerCallbacks;
    std::vector<ListenerCallbacks> listenerCallbacks;
    int originPid;
    int originUid;
    uint64_t id;
    bool sentFenceTimeoutWarning = false;
    std::vector<uint64_t> mergedTransactionIds;
    ftl::Flags<adpf::Workload> workloadHint;
};

} // namespace android
