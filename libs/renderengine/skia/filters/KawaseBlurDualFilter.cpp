/*
 * Copyright 2024 The Android Open Source Project
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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "KawaseBlurDualFilter.h"
#include <SkAlphaType.h>
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkData.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkShader.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <log/log.h>
#include <utils/Trace.h>

namespace android {
namespace renderengine {
namespace skia {

KawaseBlurDualFilter::KawaseBlurDualFilter() : BlurFilter() {
    // A shader to sample each vertex of a square, plus the original fragment coordinate,
    // using a total of 5 samples.
    SkString lowSampleBlurString(R"(
        uniform shader child;
        uniform float in_blurOffset;
        uniform float in_crossFade;
        uniform float in_weightedCrossFade;

        const float2 STEP_0 = float2( 0.707106781, 0.707106781);
        const float2 STEP_1 = float2( 0.707106781, -0.707106781);
        const float2 STEP_2 = float2(-0.707106781, -0.707106781);
        const float2 STEP_3 = float2(-0.707106781, 0.707106781);

        half4 main(float2 xy) {
            half3 c = child.eval(xy).rgb;

            c += child.eval(xy + STEP_0 * in_blurOffset).rgb;
            c += child.eval(xy + STEP_1 * in_blurOffset).rgb;
            c += child.eval(xy + STEP_2 * in_blurOffset).rgb;
            c += child.eval(xy + STEP_3 * in_blurOffset).rgb;

            return half4(c * in_weightedCrossFade, in_crossFade);
        }
    )");

    // A shader to sample each vertex of a unit regular heptagon, plus the original fragment
    // coordinate, using a total of 8 samples.
    SkString highSampleBlurString(R"(
        uniform shader child;
        uniform float in_blurOffset;

        const float2 STEP_0 = float2( 1.0, 0.0);
        const float2 STEP_1 = float2( 0.623489802,  0.781831482);
        const float2 STEP_2 = float2(-0.222520934,  0.974927912);
        const float2 STEP_3 = float2(-0.900968868,  0.433883739);
        const float2 STEP_4 = float2( 0.900968868, -0.433883739);
        const float2 STEP_5 = float2(-0.222520934, -0.974927912);
        const float2 STEP_6 = float2(-0.623489802, -0.781831482);

        half4 main(float2 xy) {
            half3 c = child.eval(xy).rgb;

            c += child.eval(xy + STEP_0 * in_blurOffset).rgb;
            c += child.eval(xy + STEP_1 * in_blurOffset).rgb;
            c += child.eval(xy + STEP_2 * in_blurOffset).rgb;
            c += child.eval(xy + STEP_3 * in_blurOffset).rgb;
            c += child.eval(xy + STEP_4 * in_blurOffset).rgb;
            c += child.eval(xy + STEP_5 * in_blurOffset).rgb;
            c += child.eval(xy + STEP_6 * in_blurOffset).rgb;

            return half4(c * 0.125, 1.0);
        }
    )");

    auto [lowSampleBlurEffect, error] = SkRuntimeEffect::MakeForShader(lowSampleBlurString);
    auto [highSampleBlurEffect, error2] = SkRuntimeEffect::MakeForShader(highSampleBlurString);
    LOG_ALWAYS_FATAL_IF(!lowSampleBlurEffect, "RuntimeShader error: %s", error.c_str());
    LOG_ALWAYS_FATAL_IF(!highSampleBlurEffect, "RuntimeShader error: %s", error2.c_str());
    mLowSampleBlurEffect = std::move(lowSampleBlurEffect);
    mHighSampleBlurEffect = std::move(highSampleBlurEffect);
}

void KawaseBlurDualFilter::blurInto(const sk_sp<SkSurface>& drawSurface,
                                    const sk_sp<SkImage>& readImage, const float radius,
                                    const float alpha,
                                    const sk_sp<SkRuntimeEffect>& blurEffect) const {
    const float scale = static_cast<float>(drawSurface->width()) / readImage->width();
    SkMatrix blurMatrix = SkMatrix::Scale(scale, scale);
    blurInto(drawSurface,
             readImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                   SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                                   blurMatrix),
             radius, alpha, blurEffect);
}

void KawaseBlurDualFilter::blurInto(const sk_sp<SkSurface>& drawSurface, sk_sp<SkShader> input,
                                    const float radius, const float alpha,
                                    const sk_sp<SkRuntimeEffect>& blurEffect) const {
    SkPaint paint;
    if (radius == 0) {
        paint.setShader(std::move(input));
        paint.setAlphaf(alpha);
    } else {
        SkRuntimeShaderBuilder blurBuilder(blurEffect);
        blurBuilder.child("child") = std::move(input);
        if (blurEffect == mLowSampleBlurEffect) {
            blurBuilder.uniform("in_crossFade") = alpha;
            blurBuilder.uniform("in_weightedCrossFade") = alpha * 0.2f;
        }
        blurBuilder.uniform("in_blurOffset") = radius;
        paint.setShader(blurBuilder.makeShader(nullptr));
    }
    paint.setBlendMode(alpha == 1.0f ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);
    drawSurface->getCanvas()->drawPaint(paint);
}

sk_sp<SkImage> KawaseBlurDualFilter::generate(SkiaGpuContext* context, const uint32_t blurRadius,
                                              const sk_sp<SkImage> input,
                                              const SkRect& blurRect) const {
    // Apply a conversion factor of (1 / sqrt(3)) to match Skia's built-in blur as used by
    // RenderEffect. See the comment in SkBlurMask.cpp for reasoning behind this.
    const float radius = blurRadius * 0.57735f;

    // Use a variable number of blur passes depending on the radius. The non-integer part of this
    // calculation is used to mix the final pass into the second-last with an alpha blend.
    constexpr int kMaxSurfaces = 3;
    const float filterDepth = std::min(kMaxSurfaces - 1.0f, radius * kInputScale / 2.5f);
    const int filterPasses = std::min(kMaxSurfaces - 1, static_cast<int>(ceil(filterDepth)));

    auto makeSurface = [&](float scale) -> sk_sp<SkSurface> {
        const auto newW = ceil(static_cast<float>(blurRect.width() / scale));
        const auto newH = ceil(static_cast<float>(blurRect.height() / scale));
        sk_sp<SkSurface> surface =
                context->createRenderTarget(input->imageInfo().makeWH(newW, newH));
        LOG_ALWAYS_FATAL_IF(!surface, "%s: Failed to create surface for blurring!", __func__);
        return surface;
    };

    // Render into surfaces downscaled by 1x, 2x, and 4x from the initial downscale.
    sk_sp<SkSurface> surfaces[kMaxSurfaces] =
            {filterPasses >= 0 ? makeSurface(1 * kInverseInputScale) : nullptr,
             filterPasses >= 1 ? makeSurface(2 * kInverseInputScale) : nullptr,
             filterPasses >= 2 ? makeSurface(4 * kInverseInputScale) : nullptr};

    // These weights for scaling offsets per-pass are handpicked to look good at 1 <= radius <= 250.
    static const float kWeights[5] = {
            1.0f, // 1st downsampling pass
            1.0f, // 2nd downsampling pass
            1.0f, // 3rd downsampling pass
            0.0f, // 1st upscaling pass. Set to zero to upscale without blurring for performance.
            1.0f, // 2nd upscaling pass
    };

    // Kawase is an approximation of Gaussian, but behaves differently because it is made up of many
    // simpler blurs. A transformation is required to approximate the same effect as Gaussian.
    float sumSquaredR = powf(kWeights[0], 2.0f);
    for (int i = 0; i < filterPasses; i++) {
        const float alpha = std::min(1.0f, filterDepth - i);
        sumSquaredR += powf(powf(2.0f, i) * alpha * kWeights[1 + i] / kInputScale, 2.0f);
        sumSquaredR += powf(powf(2.0f, i + 1) * alpha * kWeights[4 - i] / kInputScale, 2.0f);
    }
    // Solve for R = sqrt(sum(r_i^2)).
    const float step = radius * sqrt(1.0f / sumSquaredR);

    // Start by downscaling and doing the first blur pass.
    {
        // For sampling Skia's API expects the inverse of what logically seems appropriate. In this
        // case one may expect Translate(blurRect.fLeft, blurRect.fTop) * Scale(kInverseInputScale)
        // but instead we must do the inverse.
        SkMatrix blurMatrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
        blurMatrix.postScale(kInputScale, kInputScale);
        const auto sourceShader =
                input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                  SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                                  blurMatrix);
        blurInto(surfaces[0], std::move(sourceShader), kWeights[0] * step, 1.0f,
                 mLowSampleBlurEffect);
    }
    // Next the remaining downscale blur passes.
    for (int i = 0; i < filterPasses; i++) {
        // Blur with the higher sample effect into the smaller buffers, for better visual quality.
        blurInto(surfaces[i + 1], surfaces[i]->makeTemporaryImage(), kWeights[1 + i] * step, 1.0f,
                 i == 0 ? mLowSampleBlurEffect : mHighSampleBlurEffect);
    }
    // Finally blur+upscale back to our original size.
    for (int i = filterPasses - 1; i >= 0; i--) {
        blurInto(surfaces[i], surfaces[i + 1]->makeTemporaryImage(), kWeights[4 - i] * step,
                 std::min(1.0f, filterDepth - i), mLowSampleBlurEffect);
    }
    return surfaces[0]->makeTemporaryImage();
}

} // namespace skia
} // namespace renderengine
} // namespace android
