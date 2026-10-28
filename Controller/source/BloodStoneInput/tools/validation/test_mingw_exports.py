#!/usr/bin/env python3
"""核查 x86 MinGW 的导出定义转换及 x64/MSVC 的原样复用。"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[2]
MODULE = PROJECT / 'tools' / 'cmake' / 'ExportDefinitions.cmake'
SOURCE = PROJECT / 'src'
NAMES = ('xinput1_1', 'xinput1_2', 'xinput1_3', 'xinput1_4',
         'xinput9_1_0', 'dinput8', 'winmm')


def run_selection(name: str, *, mingw: bool, pointer_size: int,
                  root: Path) -> tuple[Path, Path]:
    """用真正的 CMake 脚本执行项目内的导出表生成函数。"""
    cmake = shutil.which('cmake')
    if not cmake:
        raise unittest.SkipTest('未找到 CMake，无法执行真实导出生成检查')
    source = SOURCE / f'exports_{name}_{"x86" if pointer_size == 4 else "x64"}.def'
    destination = root / f'{name}_{pointer_size}_{int(mingw)}.def'
    selected_file = root / 'selected.txt'
    script = root / 'validate.cmake'
    script.write_text('\n'.join((
        f'set(MINGW {"TRUE" if mingw else "FALSE"})',
        f'set(CMAKE_SIZEOF_VOID_P {pointer_size})',
        f'include("{MODULE.as_posix()}")',
        f'joyshim_prepare_export_definition(selected "{source.as_posix()}" "{destination.as_posix()}")',
        f'file(WRITE "{selected_file.as_posix()}" "${{selected}}")',
    )), encoding='utf-8')
    result = subprocess.run([cmake, '-P', str(script)], text=True,
                            capture_output=True, check=False)
    if result.returncode:
        raise AssertionError(result.stdout + '\n' + result.stderr)
    return source, Path(selected_file.read_text(encoding='utf-8'))


class ExportDefinitionTests(unittest.TestCase):
    def test_x86_mingw_all_seven_entries(self) -> None:
        # 七个入口必须只改变内部符号的下划线，不能改变公开名字和序号。
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in NAMES:
                with self.subTest(name=name):
                    source, generated = run_selection(name, mingw=True, pointer_size=4,
                                                      root=root)
                    self.assertNotEqual(source, generated)
                    before = source.read_text(encoding='utf-8')
                    after = generated.read_text(encoding='utf-8')
                    expected = re.sub(r'=_(\w+@\d+)', r'=\1', before)
                    self.assertEqual(after, expected)
                    self.assertNotRegex(after, r'=_[A-Za-z][A-Za-z0-9_]*@\d+')
                    self.assertIn('EXPORTS', after)
                    self.assertRegex(after, r'\s+@\d+')

    def test_msvc_unchanged(self) -> None:
        # MSVC 依赖原始 COFF 装饰名称，不能读 MinGW 的转换文件。
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in NAMES:
                with self.subTest(name=name):
                    source, selected = run_selection(name, mingw=False, pointer_size=4,
                                                     root=root)
                    self.assertEqual(source, selected)
                    self.assertRegex(source.read_text(encoding='utf-8'),
                                     r'=_[A-Za-z][A-Za-z0-9_]*@\d+')

    def test_x64_mingw_unchanged(self) -> None:
        # x64 ABI 没有 32 位 stdcall 的 @字节数 装饰，不允许错误改写。
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in NAMES:
                with self.subTest(name=name):
                    source, selected = run_selection(name, mingw=True, pointer_size=8,
                                                     root=root)
                    self.assertEqual(source, selected)
                    self.assertNotRegex(source.read_text(encoding='utf-8'),
                                        r'=_[A-Za-z][A-Za-z0-9_]*@\d+')

    def test_ordinal_and_attributes_preserved(self) -> None:
        # XInput 固定序号及 PRIVATE、NONAME 不能在格式转换中丢失。
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in NAMES:
                source, generated = run_selection(name, mingw=True, pointer_size=4,
                                                  root=root)
                for original, rewritten in zip(source.read_text(encoding='utf-8').splitlines(),
                                               generated.read_text(encoding='utf-8').splitlines()):
                    if '=' not in original:
                        self.assertEqual(original, rewritten)
                        continue
                    left, right = original.split('=', 1)
                    converted_left, converted_right = rewritten.split('=', 1)
                    self.assertEqual(left, converted_left)
                    # 内部符号之后的序号和修饰词必须按字节一致。
                    self.assertEqual(right.split(' ', 1)[-1],
                                     converted_right.split(' ', 1)[-1])


if __name__ == '__main__':
    unittest.main(verbosity=2)
