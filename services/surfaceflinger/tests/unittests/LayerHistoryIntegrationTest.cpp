/*
 * Copyright 2023 The Android Open Source Project
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
#define LOG_TAG "LayerHistoryIntegrationTest"

#include <Layer.h>
#include <com_android_graphics_surfaceflinger_flags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <log/log.h>

#include <renderengine/mock/FakeExternalTexture.h>

#include <common/test/FlagUtils.h>
#include "FpsOps.h"
#include "LayerHierarchyTest.h"
#include "Scheduler/LayerHistory.h"
#include "Scheduler/LayerInfo.h"
#include "TestableScheduler.h"
#include "TestableSurfaceFlinger.h"
#include "mock/DisplayHardware/MockDisplayMode.h"
#include "mock/MockSchedulerCallback.h"

namespace android::scheduler {

using android::mock::createDisplayMode;
using android::mock::createVrrDisplayMode;
using namespace com::android::graphics::surfaceflinger;

class LayerHistoryIntegrationTest : public surfaceflinger::frontend::LayerSnapshotTestBase {
protected:
    static constexpr auto PRESENT_TIME_HISTORY_SIZE = LayerInfo::HISTORY_SIZE;
    static constexpr auto MAX_FREQUENT_LAYER_PERIOD_NS = LayerInfo::kMaxPeriodForFrequentLayerNs;
    static constexpr auto FREQUENT_LAYER_WINDOW_SIZE = LayerInfo::kFrequentLayerWindowSize;
    static constexpr auto PRESENT_TIME_HISTORY_DURATION = LayerInfo::HISTORY_DURATION;

    static constexpr Fps LO_FPS = 30_Hz;
    static constexpr auto LO_FPS_PERIOD = LO_FPS.getPeriodNsecs();

    static constexpr Fps HI_FPS = 90_Hz;
    static constexpr auto HI_FPS_PERIOD = HI_FPS.getPeriodNsecs();

    static constexpr auto kVrrModeId = DisplayModeId(2);

    LayerHistoryIntegrationTest() : LayerSnapshotTestBase() {
        mFlinger.resetScheduler(mScheduler);
        mLifecycleManager = {};
        mHierarchyBuilder = {};
    }

    void updateLayerSnapshotsAndLayerHistory(nsecs_t now) {
        LayerSnapshotTestBase::update(mFlinger.mutableLayerSnapshotBuilder());
        mFlinger.updateLayerHistory(now);
    }

    void setBufferWithPresentTime(sp<Layer>& layer, nsecs_t time) {
        uint32_t sequence = static_cast<uint32_t>(layer->sequence);
        setBuffer(sequence);
        layer->setDesiredPresentTime(time, false /*autotimestamp*/);
        updateLayerSnapshotsAndLayerHistory(time);
    }

    void setFrontBufferWithPresentTime(sp<Layer>& layer, nsecs_t time) {
        uint32_t sequence = static_cast<uint32_t>(layer->sequence);
        setFrontBuffer(sequence);
        layer->setDesiredPresentTime(time, false /*autotimestamp*/);
        updateLayerSnapshotsAndLayerHistory(time);
    }

    LayerHistory& history() { return mScheduler->mutableLayerHistory(); }
    const LayerHistory& history() const { return mScheduler->mutableLayerHistory(); }

    LayerHistory::Summary summarizeLayerHistory(nsecs_t now) {
        // LayerHistory::summarize makes no guarantee of the order of the elements in the summary
        // however, for testing only, a stable order is required, therefore we sort the list here.
        // Any tests requiring ordered results must create layers with names.
        auto summary = history().summarize(*mScheduler->refreshRateSelector(), now);
        std::sort(summary.begin(), summary.end(),
                  [](const RefreshRateSelector::LayerRequirement& lhs,
                     const RefreshRateSelector::LayerRequirement& rhs) -> bool {
                      return lhs.name < rhs.name;
                  });
        return summary;
    }

    size_t layerCount() const { return mScheduler->layerHistorySize(); }
    size_t activeLayerCount() const NO_THREAD_SAFETY_ANALYSIS {
        return history().mActiveLayerInfos.size();
    }

    auto frequentLayerCount(nsecs_t now) const NO_THREAD_SAFETY_ANALYSIS {
        const auto& infos = history().mActiveLayerInfos;
        return std::count_if(infos.begin(), infos.end(), [now](const auto& pair) {
            return pair.second.second->isFrequent(now).isFrequent;
        });
    }

    auto animatingLayerCount(nsecs_t now) const NO_THREAD_SAFETY_ANALYSIS {
        const auto& infos = history().mActiveLayerInfos;
        return std::count_if(infos.begin(), infos.end(), [now](const auto& pair) {
            return pair.second.second->isAnimating(now);
        });
    }

    auto clearLayerHistoryCount(nsecs_t now) const NO_THREAD_SAFETY_ANALYSIS {
        const auto& infos = history().mActiveLayerInfos;
        return std::count_if(infos.begin(), infos.end(), [now](const auto& pair) {
            return pair.second.second->isFrequent(now).clearHistory;
        });
    }

    void setDefaultLayerVote(Layer* layer,
                             LayerHistory::LayerVoteType vote) NO_THREAD_SAFETY_ANALYSIS {
        auto [found, layerPair] = history().findLayer(layer->getSequence());
        if (found != LayerHistory::LayerStatus::NotFound) {
            layerPair->second->setDefaultLayerVote(vote);
        }
    }

    auto createLegacyAndFrontedEndLayer(uint32_t sequence) {
        std::string layerName = "test layer:" + std::to_string(sequence);
        const auto layer =
                sp<Layer>::make(LayerCreationArgs{mFlinger.flinger(),
                                                  nullptr,
                                                  layerName,
                                                  0,
                                                  {},
                                                  std::make_optional<uint32_t>(sequence)});
        mFlinger.injectLegacyLayer(layer);
        createRootLayer(sequence);
        return layer;
    }

    auto createLegacyAndFrontedEndLayerWithUid(uint32_t sequence, gui::Uid uid) {
        std::string layerName = "test layer:" + std::to_string(sequence);
        auto args = LayerCreationArgs{mFlinger.flinger(),
                                      nullptr,
                                      layerName,
                                      0,
                                      {},
                                      std::make_optional<uint32_t>(sequence)};
        args.ownerUid = uid.val();
        const auto layer = sp<Layer>::make(args);
        mFlinger.injectLegacyLayer(layer);
        createRootLayerWithUid(sequence, uid);
        return layer;
    }

    auto destroyLayer(sp<Layer>& layer) {
        uint32_t sequence = static_cast<uint32_t>(layer->sequence);
        mFlinger.releaseLegacyLayer(sequence);
        layer.clear();
        destroyLayerHandle(sequence);
    }

    void recordFramesAndExpect(sp<Layer>& layer, nsecs_t& time, Fps frameRate,
                               Fps desiredRefreshRate, int numFrames) {
        LayerHistory::Summary summary;
        for (int i = 0; i < numFrames; i++) {
            setBufferWithPresentTime(layer, time);
            time += frameRate.getPeriodNsecs();

            summary = summarizeLayerHistory(time);
        }

        ASSERT_EQ(1u, summary.size());
        ASSERT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
        ASSERT_EQ(desiredRefreshRate, summary[0].desiredRefreshRate);
    }

    std::shared_ptr<RefreshRateSelector> mSelector = std::make_shared<RefreshRateSelector>(
            makeModes(createDisplayMode(DisplayModeId(0), LO_FPS),
                      createDisplayMode(DisplayModeId(1), HI_FPS),
                      createVrrDisplayMode(kVrrModeId, HI_FPS,
                                           hal::VrrConfig{.minFrameIntervalNs =
                                                                  HI_FPS.getPeriodNsecs()})),
            DisplayModeId(0));

    mock::SchedulerCallback mSchedulerCallback;
    TestableSurfaceFlinger mFlinger;
    TestableScheduler* mScheduler = new TestableScheduler(mSelector, mFlinger, mSchedulerCallback);
};

