# -*- coding: utf-8 -*-
"""只读解析 Blood Stone PC 原生 2KAP 封包以及 OLTX 文字资源。

公开工具不提供 PAK 写回或重新封包操作。FilePak 通过只读内存映射
访问用户自己的大型 root.pak，只解压调用者明确选中的资源。

支持完整路径名字表和父目录索引两种已验证布局。
未知版本及未知条目布局必须拒绝，不能猜测后继续生成资源。
"""

from __future__ import annotations

import hashlib
import json
import mmap
import struct
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path

SIGNATURE = b'2KAP'
SUPPORTED_VERSIONS = (2, 3)
NAME_KEY = b'VXo40j3@$%\\%`x'
MIN_ENTRIES = 1
MAX_ENTRIES = 100_000


def align_up(value: int, align: int) -> int:
    """计算不小于 value 的下一个对齐边界，保持数据块编号可被引擎使用。"""
    if align <= 0 or (align & (align - 1)):
        raise ValueError('封包对齐值不是正的二次幂')
    return (value + align - 1) & ~(align - 1)


def file_hash(data: bytes) -> str:
    """使用 SHA-256 检查输入、输出是否被意外改变。"""
    return hashlib.sha256(data).hexdigest()


def decode_names(data: bytes) -> bytes:
    """按当前样本已验证的异或规则还原文件名表，仅用于精确匹配原条目。"""
    return bytes(b ^ NAME_KEY[i % len(NAME_KEY)] for i, b in enumerate(data))


def read_u32(data: bytes, offset: int) -> int:
    """每次读取都检查边界，以防损坏的封包驱动解析器越界。"""
    if offset < 0 or offset + 4 > len(data):
        raise ValueError(f'四字节字段越界：0x{offset:X}')
    return struct.unpack_from('<I', data, offset)[0]


def read_i64(data: bytes, offset: int) -> int:
    """读取数据块编号；-1 是没有实际数据的目录/占位条目。"""
    if offset < 0 or offset + 8 > len(data):
        raise ValueError(f'八字节字段越界：0x{offset:X}')
    return struct.unpack_from('<q', data, offset)[0]


def normalized_name(name: str) -> str:
    """只接受安全的相对资源名，再统一斜杠及大小写。"""
    value = name.replace('\\', '/')
    # 原版索引用单个前导反斜杠表示引擎虚拟根，例如 \gamedata\。
    # 去掉此标记后再检查相对名，不能将其当成磁盘根或放行 UNC。
    if name.startswith('\\') and not value.startswith('//'):
        value = value[1:]
    if (not value or value.startswith('/') or ':' in value
            or any(part in ('', '.', '..') for part in value.split('/'))
            or any(ord(char) < 32 for char in value)):
        raise ValueError(f'不安全的封包资源名：{name!r}')
    return value.lower()


@dataclass(frozen=True)
class PakEntry:
    name: str
    record_offset: int
    size: int
    data_offset: int
    offset_units: int
    compressed_size: int


