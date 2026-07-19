/*
 * SPDX-FileCopyrightText: 2026 AlphaDroid
 * SPDX-License-Identifier: Apache-2.0
 */

#undef LOG_TAG
#define LOG_TAG "OplusFusionExt"

#include "OplusFusionExt.h"

#include <android-base/properties.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <log/log.h>

#include "SensorService.h"

namespace android {

// The engine's ExtendedFactory / impl vtable indices, decoded from the dodge
// OOS16 libsensorserviceextimpl.so (llvm-readelf -r; funcs at vtable_sym+0x10,
// 8 bytes each). Verified byte-identical to the astonc engine.
//
//   ExtendedFactory (createExtendedFactory C export):
//     [0] createSensorServiceExt()   -> ISensorServiceExt*
//     [3] createSensorDeviceExt()    -> SensorDeviceExt*
//   ISensorServiceExt:
//     [0] registerOplusCustomizeSensor(const sensor_t*, size_t, SensorService*)
//   SensorDeviceExt:
//     [5] activateFusionSensor(int, bool)
//     [6] batchFusionSensor(int, int64_t, int64_t)
//     [7] getFusionSensorsList()
static constexpr int kFactory_createSensorServiceExt = 0;
static constexpr int kFactory_createSensorDeviceExt = 3;
static constexpr int kSvcExt_registerOplusCustomizeSensor = 0;
static constexpr int kDeviceExt_activateFusionSensor = 5;
static constexpr int kDeviceExt_batchFusionSensor = 6;

static const char kEngine[] = "libsensorserviceextimpl.so";
static const char kProp[] = "persist.alpha.fusion_light";

// Exported by the engine; sets [OplusSensorServiceUtils+0x3fb] via setFusionLightStatue
// when it sees qti.sensor.high_pwm_rgb in the HAL list. Must run before registration.
static const char kOnSensorFoundSym[] =
        "_ZN7android20SensorServiceExtImpl13onSensorFoundEPK8sensor_tm";

using CreateExtendedFactoryFn = void* (*)();
using OnSensorFoundFn = void (*)(void*, const sensor_t*, size_t);

static void* gDeviceExt = nullptr;

// Minimal base the engine's ExtendedFactory derives from. The engine references
// only SensorServiceExtFactory::{ctor,dtor}; the object is vtable-only (8B) and
// is intentionally leaked (never deleted through the base pointer). These must
// be exported (the lib is -fvisibility=hidden) so the engine resolves them.
#define FUSION_EXPORT __attribute__((visibility("default")))
class FUSION_EXPORT SensorServiceExtFactory {
public:
    SensorServiceExtFactory();
    virtual ~SensorServiceExtFactory();
};
SensorServiceExtFactory::SensorServiceExtFactory() {}
SensorServiceExtFactory::~SensorServiceExtFactory() {}

// Reads slot `index` from the object's vtable and calls it.
template <typename Ret, typename... Args>
static Ret callVirtual(void* obj, int index, Args... args) {
    void** vtable = *reinterpret_cast<void***>(obj);
    using Fn = Ret (*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[index])(obj, args...);
}

bool oplusFusionActive() {
    return base::GetBoolProperty(kProp, false) && gDeviceExt != nullptr;
}

status_t oplusFusionActivate(int handle, int enabled) {
    if (!oplusFusionActive()) {
        return NO_ERROR;
    }
    // DeviceExt::activateFusionSensor only latches NextGen flags for:
    //   0x3e9 + gate[0x3fb] -> mIsFusionLightActivated
    //   0x3f0 + gate[0x3fc] -> mIsFusionRGBActivated
    // FusionLightSensor::activate also pokes SensorDevice with 0x3e9 (and
    // historically ExtImpl poked -1 when the raw handle was never captured).
    // Drive both virtual handles whenever we see either signal so the RGB
    // path latches — otherwise activateInternal keeps mIsFusionRGBActivated=0
    // and lux stays dead.
    const bool driveFusion = (handle < 0) || (handle == 0x3e9) || (handle == 0x3f0);
    if (driveFusion) {
        const int ret3e9 = callVirtual<int, int, bool>(
                gDeviceExt, kDeviceExt_activateFusionSensor, 0x3e9, enabled != 0);
        const int ret3f0 = callVirtual<int, int, bool>(
                gDeviceExt, kDeviceExt_activateFusionSensor, 0x3f0, enabled != 0);
        ALOGI("fusion: activateFusionSensor(0x3e9)=%d (0x3f0)=%d enabled=%d [via handle=0x%x]",
              ret3e9, ret3f0, enabled, handle);
        return (ret3e9 == 0 || ret3f0 == 0) ? NO_ERROR : ret3e9;
    }
    const int ret = callVirtual<int, int, bool>(
            gDeviceExt, kDeviceExt_activateFusionSensor, handle, enabled != 0);
    ALOGI("fusion: activateFusionSensor(handle=0x%x, enabled=%d) -> %d", handle, enabled, ret);
    return ret;
}

status_t oplusFusionBatch(int handle, int64_t samplingPeriodNs, int64_t maxBatchReportLatencyNs) {
    if (!oplusFusionActive()) {
        return NO_ERROR;
    }
    // Mirror activate: when the engine passes -1 (raw handle never captured),
    // also drive the virtual fusion sensor's batch path.
    if (handle < 0) {
        const int ret3e9 = callVirtual<int, int, int64_t, int64_t>(
                gDeviceExt, kDeviceExt_batchFusionSensor, 0x3e9, samplingPeriodNs,
                maxBatchReportLatencyNs);
        ALOGI("fusion: batchFusionSensor(handle=0x3e9 [via -1], period=%" PRId64 ") -> %d",
              samplingPeriodNs, ret3e9);
    }
    return callVirtual<int, int, int64_t, int64_t>(
            gDeviceExt, kDeviceExt_batchFusionSensor, handle, samplingPeriodNs,
            maxBatchReportLatencyNs);
}

void loadOplusFusionSensors(SensorService* service, const sensor_t* list, size_t count) {
    if (!base::GetBoolProperty(kProp, false)) {
        return;
    }

    void* handle = dlopen(kEngine, RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        ALOGE("fusion: dlopen(%s) failed: %s", kEngine, dlerror());
        return;
    }

    auto createFactory =
            reinterpret_cast<CreateExtendedFactoryFn>(dlsym(handle, "createExtendedFactory"));
    if (createFactory == nullptr) {
        ALOGE("fusion: createExtendedFactory not found: %s", dlerror());
        return;
    }

    void* factory = createFactory();
    if (factory == nullptr) {
        ALOGE("fusion: createExtendedFactory returned null");
        return;
    }

    void* svcExt = callVirtual<void*>(factory, kFactory_createSensorServiceExt);
    if (svcExt == nullptr) {
        ALOGE("fusion: createSensorServiceExt returned null");
        return;
    }

    auto onSensorFound = reinterpret_cast<OnSensorFoundFn>(dlsym(handle, kOnSensorFoundSym));
    if (onSensorFound == nullptr) {
        ALOGE("fusion: onSensorFound not found: %s", dlerror());
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        onSensorFound(svcExt, list, i);
    }
    ALOGI("fusion: onSensorFound driven for %zu sensors", count);

    gDeviceExt = callVirtual<void*>(factory, kFactory_createSensorDeviceExt);
    if (gDeviceExt == nullptr) {
        ALOGE("fusion: createSensorDeviceExt returned null");
        return;
    }

    // registerOplusCustomizeSensor walks `list` and registers the fusion light
    // sensor through SensorService::registerSensor (which we export).
    callVirtual<void, const sensor_t*, size_t, SensorService*>(
            svcExt, kSvcExt_registerOplusCustomizeSensor, list, count, service);

    ALOGI("fusion: registerOplusCustomizeSensor driven for %zu sensors", count);
}

}  // namespace android
