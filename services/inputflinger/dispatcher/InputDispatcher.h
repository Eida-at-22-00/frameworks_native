/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <com_android_input_flags.h>

#include "AnrTracker.h"
#include "CancelationOptions.h"
#include "DragState.h"
#include "Entry.h"
#include "FocusResolver.h"
#include "InjectionState.h"
#include "InputDispatcherConfiguration.h"
#include "InputDispatcherInterface.h"
#include "InputDispatcherPolicyInterface.h"
#include "InputTarget.h"
#include "InputThread.h"
#include "LatencyAggregator.h"
#include "LatencyAggregatorWithHistograms.h"
#include "LatencyTracker.h"
#include "Monitor.h"
#include "TouchState.h"
#include "TouchedWindow.h"
#include "trace/InputTracerInterface.h"
#include "trace/InputTracingBackendInterface.h"

#include <attestation/HmacKeyManager.h>
#include <gui/InputApplication.h>
#include <gui/WindowInfosUpdate.h>
#include <input/Input.h>
#include <input/InputTransport.h>
#include <limits.h>
#include <powermanager/PowerManager.h>
#include <stddef.h>
#include <unistd.h>
#include <utils/BitSet.h>
#include <utils/Looper.h>
#include <utils/Timers.h>
#include <utils/threads.h>
#include <bitset>
#include <condition_variable>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <InputListener.h>
#include <InputReporterInterface.h>
#include <gui/WindowInfosListener.h>

namespace android::inputdispatcher {

class Connection;

/* Dispatches events to input targets.  Some functions of the input dispatcher, such as
 * identifying input targets, are controlled by a separate policy object.
 *
 * IMPORTANT INVARIANT:
 *     Because the policy can potentially block or cause re-entrance into the input dispatcher,
 *     the input dispatcher never calls into the policy while holding its internal locks.
 *     The implementation is also carefully designed to recover from scenarios such as an
 *     input channel becoming unregistered while identifying input targets or processing timeouts.
 *
 *     Methods marked 'Locked' must be called with the lock acquired.
 *
 *     Methods marked 'LockedInterruptible' must be called with the lock acquired but
 *     may during the course of their execution release the lock, call into the policy, and
 *     then reacquire the lock.  The caller is responsible for recovering gracefully.
 *
 *     A 'LockedInterruptible' method may called a 'Locked' method, but NOT vice-versa.
 */
class InputDispatcher : public android::InputDispatcherInterface {
public:
    static constexpr bool kDefaultInTouchMode = true;

    explicit InputDispatcher(InputDispatcherPolicyInterface& policy);
    // Constructor used for testing.
    explicit InputDispatcher(InputDispatcherPolicyInterface&,
                             std::unique_ptr<trace::InputTracingBackendInterface>);
    ~InputDispatcher() override;

    void dump(std::string& dump) const override;
    void monitor() override;
    bool waitForIdle() const override;
    status_t start() override;
    status_t stop() override;

    void notifyInputDevicesChanged(const NotifyInputDevicesChangedArgs& args) override;
    void notifyKey(const NotifyKeyArgs& args) override;
    void notifyMotion(const NotifyMotionArgs& args) override;
    void notifySwitch(const NotifySwitchArgs& args) override;
    void notifySensor(const NotifySensorArgs& args) override;
    void notifyVibratorState(const NotifyVibratorStateArgs& args) override;
    void notifyDeviceReset(const NotifyDeviceResetArgs& args) override;
    void notifyPointerCaptureChanged(const NotifyPointerCaptureChangedArgs& args) override;

    android::os::InputEventInjectionResult injectInputEvent(
            const InputEvent* event, std::optional<gui::Uid> targetUid,
            android::os::InputEventInjectionSync syncMode, std::chrono::milliseconds timeout,
            uint32_t policyFlags) override;

    std::unique_ptr<VerifiedInputEvent> verifyInputEvent(const InputEvent& event) override;

    void setFocusedApplication(
            ui::LogicalDisplayId displayId,
            const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle) override;
    void setFocusedDisplay(ui::LogicalDisplayId displayId) override;
    void setMinTimeBetweenUserActivityPokes(std::chrono::milliseconds interval) override;
    void setInputDispatchMode(bool enabled, bool frozen) override;
    void setInputFilterEnabled(bool enabled) override;
    bool setInTouchMode(bool inTouchMode, gui::Pid pid, gui::Uid uid, bool hasPermission,
                        ui::LogicalDisplayId displayId) override;
    void setMaximumObscuringOpacityForTouch(float opacity) override;

    bool transferTouchGesture(const sp<IBinder>& fromToken, const sp<IBinder>& toToken,
                              bool isDragDrop, bool transferEntireGesture) override;
    bool transferTouchOnDisplay(const sp<IBinder>& destChannelToken,
                                ui::LogicalDisplayId displayId) override;

    base::Result<std::unique_ptr<InputChannel>> createInputChannel(
            const std::string& name) override;
    void setFocusedWindow(const android::gui::FocusRequest&) override;
    base::Result<std::unique_ptr<InputChannel>> createInputMonitor(ui::LogicalDisplayId displayId,
                                                                   const std::string& name,
                                                                   gui::Pid pid) override;
    status_t removeInputChannel(const sp<IBinder>& connectionToken) override;
    status_t pilferPointers(const sp<IBinder>& token) override;
    void requestPointerCapture(const sp<IBinder>& windowToken, bool enabled) override;
    bool flushSensor(int deviceId, InputDeviceSensorType sensorType) override;
    void setDisplayEligibilityForPointerCapture(ui::LogicalDisplayId displayId,
                                                bool isEligible) override;

    std::array<uint8_t, 32> sign(const VerifiedInputEvent& event) const;

    void displayRemoved(ui::LogicalDisplayId displayId) override;

    // Public because it's also used by tests to simulate the WindowInfosListener callback
    void onWindowInfosChanged(const gui::WindowInfosUpdate&);

