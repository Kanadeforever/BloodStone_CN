# -*- coding: utf-8 -*-
"""公开包在没有游戏资产的机器上也能运行的格式、事务与防误分发回归。"""
from __future__ import annotations
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
RELEASE = ROOT/'release'
TOOLBOX = ROOT/'toolbox'
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(TOOLBOX/'core'))
sys.path.insert(0, str(TOOLBOX/'core'))
from synthetic_fixtures import make_pak, make_loc, make_bfa, make_game
from native_pak import NativePak, FilePak, normalized_name, read_text_entries, language_blocks
from loc_json import export_one, import_one
from native_font import preserve_original_blocks


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


packer = load('public_packer', ROOT/'source/tools/package_release.py')
font_app = load('public_font_app', TOOLBOX/'core/font_tool.py')


def copy_public_tools(target: Path) -> None:
    """独立复制工具箱；用自有 ASI 输入模拟不依赖工程源码的单独分发。"""
    for name, raw in packer.collect_public_files(ROOT).items():
        if name.startswith('toolbox/'):
            file = target/name.removeprefix('toolbox/')
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(raw)
    builder = load('public_fixture_builder', ROOT/'source/tools/bootstrap_toolbox.py')
    for name in builder.EMPTY_DIRS:
        (target/name).mkdir(parents=True, exist_ok=True)
    plugin = target/'input/plugin/BloodstoneCN.asi'
    plugin.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(RELEASE/'Runtime/BloodstoneCN.asi', plugin)


