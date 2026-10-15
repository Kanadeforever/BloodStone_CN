# -*- coding: utf-8 -*-
"""把游戏原生 OLTX/LOC 与便于人工修订的 JSON 相互转换。

工具只负责游戏当前的 EN 翻译。每次重建都使用用户自行提供的
原版 gamedata.pak 作为唯一结构模板，因此原始 KeyTable、语言目录中
不属于 EN 的七个语言块不会因为人工修订或 JSON 序列化而改变。

文件中的相对偏移和哈希都是资源索引，而不是可翻译的正文。
输入 JSON 的每个条目都必须保持 hash、key、original 不变；
只有 translate 字段可编辑。导入时重新按无符号 32 位哈希升序构造
EN 索引，以满足已在原版 86 个 LOC 中确认的排序不变量。
"""

from __future__ import annotations

import collections
import hashlib
import json
import re
import struct
import sys
import zipfile
from pathlib import Path

# 共用原生 PAK/LOC 解析器，避免并行维护两套彼此矛盾的文件格式判断。
# 源码目录位于同一个发行工程内，构建时不需要从网络安装第三方包。
_NATIVE_SOURCE = Path(__file__).resolve().parents[2] / 'NativePakLoc' / 'src'
if str(_NATIVE_SOURCE) not in sys.path:
    sys.path.insert(0, str(_NATIVE_SOURCE))
from native_pak import (NativePak, language_blocks, normalized_name,
                        read_key_entries, read_text_entries, sort_language_index)

FORMAT = 'BSCN-LOC-JSON-v1'
TOKEN_RE = re.compile(r'~~\{[^{}\r\n]+\}~~')
LANGUAGES = ('en', 'fr', 'de', 'it', 'ja', 'pl', 'ru', 'es')


def sha256(data: bytes) -> str:
    """用原始二进制的指纹识别 JSON 对应的原版模板。"""
    return hashlib.sha256(data).hexdigest()


def portable_path(path: Path) -> str:
    """包内文件输出相对路径，让复制或解包到其它磁盘仍可重现同一报告。"""
    # 独立版携带自己的原始 PAK；完整工程则继续以工程根目录为基准。
    # 两种布局都只写相对路径，便于跨盘符移动后重建相同的诊断报告。
    candidate = Path(__file__).resolve().parents[1]
    root = candidate if (candidate / 'original' / 'gamedata.pak').is_file() else Path(__file__).resolve().parents[3]
    full = path.resolve()
    try:
        return full.relative_to(root).as_posix()
    except ValueError:
        # 用户手动传入包外位置时仍保留实际路径，以便定位输出文件。
        return str(full)


def json_bytes(value: dict) -> bytes:
    """保证中文可以直接编辑，生成的文本使用统一的 CRLF 和 UTF-8 无 BOM。"""
    content = json.dumps(value, ensure_ascii=False, indent=2) + '\n'
    return content.replace('\n', '\r\n').encode('utf-8')


def export_one(original: bytes, localized: bytes, resource: str) -> dict:
    """导出一份 LOC，original 来自原版，translate 来自现有汉化。

    所有词条由原版 KeyTable 决定，保证不会因为某条译文为空就
    把对应的哈希遗漏。导出的 JSON 只有英文目标语言需要编辑。
    """
    keys = read_key_entries(original)
    if read_key_entries(localized) != keys:
        raise ValueError(f'{resource}: 中文 LOC 的 KeyTable 与原版不一致')
    if [l for l, _, _ in language_blocks(original)] != list(LANGUAGES):
        raise ValueError(f'{resource}: 原版语言目录与当前 EN 方案不一致')
    # 外部素材若改变了其它语言，也必须明确拒绝，而非在回编时静默丢弃。
    for lang in LANGUAGES[1:]:
        if read_text_entries(original, lang) != read_text_entries(localized, lang):
            raise ValueError(f'{resource}: {lang} 语言正文不同于原版')
    base = read_text_entries(original, 'en')
    translated = read_text_entries(localized, 'en')
    if set(keys) != set(base) or set(keys) != set(translated):
        raise ValueError(f'{resource}: 英文语言块词条集合不完整')
    entries = []
    for keyhash in sorted(keys):
        entries.append({
            'hash': f'0x{keyhash:08X}',
            'key': keys[keyhash],
            'original': base[keyhash],
            'translate': translated[keyhash],
        })
    return {
        'format': FORMAT,
        'resource': resource,
        'language': 'en',
        'template_sha256': sha256(original),
        'baseline_sha256': sha256(localized),
        'entries': entries,
    }


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict:
    """拒绝同一 JSON 对象中重复字段，防止编辑器误合并时静默覆盖译文。"""
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f'JSON 含重复字段：{key}')
        out[key] = value
    return out