namespace {

TEST_F(LayerHistoryIntegrationTest, singleLayerNoVoteDefaultCompatibility) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();

    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    // No layers returned if no layers are active.
    EXPECT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());

    setBuffer(1);
    setDefaultFrameRateCompatibility(1, ANATIVEWINDOW_FRAME_RATE_NO_VOTE);
    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(1u, activeLayerCount());
}

TEST_F(LayerHistoryIntegrationTest, singleLayerMinVoteDefaultCompatibility) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    EXPECT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());

    setBuffer(1);
    setDefaultFrameRateCompatibility(1, ANATIVEWINDOW_FRAME_RATE_MIN);
    updateLayerSnapshotsAndLayerHistory(time);

    auto summary = summarizeLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());

    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
}

TEST_F(LayerHistoryIntegrationTest, oneLayer) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    // No layers returned if no layers are active.
    EXPECT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());

    // Max returned if active layers have insufficient history.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE - 1; i++) {
        setBuffer(1);
        updateLayerSnapshotsAndLayerHistory(time);
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        time += LO_FPS_PERIOD;
    }

    // Max is returned since we have enough history but there is no timestamp votes.
    for (size_t i = 0; i < 10; i++) {
        setBuffer(1);
        updateLayerSnapshotsAndLayerHistory(time);
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        time += LO_FPS_PERIOD;
    }
}

TEST_F(LayerHistoryIntegrationTest, gameFrameRateOverrideMapping) {
    SET_FLAG_FOR_TEST(flags::game_default_frame_rate, true);

    history().updateGameDefaultFrameRateOverride(FrameRateOverride({0, 60.0f}));

    auto overridePair = history().getGameFrameRateOverride(0);
    EXPECT_EQ(0_Hz, overridePair.first);
    EXPECT_EQ(60_Hz, overridePair.second);

    history().updateGameModeFrameRateOverride(FrameRateOverride({0, 40.0f}));
    history().updateGameModeFrameRateOverride(FrameRateOverride({1, 120.0f}));

    overridePair = history().getGameFrameRateOverride(0);
    EXPECT_EQ(40_Hz, overridePair.first);
    EXPECT_EQ(60_Hz, overridePair.second);

    overridePair = history().getGameFrameRateOverride(1);
    EXPECT_EQ(120_Hz, overridePair.first);
    EXPECT_EQ(0_Hz, overridePair.second);

    history().updateGameDefaultFrameRateOverride(FrameRateOverride({0, 0.0f}));
    history().updateGameModeFrameRateOverride(FrameRateOverride({1, 0.0f}));

    overridePair = history().getGameFrameRateOverride(0);
    EXPECT_EQ(40_Hz, overridePair.first);
    EXPECT_EQ(0_Hz, overridePair.second);

    overridePair = history().getGameFrameRateOverride(1);
    EXPECT_EQ(0_Hz, overridePair.first);
    EXPECT_EQ(0_Hz, overridePair.second);
}