class PublicPackageTests(unittest.TestCase):
    def test_toolbox_has_one_source_and_runtime_contains_no_tools(self):
        self.assertEqual({p.name for p in (ROOT/'source').iterdir() if p.is_dir() and p.name != '__pycache__'},
                         {'src', 'config', 'tests', 'tools', 'docs'})
        self.assertFalse(list((RELEASE/'Runtime').rglob('*.py')))
        self.assertFalse((RELEASE/'LocTool').exists())
        self.assertFalse((RELEASE/'FontTool').exists())
        self.assertTrue((TOOLBOX/'gui.py').is_file())

    def test_frozen_runtime_is_exact_v12_program(self):
        raw = (RELEASE/'Runtime/BloodstoneCN.asi').read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), packer.ASI_SHA256)
        self.assertEqual(raw[:2], b'MZ')
        for name in packer.collect_public_files(ROOT):
            self.assertFalse(name.endswith(('.pak', '.loc', '.loc.json', '.dat', '.bpr',
                                           '.bfa', '.ttf', '.otf', '.ttc')), name)

    def test_dirty_personal_workspace_never_enters_public_zip(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'.build') as tmp:
            root = Path(tmp)/'project'
            files = packer.collect_public_files(ROOT)
            for name, raw in files.items():
                file = root/name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(raw)
            sentinel = b'PRIVATE-' + os.urandom(32)
            for name in ('toolbox/input/gamedata.pak',
                         'toolbox/translations/private.loc.json',
                         'toolbox/input/template/ui_font_eur.dat',
                         'toolbox/input/fonts/private.ttf',
                         'toolbox/output/Runtime/BSCNContents/loc/private.loc',
                         'source/NativePakLoc/config/original/gamedata.pak',
                         'source/LocTool/config/json/private.loc.json'):
                file = root/name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(sentinel)
            first, second = Path(tmp)/'first.zip', Path(tmp)/'second.zip'
            packer.package(root, first)
            packer.package(root, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual({n for n in archive.namelist() if not n.endswith('/')},
                                 set(files) | {packer.MANIFEST})
                for name in archive.namelist():
                    self.assertTrue(sentinel not in archive.read(name), name)

    def test_whitelist_cannot_authorize_asset_extensions(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'.build') as tmp:
            root = Path(tmp)
            (root/'source/config').mkdir(parents=True)
            names = [packer.REGISTRY, packer.ASI_PATH, 'source/config/payload.pak']
            (root/packer.REGISTRY).write_text(json.dumps({'files': names}), encoding='utf-8')
            (root/packer.ASI_PATH).parent.mkdir(parents=True)
            shutil.copyfile(RELEASE/'Runtime/BloodstoneCN.asi', root/packer.ASI_PATH)
            with self.assertRaisesRegex(ValueError, '禁止公开'):
                packer.collect_public_files(root)


class FormatContractTests(unittest.TestCase):
    def test_file_mapping_matches_memory_parser_and_is_readonly(self):
        raw = make_pak({'test/a.bin': b'alpha'*4096, 'test/b.bin': b'beta'})
        with tempfile.TemporaryDirectory(dir=ROOT/'.build') as tmp:
            source = Path(tmp)/'root.pak'
            source.write_bytes(raw)
            before = source.read_bytes()
            with FilePak(source) as archive:
                self.assertEqual(set(archive.by_name), set(NativePak(raw).by_name))
                for name in archive.by_name:
                    self.assertEqual(archive.read_name(name), NativePak(raw).read_name(name))
                with self.assertRaises(TypeError):
                    archive.data[0] = 0
            self.assertEqual(source.read_bytes(), before)

    def test_resource_names_reject_traversal_and_windows_drives(self):
        for name in ('../outside', '/outside', 'C:/outside', 'a/../b', 'a//b', 'a/./b', '\\\\host\\file'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                normalized_name(name)
        self.assertEqual(normalized_name('Ui\\Data\\FONT.DAT'), 'ui/data/font.dat')
        self.assertEqual(normalized_name('\\Ui\\Data\\FONT.DAT'), 'ui/data/font.dat')

    def test_loc_roundtrip_preserves_identity_and_seven_languages(self):
        source = make_loc(4)
        resource = 'gamedata/xt/loc/common.loc'
        doc = export_one(source, source, resource)
        doc['entries'][0]['translate'] = '中~~{SyntheticAction}~~'
        output, warnings = import_one(source, doc, resource)
        self.assertEqual(len(warnings), 1)
        self.assertEqual(read_text_entries(output, 'en')[1], '中~~{SyntheticAction}~~')
        for (_, a, n), (_, b, m) in zip(language_blocks(source)[1:], language_blocks(output)[1:]):
            self.assertEqual(source[a:a+n], output[b:b+m])
        doc['entries'][0]['original'] = 'changed'
        with self.assertRaisesRegex(ValueError, 'original'):
            import_one(source, doc, resource)

    def test_font_block_copy_uses_actual_texture_stride(self):
        level = 2
        row_bytes = (512 >> level)//4*8
        rows = (1024 >> level)//4
        original = b''.join(bytes([row])*row_bytes for row in range(rows))
        for size in (2048, 4096, 8192):
            target = bytearray((size >> level)**2//2)
            preserve_original_blocks(target, original, level, size)
            stride = (size >> level)//4*8
            for row in range(rows):
                self.assertEqual(target[row*stride:row*stride+row_bytes], bytes([row])*row_bytes)

    def test_font_grid_rounding_uses_larger_source_atlas(self):
        class FakeRenderer:
            def __init__(self, *_args, **_kwargs): pass
            def __enter__(self): return self
            def __exit__(self, *_args): pass
            def render(self, _cp): return bytes(36*44)
        with patch.object(font_app, 'WindowsGlyphRenderer', FakeRenderer):
            atlas, missing = font_app.from_user_font(Path('synthetic.ttf'), set(range(0x4000, 0x4000+2600)))
        self.assertEqual(atlas.width, 4096)
        self.assertEqual(len(atlas.glyphs), 2600)
        self.assertFalse(missing)

    def test_publish_restores_both_old_products_after_late_rename_failure(self):
        toolkit = load('public_transaction_toolkit', TOOLBOX/'toolkit.py')
        with tempfile.TemporaryDirectory(dir=ROOT/'.build') as tmp:
            directory = Path(tmp)
            stage = directory/'stage'
            stage.mkdir()
            first, second = directory/'first', directory/'second'
            new_first, new_second = stage/'first-new', stage/'second-new'
            for file, raw in ((first,b'old first'), (second,b'old second'),
                              (new_first,b'new first'), (new_second,b'new second')):
                file.write_bytes(raw)
            original_replace = Path.replace
            def fail_second(path, target):
                if path == new_second:
                    raise OSError('synthetic late rename failure')
                return original_replace(path, target)
            with patch.object(Path, 'replace', fail_second):
                with self.assertRaisesRegex(OSError, 'late rename'):
                    toolkit.publish(stage, [(new_first,first), (new_second,second)])
            self.assertEqual(first.read_bytes(), b'old first')
            self.assertEqual(second.read_bytes(), b'old second')


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT/'.build')
        self.base = Path(self.tmp.name)
        self.tools = self.base/'tools with spaces 工具'
        self.game = self.base/'game with spaces 游戏'
        copy_public_tools(self.tools)
        make_game(self.game)
        self.game_before = {p.relative_to(self.game).as_posix(): p.read_bytes()
                            for p in self.game.rglob('*') if p.is_file()}

    def tearDown(self):
        after = {p.relative_to(self.game).as_posix(): p.read_bytes()
                 for p in self.game.rglob('*') if p.is_file()}
        self.assertEqual(after, self.game_before, '任何测试都不得修改个人游戏输入')
        self.tmp.cleanup()

    def run_cli(self, *args, ok=True):
        process = subprocess.run([sys.executable, '-B', str(self.tools/'toolkit.py'), *args],
                                 env=dict(os.environ, PYTHONUTF8='1'),
                                 capture_output=True, text=True, encoding='utf-8')
        if ok:
            self.assertEqual(process.returncode, 0, process.stdout+process.stderr)
        else:
            self.assertNotEqual(process.returncode, 0)
        return process

    def test_prepare_exports_all_text_and_extracts_only_two_font_files(self):
        self.run_cli('prepare', '--game', str(self.game))
        docs = list((self.tools/'translations').rglob('*.loc.json'))
        self.assertEqual(len(docs), 86)
        self.assertEqual(sum(len(json.loads(p.read_text())['entries']) for p in docs), 7975)
        self.assertEqual({p.name for p in (self.tools/'input/template').iterdir()},
                         {'ui_font_eur.dat', 'ui_font_eur.bpr'})
        self.assertFalse((self.tools/'input/root.pak').exists())
        self.assertFalse(list((self.tools/'output/Runtime/BSCNContents').rglob('*.loc')))
        document = docs[0]
        before = document.read_bytes()
        self.run_cli('prepare', '--game', str(self.game), ok=False)
        self.assertEqual(document.read_bytes(), before)

    def test_bad_input_does_not_publish_partial_project(self):
        (self.game/'cache/root.pak').write_bytes(b'bad synthetic input')
        self.game_before['cache/root.pak'] = b'bad synthetic input'
        self.run_cli('prepare', '--game', str(self.game), ok=False)
        self.assertFalse((self.tools/'work/project.json').exists())
        self.assertFalse(list((self.tools/'translations').rglob('*.loc.json')))

    def test_build_verify_and_failed_build_retains_previous_complete_product(self):
        self.run_cli('prepare', '--game', str(self.game))
        file = self.tools/'translations/gamedata/xt/loc/common.loc.json'
        doc = json.loads(file.read_text())
        doc['entries'][0]['translate'] = '中'
        file.write_text(json.dumps(doc, ensure_ascii=False), encoding='utf-8')
        (self.tools/'input/template/fonts.bfa').write_bytes(make_bfa())
        self.run_cli('build')
        self.run_cli('verify')
        runtime = self.tools/'output/Runtime'
        self.assertEqual(len(list((runtime/'BSCNContents/loc').rglob('*.loc'))), 86)
        manifest = json.loads((self.tools/'output/build_manifest.json').read_text())
        self.assertEqual(manifest['verification']['font_faces'], 3)
        self.assertTrue(manifest['verification']['original_dxt1_blocks_unchanged'])
        old = {p.relative_to(runtime).as_posix(): p.read_bytes() for p in runtime.rglob('*') if p.is_file()}
        doc['entries'][0]['translate'] = 'new\x00broken'
        file.write_text(json.dumps(doc), encoding='utf-8')
        self.run_cli('build', ok=False)
        self.assertEqual(old, {p.relative_to(runtime).as_posix(): p.read_bytes()
                               for p in runtime.rglob('*') if p.is_file()})
        self.run_cli('verify', ok=False)

    def test_missing_font_fails_with_clear_message(self):
        self.run_cli('prepare', '--game', str(self.game))
        result = self.run_cli('build', ok=False)
        self.assertIn('请先提供自己的', result.stderr)
        self.assertNotIn('Traceback', result.stderr)

    def test_standalone_converters_need_no_plugin_and_export_keeps_translation(self):
        self.run_cli('prepare', '--game', str(self.game))
        shutil.rmtree(self.tools/'input/plugin')
        file = self.tools/'translations/gamedata/xt/loc/common.loc.json'
        doc = json.loads(file.read_text(encoding='utf-8'))
        doc['entries'][0]['translate'] = '中'
        file.write_text(json.dumps(doc, ensure_ascii=False), encoding='utf-8')
        before = file.read_bytes()
        (self.tools/'input/template/fonts.bfa').write_bytes(make_bfa())
        self.run_cli('build-loc')
        self.run_cli('build-font')
        self.assertEqual(len(list((self.tools/'output/loc/BSCNContents/loc').rglob('*.loc'))), 86)
        self.assertTrue((self.tools/'output/font/BSCNContents/fonts/ui_font_cnx.dat').is_file())
        self.run_cli('export')
        self.assertEqual(file.read_bytes(), before)
        self.run_cli('export', ok=False)
        self.assertEqual(file.read_bytes(), before)
        result = self.run_cli('build', ok=False)
        self.assertIn('组装成品需要 BloodstoneCN.asi', result.stderr)
        self.assertFalse((self.tools/'output/Runtime').exists())

    def test_font_import_only_updates_local_copy_and_rejects_bad_size(self):
        source = self.base/'个人 字体.ttf'
        source.write_bytes(b'SYNTHETIC FONT INPUT')
        self.run_cli('configure-font', '--font', str(source), '--pixel-size', '28')
        imported = self.tools/'input/fonts'/source.name
        self.assertEqual(imported.read_bytes(), source.read_bytes())
        options = self.tools/'input/font_options.json'
        self.assertEqual(json.loads(options.read_text(encoding='utf-8'))['pixel_size'], 28)
        before = options.read_bytes()
        self.run_cli('configure-font', '--font', str(source), '--pixel-size', '99', ok=False)
        self.assertEqual(options.read_bytes(), before)
        self.assertEqual(source.read_bytes(), b'SYNTHETIC FONT INPUT')

    @unittest.skipIf(os.name == 'nt', 'Windows 创建符号链接通常需要开发者模式，其他流程仍会检查重解析点')
    def test_linked_output_cannot_escape_into_game(self):
        target = self.tools/'translations'
        shutil.rmtree(target)
        target.symlink_to(self.game, target_is_directory=True)
        self.run_cli('prepare', '--game', str(self.game), ok=False)
        self.assertFalse((self.game/'gamedata').exists())


if __name__ == '__main__':
    unittest.main()