    void cancelCurrentTouch() override;

    // Public to allow tests to verify that a Monitor can get ANR.
    void setMonitorDispatchingTimeoutForTest(std::chrono::nanoseconds timeout);

    void setKeyRepeatConfiguration(std::chrono::nanoseconds timeout, std::chrono::nanoseconds delay,
                                   bool keyRepeatEnabled) override;

    bool isPointerInWindow(const sp<IBinder>& token, ui::LogicalDisplayId displayId,
                           DeviceId deviceId, int32_t pointerId) override;

    void setInputMethodConnectionIsActive(bool isActive) override;

    void setDisplayTopology(const DisplayTopologyGraph& displayTopologyGraph) override;

private:
    enum class DropReason {
        NOT_DROPPED,
        POLICY,
        DISABLED,
        BLOCKED,
        STALE,
        NO_POINTER_CAPTURE,
    };

    std::unique_ptr<InputThread> mThread;

    InputDispatcherPolicyInterface& mPolicy;
    android::InputDispatcherConfiguration mConfig GUARDED_BY(mLock);

    mutable std::mutex mLock;

    std::condition_variable mDispatcherIsAlive;
    mutable std::condition_variable mDispatcherEnteredIdle;

    // Input event tracer. The tracer will only exist on builds where input tracing is allowed.
    std::unique_ptr<trace::InputTracerInterface> mTracer GUARDED_BY(mLock);

    sp<Looper> mLooper;

    std::shared_ptr<const EventEntry> mPendingEvent GUARDED_BY(mLock);
    std::deque<std::shared_ptr<const EventEntry>> mInboundQueue GUARDED_BY(mLock);
    std::deque<std::shared_ptr<const EventEntry>> mRecentQueue GUARDED_BY(mLock);

    // A command entry captures state and behavior for an action to be performed in the
    // dispatch loop after the initial processing has taken place.  It is essentially
    // a kind of continuation used to postpone sensitive policy interactions to a point
    // in the dispatch loop where it is safe to release the lock (generally after finishing
    // the critical parts of the dispatch cycle).
    //
    // The special thing about commands is that they can voluntarily release and reacquire
    // the dispatcher lock at will.  Initially when the command starts running, the
    // dispatcher lock is held.  However, if the command needs to call into the policy to
    // do some work, it can release the lock, do the work, then reacquire the lock again
    // before returning.
    //
    // This mechanism is a bit clunky but it helps to preserve the invariant that the dispatch
    // never calls into the policy while holding its lock.
    //
    // Commands are called with the lock held, but they can release and re-acquire the lock from
    // within.
    using Command = std::function<void()>;
    std::deque<Command> mCommandQueue GUARDED_BY(mLock);

    DropReason mLastDropReason GUARDED_BY(mLock);

    const IdGenerator mIdGenerator GUARDED_BY(mLock);

    int64_t mWindowInfosVsyncId GUARDED_BY(mLock);

    std::chrono::milliseconds mMinTimeBetweenUserActivityPokes GUARDED_BY(mLock);

    /** Stores the latest user-activity poke event times per user activity types. */
    std::array<nsecs_t, USER_ACTIVITY_EVENT_LAST + 1> mLastUserActivityTimes GUARDED_BY(mLock);

    template <typename T>
    struct StrongPointerHash {
        std::size_t operator()(const sp<T>& b) const { return std::hash<T*>{}(b.get()); }
    };

    class ConnectionManager {
    public:
        ConnectionManager(const sp<Looper>& lopper);
        ~ConnectionManager();

        std::shared_ptr<Connection> getConnection(const sp<IBinder>& inputConnectionToken) const;

        // Find a monitor pid by the provided token.
        std::optional<gui::Pid> findMonitorPidByToken(const sp<IBinder>& token) const;
        void forEachGlobalMonitorConnection(
                std::function<void(const std::shared_ptr<Connection>&)> f) const;
        void forEachGlobalMonitorConnection(
                ui::LogicalDisplayId displayId,
                std::function<void(const std::shared_ptr<Connection>&)> f) const;

        void createGlobalInputMonitor(ui::LogicalDisplayId displayId,
                                      std::unique_ptr<InputChannel>&& inputChannel,
                                      const IdGenerator& idGenerator, gui::Pid pid,
                                      std::function<int(int)> callback);

        status_t removeConnection(const std::shared_ptr<Connection>& connection);

        void createConnection(std::unique_ptr<InputChannel>&& inputChannel,
                              const IdGenerator& idGenerator, std::function<int(int)> callback);

        std::string dump(nsecs_t currentTime) const;

    private:
        const sp<Looper> mLooper;

        // All registered connections mapped by input channel token.
        std::unordered_map<sp<IBinder>, std::shared_ptr<Connection>, StrongPointerHash<IBinder>>
                mConnectionsByToken;

        // Input channels that will receive a copy of all input events sent to the provided display.
        std::unordered_map<ui::LogicalDisplayId, std::vector<Monitor>> mGlobalMonitorsByDisplay;

        void removeMonitorChannel(const sp<IBinder>& connectionToken);
    };

    ConnectionManager mConnectionManager GUARDED_BY(mLock);

    class DispatcherWindowInfo {
    public:
        struct TouchOcclusionInfo {
            bool hasBlockingOcclusion;
            float obscuringOpacity;
            std::string obscuringPackage;
            gui::Uid obscuringUid = gui::Uid::INVALID;
            std::vector<std::string> debugInfo;
        };

        void setWindowHandlesForDisplay(
                ui::LogicalDisplayId displayId,
                std::vector<sp<android::gui::WindowInfoHandle>>&& windowHandles);

        void setDisplayInfos(const std::vector<android::gui::DisplayInfo>& displayInfos);

        void removeDisplay(ui::LogicalDisplayId displayId);

        void setMaximumObscuringOpacityForTouch(float opacity);

