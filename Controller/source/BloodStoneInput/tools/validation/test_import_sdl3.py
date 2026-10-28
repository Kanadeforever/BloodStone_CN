#!/usr/bin/env python3
"""检验 SDL3 3.4.18 离线导入的版本限制、失败回滚和路径安全性。"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

IMPORTER = Path(__file__).resolve().parents[1] / 'vendor' / 'import_sdl3.py'
spec = importlib.util.spec_from_file_location('joyshim_sdl_vendor', IMPORTER)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def make_archive(path: Path, *, version: str = '3.4.18', unsafe: bool = False) -> None:
    """构造只包含必要标记文件的小型测试源码，不依赖网络和第三方库。"""
    root = f'SDL-release-{version}'
    major, minor, micro = version.split('.')
    header = (f'#define SDL_MAJOR_VERSION {major}\n'
              f'#define SDL_MINOR_VERSION {minor}\n'
              f'#define SDL_MICRO_VERSION {micro}\n')
    with ZipFile(path, 'w') as archive:
        archive.writestr(root + '/CMakeLists.txt', 'project(SDL3)\n')
        archive.writestr(root + '/include/SDL3/SDL_version.h', header)
        archive.writestr(root + '/src/SDL.c', '/* 测试占位源码 */\n')
        archive.writestr(root + '/LICENSE.txt', 'zlib license placeholder for tests\n')
        if unsafe:
            archive.writestr(root + '/../escaped.txt', '不能写出目标目录')


class VendorImportTests(unittest.TestCase):
    """每项测试分别使用独立的临时目录，互不修改真实项目和第三方参考资料。"""

    def test_valid_archive_replaces_placeholder(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            src = folder / 'source.zip'
            dest = folder / 'SDL3'
            dest.mkdir()
            (dest / '.gitkeep').touch()
            make_archive(src)
            digest = module.import_sdl(src, dest)
            self.assertEqual(len(digest), 64)
            self.assertTrue((dest / 'LICENSE.txt').exists())
            self.assertTrue((dest / 'src/SDL.c').exists())
            self.assertFalse((dest / '.gitkeep').exists())
            self.assertFalse((folder / 'SDL3.previous').exists())

    def test_rejects_wrong_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            src = folder / 'source.zip'
            make_archive(src, version='3.4.16')
            with self.assertRaises(ValueError):
                module.import_sdl(src, folder / 'SDL3')

    def test_rejects_path_traversal_without_modifying_previous(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            src = folder / 'source.zip'
            dest = folder / 'SDL3'
            dest.mkdir()
            (dest / 'original.txt').write_text('保留原始版本', encoding='utf-8')
            make_archive(src, unsafe=True)
            with self.assertRaises(ValueError):
                module.import_sdl(src, dest)
            self.assertEqual((dest / 'original.txt').read_text('utf-8'), '保留原始版本')
            self.assertFalse((folder / 'escaped.txt').exists())


if __name__ == '__main__':
    unittest.main()
