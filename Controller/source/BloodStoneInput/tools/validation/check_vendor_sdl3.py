#!/usr/bin/env python3
"""检查发行包内 SDL3 上游源码版本与完整性标记，防止误用旧版本。"""
from pathlib import Path
import re
import sys
import hashlib

ROOT = Path(__file__).resolve().parents[2]
SDL = ROOT / 'tools' / 'vendor' / 'SDL3'
required = [SDL / x for x in ('CMakeLists.txt', 'LICENSE.txt', 'include/SDL3/SDL.h',
                              'include/SDL3/SDL_version.h', 'src/SDL.c', 'src/joystick/SDL_joystick.c')]
missing = [str(x.relative_to(SDL)) for x in required if not x.is_file()]
if missing:
    print('[失败] SDL3 源码文件缺失：' + ', '.join(missing), file=sys.stderr)
    raise SystemExit(1)
header = (SDL / 'include/SDL3/SDL_version.h').read_text(encoding='utf-8')
for field, expected in (('MAJOR', 3), ('MINOR', 4), ('MICRO', 18)):
    match = re.search(r'^\s*#\s*define\s+SDL_' + field + r'_VERSION\s+(\d+)', header, re.M)
    if not match or int(match.group(1)) != expected:
        print('[失败] SDL3 版本头错误：' + field, file=sys.stderr)
        raise SystemExit(1)
# 每个源文件均与本版已核验的原始归档一致，拒绝缺失、被替换和多余文件。
manifest = ROOT.parents[1] / 'docs' / 'SDL3源码SHA256清单.txt'
if not manifest.is_file():
    print('[失败] 找不到 SDL3 逐文件 SHA256 清单。', file=sys.stderr)
    raise SystemExit(1)
expected = {}
for line in manifest.read_text('utf-8').splitlines():
    digest, name = line.split('  ', 1)
    expected[name] = digest
actual_files = {p.relative_to(SDL).as_posix(): p for p in SDL.rglob('*') if p.is_file()}
if actual_files.keys() != expected.keys():
    print('[失败] SDL3 文件集合与清单不一致。', file=sys.stderr)
    raise SystemExit(1)
for name, path in actual_files.items():
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected[name]:
        print('[失败] SDL3 文件内容校验失败：' + name, file=sys.stderr)
        raise SystemExit(1)
print(f'[通过] SDL3 3.4.18 上游源码完整，{len(expected)} 个文件 SHA256 全部吻合。')
