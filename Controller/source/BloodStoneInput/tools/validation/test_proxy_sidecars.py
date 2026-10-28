#!/usr/bin/env python3
"""通过真实打包函数验证代理 DLL 与同名 TOML 一一对应。"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    'joyshim_packager_sidecar_test', PROJECT / 'tools/build_release.py')
assert SPEC is not None and SPEC.loader is not None
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


class SidecarReleaseTests(unittest.TestCase):
    def test_compiler_directory_requires_dll_only_validation(self) -> None:
        # 刚编译完成的目录不会有同名 TOML；需要允许 DLL-only 的独立性检查。
        # 但相同目录在默认的正式发行检查下仍必须拒绝，不能降低发布标准。
        checker = PROJECT / 'tools/validation/check_alias_outputs.py'
        with tempfile.TemporaryDirectory(prefix='joyshim-compiled-check-') as temp:
            compiled = Path(temp)
            for name in BUILDER.PROXIES:
                (compiled / f'{name}.dll').write_bytes(
                    f'构建输出测试桩:{name}'.encode('utf-8'))
            accepted = subprocess.run(
                [sys.executable, str(checker), str(compiled), '--dll-only'],
                check=False, capture_output=True, text=True, errors='replace')
            self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)
            strict = subprocess.run(
                [sys.executable, str(checker), str(compiled)],
                check=False, capture_output=True, text=True, errors='replace')
            self.assertNotEqual(strict.returncode, 0)
            self.assertIn('缺少与代理同名的 TOML', strict.stdout + strict.stderr)

            # 构建目录即使以 DLL-only 检查，也不能放行复制改名的代理。
            (compiled / 'winmm.dll').write_bytes((compiled / 'dinput8.dll').read_bytes())
            rejected = subprocess.run(
                [sys.executable, str(checker), str(compiled), '--dll-only'],
                check=False, capture_output=True, text=True, errors='replace')
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn('相同代理二进制', rejected.stdout + rejected.stderr)

    def test_builder_stage_transition_uses_different_validation_modes(self) -> None:
        # build_arch() 只检查 DLL，publish() 的发行阶段必须保持严格配置检查。
        # 两处若被误写成同一模式，即使独立打包测试能通过也会重复本次故障。
        import inspect
        source = inspect.getsource(BUILDER.build_arch)
        release = inspect.getsource(BUILDER.publish)
        self.assertIn("str(result_dir), '--dll-only'", source)
        self.assertIn("str(stage / arch)", release)
        self.assertNotIn('--dll-only', release)

    def test_real_packager_generates_matching_sidecars(self) -> None:
        # 修改的只是内存里的发行位置：本测试绝不写入真实 release。
        # 每种 DLL 使用不同测试字节，保证原有的独立性检查也实际运行。
        old_cache, old_release = BUILDER.CACHE, BUILDER.RELEASE
        with tempfile.TemporaryDirectory(prefix='joyshim-sidecars-') as temp:
            base = Path(temp)
            BUILDER.CACHE = base / '.build'
            BUILDER.RELEASE = base / 'release'
            try:
                results = {}
                for arch in ('x86', 'x64'):
                    compiled = base / 'compiled' / arch
                    compiled.mkdir(parents=True)
                    results[arch] = compiled
                    for name in BUILDER.PROXIES:
                        (compiled / f'{name}.dll').write_bytes(
                            f'独立构建测试桩:{arch}:{name}'.encode('utf-8'))
                BUILDER.publish(results, 'sidecar-test')
                expected = (PROJECT / 'config' / 'template.toml').read_bytes()
                for arch in ('x86', 'x64'):
                    actual = BUILDER.RELEASE / arch
                    self.assertEqual(len(list(actual.glob('*.dll'))), 7)
                    self.assertEqual(len(list(actual.glob('*.toml'))), 7)
                    self.assertFalse((actual / 'version.dll').exists())
                    self.assertFalse((actual / 'joyshim.toml').exists())
                    self.assertEqual(list(actual.glob('*.exe')), [])
                    for name in BUILDER.PROXIES:
                        self.assertEqual((actual / f'{name}.toml').read_bytes(), expected)
                self.assertTrue((BUILDER.RELEASE / '示例配置' / 'Bond.winmm.toml').exists())

                # 删除某个配置后，真实的发行目录检查必须拒绝此目录。
                (BUILDER.RELEASE / 'x86' / 'winmm.toml').unlink()
                rejected = subprocess.run(
                    [sys.executable, str(PROJECT / 'tools/validation/check_alias_outputs.py'),
                     str(BUILDER.RELEASE / 'x86')],
                    check=False, capture_output=True, text=True, errors='replace')
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn('winmm.toml', rejected.stdout + rejected.stderr)
            finally:
                BUILDER.CACHE, BUILDER.RELEASE = old_cache, old_release


if __name__ == '__main__':
    unittest.main(verbosity=2)
