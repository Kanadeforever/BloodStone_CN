#!/usr/bin/env python3
"""验证七个独立代理；发行阶段额外验证与 DLL 同名的 TOML。"""

from pathlib import Path
import argparse
import hashlib

NAMES = [
    "xinput1_1.dll", "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll",
    "xinput9_1_0.dll", "dinput8.dll", "winmm.dll",
]

# 同一个工具按阶段执行不同检查。编译产物目录只有 DLL，尚未复制配置；
# 正式发行目录则必须同时具备与每个代理同名的 TOML。
# 默认使用最严格的发行检查，只有构建器明确传入 --dll-only 时才跳过配置检查。
parser = argparse.ArgumentParser(description='检查七个独立代理及正式发行所需的同名配置')
parser.add_argument('folder', type=Path, help='待检查的单架构目录')
parser.add_argument('--dll-only', action='store_true',
                    help='仅用于刚完成链接的构建目录；正式发行不得使用')
args = parser.parse_args()
folder = args.folder
missing = [name for name in NAMES if not (folder / name).is_file()]
if missing:
    raise SystemExit("缺少入口文件：" + ", ".join(missing))
if (folder / "version.dll").exists():
    raise SystemExit("已停止支持的 version.dll 不得出现在发行目录")
if not args.dll_only:
    # 配置由发布阶段复制，不能在 build_arch() 结束时提前检查。
    missing_configs = [name.removesuffix('.dll') + '.toml' for name in NAMES
                       if not (folder / (name.removesuffix('.dll') + '.toml')).is_file()]
    if missing_configs:
        raise SystemExit('缺少与代理同名的 TOML：' + ', '.join(missing_configs))
    if (folder / 'joyshim.toml').exists():
        raise SystemExit('正式发行不得再包含固定的 joyshim.toml')

hashes = {
    name: hashlib.sha256((folder / name).read_bytes()).hexdigest()
    for name in NAMES
}
for name, digest in hashes.items():
    print(f"{digest}  {name}")
if len(set(hashes.values())) != len(NAMES):
    groups: dict[str, list[str]] = {}
    for name, digest in hashes.items():
        groups.setdefault(digest, []).append(name)
    duplicates = [names for names in groups.values() if len(names) > 1]
    raise SystemExit(
        "发现被直接复制的相同代理二进制：" +
        "; ".join(", ".join(names) for names in duplicates))
print("七个独立代理二进制检查通过。")
if not args.dll_only:
    print("七份代理同名 TOML 检查通过。")
