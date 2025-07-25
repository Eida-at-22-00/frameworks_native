/*
 * Copyright (C) 2020 The Android Open Source Project
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

#define LOG_TAG "VibratorHalWrapper"

#include <aidl/android/hardware/vibrator/IVibrator.h>
#include <hardware/vibrator.h>
#include <cmath>

#include <utils/Log.h>

#include <vibratorservice/VibratorCallbackScheduler.h>
#include <vibratorservice/VibratorHalWrapper.h>

using aidl::android::hardware::vibrator::Braking;
using aidl::android::hardware::vibrator::CompositeEffect;
using aidl::android::hardware::vibrator::CompositePrimitive;
using aidl::android::hardware::vibrator::CompositePwleV2;
using aidl::android::hardware::vibrator::Effect;
using aidl::android::hardware::vibrator::EffectStrength;
using aidl::android::hardware::vibrator::FrequencyAccelerationMapEntry;
using aidl::android::hardware::vibrator::IVibrator;
using aidl::android::hardware::vibrator::PrimitivePwle;
using aidl::android::hardware::vibrator::VendorEffect;

using std::chrono::milliseconds;

namespace android {

namespace vibrator {

// -------------------------------------------------------------------------------------------------

Info HalWrapper::getInfo() {
    getCapabilities();
    getPrimitiveDurations();
    std::lock_guard<std::mutex> lock(mInfoMutex);
    if (mInfoCache.mSupportedEffects.isFailed()) {
        mInfoCache.mSupportedEffects = getSupportedEffectsInternal();
    }
    if (mInfoCache.mSupportedBraking.isFailed()) {
        mInfoCache.mSupportedBraking = getSupportedBrakingInternal();
    }
    if (mInfoCache.mPrimitiveDelayMax.isFailed()) {
        mInfoCache.mPrimitiveDelayMax = getPrimitiveDelayMaxInternal();
    }
    if (mInfoCache.mPwlePrimitiveDurationMax.isFailed()) {
        mInfoCache.mPwlePrimitiveDurationMax = getPrimitiveDurationMaxInternal();
    }
    if (mInfoCache.mCompositionSizeMax.isFailed()) {
        mInfoCache.mCompositionSizeMax = getCompositionSizeMaxInternal();
    }
    if (mInfoCache.mPwleSizeMax.isFailed()) {
        mInfoCache.mPwleSizeMax = getPwleSizeMaxInternal();
    }
    if (mInfoCache.mMinFrequency.isFailed()) {
        mInfoCache.mMinFrequency = getMinFrequencyInternal();
    }
    if (mInfoCache.mResonantFrequency.isFailed()) {
        mInfoCache.mResonantFrequency = getResonantFrequencyInternal();
    }
    if (mInfoCache.mFrequencyResolution.isFailed()) {
        mInfoCache.mFrequencyResolution = getFrequencyResolutionInternal();
    }
    if (mInfoCache.mQFactor.isFailed()) {
        mInfoCache.mQFactor = getQFactorInternal();
    }
    if (mInfoCache.mMaxAmplitudes.isFailed()) {
        mInfoCache.mMaxAmplitudes = getMaxAmplitudesInternal();
    }
    if (mInfoCache.mMaxEnvelopeEffectSize.isFailed()) {
        mInfoCache.mMaxEnvelopeEffectSize = getMaxEnvelopeEffectSizeInternal();
    }
    if (mInfoCache.mMinEnvelopeEffectControlPointDuration.isFailed()) {
        mInfoCache.mMinEnvelopeEffectControlPointDuration =
                getMinEnvelopeEffectControlPointDurationInternal();
    }
    if (mInfoCache.mMaxEnvelopeEffectControlPointDuration.isFailed()) {
        mInfoCache.mMaxEnvelopeEffectControlPointDuration =
                getMaxEnvelopeEffectControlPointDurationInternal();
    }
    if (mInfoCache.mFrequencyToOutputAccelerationMap.isFailed()) {
        mInfoCache.mFrequencyToOutputAccelerationMap =
                getFrequencyToOutputAccelerationMapInternal();
    }
    return mInfoCache.get();
}

HalResult<void> HalWrapper::performVendorEffect(const VendorEffect&, const std::function<void()>&) {
    ALOGV("Skipped performVendorEffect because it's not available in Vibrator HAL");
    return HalResult<void>::unsupported();
}

HalResult<milliseconds> HalWrapper::performComposedEffect(const std::vector<CompositeEffect>&,
                                                          const std::function<void()>&) {
    ALOGV("Skipped performComposedEffect because it's not available in Vibrator HAL");
    return HalResult<milliseconds>::unsupported();
}

HalResult<void> HalWrapper::performPwleEffect(const std::vector<PrimitivePwle>&,
                                              const std::function<void()>&) {
    ALOGV("Skipped performPwleEffect because it's not available in Vibrator HAL");
    return HalResult<void>::unsupported();
}

HalResult<milliseconds> HalWrapper::composePwleV2(const CompositePwleV2&,
                                                  const std::function<void()>&) {
    ALOGV("Skipped composePwleV2 because it's not available in Vibrator HAL");
    return HalResult<milliseconds>::unsupported();
}

HalResult<Capabilities> HalWrapper::getCapabilities() {
    std::lock_guard<std::mutex> lock(mInfoMutex);
    if (mInfoCache.mCapabilities.isFailed()) {
        mInfoCache.mCapabilities = getCapabilitiesInternal();
    }
    return mInfoCache.mCapabilities;
}

HalResult<std::vector<milliseconds>> HalWrapper::getPrimitiveDurations() {
    std::lock_guard<std::mutex> lock(mInfoMutex);
    if (mInfoCache.mSupportedPrimitives.isFailed()) {
        mInfoCache.mSupportedPrimitives = getSupportedPrimitivesInternal();
        if (mInfoCache.mSupportedPrimitives.isUnsupported()) {
            mInfoCache.mPrimitiveDurations = HalResult<std::vector<milliseconds>>::unsupported();
        }
    }
    if (mInfoCache.mPrimitiveDurations.isFailed() && mInfoCache.mSupportedPrimitives.isOk()) {
        mInfoCache.mPrimitiveDurations =
                getPrimitiveDurationsInternal(mInfoCache.mSupportedPrimitives.value());
    }
    return mInfoCache.mPrimitiveDurations;
}

HalResult<std::vector<Effect>> HalWrapper::getSupportedEffectsInternal() {
    ALOGV("Skipped getSupportedEffects because it's not available in Vibrator HAL");
    return HalResult<std::vector<Effect>>::unsupported();
}

HalResult<std::vector<Braking>> HalWrapper::getSupportedBrakingInternal() {
    ALOGV("Skipped getSupportedBraking because it's not available in Vibrator HAL");
    return HalResult<std::vector<Braking>>::unsupported();
}

HalResult<std::vector<CompositePrimitive>> HalWrapper::getSupportedPrimitivesInternal() {
    ALOGV("Skipped getSupportedPrimitives because it's not available in Vibrator HAL");
    return HalResult<std::vector<CompositePrimitive>>::unsupported();
}

HalResult<std::vector<milliseconds>> HalWrapper::getPrimitiveDurationsInternal(
        const std::vector<CompositePrimitive>&) {
    ALOGV("Skipped getPrimitiveDurations because it's not available in Vibrator HAL");
    return HalResult<std::vector<milliseconds>>::unsupported();
}

HalResult<milliseconds> HalWrapper::getPrimitiveDelayMaxInternal() {
    ALOGV("Skipped getPrimitiveDelayMaxInternal because it's not available in Vibrator HAL");
    return HalResult<milliseconds>::unsupported();
}

HalResult<milliseconds> HalWrapper::getPrimitiveDurationMaxInternal() {
    ALOGV("Skipped getPrimitiveDurationMaxInternal because it's not available in Vibrator HAL");
    return HalResult<milliseconds>::unsupported();
}

HalResult<int32_t> HalWrapper::getCompositionSizeMaxInternal() {
    ALOGV("Skipped getCompositionSizeMaxInternal because it's not available in Vibrator HAL");
    return HalResult<int32_t>::unsupported();
}

HalResult<int32_t> HalWrapper::getPwleSizeMaxInternal() {
    ALOGV("Skipped getPwleSizeMaxInternal because it's not available in Vibrator HAL");
    return HalResult<int32_t>::unsupported();
}

HalResult<float> HalWrapper::getMinFrequencyInternal() {
    ALOGV("Skipped getMinFrequency because it's not available in Vibrator HAL");
    return HalResult<float>::unsupported();
}

HalResult<float> HalWrapper::getResonantFrequencyInternal() {
    ALOGV("Skipped getResonantFrequency because it's not available in Vibrator HAL");
    return HalResult<float>::unsupported();
}

HalResult<float> HalWrapper::getFrequencyResolutionInternal() {
    ALOGV("Skipped getFrequencyResolution because it's not available in Vibrator HAL");
    return HalResult<float>::unsupported();
}

HalResult<float> HalWrapper::getQFactorInternal() {
    ALOGV("Skipped getQFactor because it's not available in Vibrator HAL");
    return HalResult<float>::unsupported();
}

HalResult<std::vector<float>> HalWrapper::getMaxAmplitudesInternal() {
    ALOGV("Skipped getMaxAmplitudes because it's not available in Vibrator HAL");
    return HalResult<std::vector<float>>::unsupported();
}
HalResult<int32_t> HalWrapper::getMaxEnvelopeEffectSizeInternal() {
    ALOGV("Skipped getMaxEnvelopeEffectSizeInternal because it's not available "
          "in Vibrator HAL");
    return HalResult<int32_t>::unsupported();
}

HalResult<milliseconds> HalWrapper::getMinEnvelopeEffectControlPointDurationInternal() {
    ALOGV("Skipped getMinEnvelopeEffectControlPointDurationInternal because it's not "
          "available in Vibrator HAL");
    return HalResult<milliseconds>::unsupported();
}

HalResult<milliseconds> HalWrapper::getMaxEnvelopeEffectControlPointDurationInternal() {
    ALOGV("Skipped getMaxEnvelopeEffectControlPointDurationInternal because it's not "
          "available in Vibrator HAL");
    return HalResult<milliseconds>::unsupported();
}

HalResult<std::vector<FrequencyAccelerationMapEntry>>
HalWrapper::getFrequencyToOutputAccelerationMapInternal() {
    ALOGV("Skipped getFrequencyToOutputAccelerationMapInternal because it's not "
          "available in Vibrator HAL");
    return HalResult<std::vector<FrequencyAccelerationMapEntry>>::unsupported();
}

// -------------------------------------------------------------------------------------------------

HalResult<void> AidlHalWrapper::ping() {
    return HalResultFactory::fromStatus(AIBinder_ping(getHal()->asBinder().get()));
}

void AidlHalWrapper::tryReconnect() {
    auto result = mReconnectFn();
    if (!result.isOk()) {
        return;
    }
    std::shared_ptr<IVibrator> newHandle = result.value();
    if (newHandle) {
        std::lock_guard<std::mutex> lock(mHandleMutex);
        mHandle = std::move(newHandle);
    }
}

HalResult<void> AidlHalWrapper::on(milliseconds timeout,
                                   const std::function<void()>& completionCallback) {
    HalResult<Capabilities> capabilities = getCapabilities();
    bool supportsCallback = capabilities.isOk() &&
            static_cast<int32_t>(capabilities.value() & Capabilities::ON_CALLBACK);
    auto cb = supportsCallback ? ndk::SharedRefBase::make<HalCallbackWrapper>(completionCallback)
                               : nullptr;

    auto ret = HalResultFactory::fromStatus(getHal()->on(timeout.count(), cb));
    if (!supportsCallback && ret.isOk()) {
        mCallbackScheduler->schedule(completionCallback, timeout);
    }

    return ret;
}

HalResult<void> AidlHalWrapper::off() {
    return HalResultFactory::fromStatus(getHal()->off());
}

HalResult<void> AidlHalWrapper::setAmplitude(float amplitude) {
    return HalResultFactory::fromStatus(getHal()->setAmplitude(amplitude));
}

HalResult<void> AidlHalWrapper::setExternalControl(bool enabled) {
    return HalResultFactory::fromStatus(getHal()->setExternalControl(enabled));
}

HalResult<void> AidlHalWrapper::alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) {
    return HalResultFactory::fromStatus(getHal()->alwaysOnEnable(id, effect, strength));
}

HalResult<void> AidlHalWrapper::alwaysOnDisable(int32_t id) {
    return HalResultFactory::fromStatus(getHal()->alwaysOnDisable(id));
}

HalResult<milliseconds> AidlHalWrapper::performEffect(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    HalResult<Capabilities> capabilities = getCapabilities();
    bool supportsCallback = capabilities.isOk() &&
            static_cast<int32_t>(capabilities.value() & Capabilities::PERFORM_CALLBACK);
    auto cb = supportsCallback ? ndk::SharedRefBase::make<HalCallbackWrapper>(completionCallback)
                               : nullptr;

    int32_t lengthMs;
    auto status = getHal()->perform(effect, strength, cb, &lengthMs);
    milliseconds length = milliseconds(lengthMs);

    auto ret = HalResultFactory::fromStatus<milliseconds>(std::move(status), length);
    if (!supportsCallback && ret.isOk()) {
        mCallbackScheduler->schedule(completionCallback, length);
    }

    return ret;
}

HalResult<void> AidlHalWrapper::performVendorEffect(
        const VendorEffect& effect, const std::function<void()>& completionCallback) {
    // This method should always support callbacks, so no need to double check.
    auto cb = ndk::SharedRefBase::make<HalCallbackWrapper>(completionCallback);
    return HalResultFactory::fromStatus(getHal()->performVendorEffect(effect, cb));
}

HalResult<milliseconds> AidlHalWrapper::performComposedEffect(
        const std::vector<CompositeEffect>& primitives,
        const std::function<void()>& completionCallback) {
    // This method should always support callbacks, so no need to double check.
    auto cb = ndk::SharedRefBase::make<HalCallbackWrapper>(completionCallback);

    auto durations = getPrimitiveDurations().valueOr({});
    milliseconds duration(0);
    for (const auto& effect : primitives) {
        auto primitiveIdx = static_cast<size_t>(effect.primitive);
        if (primitiveIdx < durations.size()) {
            duration += durations[primitiveIdx];
        } else {
            // Make sure the returned duration is positive to indicate successful vibration.
            duration += milliseconds(1);
        }
        duration += milliseconds(effect.delayMs);
    }

    return HalResultFactory::fromStatus<milliseconds>(getHal()->compose(primitives, cb), duration);
}

HalResult<void> AidlHalWrapper::performPwleEffect(const std::vector<PrimitivePwle>& primitives,
                                                  const std::function<void()>& completionCallback) {
    // This method should always support callbacks, so no need to double check.
    auto cb = ndk::SharedRefBase::make<HalCallbackWrapper>(completionCallback);
    return HalResultFactory::fromStatus(getHal()->composePwle(primitives, cb));
}

HalResult<milliseconds> AidlHalWrapper::composePwleV2(
        const CompositePwleV2& composite, const std::function<void()>& completionCallback) {
    // This method should always support callbacks, so no need to double check.
    auto cb = ndk::SharedRefBase::make<HalCallbackWrapper>(completionCallback);

    milliseconds totalDuration(0);
    for (const auto& primitive : composite.pwlePrimitives) {
        totalDuration += milliseconds(primitive.timeMillis);
    }

    return HalResultFactory::fromStatus<milliseconds>(getHal()->composePwleV2(composite, cb),
                                                      totalDuration);
}

HalResult<Capabilities> AidlHalWrapper::getCapabilitiesInternal() {
    int32_t cap = 0;
    auto status = getHal()->getCapabilities(&cap);
    auto capabilities = static_cast<Capabilities>(cap);
    return HalResultFactory::fromStatus<Capabilities>(std::move(status), capabilities);
}

HalResult<std::vector<Effect>> AidlHalWrapper::getSupportedEffectsInternal() {
    std::vector<Effect> supportedEffects;
    auto status = getHal()->getSupportedEffects(&supportedEffects);
    return HalResultFactory::fromStatus<std::vector<Effect>>(std::move(status), supportedEffects);
}

HalResult<std::vector<Braking>> AidlHalWrapper::getSupportedBrakingInternal() {
    std::vector<Braking> supportedBraking;
    auto status = getHal()->getSupportedBraking(&supportedBraking);
    return HalResultFactory::fromStatus<std::vector<Braking>>(std::move(status), supportedBraking);
}

HalResult<std::vector<CompositePrimitive>> AidlHalWrapper::getSupportedPrimitivesInternal() {
    std::vector<CompositePrimitive> supportedPrimitives;
    auto status = getHal()->getSupportedPrimitives(&supportedPrimitives);
    return HalResultFactory::fromStatus<std::vector<CompositePrimitive>>(std::move(status),
                                                                         supportedPrimitives);
}

HalResult<std::vector<milliseconds>> AidlHalWrapper::getPrimitiveDurationsInternal(
        const std::vector<CompositePrimitive>& supportedPrimitives) {
    std::vector<milliseconds> durations;
    constexpr auto primitiveRange = ndk::enum_range<CompositePrimitive>();
    constexpr auto primitiveCount = std::distance(primitiveRange.begin(), primitiveRange.end());
    durations.resize(primitiveCount);

    for (auto primitive : supportedPrimitives) {
        auto primitiveIdx = static_cast<size_t>(primitive);
        if (primitiveIdx >= durations.size()) {
            // Safety check, should not happen if enum_range is correct.
            ALOGE("Supported primitive %zu is outside range [0,%zu), skipping load duration",
                  primitiveIdx, durations.size());
            continue;
        }
        int32_t duration = 0;
        auto status = getHal()->getPrimitiveDuration(primitive, &duration);
        auto halResult = HalResultFactory::fromStatus<int32_t>(std::move(status), duration);
        if (halResult.isUnsupported()) {
            // Should not happen, supported primitives should always support requesting duration.
            ALOGE("Supported primitive %zu returned unsupported for getPrimitiveDuration",
                  primitiveIdx);
        }
        if (halResult.isFailed()) {
            // Fail entire request if one request has failed.
            return HalResult<std::vector<milliseconds>>::failed(halResult.errorMessage());
        }
        durations[primitiveIdx] = milliseconds(duration);
    }

    return HalResult<std::vector<milliseconds>>::ok(durations);
}

HalResult<milliseconds> AidlHalWrapper::getPrimitiveDelayMaxInternal() {
    int32_t delay = 0;
    auto status = getHal()->getCompositionDelayMax(&delay);
    return HalResultFactory::fromStatus<milliseconds>(std::move(status), milliseconds(delay));
}

HalResult<milliseconds> AidlHalWrapper::getPrimitiveDurationMaxInternal() {
    int32_t delay = 0;
    auto status = getHal()->getPwlePrimitiveDurationMax(&delay);
    return HalResultFactory::fromStatus<milliseconds>(std::move(status), milliseconds(delay));
}

HalResult<int32_t> AidlHalWrapper::getCompositionSizeMaxInternal() {
    int32_t size = 0;
    auto status = getHal()->getCompositionSizeMax(&size);
    return HalResultFactory::fromStatus<int32_t>(std::move(status), size);
}

HalResult<int32_t> AidlHalWrapper::getPwleSizeMaxInternal() {
    int32_t size = 0;
    auto status = getHal()->getPwleCompositionSizeMax(&size);
    return HalResultFactory::fromStatus<int32_t>(std::move(status), size);
}

HalResult<float> AidlHalWrapper::getMinFrequencyInternal() {
    float minFrequency = 0;
    auto status = getHal()->getFrequencyMinimum(&minFrequency);
    return HalResultFactory::fromStatus<float>(std::move(status), minFrequency);
}

HalResult<float> AidlHalWrapper::getResonantFrequencyInternal() {
    float f0 = 0;
    auto status = getHal()->getResonantFrequency(&f0);
    return HalResultFactory::fromStatus<float>(std::move(status), f0);
}

HalResult<float> AidlHalWrapper::getFrequencyResolutionInternal() {
    float frequencyResolution = 0;
    auto status = getHal()->getFrequencyResolution(&frequencyResolution);
    return HalResultFactory::fromStatus<float>(std::move(status), frequencyResolution);
}

HalResult<float> AidlHalWrapper::getQFactorInternal() {
    float qFactor = 0;
    auto status = getHal()->getQFactor(&qFactor);
    return HalResultFactory::fromStatus<float>(std::move(status), qFactor);
}

HalResult<std::vector<float>> AidlHalWrapper::getMaxAmplitudesInternal() {
    std::vector<float> amplitudes;
    auto status = getHal()->getBandwidthAmplitudeMap(&amplitudes);
    return HalResultFactory::fromStatus<std::vector<float>>(std::move(status), amplitudes);
}

HalResult<int32_t> AidlHalWrapper::getMaxEnvelopeEffectSizeInternal() {
    int32_t size = 0;
    auto status = getHal()->getPwleV2CompositionSizeMax(&size);
    return HalResultFactory::fromStatus<int32_t>(std::move(status), size);
}

HalResult<milliseconds> AidlHalWrapper::getMinEnvelopeEffectControlPointDurationInternal() {
    int32_t durationMs = 0;
    auto status = getHal()->getPwleV2PrimitiveDurationMinMillis(&durationMs);
    return HalResultFactory::fromStatus<milliseconds>(std::move(status), milliseconds(durationMs));
}

HalResult<milliseconds> AidlHalWrapper::getMaxEnvelopeEffectControlPointDurationInternal() {
    int32_t durationMs = 0;
    auto status = getHal()->getPwleV2PrimitiveDurationMaxMillis(&durationMs);
    return HalResultFactory::fromStatus<milliseconds>(std::move(status), milliseconds(durationMs));
}

HalResult<std::vector<FrequencyAccelerationMapEntry>>
AidlHalWrapper::getFrequencyToOutputAccelerationMapInternal() {
    std::vector<FrequencyAccelerationMapEntry> frequencyToOutputAccelerationMap;
    auto status = getHal()->getFrequencyToOutputAccelerationMap(&frequencyToOutputAccelerationMap);
    return HalResultFactory::fromStatus<
            std::vector<FrequencyAccelerationMapEntry>>(std::move(status),
                                                        frequencyToOutputAccelerationMap);
}

std::shared_ptr<IVibrator> AidlHalWrapper::getHal() {
    std::lock_guard<std::mutex> lock(mHandleMutex);
    return mHandle;
}

// -------------------------------------------------------------------------------------------------

}; // namespace vibrator

}; // namespace android
