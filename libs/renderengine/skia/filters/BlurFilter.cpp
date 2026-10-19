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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "BlurFilter.h"
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkM44.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <android-base/parseint.h>
#include <common/ThreadStateCrashLogger.h>
#include <common/trace.h>
#include <log/log.h>

#include <algorithm>
#include <cmath>

#include "RuntimeEffectManager.h"

#include <renderengine/RrGlass.h>

#include <atomic>

namespace android::renderengine::rrglass {

static constexpr float kDefaultLightX = -0.5f;
static constexpr float kDefaultLightY = -0.866f;

static std::atomic<float> sLightX{kDefaultLightX};
static std::atomic<float> sLightY{kDefaultLightY};
static std::atomic<uint32_t> sLightSerial{0};
static std::atomic<nsecs_t> sLastDraw{0};

void setLight(float x, float y) {
    const float length = std::sqrt(x * x + y * y);
    if (length > 0.001f) {
        x /= length;
        y /= length;
    } else {
        x = kDefaultLightX;
        y = kDefaultLightY;
    }
    sLightX.store(x);
    sLightY.store(y);
    sLightSerial.fetch_add(1);
}

uint32_t lightSerial() {
    return sLightSerial.load();
}

bool drawnRecently(nsecs_t now) {
    return now - sLastDraw.load() < ms2ns(500);
}

} // namespace android::renderengine::rrglass

