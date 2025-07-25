/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include <stdint.h>
#include <sys/types.h>

#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <binder/IBinder.h>

#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/Singleton.h>
#include <utils/SortedVector.h>
#include <utils/threads.h>

#include <ui/BlurRegion.h>
#include <ui/ConfigStoreTypes.h>
#include <ui/DisplayedFrameStats.h>
#include <ui/EdgeExtensionEffect.h>
#include <ui/FrameStats.h>
#include <ui/GraphicTypes.h>
#include <ui/PictureProfileHandle.h>
#include <ui/PixelFormat.h>
#include <ui/Rotation.h>
#include <ui/StaticDisplayInfo.h>

#include <android/gui/BnJankListener.h>
#include <android/gui/ISurfaceComposerClient.h>

#include <gui/BufferReleaseChannel.h>
#include <gui/CpuConsumer.h>
#include <gui/ISurfaceComposer.h>
#include <gui/ITransactionCompletedListener.h>
#include <gui/LayerState.h>
#include <gui/SurfaceControl.h>
#include <gui/TransactionState.h>
#include <gui/WindowInfosListenerReporter.h>
#include <math/vec3.h>

#include <aidl/android/hardware/graphics/common/DisplayDecorationSupport.h>

namespace android {

class HdrCapabilities;
class IGraphicBufferProducer;
class ITunnelModeEnabledListener;
class Region;
class TransactionCompletedListener;

using gui::DisplayCaptureArgs;
using gui::IRegionSamplingListener;
using gui::ISurfaceComposerClient;
using gui::LayerCaptureArgs;
using gui::LayerMetadata;

struct SurfaceControlStats {
    SurfaceControlStats(const sp<SurfaceControl>& sc, nsecs_t latchTime,
                        std::variant<nsecs_t, sp<Fence>> acquireTimeOrFence,
                        const sp<Fence>& presentFence, const sp<Fence>& prevReleaseFence,
                        std::optional<uint32_t> hint, FrameEventHistoryStats eventStats,
                        uint32_t currentMaxAcquiredBufferCount)
          : surfaceControl(sc),
            latchTime(latchTime),
            acquireTimeOrFence(std::move(acquireTimeOrFence)),
            presentFence(presentFence),
            previousReleaseFence(prevReleaseFence),
            transformHint(hint),
            frameEventStats(eventStats),
            currentMaxAcquiredBufferCount(currentMaxAcquiredBufferCount) {}

    sp<SurfaceControl> surfaceControl;
    nsecs_t latchTime = -1;
    std::variant<nsecs_t, sp<Fence>> acquireTimeOrFence = -1;
    sp<Fence> presentFence;
    sp<Fence> previousReleaseFence;
    std::optional<uint32_t> transformHint = 0;
    FrameEventHistoryStats frameEventStats;
    uint32_t currentMaxAcquiredBufferCount = 0;
};

using TransactionCompletedCallbackTakesContext =
        std::function<void(void* /*context*/, nsecs_t /*latchTime*/,
                           const sp<Fence>& /*presentFence*/,
                           const std::vector<SurfaceControlStats>& /*stats*/)>;
using TransactionCompletedCallback =
        std::function<void(nsecs_t /*latchTime*/, const sp<Fence>& /*presentFence*/,
                           const std::vector<SurfaceControlStats>& /*stats*/)>;
using ReleaseBufferCallback =
        std::function<void(const ReleaseCallbackId&, const sp<Fence>& /*releaseFence*/,
                           std::optional<uint32_t> currentMaxAcquiredBufferCount)>;

using SurfaceStatsCallback =
        std::function<void(void* /*context*/, nsecs_t /*latchTime*/,
                           const sp<Fence>& /*presentFence*/,
                           const SurfaceStats& /*stats*/)>;

using TrustedPresentationCallback = std::function<void(void*, bool)>;

// ---------------------------------------------------------------------------

class ReleaseCallbackThread {
public:
    void addReleaseCallback(const ReleaseCallbackId, sp<Fence>);
    void threadMain();

private:
    std::thread mThread;
    std::mutex mMutex;
    bool mStarted GUARDED_BY(mMutex) = false;
    std::condition_variable mReleaseCallbackPending;
    std::queue<std::tuple<const ReleaseCallbackId, const sp<Fence>>> mCallbackInfos
            GUARDED_BY(mMutex);
};

// ---------------------------------------------------------------------------

class SurfaceComposerClient : public RefBase
{
    friend class Composer;
public:
                SurfaceComposerClient();
                SurfaceComposerClient(const sp<ISurfaceComposerClient>& client);
    virtual     ~SurfaceComposerClient();

    // Always make sure we could initialize
    status_t    initCheck() const;

    // Return the connection of this client
    sp<IBinder> connection() const;

    // Forcibly remove connection before all references have gone away.
    void        dispose();

    // callback when the composer is dies
    status_t linkToComposerDeath(const sp<IBinder::DeathRecipient>& recipient,
            void* cookie = nullptr, uint32_t flags = 0);

    // Notify the SurfaceComposerClient that the boot procedure has completed
    static status_t bootFinished();

    // Get transactional state of given display.
    static status_t getDisplayState(const sp<IBinder>& display, ui::DisplayState*);

    // Get immutable information about given physical display.
    static status_t getStaticDisplayInfo(int64_t, ui::StaticDisplayInfo*);

    // Get dynamic information about given physical display from display id
    static status_t getDynamicDisplayInfoFromId(int64_t, ui::DynamicDisplayInfo*);

    // Shorthand for the active display mode from getDynamicDisplayInfo().
    // TODO(b/180391891): Update clients to use getDynamicDisplayInfo and remove this function.
    static status_t getActiveDisplayMode(const sp<IBinder>& display, ui::DisplayMode*);