def _decode_hash(value: object, resource: str, position: int) -> int:
    """统一检查哈希的 32 位身份，禁止浮点数字或溢出被悄悄截断。"""
    if not isinstance(value, str) or not re.fullmatch(r'0[xX][0-9a-fA-F]{8}', value):
        raise ValueError(f'{resource}: 第 {position} 条 hash 必须是 0x 加八位十六进制数')
    return int(value, 16)


def validate_json(document: dict, template: bytes, resource: str,
                  strict_tokens: bool = False) -> tuple[dict[int, str], list[dict]]:
    """拒绝哈希增删、原文变更、路径串线及非法 Unicode 代理项。

    动作 token 与文本可能包含引擎语义，默认提供警告，但不因历史
    译文中的七条已知 token 翻译而阻塞全部 86 个文件的回编。
    只有开启 strict_tokens 才把 token 差异判定为错误。
    """
    if not isinstance(document, dict):
        raise ValueError(f'{resource}: JSON 顶层必须是对象')
    if document.get('format') != FORMAT:
        raise ValueError(f'{resource}: JSON 格式版本不受支持')
    if document.get('resource') != resource or document.get('language') != 'en':
        raise ValueError(f'{resource}: JSON 资源路径或目标语言不匹配')
    if document.get('template_sha256') != sha256(template):
        raise ValueError(f'{resource}: 模板指纹不一致，不允许跨游戏版本混用')
    entries = document.get('entries')
    if not isinstance(entries, list):
        raise ValueError(f'{resource}: entries 必须是数组')
    keys = read_key_entries(template)
    old = read_text_entries(template, 'en')
    if len(entries) != len(keys):
        raise ValueError(f'{resource}: 译文条目数量不是原版的 {len(keys)}')
    result: dict[int, str] = {}
    warnings: list[dict] = []
    for index, item in enumerate(entries, 1):
        if not isinstance(item, dict):
            raise ValueError(f'{resource}: 第 {index} 条不是对象')
        keyhash = _decode_hash(item.get('hash'), resource, index)
        if keyhash not in keys or keyhash in result:
            raise ValueError(f'{resource}: 第 {index} 条哈希非法、重复或不存在于原版')
        if item.get('key') != keys[keyhash]:
            raise ValueError(f'{resource}: 第 {index} 条 key 与原版不一致')
        if item.get('original') != old[keyhash]:
            raise ValueError(f'{resource}: 第 {index} 条 original 被修改')
        translation = item.get('translate')
        if not isinstance(translation, str):
            raise ValueError(f'{resource}: 第 {index} 条 translate 缺失或不是字符串')
        if '\x00' in translation:
            raise ValueError(f'{resource}: 第 {index} 条译文包含 NUL，可能截断原生字符串')
        try:
            translation.encode('utf-16le', errors='strict')
        except UnicodeEncodeError as exc:
            raise ValueError(f'{resource}: 第 {index} 条译文包含非法 Unicode 代理项') from exc
        # 原引擎使用 UTF-16LE 零结尾，文本可以是空字符串，但不能含 NUL。
        source_tokens = collections.Counter(TOKEN_RE.findall(old[keyhash]))
        target_tokens = collections.Counter(TOKEN_RE.findall(translation))
        if source_tokens != target_tokens:
            warnings.append({'resource': resource, 'hash': f'0x{keyhash:08X}',
                             'key': keys[keyhash], 'source_tokens': dict(source_tokens),
                             'translated_tokens': dict(target_tokens)})
        result[keyhash] = translation
    if set(result) != set(keys):
        raise ValueError(f'{resource}: 译文哈希集合与原版不同')
    if strict_tokens and warnings:
        raise ValueError(f'{resource}: {len(warnings)} 条译文的动作 token 有差异')
    return result, warnings