TEST_F(LayerHistoryIntegrationTest, oneLayerGameFrameRateOverride) {
    SET_FLAG_FOR_TEST(flags::game_default_frame_rate, true);

    const uid_t uid = 0;
    const Fps gameDefaultFrameRate = Fps::fromValue(30.0f);
    const Fps gameModeFrameRate = Fps::fromValue(60.0f);

    auto layer = createLegacyAndFrontedEndLayerWithUid(1, gui::Uid(uid));
    showLayer(1);

    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    // update game default frame rate override
    history().updateGameDefaultFrameRateOverride(
            FrameRateOverride({uid, gameDefaultFrameRate.getValue()}));

    LayerHistory::Summary summary;
    scheduler::LayerProps layerProps = {
            .visible = true,
            .bounds = {0, 0, 100, 100},
            .transform = {},
            .setFrameRateVote = {},
            .frameRateSelectionPriority = Layer::PRIORITY_UNSET,
            .isSmallDirty = false,
            .isFrontBuffered = false,
    };

    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += gameDefaultFrameRate.getPeriodNsecs();
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    ASSERT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summary[0].vote);
    ASSERT_EQ(30.0_Hz, summary[0].desiredRefreshRate);

    // test against setFrameRate vote
    setFrameRate(1,
                 Layer::FrameRate(Fps::fromValue(120.0f), Layer::FrameRateCompatibility::Default));
    updateLayerSnapshotsAndLayerHistory(time);

    const Fps setFrameRate = Fps::fromValue(120.0f);
    layerProps.setFrameRateVote =
            Layer::FrameRate(setFrameRate, Layer::FrameRateCompatibility::Default);

    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += setFrameRate.getPeriodNsecs();
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    ASSERT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summary[0].vote);
    ASSERT_EQ(120.0_Hz, summary[0].desiredRefreshRate);

    // update game mode frame rate override
    history().updateGameModeFrameRateOverride(
            FrameRateOverride({uid, gameModeFrameRate.getValue()}));

    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += gameModeFrameRate.getPeriodNsecs();
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    ASSERT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summary[0].vote);
    ASSERT_EQ(60.0_Hz, summary[0].desiredRefreshRate);
}

TEST_F(LayerHistoryIntegrationTest, oneInvisibleLayer) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    setBuffer(1);
    updateLayerSnapshotsAndLayerHistory(time);
    auto summary = summarizeLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    // Layer is still considered inactive so we expect to get Min
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());

    hideLayer(1);
    setBuffer(1);
    updateLayerSnapshotsAndLayerHistory(time);

    summary = summarizeLayerHistory(time);
    EXPECT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());
}

TEST_F(LayerHistoryIntegrationTest, explicitTimestamp) {
    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += LO_FPS_PERIOD;
    }

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(LO_FPS, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerNoVote) {
    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);

    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::NoVote);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    ASSERT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer became inactive
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerMinVote) {
    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);

    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::Min);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer became inactive
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerMaxVote) {
    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);

    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::Max);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += LO_FPS_PERIOD;
    }

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer became inactive
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitVote) {
    createLegacyAndFrontedEndLayer(1);
    setFrameRate(1, 73.4f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(73.4_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitExactVote) {
    createLegacyAndFrontedEndLayer(1);
    setFrameRate(1, 73.4f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(73.4_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitExactVote2) {
    auto layer = createLegacyAndFrontedEndLayer(1);
    setFrameRate(1, 73.4f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);

    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(73.4_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer became infrequent, but the vote stays
    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::Heuristic);
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(73.4_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitGte_vrr) {
    // Set the test to be on a vrr mode.
    SET_FLAG_FOR_TEST(flags::vrr_config, true);
    mSelector->setActiveMode(kVrrModeId, HI_FPS);

    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    setFrameRate(1, (33_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_AT_LEAST,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, 0);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitGte, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(33_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[0].frameRateCategory);

    // layer became inactive, but the vote stays
    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::Heuristic);
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitGte, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(33_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[0].frameRateCategory);
}

// Test for MRR device with VRR features enabled.
TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitGte_nonVrr) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, true);
    // The vrr_config flag is explicitly not set false because this test for an MRR device
    // should still work in a VRR-capable world.

    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    setFrameRate(1, (33_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_AT_LEAST,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, 0);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[0].frameRateCategory);

    // layer became infrequent, but the vote stays
    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::Heuristic);
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[0].frameRateCategory);
}

TEST_F(LayerHistoryIntegrationTest, oneLayerGteNoVote_arr) {
    SET_FLAG_FOR_TEST(flags::arr_setframerate_gte_enum, true);
    // Set the test to be on a vrr mode.
    SET_FLAG_FOR_TEST(flags::vrr_config, true);
    mSelector->setActiveMode(kVrrModeId, HI_FPS);

    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    setFrameRate(1, (0_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_AT_LEAST,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    // Layer is active but GTE with 0 should be considered NoVote, thus nothing from summarize.
    ASSERT_EQ(0u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer became inactive.
    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::Heuristic);
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_EQ(0u, summarizeLayerHistory(time).size());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerGteNoVote_mrr) {
    SET_FLAG_FOR_TEST(flags::arr_setframerate_gte_enum, true);
    // True by default on MRR devices as well, but the device is not set to VRR mode.
    SET_FLAG_FOR_TEST(flags::vrr_config, true);

    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    setFrameRate(1, (0_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_AT_LEAST,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, 0);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    // Layer is active but GTE with 0 should be considered NoVote, thus nothing from summarize.
    ASSERT_EQ(0u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer became inactive.
    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::Heuristic);
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_EQ(0u, summarizeLayerHistory(time).size());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitVoteWithCategory_vrrFeatureOff) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, false);

    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    setFrameRate(1, (73.4_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_HIGH);

    // Set default to Min so it is obvious that the vote reset triggered.
    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::Min);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    // There is only 1 LayerRequirement due to the disabled flag frame_rate_category_mrr.
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[0].frameRateCategory);
}

// This test case should be the same as oneLayerNoVote except instead of layer vote is NoVote,
// the category is NoPreference.
TEST_F(LayerHistoryIntegrationTest, oneLayerCategoryNoPreference) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, true);

    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    setFrameRate(1, (0_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_NO_PREFERENCE);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    EXPECT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer became infrequent
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    EXPECT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

// Tests MRR NoPreference-only vote, no game default override. Expects vote reset.
TEST_F(LayerHistoryIntegrationTest, oneLayerCategoryNoPreference_mrr) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, false);
    SET_FLAG_FOR_TEST(flags::game_default_frame_rate, true);
    SET_FLAG_FOR_TEST(flags::vrr_config, true);

    const LayerHistory::LayerVoteType defaultVote = LayerHistory::LayerVoteType::Min;

    auto layer = createLegacyAndFrontedEndLayer(1);
    setDefaultLayerVote(layer.get(), defaultVote);
    showLayer(1);
    setFrameRate(1, (0_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_NO_PREFERENCE);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    EXPECT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(defaultVote, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[0].frameRateCategory);
}