    // Sets the refresh rate boundaries for the display.
    static status_t setDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                               const gui::DisplayModeSpecs&);
    // Gets the refresh rate boundaries for the display.
    static status_t getDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                               gui::DisplayModeSpecs*);

    // Get the coordinates of the display's native color primaries
    static status_t getDisplayNativePrimaries(const sp<IBinder>& display,
            ui::DisplayPrimaries& outPrimaries);

    // Sets the active color mode for the given display
    static status_t setActiveColorMode(const sp<IBinder>& display,
            ui::ColorMode colorMode);

    // Gets if boot display mode operations are supported on a device
    static status_t getBootDisplayModeSupport(bool* support);

    // Gets the overlay properties of the device
    static status_t getOverlaySupport(gui::OverlayProperties* outProperties);

    // Sets the user-preferred display mode that a device should boot in
    static status_t setBootDisplayMode(const sp<IBinder>& display, ui::DisplayModeId);
    // Clears the user-preferred display mode
    static status_t clearBootDisplayMode(const sp<IBinder>& display);

    // Gets the HDR conversion capabilities of the device
    static status_t getHdrConversionCapabilities(std::vector<gui::HdrConversionCapability>*);
    // Sets the HDR conversion strategy for the device. in case when HdrConversionStrategy has
    // autoAllowedHdrTypes set. Returns Hdr::INVALID in other cases.
    static status_t setHdrConversionStrategy(gui::HdrConversionStrategy hdrConversionStrategy,
                                             ui::Hdr* outPreferredHdrOutputType);
    // Returns whether HDR conversion is supported by the device.
    static status_t getHdrOutputConversionSupport(bool* isSupported);

    // Sets the frame rate of a particular app (uid). This is currently called
    // by GameManager.
    static status_t setGameModeFrameRateOverride(uid_t uid, float frameRate);

    // Sets the frame rate of a particular app (uid). This is currently called
    // by GameManager and controlled by two sysprops:
    // "ro.surface_flinger.game_default_frame_rate_override" holding the override value,
    // "persisit.graphics.game_default_frame_rate.enabled" to determine if it's enabled.
    static status_t setGameDefaultFrameRateOverride(uid_t uid, float frameRate);

    // Update the small area detection whole appId-threshold mappings by same size appId and
    // threshold vector.
    // Ref:setSmallAreaDetectionThreshold.
    static status_t updateSmallAreaDetection(std::vector<int32_t>& appIds,
                                             std::vector<float>& thresholds);

    // Sets the small area detection threshold to particular apps (appId). Passing value 0 means
    // to disable small area detection to the app.
    static status_t setSmallAreaDetectionThreshold(int32_t appId, float threshold);

    // Switches on/off Auto Low Latency Mode on the connected display. This should only be
    // called if the connected display supports Auto Low Latency Mode as reported by
    // #getAutoLowLatencyModeSupport
    static void setAutoLowLatencyMode(const sp<IBinder>& display, bool on);

    // Turns Game mode on/off on the connected display. This should only be called
    // if the display supports Game content type, as reported by #getGameContentTypeSupport
    static void setGameContentType(const sp<IBinder>& display, bool on);

    /* Triggers screen on/off or low power mode and waits for it to complete */
    static void setDisplayPowerMode(const sp<IBinder>& display, int mode);

    /* Returns the composition preference of the default data space and default pixel format,
     * as well as the wide color gamut data space and wide color gamut pixel format.
     * If the wide color gamut data space is V0_SRGB, then it implies that the platform
     * has no wide color gamut support.
     */
    static status_t getCompositionPreference(ui::Dataspace* defaultDataspace,
                                             ui::PixelFormat* defaultPixelFormat,
                                             ui::Dataspace* wideColorGamutDataspace,
                                             ui::PixelFormat* wideColorGamutPixelFormat);

    /*
     * Gets whether SurfaceFlinger can support protected content in GPU composition.
     * Requires the ACCESS_SURFACE_FLINGER permission.
     */
    static bool getProtectedContentSupport();

    /**
     * Gets the context priority of surface flinger's render engine.
     */
    static int getGpuContextPriority();

    /**
     * Uncaches a buffer in ISurfaceComposer. It must be uncached via a transaction so that it is
     * in order with other transactions that use buffers.
     */
    static void doUncacheBufferTransaction(uint64_t cacheId);

    // Queries whether a given display is wide color display.
    static status_t isWideColorDisplay(const sp<IBinder>& display, bool* outIsWideColorDisplay);

    /*
     * Returns whether brightness operations are supported on a display.
     *
     * displayToken
     *      The token of the display.
     *
     * Returns whether brightness operations are supported on a display or not.
     */
    static bool getDisplayBrightnessSupport(const sp<IBinder>& displayToken);

    /*
     * Sets the brightness of a display.
     *
     * displayToken
     *      The token of the display whose brightness is set.
     * brightness
     *      A number between 0.0 (minimum brightness) and 1.0 (maximum brightness), or -1.0f to
     *      turn the backlight off.
     *
     * Returns NO_ERROR upon success. Otherwise,
     *      NAME_NOT_FOUND    if the display handle is invalid, or
     *      BAD_VALUE         if the brightness value is invalid, or
     *      INVALID_OPERATION if brightness operaetions are not supported.
     */
    static status_t setDisplayBrightness(const sp<IBinder>& displayToken,
                                         const gui::DisplayBrightness& brightness);

    static status_t addHdrLayerInfoListener(const sp<IBinder>& displayToken,
                                            const sp<gui::IHdrLayerInfoListener>& listener);
    static status_t removeHdrLayerInfoListener(const sp<IBinder>& displayToken,
                                               const sp<gui::IHdrLayerInfoListener>& listener);

    static status_t addActivePictureListener(const sp<gui::IActivePictureListener>& listener);

    static status_t removeActivePictureListener(const sp<gui::IActivePictureListener>& listener);

    /*
     * Sends a power boost to the composer. This function is asynchronous.
     *
     * boostId
     *      boost id according to android::hardware::power::Boost
     *
     * Returns NO_ERROR upon success.
     */
    static status_t notifyPowerBoost(int32_t boostId);

    /*
     * Sets the global configuration for all the shadows drawn by SurfaceFlinger. Shadow follows
     * material design guidelines.
     *
     * ambientColor
     *      Color to the ambient shadow. The alpha is premultiplied.
     *
     * spotColor
     *      Color to the spot shadow. The alpha is premultiplied. The position of the spot shadow
     *      depends on the light position.
     *
     * lightPosY/lightPosZ
     *      Position of the light used to cast the spot shadow. The X value is always the display
     *      width / 2.
     *
     * lightRadius
     *      Radius of the light casting the shadow.
     */
    static status_t setGlobalShadowSettings(const half4& ambientColor, const half4& spotColor,
                                            float lightPosY, float lightPosZ, float lightRadius);

    /*
     * Returns whether and how a display supports DISPLAY_DECORATION layers.
     *
     * displayToken
     *      The token of the display.
     *
     * Returns how a display supports DISPLAY_DECORATION layers, or nullopt if
     * it does not.
     */
    static std::optional<aidl::android::hardware::graphics::common::DisplayDecorationSupport>
    getDisplayDecorationSupport(const sp<IBinder>& displayToken);

    /**
     * Returns how many picture profiles are supported by the display.
     *
     * displayToken
     *      The token of the display.
     */
    static status_t getMaxLayerPictureProfiles(const sp<IBinder>& displayToken,
                                               int32_t* outMaxProfiles);

    // ------------------------------------------------------------------------
    // surface creation / destruction

    static sp<SurfaceComposerClient> getDefault();

    //! Create a surface
    sp<SurfaceControl> createSurface(const String8& name, // name of the surface
                                     uint32_t w,          // width in pixel
                                     uint32_t h,          // height in pixel
                                     PixelFormat format,  // pixel-format desired
                                     int32_t flags = 0,   // usage flags
                                     const sp<IBinder>& parentHandle = nullptr, // parentHandle
                                     LayerMetadata metadata = LayerMetadata(),  // metadata
                                     uint32_t* outTransformHint = nullptr);

    status_t createSurfaceChecked(const String8& name, // name of the surface
                                  uint32_t w,          // width in pixel
                                  uint32_t h,          // height in pixel
                                  PixelFormat format,  // pixel-format desired
                                  sp<SurfaceControl>* outSurface,
                                  int32_t flags = 0,                         // usage flags
                                  const sp<IBinder>& parentHandle = nullptr, // parentHandle
                                  LayerMetadata metadata = LayerMetadata(),  // metadata
                                  uint32_t* outTransformHint = nullptr);

    // Creates a mirrored hierarchy for the mirrorFromSurface. This returns a SurfaceControl
    // which is a parent of the root of the mirrored hierarchy.
    //
    //  Real Hierarchy    Mirror
    //                      SC (value that's returned)
    //                      |
    //      A               A'
    //      |               |
    //      B               B'
    sp<SurfaceControl> mirrorSurface(SurfaceControl* mirrorFromSurface);

    sp<SurfaceControl> mirrorDisplay(DisplayId displayId);

    static const std::string kEmpty;
    static sp<IBinder> createVirtualDisplay(const std::string& displayName, bool isSecure,
                                            bool optimizeForPower = true,
                                            const std::string& uniqueId = kEmpty,
                                            float requestedRefreshRate = 0);

    static status_t destroyVirtualDisplay(const sp<IBinder>& displayToken);

    static sp<IBinder> createDisplay(const String8& displayName, bool isSecure,
                                            float requestedRefreshRate = 0);

    static void destroyDisplay(const sp<IBinder>& displayToken);

    static std::vector<PhysicalDisplayId> getPhysicalDisplayIds();

    static sp<IBinder> getPhysicalDisplayToken(PhysicalDisplayId displayId);

    // Returns StalledTransactionInfo if a transaction from the provided pid has not been applied
    // due to an unsignaled fence.
    static std::optional<gui::StalledTransactionInfo> getStalledTransactionInfo(pid_t pid);

    struct SCHash {
        std::size_t operator()(const sp<SurfaceControl>& sc) const {
            return std::hash<SurfaceControl *>{}(sc.get());
        }
    };

    struct IBinderHash {
        std::size_t operator()(const sp<IBinder>& iBinder) const {
            return std::hash<IBinder*>{}(iBinder.get());
        }
    };

    struct TCLHash {
        std::size_t operator()(const sp<ITransactionCompletedListener>& tcl) const {
            return std::hash<IBinder*>{}((tcl) ? IInterface::asBinder(tcl).get() : nullptr);
        }
    };

    struct CallbackInfo {
        // All the callbacks that have been requested for a TransactionCompletedListener in the
        // Transaction
        std::unordered_set<CallbackId, CallbackIdHash> callbackIds;
        // All the SurfaceControls that have been modified in this TransactionCompletedListener's
        // process that require a callback if there is one or more callbackIds set.
        std::unordered_set<sp<SurfaceControl>, SCHash> surfaceControls;
    };

    struct PresentationCallbackRAII : public RefBase {
        sp<TransactionCompletedListener> mTcl;
        int mId;
        PresentationCallbackRAII(TransactionCompletedListener* tcl, int id);
        virtual ~PresentationCallbackRAII();
    };

    class Transaction {
    private:
        static sp<IBinder> sApplyToken;
        static std::mutex sApplyTokenMutex;
        void releaseBufferIfOverwriting(const layer_state_t& state);
        // Tracks registered callbacks
        sp<TransactionCompletedListener> mTransactionCompletedListener = nullptr;

        TransactionState mState;

        int mStatus = NO_ERROR;

        layer_state_t* getLayerState(const sp<SurfaceControl>& sc);
        DisplayState& getDisplayState(const sp<IBinder>& token);

        void cacheBuffers();
        void registerSurfaceControlForCallback(const sp<SurfaceControl>& sc);
        void setReleaseBufferCallback(BufferData*, ReleaseBufferCallback);

    protected:
        // Accessed in tests.
        explicit Transaction(Transaction const& other) = default;
        std::unordered_map<sp<ITransactionCompletedListener>, CallbackInfo, TCLHash>
                mListenerCallbacks;

    public:
        Transaction();
        Transaction(Transaction&& other);
        Transaction& operator=(Transaction&& other) = default;

        // Factory method that creates a new Transaction instance from the parcel.
        static std::unique_ptr<Transaction> createFromParcel(const Parcel* parcel);

        status_t writeToParcel(Parcel* parcel) const;
        status_t readFromParcel(const Parcel* parcel);

        // Clears the contents of the transaction without applying it.
        void clear();

        // Returns the current id of the transaction.
        // The id is updated every time the transaction is applied.
        uint64_t getId() const;

        std::vector<uint64_t> getMergedTransactionIds();

        status_t apply(bool synchronous = false, bool oneWay = false);
        // Merge another transaction in to this one, clearing other
        // as if it had been applied.
        Transaction& merge(Transaction&& other);
        Transaction& show(const sp<SurfaceControl>& sc);
        Transaction& hide(const sp<SurfaceControl>& sc);
        Transaction& setPosition(const sp<SurfaceControl>& sc, float x, float y);
        // b/243180033 remove once functions are not called from vendor code
        Transaction& setSize(const sp<SurfaceControl>&, uint32_t, uint32_t) { return *this; }
        Transaction& setLayer(const sp<SurfaceControl>& sc,
                int32_t z);

        // Sets a Z order relative to the Surface specified by "relativeTo" but
        // without becoming a full child of the relative. Z-ordering works exactly
        // as if it were a child however.
        //
        // As a nod to sanity, only non-child surfaces may have a relative Z-order.
        //
        // This overrides any previous call and is overriden by any future calls
        // to setLayer.
        //
        // If the relative is removed, the Surface will have no layer and be
        // invisible, until the next time set(Relative)Layer is called.
        Transaction& setRelativeLayer(const sp<SurfaceControl>& sc,
                                      const sp<SurfaceControl>& relativeTo, int32_t z);
        Transaction& setFlags(const sp<SurfaceControl>& sc,
                uint32_t flags, uint32_t mask);
        Transaction& setTransparentRegionHint(const sp<SurfaceControl>& sc,
                const Region& transparentRegion);
        Transaction& setDimmingEnabled(const sp<SurfaceControl>& sc, bool dimmingEnabled);
        Transaction& setAlpha(const sp<SurfaceControl>& sc,
                float alpha);
        Transaction& setMatrix(const sp<SurfaceControl>& sc,
                float dsdx, float dtdx, float dtdy, float dsdy);
        Transaction& setCrop(const sp<SurfaceControl>& sc, const Rect& crop);
        Transaction& setCrop(const sp<SurfaceControl>& sc, const FloatRect& crop);
        Transaction& setCornerRadius(const sp<SurfaceControl>& sc, float cornerRadius);
        // Sets the client drawn corner radius for the layer. If both a corner radius and a client
        // radius are sent to SF, the client radius will be used. This indicates that the corner
        // radius is drawn by the client and not SurfaceFlinger.
        Transaction& setClientDrawnCornerRadius(const sp<SurfaceControl>& sc,
                                                float clientDrawnCornerRadius);
        Transaction& setBackgroundBlurRadius(const sp<SurfaceControl>& sc,
                                             int backgroundBlurRadius);
        Transaction& setBlurRegions(const sp<SurfaceControl>& sc,
                                    const std::vector<BlurRegion>& regions);
        Transaction& setLayerStack(const sp<SurfaceControl>&, ui::LayerStack);
        Transaction& setMetadata(const sp<SurfaceControl>& sc, uint32_t key, const Parcel& p);

        /// Reparents the current layer to the new parent handle. The new parent must not be null.
        Transaction& reparent(const sp<SurfaceControl>& sc, const sp<SurfaceControl>& newParent);

        Transaction& setColor(const sp<SurfaceControl>& sc, const half3& color);

        // Sets the background color of a layer with the specified color, alpha, and dataspace
        Transaction& setBackgroundColor(const sp<SurfaceControl>& sc, const half3& color,
                                        float alpha, ui::Dataspace dataspace);

        Transaction& setTransform(const sp<SurfaceControl>& sc, uint32_t transform);
        Transaction& setTransformToDisplayInverse(const sp<SurfaceControl>& sc,
                                                  bool transformToDisplayInverse);
        Transaction& setBuffer(const sp<SurfaceControl>& sc, const sp<GraphicBuffer>& buffer,
                               const std::optional<sp<Fence>>& fence = std::nullopt,
                               const std::optional<uint64_t>& frameNumber = std::nullopt,
                               uint32_t producerId = 0, ReleaseBufferCallback callback = nullptr,
                               nsecs_t dequeueTime = -1);
        Transaction& unsetBuffer(const sp<SurfaceControl>& sc);
        std::shared_ptr<BufferData> getAndClearBuffer(const sp<SurfaceControl>& sc);

        /**
         * If this transaction, has a a buffer set for the given SurfaceControl
         * mark that buffer as ordered after a given barrierFrameNumber.
         *
         * SurfaceFlinger will refuse to apply this transaction until after
         * the frame in barrierFrameNumber has been applied. This transaction may
         * be applied in the same frame as the barrier buffer or after.
         *
         * This is only designed to be used to handle switches between multiple
         * apply tokens, as explained in the comment for BLASTBufferQueue::mAppliedLastTransaction.
         *
         * Has to be called after setBuffer.
         *
         * WARNING:
         * This API is very dangerous to the caller, as if you invoke it without
         * a frameNumber you have not yet submitted, you can dead-lock your
         * SurfaceControl's transaction queue.
         */
        Transaction& setBufferHasBarrier(const sp<SurfaceControl>& sc,
                                         uint64_t barrierFrameNumber);
        Transaction& setDataspace(const sp<SurfaceControl>& sc, ui::Dataspace dataspace);
        Transaction& setExtendedRangeBrightness(const sp<SurfaceControl>& sc,
                                                float currentBufferRatio, float desiredRatio);
        Transaction& setDesiredHdrHeadroom(const sp<SurfaceControl>& sc, float desiredRatio);
        Transaction& setLuts(const sp<SurfaceControl>& sc, base::unique_fd&& lutFd,
                             const std::vector<int32_t>& offsets,
                             const std::vector<int32_t>& dimensions,
                             const std::vector<int32_t>& sizes,
                             const std::vector<int32_t>& samplingKeys);
        Transaction& setCachingHint(const sp<SurfaceControl>& sc, gui::CachingHint cachingHint);
        Transaction& setHdrMetadata(const sp<SurfaceControl>& sc, const HdrMetadata& hdrMetadata);
        Transaction& setSurfaceDamageRegion(const sp<SurfaceControl>& sc,
                                            const Region& surfaceDamageRegion);
        Transaction& setApi(const sp<SurfaceControl>& sc, int32_t api);
        Transaction& setSidebandStream(const sp<SurfaceControl>& sc,
                                       const sp<NativeHandle>& sidebandStream);
        Transaction& setDesiredPresentTime(nsecs_t desiredPresentTime);
        Transaction& setColorSpaceAgnostic(const sp<SurfaceControl>& sc, const bool agnostic);

        // Sets information about the priority of the frame.
        Transaction& setFrameRateSelectionPriority(const sp<SurfaceControl>& sc, int32_t priority);

        Transaction& addTransactionCallback(TransactionCompletedCallbackTakesContext callback,
                                            void* callbackContext, CallbackId::Type callbackType);

        Transaction& addTransactionCompletedCallback(
                TransactionCompletedCallbackTakesContext callback, void* callbackContext);

        Transaction& addTransactionCommittedCallback(
                TransactionCompletedCallbackTakesContext callback, void* callbackContext);

        /**
         * Set a callback to receive feedback about the presentation of a layer.
         * When the layer is presented according to the passed in Thresholds,
         * it is said to "enter the state", and receives the callback with true.
         * When the conditions fall out of thresholds, it is then said to leave the
         * state.
         *
         * There are a few simple thresholds:
         *    minAlpha: Lower bound on computed alpha
         *    minFractionRendered: Lower bounds on fraction of pixels that
         *    were rendered.
         *    stabilityThresholdMs: A time that alpha and fraction rendered
         *    must remain within bounds before we can "enter the state"
         *
         * The fraction of pixels rendered is a computation based on scale, crop
         * and occlusion. The calculation may be somewhat counterintuitive, so we
         * can work through an example. Imagine we have a layer with a 100x100 buffer
         * which is occluded by (10x100) pixels on the left, and cropped by (100x10) pixels
         * on the top. Furthermore imagine this layer is scaled by 0.9 in both dimensions.
         * (c=crop,o=occluded,b=both,x=none
         *      b c c c
         *      o x x x
         *      o x x x
         *      o x x x
         *
         * We first start by computing fr=xscale*yscale=0.9*0.9=0.81, indicating
         * that "81%" of the pixels were rendered. This corresponds to what was 100
         * pixels being displayed in 81 pixels. This is somewhat of an abuse of
         * language, as the information of merged pixels isn't totally lost, but
         * we err on the conservative side.
         *
         * We then repeat a similar process for the crop and covered regions and
         * accumulate the results: fr = fr * (fractionNotCropped) * (fractionNotCovered)
         * So for this example we would get 0.9*0.9*0.9*0.9=0.65...
         *
         * Notice that this is not completely accurate, as we have double counted
         * the region marked as b. However we only wanted a "lower bound" and so it
         * is ok to err in this direction. Selection of the threshold will ultimately
         * be somewhat arbitrary, and so there are some somewhat arbitrary decisions in
         * this API as well.
         *
         * The caller must keep "PresentationCallbackRAII" alive, or the callback
         * in SurfaceComposerClient will be unregistered.
         */
        Transaction& setTrustedPresentationCallback(const sp<SurfaceControl>& sc,
                                                    TrustedPresentationCallback callback,
                                                    const TrustedPresentationThresholds& thresholds,
                                                    void* context,
                                                    sp<PresentationCallbackRAII>& outCallbackOwner);

        // Clear local memory in SCC
        Transaction& clearTrustedPresentationCallback(const sp<SurfaceControl>& sc);

        // ONLY FOR BLAST ADAPTER
        Transaction& notifyProducerDisconnect(const sp<SurfaceControl>& sc);

        Transaction& setInputWindowInfo(const sp<SurfaceControl>& sc,
                                        sp<gui::WindowInfoHandle> info);
        Transaction& setFocusedWindow(const gui::FocusRequest& request);

        Transaction& addWindowInfosReportedListener(
                sp<gui::IWindowInfosReportedListener> windowInfosReportedListener);

        // Set a color transform matrix on the given layer on the built-in display.
        Transaction& setColorTransform(const sp<SurfaceControl>& sc, const mat3& matrix,
                                       const vec3& translation);

        Transaction& setGeometry(const sp<SurfaceControl>& sc,
                const Rect& source, const Rect& dst, int transform);
        Transaction& setShadowRadius(const sp<SurfaceControl>& sc, float cornerRadius);

        Transaction& setBorderSettings(const sp<SurfaceControl>& sc, gui::BorderSettings settings);

        Transaction& setFrameRate(const sp<SurfaceControl>& sc, float frameRate,
                                  int8_t compatibility, int8_t changeFrameRateStrategy);

        Transaction& setDefaultFrameRateCompatibility(const sp<SurfaceControl>& sc,
                                                      int8_t compatibility);

        Transaction& setFrameRateCategory(const sp<SurfaceControl>& sc, int8_t category,
                                          bool smoothSwitchOnly);

        Transaction& setFrameRateSelectionStrategy(const sp<SurfaceControl>& sc, int8_t strategy);

        // Set by window manager indicating the layer and all its children are
        // in a different orientation than the display. The hint suggests that
        // the graphic producers should receive a transform hint as if the
        // display was in this orientation. When the display changes to match
        // the layer orientation, the graphic producer may not need to allocate
        // a buffer of a different size.
        Transaction& setFixedTransformHint(const sp<SurfaceControl>& sc, int32_t transformHint);

        // Sets the frame timeline vsync id received from choreographer that corresponds
        // to the transaction, and the input event id that identifies the input event that caused
        // the current frame.
        Transaction& setFrameTimelineInfo(const FrameTimelineInfo& frameTimelineInfo);

        // Indicates that the consumer should acquire the next frame as soon as it
        // can and not wait for a frame to become available. This is only relevant
        // in shared buffer mode.
        Transaction& setAutoRefresh(const sp<SurfaceControl>& sc, bool autoRefresh);

        // Sets that this surface control and its children are trusted overlays for input
        Transaction& setTrustedOverlay(const sp<SurfaceControl>& sc, bool isTrustedOverlay);
        Transaction& setTrustedOverlay(const sp<SurfaceControl>& sc,
                                       gui::TrustedOverlay trustedOverlay);

        // Queues up transactions using this token in SurfaceFlinger.  By default, all transactions
        // from a client are placed on the same queue. This can be used to prevent multiple
        // transactions from blocking each other.
        Transaction& setApplyToken(const sp<IBinder>& token);

        /**
         * Provides the stretch effect configured on a container that the
         * surface is rendered within.
         * @param sc target surface the stretch should be applied to
         * @param stretchEffect the corresponding stretch effect to be applied
         *    to the surface. This can be directly on the surface itself or
         *    configured from a parent of the surface in which case the
         *    StretchEffect provided has parameters mapping the position of
         *    the surface within the container that has the stretch configured
         *    on it
         * @return The transaction being constructed
         */
        Transaction& setStretchEffect(const sp<SurfaceControl>& sc,
                                      const StretchEffect& stretchEffect);

        /**
         * Provides the edge extension effect configured on a container that the
         * surface is rendered within.
         * @param sc target surface the edge extension should be applied to
         * @param effect the corresponding EdgeExtensionParameters to be applied
         *    to the surface.
         * @return The transaction being constructed
         */
        Transaction& setEdgeExtensionEffect(const sp<SurfaceControl>& sc,
                                            const gui::EdgeExtensionParameters& effect);

        Transaction& setBufferCrop(const sp<SurfaceControl>& sc, const Rect& bufferCrop);
        Transaction& setDestinationFrame(const sp<SurfaceControl>& sc,
                                         const Rect& destinationFrame);
        Transaction& setDropInputMode(const sp<SurfaceControl>& sc, gui::DropInputMode mode);

        Transaction& setBufferReleaseChannel(
                const sp<SurfaceControl>& sc,
                const std::shared_ptr<gui::BufferReleaseChannel::ProducerEndpoint>& channel);

        /**
         * Configures a surface control to use picture processing hardware, configured as specified
         * by the picture profile, to enhance the quality of all subsequent buffer contents.
         */
        Transaction& setPictureProfileHandle(const sp<SurfaceControl>& sc,
                                             const PictureProfileHandle& pictureProfileHandle);

        /**
         * Configures the relative importance of the contents of the layer with respect to the app's
         * user experience. A lower priority value will give the layer preferred access to limited
         * resources, such as picture processing, over a layer with a higher priority value.
         */
        Transaction& setContentPriority(const sp<SurfaceControl>& sc, int32_t contentPriority);

        status_t setDisplaySurface(const sp<IBinder>& token,
                const sp<IGraphicBufferProducer>& bufferProducer);

        void setDisplayLayerStack(const sp<IBinder>& token, ui::LayerStack);

        void setDisplayFlags(const sp<IBinder>& token, uint32_t flags);

        /* setDisplayProjection() defines the projection of layer stacks
         * to a given display.
         *
         * - orientation defines the display's orientation.
         * - layerStackRect defines which area of the window manager coordinate
         * space will be used.
         * - displayRect defines where on the display will layerStackRect be
         * mapped to. displayRect is specified post-orientation, that is
         * it uses the orientation seen by the end-user.
         */
        void setDisplayProjection(const sp<IBinder>& token, ui::Rotation orientation,
                                  const Rect& layerStackRect, const Rect& displayRect);
        void setDisplaySize(const sp<IBinder>& token, uint32_t width, uint32_t height);
        void setAnimationTransaction();
        void setEarlyWakeupStart();
        void setEarlyWakeupEnd();

        /**
         * Strip the transaction of all permissioned requests, required when
         * accepting transactions across process boundaries.
         *
         * TODO (b/213644870): Remove all permissioned things from Transaction
         */
        void sanitize(int pid, int uid);

        static sp<IBinder> getDefaultApplyToken();
        static void setDefaultApplyToken(sp<IBinder> applyToken);

        static status_t sendSurfaceFlushJankDataTransaction(const sp<SurfaceControl>& sc);
        void enableDebugLogCallPoints();
    };

    status_t clearLayerFrameStats(const sp<IBinder>& token) const;
    status_t getLayerFrameStats(const sp<IBinder>& token, FrameStats* outStats) const;
    static status_t clearAnimationFrameStats();
    static status_t getAnimationFrameStats(FrameStats* outStats);

    static status_t overrideHdrTypes(const sp<IBinder>& display,
                                     const std::vector<ui::Hdr>& hdrTypes);

    static status_t onPullAtom(const int32_t atomId, std::string* outData, bool* success);

    static void setDisplayProjection(const sp<IBinder>& token, ui::Rotation orientation,
                                     const Rect& layerStackRect, const Rect& displayRect);

    inline sp<ISurfaceComposerClient> getClient() { return mClient; }

    static status_t getDisplayedContentSamplingAttributes(const sp<IBinder>& display,
                                                          ui::PixelFormat* outFormat,
                                                          ui::Dataspace* outDataspace,
                                                          uint8_t* outComponentMask);
    static status_t setDisplayContentSamplingEnabled(const sp<IBinder>& display, bool enable,
                                                     uint8_t componentMask, uint64_t maxFrames);

    static status_t getDisplayedContentSample(const sp<IBinder>& display, uint64_t maxFrames,
                                              uint64_t timestamp, DisplayedFrameStats* outStats);
    static status_t addRegionSamplingListener(const Rect& samplingArea,
                                              const sp<IBinder>& stopLayerHandle,
                                              const sp<IRegionSamplingListener>& listener);
    static status_t removeRegionSamplingListener(const sp<IRegionSamplingListener>& listener);
    static status_t addFpsListener(int32_t taskId, const sp<gui::IFpsListener>& listener);
    static status_t removeFpsListener(const sp<gui::IFpsListener>& listener);
    static status_t addTunnelModeEnabledListener(
            const sp<gui::ITunnelModeEnabledListener>& listener);
    static status_t removeTunnelModeEnabledListener(
            const sp<gui::ITunnelModeEnabledListener>& listener);

    status_t addWindowInfosListener(
            const sp<gui::WindowInfosListener>& windowInfosListener,
            std::pair<std::vector<gui::WindowInfo>, std::vector<gui::DisplayInfo>>* outInitialInfo =
                    nullptr);
    status_t removeWindowInfosListener(const sp<gui::WindowInfosListener>& windowInfosListener);

    static void notifyShutdown();

