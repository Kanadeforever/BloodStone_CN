#!/usr/bin/env python3
"""验证 CMake 的 LTO 兼容性策略，避免重复出现链接器内部崩溃。"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

# 直接运行实际的 CMake 策略模块，不在测试里复制它的判断逻辑。
POLICY = Path(__file__).resolve().parents[1] / 'cmake' / 'LtoPolicy.cmake'
CMAKE = shutil.which('cmake')


class LtoPolicyTests(unittest.TestCase):
    def check_policy(self, *, mingw: bool, bits: int, compiler: str,
                     version: str, expected: bool) -> None:
        # 在临时脚本中设置编译器信息，不依赖本机是否安装 Windows 编译器。
        if CMAKE is None:
            self.skipTest('缺少 CMake，不能运行模块测试')
        with tempfile.TemporaryDirectory() as folder:
            script = Path(folder) / 'policy_test.cmake'
            # 将每个设置分开列出，以便出错时能够定位具体模拟条件。
            lines = [
                f'set(MINGW {"TRUE" if mingw else "FALSE"})',
                f'set(CMAKE_SIZEOF_VOID_P {bits // 8})',
                f'set(CMAKE_C_COMPILER_ID {compiler})',
                f'set(CMAKE_CXX_COMPILER_ID {compiler})',
                f'set(CMAKE_CXX_COMPILER_VERSION {version})',
                f'include("{POLICY.as_posix()}")',
                'joyshim_lto_needs_fallback(actual)',
                f'if(NOT actual STREQUAL {"TRUE" if expected else "FALSE"})',
                '  message(FATAL_ERROR "LTO 兼容性决策错误：${actual}")',
                'endif()',
            ]
            script.write_bytes(('\r\n'.join(lines) + '\r\n').encode('utf-8'))
            result = subprocess.run([CMAKE, '-P', str(script)], capture_output=True,
                                    text=True, errors='replace')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_mingw_gcc16_x86_falls_back(self) -> None:
        # 用户日志中的 32 位 GCC 16.1 必须提前关闭 LTO。
        self.check_policy(mingw=True, bits=32, compiler='GNU',
                          version='16.1.0', expected=True)

    def test_mingw_gcc16_x64_falls_back(self) -> None:
        # x64 使用同一 GNU LTO 优化器，尚未实编时优先保证构建稳定。
        self.check_policy(mingw=True, bits=64, compiler='GNU',
                          version='16.1.0', expected=True)

    def test_mingw_gcc15_x86_preserves_lto(self) -> None:
        # 旧的主版本不应被本次特定兼容性规则误伤。
        self.check_policy(mingw=True, bits=32, compiler='GNU',
                          version='15.2.0', expected=False)

    def test_mingw_clang_x86_preserves_lto(self) -> None:
        # LLVM 的优化器不等于 GCC 的 lto1，因此保持原有策略。
        self.check_policy(mingw=True, bits=32, compiler='Clang',
                          version='16.1.0', expected=False)

    def test_native_gcc_not_affected(self) -> None:
        # 非 MinGW 的 GNU 编译器不走 Windows 32 位回退。
        self.check_policy(mingw=False, bits=32, compiler='GNU',
                          version='16.1.0', expected=False)

    def test_msvc_not_affected(self) -> None:
        # MSVC 在后续 CMake 流程中继续使用自己的链接优化。
        self.check_policy(mingw=False, bits=64, compiler='MSVC',
                          version='19.51.0', expected=False)

    def test_future_gcc_not_unconditionally_blocked(self) -> None:
        # 保留未来主版本独立评估的机会。
        self.check_policy(mingw=True, bits=32, compiler='GNU',
                          version='17.0.0', expected=False)


if __name__ == '__main__':
    unittest.main(verbosity=2)
