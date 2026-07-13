/*
 * SPDX-FileCopyrightText: 2026 AlphaDroid
 * SPDX-License-Identifier: Apache-2.0
 */

#undef LOG_TAG
#define LOG_TAG "OplusFusionExt"

#include "OplusFusionExt.h"

#include <android-base/properties.h>
#include <dlfcn.h>
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
static constexpr int kSvcExt_registerOplusCustomizeSensor = 0;

static const char kEngine[] = "libsensorserviceextimpl.so";
static const char kProp[] = "persist.alpha.fusion_light";

using CreateExtendedFactoryFn = void* (*)();

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

    // registerOplusCustomizeSensor walks `list` and registers the fusion light
    // sensor through SensorService::registerSensor (which we export).
    callVirtual<void, const sensor_t*, size_t, SensorService*>(
            svcExt, kSvcExt_registerOplusCustomizeSensor, list, count, service);

    ALOGI("fusion: registerOplusCustomizeSensor driven for %zu sensors", count);
}

}  // namespace android
