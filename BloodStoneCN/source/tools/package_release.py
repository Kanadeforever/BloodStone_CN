# -*- coding: utf-8 -*-
"""仅按人工审定白名单生成完整公开包，个人游戏输入和成品永不自动打包。"""
from __future__ import annotations

import argparse
import hashlib
import json
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
FIXED_TIME = (2026, 10, 15, 0, 0, 0)
ASI_SHA256 = 'f5962e97828607463e8c70b79d09597fe962965510d0695cf379b04dfe8a637b'
ASI_PATH = 'release/Runtime/BloodstoneCN.asi'
BAD_SUFFIXES = {'.pak', '.loc', '.dat', '.bpr', '.bfa', '.ttf', '.otf', '.ttc',
                '.png', '.jpg', '.dds', '.wav', '.ogg', '.mp3', '.exe', '.dll', '.pyc', '.zip'}
REGISTRY = 'source/config/public_files.json'
MANIFEST = 'docs/校验清单.sha256'


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def collect_public_files(root: Path = ROOT) -> dict[str, bytes]:
    """白名单固定的是文件身份，不会递归采集新增文件、游戏目录或工具输出。"""
    registry_path = root/REGISTRY
    if registry_path.is_symlink() or not registry_path.resolve().is_relative_to(root.resolve()):
        raise ValueError('公开文件清单不能指向工程外部')
    registry = json.loads(registry_path.read_text(encoding='utf-8'))
    names = registry['files']
    if not isinstance(names, list) or not all(isinstance(n, str) for n in names):
        raise ValueError('公开文件清单必须为字符串数组')
    if len(names) != len(set(names)) or REGISTRY not in names or ASI_PATH not in names:
        raise ValueError('公开文件清单重复或不完整')
    result = {}
    for name in sorted(names):
        relative = PurePosixPath(name)
        if (not name or '\\' in name or ':' in name or relative.is_absolute()
                or any(part in ('..', '.', '__pycache__', '.git', '.caches', '.build',
                                'output', 'work', 'dist', 'input', 'translations') for part in relative.parts)
                or name.endswith('.loc.json') or relative.suffix.lower() in BAD_SUFFIXES):
            raise ValueError(f'白名单含禁止公开的路径或资源类型：{name}')
        if name == MANIFEST:
            raise ValueError('校验清单由打包器生成，不能登记它自身')
        file = root.joinpath(*relative.parts)
        if not file.resolve().is_relative_to(root.resolve()) or file.is_symlink():
            raise ValueError(f'公开文件越界：{name}')
        for parent in (file, *file.parents):
            if parent == root.parent:
                break
            if parent.is_symlink() or (parent.exists() and bool(
                    getattr(parent.lstat(), 'st_file_attributes', 0) & 0x400)):
                raise ValueError(f'公开文件包含链接或重解析点：{name}')
            if parent == root:
                break
        payload = file.read_bytes()
        if name == ASI_PATH:
            if sha(payload) != ASI_SHA256:
                raise ValueError('正式 ASI 已改变；公开工具版冻结原 v1.2 二进制，请先独立审核新编译产物')
        else:
            if payload.startswith(b'\xef\xbb\xbf'):
                raise ValueError(f'文本含 BOM：{name}')
            text = payload.decode('utf-8')
            if b'\n' in payload.replace(b'\r\n', b'') or b'\r' in payload.replace(b'\r\n', b''):
                raise ValueError(f'文本未统一为 CRLF：{name}')
            if relative.suffix.lower() == '.bat':
                if 'chcp 65001 >nul' not in text:
                    raise ValueError(f'BAT 缺少 UTF-8 代码页：{name}')
                for line in text.splitlines():
                    if line.lstrip().lower().startswith(('echo ', 'rem ')) and not line.endswith('  '):
                        raise ValueError(f'BAT 文字行缺少结尾空格：{name}')
            if relative.suffix.lower() == '.json' and name not in {
                    REGISTRY, 'source/config/profile.json',
                    'toolbox/config/defaults.json'}:
                raise ValueError(f'未经允许的 JSON 数据：{name}')
            result[name] = payload
            continue
        result[name] = payload
    return result


def package(root: Path, output: Path) -> dict:
    files = collect_public_files(root)
    lines = [f'{sha(data)}  {name}' for name, data in sorted(files.items())]
    files[MANIFEST] = ('\r\n'.join(lines)+'\r\n').encode('utf-8')
    (root/MANIFEST).write_bytes(files[MANIFEST])
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name+'.part')
    try:
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for directory in ('.build/', 'toolbox/input/template/', 'toolbox/input/fonts/',
                              'toolbox/translations/',
                              'release/Runtime/BSCNContents/loc/', 'release/Runtime/BSCNContents/fonts/'):
                archive.writestr(zipfile.ZipInfo(directory, FIXED_TIME), b'')
            for name, data in sorted(files.items()):
                item = zipfile.ZipInfo(name, FIXED_TIME)
                item.compress_type = zipfile.ZIP_DEFLATED
                item.external_attr = 0o100644 << 16
                archive.writestr(item, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
        with zipfile.ZipFile(temporary) as archive:
            if archive.testzip() is not None:
                raise ValueError('公开 ZIP 的 CRC 校验失败')
            actual = {n for n in archive.namelist() if not n.endswith('/')}
            if actual != set(files):
                raise ValueError('公开 ZIP 的文件集合不是审定白名单')
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    result = {'files': len(files), 'bytes': output.stat().st_size, 'sha256': sha(output.read_bytes())}
    print('[完成] 无游戏资产公开包：', output)
    print('[校验]', json.dumps(result, ensure_ascii=False))
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description='BloodstoneCN 无游戏资产公开工具包封装')
    parser.add_argument('--output', type=Path, default=ROOT/'release/dist/BloodStoneCN_M1_v1.2_PublicToolkit_GUI_PakFix_20261015.zip')
    args = parser.parse_args()
    package(ROOT, args.output.resolve())


if __name__ == '__main__':
    main()
