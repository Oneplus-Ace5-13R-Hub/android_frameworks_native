/*
 * SPDX-FileCopyrightText: The Resurrected Remix Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

#include <utils/Timers.h>

namespace android::renderengine::rrglass {

// Liquid Glass light direction in screen space, pointing toward the light. system_server sends
// it from the tilt sensor; (0, 0) restores the default.
void setLight(float x, float y);

// Changes on every setLight, so client composition can't reuse a frame lit the old way.
uint32_t lightSerial();

// Whether Liquid glass was drawn in the last half second.
bool drawnRecently(nsecs_t now);

} // namespace android::renderengine::rrglass
