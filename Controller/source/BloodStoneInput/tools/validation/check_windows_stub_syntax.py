#!/usr/bin/env python3
"""使用最小 Windows ABI 头桩对 Windows 专属翻译单元执行严格语法检查。"""

from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CLANG = shutil.which("clang++")
if CLANG is None:
    print("未找到 clang++，无法执行 Windows ABI 头桩语法检查。", file=sys.stderr)
    raise SystemExit(2)

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
    CLANG,
    "-std=c++17",
    "-fms-extensions",
    "-fsyntax-only",
    "-D_WIN32",
    f"-I{ROOT / 'tests/windows_stub/include'}",
    f"-I{ROOT / 'src'}",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wcast-function-type",
    "-Werror",
]

# 老 Windows SDK 可能没有扩展能力声明，新 MinGW-w64 则有自己的扩展结构。
# 对两种头文件分别做严格编译，避免某个平台通过而另一平台因重声明失败。
for sdk_name, sdk_flags in (("legacy", ["-DJOYSHIM_TEST_LEGACY_XINPUT_SDK"]),
                            ("modern", [])):
    for architecture, extra in (("x86", []), ("x64", ["-D_WIN64"])):
        for source in SOURCES:
            command = COMMON + sdk_flags + extra + [str(ROOT / source)]
            result = subprocess.run(command, cwd=ROOT, check=False)
            if result.returncode != 0:
                print(f"Windows ABI 头桩语法检查失败：{sdk_name} / {architecture} / {source}", file=sys.stderr)
                raise SystemExit(result.returncode)

print(f"Windows ABI 头桩严格语法检查通过：新旧 SDK × x86/x64 × {len(SOURCES)} 个翻译单元，共 {4 * len(SOURCES)} 项。")
