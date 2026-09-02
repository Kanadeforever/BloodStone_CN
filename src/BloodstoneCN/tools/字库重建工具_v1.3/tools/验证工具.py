#!/usr/bin/env python3
"""BloodStoneCN 中日文字库重建工具 v1.3 自检。

不依赖用户提供字体即可检查目录结构和 JSON 中/日字段扫描规则；若
font_source 中已经存在字体，还会检查“全量固定保留标点”集合。
"""
from __future__ import annotations

import json
import tempfile
import unicodedata
from pathlib import Path

from font_atlas_tool import (
    discover_font_files,
    scan_json_directory,
    select_font_file,
    select_font_index,
    supported_punctuation_codepoints,
)


def main() -> int:
    tool_dir = Path(__file__).resolve().parent
    root = tool_dir.parent
    checks: dict[str, bool] = {
        "保持tools子目录": tool_dir.name == "tools",
        "font_source位于根目录": (root / "font_source").is_dir(),
        "根目录安装依赖BAT": (root / "安装依赖.bat").is_file(),
        "根目录扫描JSON字符BAT": (root / "扫描JSON字符.bat").is_file(),
        "根目录重建字体BAT": (root / "重建字体.bat").is_file(),
    }
    details: dict[str, object] = {}

    with tempfile.TemporaryDirectory() as td:
        sample = Path(td)
        (sample / "子目录").mkdir()
        (sample / "主.json").write_text(
            json.dumps(
                {
                    "source": "Stop Greco's attack",
                    "zh-CN": "阻止格雷科的攻势",
                    "ja-JP": "グレコの攻撃を阻止せよ",
                    "meta": {"note": "中文备注"},
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        (sample / "子目录" / "次.json").write_text(
            json.dumps(
                {
                    "entries": [
                        {"chinese": "按下方向键", "japanese": "方向キーを押す"},
                        {"中文": "任务更新", "日本語": "ミッション更新"},
                    ]
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )

        zh = scan_json_directory(sample, "zh")
        ja = scan_json_directory(sample, "ja")
        checks["递归扫描2个JSON"] = len(zh.files) == 2 and len(ja.files) == 2
        checks["中文排除日文字段"] = all(
            ch not in zh.all_characters for ch in "グレコミッションキー"
        )
        checks["中文保留中文"] = all(
            ch in zh.han_characters for ch in "阻止格雷科任务更新"
        )
        checks["日文只取日文字段"] = (
            "中" not in ja.all_characters
            and "グ" in ja.all_characters
            and "攻" in ja.all_characters
        )
        checks["日文统计假名"] = len(ja.kana_characters) > 0

    fonts = discover_font_files(root / "font_source")
    if fonts:
        for language in ("zh", "ja"):
            font = select_font_file(root / "font_source", language, None)
            index = select_font_index(font, language, None)
            punctuation = supported_punctuation_codepoints(font, index)
            checks[f"{language}字体全量标点集合非空"] = bool(punctuation)
            checks[f"{language}标点集合类别合法"] = all(
                cp == 0x3000 or unicodedata.category(chr(cp)).startswith("P")
                for cp in punctuation
            )
            details[f"{language}_punctuation_count"] = len(punctuation)
    else:
        details["font_test"] = "SKIP：交付包按要求不包含字体；放入字体后可再次运行自检。"

    status = "PASS" if all(checks.values()) else "FAIL"
    result = {"status": status, "checks": checks, "details": details}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
