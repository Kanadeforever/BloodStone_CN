#pragma once

#include <cstddef>

namespace joyshim {

// 文件不存在时自动生成可读的最简配置，复杂的兼容性参数只在文档中介绍。
// 自动生成的文件为 UTF-8 无 BOM，程序不会擅自修改已存在的文件。
inline constexpr char DefaultTomlTemplate[] = R"JOYSHIMTOML(# JoyShim 配置模板：部署时文件主名应与所选 DLL 一致。
# 例如 winmm.dll 使用 winmm.toml；xinput1_3.dll 使用 xinput1_3.toml。
# 游戏专用配置可使用 <游戏EXE主名>.<代理DLL主名>.toml。
# 游戏仍需通过本插件提供的 XInput 接口读取手柄。

[Input]
Rumble = true

[Compatibility]
# 默认保持原生 DirectInput 设备可见；仅有重复输入冲突的游戏改成 true。
# true 也只隐藏 SDL 已接管且实例路径可靠匹配的设备。
HideDirectInput = false

[Debug]
Log = false
)JOYSHIMTOML";
inline constexpr std::size_t DefaultTomlTemplateSize = sizeof(DefaultTomlTemplate) - 1;

}  // namespace joyshim