        void setDisplayTopology(const DisplayTopologyGraph& displayTopologyGraph);

        // Get a reference to window handles by display, return an empty vector if not found.
        const std::vector<sp<android::gui::WindowInfoHandle>>& getWindowHandlesForDisplay(
                ui::LogicalDisplayId displayId) const;

        void forEachWindowHandle(
                std::function<void(const sp<android::gui::WindowInfoHandle>&)> f) const;

        void forEachDisplayId(std::function<void(ui::LogicalDisplayId)> f) const;

        // Get the transform for display, returns Identity-transform if display is missing.
        ui::Transform getDisplayTransform(ui::LogicalDisplayId displayId) const;

        // Get the raw transform to use for motion events going to the given window. Optionally a
        // pointer displayId may be supplied if pointer is on a different display from the window.
        ui::Transform getRawTransform(
                const android::gui::WindowInfo& windowInfo,
                std::optional<ui::LogicalDisplayId> pointerDisplayId = std::nullopt) const;

        // Lookup for WindowInfoHandle from token and optionally a display-id. In cases where
        // display-id is not provided lookup is done for all displays.
        sp<android::gui::WindowInfoHandle> findWindowHandle(
                const sp<IBinder>& windowHandleToken,
                std::optional<ui::LogicalDisplayId> displayId = {}) const;

        // Lookup for WindowInfoHandle from token and a display-id. Lookup is done for all connected
        // displays in the topology of the queried display.
        sp<android::gui::WindowInfoHandle> findWindowHandleOnConnectedDisplays(
                const sp<IBinder>& windowHandleToken, ui::LogicalDisplayId displayId) const;

        bool isWindowPresent(const sp<android::gui::WindowInfoHandle>& windowHandle) const;

        // Returns the touched window at the given location, excluding the ignoreWindow if provided.
        sp<android::gui::WindowInfoHandle> findTouchedWindowAt(
                ui::LogicalDisplayId displayId, float x, float y, bool isStylus = false,
                const sp<android::gui::WindowInfoHandle> ignoreWindow = nullptr) const;

        TouchOcclusionInfo computeTouchOcclusionInfo(
                const sp<android::gui::WindowInfoHandle>& windowHandle, float x, float y) const;

        bool isWindowObscured(const sp<android::gui::WindowInfoHandle>& windowHandle) const;

        bool isWindowObscuredAtPoint(const sp<android::gui::WindowInfoHandle>& windowHandle,
                                     float x, float y) const;

        sp<android::gui::WindowInfoHandle> findWallpaperWindowBelow(
                const sp<android::gui::WindowInfoHandle>& windowHandle) const;

        bool isTouchTrusted(const TouchOcclusionInfo& occlusionInfo) const;

        // Returns topology's primary display if the display belongs to it, otherwise the
        // same displayId.
        ui::LogicalDisplayId getPrimaryDisplayId(ui::LogicalDisplayId displayId) const;

        bool areDisplaysConnected(ui::LogicalDisplayId display1,
                                  ui::LogicalDisplayId display2) const;

        std::string dumpDisplayAndWindowInfo() const;

    private:
        std::vector<ui::LogicalDisplayId> getConnectedDisplays(
                ui::LogicalDisplayId displayId) const;

        sp<android::gui::WindowInfoHandle> findWindowHandleOnDisplay(
                const sp<IBinder>& windowHandleToken, ui::LogicalDisplayId displayId) const;

        std::unordered_map<ui::LogicalDisplayId /*displayId*/,
                           std::vector<sp<android::gui::WindowInfoHandle>>>
                mWindowHandlesByDisplay;
        std::unordered_map<ui::LogicalDisplayId /*displayId*/, android::gui::DisplayInfo>
                mDisplayInfos;
        float mMaximumObscuringOpacityForTouch{1.0f};

        // Topology is initialized with default-constructed value, which is an empty topology until
        // we receive setDisplayTopology call. Meanwhile we will treat every display as an
        // independent display.
        DisplayTopologyGraph mTopology;
    };

    DispatcherWindowInfo mWindowInfos GUARDED_BY(mLock);

    class DispatcherTouchState {
    public:
        struct CancellationArgs {
            const sp<gui::WindowInfoHandle> windowHandle;
            CancelationOptions::Mode mode;
            std::optional<DeviceId> deviceId{std::nullopt};
            ui::LogicalDisplayId displayId{ui::LogicalDisplayId::INVALID};
            std::bitset<MAX_POINTER_ID + 1> pointerIds{};
        };

        struct PointerDownArgs {
            const nsecs_t downTimeInTarget;
            const std::shared_ptr<Connection> connection;
            const ftl::Flags<InputTarget::Flags> targetFlags;
        };

        DispatcherTouchState(const DispatcherWindowInfo& windowInfos,
                             const ConnectionManager& connections);

        void addPointerWindowTarget(const sp<android::gui::WindowInfoHandle>& windowHandle,
                                    InputTarget::DispatchMode dispatchMode,
                                    ftl::Flags<InputTarget::Flags> targetFlags,
                                    std::bitset<MAX_POINTER_ID + 1> pointerIds,
                                    std::optional<nsecs_t> firstDownTimeInTarget,
                                    std::optional<ui::LogicalDisplayId> pointerDisplayId,
                                    std::function<void()> dump,
                                    std::vector<InputTarget>& inputTargets);

        base::Result<std::vector<InputTarget>, android::os::InputEventInjectionResult>
        findTouchedWindowTargets(nsecs_t currentTime, const MotionEntry& entry,
                                 const sp<android::gui::WindowInfoHandle> dragWindow,
                                 std::function<void(const MotionEntry&)> addDragEvent,
                                 std::function<void()> dump);

        sp<android::gui::WindowInfoHandle> findTouchedForegroundWindow(
                ui::LogicalDisplayId displayId) const;

        bool hasTouchingOrHoveringPointers(ui::LogicalDisplayId displayId, int32_t deviceId) const;

