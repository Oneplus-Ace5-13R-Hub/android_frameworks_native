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

#include "Utils/OverlayUtils.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ftl/flags.h>
#include <ftl/small_map.h>
#include <ui/LayerStack.h>
#include <ui/Size.h>
#include <ui/Transform.h>
#include <utils/StrongPointer.h>

#include <scheduler/Fps.h>

class SkCanvas;

namespace android {

class GraphicBuffer;
class SurfaceFlinger;

class RefreshRateOverlay {
private:
    // Effectively making the constructor private, while keeping std::make_unique work
    struct ConstructorTag {};

public:
    enum class Features {
        Spinner = 1 << 0,
        RenderRate = 1 << 1,
        ShowInMiddle = 1 << 2,
        SetByHwc = 1 << 3,
    };

    static std::unique_ptr<RefreshRateOverlay> create(FpsRange, ftl::Flags<Features>);

    ~RefreshRateOverlay();

    void setLayerStack(ui::LayerStack);
    void setViewport(ui::Size);
    void changeRefreshRate(Fps, Fps);
    void changeRenderRate(Fps);
    void animate();
    bool isSetByHwc() const { return mFeatures.test(RefreshRateOverlay::Features::SetByHwc); }
    void onVrrIdle(bool idle);

    RefreshRateOverlay(ConstructorTag, FpsRange, ftl::Flags<Features>);

private:
    bool initCheck() const;

    // If the ro.surface_flinger.panel_refresh_rate_node sysprop names a readable
    // sysfs node reporting the panel's real self-refresh rate (e.g. an LTPO TE
    // counter), a poller thread owns the displayed number instead of the vsync
    // rate reported by mode changes.
    void startPanelRefreshPoller();
    void panelRefreshPollLoop(const std::string& nodePath);
    bool usesPanelRefreshRate() const { return mPanelPollThread.joinable(); }

    using Buffers = std::vector<sp<GraphicBuffer>>;

    static Buffers draw(int refreshRate, int renderFps, bool idle, SkColor,
                        ui::Transform::RotationFlags, ftl::Flags<Features>);
    static void drawNumber(int number, int left, SkColor, SkCanvas&);
    static void drawDash(int left, SkCanvas&);

    const Buffers& getOrCreateBuffers(Fps, Fps, bool);

    SurfaceComposerClient::Transaction createTransaction() const;

    struct Key {
        int refreshRate;
        int renderFps;
        ui::Transform::RotationFlags flags;
        bool idle;

        bool operator==(Key other) const {
            return refreshRate == other.refreshRate && renderFps == other.renderFps &&
                    flags == other.flags && idle == other.idle;
        }
    };

    using BufferCache = ftl::SmallMap<Key, Buffers, 9>;
    BufferCache mBufferCache;

    std::optional<Fps> mRefreshRate;
    std::optional<Fps> mRenderFps;
    bool mIsVrrIdle = false;
    size_t mFrame = 0;

    // Real panel self-refresh rate from the polled sysfs node; when present it
    // is what drawNumber shows as the refresh rate.
    std::optional<Fps> mPanelRefreshRate;

    // Guards the buffer cache, the displayed state above and transaction
    // application, since the poller thread updates the overlay concurrently
    // with SurfaceFlinger's main thread.
    std::mutex mMutex;
    std::thread mPanelPollThread;
    std::mutex mPollMutex;
    std::condition_variable mPollCondition;
    bool mPollStop = false;

    const FpsRange mFpsRange; // For color interpolation.
    const ftl::Flags<Features> mFeatures;

    std::unique_ptr<SurfaceControlHolder> mSurfaceControl;
};

} // namespace android
