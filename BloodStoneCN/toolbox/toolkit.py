# -*- coding: utf-8 -*-
"""不含游戏资产的本地汉化工作流；外部游戏只读，所有输出留在工具目录。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'core'))
from native_pak import FilePak, NativePak, language_blocks, read_key_entries, read_text_entries
from loc_json import export_one, import_one, json_bytes, _unique_json_object
from native_font import NativeDat, NativeBpr, split_mipmaps

VERSION = 'M1-v1.2-Toolkit-public3-PakFix'
REFERENCE_ROOT_SHA256 = 'ff8a5ba04a336e05fae2cdc52720373742adc62cc9c4b153f4167d486552df10'
FACES = ['E_Century_Gothic', 'E_Century_Gothic_Bold', 'E_Microgramma']
REFERENCE = {
    'gamedata.pak': '070b7592885c36e2b788b43a0b9ed8f28cf36b8b087a734e4c6e700ed2629fb4',
    'ui_font_eur.dat': 'a12fefdfc15d1fbd55d19739d9031faf637f837d37b2febba4a1f2fc8d50b7bb',
    'ui_font_eur.bpr': 'd4e46ebcaa4b82e75dc68fb5c8fc725c03df5d77f405eef0ddcc906f8768509f',
}


def digest(path: Path) -> str:
    """流式散列，输入文件始终只读。"""
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def linked(path: Path) -> bool:
    return path.is_symlink() or (path.exists() and bool(
        getattr(path.lstat(), 'st_file_attributes', 0) & 0x400))


def local(relative: str) -> Path:
    path = ROOT / relative
    if not path.resolve().is_relative_to(ROOT):
        raise ValueError(f'工具输出越界：{relative}')
    return path


def check_local_trees() -> None:
    """拒绝链接和 Windows 重解析点，不让缓存或输出间接写进游戏。"""
    for name in ('core', 'config', 'input', 'translations', 'output', 'work', '.caches'):
        folder = local(name)
        if linked(folder):
            raise ValueError(f'工具目录不能是链接：{name}')
        for current, dirs, files in os.walk(folder, followlinks=False):
            for name in dirs + files:
                child = Path(current) / name
                if linked(child) or not child.resolve().is_relative_to(ROOT):
                    raise ValueError(f'工具输入输出含链接：{child}')


def populated(path: Path) -> bool:
    return path.exists() and any(p.is_file() and p.name != '.keep' for p in path.rglob('*'))


def publish(stage: Path, pairs: list[tuple[Path, Path]]) -> None:
    """在同一文件系统中成组发布，后项失败时恢复先前的完整成品。"""
    backups = []
    installed = []
    try:
        for number, (source, target) in enumerate(pairs):
            if not source.exists():
                raise ValueError(f'暂存内容缺失：{source.name}')
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                backup = stage / f'previous-{number}'
                target.replace(backup)
                backups.append((backup, target))
            source.replace(target)
            installed.append(target)
    except BaseException:
        for path in reversed(installed):
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink(missing_ok=True)
        for backup, target in reversed(backups):
            backup.replace(target)
        raise


def validate_templates(dat: bytes, bpr: bytes) -> None:
    native = NativeDat.parse(dat)
    textures = NativeBpr.parse(bpr, len(native.faces))
    if [f.name for f in native.faces] != FACES or [f.name for f in textures.faces] != FACES:
        raise ValueError('需要原版 EUR 的三个字体面，不能使用 JA/EA/BA 或已经扩容的字库')
    if any((f.width, f.height) != (512, 1024) for f in native.faces + textures.faces):
        raise ValueError('原始 EUR 模板必须为三面 512×1024')
    if native.to_bytes() != dat or textures.to_bytes() != bpr:
        raise ValueError('原生模板不能逐字节往返，拒绝未知结构')
    if (len(native.faces[2].extra) != 3528 or not native.faces[2].glyphs
            or struct.unpack_from('<I', native.faces[2].glyphs[-1], 24)[0] != 588):
        raise ValueError('第三字体面原始字距配对表与已确认结构不符')


def prepare(gamedata: Path, root_pak: Path | None, template_dir: Path | None) -> dict:
    """从用户自己的文件生成完整工作输入，绝不调用历史译文或历史字库。"""
    check_local_trees()
    targets = ['input/gamedata.pak', 'translations', 'input/template', 'work/project.json']
    for name in targets:
        path = local(name)
        if path.is_file() or populated(path):
            raise ValueError(f'{name} 已有个人数据，拒绝重新初始化；新项目请重新解压工具包')
    if (root_pak is None) == (template_dir is None):
        raise ValueError('必须选择 root.pak 或一套自行提取的 EUR 模板')
    gamedata = gamedata.resolve(strict=True)
    if gamedata.is_relative_to(ROOT):
        raise ValueError('初始化输入应来自工具目录之外的个人游戏副本')
    print('[读取] 原版 gamedata.pak；不修改输入', flush=True)
    gamedata_hash = digest(gamedata)
    with FilePak(gamedata) as archive:
        gamedata_version = archive.version
        print(f'[格式] gamedata.pak：2KAP v{gamedata_version}', flush=True)
        names = sorted(n for n in archive.by_name if n.endswith('.loc'))
        if len(names) != 86 or any(not n.startswith('gamedata/xt/loc/') for n in names):
            raise ValueError('需要当前 PC 基线的完整 86 个原生 LOC')
        documents = {}
        entries = 0
        for name in names:
            payload = archive.read_name(name)
            doc = export_one(payload, payload, name)
            entries += len(doc['entries'])
            documents[name + '.json'] = json_bytes(doc)
    if entries != 7975:
        raise ValueError(f'当前 PC 基线应有 7975 条 EN 记录，实际 {entries}')
    root_hash = None
    root_version = None
    if template_dir is not None:
        template_dir = template_dir.resolve(strict=True)
        dat = (template_dir / 'ui_font_eur.dat').read_bytes()
        bpr = (template_dir / 'ui_font_eur.bpr').read_bytes()
        font_source = str(template_dir)
    else:
        root_pak = root_pak.resolve(strict=True)
        print('[提取] 只读映射 root.pak，仅解压 EUR DAT/BPR', flush=True)
        with FilePak(root_pak) as archive:
            root_version = archive.version
            print(f'[格式] root.pak：2KAP v{root_version}，完整路径索引已校验', flush=True)
            dat = archive.read_name('ui/data/ui_font_eur.dat')
            bpr = archive.read_name('ui/data/ui_font_eur.bpr')
        root_hash = digest(root_pak)
        font_source = str(root_pak)
    validate_templates(dat, bpr)
    hashes = {'gamedata.pak': gamedata_hash,
              'ui_font_eur.dat': hashlib.sha256(dat).hexdigest(),
              'ui_font_eur.bpr': hashlib.sha256(bpr).hexdigest()}
    report = {'format': 'BSCN-TOOLKIT-PROJECT-v1', 'toolkit_version': VERSION,
              'files': len(documents), 'entries': entries, 'language': 'en',
              'source_gamedata': str(gamedata), 'source_font': font_source,
              'pak_versions': {'gamedata': gamedata_version, 'root': root_version},
              'source_root_sha256': root_hash,
              'matches_reference_root': root_hash == REFERENCE_ROOT_SHA256 if root_hash else None,
              'input_sha256': hashes, 'matches_reference_assets': hashes == REFERENCE,
              'translation_master': 'translations', 'game_files_written': False}
    cache = local('.caches')
    cache.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-', dir=cache) as folder:
        stage = Path(folder)
        pak_copy = stage / 'gamedata.pak'
        shutil.copyfile(gamedata, pak_copy)
        if digest(pak_copy) != gamedata_hash or digest(gamedata) != gamedata_hash:
            raise ValueError('读取期间原版 PAK 发生变化，未发布工作输入')
        loc_json, templates = stage/'translations', stage/'template'
        # 译文只保存一份。字体工具直接读取这份数据，无需复制或人工同步。
        for relative, payload in documents.items():
            output = loc_json / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(payload)
        templates.mkdir()
        (templates/'ui_font_eur.dat').write_bytes(dat)
        (templates/'ui_font_eur.bpr').write_bytes(bpr)
        info = stage/'project.json'
        info.write_bytes(json_bytes(report))
        publish(stage, [(pak_copy, local(targets[0])), (loc_json, local(targets[1])),
                        (templates, local(targets[2])), (info, local(targets[3]))])
    print(f'[完成] 已初始化 {len(documents)} 份 JSON、{entries} 条原文和三面 EUR 模板')
    local('input/fonts').mkdir(parents=True, exist_ok=True)
    print('[下一步] 在 GUI 中编辑译文、导入个人字体，然后构建完整汉化')
    if not report['matches_reference_assets']:
        print('[提醒] 输入格式通过，但指纹不等于 v1.2 参考样本；游戏 EXE 兼容性由 ASI 运行时签名判定')
    return report


def source_snapshot(plugin: Path | None = None) -> dict[str, str]:
    """输入指纹同时防止使用旧输出以及构建期间的译文变动。"""
    paths = [local('input/gamedata.pak'),
             local('input/template/ui_font_eur.dat'),
             local('input/template/ui_font_eur.bpr')]
    paths += sorted(local('translations').rglob('*.loc.json'))
    for directory in ('input/fonts', 'input/template'):
        paths += sorted(p for p in local(directory).iterdir()
                        if p.is_file() and p.suffix.lower() in ('.ttf', '.otf', '.ttc', '.bfa'))
    options = local('input/font_options.json')
    if options.exists():
        paths.append(options)
    defaults = local('config/defaults.json')
    if defaults.exists():
        paths.append(defaults)
    result = {p.relative_to(ROOT).as_posix(): digest(p) for p in paths}
    if plugin is not None:
        result['@plugin_sha256'] = digest(plugin)
    return result


def verify_runtime(runtime: Path, pak_path: Path, json_dir: Path, template: Path,
                   plugin_sha256: str) -> dict:
    """独立验收安装目录的文本语义、非 EN 字节、三字体面及原纹理块。"""
    archive = NativePak(pak_path.read_bytes())
    names = sorted(n for n in archive.by_name if n.endswith('.loc'))
    actual = {p.relative_to(runtime/'BSCNContents/loc').as_posix()
              for p in (runtime/'BSCNContents/loc').rglob('*.loc')}
    if actual != {n.removeprefix('gamedata/xt/loc/') for n in names}:
        raise ValueError('安装目录的 LOC 集合与原版 86 份资源不一致')
    chars = set()
    count = 0
    for resource in names:
        original = archive.read_name(resource)
        doc = json.loads((json_dir/(resource+'.json')).read_text(encoding='utf-8-sig'),
                         object_pairs_hook=_unique_json_object)
        expected, _ = import_one(original, doc, resource)
        generated = (runtime/'BSCNContents/loc'/resource.removeprefix('gamedata/xt/loc/')).read_bytes()
        if generated != expected:
            raise ValueError(f'安装目录使用了旧版或不同译文的 LOC：{resource}')
        texts = read_text_entries(generated, 'en')
        count += len(texts)
        for text in texts.values():
            chars.update(ord(c) for c in text if ord(c) >= 128 and ord(c) not in (0xA0, 0xFEFF))
        if read_key_entries(generated) != read_key_entries(original):
            raise AssertionError('KeyTable 身份改变')
        for (_, a, size_a), (_, b, size_b) in zip(language_blocks(original)[1:], language_blocks(generated)[1:]):
            if original[a:a+size_a] != generated[b:b+size_b]:
                raise AssertionError('非 EN 语言原始字节改变')
    dat = NativeDat.parse((runtime/'BSCNContents/fonts/ui_font_cnx.dat').read_bytes())
    bpr = NativeBpr.parse((runtime/'BSCNContents/fonts/ui_font_cnx.bpr').read_bytes(), len(dat.faces))
    original_dat = NativeDat.parse((template/'ui_font_eur.dat').read_bytes())
    original_bpr = NativeBpr.parse((template/'ui_font_eur.bpr').read_bytes(), 3)
    if [f.name for f in dat.faces] != FACES or [f.name for f in bpr.faces] != FACES:
        raise ValueError('成品没有完整保留 EUR 三个字体面')
    for index, face in enumerate(dat.faces):
        keys = set(face.codepoints())
        missing = sorted(cp for cp in chars if cp not in keys or cp + 1 not in keys)
        if missing:
            raise ValueError(f'{face.name} 缺少译文字形入口或后继记录：'+str(missing[:12]))
        if face.width != face.height or face.width not in (2048, 4096, 8192):
            raise ValueError('成品 DAT 图集尺寸不受支持')
        if (bpr.faces[index].width, bpr.faces[index].height) != (face.width, face.height):
            raise ValueError('DAT/BPR 的纹理尺寸不配套')
        for level, (before, after) in enumerate(zip(split_mipmaps(original_bpr.faces[index]),
                                                  split_mipmaps(bpr.faces[index]))):
            row_size = (512 >> level) // 4 * 8
            new_stride = (face.width >> level) // 4 * 8
            for row in range((1024 >> level)//4):
                if before[row*row_size:(row+1)*row_size] != after[row*new_stride:row*new_stride+row_size]:
                    raise ValueError('成品意外改变原版西文字形或图标的 DXT1 块')
    if (dat.faces[2].extra != original_dat.faces[2].extra
            or dat.faces[2].glyphs[-1] != original_dat.faces[2].glyphs[-1]):
        raise ValueError('第三字体面字距数据或尾记录改变')
    if digest(runtime/'BloodstoneCN.asi') != plugin_sha256:
        raise ValueError('成品 ASI 与工具包提供的版本不同')
    return {'loc_files': len(names), 'entries': count, 'non_ascii_characters': len(chars),
            'font_faces': 3, 'texture_size': dat.faces[0].width,
            'keytable_unchanged': True, 'non_en_bytes_unchanged': True,
            'original_dxt1_blocks_unchanged': True, 'third_kerning_unchanged': True,
            'runtime_asi_matches_template': True}



def ensure_project() -> dict:
    """确认初始化资料完整，构建时始终使用提取时的原始模板。"""
    check_local_trees()
    path = local('work/project.json')
    if not path.is_file():
        raise ValueError('请先在 GUI 中初始化个人项目，或执行 prepare 命令')
    project = json.loads(path.read_text(encoding='utf-8'))
    for name, relative in [('gamedata.pak', 'input/gamedata.pak'),
                           ('ui_font_eur.dat', 'input/template/ui_font_eur.dat'),
                           ('ui_font_eur.bpr', 'input/template/ui_font_eur.bpr')]:
        if digest(local(relative)) != project['input_sha256'][name]:
            raise ValueError(f'初始化后的原始模板发生变化：{relative}')
    if len(list(local('translations').rglob('*.loc.json'))) != 86:
        raise ValueError('translations 中必须完整保留 86 份翻译 JSON')
    return project


def resolve_plugin(candidate: Path | None = None) -> Path:
    """转换器本身独立；组装安装目录时才只读取得调用者提供的 ASI。"""
    choices = [candidate] if candidate is not None else [
        ROOT/'input/plugin/BloodstoneCN.asi', ROOT.parent/'release/Runtime/BloodstoneCN.asi']
    for path in choices:
        if path.is_file():
            with path.open('rb') as stream:
                if stream.read(2) != b'MZ':
                    raise ValueError('所选 ASI 不是 Windows PE 程序')
            return path.resolve(strict=True)
    raise ValueError('组装成品需要 BloodstoneCN.asi；请在 GUI 选择插件或使用 --plugin 指定。单独转换文本和字库不需要插件')


def configure_font(font: Path | None, pixel_size: int) -> dict:
    """复制个人字体并保存字号；只替换工具中的副本，不注册系统字体。"""
    check_local_trees()
    if type(pixel_size) is not int or not 16 <= pixel_size <= 34:
        raise ValueError('像素字号必须是 16～34 的整数')
    if font is not None:
        font = font.resolve(strict=True)
        if not font.is_file() or font.suffix.lower() not in ('.ttf', '.otf', '.ttc'):
            raise ValueError('请选择一份 TTF、OTF 或 TTC 字体文件')
    cache = local('.caches')
    cache.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='font-config-', dir=cache) as folder:
        stage = Path(folder)
        options = stage/'font_options.json'
        options.write_bytes(json_bytes({'pixel_size': pixel_size}))
        pairs = [(options, local('input/font_options.json'))]
        if font is not None:
            fonts = stage/'fonts'
            fonts.mkdir()
            before = digest(font)
            shutil.copyfile(font, fonts/font.name)
            if digest(fonts/font.name) != before or digest(font) != before:
                raise ValueError('复制期间字体文件发生变化，未更新字体配置')
            pairs.insert(0, (fonts, local('input/fonts')))
        publish(stage, pairs)
    result = {'font': font.name if font else '保持当前字体', 'pixel_size': pixel_size}
    print('[完成] 字体配置：'+json.dumps(result, ensure_ascii=False))
    return result


def validate_font(mode: str) -> None:
    """构建前检查字体来源，让缺字体和平台限制变成明确的中文提示。"""
    font_dir = local('input/fonts')
    font_dir.mkdir(parents=True, exist_ok=True)
    fonts = [p for p in font_dir.iterdir() if p.is_file() and p.suffix.lower() in ('.ttf', '.otf', '.ttc')]
    if len(fonts) > 1:
        raise ValueError('input/fonts 中只能保留一份字体；可以通过 GUI 重新导入')
    if not fonts and not (mode == 'precise' and local('input/template/fonts.bfa').is_file()):
        raise ValueError('请先提供自己的 TTF/OTF/TTC；公开包不附带字体或历史 BFA')
    if fonts and sys.platform != 'win32':
        raise ValueError('用户 TTF/OTF/TTC 栅格化需要 Windows GDI；请在 Windows 上执行构建')


def run_stage(stage: Path, component: str, mode: str) -> None:
    """在隔离目录调用同一份工具核心，避免子工具提前发布半套结果。"""
    for name in ('core', 'input', 'translations', 'config'):
        shutil.copytree(local(name), stage/name, ignore=shutil.ignore_patterns('__pycache__'))
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONUTF8='1')
    commands = []
    if component in ('all', 'loc'):
        commands.append(('loc_tool.py', 'import'))
    if component in ('all', 'font'):
        commands.append(('font_tool.py', mode))
    for script, action in commands:
        result = subprocess.run([sys.executable, '-u', '-B', str(stage/'core'/script), action],
                                cwd=stage, env=environment,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        if result.returncode:
            raise ValueError('子工具构建失败；此前的完整成品保持不变')


def build(mode: str = 'precise', plugin: Path | None = None, component: str = 'all') -> dict:
    """共用一套输入，支持整套汉化、单独 LOC 和单独字库三个出口。"""
    if mode not in ('precise', 'common', 'gbk') or component not in ('all', 'loc', 'font'):
        raise ValueError('未知构建模式')
    ensure_project()
    if component in ('all', 'font'):
        validate_font(mode)
    asi = resolve_plugin(plugin) if component == 'all' else None
    snapshot = source_snapshot(asi)
    cache = local('.caches')
    cache.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='build-', dir=cache) as folder:
        stage = Path(folder)
        run_stage(stage, component, mode)
        pairs = []
        for name in ('loc', 'font'):
            if component in ('all', name):
                pairs.append((stage/'output'/name, local('output/'+name)))
        report = {'format': 'BSCN-TOOLKIT-BUILD-v2', 'toolkit_version': VERSION,
                  'component': component, 'mode': mode, 'input_sha256': snapshot,
                  'game_files_written': False, 'game_runtime_tested': False}
        if component == 'all':
            # 安装目录由独立插件和当前两个转换结果组成，工具代码不会进入其中。
            runtime = stage/'Runtime'
            runtime.mkdir()
            shutil.copyfile(asi, runtime/'BloodstoneCN.asi')
            shutil.copytree(stage/'output/loc/BSCNContents/loc', runtime/'BSCNContents/loc')
            shutil.copytree(stage/'output/font/BSCNContents/fonts', runtime/'BSCNContents/fonts')
            verification = verify_runtime(runtime, stage/'input/gamedata.pak',
                                          stage/'translations', stage/'input/template',
                                          snapshot['@plugin_sha256'])
            loc_report = json.loads((stage/'output/loc/result.json').read_text(encoding='utf-8'))
            font_report = json.loads((stage/'output/font/生成结果.json').read_text(encoding='utf-8'))
            report.update({'verification': verification, 'warning_count': loc_report['warning_count'],
                           'font_source': font_report['source'], 'plugin_source': str(asi),
                           'files_sha256': {p.relative_to(runtime).as_posix(): digest(p)
                                            for p in sorted(runtime.rglob('*')) if p.is_file()}})
            pairs.append((runtime, local('output/Runtime')))
        # 计算发生在临时目录。用户此时改动译文或字体会阻止发布正式结果。
        if source_snapshot(asi) != snapshot:
            raise ValueError('构建期间输入发生变化；未更新正式成品，请保存译文后重试')
        info = stage/'build_manifest.json'
        info.write_bytes(json_bytes(report))
        report_name = 'build_manifest.json' if component == 'all' else f'{component}_manifest.json'
        pairs.append((info, local('output/'+report_name)))
        publish(stage, pairs)
    print('[完成] '+('整套可安装汉化：output\\Runtime' if component == 'all' else '单独输出：output\\'+component))
    if component == 'all':
        print('[安装] 只复制 BloodstoneCN.asi 与 BSCNContents，游戏语言设为 EN')
        if report['verification']['texture_size'] != 2048:
            print('[提醒] 大于 2048 的图集仍需单独实机验证')
    return report


def export_original() -> dict:
    """另行导出原文副本，绝不重置正在编辑的 translations。"""
    ensure_project()
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONUTF8='1')
    result = subprocess.run([sys.executable, '-u', '-B', str(ROOT/'core/loc_tool.py'), 'export'],
                            cwd=ROOT, env=environment,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    if result.returncode:
        raise ValueError('原文副本导出失败；正在编辑的译文没有被改写')
    return {'output': 'output/loc/untranslated_json'}


def verify(plugin: Path | None = None) -> dict:
    """从当前唯一输入和构建记录出发，重新验收全部安装文件。"""
    ensure_project()
    report_path = local('output/build_manifest.json')
    if not report_path.is_file():
        raise ValueError('尚未构建整套成品，请先执行完整构建')
    report = json.loads(report_path.read_text(encoding='utf-8'))
    if plugin is None:
        try:
            asi = resolve_plugin()
        except ValueError:
            asi = resolve_plugin(Path(report['plugin_source']))
        if digest(asi) != report['input_sha256']['@plugin_sha256']:
            asi = resolve_plugin(Path(report['plugin_source']))
    else:
        asi = resolve_plugin(plugin)
    if source_snapshot(asi) != report['input_sha256']:
        raise ValueError('译文、字体、模板或插件已修改，需要重新构建整套成品')
    runtime = local('output/Runtime')
    files = {p.relative_to(runtime).as_posix(): digest(p) for p in runtime.rglob('*') if p.is_file()}
    if files != report['files_sha256']:
        raise ValueError('成品文件被增删或修改，请重新构建')
    return verify_runtime(runtime, local('input/gamedata.pak'), local('translations'),
                          local('input/template'), report['input_sha256']['@plugin_sha256'])


def status() -> dict:
    """提供 GUI 和命令行共用的轻量状态，不把存在成品误写为验收通过。"""
    check_local_trees()
    result = {'version': VERSION, 'initialized': local('work/project.json').is_file(),
              'translation_files': len(list(local('translations').rglob('*.loc.json'))),
              'built': local('output/build_manifest.json').is_file()}
    print('[状态] '+json.dumps(result, ensure_ascii=False))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description='BloodstoneCN 统一汉化工具箱；无参数交互请打开 gui.py')
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('prepare', help='一次性从个人游戏提取原文与模板')
    p.add_argument('--game', type=Path)
    p.add_argument('--gamedata', type=Path)
    fonts = p.add_mutually_exclusive_group()
    fonts.add_argument('--root-pak', type=Path)
    fonts.add_argument('--template-dir', type=Path)
    f = sub.add_parser('configure-font', help='导入个人字体或修改像素字号')
    f.add_argument('--font', type=Path)
    f.add_argument('--pixel-size', type=int, default=32)
    for command in ('build', 'build-loc', 'build-font'):
        b = sub.add_parser(command, help='整套构建或单独转换')
        b.add_argument('--mode', choices=('precise', 'common', 'gbk'), default='precise')
        if command == 'build':
            b.add_argument('--plugin', type=Path)
        if command != 'build-loc':
            b.add_argument('--font', type=Path)
            b.add_argument('--pixel-size', type=int)
    v = sub.add_parser('verify', help='重新验收整套成品')
    v.add_argument('--plugin', type=Path)
    sub.add_parser('export', help='另行导出原文副本，不覆盖现有译文')
    sub.add_parser('status', help='查看项目状态')
    args = parser.parse_args()
    try:
        if args.command == 'prepare':
            if args.game:
                if args.gamedata or args.root_pak or args.template_dir:
                    raise ValueError('--game 不与手动文件参数混用')
                game = args.game.resolve(strict=True)
                prepare(game/'cache/gamedata.pak', game/'cache/root.pak', None)
            else:
                if args.gamedata is None:
                    raise ValueError('请指定 --game，或 --gamedata 加 --root-pak/--template-dir')
                prepare(args.gamedata, args.root_pak, args.template_dir)
        elif args.command == 'configure-font':
            configure_font(args.font, args.pixel_size)
        elif args.command in ('build', 'build-loc', 'build-font'):
            component = {'build': 'all', 'build-loc': 'loc', 'build-font': 'font'}[args.command]
            if getattr(args, 'font', None) is not None or getattr(args, 'pixel_size', None) is not None:
                configure_font(args.font, args.pixel_size if args.pixel_size is not None else 32)
            build(args.mode, getattr(args, 'plugin', None), component)
        elif args.command == 'verify':
            print('[通过] '+json.dumps(verify(args.plugin), ensure_ascii=False))
        elif args.command == 'export':
            export_original()
        else:
            status()
        return 0
    except (OSError, ValueError, KeyError, AssertionError, RuntimeError, TypeError) as exc:
        print('[错误]', exc, file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