def import_one(template: bytes, document: dict, resource: str,
               strict_tokens: bool = False, baseline: bytes | None = None) -> tuple[bytes, list[dict]]:
    """按原版语言目录重建 EN，并原样复制其余七个语言块。

    目标码点查找应使用排序后的哈希/相对偏移数组。正文 UTF-16LE
    在所有索引之后写入，每个偏移都相对于 EN 块起点。语言块的
    起点变更通过目录中的绝对偏移同步更新。
    """
    translations, warnings = validate_json(document, template, resource, strict_tokens)
    blocks = language_blocks(template)
    if tuple(lang for lang, _, _ in blocks) != LANGUAGES:
        raise ValueError(f'{resource}: 语言排列不是已验证的原生布局')
    first = blocks[0][1]
    if blocks[-1][1] + blocks[-1][2] != len(template):
        raise ValueError(f'{resource}: 模板语言块尾部存在未知附加数据')
    if first < 0xDC or first >= len(template):
        raise ValueError(f'{resource}: 模板 EN 块偏移无效')
    # 导出 JSON 时记录了译文本身的指纹。用户未改变任何内容时，
    # 直接返回原始修复版 LOC，保证 LOC→JSON→LOC 字节级可逆。
    # 有修改时沿用修复版语言块中字符串的存放顺序，而不是按哈希
    # 重新排列字符串，以便最大限度减少生成文件中的无关差异。
    string_order = sorted(translations)
    if baseline is not None:
        if document.get('baseline_sha256') != sha256(baseline):
            raise ValueError(f'{resource}: 中文 LOC 基线指纹不一致')
        if read_key_entries(baseline) != read_key_entries(template):
            raise ValueError(f'{resource}: 中文 LOC 基线 KeyTable 不一致')
        baseline_blocks = language_blocks(baseline)
        if tuple(lang for lang, _, _ in baseline_blocks) != LANGUAGES:
            raise ValueError(f'{resource}: 中文 LOC 基线语言顺序错误')
        for i in range(1, 8):
            _, original_begin, original_size = blocks[i]
            _, baseline_begin, baseline_size = baseline_blocks[i]
            if template[original_begin:original_begin + original_size] != baseline[baseline_begin:baseline_begin + baseline_size]:
                raise ValueError(f'{resource}: 中文 LOC 基线修改了非 EN 语言块')
        old_translations = read_text_entries(baseline, 'en')
        if set(old_translations) != set(translations):
            raise ValueError(f'{resource}: 中文 LOC 基线 EN 哈希集合不一致')
        base_start, base_size = baseline_blocks[0][1:]
        count = len(translations)
        indexes = [struct.unpack_from('<II', baseline, base_start + 8 + i * 8)
                   for i in range(count)]
        if len({hash_value for hash_value, _ in indexes}) != count:
            raise ValueError(f'{resource}: 中文 LOC 基线出现重复哈希')
        if [hash_value for hash_value, _ in indexes] != sorted(translations):
            raise ValueError(f'{resource}: 中文 LOC 基线哈希索引不是原生升序')
        # 原始字符串的放置顺序由相对偏移确定，它通常和哈希顺序不同。
        string_order = [hash_value for hash_value, _ in sorted(indexes, key=lambda item: item[1])]
        if translations == old_translations:
            return baseline, warnings
    # 必须原样保留原版 KeyTable 和语言块以前的全部字节。
    result = bytearray(template[:first])
    count = len(translations)
    strings = bytearray()
    relative: dict[int, int] = {}
    for keyhash in string_order:
        relative[keyhash] = 8 + 8 * count + len(strings)
        utf16 = translations[keyhash].encode('utf-16le') + b'\x00\x00'
        if relative[keyhash] > 0xFFFFFFFF:
            raise ValueError(f'{resource}: 单个 LOC 超过 32 位索引范围')
        strings.extend(utf16)
    en = bytearray(struct.pack('<II', count, 8))
    for keyhash in sorted(translations):
        en.extend(struct.pack('<II', keyhash, relative[keyhash]))
    en.extend(strings)
    # 不对原始七种语言重新编码，保持其纹理控制符和所有二进制字节。
    payloads = [bytes(en)]
    for language, begin, size in blocks[1:]:
        original_block = template[begin:begin + size]
        ordered, changed = sort_language_index(original_block, language)
        if changed or ordered != original_block:
            raise ValueError(f'{resource}: 原版 {language} 索引异常，拒绝直接复制')
        payloads.append(original_block)
    for index, payload in enumerate(payloads):
        directory = 0x1C + index * 24
        struct.pack_into('<II', result, directory + 16, len(result), len(payload))
        result.extend(payload)
    output = bytes(result)
    if read_key_entries(output) != read_key_entries(template):
        raise AssertionError(f'{resource}: 回编时 KeyTable 改变')
    if read_text_entries(output, 'en') != translations:
        raise AssertionError(f'{resource}: 回编译文与输入 JSON 不一致')
    # 对每个语言区块检查二分查找所需要的升序约束。
    for language, start, size in language_blocks(output):
        _, changed = sort_language_index(output[start:start + size], language)
        if changed:
            raise AssertionError(f'{resource}: {language} 哈希索引不再有序')
    for index in range(1, 8):
        _, original_begin, original_size = blocks[index]
        _, new_begin, new_size = language_blocks(output)[index]
        if output[new_begin:new_begin + new_size] != template[original_begin:original_begin + original_size]:
            raise AssertionError(f'{resource}: 非 EN 的原始字节被改动')
    return output, warnings


