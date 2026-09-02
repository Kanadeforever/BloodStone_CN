#!/usr/bin/env python3
"""递归统计 BloodstoneCN 翻译 JSON 字符。实际扫描逻辑复用 font_atlas_tool.py。"""

from __future__ import annotations

import argparse
from pathlib import Path

from font_atlas_tool import scan_json_directory, write_json_scan_outputs


def main() -> int:
    parser = argparse.ArgumentParser(
        description="递归扫描指定目录及子目录的全部 JSON，并按中文/日文字段策略统计字符"
    )
    parser.add_argument("目录", type=Path, help="要扫描的 JSON 根目录")
    parser.add_argument(
        "--language",
        "--语言",
        dest="language",
        choices=("zh", "ja"),
        default="zh",
        help="zh=排除日文字段；ja=只扫描日文字段（默认 zh）",
    )
    parser.add_argument(
        "--output",
        "--输出",
        dest="output",
        type=Path,
        default=Path("字符统计输出"),
    )
    args = parser.parse_args()
    result = scan_json_directory(args.目录, args.language)
    report = write_json_scan_outputs(args.output.resolve(), result)
    print(f"[OK] JSON 文件：{report['json_file_count']}")
    print(f"[OK] 使用字符串：{report['string_count_used']}")
    print(f"[OK] 排除字符串：{report['string_count_excluded']}")
    print(f"[OK] 唯一字符：{report['unique_all_characters']}")
    print(f"[OK] 唯一汉字：{report['unique_han_characters']}")
    if args.language == "ja":
        print(f"[OK] 唯一假名：{report['unique_kana_characters']}")
    print(f"[OK] 输出目录：{args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