protected:
    ReleaseCallbackThread mReleaseCallbackThread;

private:
    // Get dynamic information about given physical display from token
    static status_t getDynamicDisplayInfoFromToken(const sp<IBinder>& display,
                                                   ui::DynamicDisplayInfo*);

    static void getDynamicDisplayInfoInternal(gui::DynamicDisplayInfo& ginfo,
                                              ui::DynamicDisplayInfo*& outInfo);
    virtual void onFirstRef();

    mutable     Mutex                       mLock;
                status_t                    mStatus;
                sp<ISurfaceComposerClient>  mClient;
};

// ---------------------------------------------------------------------------

class ScreenshotClient {
public:
    static status_t captureDisplay(const DisplayCaptureArgs&, const sp<IScreenCaptureListener>&);
    static status_t captureDisplay(DisplayId, const gui::CaptureArgs&,
                                   const sp<IScreenCaptureListener>&);
    static status_t captureLayers(const LayerCaptureArgs&, const sp<IScreenCaptureListener>&,
                                  bool sync);

    [[deprecated]] static status_t captureDisplay(DisplayId id,
                                                  const sp<IScreenCaptureListener>& listener) {
        return captureDisplay(id, gui::CaptureArgs(), listener);
    }
};

// ---------------------------------------------------------------------------