def _safe_relative_name(entry: str, expected: str) -> Path:
    """把外部路径限定为原版索引中的规范相对路径，杜绝目录穿越。"""
    if entry != expected or entry.startswith('/') or '..' in Path(entry).parts:
        raise ValueError(f'非法资源路径：{entry}')
    return Path(*entry.split('/'))


def _read_localized(root: Path | None, zip_path: Path | None, resource: str) -> bytes:
    """支持松散 LOC 与用户既有 ZIP 两种输入方式。"""
    if (root is None) == (zip_path is None):
        raise ValueError('必须且只能选择 LOC 文件夹或 LOC ZIP 一种输入')
    if zip_path:
        with zipfile.ZipFile(zip_path) as archive:
            matches = [name for name in archive.namelist()
                       if normalized_name(name) == resource]
            if len(matches) != 1:
                raise ValueError(f'中文 ZIP 缺失或重复资源：{resource}')
            return archive.read(matches[0])
    assert root is not None
    path = root / _safe_relative_name(resource, resource)
    return path.read_bytes()


def export_batch(original_pak: Path, out_dir: Path, *,
                 loc_root: Path | None = None, loc_zip: Path | None = None,
                 overwrite: bool = False) -> dict:
    """只有明确授权覆盖，才能从 LOC 重新导出全套 JSON。

    译文 JSON 是人工修订的唯一主源。用户可能已经修改过其中的数千条译文，
    因而这里必须在写入任何文件前，一次性检查整套输出是否已存在。
    默认拒绝覆盖可以避免旧版 LOC 把尚未回编的人工修改悄悄覆盖掉。
    """
    original = NativePak(original_pak.read_bytes())
    files = sorted(name for name in original.by_name if name.endswith('.loc'))
    if len(files) != 86:
        raise ValueError(f'当前基线预期 86 个 LOC，实际 {len(files)}')
    staged: list[tuple[Path, bytes]] = []
    counts = 0
    for resource in files:
        original_loc = original.read_name(resource)
        chinese = _read_localized(loc_root, loc_zip, resource)
        document = export_one(original_loc, chinese, resource)
        counts += len(document['entries'])
        path = out_dir / _safe_relative_name(resource, resource)
        staged.append((path.with_name(path.name + '.json'), json_bytes(document)))
    # 默认严格禁止覆盖任何已有的 JSON；即使只存在其中一个文件，
    # 也必须先人工整理，防止半套旧数据和半套新数据混用。
    existing = [path for path, _ in staged if path.exists()]
    if existing and not overwrite:
        raise ValueError(f'译文 JSON 已存在 {len(existing)} 份；拒绝覆盖。确实需要重新导出时请明确指定 --overwrite')
    # 所有输入与覆盖权限先检查成功，再写文件，避免留下半套待翻译 JSON。
    for path, payload in staged:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
    return {'files': len(staged), 'entries': counts, 'output': portable_path(out_dir)}