// Tests VRR NoPreference-only vote, no game default override. Expects NoPreference, *not* vote
// reset.
TEST_F(LayerHistoryIntegrationTest, oneLayerCategoryNoPreference_vrr) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, false);
    SET_FLAG_FOR_TEST(flags::game_default_frame_rate, true);
    SET_FLAG_FOR_TEST(flags::vrr_config, true);
    mSelector->setActiveMode(kVrrModeId, HI_FPS);

    const LayerHistory::LayerVoteType defaultVote = LayerHistory::LayerVoteType::Min;

    auto layer = createLegacyAndFrontedEndLayer(1);
    setDefaultLayerVote(layer.get(), defaultVote);
    showLayer(1);
    setFrameRate(1, (0_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_NO_PREFERENCE);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    EXPECT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitCategory, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::NoPreference, summarizeLayerHistory(time)[0].frameRateCategory);
}

TEST_F(LayerHistoryIntegrationTest, oneLayerCategoryNoPreferenceWithGameDefault_vrr) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, false);
    SET_FLAG_FOR_TEST(flags::game_default_frame_rate, true);
    SET_FLAG_FOR_TEST(flags::vrr_config, true);
    mSelector->setActiveMode(kVrrModeId, HI_FPS);

    const Fps gameDefaultFrameRate = Fps::fromValue(30.0f);
    const uid_t uid = 456;

    history().updateGameDefaultFrameRateOverride(
            FrameRateOverride({uid, gameDefaultFrameRate.getValue()}));

    auto layer = createLegacyAndFrontedEndLayerWithUid(1, gui::Uid(uid));
    showLayer(1);
    setFrameRate(1, (0_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_NO_PREFERENCE);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    EXPECT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(gameDefaultFrameRate, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[0].frameRateCategory);
}

TEST_F(LayerHistoryIntegrationTest, oneLayerCategoryNoPreferenceWithGameDefault_mrr) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, false);
    SET_FLAG_FOR_TEST(flags::game_default_frame_rate, true);
    SET_FLAG_FOR_TEST(flags::vrr_config, true);

    const Fps gameDefaultFrameRate = Fps::fromValue(30.0f);
    const uid_t uid = 456;

    history().updateGameDefaultFrameRateOverride(
            FrameRateOverride({uid, gameDefaultFrameRate.getValue()}));

    auto layer = createLegacyAndFrontedEndLayerWithUid(1, gui::Uid(uid));
    showLayer(1);
    setFrameRate(1, (0_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_NO_PREFERENCE);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    EXPECT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(gameDefaultFrameRate, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[0].frameRateCategory);
}