        bool isPointerInWindow(const sp<android::IBinder>& token, ui::LogicalDisplayId displayId,
                               DeviceId deviceId, int32_t pointerId) const;

        // Find an existing touched windowHandle and display by token.
        std::tuple<const sp<gui::WindowInfoHandle>&, ui::LogicalDisplayId>
        findExistingTouchedWindowHandleAndDisplay(const sp<IBinder>& token) const;

        void forAllTouchedWindows(std::function<void(const sp<gui::WindowInfoHandle>&)> f) const;

        void forAllTouchedWindowsOnDisplay(
                ui::LogicalDisplayId displayId,
                std::function<void(const sp<gui::WindowInfoHandle>&)> f) const;

        std::string dump() const;

        // Updates the touchState for display from WindowInfo,
        // returns list of CancellationArgs for every cancelled touch
        std::list<CancellationArgs> updateFromWindowInfo(ui::LogicalDisplayId displayId);

        void removeAllPointersForDevice(DeviceId deviceId);

        // transfer touch between provided tokens, returns destination WindowHandle, deviceId,
        // pointers, list of cancelled windows and pointers on successful transfer.
        std::optional<
                std::tuple<sp<gui::WindowInfoHandle>, DeviceId, std::vector<PointerProperties>,
                           std::list<CancellationArgs>, std::list<PointerDownArgs>>>
        transferTouchGesture(const sp<IBinder>& fromToken, const sp<IBinder>& toToken,
                             bool transferEntireGesture);

        base::Result<std::list<CancellationArgs>, status_t> pilferPointers(
                const sp<IBinder>& token, const Connection& requestingConnection);

        void clear();

    private:
        std::unordered_map<ui::LogicalDisplayId, TouchState> mTouchStatesByDisplay;

        // As there can be only one CursorState per topology group, we will treat all displays in
        // the topology as one connected display-group. These will be identified by
        // DisplayTopologyGraph::primaryDisplayId.
        // Cursor on the any of the displays that are not part of the topology will be identified by
        // the displayId similar to mTouchStatesByDisplay.
        std::unordered_map<ui::LogicalDisplayId, TouchState> mCursorStateByDisplay;

        // The supplied lambda is invoked for each touch and cursor state of the display.
        // The function iterates until the lambda returns true, effectively performing a 'break'
        // from the iteration.
        void forTouchAndCursorStatesOnDisplay(ui::LogicalDisplayId displayId,
                                              std::function<bool(const TouchState&)> f) const;

        void forTouchAndCursorStatesOnDisplay(ui::LogicalDisplayId displayId,
                                              std::function<bool(TouchState&)> f);

        // The supplied lambda is invoked for each touchState. The function iterates until
        // the lambda returns true, effectively performing a 'break' from the iteration.
        void forAllTouchAndCursorStates(
                std::function<bool(ui::LogicalDisplayId, const TouchState&)> f) const;

        void forAllTouchAndCursorStates(std::function<bool(ui::LogicalDisplayId, TouchState&)> f);

        std::optional<std::tuple<TouchState&, TouchedWindow&, ui::LogicalDisplayId>>
        findTouchStateWindowAndDisplay(const sp<IBinder>& token);

        std::pair<std::list<CancellationArgs>, std::list<PointerDownArgs>> transferWallpaperTouch(
                const sp<gui::WindowInfoHandle> fromWindowHandle,
                const sp<gui::WindowInfoHandle> toWindowHandle, TouchState& state,
                DeviceId deviceId, const std::vector<PointerProperties>& pointers,
                ftl::Flags<InputTarget::Flags> oldTargetFlags,
                ftl::Flags<InputTarget::Flags> newTargetFlags);

        void saveTouchStateForMotionEntry(const MotionEntry& entry, TouchState&& touchState);

        void eraseTouchStateForMotionEntry(const MotionEntry& entry);

        const TouchState* getTouchStateForMotionEntry(
                const android::inputdispatcher::MotionEntry& entry) const;

        bool canWindowReceiveMotion(const sp<gui::WindowInfoHandle>& window,
                                    const MotionEntry& motionEntry) const;

        // Return true if stylus is currently down anywhere on the specified display,
        // and false otherwise.
        bool isStylusActiveInDisplay(ui::LogicalDisplayId displayId) const;

        std::list<CancellationArgs> eraseRemovedWindowsFromWindowInfo(
                TouchState& state, ui::LogicalDisplayId displayId);

        std::list<CancellationArgs> updateHoveringStateFromWindowInfo(
                TouchState& state, ui::LogicalDisplayId displayId);

        std::vector<InputTarget> findOutsideTargets(ui::LogicalDisplayId displayId,
                                                    const sp<gui::WindowInfoHandle>& touchedWindow,
                                                    int32_t pointerId, std::function<void()> dump);

        /**
         * Slip the wallpaper touch if necessary.
         *
         * @param targetFlags the target flags
         * @param oldWindowHandle the old window that the touch slipped out of
         * @param newWindowHandle the new window that the touch is slipping into
         * @param state the current touch state. This will be updated if necessary to reflect the
         * new windows that are receiving touch.
         * @param deviceId the device id of the current motion being processed
         * @param pointerProperties the pointer properties of the current motion being processed
         * @param targets the current targets to add the walpaper ones to
         * @param eventTime the new downTime for the wallpaper target
         */
        void slipWallpaperTouch(ftl::Flags<InputTarget::Flags> targetFlags,
                                const sp<android::gui::WindowInfoHandle>& oldWindowHandle,
                                const sp<android::gui::WindowInfoHandle>& newWindowHandle,
                                TouchState& state, const MotionEntry& entry,
                                std::vector<InputTarget>& targets, std::function<void()> dump);

        ftl::Flags<InputTarget::Flags> getTargetFlags(
                const sp<android::gui::WindowInfoHandle>& targetWindow, vec2 targetPosition,
                bool isSplit);