class JankDataListener;

// Acts as a representative listener to the composer for a single layer and
// forwards any received jank data to multiple listeners. Will remove itself
// from the composer only once the last listener is removed.
class JankDataListenerFanOut : public gui::BnJankListener {
public:
    JankDataListenerFanOut(int32_t layerId) : mLayerId(layerId) {}

    binder::Status onJankData(const std::vector<gui::JankData>& jankData) override;

    static status_t addListener(sp<SurfaceControl> sc, sp<JankDataListener> listener);
    static status_t removeListener(sp<JankDataListener> listener);

private:
    std::vector<sp<JankDataListener>> getActiveListeners();
    bool removeListeners(const std::vector<wp<JankDataListener>>& listeners);
    int64_t updateAndGetRemovalVSync();

    struct WpJDLHash {
        std::size_t operator()(const wp<JankDataListener>& listener) const {
            return std::hash<JankDataListener*>{}(listener.unsafe_get());
        }
    };

    std::mutex mMutex;
    std::unordered_set<wp<JankDataListener>, WpJDLHash> mListeners GUARDED_BY(mMutex);
    int32_t mLayerId;
    int64_t mRemoveAfter = -1;

    static std::mutex sFanoutInstanceMutex;
    static std::unordered_map<int32_t, sp<JankDataListenerFanOut>> sFanoutInstances;
};

