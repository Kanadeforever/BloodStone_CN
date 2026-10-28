#!/usr/bin/env python3
"""使用真实 SDL3 头文件编译静态输入后端，防止链接前的严格告警。"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

# 从本脚本所在位置找到项目根目录，不依赖用户计算机的盘符或用户名。
ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src" / "sdl_backend.cpp"
SDL_HEADERS = ROOT / "tools" / "vendor" / "SDL3" / "include"
STUB_HEADERS = ROOT / "tests" / "windows_stub" / "include"


def check_compiler(compiler: str, extra_include: Path | None) -> None:
    """只执行语法编译，验证静态分支是否满足发布时的严格警告要求。"""
    command = [
        compiler,
        "-std=c++17",
        "-fsyntax-only",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-fno-exceptions",
        "-fno-rtti",
        "-DJOYSHIM_SDL_STATIC=1",
        "-I", str(ROOT / "src"),
        "-I", str(SDL_HEADERS),
    ]
    if os.name != "nt":
        # Linux 开发机没有 Windows SDK，这里使用项目的 Windows 接口测试头。
        # 官方 SDL3 头文件仍是实际参与编译的头文件，而非自制 SDL 假声明。
        # SDL_thread.h 在 Win32 分支会包含 process.h；它只声明线程接口，
        # 当前后端翻译单元不调用其中的函数，所以测试环境放空占位文件即可。
        command += [
            "-fms-extensions", "-D_WIN32", "-D__cdecl=",
            "-I", str(extra_include), "-I", str(STUB_HEADERS),
        ]
    command.append(str(SOURCE))
    print(f"[检查] SDL3 静态后端严格语法编译：{Path(compiler).name}", flush=True)
    subprocess.run(command, check=True, cwd=ROOT)


def main() -> int:
    if not (SDL_HEADERS / "SDL3" / "SDL.h").is_file():
        print("[失败] 缺少 SDL3 官方源码头文件。", file=sys.stderr)
        return 1

    # 这项检查是 GCC/Clang 的额外兼容性门禁；纯 MSVC 系统仍由正式
    # Windows 构建验证 MSVC 编译，不能因为没有 GCC 就禁止 MSVC 构建。
    compilers = [exe for name in ("g++", "clang++") if (exe := shutil.which(name))]
    if not compilers:
        print("[提示] 未安装 GCC/Clang，静态后端专项检查不可执行；需由完整 MSVC 构建验收。")
        return 0

    with tempfile.TemporaryDirectory(prefix="joyshim_sdl_stub_") as temporary:
        include = Path(temporary)
        if os.name != "nt":
            (include / "process.h").write_text(
                "// Windows 线程声明占位，仅用于 SDL3 静态后端的跨平台语法检查。\n",
                encoding="utf-8",
            )
        for compiler in compilers:
            check_compiler(compiler, include)
    print(f"[通过] SDL3 3.4.18 静态后端严格语法检查：{len(compilers)} 套编译器。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
