#!/usr/bin/env python3
"""用 GCC 的严格告警规则检查两类 Windows SDK 的 XInput 声明兼容性。"""

from pathlib import Path
import shutil
import subprocess
import sys

# 工程不要求每个使用者安装 Linux GCC。
# 该工具用于开发阶段：发现 Clang 不会提示、但 MSYS2 MinGW 会以 -Werror 阻断的告警。
ROOT = Path(__file__).resolve().parents[2]
GCC = shutil.which("g++")
if GCC is None:
    print("当前环境未提供 g++，无法运行 GCC SDK 兼容性专项检查。", file=sys.stderr)
    raise SystemExit(2)

# 这些翻译单元包含所有会在完整 Windows 构建中编译的关键源码和工具入口。
SOURCES = (
    "src/config.cpp",
    "src/toml_reader.cpp",
    "src/input_routing.cpp",
    "src/mapping_database.cpp",
    "src/sdl_backend.cpp",
    "src/proxy_entry.cpp",
    "src/proxy_system.cpp",
    "src/directinput_filter.cpp",
    "src/system_exports.cpp",
    "src/xinput_proxy.cpp",
    "tools/src/gamepad_probe.cpp",
    "tools/src/gamepad_info.cpp",
    "tools/src/config_check.cpp",
    "tools/src/xinput_state_probe.cpp",
    "tools/src/proxy_entry_probe.cpp",
    "tests/test_config_scope.cpp",
)
COMMON = [
    GCC, "-std=c++17", "-fsyntax-only", "-fms-extensions", "-D_WIN32",
    f"-I{ROOT / 'tests/windows_stub/include'}", f"-I{ROOT / 'src'}",
    "-Wall", "-Wextra", "-Wpedantic", "-Werror",
    "-fno-exceptions", "-fno-rtti",
]

# 老 SDK 未声明 XInputGetCapabilitiesEx，新 MinGW 已经自行声明。
# 对两种情况分别编译，防止同名 C 函数冲突重新出现。
for sdk, flags in (("legacy", ["-DJOYSHIM_TEST_LEGACY_XINPUT_SDK"]),
                   ("modern", [])):
    for arch, arch_flags in (("x86", []), ("x64", ["-D_WIN64"])):
        for relative in SOURCES:
            command = COMMON + flags + arch_flags + [str(ROOT / relative)]
            result = subprocess.run(command, cwd=ROOT, check=False)
            if result.returncode != 0:
                print(f"GCC 检查失败：SDK={sdk}，架构={arch}，文件={relative}", file=sys.stderr)
                raise SystemExit(result.returncode)

print(f"GCC 严格语法检查通过：新旧 SDK × x86/x64 × {len(SOURCES)} 文件，共 {4 * len(SOURCES)} 项。")