def _zip_bytes(files: dict[str, bytes]) -> bytes:
    """将导入后的松散 LOC 打成可复现的 ZIP，供旧资源构建器复用。"""
    import io
    memory = io.BytesIO()
    with zipfile.ZipFile(memory, 'w', compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9) as zipped:
        for name, payload in sorted(files.items()):
            item = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            item.external_attr = 0o100644 << 16
            zipped.writestr(item, payload, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    return memory.getvalue()


def import_batch(original_pak: Path, json_dir: Path, out_dir: Path, *,
                 zip_output: Path | None = None, strict_tokens: bool = False,
                 baseline_dir: Path | None = None, font_bfa: Path | None = None,
                 strict_glyphs: bool = False) -> dict:
    """先完整验证 86 份 JSON，再落盘松散 LOC 与可选 ZIP。"""
    original = NativePak(original_pak.read_bytes())
    resources = sorted(name for name in original.by_name if name.endswith('.loc'))
    if len(resources) != 86:
        raise ValueError(f'原版资源数量不符：{len(resources)}')
    staged: dict[str, bytes] = {}
    warnings: list[dict] = []
    count = 0
    used_cjk: set[int] = set()
    expected_paths = set()
    for resource in resources:
        rel = _safe_relative_name(resource, resource)
        path = json_dir / rel.parent / (rel.name + '.json')
        expected_paths.add(path.relative_to(json_dir).as_posix())
        # 不接受 BOM 或格式错误时的宽松替代解码。
        document = json.loads(path.read_text(encoding='utf-8-sig'),
                              object_pairs_hook=_unique_json_object)
        baseline = (baseline_dir / rel).read_bytes() if baseline_dir is not None else None
        payload, new_warnings = import_one(original.read_name(resource), document,
                                           resource, strict_tokens, baseline)
        staged[resource] = payload
        localized_texts = read_text_entries(payload, 'en')
        count += len(localized_texts)
        for value in localized_texts.values():
            for char in value:
                cp = ord(char)
                if (0x3400 <= cp <= 0x9FFF) or (0x20000 <= cp <= 0x2FA1F):
                    used_cjk.add(cp)
        warnings.extend(new_warnings)
    provided = {p.relative_to(json_dir).as_posix() for p in json_dir.rglob('*.loc.json')}
    if provided != expected_paths:
        raise ValueError(f'JSON 文件集合存在额外或缺失资源：额外 {sorted(provided - expected_paths)}，缺失 {sorted(expected_paths - provided)}')
    # 检查输出不能与 JSON、中文基线、原版封包所在目录重叠。
    out_resolved = out_dir.resolve()
    if (out_resolved == json_dir.resolve()
            or (baseline_dir is not None and
                (out_resolved == baseline_dir.resolve()
                 or baseline_dir.resolve().is_relative_to(out_resolved)))
            or original_pak.resolve().is_relative_to(out_resolved)):
        raise ValueError('输出目录可能覆盖翻译输入或原始参考资料，拒绝写入')
    if zip_output is not None and (zip_output.resolve() == original_pak.resolve()
                                   or zip_output.resolve().is_relative_to(json_dir.resolve())
                                   or (baseline_dir is not None and
                                       zip_output.resolve().is_relative_to(baseline_dir.resolve()))):
        raise ValueError('输出 ZIP 路径可能覆盖原始参考资料或 JSON，拒绝写入')
    missing_cjk: list[int] = []
    if font_bfa is not None:
        # 译文允许继续补充汉字，但应告知哪些新增汉字还未收录在
        # 本发行包所用的原生 EUR 字库来源 BFA 中，防止再次出现方框。
        source = Path(__file__).resolve().parents[2] / 'NativeEurFont' / 'src'
        if str(source) not in sys.path:
            sys.path.insert(0, str(source))
        from native_font import ChineseAtlas
        available = set(ChineseAtlas.from_bfa(font_bfa.read_bytes()).glyphs)
        missing_cjk = sorted(used_cjk - available)
        if strict_glyphs and missing_cjk:
            sample = ','.join(f'U+{cp:04X}' for cp in missing_cjk[:12])
            raise ValueError(f'新增译文含 {len(missing_cjk)} 个当前中文字库未覆盖的汉字：{sample}')
    for resource, payload in sorted(staged.items()):
        target = out_dir / _safe_relative_name(resource, resource)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
    if zip_output is not None:
        zip_output.parent.mkdir(parents=True, exist_ok=True)
        zip_output.write_bytes(_zip_bytes(staged))
        # 校验压缩包与松散 LOC 的实际字节相同。
        with zipfile.ZipFile(zip_output) as zipped:
            for name, payload in staged.items():
                if zipped.read(name) != payload:
                    raise AssertionError(f'{name}: ZIP 内容与输出 LOC 不一致')
    return {'files': len(staged), 'entries': count, 'warnings': warnings,
            'warning_count': len(warnings),
            'missing_cjk_count': len(missing_cjk),
            'missing_cjk': [f'U+{cp:04X}' for cp in missing_cjk],
            'output': portable_path(out_dir),
            'zip_output': portable_path(zip_output) if zip_output else None}