namespace android {
namespace renderengine {
namespace skia {

const SkString kEffectSource_BlurFilter_MixEffect(R"(
    uniform shader blurredInput;
    uniform shader originalInput;
    uniform float mixFactor;

    half4 main(float2 xy) {
        return half4(mix(originalInput.eval(xy), blurredInput.eval(xy), mixFactor)).rgb1;
    }
)");

// Liquid Glass: a lens at the edge of rounded blur regions (refraction, a little dispersion,
// a lit rim), plus the user's tint. Full-screen backdrops only take the tint. The rim bends the
// sharp scene; bending only the blur barely shows.
static const SkString kGlassEffectSource(R"(
    uniform shader blurred;
    uniform shader sharp;
    uniform float4 rect;
    uniform float cornerRadius;
    uniform float liquid;
    uniform float refraction;
    uniform float dim;
    uniform float4 tint;
    uniform float2 lightDir;

    float boxSdf(float2 p, float2 halfSize, float r) {
        float2 q = abs(p) - halfSize + float2(r);
        return length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    }

    half4 main(float2 xy) {
        float3 color = float3(0.0);
        if (liquid > 0.5) {
            float2 halfSize = max((rect.zw - rect.xy) * 0.5, float2(1.0));
            float2 p = xy - (rect.xy + rect.zw) * 0.5;
            float r = min(cornerRadius, min(halfSize.x, halfSize.y));
            float d = boxSdf(p, halfSize, r);
            float2 s = float2(p.x < 0.0 ? -1.0 : 1.0, p.y < 0.0 ? -1.0 : 1.0);
            float2 a = clamp(abs(p) / halfSize, float2(0.0), float2(1.0));
            float2 n = normalize(s * pow(a, float2(6.0)) + float2(0.00001));
            float band = clamp(min(halfSize.x, halfSize.y) * 0.5, 6.0, 96.0);
            float edge = 1.0 - clamp(-d / band, 0.0, 1.0);
            float bend = edge * edge * edge;
            float2 uv = xy - n * (bend * band * refraction * 0.9);
            float split = bend * refraction * 4.0;
            float3 soft = float3(blurred.eval(uv).rgb);
            soft.r = float(blurred.eval(uv + n * split).r);
            soft.b = float(blurred.eval(uv - n * split).b);
            float3 crisp = float3(sharp.eval(uv).rgb);
            crisp.r = float(sharp.eval(uv + n * split).r);
            crisp.b = float(sharp.eval(uv - n * split).b);
            color = mix(soft, crisp, smoothstep(0.35, 1.0, edge) * 0.8);
            float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
            color = mix(float3(luma), color, 1.2) * 1.03;
            color = mix(color, tint.rgb, tint.a) * (1.0 - dim);
            float rim = 1.0 - smoothstep(0.0, 1.5 + 3.5 * refraction, -d);
            float light = dot(n, lightDir);
            color += rim * (0.08 + 0.5 * max(light, 0.0) + 0.18 * max(-light, 0.0)) *
                    (0.5 + 0.5 * refraction) + edge * edge * (0.04 + 0.1 * max(light, 0.0));
        } else {
            color = float3(blurred.eval(xy).rgb);
            color = mix(color, tint.rgb, tint.a) * (1.0 - dim);
        }
        return half4(half3(clamp(color, float3(0.0), float3(1.0))), 1.0);
    }
)");

static constexpr int kGlassLiquid = 2;

static sk_sp<SkRuntimeEffect> makeGlassEffect() {
    auto [effect, error] = SkRuntimeEffect::MakeForShader(kGlassEffectSource);
    if (!effect) {
        ALOGE("Liquid Glass effect failed to compile: %s", error.c_str());
    }
    return effect;
}

// The unblurred input, mapped onto the canvas.
static sk_sp<SkShader> makeInputShader(const SkCanvas* canvas, const sk_sp<SkImage>& input,
                                       const SkRect& blurRect, const float zoomScale,
                                       const SkSamplingOptions& sampling) {
    // For sampling Skia's API expects the inverse of what logically seems appropriate. In this
    // case you might expect the matrix to simply be the canvas matrix.
    SkMatrix inputMatrix;
    if (!canvas->getTotalMatrix().invert(&inputMatrix)) {
        ALOGE("matrix was unable to be inverted");
    }
    if (zoomScale != 1.0f) {
        inputMatrix.preTranslate(
                blurRect.width() * (1 - zoomScale) / 2.0f,
                blurRect.height() * (1 - zoomScale) / 2.0f);
        inputMatrix.preScale(zoomScale, zoomScale);
    }
    return input->makeShader(SkTileMode::kMirror, SkTileMode::kMirror, sampling, inputMatrix);
}

static SkMatrix getShaderTransform(const SkCanvas* canvas, const SkRect& blurRect,
                                   const float scale, const float zoomScale) {
    // 1. Apply the blur shader matrix, which scales up the blurred surface to its real size
    auto matrix = SkMatrix::Scale(scale, scale);
    // 2. Since the blurred surface has the size of the layer, we align it with the
    // top left corner of the layer position.
    matrix.postConcat(SkMatrix::Translate(blurRect.fLeft, blurRect.fTop));
    // 3. Apply the "zoom" effect as an extra scale + translate around the center of the blur.
    if (zoomScale != 1.0f) {
        matrix.postScale(zoomScale, zoomScale);
        matrix.postTranslate(
                blurRect.width() * (1 - zoomScale) / 2.0f,
                blurRect.height() * (1 - zoomScale) / 2.0f);
    }
    // 4. Finally, apply the inverse canvas matrix. The snapshot made in the BlurFilter is in the
    // original surface orientation. The inverse matrix has to be applied to align the blur
    // surface with the current orientation/position of the canvas.
    SkMatrix drawInverse;
    if (canvas != nullptr && canvas->getTotalMatrix().invert(&drawInverse)) {
        matrix.postConcat(drawInverse);
    }
    return matrix;
}

BlurFilter::BlurFilter(RuntimeEffectManager& effectManager, const float maxCrossFadeRadius)
      : mMaxCrossFadeRadius(maxCrossFadeRadius),
        mMixEffect(effectManager.mKnownEffects[kBlurFilter_MixEffect]),
        mGlassEffect(makeGlassEffect()) {}

BlurFilter::GlassProperty::GlassProperty(const char* name, int defaultValue)
      : property(name), fallback(defaultValue), value(defaultValue) {}

int BlurFilter::GlassProperty::get() {
    bool changed = false;
    const char* raw = property.Get(&changed);
    if (changed || !read) {
        read = true;
        if (raw == nullptr || !android::base::ParseInt(raw, &value)) {
            value = fallback;
        }
    }
    return value;
}

uint32_t BlurFilter::scaleRadius(uint32_t radius) {
    if (radius == 0) {
        return 0;
    }
    float scale = 0.5f + std::clamp(mGlassBlur.get(), 0, 100) / 100.0f;
    if (mGlassMaterial.get() == kGlassLiquid) {
        // A clearer backdrop, so the lens at the edges reads.
        scale *= 0.65f;
    }
    const long scaled = std::lround(static_cast<float>(radius) * scale);
    return std::max<uint32_t>(1, static_cast<uint32_t>(scaled));
}

sk_sp<SkShader> BlurFilter::makeGlassShader(const SkCanvas* canvas, const SkRRect& effectRegion,
                                            const sk_sp<SkShader>& blurShader,
                                            const sk_sp<SkImage>& input, const SkRect& blurRect,
                                            const float zoomScale) {
    if (!mGlassEffect) {
        return nullptr;
    }
    const float dim = std::clamp(mGlassTint.get(), 0, 100) / 200.0f;
    const bool accent = mGlassAccentTint.get() != 0;
    bool liquid = mGlassMaterial.get() == kGlassLiquid;
    if (liquid && input) {
        const SkRect device = canvas->getTotalMatrix().mapRect(effectRegion.rect());
        liquid = device.width() * device.height() <
                0.85f * static_cast<float>(input->width()) * static_cast<float>(input->height());
    }
    if (!liquid && dim == 0.0f && !accent) {
        return nullptr;
    }
    if (liquid) {
        rrglass::sLastDraw.store(systemTime());
    }

    const SkRect& rect = effectRegion.rect();
    float corner = 0.0f;
    for (int i = 0; i < 4; i++) {
        const SkVector radii = effectRegion.radii(static_cast<SkRRect::Corner>(i));
        corner += std::min(radii.fX, radii.fY) / 4.0f;
    }
    const uint32_t color = static_cast<uint32_t>(mGlassAccentColor.get());

    SkRuntimeShaderBuilder builder(mGlassEffect);
    builder.child("blurred") = blurShader;
    builder.child("sharp") = liquid && input
            ? makeInputShader(canvas, input, blurRect, zoomScale,
                              SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone))
            : blurShader;
    builder.uniform("rect") = SkV4{rect.fLeft, rect.fTop, rect.fRight, rect.fBottom};
    builder.uniform("cornerRadius") = corner;
    builder.uniform("liquid") = liquid ? 1.0f : 0.0f;
    builder.uniform("refraction") = std::clamp(mGlassRefraction.get(), 0, 100) / 100.0f;
    builder.uniform("dim") = dim;
    builder.uniform("lightDir") = SkV2{rrglass::sLightX.load(), rrglass::sLightY.load()};
    builder.uniform("tint") = SkV4{static_cast<float>((color >> 16) & 0xff) / 255.0f,
                                   static_cast<float>((color >> 8) & 0xff) / 255.0f,
                                   static_cast<float>(color & 0xff) / 255.0f,
                                   accent ? 0.16f : 0.0f};
    return builder.makeShader();
}

float BlurFilter::getMaxCrossFadeRadius() const {
    return mMaxCrossFadeRadius;
}

void BlurFilter::drawBlurRegion(SkCanvas* canvas, const SkRRect& effectRegion,
                                const uint32_t blurRadius, const float zoomScale,
                                const float blurAlpha,
                                const SkRect& blurRect, sk_sp<SkImage> blurredImage,
                                sk_sp<SkImage> input) {
    SFTRACE_CALL();

    SkPaint paint;
    paint.setAlphaf(blurAlpha);

    auto blurMatrix = getShaderTransform(canvas, blurRect, kInverseInputScale, zoomScale);

    SkSamplingOptions linearSampling(SkFilterMode::kLinear, SkMipmapMode::kNone);
    const auto blurShader = blurredImage->makeShader(SkTileMode::kMirror, SkTileMode::kMirror,
                                                     linearSampling, &blurMatrix);

    if (blurRadius < mMaxCrossFadeRadius) {
        LOG_THREAD_STATE_AND_CRASH_IF(!input);

        SkRuntimeShaderBuilder blurBuilder(mMixEffect);
        blurBuilder.child("blurredInput") = blurShader;
        blurBuilder.child("originalInput") =
                makeInputShader(canvas, input, blurRect, zoomScale, linearSampling);

        blurBuilder.uniform("mixFactor") = blurRadius / mMaxCrossFadeRadius;

        paint.setShader(blurBuilder.makeShader());
    } else if (auto glassShader = makeGlassShader(canvas, effectRegion, blurShader, input,
                                                  blurRect, zoomScale)) {
        paint.setShader(std::move(glassShader));
    } else {
        paint.setShader(blurShader);
    }

    if (effectRegion.isRect()) {
        if (blurAlpha == 1.0f) {
            paint.setBlendMode(SkBlendMode::kSrc);
        }
        canvas->drawRect(effectRegion.rect(), paint);
    } else {
        paint.setAntiAlias(true);
        canvas->drawRRect(effectRegion, paint);
    }
}

} // namespace skia
} // namespace renderengine
} // namespace android
