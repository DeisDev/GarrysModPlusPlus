#pragma once

#include <cstddef>

namespace gmodpp::shim {

struct SlotInfo {
    const char* name;
    const char* returns;
    const char* params;
};

extern const SlotInfo kShaderDeviceMgrSlots[];
extern const size_t kShaderDeviceMgrSlotsCount;
extern const SlotInfo kShaderDeviceSlots[];
extern const size_t kShaderDeviceSlotsCount;
extern const SlotInfo kShaderApiSlots[];
extern const size_t kShaderApiSlotsCount;
extern const SlotInfo kShaderShadowSlots[];
extern const size_t kShaderShadowSlotsCount;
extern const SlotInfo kHardwareConfigSlots[];
extern const size_t kHardwareConfigSlotsCount;
extern const SlotInfo kDebugTextureInfoSlots[];
extern const size_t kDebugTextureInfoSlotsCount;
extern const SlotInfo kVBAllocTrackerSlots[];
extern const size_t kVBAllocTrackerSlotsCount;
extern const SlotInfo kShaderUtilSlots[];
extern const size_t kShaderUtilSlotsCount;

}
