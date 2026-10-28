#!/usr/bin/env python3
"""检查 MinGW GCC 16 的 C++ 标准库移动构造兼容策略。"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

# 读取实际 CMake 策略，而不是在 Python 中复制一份相似判断。
APP = Path(__file__).resolve().parents[2]
POLICY = APP / 'tools/cmake/GccStringPolicy.cmake'
CMAKE_FILE = APP / 'CMakeLists.txt'
CMAKE = shutil.which('cmake')


class GccStringPolicyTests(unittest.TestCase):
    def test_compiler_matrix(self) -> None:
        # 一次 CMake 脚本覆盖用户 GCC16 x86/x64 以及其他工具链。
        if CMAKE is None:
            self.skipTest('没有安装 CMake')
        cases = [(True, 'GNU', '16.1.0', 4, True),
                 (True, 'GNU', '16.1.0', 8, True),
                 (True, 'GNU', '15.2.0', 4, False),
                 (True, 'GNU', '17.0.0', 4, False),
                 (True, 'Clang', '16.1.0', 4, False),
                 (False, 'GNU', '16.1.0', 4, False),
                 (False, 'MSVC', '19.51', 8, False)]
        with tempfile.TemporaryDirectory() as folder:
            script = Path(folder) / 'policy.cmake'
            lines = [f'include("{POLICY.as_posix()}")']
            for mingw, compiler, version, bits, expected in cases:
                lines.extend([f'set(MINGW {"TRUE" if mingw else "FALSE"})',
                              f'set(CMAKE_CXX_COMPILER_ID {compiler})',
                              f'set(CMAKE_CXX_COMPILER_VERSION {version})',
                              f'set(CMAKE_SIZEOF_VOID_P {bits})',
                              'joyshim_gcc_string_needs_fallback(actual)',
                              f'if(NOT actual STREQUAL {"TRUE" if expected else "FALSE"})',
                              '  message(FATAL_ERROR "C++ ABI 兼容性决策错误：${actual}")',
                              'endif()'])
            script.write_bytes(('\r\n'.join(lines) + '\r\n').encode('utf-8'))
            result = subprocess.run([CMAKE, '-P', str(script)], capture_output=True,
                                    text=True, errors='replace')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cpp17_is_strictly_required(self) -> None:
        # cxx_std_17 表示最低版本；必须另设 CMAKE_CXX_STANDARD 防止 GCC16
        # 默认采用 C++20，触发已知的 std::string 移动构造链接问题。
        source = CMAKE_FILE.read_text(encoding='utf-8')
        for line in ('set(CMAKE_CXX_STANDARD 17)',
                     'set(CMAKE_CXX_STANDARD_REQUIRED ON)',
                     'set(CMAKE_CXX_EXTENSIONS OFF)',
                     '-fno-declone-ctor-dtor',
                     'JOYSHIM_GCC_STRING_LINK_OK'):
            self.assertIn(line, source)

    def test_native_compiler_uses_cpp17(self) -> None:
        # 实际调用 CMake 生成项目测试编译命令，检查项目目标用的是
        # 确切的 C++17 而不是编译器默认的、更高版本的 GNU 方言。
        if os.name == 'nt':
            # Windows 的完整 CMake 配置需要检测 SDL3 及系统 SDK，速度较慢，
            # 还可能默认选择不提供 compile_commands.json 的 Visual Studio 生成器。
            # Windows 真实 MinGW 构建会执行前面的强制 C++17 配置和 ABI 链接预检。
            self.skipTest('Windows 由正式 CMake 配置执行 C++17 和 ABI 链接预检')
        if CMAKE is None or not shutil.which('c++'):
            self.skipTest('缺少本机 C++ 编译器或 CMake')
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run([CMAKE, '-S', str(APP), '-B', folder,
                                     '-DBUILD_TESTING=ON',
                                     '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                                     '-DCMAKE_BUILD_TYPE=Release'],
                                    capture_output=True, text=True, errors='replace')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            commands = json.loads((Path(folder) / 'compile_commands.json').read_text())
            matching = [item['command'] for item in commands
                        if item['file'].replace('\\', '/').endswith('/tests/test_toml_reader.cpp')]
            self.assertEqual(len(matching), 1)
            self.assertIn('-std=c++17', matching[0])


if __name__ == '__main__':
    unittest.main(verbosity=2)