class NativePak:
    """封装原生索引和完整字节，向调用方提供安全的只读解压接口。"""

    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 12:
            raise ValueError('PAK 文件头不足 12 字节，无法读取格式')
        self.version = read_u32(data, 4)
        if data[:4] != SIGNATURE or self.version not in SUPPORTED_VERSIONS:
            raise ValueError(f'不支持的 PAK 文件头：标识={data[:4]!r}，版本={self.version}；支持 PC 2KAP v2/v3')
        self.alignment = read_u32(data, 8)
        if self.alignment < 512 or self.alignment > 65536:
            raise ValueError('PAK 数据对齐值不在可信范围内')
        align_up(1, self.alignment)

        # 本版 names_size 位于第一个对齐页；紧随其后的数据是异或文件名表。
        # 只接受已经确认的名字表布局，避免把其他封包头误识别为原生资源。
        if read_u32(data, 0x38) != 0:
            raise ValueError('本样本不使用头部内联名字表布局，拒绝未知样本')
        name_size = read_u32(data, self.alignment)
        name_begin = self.alignment + 4
        name_end = name_begin + name_size
        if name_size <= 0 or name_end >= len(data):
            raise ValueError('加密文件名表越界')
        self.names = decode_names(data[name_begin:name_end])
        table_start = align_up(name_end, self.alignment)
        declared = read_u32(data, table_start)
        if not MIN_ENTRIES <= declared <= MAX_ENTRIES:
            raise ValueError('资源表声明的条目数量异常')
        if self.version == 3 and read_u32(data, table_start + 4) != declared * 40:
            raise ValueError('目录索引的记录区长度与条目数量不一致')

        self.entries: list[PakEntry] = []
        self.empty_entries = 0
        cursor = table_start + 12
        for _ in range(declared):
            # 前面保存名称、未压缩大小、时间戳和数据块编号。
            # 完整路径布局可能带扩展区；目录索引布局固定每条 40 字节，
            # 末尾字段不是扩展标志，不能据它改变下一条记录的位置。
            start = cursor
            name_off = read_u32(data, start)
            size = read_u32(data, start + 8)
            units = read_i64(data, start + 20)
            # 偏移 0x20 是原生读取器实际使用的压缩字节长度。
            # 不能仅靠下一块的位置推算，否则错误长度会被对齐填充掩盖。
            compressed_size = read_u32(data, start + 32)
            extension_flag = read_u32(data, start + 36)
            cursor = start + 40 + (24 if self.version == 2 and extension_flag == 0 else 0)
            if cursor > len(data):
                raise ValueError('PAK 索引记录越界')
            if units == -1:
                self.empty_entries += 1
                continue
            if units < 0 or name_off >= len(self.names):
                raise ValueError('PAK 存在非法资源条目')
            end = self.names.find(b'\0', name_off)
            if end < 0:
                raise ValueError('PAK 文件名缺少结束标记')
            name = self.names[name_off:end].decode('utf-8', errors='strict')
            position = units * self.alignment
            if not name or position < cursor or position >= len(data):
                raise ValueError(f'PAK 资源位置或文件名异常：{name}')
            # 严格按照索引声明的字节范围验收，不能把后续资源的数据
            # 或对齐填充当成当前 zlib 流的一部分。
            if not compressed_size or compressed_size > len(data) - position:
                raise ValueError(f'PAK 压缩长度无效：{name}')
            self.entries.append(PakEntry(name, start, size, position, units, compressed_size))

        self.index_end = cursor
        if self.version == 3:
            # 索引里只存文件名片段，通过父目录记录偏移还原完整路径。
            # 同名文件可以位于不同目录，不能仅凭文件名建立资源字典。
            paths = self._tree_paths(table_start + 12, declared)
            self.entries = [PakEntry(paths[e.record_offset], e.record_offset, e.size,
                                     e.data_offset, e.offset_units, e.compressed_size)
                            for e in self.entries]
        self.by_name: dict[str, PakEntry] = {}
        for entry in self.entries:
            key = normalized_name(entry.name)
            if key in self.by_name:
                raise ValueError(f'PAK 包含重复文件名，不能安全覆盖：{key}')
            self.by_name[key] = entry
        if not self.entries or self.index_end >= min(e.data_offset for e in self.entries):
            raise ValueError('PAK 资源表与文件数据区域重叠')

        # 保留原文件的实际压缩数据边界。不同资源间可以存在对齐填充，
        # 所以不能用下一条的未压缩大小来切分当前 zlib 流。
        sorted_positions = sorted(e.data_offset for e in self.entries)
        if len(sorted_positions) != len(set(sorted_positions)):
            raise ValueError('PAK 出现重复数据块位置，当前生成器不处理共享块')
        # 检查索引中的压缩长度不会跨过下一资源的起点。
        # 仅有偏移正确是不够的：游戏会按每条记录的压缩长度读取。
        self.extents = {}
        for i, start in enumerate(sorted_positions):
            end = sorted_positions[i + 1] if i + 1 < len(sorted_positions) else len(data)
            self.extents[start] = end
        for entry in self.entries:
            if entry.compressed_size > self.extents[entry.data_offset] - entry.data_offset:
                raise ValueError(f'PAK 索引压缩范围与相邻资源重叠：{entry.name}')

    def _tree_paths(self, first: int, count: int) -> dict[int, str]:
        """校验父目录关系，再从根目录逐层拼出每个文件的真实资源名。"""
        components = []
        parents = []
        roots = []
        for index in range(count):
            record = first + index * 40
            offset = read_u32(self.data, record)
            if offset >= len(self.names):
                raise ValueError('目录索引的名称偏移越界')
            end = self.names.find(b'\0', offset)
            if end < 0:
                raise ValueError('目录索引的名称缺少结束标记')
            name = self.names[offset:end].decode('utf-8', errors='strict')
            parent_offset = read_u32(self.data, record + 4)
            if parent_offset == 0xFFFFFFFF:
                if name != '\\' or read_i64(self.data, record + 20) != -1:
                    raise ValueError('目录索引的根条目不是虚拟根目录')
                roots.append(index)
                parents.append(None)
                components.append('')
                continue
            # 父项使用相对于记录区开头的字节偏移，必须落在完整目录记录上。
            if parent_offset % 40 or parent_offset >= count * 40:
                raise ValueError('目录索引的父目录偏移越界或未对齐')
            parent = parent_offset // 40
            if read_i64(self.data, first + parent_offset + 20) != -1:
                raise ValueError('目录索引将普通文件当作父目录')
            if '/' in name or '\\' in name:
                raise ValueError('目录索引的单个名称片段包含路径分隔符')
            normalized_name(name)
            parents.append(parent)
            components.append(name)
        if len(roots) != 1:
            raise ValueError('目录索引必须有且只有一个虚拟根目录')
        paths = {roots[0]: ''}
        for index in range(count):
            # 使用迭代追溯，既能发现循环，也不会受 Python 递归深度限制。
            pending = []
            seen = set()
            current = index
            while current not in paths:
                if current in seen:
                    raise ValueError('目录索引的父目录关系存在循环')
                seen.add(current)
                pending.append(current)
                current = parents[current]
            for child in reversed(pending):
                parent_path = paths[parents[child]]
                path = parent_path + '/' + components[child] if parent_path else components[child]
                normalized_name(path)
                paths[child] = path
        return {first + index * 40: path for index, path in paths.items()}

    def read(self, entry: PakEntry) -> bytes:
        """严格按原生索引里的压缩长度读取，拒绝越界、多余或截断的 zlib 流。"""
        start = entry.data_offset
        end = start + entry.compressed_size
        decompressor = zlib.decompressobj()
        try:
            raw = decompressor.decompress(self.data[start:end])
            raw += decompressor.flush()
        except zlib.error as exc:
            raise ValueError(f'PAK 原生压缩范围解码失败：{entry.name}') from exc
        if (not decompressor.eof or decompressor.unused_data or
                decompressor.unconsumed_tail or len(raw) != entry.size):
            raise ValueError(f'PAK 索引压缩长度或资源解压大小不一致：{entry.name}')
        return raw

    def read_name(self, name: str) -> bytes:
        """通过精确的原生路径读取某个原资源。"""
        entry = self.by_name.get(normalized_name(name))
        if entry is None:
            raise KeyError(name)
        return self.read(entry)