TEST_F(LayerHistoryIntegrationTest, oneLayerNoVoteWithGameDefault_vrr) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, false);
    SET_FLAG_FOR_TEST(flags::game_default_frame_rate, true);
    SET_FLAG_FOR_TEST(flags::vrr_config, true);
    mSelector->setActiveMode(kVrrModeId, HI_FPS);

    const Fps gameDefaultFrameRate = Fps::fromValue(30.0f);
    const uid_t uid = 456;

    history().updateGameDefaultFrameRateOverride(
            FrameRateOverride({uid, gameDefaultFrameRate.getValue()}));

    auto layer = createLegacyAndFrontedEndLayerWithUid(1, gui::Uid(uid));
    showLayer(1);
    setFrameRate(1, (0_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_NO_VOTE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    // Expect NoVote to be skipped in summarize.
    EXPECT_EQ(0u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerNoVoteWithGameDefault_mrr) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, false);
    SET_FLAG_FOR_TEST(flags::game_default_frame_rate, true);
    SET_FLAG_FOR_TEST(flags::vrr_config, true);

    const Fps gameDefaultFrameRate = Fps::fromValue(30.0f);
    const uid_t uid = 456;

    history().updateGameDefaultFrameRateOverride(
            FrameRateOverride({uid, gameDefaultFrameRate.getValue()}));

    auto layer = createLegacyAndFrontedEndLayerWithUid(1, gui::Uid(uid));
    showLayer(1);
    setFrameRate(1, (0_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_NO_VOTE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    // Expect NoVote to be skipped in summarize.
    EXPECT_EQ(0u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitVoteWithCategory) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, true);

    auto layer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    setFrameRate(1, (73.4_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_HIGH);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    // There are 2 LayerRequirement's due to the frame rate category.
    ASSERT_EQ(2u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    // First LayerRequirement is the layer's category specification
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitCategory, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::High, summarizeLayerHistory(time)[0].frameRateCategory);

    // Second LayerRequirement is the frame rate specification
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summarizeLayerHistory(time)[1].vote);
    EXPECT_EQ(73.4_Hz, summarizeLayerHistory(time)[1].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[1].frameRateCategory);

    // layer became infrequent, but the vote stays
    setDefaultLayerVote(layer.get(), LayerHistory::LayerVoteType::Heuristic);
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_EQ(2u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitCategory, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::High, summarizeLayerHistory(time)[0].frameRateCategory);
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitVoteWithCategoryNotVisibleDoesNotVote) {
    SET_FLAG_FOR_TEST(flags::misc1, true);

    auto layer = createLegacyAndFrontedEndLayer(1);
    hideLayer(1);
    setFrameRate(1, (12.34_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_HIGH);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    // Layer is not visible, so the layer is moved to inactive, infrequent, and it will not have
    // votes to consider for refresh rate selection.
    ASSERT_EQ(0u, summarizeLayerHistory(time).size());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, invisibleExplicitLayer) {
    SET_FLAG_FOR_TEST(flags::misc1, false);

    auto explicitVisiblelayer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    setFrameRate(1, (60_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, 0);

    auto explicitInvisiblelayer = createLegacyAndFrontedEndLayer(2);
    hideLayer(2);
    setFrameRate(2, (90_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, 0);

    nsecs_t time = systemTime();

    // Post a buffer to the layers to make them active
    setBuffer(1);
    setBuffer(2);
    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_EQ(2u, layerCount());
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(60_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(2, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, invisibleExplicitLayerDoesNotVote) {
    SET_FLAG_FOR_TEST(flags::misc1, true);

    auto explicitVisiblelayer = createLegacyAndFrontedEndLayer(1);
    showLayer(1);
    setFrameRate(1, (60_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, 0);

    auto explicitInvisiblelayer = createLegacyAndFrontedEndLayer(2);
    hideLayer(2);
    setFrameRate(2, (90_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, 0);

    nsecs_t time = systemTime();

    // Post a buffer to the layers to make them active
    setBuffer(1);
    setBuffer(2);
    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_EQ(2u, layerCount());
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(60_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, frontBufferedLayerVotesMax) {
    SET_FLAG_FOR_TEST(flags::vrr_config, true);

    auto layer = createLegacyAndFrontedEndLayer(1);
    setFrontBuffer(1);
    showLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // layer is active but infrequent.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setFrontBufferWithPresentTime(layer, time);
        time += MAX_FREQUENT_LAYER_PERIOD_NS.count();
    }

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // Layer still active due to front buffering, but it's infrequent.
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitCategory) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, true);

    createLegacyAndFrontedEndLayer(1);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_HIGH);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    // First LayerRequirement is the frame rate specification
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitCategory, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::High, summarizeLayerHistory(time)[0].frameRateCategory);
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitVoteWithFixedSourceAndNoPreferenceCategory) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, false);

    auto layer = createLegacyAndFrontedEndLayer(1);
    setFrameRate(1, (45.6_Hz).getValue(), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_NO_PREFERENCE);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
    }

    // There are 2 LayerRequirement's due to the frame rate category.
    ASSERT_EQ(2u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    // First LayerRequirement is the layer's category specification
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitCategory, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::NoPreference, summarizeLayerHistory(time)[0].frameRateCategory);

    // Second LayerRequirement is the frame rate specification
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[1].vote);
    EXPECT_EQ(45.6_Hz, summarizeLayerHistory(time)[1].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::Default, summarizeLayerHistory(time)[1].frameRateCategory);

    // layer became infrequent, but the vote stays
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();
    ASSERT_EQ(2u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitCategory, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::NoPreference, summarizeLayerHistory(time)[0].frameRateCategory);
}