        const DispatcherWindowInfo& mWindowInfos;
        const ConnectionManager& mConnectionManager;
    };

    DispatcherTouchState mTouchStates GUARDED_BY(mLock);

    // With each iteration, InputDispatcher nominally processes one queued event,
    // a timeout, or a response from an input consumer.
    // This method should only be called on the input dispatcher's own thread.
    void dispatchOnce();

    void dispatchOnceInnerLocked(nsecs_t& nextWakeupTime) REQUIRES(mLock);

    // Enqueues an inbound event.  Returns true if mLooper->wake() should be called.
    bool enqueueInboundEventLocked(std::unique_ptr<EventEntry> entry) REQUIRES(mLock);

    // Cleans up input state when dropping an inbound event.
    void dropInboundEventLocked(const EventEntry& entry, DropReason dropReason) REQUIRES(mLock);

    // Enqueues a focus event.
    void enqueueFocusEventLocked(const sp<IBinder>& windowToken, bool hasFocus,
                                 const std::string& reason) REQUIRES(mLock);
    // Enqueues a drag event.
    void enqueueDragEventLocked(const sp<android::gui::WindowInfoHandle>& windowToken,
                                bool isExiting, const int32_t rawX, const int32_t rawY)
            REQUIRES(mLock);

    // Adds an event to a queue of recent events for debugging purposes.
    void addRecentEventLocked(std::shared_ptr<const EventEntry> entry) REQUIRES(mLock);

    // Blocked event latency optimization.  Drops old events when the user intends
    // to transfer focus to a new application.
    std::shared_ptr<const EventEntry> mNextUnblockedEvent GUARDED_BY(mLock);

    status_t pilferPointersLocked(const sp<IBinder>& token) REQUIRES(mLock);

    const HmacKeyManager mHmacKeyManager;
    const std::array<uint8_t, 32> getSignature(const MotionEntry& motionEntry,
                                               const DispatchEntry& dispatchEntry) const;
    const std::array<uint8_t, 32> getSignature(const KeyEntry& keyEntry,
                                               const DispatchEntry& dispatchEntry) const;

    // Event injection and synchronization.
    std::condition_variable mInjectionResultAvailable;
    bool shouldRejectInjectedMotionLocked(const MotionEvent& motion, DeviceId deviceId,
                                          ui::LogicalDisplayId displayId,
                                          std::optional<gui::Uid> targetUid, int32_t flags)
            REQUIRES(mLock);
    void setInjectionResult(const EventEntry& entry,
                            android::os::InputEventInjectionResult injectionResult);
    void transformMotionEntryForInjectionLocked(MotionEntry&,
                                                const ui::Transform& injectedTransform) const
            REQUIRES(mLock);
    // Per-display correction of injected events
    std::map<android::ui::LogicalDisplayId, InputVerifier> mInputFilterVerifiersByDisplay
            GUARDED_BY(mLock);
    std::condition_variable mInjectionSyncFinished;
    void incrementPendingForegroundDispatches(const EventEntry& entry);
    void decrementPendingForegroundDispatches(const EventEntry& entry);

    // Key repeat tracking.
    struct KeyRepeatState {
        std::shared_ptr<const KeyEntry> lastKeyEntry; // or null if no repeat
        nsecs_t nextRepeatTime;
    } mKeyRepeatState GUARDED_BY(mLock);

    void resetKeyRepeatLocked() REQUIRES(mLock);
    std::shared_ptr<KeyEntry> synthesizeKeyRepeatLocked(nsecs_t currentTime) REQUIRES(mLock);

    // Deferred command processing.
    bool haveCommandsLocked() const REQUIRES(mLock);
    bool runCommandsLockedInterruptable() REQUIRES(mLock);
    void postCommandLocked(Command&& command) REQUIRES(mLock);

    // The dispatching timeout to use for Monitors.
    std::chrono::nanoseconds mMonitorDispatchingTimeout GUARDED_BY(mLock);

    nsecs_t processAnrsLocked() REQUIRES(mLock);
    void processLatencyStatisticsLocked() REQUIRES(mLock);
    std::chrono::nanoseconds getDispatchingTimeoutLocked(
            const std::shared_ptr<Connection>& connection) REQUIRES(mLock);

    // Input filter processing.
    bool shouldSendKeyToInputFilterLocked(const NotifyKeyArgs& args) REQUIRES(mLock);
    bool shouldSendMotionToInputFilterLocked(const NotifyMotionArgs& args) REQUIRES(mLock);

    // Inbound event processing.
    void drainInboundQueueLocked() REQUIRES(mLock);
    void releasePendingEventLocked() REQUIRES(mLock);
    void releaseInboundEventLocked(std::shared_ptr<const EventEntry> entry) REQUIRES(mLock);

    // Dispatch state.
    bool mDispatchEnabled GUARDED_BY(mLock);
    bool mDispatchFrozen GUARDED_BY(mLock);
    bool mInputFilterEnabled GUARDED_BY(mLock);

    // This map is not really needed, but it helps a lot with debugging (dumpsys input).
    // In the java layer, touch mode states are spread across multiple DisplayContent objects,
    // making harder to snapshot and retrieve them.
    std::map<ui::LogicalDisplayId /*displayId*/, bool /*inTouchMode*/> mTouchModePerDisplay
            GUARDED_BY(mLock);

    class DispatcherWindowListener : public gui::WindowInfosListener {
    public:
        explicit DispatcherWindowListener(InputDispatcher& dispatcher) : mDispatcher(dispatcher){};
        void onWindowInfosChanged(const gui::WindowInfosUpdate&) override;

    private:
        InputDispatcher& mDispatcher;
    };
    sp<gui::WindowInfosListener> mWindowInfoListener;

    void setInputWindowsLocked(
            const std::vector<sp<android::gui::WindowInfoHandle>>& inputWindowHandles,
            ui::LogicalDisplayId displayId) REQUIRES(mLock);