class FilePak(NativePak):
    """文件始终以 rb 打开；映射只读，不把整个大型封包复制到 Python 堆。"""

    def __init__(self, path: Path):
        self._stream = None
        self._mapping = None
        try:
            self._stream = Path(path).open('rb')
            if not self._stream.seek(0, 2):
                raise ValueError('PAK 是空文件')
            self._stream.seek(0)
            self._mapping = mmap.mmap(self._stream.fileno(), 0, access=mmap.ACCESS_READ)
            super().__init__(self._mapping)
        except BaseException:
            self.close()
            raise

    def close(self) -> None:
        if self._mapping is not None:
            self._mapping.close()
            self._mapping = None
        if self._stream is not None:
            self._stream.close()
            self._stream = None

    def __enter__(self) -> 'FilePak':
        return self

    def __exit__(self, *_args) -> None:
        self.close()


def read_text_entries(data: bytes, language: str) -> dict[int, str]:
    """解析 LOC 指定语言的 UTF-16 正文，支持原版文件中的奇数绝对偏移。"""
    if data[:4] != b'OLTX' or len(data) < 0xDC:
        raise ValueError('LOC 不是完整的 OLTX 文件')
    descriptors = []
    for index in range(8):
        entry = 0x1C + 24 * index
        code = data[entry:entry + 2].decode('ascii', errors='strict')
        offset = read_u32(data, entry + 16)
        size = read_u32(data, entry + 20)
        if offset + size > len(data) or offset < 0xDC:
            raise ValueError('LOC 语言块边界异常')
        descriptors.append((code, offset, size))
    matching = [(offset, size) for code, offset, size in descriptors if code == language]
    if len(matching) != 1:
        raise ValueError(f'LOC 未找到唯一的 {language} 语言块')
    offset, size = matching[0]
    end = offset + size
    count, stride = read_u32(data, offset), read_u32(data, offset + 4)
    if stride != 8 or count > (size - 8) // 8:
        raise ValueError('LOC 语言哈希表数量或步长异常')
    text_start = offset + 8 + count * 8
    result = {}
    for i in range(count):
        item = offset + 8 + 8 * i
        key, relative = read_u32(data, item), read_u32(data, item + 4)
        start = offset + relative
        if start < text_start or start >= end or (relative & 1):
            raise ValueError('LOC 文字区域或 UTF-16 相对偏移无效')
        terminator = start
        while terminator + 1 < end and data[terminator:terminator + 2] != b'\x00\x00':
            terminator += 2
        if terminator + 1 >= end:
            raise ValueError('LOC UTF-16 字符串未终止')
        if key in result:
            raise ValueError('LOC 出现重复哈希，可能产生身份歧义')
        result[key] = data[start:terminator].decode('utf-16le', errors='strict')
    return result


