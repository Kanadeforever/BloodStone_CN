# -*- coding: utf-8 -*-
"""LOC 翻译工具：只允许访问本工具目录，绝不写入游戏安装目录。

入口提供两个操作：
1. 从用户拷入的原版 gamedata.pak 生成 86 个待翻译 JSON。
2. 将本目录 translations 中的 86 份译文转换为可供 BloodstoneCN 使用的外置 LOC。

本工具不会重封 PAK，任何时候都不修改 input 中的原版文件。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import tempfile
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = Path(__file__).resolve().parent
if not (CORE / 'loc_json.py').is_file() or not (CORE / 'native_pak.py').is_file():
    raise SystemExit('[错误] 转换核心丢失，请解压完整的 toolbox 文件夹')
sys.path.insert(0, str(CORE))
from loc_json import export_batch, import_batch, json_bytes  # noqa: E402
from native_pak import NativePak  # noqa: E402


def local(path: Path) -> Path:
    """拒绝所有包外路径；本工具不支持传入任意绝对游戏目录。"""
    full = (ROOT / path).resolve()
    if not full.is_relative_to(ROOT.resolve()):
        raise ValueError('不允许读取或写入工具目录以外的文件')
    return full


def source_pak() -> Path:
    """用户主动复制原版 PAK；读取前确认它是可解析的原生封包。"""
    pak = local(Path('input/gamedata.pak'))
    if not pak.is_file():
        raise FileNotFoundError('请先把原版 gamedata.pak 复制到 input\\gamedata.pak')
    return pak


def inspect_json_dir(directory: Path) -> int:
    """输入目录必须具备完整 86 份 JSON，不接受半套更新。"""
    return len(list(directory.rglob('*.loc.json')))


def verify_local_tree() -> None:
    """禁止符号链接和目录联接将文件访问引导到工具目录外。"""
    for name in ('input', 'translations', 'output'):
        folder = ROOT / name
        if folder.is_symlink() or (hasattr(folder, 'is_junction') and folder.is_junction()):
            raise ValueError(f'{name} 不能是链接目录')
        for current, directories, files in os.walk(folder, followlinks=False):
            for entry in [*directories, *files]:
                path = Path(current) / entry
                if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
                    raise ValueError(f'链接文件/目录不允许用于工具输入输出：{path.relative_to(ROOT)}')
                if not path.resolve().is_relative_to(ROOT):
                    raise ValueError('工具检测到目录之外的输入输出，已拒绝')


def publish(stage: Path, pairs: list[tuple[Path, Path]]) -> None:
    """先完成整套文件，再以重命名方式更新，并在失败时恢复原有产物。"""
    backups: list[tuple[Path, Path]] = []
    installed: list[Path] = []
    try:
        for index, (source, target) in enumerate(pairs):
            if not source.exists():
                raise ValueError(f'暂存文件丢失：{source.name}')
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                backup = stage / f'old-{index}'
                target.replace(backup)
                backups.append((backup, target))
            source.replace(target)
            installed.append(target)
    except Exception:
        for target in reversed(installed):
            if target.is_dir():
                shutil.rmtree(target)
            elif target.exists():
                target.unlink()
        for backup, target in reversed(backups):
            backup.replace(target)
        raise


def export_untranslated() -> dict:
    """从原版 PAK 导出全套待翻译 JSON，只有成功后才公布成品。"""
    verify_local_tree()
    pak = source_pak()
    base = local(Path('output/loc/untranslated_json'))
    original = NativePak(pak.read_bytes())
    resources = sorted(k for k in original.by_name if k.endswith('.loc'))
    if len(resources) != 86:
        raise ValueError(f'当前原版预期 86 个 LOC，实际为 {len(resources)}')
    if base.exists() and any(base.iterdir()):
        raise ValueError('output\\untranslated_json 已有文件，拒绝覆盖既有翻译')
    # 先在内存中处理全部资源。即使某一个 LOC 损坏，也不会留下半套 JSON。
    from loc_json import export_one
    staged = []
    count = 0
    for resource in resources:
        payload = original.read_name(resource)
        doc = export_one(payload, payload, resource)
        count += len(doc['entries'])
        staged.append((Path(resource + '.json'), json_bytes(doc)))
    result = {'mode': 'export', 'files': len(staged), 'entries': count,
              'output': 'output/loc/untranslated_json',
              'instruction': '复制整套 JSON 到 translations 目录后修改 translate，再运行导入'}
    output = local(Path('output/loc'))
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.loc-stage-', dir=output) as temporary:
        stage = Path(temporary)
        folder = stage / 'untranslated_json'
        for relative, payload in staged:
            destination = folder / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(payload)
        info = stage / 'result.json'
        info.write_bytes(json_bytes(result))
        publish(stage, [(folder, base), (info, output / 'result.json')])
    return result


def import_translated() -> dict:
    """全部 JSON 检查与 LOC 重建成功后，才替换旧版输出。"""
    verify_local_tree()
    pak = source_pak()
    translations = local(Path('translations'))
    if inspect_json_dir(translations) != 86:
        raise ValueError('translations 目录必须完整包含 86 份 *.loc.json（不能只放一个 JSON）')
    output = local(Path('output/loc'))
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.loc-stage-', dir=output) as temporary:
        stage = Path(temporary)
        native_out = stage / 'native_loc'
        zipped = stage / 'native_loc.zip'
        result = import_batch(pak, translations, native_out, zip_output=zipped,
                              baseline_dir=None, font_bfa=None)
        expected = native_out / 'gamedata/xt/loc'
        files = sorted(expected.rglob('*.loc'))
        if len(files) != 86:
            raise ValueError('中间 LOC 不完整，拒绝复制到可安装目录')
        out_root = stage / 'BSCNContents/loc'
        for source in files:
            relative = source.relative_to(expected)
            destination = out_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            if source.read_bytes() != destination.read_bytes():
                raise AssertionError('原生 LOC 拷贝内容不一致')
        if len(list(out_root.rglob('*.loc'))) != 86:
            raise ValueError('可安装 LOC 集合不完整')
        result.update({'mode': 'import', 'install_directory': 'output/loc/BSCNContents/loc',
                       'pak_unchanged_sha256': hashlib.sha256(pak.read_bytes()).hexdigest()})
        info = stage / 'result.json'
        info.write_bytes(json_bytes(result))
        publish(stage, [(native_out, output / 'native_loc'),
                        (out_root, output / 'BSCNContents/loc'),
                        (zipped, output / 'native_loc.zip'),
                        (info, output / 'result.json')])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description='BloodstoneCN 原生 LOC 与 JSON 双向转换')
    parser.add_argument('command', choices=('export', 'import'))
    args = parser.parse_args()
    try:
        result = export_untranslated() if args.command == 'export' else import_translated()
        print(f'[完成] {args.command}：{result["files"]} 份文件、{result["entries"]} 条文本')
        print('[输出]', result.get('install_directory', result['output']))
        if result.get('warning_count'):
            print('[提醒] 发现', result['warning_count'], '条译文控制标记差异，请检查 output\\loc\\result.json')
        return 0
    except (OSError, ValueError, KeyError, AssertionError, json.JSONDecodeError) as exc:
        print('[错误]', exc, file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