    sp<android::gui::WindowInfoHandle> getFocusedWindowHandleLocked(
            ui::LogicalDisplayId displayId) const REQUIRES(mLock);

    // Returns all the input targets (with their respective input channels) from the window handles
    // passed as argument.
    std::vector<InputTarget> getInputTargetsFromWindowHandlesLocked(
            const std::vector<sp<android::gui::WindowInfoHandle>>& windowHandles) const
            REQUIRES(mLock);

    /*
     * Validate and update InputWindowHandles for a given display.
     */
    void updateWindowHandlesForDisplayLocked(
            const std::vector<sp<android::gui::WindowInfoHandle>>& inputWindowHandles,
            ui::LogicalDisplayId displayId) REQUIRES(mLock);

    std::unique_ptr<DragState> mDragState GUARDED_BY(mLock);

    void setFocusedApplicationLocked(
            ui::LogicalDisplayId displayId,
            const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle) REQUIRES(mLock);
    // Focused applications.
    std::unordered_map<ui::LogicalDisplayId /*displayId*/, std::shared_ptr<InputApplicationHandle>>
            mFocusedApplicationHandlesByDisplay GUARDED_BY(mLock);

    // Top focused display.
    ui::LogicalDisplayId mFocusedDisplayId GUARDED_BY(mLock);

    // Keeps track of the focused window per display and determines focus changes.
    FocusResolver mFocusResolver GUARDED_BY(mLock);

    // The enabled state of this request is true iff the focused window on the focused display has
    // requested Pointer Capture. This request also contains the sequence number associated with the
    // current request. The state of this variable should always be in sync with the state of
    // Pointer Capture in the policy, and is only updated through setPointerCaptureLocked(request).
    PointerCaptureRequest mCurrentPointerCaptureRequest GUARDED_BY(mLock);

    // The window token that has Pointer Capture.
    // This should be in sync with PointerCaptureChangedEvents dispatched to the input channel.
    sp<IBinder> mWindowTokenWithPointerCapture GUARDED_BY(mLock);

    // Displays that are ineligible for pointer capture.
    // TODO(b/214621487): Remove or move to a display flag.
    std::vector<ui::LogicalDisplayId /*displayId*/> mIneligibleDisplaysForPointerCapture
            GUARDED_BY(mLock);

    // Disable Pointer Capture as a result of loss of window focus.
    void disablePointerCaptureForcedLocked() REQUIRES(mLock);

    // Set the Pointer Capture state in the Policy.
    // The window is not nullptr for requests to enable, otherwise it is nullptr.
    void setPointerCaptureLocked(const sp<IBinder>& window) REQUIRES(mLock);

    // Dispatcher state at time of last ANR.
    std::string mLastAnrState GUARDED_BY(mLock);

    // The connection tokens of the channels that the user last interacted (used for debugging and
    // when switching touch mode state).
    std::unordered_set<sp<IBinder>, StrongPointerHash<IBinder>> mInteractionConnectionTokens
            GUARDED_BY(mLock);
    void processInteractionsLocked(const EventEntry& entry, const std::vector<InputTarget>& targets)
            REQUIRES(mLock);

    // Dispatch inbound events.
    bool dispatchDeviceResetLocked(nsecs_t currentTime, const DeviceResetEntry& entry)
            REQUIRES(mLock);
    bool dispatchKeyLocked(nsecs_t currentTime, std::shared_ptr<const KeyEntry> entry,
                           DropReason* dropReason, nsecs_t& nextWakeupTime) REQUIRES(mLock);
    bool dispatchMotionLocked(nsecs_t currentTime, std::shared_ptr<const MotionEntry> entry,
                              DropReason* dropReason, nsecs_t& nextWakeupTime) REQUIRES(mLock);
    void dispatchFocusLocked(nsecs_t currentTime, std::shared_ptr<const FocusEntry> entry)
            REQUIRES(mLock);
    void dispatchPointerCaptureChangedLocked(
            nsecs_t currentTime, const std::shared_ptr<const PointerCaptureChangedEntry>& entry,
            DropReason& dropReason) REQUIRES(mLock);
    void dispatchTouchModeChangeLocked(nsecs_t currentTime,
                                       const std::shared_ptr<const TouchModeEntry>& entry)
            REQUIRES(mLock);
    void dispatchEventLocked(nsecs_t currentTime, std::shared_ptr<const EventEntry> entry,
                             const std::vector<InputTarget>& inputTargets) REQUIRES(mLock);
    void dispatchSensorLocked(nsecs_t currentTime, const std::shared_ptr<const SensorEntry>& entry,
                              DropReason* dropReason, nsecs_t& nextWakeupTime) REQUIRES(mLock);
    void dispatchDragLocked(nsecs_t currentTime, std::shared_ptr<const DragEntry> entry)
            REQUIRES(mLock);
    void logOutboundKeyDetails(const char* prefix, const KeyEntry& entry);
    void logOutboundMotionDetails(const char* prefix, const MotionEntry& entry);

    /**
     * This field is set if there is no focused window, and we have an event that requires
     * a focused window to be dispatched (for example, a KeyEvent).
     * When this happens, we will wait until *mNoFocusedWindowTimeoutTime before
     * dropping the event and raising an ANR for that application.
     * This is useful if an application is slow to add a focused window.
     */
    std::optional<nsecs_t> mNoFocusedWindowTimeoutTime GUARDED_BY(mLock);

    bool isStaleEvent(nsecs_t currentTime, const EventEntry& entry);

    bool shouldPruneInboundQueueLocked(const MotionEntry& motionEntry) const REQUIRES(mLock);

    /**
     * Time to stop waiting for the events to be processed while trying to dispatch a key.
     * When this time expires, we just send the pending key event to the currently focused window,
     * without waiting on other events to be processed first.
     */
    std::optional<nsecs_t> mKeyIsWaitingForEventsTimeout GUARDED_BY(mLock);
    bool shouldWaitToSendKeyLocked(nsecs_t currentTime, const char* focusedWindowName)
            REQUIRES(mLock);