def read_key_entries(data: bytes) -> dict[int, str]:
    """取得 LOC 原有的 KeyTable，校验译文没有重算或删除资源键。"""
    if data[:4] != b'OLTX' or len(data) < 0xDC:
        raise ValueError('LOC 不是完整的 OLTX 文件')
    count, stride = read_u32(data, 0xDC), read_u32(data, 0xE0)
    if stride != 8 or count > 100000:
        raise ValueError('LOC KeyTable 字段无效')
    text_start = 0xDC + 8 + count * 8
    out = {}
    for i in range(count):
        item = 0xE4 + i * 8
        h, relative = read_u32(data, item), read_u32(data, item + 4)
        begin = 0xDC + relative
        if begin < text_start or begin >= len(data):
            raise ValueError('LOC KeyTable 字符串偏移越界')
        end = data.find(b'\0', begin)
        if end < 0:
            raise ValueError('LOC KeyTable 字符串缺失结尾')
        if h in out:
            raise ValueError('LOC KeyTable 哈希冲突')
        out[h] = data[begin:end].decode('ascii', errors='strict')
    return out


def verify_loc_pair(original: bytes, localized: bytes, name: str) -> dict:
    """严格验证只修改 EN 译文，不能损坏其它七种语言的内容和键身份。"""
    originals = read_key_entries(original)
    localized_keys = read_key_entries(localized)
    if originals != localized_keys:
        raise ValueError(f'{name}: 译文 KeyTable 不等于原版，禁止封入')
    for lang in ('en', 'fr', 'de', 'it', 'ja', 'pl', 'ru', 'es'):
        a, b = read_text_entries(original, lang), read_text_entries(localized, lang)
        if set(a) != set(originals) or set(b) != set(originals):
            raise ValueError(f'{name}: {lang} 的文本哈希集合不等于原版 KeyTable')
        if lang != 'en' and a != b:
            raise ValueError(f'{name}: 非 EN 语言 {lang} 内容发生改变')
    en = read_text_entries(localized, 'en')
    translated = sum(any('\u3400' <= char <= '\u9fff' for char in value) for value in en.values())
    return {'keys': len(originals), 'en_cjk_entries': translated,
            'original_size': len(original), 'translated_size': len(localized)}



def language_blocks(data: bytes) -> list[tuple[str, int, int]]:
    """按原生语言目录顺序读取八个语言块的绝对起点与长度。"""
    if data[:4] != b'OLTX' or len(data) < 0xDC:
        raise ValueError('LOC 不包含完整的原生语言目录')
    result = []
    for index in range(8):
        item = 0x1C + index * 24
        language = data[item:item + 2].decode('ascii', errors='strict')
        begin, size = read_u32(data, item + 16), read_u32(data, item + 20)
        if not size or begin < 0xDC or begin + size > len(data):
            raise ValueError(f'语言块 {language} 越界或为空')
        result.append((language, begin, size))
    return result


def sort_language_index(block: bytes, label: str) -> tuple[bytes, bool]:
    """只排序语言块首部的 8 字节哈希/相对偏移对，不改动正文任何字节。

    原版语言块按无符号 32 位哈希升序保存；转换工具如果采用
    KeyTable 顺序重建语言块，会破坏二分查找所需的排序不变量。
    相对偏移仍指向当前块中的原始 UTF-16 字节位置，所以排序后
    字符串区与偏移不需要移动。禁止重新编码正文，避免破坏控制标记。
    """
    if len(block) < 8:
        raise ValueError(f'{label} 语言块太短')
    count, stride = struct.unpack_from('<II', block)
    if stride != 8 or count > (len(block) - 8) // 8:
        raise ValueError(f'{label} 索引表越界或条目步长错误')
    entries = [block[8 + i * 8:16 + i * 8] for i in range(count)]
    hashes = [struct.unpack_from('<I', record)[0] for record in entries]
    if len(set(hashes)) != len(hashes):
        raise ValueError(f'{label} 存在哈希重复，不能安全重排')
    ordered = sorted(entries, key=lambda rec: struct.unpack_from('<I', rec)[0])
    result = block[:8] + b''.join(ordered) + block[8 + count * 8:]
    if result[8 + count * 8:] != block[8 + count * 8:]:
        raise AssertionError('索引整理意外改变字符串区')
    return result, hashes != sorted(hashes)