// Base class for client listeners interested in jank classification data from
// the composer. Subclasses should override onJankDataAvailable and call the add
// and removal methods to receive jank data.
class JankDataListener : public virtual RefBase {
public:
    JankDataListener() {}
    virtual ~JankDataListener();

    virtual bool onJankDataAvailable(const std::vector<gui::JankData>& jankData) = 0;

    status_t addListener(sp<SurfaceControl> sc) {
        if (mLayerId != -1) {
            removeListener(0);
            mLayerId = -1;
        }

        int32_t layerId = sc->getLayerId();
        status_t status =
                JankDataListenerFanOut::addListener(std::move(sc),
                                                    sp<JankDataListener>::fromExisting(this));
        if (status == OK) {
            mLayerId = layerId;
        }
        return status;
    }

    status_t removeListener(int64_t afterVsync) {
        mRemoveAfter = std::max(static_cast<int64_t>(0), afterVsync);
        return JankDataListenerFanOut::removeListener(sp<JankDataListener>::fromExisting(this));
    }

    status_t flushJankData();

    friend class JankDataListenerFanOut;

private:
    int32_t mLayerId = -1;
    int64_t mRemoveAfter = -1;
};

// ---------------------------------------------------------------------------

class TransactionCompletedListener : public BnTransactionCompletedListener {
public:
    TransactionCompletedListener();

protected:
    int64_t getNextIdLocked() REQUIRES(mMutex);

