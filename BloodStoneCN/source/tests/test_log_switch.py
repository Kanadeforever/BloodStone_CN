# -*- coding: utf-8 -*-
"""检查 --log 对正式 Win32 插件与独立参数解析器的生效条件。

不需要运行游戏：原生 C++ 参数解析器会在可用编译器下直接执行，
源码测试则保证普通启动不进行日志文件操作，以及五处原生补丁未被替换。
"""
from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'source'
CPP = SOURCE / 'src/bloodstone_cn.cpp'
HEADER = SOURCE / 'src/command_line.h'


class LogSwitchTests(unittest.TestCase):
    def test_flag_is_explicit_and_optional(self) -> None:
        """游戏启动时先判断独立参数，未启用不允许创建文件或写日志。"""
        code = CPP.read_text(encoding='utf-8')
        self.assertIn('bscn::HasLogArgument(GetCommandLineW())', code)
        self.assertIn('if (!g_logging_requested) return;', code)
        self.assertIn('if (!g_logging_requested || !g_log_file) return;', code)
        self.assertIn('if (!g_logging_requested || !g_log_file || !length) return;', code)
        self.assertIn('kCreateAlways', code)
        self.assertIn('L"BloodstoneCN.log"', code)
        self.assertNotIn('OutputDebugStringA(report)', code)

    def test_logs_enough_to_locate_failure(self) -> None:
        """记录五处指令、资源文件大小、目录清单以及失败的 Win32 错误。"""
        code = CPP.read_text(encoding='utf-8')
        for required in (
            'LogSite(index, module)', 'LogRequiredAsset(', 'LogLocTree(',
            'g_last_patch_error = GetLastError()', 'ROLLBACK_FAILED',
            'GetTickCount() - g_log_started', 'FlushFileBuffers(g_log_file)',
            'diagnostic_loc_files_enumerated', 'LogFileFingerprint(',
            'file_fnv1a32', 'State::PathsRedirected',
            'FIVE_PATHS_REDIRECTED', 'g_failed_site = 0;',
        ):
            with self.subTest(required=required):
                self.assertIn(required, code)

    def test_offline_cpp_parser(self) -> None:
        """编译运行实际 C++ 解析器，覆盖引号、路径、参数前后缀等情况。"""
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('当前系统没有可执行本机 C++ 测试的编译器')
        self.assertTrue(HEADER.is_file())
        with tempfile.TemporaryDirectory(dir=ROOT / '.build') as directory:
            test_binary = Path(directory) / 'command_line_smoke'
            result = subprocess.run([
                compiler, '-std=c++17', '-O2', str(SOURCE / 'tests/command_line_smoke.cpp'),
                '-o', str(test_binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            execution = subprocess.run([str(test_binary)], capture_output=True, text=True)
            self.assertEqual(execution.returncode, 0,
                             f'命令行解析错误案例 #{execution.returncode}')


if __name__ == '__main__':
    unittest.main()