def repair_loc(original: bytes, localized: bytes, name: str) -> tuple[bytes, dict]:
    """复用原版 KeyTable/七个语言块，只换入已经修复排序的中文 EN 块。

    这个策略能同时修复现有 65 个无序 EN 索引，并排除转换器改变
    非 EN 语言、语言目录、字符串填充等其它因素。不会重新翻译任何正文。
    """
    old_keys = read_key_entries(original)
    new_keys = read_key_entries(localized)
    if old_keys != new_keys:
        raise ValueError(f'{name}: 原版与译文 KeyTable 不一致')
    before = language_blocks(original)
    after = language_blocks(localized)
    if [entry[0] for entry in before] != [entry[0] for entry in after]:
        raise ValueError(f'{name}: 语言目录顺序发生变化')
    if before[0][0] != 'en':
        raise ValueError(f'{name}: 当前样本必须使用第一个 EN 语言块')
    original_end = before[-1][1] + before[-1][2]
    if original_end != len(original):
        raise ValueError(f'{name}: 原版语言块后存在未知数据，拒绝重建')
    ordered_original_langs = []
    for lang, start, size in before:
        if lang != 'en':
            untouched, changed = sort_language_index(original[start:start + size], lang)
            if changed or untouched != original[start:start + size]:
                raise ValueError(f'{name}: 原版非 EN 语言索引异常')
            ordered_original_langs.append((lang, untouched))
    trans_en_start, trans_en_size = after[0][1], after[0][2]
    en_block, reordered = sort_language_index(
        localized[trans_en_start:trans_en_start + trans_en_size], 'en')
    # 以原版 EN 之前的所有字节为基础，包括原始 KeyTable 和目录。
    # 后续仅调整目录内块起点/长度，保持所有原版哈希身份不变。
    output = bytearray(original[:before[0][1]])
    assembled = [('en', en_block), *ordered_original_langs]
    if [item[0] for item in assembled] != [item[0] for item in before]:
        raise AssertionError('重建顺序不等于原版目录顺序')
    for index, (lang, block) in enumerate(assembled):
        directory = 0x1C + 24 * index
        struct.pack_into('<II', output, directory + 16, len(output), len(block))
        output += block
    fixed = bytes(output)
    report = verify_loc_pair(original, fixed, name)
    if read_text_entries(fixed, 'en') != read_text_entries(localized, 'en'):
        raise AssertionError(f'{name}: 重建后译文不同于用户提供的 EN 文本')
    for lang, _, _ in before[1:]:
        if read_text_entries(fixed, lang) != read_text_entries(original, lang):
            raise AssertionError(f'{name}: 原始 {lang} 内容未完全还原')
    report['en_index_reordered'] = reordered
    report['input_sha256'] = file_hash(localized)
    report['repaired_sha256'] = file_hash(fixed)
    return fixed, report


def repaired_loc_files(original_data: bytes, zip_path: Path) -> tuple[dict[str, bytes], list[dict]]:
    """一次解析 86 个原版和译文 LOC，生成相同的独立松散修正版。"""
    archive = NativePak(original_data)
    result = {}
    reports = []
    with zipfile.ZipFile(zip_path) as z:
        input_names = [name for name in z.namelist() if name.lower().endswith('.loc')]
        if len(input_names) != 86:
            raise ValueError(f'当前中文基线必须有 86 个 LOC，实际 {len(input_names)}')
        localized = {normalized_name(name): name for name in input_names}
        if len(localized) != 86:
            raise ValueError('中文 LOC 路径存在重复')
        native_names = {name for name in archive.by_name if name.endswith('.loc')}
        if set(localized) != native_names:
            raise ValueError('中文 LOC 清单与原生 PAK 不一致，拒绝部分覆盖')
        for name in sorted(localized):
            original = archive.read_name(name)
            fixed, detail = repair_loc(original, z.read(localized[name]), name)
            result[name] = fixed
            reports.append({'resource': name, **detail,
                            'original_sha256': file_hash(original)})
    return result, reports


