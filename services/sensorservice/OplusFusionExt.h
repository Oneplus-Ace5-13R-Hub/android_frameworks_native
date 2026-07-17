/*
 * SPDX-FileCopyrightText: 2026 AlphaDroid
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loader for the ColorOS "OPLUS Fusion Light Sensor (Next Gen)" — a
 * content-immune ambient-light virtual sensor produced by the prebuilt
 * libsensorserviceextimpl.so. We do NOT reconstruct the OPPO C++ interface
 * hierarchy; we call the engine's virtuals by their (verified) vtable index.
 *
 * Gated on persist.alpha.fusion_light (default 0). No-op unless the engine
 * blob and its deps are present (dlopen failure is logged and ignored).
 */

#pragma once

#include <hardware/sensors.h>
#include <utils/Errors.h>

namespace android {

class SensorService;

// Called from SensorService::onFirstRef after the AOSP sensors are registered.
// Loads the fusion engine and lets it register its virtual light sensor via
// SensorService::registerSensor. Safe to call unconditionally.
void loadOplusFusionSensors(SensorService* service, const sensor_t* list, size_t count);

// SensorDevice hooks — drive the engine's SensorDeviceExt activate/batch path
// (activateInternal / CWB screenshot monitor). No-op when fusion is disabled.
bool oplusFusionActive();
status_t oplusFusionActivate(int handle, int enabled);
status_t oplusFusionBatch(int handle, int64_t samplingPeriodNs, int64_t maxBatchReportLatencyNs);

}  // namespace android