    std::mutex mMutex;

    // This lock needs to be recursive so we can unregister a callback from within that callback.
    std::recursive_mutex mSurfaceStatsListenerMutex;

    bool mListening GUARDED_BY(mMutex) = false;

    int64_t mCallbackIdCounter GUARDED_BY(mMutex) = 1;
    struct CallbackTranslation {
        TransactionCompletedCallback callbackFunction;
        std::unordered_map<sp<IBinder>, sp<SurfaceControl>, SurfaceComposerClient::IBinderHash>
                surfaceControls;
    };

    struct SurfaceStatsCallbackEntry {
        SurfaceStatsCallbackEntry(void* context, void* cookie, SurfaceStatsCallback callback)
                : context(context),
                cookie(cookie),
                callback(callback) {}

        void* context;
        void* cookie;
        SurfaceStatsCallback callback;
    };

    std::unordered_map<CallbackId, CallbackTranslation, CallbackIdHash> mCallbacks
            GUARDED_BY(mMutex);
    std::unordered_map<ReleaseCallbackId, ReleaseBufferCallback, ReleaseBufferCallbackIdHash>
            mReleaseBufferCallbacks GUARDED_BY(mMutex);

    // This is protected by mSurfaceStatsListenerMutex, but GUARDED_BY isn't supported for
    // std::recursive_mutex
    std::multimap<int32_t, SurfaceStatsCallbackEntry> mSurfaceStatsListeners;
    std::unordered_map<void*, std::function<void(const std::string&)>> mQueueStallListeners;