    /**
     * The focused application at the time when no focused window was present.
     * Used to raise an ANR when we have no focused window.
     */
    std::shared_ptr<InputApplicationHandle> mAwaitedFocusedApplication GUARDED_BY(mLock);
    /**
     * The displayId that the focused application is associated with.
     */
    ui::LogicalDisplayId mAwaitedApplicationDisplayId GUARDED_BY(mLock);
    void processNoFocusedWindowAnrLocked() REQUIRES(mLock);

    /**
     * Tell policy about a window or a monitor that just became unresponsive. Starts ANR.
     */
    void processConnectionUnresponsiveLocked(const Connection& connection, std::string reason)
            REQUIRES(mLock);
    /**
     * Tell policy about a window or a monitor that just became responsive.
     */
    void processConnectionResponsiveLocked(const Connection& connection) REQUIRES(mLock);

    void sendWindowUnresponsiveCommandLocked(const sp<IBinder>& connectionToken,
                                             std::optional<gui::Pid> pid, std::string reason)
            REQUIRES(mLock);
    void sendWindowResponsiveCommandLocked(const sp<IBinder>& connectionToken,
                                           std::optional<gui::Pid> pid) REQUIRES(mLock);

    // Optimization: AnrTracker is used to quickly find which connection is due for a timeout next.
    // AnrTracker must be kept in-sync with all responsive connection.waitQueues.
    // If a connection is not responsive, then the entries should not be added to the AnrTracker.
    // Once a connection becomes unresponsive, its entries are removed from AnrTracker to
    // prevent unneeded wakeups.
    AnrTracker mAnrTracker GUARDED_BY(mLock);

    void cancelEventsForAnrLocked(const std::shared_ptr<Connection>& connection) REQUIRES(mLock);
    // If a focused application changes, we should stop counting down the "no focused window" time,
    // because we will have no way of knowing when the previous application actually added a window.
    // This also means that we will miss cases like pulling down notification shade when the
    // focused application does not have a focused window (no ANR will be raised if notification
    // shade is pulled down while we are counting down the timeout).
    void resetNoFocusedWindowTimeoutLocked() REQUIRES(mLock);

    ui::LogicalDisplayId getTargetDisplayId(const EventEntry& entry);
    base::Result<sp<android::gui::WindowInfoHandle>, android::os::InputEventInjectionResult>
    findFocusedWindowTargetLocked(nsecs_t currentTime, const EventEntry& entry,
                                  nsecs_t& nextWakeupTime) REQUIRES(mLock);

    void addWindowTargetLocked(const sp<android::gui::WindowInfoHandle>& windowHandle,
                               InputTarget::DispatchMode dispatchMode,
                               ftl::Flags<InputTarget::Flags> targetFlags,
                               std::optional<nsecs_t> firstDownTimeInTarget,
                               std::vector<InputTarget>& inputTargets) const REQUIRES(mLock);
    void addGlobalMonitoringTargetsLocked(std::vector<InputTarget>& inputTargets,
                                          ui::LogicalDisplayId displayId) REQUIRES(mLock);
    void pokeUserActivityLocked(const EventEntry& eventEntry) REQUIRES(mLock);
    // Enqueue a drag event if needed, and update the touch state.
    // Uses findTouchedWindowTargetsLocked to make the decision
    void addDragEventLocked(const MotionEntry& entry) REQUIRES(mLock);
    void finishDragAndDrop(ui::LogicalDisplayId displayId, float x, float y) REQUIRES(mLock);

    std::string getApplicationWindowLabel(const InputApplicationHandle* applicationHandle,
                                          const sp<android::gui::WindowInfoHandle>& windowHandle);

    static std::vector<sp<android::gui::WindowInfoHandle>> findTouchedSpyWindowsAt(
            ui::LogicalDisplayId displayId, float x, float y, bool isStylus, DeviceId deviceId,
            const DispatcherWindowInfo& windowInfos);

    static bool shouldDropInput(const EventEntry& entry,
                                const sp<android::gui::WindowInfoHandle>& windowHandle,
                                const DispatcherWindowInfo& windowInfo);

    // Manage the dispatch cycle for a single connection.
    // These methods are deliberately not Interruptible because doing all of the work
    // with the mutex held makes it easier to ensure that connection invariants are maintained.
    // If needed, the methods post commands to run later once the critical bits are done.
    void prepareDispatchCycleLocked(nsecs_t currentTime,
                                    const std::shared_ptr<Connection>& connection,
                                    std::shared_ptr<const EventEntry>,
                                    const InputTarget& inputTarget) REQUIRES(mLock);
    void enqueueDispatchEntryAndStartDispatchCycleLocked(
            nsecs_t currentTime, const std::shared_ptr<Connection>& connection,
            std::shared_ptr<const EventEntry>, const InputTarget& inputTarget) REQUIRES(mLock);
    void enqueueDispatchEntryLocked(const std::shared_ptr<Connection>& connection,
                                    std::shared_ptr<const EventEntry>,
                                    const InputTarget& inputTarget) REQUIRES(mLock);
    status_t publishMotionEvent(Connection& connection, DispatchEntry& dispatchEntry) const;
    void startDispatchCycleLocked(nsecs_t currentTime,
                                  const std::shared_ptr<Connection>& connection) REQUIRES(mLock);
    void finishDispatchCycleLocked(nsecs_t currentTime,
                                   const std::shared_ptr<Connection>& connection, uint32_t seq,
                                   bool handled, nsecs_t consumeTime) REQUIRES(mLock);
    void abortBrokenDispatchCycleLocked(const std::shared_ptr<Connection>& connection, bool notify)
            REQUIRES(mLock);
    void drainDispatchQueue(std::deque<std::unique_ptr<DispatchEntry>>& queue);
    void releaseDispatchEntry(std::unique_ptr<DispatchEntry> dispatchEntry);
    int handleReceiveCallback(int events, sp<IBinder> connectionToken);
    // The action sent should only be of type AMOTION_EVENT_*
    void dispatchPointerDownOutsideFocus(uint32_t source, int32_t action,
                                         const sp<IBinder>& newToken) REQUIRES(mLock);

