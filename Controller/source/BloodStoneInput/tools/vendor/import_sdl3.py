#!/usr/bin/env python3
"""将经过核验的 SDL 3.4.18 完整 ZIP 源码安全导入 JoyShim 的离线构建目录。"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
from zipfile import ZipFile, BadZipFile

# 只接受已确定的 SDL 源码版本。更新到其他版本应当先复核 CMake 和 ABI。
VERSION = (3, 4, 18)
VERSION_TEXT = '.'.join(map(str, VERSION))
SOURCE_ROOT_NAMES = {f'SDL-release-{VERSION_TEXT}'}
EXPECTED_SOURCE_ZIP_SHA256 = '8a93da90d4fb8f7797acc626a15078bc106655dfc44102f920b5af478222b797'
VENDOR_DIR = Path(__file__).resolve().parent
TARGET_DIR = VENDOR_DIR / 'SDL3'
MAX_FILES = 25000
MAX_UNPACKED_SIZE = 650 * 1024 * 1024


def check_archive(archive: Path) -> tuple[str, list]:
    """预先审核全部 ZIP 条目，避免路径穿越、符号链接和异常膨胀。"""
    with ZipFile(archive) as source:
        members = source.infolist()
        if not members or len(members) > MAX_FILES:
            raise ValueError('压缩包缺少内容或条目过多。')
        roots = set()
        total_size = 0
        for member in members:
            # ZIP 内部统一使用 /；绝对路径、盘符、反斜杠和 .. 一律禁止。
            name = member.filename
            path = PurePosixPath(name)
            if (not name or '\\' in name or name.startswith('/') or
                    ':' in name or '..' in path.parts or path.is_absolute()):
                raise ValueError(f'压缩包包含不安全路径：{name!r}')
            roots.add(path.parts[0])
            mode = member.external_attr >> 16
            if stat.S_IFMT(mode) == stat.S_IFLNK:
                raise ValueError(f'不接受 ZIP 内的符号链接：{name!r}')
            if (mode and stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)):
                raise ValueError(f'压缩包包含不支持的特殊文件：{name!r}')
            total_size += member.file_size
            if total_size > MAX_UNPACKED_SIZE:
                raise ValueError('解压后内容超过 650 MiB 上限。')
        if len(roots) != 1 or next(iter(roots)) not in SOURCE_ROOT_NAMES:
            raise ValueError(f'压缩包顶级目录不属于 SDL {VERSION_TEXT} 源码：{sorted(roots)}')
        root = next(iter(roots))
        paths = {str(PurePosixPath(item.filename)) for item in members}
        needed = (f'{root}/CMakeLists.txt', f'{root}/include/SDL3/SDL_version.h')
        if not all(item in paths for item in needed):
            raise ValueError('源码 ZIP 缺少 CMakeLists.txt 或 SDL_version.h。')
        if not any(f'{root}/{license_name}' in paths for license_name in ('LICENSE.txt', 'LICENSE')):
            raise ValueError('SDL3 上游许可证文件缺失。')
        if not any(item.startswith(f'{root}/src/') for item in paths):
            raise ValueError('源码 ZIP 缺少 SDL3 实现文件。')
        return root, members


def check_version(source: Path) -> None:
    """读取 SDL 官方版本头，避免同名压缩包中混入其他版本。"""
    text = (source / 'include' / 'SDL3' / 'SDL_version.h').read_text('utf-8')
    for name, expected in zip(('MAJOR', 'MINOR', 'MICRO'), VERSION):
        match = re.search(r'^\s*#\s*define\s+SDL_' + name + r'_VERSION\s+(\d+)', text, re.M)
        if not match or int(match.group(1)) != expected:
            raise ValueError(f'SDL3 版本头不符合 {VERSION_TEXT}：{name}。')


def import_sdl(archive: Path, target: Path = TARGET_DIR) -> str:
    """完整解压到临时目录，核验成功后再安全替换旧源码。"""
    archive = archive.expanduser().absolute()
    if not archive.is_file():
        raise FileNotFoundError(f'找不到 SDL3 离线源码包：{archive}')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    # 对指定的用户提供的源码 ZIP 做字节级校验；其他自建测试包依赖内部文件检查。
    if archive.name == f'SDL-release-{VERSION_TEXT}.zip' and digest != EXPECTED_SOURCE_ZIP_SHA256:
        raise ValueError('SDL3 3.4.18 源码压缩包的 SHA256 与已确认样本不一致。')
    root, members = check_archive(archive)
    target.parent.mkdir(parents=True, exist_ok=True)
    # 暂存目录位于同一文件系统，保证可以通过重命名切换完整目录。
    with tempfile.TemporaryDirectory(prefix='joyshim-sdl-', dir=target.parent) as temporary:
        work = Path(temporary)
        with ZipFile(archive) as source:
            for member in members:
                relative = PurePosixPath(member.filename).parts[1:]
                destination = work.joinpath(*relative)
                if member.is_dir():
                    destination.mkdir(parents=True, exist_ok=True)
                elif relative:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with source.open(member) as src, destination.open('wb') as dst:
                        shutil.copyfileobj(src, dst, 1024 * 1024)
        check_version(work)
        backup = target.with_name(target.name + '.previous')
        if backup.exists():
            raise RuntimeError(f'发现未清理的上次备份，停止以保护数据：{backup}')
        try:
            if target.exists():
                target.rename(backup)
            work.rename(target)
        except Exception:
            if backup.exists() and not target.exists():
                backup.rename(target)
            raise
        if backup.exists():
            shutil.rmtree(backup)
    print(f'[完成] 已完整导入 SDL3 {VERSION_TEXT} 源码：{target}')
    print(f'[校验] 原始压缩包 SHA256：{digest}')
    return digest


def main() -> None:
    parser = argparse.ArgumentParser(description='导入 SDL3 3.4.18 完整离线源码')
    parser.add_argument('archive', type=Path, help='SDL-release-3.4.18.zip')
    options = parser.parse_args()
    import_sdl(options.archive)


if __name__ == '__main__':
    main()
