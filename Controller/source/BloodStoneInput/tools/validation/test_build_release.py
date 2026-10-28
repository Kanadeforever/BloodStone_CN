#!/usr/bin/env python3
"""单元测试构建器的跨平台命令生成与 PE 架构保护，不依赖 Windows 编译器。"""

from __future__ import annotations

import importlib.util
import os
from unittest.mock import patch
from pathlib import Path
import struct
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'build_release.py'
spec = importlib.util.spec_from_file_location('joyshim_build_release', SCRIPT)
assert spec is not None and spec.loader is not None
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class BuildContractTests(unittest.TestCase):
    def test_linux_cross_compile_must_target_windows(self) -> None:
        # Linux 构建 Windows DLL 必须传入 Windows 目标，否则会落入主机测试分支。
        flags = builder.mingw_cmake_flags('/mingw/bin/i686-w64-mingw32-g++',
                                          '/usr/bin/ninja', cross_host=True)
        self.assertIn('-DCMAKE_SYSTEM_NAME=Windows', flags)
        self.assertIn('-G', flags)
        self.assertIn('Ninja', flags)

    def test_windows_native_mingw_needs_no_cross_target(self) -> None:
        # Windows 下 MinGW 是原生编译，不需要假设存在 Linux 路径或 Wine。
        flags = builder.mingw_cmake_flags('g++.exe', 'ninja.exe', cross_host=False)
        self.assertNotIn('-DCMAKE_SYSTEM_NAME=Windows', flags)

    def test_wine_emulator_only_when_specified(self) -> None:
        # 没有 Wine 时 Windows EXE 不能作为 Linux 的原生测试执行。
        flags = builder.mingw_cmake_flags('g++', 'ninja', cross_host=True,
                                          emulator='/usr/bin/wine')
        self.assertIn('-DCMAKE_CROSSCOMPILING_EMULATOR=/usr/bin/wine', flags)

    def test_both_pe_architectures(self) -> None:
        # 模拟真实 PE 文件的 DOS 头和 COFF Machine 字段，检查双架构判定。
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / 'check.exe'
            for name, code in (('x86', 0x14C), ('x64', 0x8664)):
                data = bytearray(128)
                data[:2] = b'MZ'
                struct.pack_into('<I', data, 0x3C, 0x40)
                data[0x40:0x44] = b'PE\0\0'
                struct.pack_into('<H', data, 0x44, code)
                file.write_bytes(data)
                builder.check_pe_arch(file, name)
                opposite = 'x64' if name == 'x86' else 'x86'
                with self.assertRaises(RuntimeError):
                    builder.check_pe_arch(file, opposite)

    def test_llvm_mingw_symlink_wrapper_keeps_target_name(self) -> None:
        # 两种目标架构可以共用 clang++ 可执行体，必须保留不同名称的启动包装器。
        with tempfile.TemporaryDirectory() as folder:
            folder_path = Path(folder)
            compiler = folder_path / 'clang++'
            compiler.write_bytes(b'#!/bin/sh\nexit 0\n')
            compiler.chmod(0o755)
            wrappers = []
            for name in ('i686-w64-mingw32-clang++', 'x86_64-w64-mingw32-clang++'):
                wrapper = folder_path / name
                try:
                    wrapper.symlink_to(compiler)
                except (OSError, NotImplementedError):
                    self.skipTest('当前文件系统不支持创建符号链接')
                wrappers.append(wrapper)
            with patch.dict(os.environ, {'JOYSHIM_MINGW_X86': str(wrappers[0]),
                                         'JOYSHIM_MINGW_X64': str(wrappers[1])}):
                result = builder.resolve_mingw()
            self.assertIsNotNone(result)
            assert result is not None
            self.assertEqual(Path(result['x86'][0]).name, wrappers[0].name)
            self.assertEqual(Path(result['x64'][0]).name, wrappers[1].name)
            self.assertNotEqual(result['x86'][0], result['x64'][0])

    def test_llvm_mingw_both_architectures_required(self) -> None:
        # 即使 PATH 中有另一套有效 x64 编译器，用户指定的错误路径也不能
        # 被悄悄替换。模拟 which 的返回值使检查不依赖测试机实际安装情况。
        with tempfile.TemporaryDirectory() as folder:
            binary_dir = Path(folder)
            x86_wrapper = binary_dir / 'i686-w64-mingw32-clang++'
            x64_wrapper = binary_dir / 'x86_64-w64-mingw32-g++'
            for path in (x86_wrapper, x64_wrapper):
                path.write_bytes(b'compiler')
            with patch.dict(os.environ, {'JOYSHIM_MINGW_X86': str(x86_wrapper),
                                         'JOYSHIM_MINGW_X64': str(binary_dir / 'missing-clang++')}):
                with patch.object(builder.shutil, 'which', return_value=str(x64_wrapper)) as which:
                    self.assertIsNone(builder.resolve_mingw())
                    # 两种路径均由显式变量控制，失败时不应进行其他搜索。
                    which.assert_not_called()

    def test_llvm_mingw_auto_detect_when_not_overridden(self) -> None:
        # 未显式指定路径时，应使用自动搜索结果，确保功能没有被安全检查废掉。
        with tempfile.TemporaryDirectory() as folder:
            binary_dir = Path(folder)
            wrappers = {}
            for name in ('i686-w64-mingw32-g++', 'x86_64-w64-mingw32-g++'):
                wrapper = binary_dir / name
                wrapper.write_bytes(b'compiler')
                wrappers[name] = wrapper
            with patch.dict(os.environ, {'JOYSHIM_MINGW_X86': '', 'JOYSHIM_MINGW_X64': ''}):
                with patch.object(builder.shutil, 'which', side_effect=lambda name: str(wrappers[name]) if name in wrappers else None):
                    result = builder.resolve_mingw()
            self.assertIsNotNone(result)
            assert result is not None
            self.assertEqual(Path(result['x86'][0]), wrappers['i686-w64-mingw32-g++'])
            self.assertEqual(Path(result['x64'][0]), wrappers['x86_64-w64-mingw32-g++'])

    def test_non_pe_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / 'invalid.exe'
            file.write_bytes(b'not a PE executable')
            with self.assertRaises(RuntimeError):
                builder.check_pe_arch(file, 'x64')


if __name__ == '__main__':
    unittest.main(verbosity=2)
