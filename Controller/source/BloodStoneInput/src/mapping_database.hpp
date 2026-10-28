#pragma once

#include <cstddef>

#include "config.hpp"

namespace joyshim {

class SdlBackend;

struct MappingLoadResult {
    bool requested{};
    bool fileExists{};
    bool apiAvailable{};
    int mappingsAdded{-1};
    wchar_t detail[512]{};
};

// 在 SDL Gamepad 子系统初始化成功后、打开任何手柄之前加载外部映射库。
// 后加载的同 GUID 映射由当前 SDL 后端覆盖内置条目。
MappingLoadResult LoadConfiguredMappingDatabase(
    const RuntimeConfig& config,
    SdlBackend& backend) noexcept;

}  // namespace joyshim
