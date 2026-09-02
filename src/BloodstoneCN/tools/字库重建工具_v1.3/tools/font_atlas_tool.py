#!/usr/bin/env python3
"""BloodstoneCN BFA 字库生成、检查、标准网格、PNG 导出与回导工具。

BFA v1 由 64 字节头、固定 32 字节字形记录和一张 A8 灰度图组成。
导出的 PNG 约定为：

* 黑色 0：完全没有字形覆盖；
* 白色 255：完全覆盖；
* 灰色：抗锯齿边缘。

典型命令：

    python tools/font_atlas_tool.py build \
      --font font_source/NotoSansSC-BloodStone-Full.ttf \
      --index fonts/unicode_loc_index.bli \
      --font-size 40 --atlas-width 2048 --atlas-height 2048 \
      --output font_build

    python tools/font_atlas_tool.py export \
      --bfa fonts/cn_font.bfa --output font_export

    python tools/font_atlas_tool.py import-png \
      --bfa fonts/cn_font.bfa --png font_export/cn_font.png \
      --metrics font_export/cn_font.glyphs.tsv \
      --output font_export/cn_font_edited.bfa

    python tools/font_atlas_tool.py repack-grid \
      --bfa fonts/cn_font.bfa \
      --cell-width 36 --cell-height 44 \
      --origin-x 2 --baseline-y 35 \
      --atlas-width 2048 --atlas-height 2048 \
      --output font_grid
全量重建模式（推荐日常使用，v1.2 固定保留字体全部标点）：

    python font_atlas_tool.py rebuild --language zh \
      --json-dir 翻译JSON --font-source font_source --output 输出/中文

    python font_atlas_tool.py rebuild --language ja \
      --json-dir 翻译JSON --font-source font_source --output 输出/日文

中文模式会递归扫描全部 JSON 并排除明确日文字段；日文模式只扫描明确
日文字段。rebuild 每次从当前 JSON 重新统计字符并完整重建 fonts.bfa，
不读取旧 fonts.bfa，也不做“新增字符”式增量追加。

"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import struct
import sys
import unicodedata
from collections import Counter
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Iterable, Sequence

from PIL import Image, ImageDraw, ImageFont

try:
    from fontTools.ttLib import TTCollection, TTFont
except ImportError:  # pragma: no cover
    TTFont = None
    TTCollection = None


MAGIC = b"BSCNATL1"
VERSION = 1
HEADER_SIZE = 64
RECORD_SIZE = 32
PIXEL_FORMAT_A8 = 1
TOKEN_WIDTH = 4
TOKEN_PREFIX = b"QZ"
TOKEN_ALPHABET_ID = 1
NO_TOKEN = 0xFFFFFFFF
TOKEN_CAPACITY = 36**4
TOKEN_MULTIPLIER = 104729
TOKEN_OFFSET = 742381

HEADER = struct.Struct("<8s12I4BI")
RECORD = struct.Struct("<IIHHHHhhhHII")
BLI_HEADER = struct.Struct("<8sIIIIII")
BLI2_RECORD_PREFIX = struct.Struct("<III")
BLI3_RECORD_PREFIX = struct.Struct("<IIII")
TEXT_EXTENSIONS = {
    ".txt",
    ".json",
    ".csv",
    ".tsv",
    ".md",
}


@dataclass(frozen=True)
class GlyphRecord:
    codepoint: int
    carrier_token: int
    x: int
    y: int
    width: int
    height: int
    bearing_x: int
    bearing_y: int
    advance_x: int
    flags: int = 0
    reserved0: int = 0
    reserved1: int = 0


@dataclass(frozen=True)
class BfaAtlas:
    width: int
    height: int
    line_height: int
    ascent: int
    records: tuple[GlyphRecord, ...]
    pixels: bytes


@dataclass(frozen=True)
class RasterGlyph:
    record: GlyphRecord
    image: Image.Image | None


@dataclass(frozen=True)
class GridPlacement:
    """一个字形从紧凑矩形移动到固定网格单元的完整对应关系。"""

    row: int
    column: int
    cell_x: int
    cell_y: int
    local_ink_x: int
    local_ink_y: int
    original: GlyphRecord
    grid: GlyphRecord


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def unicode_scalar(value: int) -> bool:
    return 0 <= value <= 0x10FFFF and not 0xD800 <= value <= 0xDFFF


def visible_character(codepoint: int) -> str:
    character = chr(codepoint)
    return {
        "\t": r"\t",
        "\n": r"\n",
        "\r": r"\r",
        " ": r"\s",
    }.get(character, character)


def token_for_codepoint(codepoint: int) -> int:
    if not unicode_scalar(codepoint):
        raise ValueError(f"不是 Unicode 标量：U+{codepoint:04X}")
    return (codepoint * TOKEN_MULTIPLIER + TOKEN_OFFSET) % TOKEN_CAPACITY


def cp1252_extras() -> set[int]:
    result: set[int] = set()
    for value in range(0x80, 0x100):
        try:
            result.add(ord(bytes((value,)).decode("cp1252")))
        except UnicodeDecodeError:
            pass
    return result


def validate_record(record: GlyphRecord, width: int, height: int) -> None:
    if not unicode_scalar(record.codepoint):
        raise ValueError(f"无效码点：U+{record.codepoint:04X}")
    if not 0 <= record.carrier_token <= 0xFFFFFFFF:
        raise ValueError(f"U+{record.codepoint:04X} 的 carrier_token 越界")
    for name, value in (
        ("x", record.x),
        ("y", record.y),
        ("width", record.width),
        ("height", record.height),
    ):
        if not 0 <= value <= 0xFFFF:
            raise ValueError(
                f"U+{record.codepoint:04X} 的 {name} 不适合 uint16：{value}"
            )
    if record.x + record.width > width or record.y + record.height > height:
        raise ValueError(
            f"U+{record.codepoint:04X} 矩形越界："
            f"{record.x},{record.y},{record.width},{record.height} / "
            f"{width}x{height}"
        )
    for name, value in (
        ("bearing_x", record.bearing_x),
        ("bearing_y", record.bearing_y),
        ("advance_x", record.advance_x),
    ):
        if not -0x8000 <= value <= 0x7FFF:
            raise ValueError(
                f"U+{record.codepoint:04X} 的 {name} 不适合 int16：{value}"
            )
    if not 0 <= record.flags <= 0xFFFF:
        raise ValueError(f"U+{record.codepoint:04X} 的 flags 越界")
    for name, value in (
        ("reserved0", record.reserved0),
        ("reserved1", record.reserved1),
    ):
        if not 0 <= value <= 0xFFFFFFFF:
            raise ValueError(f"U+{record.codepoint:04X} 的 {name} 越界")


def validate_atlas(atlas: BfaAtlas) -> None:
    if not 1 <= atlas.width <= 8192 or not 1 <= atlas.height <= 8192:
        raise ValueError(f"图集尺寸越界：{atlas.width}x{atlas.height}")
    if not 1 <= atlas.line_height <= 4096:
        raise ValueError(f"line_height 越界：{atlas.line_height}")
    if not 1 <= atlas.ascent <= atlas.line_height * 2:
        raise ValueError(f"ascent 越界：{atlas.ascent}")
    if not atlas.records or len(atlas.records) > 200000:
        raise ValueError(f"字形数量无效：{len(atlas.records)}")
    if len(atlas.pixels) != atlas.width * atlas.height:
        raise ValueError(
            f"A8 像素数不匹配：{len(atlas.pixels)} != "
            f"{atlas.width * atlas.height}"
        )
    codepoints: set[int] = set()
    tokens: set[int] = set()
    for record in atlas.records:
        validate_record(record, atlas.width, atlas.height)
        if record.codepoint in codepoints:
            raise ValueError(f"重复码点：U+{record.codepoint:04X}")
        codepoints.add(record.codepoint)
        if record.carrier_token != NO_TOKEN:
            if record.carrier_token >= TOKEN_CAPACITY:
                raise ValueError(
                    f"U+{record.codepoint:04X} 的载体 token 越界："
                    f"{record.carrier_token}"
                )
            if record.carrier_token in tokens:
                raise ValueError(
                    f"重复载体 token：{record.carrier_token}"
                )
            tokens.add(record.carrier_token)


def read_bfa(path: Path) -> BfaAtlas:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError(f"BFA 小于 64 字节：{path}")
    (
        magic,
        version,
        header_size,
        width,
        height,
        line_height,
        ascent,
        glyph_count,
        record_size,
        pixels_offset,
        pixels_size,
        pixel_format,
        token_width,
        prefix0,
        prefix1,
        token_alphabet,
        reserved_byte,
        reserved,
    ) = HEADER.unpack_from(data)
    if (
        magic != MAGIC
        or version != VERSION
        or header_size != HEADER_SIZE
        or record_size != RECORD_SIZE
        or pixel_format != PIXEL_FORMAT_A8
        or token_width != TOKEN_WIDTH
        or bytes((prefix0, prefix1)) != TOKEN_PREFIX
        or token_alphabet != TOKEN_ALPHABET_ID
        or reserved_byte != 0
        or reserved != 0
    ):
        raise ValueError(f"BFA 文件头不兼容：{path}")
    records_end = HEADER_SIZE + glyph_count * RECORD_SIZE
    if pixels_offset < records_end or pixels_size != width * height:
        raise ValueError(f"BFA 偏移或像素长度无效：{path}")
    if pixels_offset + pixels_size != len(data):
        raise ValueError(
            f"BFA 尾部长度无效：期望 {pixels_offset + pixels_size}，"
            f"实际 {len(data)}"
        )
    records: list[GlyphRecord] = []
    for index in range(glyph_count):
        values = RECORD.unpack_from(data, HEADER_SIZE + index * RECORD_SIZE)
        records.append(GlyphRecord(*values))
    atlas = BfaAtlas(
        width=width,
        height=height,
        line_height=line_height,
        ascent=ascent,
        records=tuple(records),
        pixels=data[pixels_offset : pixels_offset + pixels_size],
    )
    validate_atlas(atlas)
    return atlas


def encode_bfa(atlas: BfaAtlas) -> bytes:
    validate_atlas(atlas)
    records = bytearray()
    for record in atlas.records:
        records.extend(
            RECORD.pack(
                record.codepoint,
                record.carrier_token,
                record.x,
                record.y,
                record.width,
                record.height,
                record.bearing_x,
                record.bearing_y,
                record.advance_x,
                record.flags,
                record.reserved0,
                record.reserved1,
            )
        )
    pixels_offset = HEADER_SIZE + len(records)
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        atlas.width,
        atlas.height,
        atlas.line_height,
        atlas.ascent,
        len(atlas.records),
        RECORD_SIZE,
        pixels_offset,
        len(atlas.pixels),
        PIXEL_FORMAT_A8,
        TOKEN_WIDTH,
        TOKEN_PREFIX[0],
        TOKEN_PREFIX[1],
        TOKEN_ALPHABET_ID,
        0,
        0,
    )
    return header + records + atlas.pixels


def write_bfa(path: Path, atlas: BfaAtlas) -> bytes:
    data = encode_bfa(atlas)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return data


def atlas_report(atlas: BfaAtlas, bfa_bytes: bytes | None = None) -> dict[str, object]:
    nonzero = sum(value != 0 for value in atlas.pixels)
    full = sum(value == 255 for value in atlas.pixels)
    codepoints = [record.codepoint for record in atlas.records]
    return {
        "format": "BSCNATL1",
        "version": VERSION,
        "atlas_width": atlas.width,
        "atlas_height": atlas.height,
        "line_height": atlas.line_height,
        "ascent": atlas.ascent,
        "glyph_count": len(atlas.records),
        "first_codepoint": f"U+{min(codepoints):04X}",
        "last_codepoint": f"U+{max(codepoints):04X}",
        "nonzero_pixels": nonzero,
        "full_pixels": full,
        "coverage_percent": round(nonzero * 100.0 / len(atlas.pixels), 6),
        "bfa_sha256": sha256_bytes(bfa_bytes) if bfa_bytes is not None else None,
        "pixels_sha256": sha256_bytes(atlas.pixels),
    }


def save_png(path: Path, atlas: BfaAtlas) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image = Image.frombytes("L", (atlas.width, atlas.height), atlas.pixels)
    image.save(path, optimize=True)


def write_metrics_tsv(path: Path, atlas: BfaAtlas) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle, dialect="excel-tab", lineterminator="\n")
        writer.writerow(
            (
                "codepoint",
                "character",
                "carrier_token",
                "x",
                "y",
                "width",
                "height",
                "bearing_x",
                "bearing_y",
                "advance_x",
                "flags",
            )
        )
        for record in atlas.records:
            writer.writerow(
                (
                    f"U+{record.codepoint:04X}",
                    visible_character(record.codepoint),
                    (
                        "NONE"
                        if record.carrier_token == NO_TOKEN
                        else str(record.carrier_token)
                    ),
                    record.x,
                    record.y,
                    record.width,
                    record.height,
                    record.bearing_x,
                    record.bearing_y,
                    record.advance_x,
                    record.flags,
                )
            )


def parse_int(text: str, name: str) -> int:
    try:
        return int(text.strip(), 0)
    except ValueError as error:
        raise ValueError(f"{name} 不是整数：{text!r}") from error


def read_metrics_tsv(path: Path, original: BfaAtlas) -> tuple[GlyphRecord, ...]:
    by_codepoint = {record.codepoint: record for record in original.records}
    result: dict[int, GlyphRecord] = {}
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle, dialect="excel-tab")
        required = {
            "codepoint",
            "carrier_token",
            "x",
            "y",
            "width",
            "height",
            "bearing_x",
            "bearing_y",
            "advance_x",
            "flags",
        }
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(
                f"TSV 缺少列：{sorted(required - set(reader.fieldnames or []))}"
            )
        for line_number, row in enumerate(reader, 2):
            codepoint_text = (row["codepoint"] or "").strip().upper()
            if codepoint_text.startswith("U+"):
                codepoint = int(codepoint_text[2:], 16)
            else:
                codepoint = parse_int(codepoint_text, "codepoint")
            original_record = by_codepoint.get(codepoint)
            if original_record is None:
                raise ValueError(
                    f"{path}:{line_number} 包含原 BFA 中不存在的 "
                    f"U+{codepoint:04X}"
                )
            token_text = (row["carrier_token"] or "").strip().upper()
            carrier_token = (
                NO_TOKEN
                if token_text in {"", "NONE", "NO_TOKEN"}
                else parse_int(token_text, "carrier_token")
            )
            record = replace(
                original_record,
                carrier_token=carrier_token,
                x=parse_int(row["x"] or "", "x"),
                y=parse_int(row["y"] or "", "y"),
                width=parse_int(row["width"] or "", "width"),
                height=parse_int(row["height"] or "", "height"),
                bearing_x=parse_int(row["bearing_x"] or "", "bearing_x"),
                bearing_y=parse_int(row["bearing_y"] or "", "bearing_y"),
                advance_x=parse_int(row["advance_x"] or "", "advance_x"),
                flags=parse_int(row["flags"] or "", "flags"),
            )
            if codepoint in result:
                raise ValueError(
                    f"{path}:{line_number} 重复 U+{codepoint:04X}"
                )
            result[codepoint] = record
    missing = sorted(set(by_codepoint) - set(result))
    if missing:
        preview = ", ".join(f"U+{value:04X}" for value in missing[:20])
        raise ValueError(f"TSV 缺少 {len(missing)} 个字形：{preview}")
    ordered = tuple(result[record.codepoint] for record in original.records)
    candidate = replace(original, records=ordered)
    validate_atlas(candidate)
    return ordered


def png_to_coverage(path: Path, width: int, height: int, channel: str) -> bytes:
    with Image.open(path) as source:
        source.load()
        if source.size != (width, height):
            raise ValueError(
                f"PNG 尺寸必须是 {width}x{height}，实际为 "
                f"{source.width}x{source.height}"
            )
        if channel == "luminance":
            image = source.convert("L")
        elif channel == "red":
            image = source.convert("RGB").getchannel("R")
        elif channel == "alpha":
            if "A" not in source.getbands():
                raise ValueError("选择 alpha，但 PNG 没有 Alpha 通道")
            image = source.convert("RGBA").getchannel("A")
        elif channel == "auto":
            if "A" in source.getbands():
                rgba = source.convert("RGBA")
                alpha = rgba.getchannel("A")
                extrema = alpha.getextrema()
                image = alpha if extrema != (255, 255) else rgba.convert("L")
            else:
                image = source.convert("L")
        else:  # pragma: no cover - argparse 已限制
            raise ValueError(f"未知 PNG 通道：{channel}")
        return image.tobytes()


def iter_bli_replacements(path: Path) -> Iterable[str]:
    data = path.read_bytes()
    if len(data) < BLI_HEADER.size:
        raise ValueError(f"BLI 过短：{path}")
    magic, version, header_size, count, payload_size, changed, reserved = (
        BLI_HEADER.unpack_from(data)
    )
    is_v2 = magic == b"BSCNLI2\0" and version == 2
    is_v3 = magic == b"BSCNLI3\0" and version == 3
    if (
        not (is_v2 or is_v3)
        or header_size != BLI_HEADER.size
        or reserved != 0
        or payload_size != len(data) - header_size
        or changed > count
    ):
        raise ValueError(f"BLI 文件头无效：{path}")
    record_prefix = (
        BLI3_RECORD_PREFIX if is_v3 else BLI2_RECORD_PREFIX
    )
    position = header_size
    for index in range(count):
        if position + record_prefix.size > len(data):
            raise ValueError(f"BLI 在记录 {index} 截断：{path}")
        fields = record_prefix.unpack_from(data, position)
        _key_hash, source_units, replacement_units = fields[:3]
        position += record_prefix.size
        source_size = source_units * 2
        replacement_size = replacement_units * 2
        if position + source_size + replacement_size > len(data):
            raise ValueError(f"BLI 在记录 {index} 字符串处截断：{path}")
        position += source_size
        replacement = data[
            position : position + replacement_size
        ].decode("utf-16le", errors="strict")
        position += replacement_size
        yield replacement
    if position != len(data):
        raise ValueError(f"BLI 尾部存在 {len(data) - position} 个额外字节：{path}")


def codepoints_from_text(text: str) -> set[int]:
    return {
        ord(character)
        for character in text
        if unicode_scalar(ord(character))
        and ord(character) not in {0xFEFF}
        and (ord(character) >= 0x20 or character in "\t\n\r")
    }


def collect_codepoints(args: argparse.Namespace) -> set[int]:
    result: set[int] = set()
    for path in args.index:
        for replacement in iter_bli_replacements(path.resolve()):
            result.update(codepoints_from_text(replacement))
    for path in args.text_file:
        result.update(
            codepoints_from_text(path.read_text(encoding="utf-8-sig"))
        )
    for root in args.text_dir:
        for path in sorted(root.rglob("*")):
            if path.is_file() and (
                path.suffix.lower() in TEXT_EXTENSIONS
                or path.name.lower().endswith(".loc.json")
            ):
                result.update(
                    codepoints_from_text(
                        path.read_text(encoding="utf-8-sig")
                    )
                )
    result.update(codepoints_from_text(args.characters))
    if args.include_ascii:
        result.update(range(0x20, 0x7F))
    if args.include_cp1252:
        result.update(cp1252_extras())
    return {
        codepoint
        for codepoint in result
        if codepoint >= 0x20 and codepoint != 0x7F
    }



FONT_EXTENSIONS = {".ttf", ".otf", ".ttc", ".otc"}
JAPANESE_KEY_MARKERS = {
    "ja", "jp", "jpn", "ja-jp", "ja_jp", "ja-jpan", "japanese",
    "日本語", "日文", "日本", "日语", "日語",
}
CHINESE_KEY_MARKERS = {
    "zh", "zh-cn", "zh_cn", "zh-hans", "zh_hans", "cn", "chs", "zhs", "chinese",
    "中文", "简体", "簡體", "简中", "簡中", "汉化", "漢化",
}


def _normalized_key_tokens(value: str) -> set[str]:
    lowered = value.strip().lower()
    pieces = {lowered}
    for token in re.split(r"[^0-9a-zA-Z\u3040-\u30ff\u3400-\u9fff]+", lowered):
        if token:
            pieces.add(token)
    return pieces


def key_matches_language(value: str, language: str) -> bool:
    tokens = _normalized_key_tokens(value)
    markers = JAPANESE_KEY_MARKERS if language == "ja" else CHINESE_KEY_MARKERS
    if tokens & markers:
        return True
    # 常见复合字段名，例如 text_ja / name-jp / japaneseText。
    lowered = value.strip().lower()
    if language == "ja":
        return bool(re.search(r"(^|[_\-.])(ja|jp|jpn)([_\-.]|$)|japanese|日本語|日文|日语|日語", lowered))
    return bool(re.search(r"(^|[_\-.])(zh|cn|chs|zhs)([_\-.]|$)|chinese|中文|简体|簡體|简中|簡中", lowered))


def is_han_character(character: str) -> bool:
    cp = ord(character)
    return (
        0x3400 <= cp <= 0x4DBF
        or 0x4E00 <= cp <= 0x9FFF
        or 0xF900 <= cp <= 0xFAFF
        or 0x20000 <= cp <= 0x2EBEF
        or 0x30000 <= cp <= 0x323AF
    )


def is_kana_character(character: str) -> bool:
    cp = ord(character)
    return (
        0x3040 <= cp <= 0x309F
        or 0x30A0 <= cp <= 0x30FF
        or 0x31F0 <= cp <= 0x31FF
        or 0xFF65 <= cp <= 0xFF9F
    )


def is_printable_font_character(character: str) -> bool:
    cp = ord(character)
    if cp == 0xFEFF or cp == 0x7F or cp < 0x20:
        return False
    category = unicodedata.category(character)
    return not category.startswith("C") and not character.isspace()


@dataclass
class JsonScanResult:
    language: str
    files: list[Path]
    strings_total: int
    strings_used: int
    strings_excluded: int
    tagged_japanese_strings: int
    tagged_chinese_strings: int
    all_characters: Counter[str]
    han_characters: Counter[str]
    kana_characters: Counter[str]
    source_files: Counter[str]
    excluded_paths: Counter[str]
    parse_errors: list[str]

    @property
    def codepoints(self) -> set[int]:
        return {ord(ch) for ch in self.all_characters}


def scan_json_directory(root: Path, language: str) -> JsonScanResult:
    root = root.resolve()
    if not root.is_dir():
        raise ValueError(f"JSON 扫描目录不存在：{root}")
    if language not in {"zh", "ja"}:
        raise ValueError(f"未知语言模式：{language}")

    files = sorted(path for path in root.rglob("*.json") if path.is_file())
    if not files:
        raise ValueError(f"目录及子目录中没有找到 JSON：{root}")

    all_characters: Counter[str] = Counter()
    han_characters: Counter[str] = Counter()
    kana_characters: Counter[str] = Counter()
    source_files: Counter[str] = Counter()
    excluded_paths: Counter[str] = Counter()
    parse_errors: list[str] = []
    strings_total = 0
    strings_used = 0
    strings_excluded = 0
    tagged_japanese_strings = 0
    tagged_chinese_strings = 0

    def visit(value: object, *, path_keys: tuple[str, ...], file_rel: str) -> None:
        nonlocal strings_total, strings_used, strings_excluded
        nonlocal tagged_japanese_strings, tagged_chinese_strings

        if isinstance(value, dict):
            for key, child in value.items():
                visit(child, path_keys=(*path_keys, str(key)), file_rel=file_rel)
            return
        if isinstance(value, list):
            for index, child in enumerate(value):
                visit(child, path_keys=(*path_keys, f"[{index}]"), file_rel=file_rel)
            return
        if not isinstance(value, str):
            return

        strings_total += 1
        has_ja = any(key_matches_language(key, "ja") for key in path_keys)
        has_zh = any(key_matches_language(key, "zh") for key in path_keys)
        if has_ja:
            tagged_japanese_strings += 1
        if has_zh:
            tagged_chinese_strings += 1

        # 中文模式：扫描全部非日文字段；日文字段严格排除。
        # 日文模式：只扫描明确位于日文字段内的字符串，防止把中文/英文 source 混入。
        use = (not has_ja) if language == "zh" else has_ja
        if not use:
            strings_excluded += 1
            excluded_paths[".".join(path_keys) or "<root>"] += 1
            return

        strings_used += 1
        source_files[file_rel] += 1
        for character in value:
            if not is_printable_font_character(character):
                continue
            all_characters[character] += 1
            if is_han_character(character):
                han_characters[character] += 1
            if is_kana_character(character):
                kana_characters[character] += 1

    for path in files:
        rel = str(path.relative_to(root))
        try:
            data = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            parse_errors.append(f"{rel}: {error}")
            continue
        visit(data, path_keys=(), file_rel=rel)

    result = JsonScanResult(
        language=language,
        files=files,
        strings_total=strings_total,
        strings_used=strings_used,
        strings_excluded=strings_excluded,
        tagged_japanese_strings=tagged_japanese_strings,
        tagged_chinese_strings=tagged_chinese_strings,
        all_characters=all_characters,
        han_characters=han_characters,
        kana_characters=kana_characters,
        source_files=source_files,
        excluded_paths=excluded_paths,
        parse_errors=parse_errors,
    )
    if parse_errors:
        preview = "\n".join(parse_errors[:20])
        suffix = "\n..." if len(parse_errors) > 20 else ""
        raise ValueError(
            f"发现 {len(parse_errors)} 个 JSON 解析错误；为避免漏字，已停止：\n{preview}{suffix}"
        )
    if language == "ja" and result.strings_used == 0:
        raise ValueError(
            "日文模式没有找到任何明确日文字段。请确认 JSON 字段名包含 "
            "ja/jp/ja-JP/japanese/日本語/日文 等标记。"
        )
    return result


def _sort_characters(counter: Counter[str]) -> list[tuple[str, int]]:
    return sorted(counter.items(), key=lambda item: (ord(item[0]), item[0]))


def write_json_scan_outputs(output: Path, result: JsonScanResult) -> dict[str, object]:
    output.mkdir(parents=True, exist_ok=True)
    language_name = "中文" if result.language == "zh" else "日文"

    all_chars = "".join(ch for ch, _ in _sort_characters(result.all_characters))
    han_chars = "".join(ch for ch, _ in _sort_characters(result.han_characters))
    kana_chars = "".join(ch for ch, _ in _sort_characters(result.kana_characters))

    (output / f"{language_name}全部字符.txt").write_text(all_chars + "\n", encoding="utf-8-sig")
    (output / f"{language_name}汉字.txt").write_text(han_chars + "\n", encoding="utf-8-sig")
    if result.language == "ja":
        (output / "日文假名.txt").write_text(kana_chars + "\n", encoding="utf-8-sig")

    with (output / f"{language_name}字符频次.tsv").open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle, dialect="excel-tab", lineterminator="\n")
        writer.writerow(("codepoint", "character", "count", "unicode_name", "category"))
        for character, count in sorted(result.all_characters.items(), key=lambda item: (-item[1], ord(item[0]))):
            writer.writerow((
                f"U+{ord(character):04X}",
                visible_character(ord(character)),
                count,
                unicodedata.name(character, "<无名称>"),
                unicodedata.category(character),
            ))

    with (output / "JSON来源统计.tsv").open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle, dialect="excel-tab", lineterminator="\n")
        writer.writerow(("json_file", "used_string_count"))
        for file_name, count in sorted(result.source_files.items()):
            writer.writerow((file_name, count))

    with (output / "被排除字段统计.tsv").open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle, dialect="excel-tab", lineterminator="\n")
        writer.writerow(("field_path", "excluded_string_count"))
        for field_path, count in result.excluded_paths.most_common():
            writer.writerow((field_path, count))

    report = {
        "language": result.language,
        "language_name": language_name,
        "json_file_count": len(result.files),
        "string_count_total": result.strings_total,
        "string_count_used": result.strings_used,
        "string_count_excluded": result.strings_excluded,
        "tagged_japanese_strings": result.tagged_japanese_strings,
        "tagged_chinese_strings": result.tagged_chinese_strings,
        "unique_all_characters": len(result.all_characters),
        "unique_han_characters": len(result.han_characters),
        "unique_kana_characters": len(result.kana_characters),
        "policy": (
            "exclude_japanese_fields" if result.language == "zh" else "japanese_fields_only"
        ),
        "parse_errors": result.parse_errors,
    }
    write_manifest(output / "扫描报告.json", report)
    return report


def discover_font_files(font_source: Path) -> list[Path]:
    root = font_source.resolve()
    if not root.is_dir():
        raise ValueError(f"font_source 目录不存在：{root}")
    return sorted(
        path for path in root.rglob("*")
        if path.is_file() and path.suffix.lower() in FONT_EXTENSIONS
    )


def select_font_file(font_source: Path, language: str, font_name: str | None) -> Path:
    fonts = discover_font_files(font_source)
    if not fonts:
        raise ValueError(
            f"font_source 中没有找到字体；支持：{', '.join(sorted(FONT_EXTENSIONS))}"
        )
    if font_name:
        needle = font_name.strip().lower()
        matches = [
            path for path in fonts
            if path.name.lower() == needle
            or str(path.relative_to(font_source.resolve())).lower() == needle
        ]
        if len(matches) != 1:
            names = "\n".join(f"  - {path.relative_to(font_source.resolve())}" for path in fonts)
            raise ValueError(f"--font-name 未唯一匹配：{font_name}\n可用字体：\n{names}")
        return matches[0]
    if len(fonts) == 1:
        return fonts[0]

    def score(path: Path) -> int:
        name = path.stem.lower().replace("-", "_")
        zh_positive = ("notosanssc", "sourcehansanssc", "cjk_sc", "_sc", "hans", "chinese", "zh", "cn")
        ja_positive = ("notosansjp", "sourcehansansjp", "cjk_jp", "_jp", "japanese", "ja")
        positive = zh_positive if language == "zh" else ja_positive
        negative = ja_positive if language == "zh" else zh_positive
        value = sum(10 for token in positive if token in name)
        value -= sum(6 for token in negative if token in name)
        return value

    scored = sorted(((score(path), path) for path in fonts), key=lambda item: (-item[0], str(item[1])))
    best_score = scored[0][0]
    best = [path for value, path in scored if value == best_score]
    if best_score <= 0 or len(best) != 1:
        listing = "\n".join(
            f"  - score={value:>3} {path.relative_to(font_source.resolve())}"
            for value, path in scored
        )
        raise ValueError(
            "font_source 中有多个字体，无法安全自动决定使用哪一个。"
            "请用 --font-name 指定文件名或相对路径。\n" + listing
        )
    return best[0]



def _font_face_display_names(font: object) -> list[str]:
    result: list[str] = []
    try:
        name_table = font["name"]
    except Exception:
        return result
    for record in name_table.names:
        if record.nameID not in {1, 2, 4, 6, 16, 17}:
            continue
        try:
            text = record.toUnicode().strip()
        except Exception:
            continue
        if text and text not in result:
            result.append(text)
    return result


def _language_face_score(names: Sequence[str], language: str) -> int:
    joined = " | ".join(names).lower()
    if language == "zh":
        positive = (
            "cjk sc", "cjksc", "simplified chinese", "hans", "zh-cn", "zh_cn",
            "简体", "簡體", "简中", "簡中",
        )
        negative = ("cjk jp", "cjkjp", "japanese", "日本語")
    else:
        positive = ("cjk jp", "cjkjp", "japanese", "日本語", "japan")
        negative = ("cjk sc", "cjksc", "simplified chinese", "hans", "简体", "簡體")
    score = sum(20 for token in positive if token in joined)
    score -= sum(12 for token in negative if token in joined)
    if " mono " in f" {joined} " or "sansmono" in joined or "sans mono" in joined:
        score -= 5
    else:
        score += 5
    return score


def select_font_index(font_path: Path, language: str, requested_index: int | None) -> int:
    if requested_index is not None:
        if requested_index < 0:
            raise ValueError("--font-index 不能小于 0")
        return requested_index
    if font_path.suffix.lower() not in {".ttc", ".otc"}:
        return 0
    if TTCollection is None:
        raise RuntimeError("自动选择 TTC/OTC 字体面需要 fontTools")
    collection = TTCollection(str(font_path), lazy=True)
    try:
        scored: list[tuple[int, int, list[str]]] = []
        for index, font in enumerate(collection.fonts):
            names = _font_face_display_names(font)
            scored.append((_language_face_score(names, language), index, names))
        if not scored:
            raise ValueError(f"字体集合没有可用字体面：{font_path}")
        scored.sort(key=lambda item: (-item[0], item[1]))
        best_score = scored[0][0]
        best = [item for item in scored if item[0] == best_score]
        if best_score <= 0 or len(best) != 1:
            listing = "\n".join(
                f"  - index={index} score={score:>3} {' / '.join(names[:3]) or '<未知名称>'}"
                for score, index, names in scored
            )
            raise ValueError(
                "TTC/OTC 中有多个字体面，无法安全自动选择中/日文字体面。"
                "请显式使用 --font-index。\n" + listing
            )
        return best[0][1]
    finally:
        for font in collection.fonts:
            try:
                font.close()
            except Exception:
                pass


def command_scan_json(args: argparse.Namespace) -> int:
    result = scan_json_directory(args.json_dir, args.language)
    report = write_json_scan_outputs(args.output.resolve(), result)
    print(f"[OK] JSON：{report['json_file_count']} 个")
    print(f"[OK] 使用字符串：{report['string_count_used']}，排除：{report['string_count_excluded']}")
    print(f"[OK] 唯一字符：{report['unique_all_characters']}，汉字：{report['unique_han_characters']}，假名：{report['unique_kana_characters']}")
    print(f"[OK] 输出：{args.output.resolve()}")
    return 0


def command_rebuild(args: argparse.Namespace) -> int:
    """从当前 JSON 全量字符集重建，不读取旧 BFA，也不做增量追加。"""
    output = args.output.resolve()
    scan_output = output / "字符统计"
    compact_output = output / "中间文件_紧凑字库"
    grid_output = output / "中间文件_标准网格"

    result = scan_json_directory(args.json_dir, args.language)
    scan_report = write_json_scan_outputs(scan_output, result)
    font_path = select_font_file(args.font_source, args.language, args.font_name)
    font_index = select_font_index(font_path, args.language, args.font_index)

    # 固定规则：当前字体实际支持的全部 Unicode 标点必须永久保留。
    # 这部分不依赖翻译 JSON，因此删改文本也不会让标点从下次字库消失。
    punctuation_codepoints = supported_punctuation_codepoints(font_path, font_index)

    codepoints = set(result.codepoints)
    codepoints.update(punctuation_codepoints)
    if args.include_ascii:
        codepoints.update(range(0x20, 0x7F))
    if args.include_cp1252:
        codepoints.update(cp1252_extras())
    for extra_file in args.extra_text_file:
        codepoints.update(codepoints_from_text(extra_file.read_text(encoding="utf-8-sig")))
    codepoints.update(codepoints_from_text(args.characters))
    if not codepoints:
        raise ValueError("扫描后没有可用于重建的字符")
    resolved_columns = args.columns or (args.grid_atlas_width // args.cell_width)
    maximum_rows = args.grid_atlas_height // args.cell_height
    grid_capacity = resolved_columns * maximum_rows
    if len(codepoints) > grid_capacity:
        raise ValueError(
            f"当前全量字符集需要 {len(codepoints)} 个字形，但 Grid074 当前参数容量仅 "
            f"{grid_capacity}（{resolved_columns}列×{maximum_rows}行）。"
            f"其中当前字体固定保留标点={len(punctuation_codepoints)}。"
            "工具不会静默丢字，也不会为了腾容量删除标点；请评估翻译字符集或扩展图集。"
        )

    rasters, natural_line, natural_ascent, missing = rasterize(
        font_path,
        sorted(codepoints),
        font_index=font_index,
        font_size=args.font_size,
        oversample=args.oversample,
        stroke_width=args.stroke_width,
        raster_scale_x=args.raster_scale_x,
        raster_scale_y=args.raster_scale_y,
        bearing_x_offset=args.bearing_x_offset,
        bearing_y_offset=args.bearing_y_offset,
        advance_scale=args.advance_scale,
        advance_offset=args.advance_offset,
        gamma=args.raster_gamma,
        missing_policy=args.missing,
    )
    rasters, adjusted_punctuation = fit_punctuation_rasters_to_grid(
        rasters, punctuation_codepoints,
        cell_width=args.cell_width,
        cell_height=args.cell_height,
        origin_x=args.origin_x,
        baseline_y=args.baseline_y,
    )
    line_height = resolve_metric(
        natural_line, args.line_height_scale, args.line_height_offset, args.line_height
    )
    ascent = resolve_metric(
        natural_ascent, args.ascent_scale, args.ascent_offset, args.ascent
    )
    records, pixels = pack_rasters(
        rasters, args.atlas_width, args.atlas_height, args.padding, args.margin
    )
    compact = BfaAtlas(
        width=args.atlas_width,
        height=args.atlas_height,
        line_height=line_height,
        ascent=ascent,
        records=records,
        pixels=pixels,
    )
    compact_output.mkdir(parents=True, exist_ok=True)
    compact_bfa = compact_output / "cn_font.bfa"
    compact_data = write_bfa(compact_bfa, compact)
    save_png(compact_output / "cn_font.png", compact)
    write_metrics_tsv(compact_output / "cn_font.glyphs.tsv", compact)

    grid, placements = repack_atlas_grid(
        compact,
        cell_width=args.cell_width,
        cell_height=args.cell_height,
        origin_x=args.origin_x,
        baseline_y=args.baseline_y,
        atlas_width=args.grid_atlas_width,
        atlas_height=args.grid_atlas_height,
        columns=args.columns,
        overflow=args.overflow,
    )
    grid_output.mkdir(parents=True, exist_ok=True)
    grid_data = write_bfa(grid_output / "cn_font_grid.bfa", grid)
    save_png(grid_output / "cn_font_grid.png", grid)
    save_grid_guide(
        grid_output / "cn_font_grid.guide.png",
        grid,
        placements,
        origin_x=args.origin_x,
        baseline_y=args.baseline_y,
    )
    write_metrics_tsv(grid_output / "cn_font_grid.glyphs.tsv", grid)
    write_grid_map(
        grid_output / "cn_font_grid.map.tsv",
        placements,
        origin_x=args.origin_x,
        baseline_y=args.baseline_y,
        punctuation_only=False,
    )
    write_grid_map(
        grid_output / "cn_font_grid.punctuation.tsv",
        placements,
        origin_x=args.origin_x,
        baseline_y=args.baseline_y,
        punctuation_only=True,
    )

    punctuation_text = "".join(chr(cp) for cp in sorted(punctuation_codepoints))
    (output / "固定保留标点.txt").write_text(
        punctuation_text + "\n", encoding="utf-8-sig"
    )

    final_bfa = output / "fonts.bfa"
    final_bfa.write_bytes(grid_data)
    language_name = "中文" if args.language == "zh" else "日文"
    rebuild_report = {
        "mode": "full_rebuild",
        "language": args.language,
        "language_name": language_name,
        "json_root": str(args.json_dir.resolve()),
        "font_source_root": str(args.font_source.resolve()),
        "selected_font": str(font_path.relative_to(args.font_source.resolve())),
        "selected_font_sha256": sha256_bytes(font_path.read_bytes()),
        "font_index": font_index,
        "json_scan": scan_report,
        "requested_codepoints": len(codepoints),
        "fixed_punctuation_codepoints": len(punctuation_codepoints),
        "fixed_punctuation_policy": "保留当前所选字体 cmap 中全部 Unicode P* 标点 + U+3000",
        "grid_adjusted_punctuation": [f"U+{cp:04X}" for cp in adjusted_punctuation],
        "glyph_count": len(grid.records),
        "missing_codepoints": [f"U+{cp:04X}" for cp in missing],
        "compact_bfa_sha256": sha256_bytes(compact_data),
        "final_bfa_sha256": sha256_bytes(grid_data),
        "font_size": args.font_size,
        "line_height": grid.line_height,
        "ascent": grid.ascent,
        "grid": {
            "cell_width": args.cell_width,
            "cell_height": args.cell_height,
            "origin_x": args.origin_x,
            "baseline_y": args.baseline_y,
            "atlas_width": args.grid_atlas_width,
            "atlas_height": args.grid_atlas_height,
            "columns": args.columns or args.grid_atlas_width // args.cell_width,
            "overflow": args.overflow,
        },
        "final_output": "fonts.bfa",
        "important": "每次均从当前 JSON 全量重建；不读取旧 fonts.bfa，不做增量追加。",
    }
    write_manifest(output / "重建报告.json", rebuild_report)

    print(f"[OK] 模式：{language_name}全量重建")
    print(f"[OK] 字体：{font_path}（font-index={font_index}）")
    print(f"[OK] JSON：{scan_report['json_file_count']} 个，唯一字符={scan_report['unique_all_characters']}")
    print(f"[OK] 固定保留标点：{len(punctuation_codepoints)} 个")
    if adjusted_punctuation:
        print(f"[OK] 为适配 36x44 网格缩放超宽/超高标点：{len(adjusted_punctuation)} 个")
    print(f"[OK] 最终字形：{len(grid.records)}")
    print(f"[OK] 最终 BFA：{final_bfa}")
    print(f"[OK] SHA-256：{sha256_bytes(grid_data)}")
    return 0

def font_cmap(font_path: Path, font_index: int) -> set[int]:
    if TTFont is None:
        raise RuntimeError(
            "缺少 fontTools；请运行 python -m pip install -r tools/requirements.txt"
        )
    font = TTFont(str(font_path), fontNumber=font_index, lazy=True)
    try:
        result: set[int] = set()
        for table in font["cmap"].tables:
            if table.isUnicode():
                result.update(table.cmap)
        return result
    finally:
        font.close()


def supported_punctuation_codepoints(font_path: Path, font_index: int) -> set[int]:
    """返回当前字体实际支持的全部 Unicode 标点。

    每次全量重建都强制保留这些字符，不依赖本轮 JSON 是否恰好出现。
    除 Unicode P* 类别外额外保留 U+3000 全角空格（CJK 排版基础空白）。
    """
    cmap = font_cmap(font_path, font_index)
    result = {
        codepoint
        for codepoint in cmap
        if unicode_scalar(codepoint)
        and unicodedata.category(chr(codepoint)).startswith("P")
    }
    if 0x3000 in cmap:
        result.add(0x3000)
    return result


def transform_coverage(image: Image.Image, gamma: float) -> Image.Image:
    if gamma == 1.0:
        return image
    table = [
        max(0, min(255, int(round((value / 255.0) ** gamma * 255.0))))
        for value in range(256)
    ]
    return image.point(table)


def rasterize(
    font_path: Path,
    codepoints: Sequence[int],
    *,
    font_index: int,
    font_size: int,
    oversample: int,
    stroke_width: float,
    raster_scale_x: float,
    raster_scale_y: float,
    bearing_x_offset: float,
    bearing_y_offset: float,
    advance_scale: float,
    advance_offset: float,
    gamma: float,
    missing_policy: str,
) -> tuple[list[RasterGlyph], int, int, list[int]]:
    high_size = font_size * oversample
    font = ImageFont.truetype(
        str(font_path), high_size, index=font_index
    )
    cmap = font_cmap(font_path, font_index)
    stroke_high = max(0, int(round(stroke_width * oversample)))
    resampling = getattr(Image, "Resampling", Image).LANCZOS
    output: list[RasterGlyph] = []
    missing: list[int] = []
    required_missing: list[int] = []
    optional_cp1252 = cp1252_extras()

    for codepoint in sorted(set(codepoints)):
        if codepoint not in cmap and codepoint != 0x20:
            missing.append(codepoint)
            if codepoint not in optional_cp1252:
                required_missing.append(codepoint)
            if missing_policy == "skip":
                continue
            continue
        character = chr(codepoint)
        left, top, right, bottom = font.getbbox(
            character, anchor="ls", stroke_width=stroke_high
        )
        high_width = max(0, int(math.ceil(right - left)))
        high_height = max(0, int(math.ceil(bottom - top)))
        target_width = max(
            0,
            int(math.ceil(high_width / oversample * raster_scale_x)),
        )
        target_height = max(
            0,
            int(math.ceil(high_height / oversample * raster_scale_y)),
        )
        image: Image.Image | None = None
        if high_width and high_height and target_width and target_height:
            high_image = Image.new("L", (high_width, high_height), 0)
            draw = ImageDraw.Draw(high_image)
            draw.text(
                (-left, -top),
                character,
                font=font,
                fill=255,
                stroke_width=stroke_high,
                stroke_fill=255,
                anchor="ls",
            )
            image = high_image.resize(
                (target_width, target_height), resample=resampling
            )
            image = transform_coverage(image, gamma)
        advance = int(
            round(
                font.getlength(character) / oversample
                * advance_scale
                + advance_offset
            )
        )
        record = GlyphRecord(
            codepoint=codepoint,
            carrier_token=(
                token_for_codepoint(codepoint)
                if codepoint >= 0x80
                else NO_TOKEN
            ),
            x=0,
            y=0,
            width=target_width,
            height=target_height,
            bearing_x=int(
                math.floor(left / oversample + bearing_x_offset)
            ),
            bearing_y=int(
                math.ceil(-top / oversample + bearing_y_offset)
            ),
            advance_x=advance,
        )
        output.append(RasterGlyph(record=record, image=image))

    if required_missing and missing_policy == "error":
        preview = ", ".join(
            f"U+{codepoint:04X} {chr(codepoint)!r}"
            for codepoint in required_missing[:40]
        )
        suffix = " ..." if len(required_missing) > 40 else ""
        raise ValueError(
            f"字体缺少 {len(required_missing)} 个必需字形：{preview}{suffix}"
        )
    ascent_high, descent_high = font.getmetrics()
    natural_ascent = max(1, int(round(ascent_high / oversample)))
    natural_line = max(
        1,
        int(
            round(
                (ascent_high + descent_high) / oversample
                + max(1, font_size // 12)
            )
        ),
    )
    return output, natural_line, natural_ascent, missing


def fit_punctuation_rasters_to_grid(
    rasters: Sequence[RasterGlyph],
    punctuation_codepoints: set[int],
    *,
    cell_width: int,
    cell_height: int,
    origin_x: int,
    baseline_y: int,
) -> tuple[list[RasterGlyph], list[int]]:
    """让极少数超宽/超高标点仍能完整放入固定 Grid074 单元。

    CJK 字体中 TWO-EM/THREE-EM DASH 等标点天然可能宽于 36px。既然标点
    被定义为永久保留项，就不能因为它们让全量重建失败，也不能静默删掉。
    这里只对 Unicode P* 标点做最小必要缩放；普通汉字/假名绝不改尺寸。
    """
    resampling = getattr(Image, "Resampling", Image).LANCZOS
    output: list[RasterGlyph] = []
    adjusted: list[int] = []
    for item in rasters:
        record = item.record
        if record.codepoint not in punctuation_codepoints:
            output.append(item)
            continue

        bearing_x = record.bearing_x
        bearing_y = record.bearing_y
        local_x = origin_x + bearing_x
        local_y = baseline_y - bearing_y
        if local_x < 0:
            bearing_x -= local_x
            local_x = 0
        if local_y < 0:
            bearing_y = baseline_y
            local_y = 0
        if local_x >= cell_width:
            bearing_x = -origin_x
            local_x = 0
        if local_y >= cell_height:
            bearing_y = baseline_y
            local_y = 0

        max_width = max(1, cell_width - local_x)
        max_height = max(1, cell_height - local_y)
        target_width = min(record.width, max_width)
        target_height = min(record.height, max_height)
        image = item.image
        if image is not None and (target_width != record.width or target_height != record.height):
            image = image.resize((target_width, target_height), resample=resampling)

        if (target_width != record.width or target_height != record.height or
                bearing_x != record.bearing_x or bearing_y != record.bearing_y):
            adjusted.append(record.codepoint)
            record = replace(
                record,
                width=target_width,
                height=target_height,
                bearing_x=bearing_x,
                bearing_y=bearing_y,
                advance_x=min(record.advance_x, max(1, cell_width - origin_x)),
            )
        output.append(RasterGlyph(record=record, image=image))
    return output, adjusted


def pack_rasters(
    rasters: Sequence[RasterGlyph],
    width: int,
    height: int,
    padding: int,
    margin: int,
) -> tuple[tuple[GlyphRecord, ...], bytes]:
    image = Image.new("L", (width, height), 0)
    x = margin
    y = margin
    row_height = 0
    packed: dict[int, GlyphRecord] = {}
    ordered = sorted(
        rasters,
        key=lambda item: (
            -item.record.height,
            -item.record.width,
            item.record.codepoint,
        ),
    )
    for item in ordered:
        record = item.record
        if not record.width or not record.height or item.image is None:
            packed[record.codepoint] = record
            continue
        cell_width = record.width + padding * 2
        cell_height = record.height + padding * 2
        if cell_width > width - margin * 2 or cell_height > height - margin * 2:
            raise ValueError(
                f"U+{record.codepoint:04X} 单字超过图集尺寸"
            )
        if x + cell_width > width - margin:
            x = margin
            y += row_height
            row_height = 0
        if y + cell_height > height - margin:
            raise ValueError(
                f"{len(rasters)} 个字形无法装入 {width}x{height}；"
                "请增大 --atlas-width/--atlas-height，减小字号或减少字符"
            )
        glyph_x = x + padding
        glyph_y = y + padding
        image.paste(item.image, (glyph_x, glyph_y))
        packed[record.codepoint] = replace(
            record, x=glyph_x, y=glyph_y
        )
        x += cell_width
        row_height = max(row_height, cell_height)
    records = tuple(
        packed[item.record.codepoint]
        for item in sorted(rasters, key=lambda value: value.record.codepoint)
    )
    return records, image.tobytes()


def repack_atlas_grid(
    atlas: BfaAtlas,
    *,
    cell_width: int,
    cell_height: int,
    origin_x: int,
    baseline_y: int,
    atlas_width: int,
    atlas_height: int,
    columns: int,
    overflow: str,
) -> tuple[BfaAtlas, tuple[GridPlacement, ...]]:
    """把紧凑图集无缩放地搬入按 Unicode 排序的固定网格。

    `origin_x` 是每格中的书写原点，`baseline_y` 是共同基线。原字形的
    bearing 用来恢复它相对原点和基线的位置：

        local_x = origin_x + bearing_x
        local_y = baseline_y - bearing_y

    新记录的 x/y/width/height 覆盖整个单元格，因此以后只移动 PNG 中的
    像素，就能改变标点在同一标准字框中的显示位置。advance_x 和原 bearing
    保持不变，便于 INI 继续选用这些度量源。
    """

    if cell_width <= 0 or cell_height <= 0:
        raise ValueError("网格单元宽高必须大于 0")
    if atlas_width <= 0 or atlas_height <= 0:
        raise ValueError("网格图集宽高必须大于 0")
    if atlas_width > 8192 or atlas_height > 8192:
        raise ValueError("网格图集不能超过 8192x8192")
    maximum_columns = atlas_width // cell_width
    if maximum_columns <= 0:
        raise ValueError("单元宽度大于整张网格图集")
    resolved_columns = maximum_columns if columns == 0 else columns
    if resolved_columns <= 0 or resolved_columns > maximum_columns:
        raise ValueError(
            f"--columns 必须在 1..{maximum_columns}，或写 0 自动填满"
        )
    ordered = sorted(atlas.records, key=lambda record: record.codepoint)
    required_rows = math.ceil(len(ordered) / resolved_columns)
    if required_rows * cell_height > atlas_height:
        raise ValueError(
            f"{len(ordered)} 个字形需要 {required_rows} 行，"
            f"超出 {atlas_width}x{atlas_height} 的网格图集"
        )
    if overflow not in {"error", "clip"}:
        raise ValueError(f"未知网格越界策略：{overflow}")

    source_image = Image.frombytes(
        "L", (atlas.width, atlas.height), atlas.pixels
    )
    output_image = Image.new("L", (atlas_width, atlas_height), 0)
    records: list[GlyphRecord] = []
    placements: list[GridPlacement] = []
    for index, record in enumerate(ordered):
        row, column = divmod(index, resolved_columns)
        cell_x = column * cell_width
        cell_y = row * cell_height
        local_ink_x = origin_x + record.bearing_x
        local_ink_y = baseline_y - record.bearing_y
        ink_right = local_ink_x + record.width
        ink_bottom = local_ink_y + record.height
        fits = (
            local_ink_x >= 0
            and local_ink_y >= 0
            and ink_right <= cell_width
            and ink_bottom <= cell_height
        )
        if not fits and overflow == "error":
            raise ValueError(
                f"U+{record.codepoint:04X} 放入标准格后越界："
                f"ink={local_ink_x},{local_ink_y},"
                f"{record.width},{record.height} / "
                f"cell={cell_width}x{cell_height}；"
                "请增大格子、调整原点/基线，或明确使用 --overflow clip"
            )

        if record.width > 0 and record.height > 0:
            glyph_image = source_image.crop(
                (
                    record.x,
                    record.y,
                    record.x + record.width,
                    record.y + record.height,
                )
            )
            # 先贴到单格临时图，再贴入总图。Pillow 会在 clip 模式下安全
            # 裁掉落在单元格外的部分，不会污染相邻字形。
            cell_image = Image.new("L", (cell_width, cell_height), 0)
            cell_image.paste(glyph_image, (local_ink_x, local_ink_y))
            output_image.paste(cell_image, (cell_x, cell_y))

        grid_record = replace(
            record,
            x=cell_x,
            y=cell_y,
            width=cell_width,
            height=cell_height,
        )
        records.append(grid_record)
        placements.append(
            GridPlacement(
                row=row,
                column=column,
                cell_x=cell_x,
                cell_y=cell_y,
                local_ink_x=local_ink_x,
                local_ink_y=local_ink_y,
                original=record,
                grid=grid_record,
            )
        )

    result = BfaAtlas(
        width=atlas_width,
        height=atlas_height,
        line_height=atlas.line_height,
        ascent=atlas.ascent,
        records=tuple(records),
        pixels=output_image.tobytes(),
    )
    validate_atlas(result)
    return result, tuple(placements)


def write_grid_map(
    path: Path,
    placements: Sequence[GridPlacement],
    *,
    origin_x: int,
    baseline_y: int,
    punctuation_only: bool,
) -> None:
    """写出可搜索的标准格坐标表；另表只保留 Unicode 标点。"""

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(
            handle, dialect="excel-tab", lineterminator="\n"
        )
        writer.writerow(
            (
                "codepoint",
                "character",
                "unicode_category",
                "row",
                "column",
                "cell_x",
                "cell_y",
                "cell_width",
                "cell_height",
                "origin_x",
                "baseline_y",
                "ink_x_in_cell",
                "ink_y_in_cell",
                "original_width",
                "original_height",
                "bearing_x",
                "bearing_y",
                "advance_x",
            )
        )
        for placement in placements:
            record = placement.original
            category = unicodedata.category(chr(record.codepoint))
            if punctuation_only and not category.startswith("P"):
                continue
            writer.writerow(
                (
                    f"U+{record.codepoint:04X}",
                    visible_character(record.codepoint),
                    category,
                    placement.row,
                    placement.column,
                    placement.cell_x,
                    placement.cell_y,
                    placement.grid.width,
                    placement.grid.height,
                    origin_x,
                    baseline_y,
                    placement.local_ink_x,
                    placement.local_ink_y,
                    record.width,
                    record.height,
                    record.bearing_x,
                    record.bearing_y,
                    record.advance_x,
                )
            )


def save_grid_guide(
    path: Path,
    atlas: BfaAtlas,
    placements: Sequence[GridPlacement],
    *,
    origin_x: int,
    baseline_y: int,
) -> None:
    """生成只用于观察的彩色参考图；该图绝不能回导。"""

    image = Image.frombytes(
        "L", (atlas.width, atlas.height), atlas.pixels
    ).convert("RGB")
    draw = ImageDraw.Draw(image)
    seen_rows = {placement.row for placement in placements}
    seen_columns = {placement.column for placement in placements}
    cell_width = placements[0].grid.width
    cell_height = placements[0].grid.height
    for column in sorted(seen_columns):
        x = column * cell_width
        draw.line((x, 0, x, atlas.height - 1), fill=(72, 72, 72))
        origin = x + origin_x
        draw.line(
            (origin, 0, origin, atlas.height - 1),
            fill=(112, 48, 48),
        )
    for row in sorted(seen_rows):
        y = row * cell_height
        draw.line((0, y, atlas.width - 1, y), fill=(72, 72, 72))
        baseline = y + baseline_y
        draw.line(
            (0, baseline, atlas.width - 1, baseline),
            fill=(48, 112, 48),
        )
    image.save(path, optimize=True)


def resolve_metric(
    natural: int, scale: float, offset: float, override: int | None
) -> int:
    value = override if override is not None else int(round(natural * scale + offset))
    return max(1, min(4096, value))


def write_manifest(path: Path, report: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def command_inspect(args: argparse.Namespace) -> int:
    path = args.bfa.resolve()
    data = path.read_bytes()
    atlas = read_bfa(path)
    report = atlas_report(atlas, data)
    if args.report:
        write_manifest(args.report.resolve(), report)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


def command_export(args: argparse.Namespace) -> int:
    source = args.bfa.resolve()
    data = source.read_bytes()
    atlas = read_bfa(source)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    png = output / "cn_font.png"
    metrics = output / "cn_font.glyphs.tsv"
    report_path = output / "cn_font.manifest.json"
    save_png(png, atlas)
    write_metrics_tsv(metrics, atlas)
    report = {
        **atlas_report(atlas, data),
        "source_bfa": source.name,
        "png": png.name,
        "metrics": metrics.name,
        "png_editing_rule": (
            "L 灰度：黑=0 背景，白=255 字面，灰=抗锯齿"
        ),
    }
    write_manifest(report_path, report)
    print(f"[OK] PNG：{png}")
    print(f"[OK] 字形表：{metrics}")
    print(f"[OK] 清单：{report_path}")
    return 0


def command_import_png(args: argparse.Namespace) -> int:
    source = args.bfa.resolve()
    original = read_bfa(source)
    pixels = png_to_coverage(
        args.png.resolve(),
        original.width,
        original.height,
        args.channel,
    )
    records = (
        read_metrics_tsv(args.metrics.resolve(), original)
        if args.metrics
        else original.records
    )
    atlas = replace(original, records=records, pixels=pixels)
    output = args.output.resolve()
    data = write_bfa(output, atlas)
    read_back = read_bfa(output)
    if read_back != atlas:
        raise AssertionError("回写后的 BFA 重新读取不一致")
    if args.preview:
        save_png(args.preview.resolve(), atlas)
    print(
        f"[OK] BFA：{output}，{len(atlas.records)} 字形，"
        f"SHA-256={sha256_bytes(data)}"
    )
    return 0


def command_repack_grid(args: argparse.Namespace) -> int:
    source = args.bfa.resolve()
    source_data = source.read_bytes()
    original = read_bfa(source)
    atlas, placements = repack_atlas_grid(
        original,
        cell_width=args.cell_width,
        cell_height=args.cell_height,
        origin_x=args.origin_x,
        baseline_y=args.baseline_y,
        atlas_width=args.atlas_width,
        atlas_height=args.atlas_height,
        columns=args.columns,
        overflow=args.overflow,
    )
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    bfa_path = output / "cn_font_grid.bfa"
    png_path = output / "cn_font_grid.png"
    guide_path = output / "cn_font_grid.guide.png"
    metrics_path = output / "cn_font_grid.glyphs.tsv"
    map_path = output / "cn_font_grid.map.tsv"
    punctuation_path = output / "cn_font_grid.punctuation.tsv"
    manifest_path = output / "cn_font_grid.manifest.json"

    data = write_bfa(bfa_path, atlas)
    save_png(png_path, atlas)
    save_grid_guide(
        guide_path,
        atlas,
        placements,
        origin_x=args.origin_x,
        baseline_y=args.baseline_y,
    )
    write_metrics_tsv(metrics_path, atlas)
    write_grid_map(
        map_path,
        placements,
        origin_x=args.origin_x,
        baseline_y=args.baseline_y,
        punctuation_only=False,
    )
    write_grid_map(
        punctuation_path,
        placements,
        origin_x=args.origin_x,
        baseline_y=args.baseline_y,
        punctuation_only=True,
    )
    report = {
        **atlas_report(atlas, data),
        "layout": "fixed_unicode_grid",
        "source_bfa": source.name,
        "source_bfa_sha256": sha256_bytes(source_data),
        "cell_width": args.cell_width,
        "cell_height": args.cell_height,
        "origin_x": args.origin_x,
        "baseline_y": args.baseline_y,
        "columns": (
            args.columns
            if args.columns != 0
            else args.atlas_width // args.cell_width
        ),
        "overflow": args.overflow,
        "editable_png": png_path.name,
        "reference_guide_png": guide_path.name,
        "all_glyph_map": map_path.name,
        "punctuation_map": punctuation_path.name,
        "editing_rule": (
            "只编辑 cn_font_grid.png；guide.png 含彩色辅助线，绝不能回导"
        ),
    }
    write_manifest(manifest_path, report)
    print(f"[OK] 标准网格 BFA：{bfa_path}")
    print(f"[OK] 可回导 PNG：{png_path}")
    print(f"[OK] 只读辅助线图：{guide_path}")
    print(f"[OK] 全字符坐标表：{map_path}")
    print(f"[OK] 标点坐标表：{punctuation_path}")
    print(f"[OK] 清单：{manifest_path}")
    return 0


def command_build(args: argparse.Namespace) -> int:
    font_path = args.font.resolve()
    if not font_path.is_file():
        raise ValueError(f"字体文件不存在：{font_path}")
    codepoints = collect_codepoints(args)
    if not codepoints:
        raise ValueError("没有收集到任何字符")
    rasters, natural_line, natural_ascent, missing = rasterize(
        font_path,
        sorted(codepoints),
        font_index=args.font_index,
        font_size=args.font_size,
        oversample=args.oversample,
        stroke_width=args.stroke_width,
        raster_scale_x=args.raster_scale_x,
        raster_scale_y=args.raster_scale_y,
        bearing_x_offset=args.bearing_x_offset,
        bearing_y_offset=args.bearing_y_offset,
        advance_scale=args.advance_scale,
        advance_offset=args.advance_offset,
        gamma=args.raster_gamma,
        missing_policy=args.missing,
    )
    line_height = resolve_metric(
        natural_line,
        args.line_height_scale,
        args.line_height_offset,
        args.line_height,
    )
    ascent = resolve_metric(
        natural_ascent,
        args.ascent_scale,
        args.ascent_offset,
        args.ascent,
    )
    if ascent > line_height * 2:
        raise ValueError(
            f"ascent={ascent} 超过 line_height*2={line_height * 2}"
        )
    records, pixels = pack_rasters(
        rasters,
        args.atlas_width,
        args.atlas_height,
        args.padding,
        args.margin,
    )
    atlas = BfaAtlas(
        width=args.atlas_width,
        height=args.atlas_height,
        line_height=line_height,
        ascent=ascent,
        records=records,
        pixels=pixels,
    )
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    bfa_path = output / "cn_font.bfa"
    png_path = output / "cn_font.png"
    metrics_path = output / "cn_font.glyphs.tsv"
    manifest_path = output / "cn_font.manifest.json"
    data = write_bfa(bfa_path, atlas)
    save_png(png_path, atlas)
    write_metrics_tsv(metrics_path, atlas)
    report = {
        **atlas_report(atlas, data),
        "source_font": font_path.name,
        "source_font_sha256": sha256_bytes(font_path.read_bytes()),
        "font_index": args.font_index,
        "font_size": args.font_size,
        "oversample": args.oversample,
        "stroke_width": args.stroke_width,
        "raster_scale_x": args.raster_scale_x,
        "raster_scale_y": args.raster_scale_y,
        "bearing_x_offset": args.bearing_x_offset,
        "bearing_y_offset": args.bearing_y_offset,
        "advance_scale": args.advance_scale,
        "advance_offset": args.advance_offset,
        "raster_gamma": args.raster_gamma,
        "padding": args.padding,
        "margin": args.margin,
        "requested_codepoints": len(codepoints),
        "skipped_missing_codepoints": [
            f"U+{codepoint:04X}" for codepoint in missing
        ],
        "input_indexes": [path.name for path in args.index],
        "input_text_files": [path.name for path in args.text_file],
    }
    write_manifest(manifest_path, report)
    print(
        f"[OK] 字库：{bfa_path}，{atlas.width}x{atlas.height}，"
        f"{len(atlas.records)} 字形"
    )
    print(f"[OK] PNG：{png_path}")
    print(f"[OK] 字形表：{metrics_path}")
    print(f"[OK] 清单：{manifest_path}")
    return 0


def add_common_build_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--font-index", type=int, default=0)
    parser.add_argument("--font-size", type=int, default=32)
    parser.add_argument("--oversample", type=int, default=1)
    parser.add_argument("--stroke-width", type=float, default=0.0)
    parser.add_argument("--raster-scale-x", type=float, default=1.0)
    parser.add_argument("--raster-scale-y", type=float, default=1.0)
    parser.add_argument("--bearing-x-offset", type=float, default=0.0)
    parser.add_argument("--bearing-y-offset", type=float, default=0.0)
    parser.add_argument("--advance-scale", type=float, default=1.0)
    parser.add_argument("--advance-offset", type=float, default=0.0)
    parser.add_argument("--raster-gamma", type=float, default=1.0)
    parser.add_argument("--line-height", type=int)
    parser.add_argument("--line-height-scale", type=float, default=1.0)
    parser.add_argument("--line-height-offset", type=float, default=0.0)
    parser.add_argument("--ascent", type=int)
    parser.add_argument("--ascent-scale", type=float, default=1.0)
    parser.add_argument("--ascent-offset", type=float, default=0.0)
    parser.add_argument("--atlas-width", type=int, default=2048)
    parser.add_argument("--atlas-height", type=int, default=2048)
    parser.add_argument("--padding", type=int, default=2)
    parser.add_argument("--margin", type=int, default=2)
    parser.add_argument(
        "--missing",
        choices=("error", "skip"),
        default="error",
        help="源字体缺字时停止，或跳过并写入清单",
    )
    parser.add_argument(
        "--index",
        action="append",
        default=[],
        type=Path,
        help="BSCNLI2/3 索引，可重复；从中文 replacement 收集字符",
    )
    parser.add_argument(
        "--text-file",
        action="append",
        default=[],
        type=Path,
        help="额外 UTF-8 文本文件，可重复",
    )
    parser.add_argument(
        "--text-dir",
        action="append",
        default=[],
        type=Path,
        help="递归扫描 UTF-8 文本/JSON/CSV/Markdown，可重复",
    )
    parser.add_argument(
        "--characters",
        default="",
        help="命令行直接补充的字符",
    )
    parser.add_argument(
        "--include-ascii",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    parser.add_argument(
        "--include-cp1252",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    parser.add_argument("--output", required=True, type=Path)


def validate_build_arguments(args: argparse.Namespace) -> None:
    if not 8 <= args.font_size <= 256:
        raise ValueError("--font-size 必须在 8..256")
    if not 1 <= args.oversample <= 8:
        raise ValueError("--oversample 必须在 1..8")
    if not 0.0 <= args.stroke_width <= 32.0:
        raise ValueError("--stroke-width 必须在 0..32")
    if not 0.05 <= args.raster_scale_x <= 16.0:
        raise ValueError("--raster-scale-x 必须在 0.05..16")
    if not 0.05 <= args.raster_scale_y <= 16.0:
        raise ValueError("--raster-scale-y 必须在 0.05..16")
    if not -64.0 <= args.advance_scale <= 64.0:
        raise ValueError("--advance-scale 必须在 -64..64")
    if not 0.05 <= args.raster_gamma <= 20.0:
        raise ValueError("--raster-gamma 必须在 0.05..20")
    if not 128 <= args.atlas_width <= 8192:
        raise ValueError("--atlas-width 必须在 128..8192")
    if not 128 <= args.atlas_height <= 8192:
        raise ValueError("--atlas-height 必须在 128..8192")
    if not 0 <= args.padding <= 64 or not 0 <= args.margin <= 64:
        raise ValueError("--padding/--margin 必须在 0..64")
    for path in (*args.index, *args.text_file):
        if not path.is_file():
            raise ValueError(f"输入文件不存在：{path}")
    for path in args.text_dir:
        if not path.is_dir():
            raise ValueError(f"输入目录不存在：{path}")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "BloodstoneCN BFA 字库生成、检查、标准网格、PNG 导出与回导"
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect", help="检查 BFA")
    inspect_parser.add_argument("--bfa", required=True, type=Path)
    inspect_parser.add_argument("--report", type=Path)
    inspect_parser.set_defaults(handler=command_inspect)

    export_parser = subparsers.add_parser(
        "export", help="把 BFA 导出为灰度 PNG、TSV 和 JSON"
    )
    export_parser.add_argument("--bfa", required=True, type=Path)
    export_parser.add_argument("--output", required=True, type=Path)
    export_parser.set_defaults(handler=command_export)

    import_parser = subparsers.add_parser(
        "import-png", help="把编辑后的 PNG（及可选 TSV）回导为 BFA"
    )
    import_parser.add_argument("--bfa", required=True, type=Path)
    import_parser.add_argument("--png", required=True, type=Path)
    import_parser.add_argument("--metrics", type=Path)
    import_parser.add_argument("--output", required=True, type=Path)
    import_parser.add_argument("--preview", type=Path)
    import_parser.add_argument(
        "--channel",
        choices=("auto", "luminance", "alpha", "red"),
        default="auto",
    )
    import_parser.set_defaults(handler=command_import_png)

    grid_parser = subparsers.add_parser(
        "repack-grid",
        help="把现有 BFA 无缩放重排为按 Unicode 排序的固定网格",
    )
    grid_parser.add_argument("--bfa", required=True, type=Path)
    grid_parser.add_argument("--cell-width", type=int, default=36)
    grid_parser.add_argument("--cell-height", type=int, default=44)
    grid_parser.add_argument(
        "--origin-x",
        type=int,
        default=2,
        help="每格内部的共同书写原点 X",
    )
    grid_parser.add_argument(
        "--baseline-y",
        type=int,
        default=35,
        help="每格内部的共同基线 Y",
    )
    grid_parser.add_argument("--atlas-width", type=int, default=2048)
    grid_parser.add_argument("--atlas-height", type=int, default=2048)
    grid_parser.add_argument(
        "--columns",
        type=int,
        default=0,
        help="每行格数；0 表示按图集宽度自动取最大值",
    )
    grid_parser.add_argument(
        "--overflow",
        choices=("error", "clip"),
        default="error",
        help="原字形超出格子时停止，或明确裁切",
    )
    grid_parser.add_argument("--output", required=True, type=Path)
    grid_parser.set_defaults(handler=command_repack_grid)

    build_parser = subparsers.add_parser(
        "build", help="从 TTF/OTF 和当前汉化索引生成新 BFA"
    )
    add_common_build_arguments(build_parser)
    build_parser.set_defaults(handler=command_build)

    scan_parser = subparsers.add_parser(
        "scan-json",
        help="递归扫描 JSON 字符；中文模式排除日文字段，日文模式只取日文字段",
    )
    scan_parser.add_argument("--json-dir", required=True, type=Path)
    scan_parser.add_argument("--language", choices=("zh", "ja"), required=True)
    scan_parser.add_argument("--output", required=True, type=Path)
    scan_parser.set_defaults(handler=command_scan_json)

    rebuild_parser = subparsers.add_parser(
        "rebuild",
        help="从当前 JSON 全量字符集 + font_source 字体完整重建 Grid074 fonts.bfa",
    )
    rebuild_parser.add_argument("--json-dir", required=True, type=Path)
    rebuild_parser.add_argument("--language", choices=("zh", "ja"), required=True)
    rebuild_parser.add_argument("--font-source", type=Path, default=Path("font_source"))
    rebuild_parser.add_argument(
        "--font-name",
        help="font_source 内字体文件名/相对路径；多个字体无法自动判断时必须指定",
    )
    rebuild_parser.add_argument(
        "--font-index",
        type=int,
        default=None,
        help="TTC/OTC 字体面；省略时按中文/日文模式自动选择",
    )
    rebuild_parser.add_argument("--font-size", type=int, default=32)
    rebuild_parser.add_argument("--oversample", type=int, default=1)
    rebuild_parser.add_argument("--stroke-width", type=float, default=0.0)
    rebuild_parser.add_argument("--raster-scale-x", type=float, default=1.0)
    rebuild_parser.add_argument("--raster-scale-y", type=float, default=1.0)
    rebuild_parser.add_argument("--bearing-x-offset", type=float, default=0.0)
    rebuild_parser.add_argument("--bearing-y-offset", type=float, default=0.0)
    rebuild_parser.add_argument("--advance-scale", type=float, default=1.0)
    rebuild_parser.add_argument("--advance-offset", type=float, default=0.0)
    rebuild_parser.add_argument("--raster-gamma", type=float, default=1.0)
    # Grid074 冻结指标；可显式覆盖，但默认不随源字体自然 metrics 漂移。
    rebuild_parser.add_argument("--line-height", type=int, default=50)
    rebuild_parser.add_argument("--line-height-scale", type=float, default=1.0)
    rebuild_parser.add_argument("--line-height-offset", type=float, default=0.0)
    rebuild_parser.add_argument("--ascent", type=int, default=38)
    rebuild_parser.add_argument("--ascent-scale", type=float, default=1.0)
    rebuild_parser.add_argument("--ascent-offset", type=float, default=0.0)
    rebuild_parser.add_argument("--atlas-width", type=int, default=2048)
    rebuild_parser.add_argument("--atlas-height", type=int, default=2048)
    rebuild_parser.add_argument("--padding", type=int, default=2)
    rebuild_parser.add_argument("--margin", type=int, default=2)
    rebuild_parser.add_argument("--cell-width", type=int, default=36)
    rebuild_parser.add_argument("--cell-height", type=int, default=44)
    rebuild_parser.add_argument("--origin-x", type=int, default=2)
    rebuild_parser.add_argument("--baseline-y", type=int, default=35)
    rebuild_parser.add_argument("--grid-atlas-width", type=int, default=2048)
    rebuild_parser.add_argument("--grid-atlas-height", type=int, default=2048)
    rebuild_parser.add_argument("--columns", type=int, default=0)
    rebuild_parser.add_argument("--overflow", choices=("error", "clip"), default="error")
    rebuild_parser.add_argument("--missing", choices=("error", "skip"), default="error")
    rebuild_parser.add_argument(
        "--extra-text-file",
        action="append",
        default=[],
        type=Path,
        help="必须永久保留的额外 UTF-8 字符文件，可重复；仍属于全量重建输入",
    )
    rebuild_parser.add_argument("--characters", default="", help="额外固定字符")
    rebuild_parser.add_argument(
        "--include-ascii", action=argparse.BooleanOptionalAction, default=True
    )
    rebuild_parser.add_argument(
        "--include-cp1252", action=argparse.BooleanOptionalAction, default=True
    )
    rebuild_parser.add_argument("--output", required=True, type=Path)
    rebuild_parser.set_defaults(handler=command_rebuild)

    args = parser.parse_args(argv)
    if args.command == "build":
        validate_build_arguments(args)
    elif args.command == "rebuild":
        if not args.json_dir.is_dir():
            parser.error(f"--json-dir 不是目录：{args.json_dir}")
        if not args.font_source.is_dir():
            parser.error(f"--font-source 不是目录：{args.font_source}")
        if not 8 <= args.font_size <= 256:
            parser.error("--font-size 必须在 8..256")
        if not 1 <= args.oversample <= 8:
            parser.error("--oversample 必须在 1..8")
        if not 128 <= args.atlas_width <= 8192 or not 128 <= args.atlas_height <= 8192:
            parser.error("紧凑图集尺寸必须在 128..8192")
        if not 128 <= args.grid_atlas_width <= 8192 or not 128 <= args.grid_atlas_height <= 8192:
            parser.error("网格图集尺寸必须在 128..8192")
        if not 1 <= args.cell_width <= 512 or not 1 <= args.cell_height <= 512:
            parser.error("网格 cell 必须在 1..512")
        for path in args.extra_text_file:
            if not path.is_file():
                parser.error(f"--extra-text-file 不存在：{path}")
    elif args.command == "scan-json":
        if not args.json_dir.is_dir():
            parser.error(f"--json-dir 不是目录：{args.json_dir}")
    elif args.command == "repack-grid":
        if not 1 <= args.cell_width <= 512:
            parser.error("--cell-width 必须在 1..512")
        if not 1 <= args.cell_height <= 512:
            parser.error("--cell-height 必须在 1..512")
        if not 128 <= args.atlas_width <= 8192:
            parser.error("--atlas-width 必须在 128..8192")
        if not 128 <= args.atlas_height <= 8192:
            parser.error("--atlas-height 必须在 128..8192")
        if not 0 <= args.columns <= 8192:
            parser.error("--columns 必须在 0..8192")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    return int(args.handler(args))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        raise SystemExit(1)
