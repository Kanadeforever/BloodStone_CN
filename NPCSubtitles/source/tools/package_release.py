# -*- coding: utf-8 -*-
"""为 NPCSubtitles 创建可独立接档的完整压缩包，并刷新 SHA-256 文件清单。"""
from pathlib import Path
from hashlib import sha256
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED
import argparse

# 此工具位于「发行根目录/source/NPCSubtitles/tools」，因此向上三级即项目根目录。
ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "docs" / "文件校验.sha256"
DIRECTORIES = ("docs", "source", "release", ".build")

def collect():
    """只收集项目交付文件，不把 Git 数据、临时缓存或 Python 字节码打入包中。"""
    result = []
    for directory in DIRECTORIES:
        folder = ROOT / directory
        if not folder.is_dir():
            raise FileNotFoundError(f"缺失交付目录：{folder}")
        for file in folder.rglob("*"):
            if not file.is_file() or file == MANIFEST:
                continue
            relative = file.relative_to(ROOT)
            if any(x in {".git", "__pycache__", ".pytest_cache", "NativeCross"} for x in relative.parts):
                continue
            if file.suffix.lower() in {".pyc", ".obj", ".lib", ".pdb"}:
                continue
            result.append(file)
    return sorted(result, key=lambda file: file.relative_to(ROOT).as_posix())

def write_manifest(files):
    """校验清单不包含自身，避免每次重新计算都改变自身哈希。"""
    lines = ["# NPCSubtitles 当前正式封版发行文件 SHA-256（不包含本清单自身）"]
    for file in files:
        rel = file.relative_to(ROOT).as_posix()
        lines.append(f"{sha256(file.read_bytes()).hexdigest()}  {rel}")
    MANIFEST.write_bytes(("\r\n".join(lines) + "\r\n").encode("utf-8"))

def create_zip(output, files):
    """统一 Zip 时间戳，保证相同输入可以反复得到相同归档内容。"""
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", compression=ZIP_DEFLATED, compresslevel=9) as package:
        for file in sorted([*files, MANIFEST], key=lambda x: x.relative_to(ROOT).as_posix()):
            rel = file.relative_to(ROOT).as_posix()
            info = ZipInfo(rel, date_time=(2026, 10, 20, 0, 0, 0))
            info.compress_type = ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            package.writestr(info, file.read_bytes(), compress_type=ZIP_DEFLATED, compresslevel=9)
    return output

def main():
    parser = argparse.ArgumentParser(description="生成 NPCSubtitles 当前完整离线发行包")
    parser.add_argument("--output", type=Path, default=ROOT.parent / "NPCSubtitles_v0.1e-log9_四条过场白名单_维护封版完整工程包_20261020.zip")
    args = parser.parse_args()
    if not (ROOT / "release" / "NPCSubtitles.asi").is_file():
        raise FileNotFoundError("缺少已编译的 release/NPCSubtitles.asi，请先运行构建脚本")
    files = collect()
    write_manifest(files)
    create_zip(args.output, files)
    print(f"[完成] 校验文件：{len(files)}；压缩包：{args.output}")

if __name__ == "__main__":
    main()