TEST_F(LayerHistoryIntegrationTest, multipleLayers) {
    auto layer1 = createLegacyAndFrontedEndLayer(1);
    auto layer2 = createLegacyAndFrontedEndLayer(2);
    auto layer3 = createLegacyAndFrontedEndLayer(3);

    nsecs_t time = systemTime();

    EXPECT_EQ(3u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    LayerHistory::Summary summary;

    // layer1 is active but infrequent.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer1, time);
        time += MAX_FREQUENT_LAYER_PERIOD_NS.count();
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summary[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    // layer2 is frequent and has high refresh rate.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer2, time);
        time += HI_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    // layer1 is still active but infrequent.
    setBufferWithPresentTime(layer1, time);

    ASSERT_EQ(2u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summary[0].vote);
    ASSERT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[1].vote);
    EXPECT_EQ(HI_FPS, summarizeLayerHistory(time)[1].desiredRefreshRate);

    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer1 is no longer active.
    // layer2 is frequent and has low refresh rate.
    for (size_t i = 0; i < 2 * PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer2, time);
        time += LO_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer2 still has low refresh rate.
    // layer3 has high refresh rate but not enough history.
    constexpr int RATIO = LO_FPS_PERIOD / HI_FPS_PERIOD;
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE - 1; i++) {
        if (i % RATIO == 0) {
            setBufferWithPresentTime(layer2, time);
        }

        setBufferWithPresentTime(layer3, time);
        time += HI_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(2u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summary[1].vote);
    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(2, frequentLayerCount(time));

    // layer3 becomes recently active.
    setBufferWithPresentTime(layer3, time);
    summary = summarizeLayerHistory(time);
    ASSERT_EQ(2u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[1].vote);
    EXPECT_EQ(HI_FPS, summary[1].desiredRefreshRate);
    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(2, frequentLayerCount(time));

    // layer1 expires.
    destroyLayer(layer1);
    updateLayerSnapshotsAndLayerHistory(time);

    summary = summarizeLayerHistory(time);
    ASSERT_EQ(2u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[1].vote);
    EXPECT_EQ(HI_FPS, summary[1].desiredRefreshRate);
    EXPECT_EQ(2u, layerCount());
    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(2, frequentLayerCount(time));

    // layer2 still has low refresh rate.
    // layer3 becomes inactive.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer2, time);
        time += LO_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer2 expires.
    destroyLayer(layer2);
    updateLayerSnapshotsAndLayerHistory(time);
    summary = summarizeLayerHistory(time);
    EXPECT_TRUE(summary.empty());
    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    // layer3 becomes active and has high refresh rate.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE + FREQUENT_LAYER_WINDOW_SIZE + 1; i++) {
        setBufferWithPresentTime(layer3, time);
        time += HI_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(HI_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer3 expires.
    destroyLayer(layer3);
    updateLayerSnapshotsAndLayerHistory(time);
    summary = summarizeLayerHistory(time);
    EXPECT_TRUE(summary.empty());
    EXPECT_EQ(0u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, inactiveLayers) {
    auto layer = createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();

    // the very first updates makes the layer frequent
    for (size_t i = 0; i < FREQUENT_LAYER_WINDOW_SIZE - 1; i++) {
        setBufferWithPresentTime(layer, time);
        time += MAX_FREQUENT_LAYER_PERIOD_NS.count();

        EXPECT_EQ(1u, layerCount());
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        EXPECT_EQ(1, frequentLayerCount(time));
    }

    // the next update with the MAX_FREQUENT_LAYER_PERIOD_NS will get us to infrequent
    time += MAX_FREQUENT_LAYER_PERIOD_NS.count();
    setBufferWithPresentTime(layer, time);

    EXPECT_EQ(1u, layerCount());
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    // advance the time for the previous frame to be inactive
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();

    // Now even if we post a quick few frame we should stay infrequent
    for (size_t i = 0; i < FREQUENT_LAYER_WINDOW_SIZE - 1; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;

        EXPECT_EQ(1u, layerCount());
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        EXPECT_EQ(0, frequentLayerCount(time));
    }

    // More quick frames will get us to frequent again
    setBufferWithPresentTime(layer, time);
    time += HI_FPS_PERIOD;

    EXPECT_EQ(1u, layerCount());
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, invisibleExplicitLayerIsActive) {
    SET_FLAG_FOR_TEST(flags::misc1, false);

    auto explicitVisiblelayer = createLegacyAndFrontedEndLayer(1);
    auto explicitInvisiblelayer = createLegacyAndFrontedEndLayer(2);
    hideLayer(2);
    setFrameRate(1, 60.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRate(2, 90.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    nsecs_t time = systemTime();

    // Post a buffer to the layers to make them active
    setBufferWithPresentTime(explicitVisiblelayer, time);
    setBufferWithPresentTime(explicitInvisiblelayer, time);

    EXPECT_EQ(2u, layerCount());
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(60_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(2, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, invisibleExplicitLayerIsNotActive) {
    SET_FLAG_FOR_TEST(flags::misc1, true);

    auto explicitVisiblelayer = createLegacyAndFrontedEndLayer(1);
    auto explicitInvisiblelayer = createLegacyAndFrontedEndLayer(2);
    hideLayer(2);
    setFrameRate(1, 60.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRate(2, 90.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    nsecs_t time = systemTime();

    // Post a buffer to the layers to make them active
    setBufferWithPresentTime(explicitVisiblelayer, time);
    setBufferWithPresentTime(explicitInvisiblelayer, time);

    EXPECT_EQ(2u, layerCount());
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(60_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, infrequentAnimatingLayer) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // layer is active but infrequent.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += MAX_FREQUENT_LAYER_PERIOD_NS.count();
    }

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // another update with the same cadence keep in infrequent
    setBufferWithPresentTime(layer, time);
    time += MAX_FREQUENT_LAYER_PERIOD_NS.count();

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    mFlinger.mutableLayerSnapshotBuilder().getSnapshot(1)->changes |=
            frontend::RequestedLayerState::Changes::Animation;
    mFlinger.updateLayerHistory(time);
    // an update as animation will immediately vote for Max
    time += MAX_FREQUENT_LAYER_PERIOD_NS.count();

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(1, animatingLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, frequentLayerBecomingInfrequentAndBack) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // Fill up the window with frequent updates
    for (size_t i = 0; i < FREQUENT_LAYER_WINDOW_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += (60_Hz).getPeriodNsecs();

        EXPECT_EQ(1u, layerCount());
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        EXPECT_EQ(1, frequentLayerCount(time));
    }

    // posting a buffer after long inactivity should retain the layer as active
    time += std::chrono::nanoseconds(3s).count();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(60_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting more infrequent buffer should make the layer infrequent
    time += (MAX_FREQUENT_LAYER_PERIOD_NS + 1ms).count();
    setBufferWithPresentTime(layer, time);
    time += (MAX_FREQUENT_LAYER_PERIOD_NS + 1ms).count();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting another buffer should keep the layer infrequent
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting more buffers would mean starting of an animation, so making the layer frequent
    setBufferWithPresentTime(layer, time);
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(1, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting a buffer after long inactivity should retain the layer as active
    time += std::chrono::nanoseconds(3s).count();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting another buffer should keep the layer frequent
    time += (60_Hz).getPeriodNsecs();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, inconclusiveLayerBecomingFrequent) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // Fill up the window with frequent updates
    for (size_t i = 0; i < FREQUENT_LAYER_WINDOW_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += (60_Hz).getPeriodNsecs();

        EXPECT_EQ(1u, layerCount());
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        EXPECT_EQ(1, frequentLayerCount(time));
    }

    // posting infrequent buffers after long inactivity should make the layer
    // inconclusive but frequent.
    time += std::chrono::nanoseconds(3s).count();
    setBufferWithPresentTime(layer, time);
    time += (MAX_FREQUENT_LAYER_PERIOD_NS + 1ms).count();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting more buffers should make the layer frequent and switch the refresh rate to max
    // by clearing the history
    setBufferWithPresentTime(layer, time);
    setBufferWithPresentTime(layer, time);
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(1, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, getFramerate) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // layer is active but infrequent.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += MAX_FREQUENT_LAYER_PERIOD_NS.count();
    }

    float expectedFramerate = 1e9f / MAX_FREQUENT_LAYER_PERIOD_NS.count();
    EXPECT_FLOAT_EQ(expectedFramerate, history().getLayerFramerate(time, layer->getSequence()));
}

TEST_F(LayerHistoryIntegrationTest, heuristicLayer60Hz) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();
    for (float fps = 54.0f; fps < 65.0f; fps += 0.1f) {
        recordFramesAndExpect(layer, time, Fps::fromValue(fps), 60_Hz, PRESENT_TIME_HISTORY_SIZE);
    }
}

TEST_F(LayerHistoryIntegrationTest, heuristicLayer60_30Hz) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();
    recordFramesAndExpect(layer, time, 60_Hz, 60_Hz, PRESENT_TIME_HISTORY_SIZE);

    recordFramesAndExpect(layer, time, 60_Hz, 60_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 30_Hz, 60_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 30_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 60_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 60_Hz, 60_Hz, PRESENT_TIME_HISTORY_SIZE);
}

TEST_F(LayerHistoryIntegrationTest, heuristicLayerNotOscillating) {
    SET_FLAG_FOR_TEST(flags::use_known_refresh_rate_for_fps_consistency, false);
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    recordFramesAndExpect(layer, time, 27.1_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 26.9_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 26_Hz, 24_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 26.9_Hz, 24_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 27.1_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
}

TEST_F(LayerHistoryIntegrationTest, heuristicLayerNotOscillating_useKnownRefreshRate) {
    SET_FLAG_FOR_TEST(flags::use_known_refresh_rate_for_fps_consistency, true);
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    recordFramesAndExpect(layer, time, 27.1_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 26.9_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 26_Hz, 24_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 26.9_Hz, 24_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 27.1_Hz, 24_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 27.1_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
}

TEST_F(LayerHistoryIntegrationTest, smallDirtyLayer) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    LayerHistory::Summary summary;

    // layer is active but infrequent.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        scheduler::LayerProps props = {
                .visible = false,
                .bounds = {0, 0, 100, 100},
                .transform = {},
                .setFrameRateVote = {},
                .frameRateSelectionPriority = Layer::PRIORITY_UNSET,
                .isSmallDirty = false,
                .isFrontBuffered = false,
        };
        if (i % 3 == 0) {
            props.isSmallDirty = false;
        } else {
            props.isSmallDirty = true;
        }

        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    ASSERT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_GE(HI_FPS, summary[0].desiredRefreshRate);
}

TEST_F(LayerHistoryIntegrationTest, DISABLED_smallDirtyInMultiLayer) {
    auto uiLayer = createLegacyAndFrontedEndLayer(1);
    auto videoLayer = createLegacyAndFrontedEndLayer(2);
    setFrameRate(2, 30.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    nsecs_t time = systemTime();

    EXPECT_EQ(2u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    LayerHistory::Summary summary;

    // uiLayer is updating small dirty.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE + FREQUENT_LAYER_WINDOW_SIZE + 1; i++) {
        scheduler::LayerProps props = {
                .visible = false,
                .bounds = {0, 0, 100, 100},
                .transform = {},
                .setFrameRateVote = {},
                .frameRateSelectionPriority = Layer::PRIORITY_UNSET,
                .isSmallDirty = true,
                .isFrontBuffered = false,
        };
        setBuffer(1);
        uiLayer->setDesiredPresentTime(0, false /*autotimestamp*/);
        updateLayerSnapshotsAndLayerHistory(time);
        setBufferWithPresentTime(videoLayer, time);
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    ASSERT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summary[0].vote);
    ASSERT_EQ(30_Hz, summary[0].desiredRefreshRate);
}

TEST_F(LayerHistoryIntegrationTest, hidingLayerUpdatesLayerHistory) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    setBuffer(1);
    updateLayerSnapshotsAndLayerHistory(time);
    auto summary = summarizeLayerHistory(time);
    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summary[0].vote);
    EXPECT_EQ(1u, activeLayerCount());

    hideLayer(1);
    updateLayerSnapshotsAndLayerHistory(time);

    summary = summarizeLayerHistory(time);
    EXPECT_TRUE(summary.empty());
    EXPECT_EQ(0u, activeLayerCount());
}

TEST_F(LayerHistoryIntegrationTest, showingLayerUpdatesLayerHistory) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    hideLayer(1);
    setBuffer(1);
    updateLayerSnapshotsAndLayerHistory(time);
    auto summary = summarizeLayerHistory(time);
    EXPECT_TRUE(summary.empty());
    EXPECT_EQ(0u, activeLayerCount());

    showLayer(1);
    updateLayerSnapshotsAndLayerHistory(time);

    summary = summarizeLayerHistory(time);
    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summary[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
}

TEST_F(LayerHistoryIntegrationTest, updatingGeometryUpdatesWeight) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    setBuffer(1,
              std::make_shared<
                      renderengine::mock::FakeExternalTexture>(100U /*width*/, 100U /*height*/, 1,
                                                               HAL_PIXEL_FORMAT_RGBA_8888,
                                                               GRALLOC_USAGE_PROTECTED /*usage*/));
    mFlinger.setLayerHistoryDisplayArea(100 * 100);
    updateLayerSnapshotsAndLayerHistory(time);
    auto summary = summarizeLayerHistory(time);
    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summary[0].vote);
    EXPECT_EQ(1u, activeLayerCount());

    auto startingWeight = summary[0].weight;

    setMatrix(1, 0.1f, 0.f, 0.f, 0.1f);
    updateLayerSnapshotsAndLayerHistory(time);

    summary = summarizeLayerHistory(time);
    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summary[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_GT(startingWeight, summary[0].weight);
}

class LayerHistoryIntegrationTestParameterized
      : public LayerHistoryIntegrationTest,
        public testing::WithParamInterface<std::chrono::nanoseconds> {};

TEST_P(LayerHistoryIntegrationTestParameterized, HeuristicLayerWithInfrequentLayer) {
    std::chrono::nanoseconds infrequentUpdateDelta = GetParam();
    auto heuristicLayer = createLegacyAndFrontedEndLayer(1);
    auto infrequentLayer = createLegacyAndFrontedEndLayer(2);

    const nsecs_t startTime = systemTime();

    const std::chrono::nanoseconds heuristicUpdateDelta = 41'666'667ns;
    setBufferWithPresentTime(heuristicLayer, startTime);
    setBufferWithPresentTime(infrequentLayer, startTime);

    nsecs_t time = startTime;
    nsecs_t lastInfrequentUpdate = startTime;
    const size_t totalInfrequentLayerUpdates = FREQUENT_LAYER_WINDOW_SIZE * 5;
    size_t infrequentLayerUpdates = 0;
    while (infrequentLayerUpdates <= totalInfrequentLayerUpdates) {
        time += heuristicUpdateDelta.count();
        setBufferWithPresentTime(heuristicLayer, time);

        if (time - lastInfrequentUpdate >= infrequentUpdateDelta.count()) {
            ALOGI("submitting infrequent frame [%zu/%zu]", infrequentLayerUpdates,
                  totalInfrequentLayerUpdates);
            lastInfrequentUpdate = time;
            setBufferWithPresentTime(infrequentLayer, time);
            infrequentLayerUpdates++;
        }

        if (time - startTime > PRESENT_TIME_HISTORY_DURATION.count()) {
            ASSERT_NE(0u, summarizeLayerHistory(time).size());
            ASSERT_GE(2u, summarizeLayerHistory(time).size());

            bool max = false;
            bool min = false;
            Fps heuristic;
            for (const auto& layer : summarizeLayerHistory(time)) {
                if (layer.vote == LayerHistory::LayerVoteType::Heuristic) {
                    heuristic = layer.desiredRefreshRate;
                } else if (layer.vote == LayerHistory::LayerVoteType::Max) {
                    max = true;
                } else if (layer.vote == LayerHistory::LayerVoteType::Min) {
                    min = true;
                }
            }

            if (infrequentLayerUpdates > FREQUENT_LAYER_WINDOW_SIZE) {
                EXPECT_EQ(24_Hz, heuristic);
                EXPECT_FALSE(max);
                if (summarizeLayerHistory(time).size() == 2) {
                    EXPECT_TRUE(min);
                }
            }
        }
    }
}

class SmallAreaDetectionTest : public LayerHistoryIntegrationTest {
protected:
    static constexpr int32_t DISPLAY_WIDTH = 100;
    static constexpr int32_t DISPLAY_HEIGHT = 100;

    static constexpr int32_t kAppId1 = 10100;
    static constexpr int32_t kAppId2 = 10101;

    static constexpr float kThreshold1 = 0.05f;
    static constexpr float kThreshold2 = 0.07f;

    SmallAreaDetectionTest() : LayerHistoryIntegrationTest() {
        std::vector<std::pair<int32_t, float>> mappings;
        mappings.reserve(2);
        mappings.push_back(std::make_pair(kAppId1, kThreshold1));
        mappings.push_back(std::make_pair(kAppId2, kThreshold2));

        mScheduler->onActiveDisplayAreaChanged(DISPLAY_WIDTH * DISPLAY_HEIGHT);
        mScheduler->updateSmallAreaDetection(mappings);
    }

    auto createLegacyAndFrontedEndLayer(uint32_t sequence) {
        std::string layerName = "test layer:" + std::to_string(sequence);

        LayerCreationArgs args = LayerCreationArgs{mFlinger.flinger(),
                                                   nullptr,
                                                   layerName,
                                                   0,
                                                   {},
                                                   std::make_optional<uint32_t>(sequence)};
        args.ownerUid = kAppId1;
        args.metadata.setInt32(gui::METADATA_WINDOW_TYPE, 2); // APPLICATION
        const auto layer = sp<Layer>::make(args);
        mFlinger.injectLegacyLayer(layer);
        createRootLayer(sequence);
        return layer;
    }
};

TEST_F(SmallAreaDetectionTest, SmallDirtyLayer) {
    SET_FLAG_FOR_TEST(flags::enable_small_area_detection, true);
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    uint32_t sequence = static_cast<uint32_t>(layer->sequence);
    setBuffer(sequence);
    setDamageRegion(sequence, Region(Rect(10, 10)));
    updateLayerSnapshotsAndLayerHistory(time);

    ASSERT_EQ(true, mFlinger.mutableLayerSnapshotBuilder().getSnapshot(1)->isSmallDirty);
}

TEST_F(SmallAreaDetectionTest, NotSmallDirtyLayer) {
    SET_FLAG_FOR_TEST(flags::enable_small_area_detection, true);
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    uint32_t sequence = static_cast<uint32_t>(layer->sequence);
    setBuffer(sequence);
    setDamageRegion(sequence, Region(Rect(50, 50)));
    updateLayerSnapshotsAndLayerHistory(time);

    ASSERT_EQ(false, mFlinger.mutableLayerSnapshotBuilder().getSnapshot(1)->isSmallDirty);
}

TEST_F(SmallAreaDetectionTest, smallDirtyLayerWithMatrix) {
    SET_FLAG_FOR_TEST(flags::enable_small_area_detection, true);
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    // Original damage region is a small dirty.
    uint32_t sequence = static_cast<uint32_t>(layer->sequence);
    setBuffer(sequence);
    setDamageRegion(sequence, Region(Rect(20, 20)));
    updateLayerSnapshotsAndLayerHistory(time);
    ASSERT_EQ(true, mFlinger.mutableLayerSnapshotBuilder().getSnapshot(1)->isSmallDirty);

    setMatrix(sequence, 2.0f, 0, 0, 2.0f);
    updateLayerSnapshotsAndLayerHistory(time);

    // Verify if the small dirty is scaled.
    ASSERT_EQ(false, mFlinger.mutableLayerSnapshotBuilder().getSnapshot(1)->isSmallDirty);
}

INSTANTIATE_TEST_CASE_P(LeapYearTests, LayerHistoryIntegrationTestParameterized,
                        ::testing::Values(1s, 2s, 3s, 4s, 5s));

} // namespace
} // namespace android::scheduler
