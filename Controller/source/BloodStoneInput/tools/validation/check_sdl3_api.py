#!/usr/bin/env python3
"""用官方 SDL3 头文件验证 JoyShim 的 44 项控制器函数指针类型。"""
from pathlib import Path
import re
import shutil
import subprocess
import sys

APP = Path(__file__).resolve().parents[2]
VENDOR = APP / 'tools/vendor/SDL3/include'
HEADER = APP / 'src/sdl3_minimal.hpp'
if not (VENDOR / 'SDL3/SDL.h').is_file():
    print('[失败] 缺少 SDL3 官方头文件。', file=sys.stderr)
    raise SystemExit(1)
# 用实际官方声明与每个函数指针的 C++ 类型进行编译期比较。
# 如果新版 SDL 更改了返回值、参数或者调用约定，会在编译时直接失败。
names = re.findall(r'using (SDL_\w+Fn)\s*=', HEADER.read_text(encoding='utf-8'))
if len(names) != 44:
    print('[失败] SDL 函数指针列表长度变化，需要同步审查。', file=sys.stderr)
    raise SystemExit(1)
lines = ['#include <type_traits>', '#include "sdl3_minimal.hpp"']
for name in names:
    func = name.removesuffix('Fn')
    if func == 'SDL_Free':
        func = 'SDL_free'
    lines.append(f'static_assert(std::is_same_v<{name}, decltype(&{func})>, "{func}");')
# Windows 通过实际构建工具链检测 __cdecl；Linux 主机上的 SDL_CALL 则为空。
source = APP.parents[1] / '.build' / 'sdl3_api_contract.cpp'
source.parent.mkdir(parents=True, exist_ok=True)
source.write_text('\n'.join(lines) + '\n', encoding='utf-8')
compilers = [p for name in ('g++', 'clang++') if (p := shutil.which(name))]
if not compilers:
    print('[跳过] 无 C++ 编译器，无法运行本地 SDL3 API 契约检查。')
    raise SystemExit(0)
for compiler in compilers:
    command = [compiler, '-std=c++17', '-DJOYSHIM_SDL_STATIC=1', '-Wall', '-Wextra',
               '-Wpedantic', '-Werror', '-fsyntax-only',
               '-I', str(APP / 'src'), '-I', str(VENDOR), str(source)]
    if sys.platform != 'win32':
        command.insert(3, '-D__cdecl=')
    subprocess.run(command, check=True)
print(f'[通过] SDL3 3.4.18 的 {len(names)} 项函数指针 ABI 类型检查通过。')