    void synthesizeCancelationEventsForAllConnectionsLocked(const CancelationOptions& options)
            REQUIRES(mLock);
    void synthesizeCancelationEventsForMonitorsLocked(const CancelationOptions& options)
            REQUIRES(mLock);
    void synthesizeCancelationEventsForWindowLocked(const sp<gui::WindowInfoHandle>&,
                                                    const CancelationOptions&,
                                                    const std::shared_ptr<Connection>& = nullptr)
            REQUIRES(mLock);
    // This is a convenience function used to generate cancellation for a connection without having
    // to check whether it's a monitor or a window. For non-monitors, the window handle must not be
    // null. Always prefer the "-ForWindow" method above when explicitly dealing with windows.
    void synthesizeCancelationEventsForConnectionLocked(
            const std::shared_ptr<Connection>& connection, const CancelationOptions& options,
            const sp<gui::WindowInfoHandle>& window) REQUIRES(mLock);

    void synthesizePointerDownEventsForConnectionLocked(
            const nsecs_t downTime, const std::shared_ptr<Connection>& connection,
            ftl::Flags<InputTarget::Flags> targetFlags,
            const std::unique_ptr<trace::EventTrackerInterface>& traceTracker) REQUIRES(mLock);

    // Splitting motion events across windows. When splitting motion event for a target,
    // splitDownTime refers to the time of first 'down' event on that particular target
    std::unique_ptr<MotionEntry> splitMotionEvent(const MotionEntry& originalMotionEntry,
                                                  std::bitset<MAX_POINTER_ID + 1> pointerIds,
                                                  nsecs_t splitDownTime) REQUIRES(mLock);

    // Reset and drop everything the dispatcher is doing.
    void resetAndDropEverythingLocked(const char* reason) REQUIRES(mLock);

    // Dump state.
    void dumpDispatchStateLocked(std::string& dump) const REQUIRES(mLock);
    void logDispatchStateLocked() const REQUIRES(mLock);
    std::string dumpPointerCaptureStateLocked() const REQUIRES(mLock);

    status_t removeInputChannelLocked(const std::shared_ptr<Connection>& connection, bool notify)
            REQUIRES(mLock);

    // Interesting events that we might like to log or tell the framework about.
    void doDispatchCycleFinishedCommand(nsecs_t finishTime,
                                        const std::shared_ptr<Connection>& connection, uint32_t seq,
                                        bool handled, nsecs_t consumeTime) REQUIRES(mLock);
    void doInterceptKeyBeforeDispatchingCommand(const sp<IBinder>& focusedWindowToken,
                                                const KeyEntry& entry) REQUIRES(mLock);
    void onFocusChangedLocked(const FocusResolver::FocusChanges& changes,
                              const std::unique_ptr<trace::EventTrackerInterface>& traceTracker,
                              const sp<gui::WindowInfoHandle> removedFocusedWindowHandle = nullptr)
            REQUIRES(mLock);
    void sendFocusChangedCommandLocked(const sp<IBinder>& oldToken, const sp<IBinder>& newToken)
            REQUIRES(mLock);
    void sendDropWindowCommandLocked(const sp<IBinder>& token, float x, float y) REQUIRES(mLock);
    void onAnrLocked(const std::shared_ptr<Connection>& connection) REQUIRES(mLock);
    void onAnrLocked(std::shared_ptr<InputApplicationHandle> application) REQUIRES(mLock);
    void updateLastAnrStateLocked(const sp<android::gui::WindowInfoHandle>& window,
                                  const std::string& reason) REQUIRES(mLock);
    void updateLastAnrStateLocked(const InputApplicationHandle& application,
                                  const std::string& reason) REQUIRES(mLock);
    void updateLastAnrStateLocked(const std::string& windowLabel, const std::string& reason)
            REQUIRES(mLock);
    std::map<ui::LogicalDisplayId, InputVerifier> mVerifiersByDisplay;
    // Returns a fallback KeyEntry that should be sent to the connection, if required.
    std::unique_ptr<const KeyEntry> afterKeyEventLockedInterruptable(
            const std::shared_ptr<Connection>& connection, DispatchEntry* dispatchEntry,
            bool handled) REQUIRES(mLock);
    void findAndDispatchFallbackEvent(nsecs_t currentTime, std::shared_ptr<const KeyEntry> entry,
                                      std::vector<InputTarget>& inputTargets) REQUIRES(mLock);

    // Statistics gathering.
    nsecs_t mLastStatisticPushTime = 0;
    std::unique_ptr<InputEventTimelineProcessor> mInputEventTimelineProcessor GUARDED_BY(mLock);
    // Must outlive `mLatencyTracker`.
    std::vector<InputDeviceInfo> mInputDevices;
    LatencyTracker mLatencyTracker GUARDED_BY(mLock);
    void traceInboundQueueLengthLocked() REQUIRES(mLock);
    void traceOutboundQueueLength(const Connection& connection);
    void traceWaitQueueLength(const Connection& connection);

    // Check window ownership
    bool focusedWindowIsOwnedByLocked(gui::Pid pid, gui::Uid uid) REQUIRES(mLock);
    bool recentWindowsAreOwnedByLocked(gui::Pid pid, gui::Uid uid) REQUIRES(mLock);

    sp<InputReporterInterface> mReporter;

    /** Stores the value of the input flag for per device input latency metrics. */
    const bool mPerDeviceInputLatencyMetricsFlag =
            com::android::input::flags::enable_per_device_input_latency_metrics();
};

} // namespace android::inputdispatcher