    std::unordered_map<int, std::tuple<TrustedPresentationCallback, void*>>
            mTrustedPresentationCallbacks;

public:
    static sp<TransactionCompletedListener> getInstance();
    static sp<ITransactionCompletedListener> getIInstance();

    void startListeningLocked() REQUIRES(mMutex);

    CallbackId addCallbackFunction(
            const TransactionCompletedCallback& callbackFunction,
            const std::unordered_set<sp<SurfaceControl>, SurfaceComposerClient::SCHash>&
                    surfaceControls,
            CallbackId::Type callbackType);

    void addSurfaceControlToCallbacks(
            const sp<SurfaceControl>& surfaceControl,
            const std::unordered_set<CallbackId, CallbackIdHash>& callbackIds);

    void addQueueStallListener(std::function<void(const std::string&)> stallListener, void* id);
    void removeQueueStallListener(void *id);

    sp<SurfaceComposerClient::PresentationCallbackRAII> addTrustedPresentationCallback(
            TrustedPresentationCallback tpc, int id, void* context);
    void clearTrustedPresentationCallback(int id);

    void addSurfaceStatsListener(void* context, void* cookie, sp<SurfaceControl> surfaceControl,
                SurfaceStatsCallback listener);
    void removeSurfaceStatsListener(void* context, void* cookie);

    void setReleaseBufferCallback(const ReleaseCallbackId&, ReleaseBufferCallback);

    // BnTransactionCompletedListener overrides
    void onTransactionCompleted(ListenerStats stats) override;
    void onReleaseBuffer(ReleaseCallbackId, sp<Fence> releaseFence,
                         uint32_t currentMaxAcquiredBufferCount) override;

    void removeReleaseBufferCallback(const ReleaseCallbackId& callbackId);

    // For Testing Only
    static void setInstance(const sp<TransactionCompletedListener>&);

    void onTransactionQueueStalled(const String8& reason) override;

    void onTrustedPresentationChanged(int id, bool presentedWithinThresholds) override;

private:
    ReleaseBufferCallback popReleaseBufferCallbackLocked(const ReleaseCallbackId&) REQUIRES(mMutex);
    static sp<TransactionCompletedListener> sInstance;
};

} // namespace android
