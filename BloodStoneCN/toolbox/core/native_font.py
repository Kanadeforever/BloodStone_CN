# -*- coding: utf-8 -*-
"""《007 Blood Stone》原生 EUR 字库的离线读取、扩容和校验。

输入是游戏原有的 DAT/BPR 和已经确认能够显示中文的 BFA。
输出是保持原始三个字体面的 DAT/BPR 实验候选资源。

本模块只处理离线二进制文件，不修改游戏文件，也不注入游戏进程。
所有格式边界都会进行检查，避免把不认识的数据猜测为可写结构。
"""

from __future__ import annotations

import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


# DAT 字体面有固定大小的名称、资源名和属性区，字形表紧随其后。
# 本项目使用的 EUR 原版具有三个字体面，第三个面还存在字距配对尾表。
DAT_FACE_HEADER_SIZE = 296
DAT_GLYPH_RECORD_SIZE = 28
BPR_DESCRIPTOR_SIZE = 28
BPR_HEADER_SIZE = 20
MIP_LEVELS = 3
EXTENDED_WIDTH = 2048
EXTENDED_HEIGHT = 2048


@dataclass
class Face:
    name: str
    resource_name: str
    header: bytes
    glyphs: list[bytes]
    extra: bytes

    @property
    def width(self) -> int:
        return struct.unpack_from('<I', self.header, 0x104)[0]

    @property
    def height(self) -> int:
        return struct.unpack_from('<I', self.header, 0x108)[0]

    @property
    def glyph_count(self) -> int:
        return struct.unpack_from('<I', self.header, 0x10C)[0]

    def codepoints(self) -> list[int]:
        # 记录尾部是 Unicode 的边界索引值，不是图像自身的码点。
        # 原生 EUR 中 0x0041 是字母 A 的查找入口，0x0042 才存放 A 的位图。
        # 若只写入后面的字形记录，前面的查找键缺失，就会显示缺字符号。
        # 返回这些索引供生成器同时校验“入口”与“下一条实际字形”。
        return [struct.unpack_from('<I', item, 24)[0] for item in self.glyphs]


@dataclass
class NativeDat:
    prefix: bytes
    faces: list[Face]

    @classmethod
    def parse(cls, data: bytes) -> 'NativeDat':
        if len(data) < 8 or data[:4] != b'\xff' * 4:
            raise ValueError('DAT 文件标识不正确')
        face_count = struct.unpack_from('<I', data, 4)[0]
        if not 1 <= face_count <= 8:
            raise ValueError('DAT 字体面数量异常')

        pos = 8
        faces = []
        for index in range(face_count):
            if pos + DAT_FACE_HEADER_SIZE > len(data):
                raise ValueError('DAT 字体面头部越界')
            header = data[pos:pos + DAT_FACE_HEADER_SIZE]
            name = header[:128].split(b'\0', 1)[0].decode('ascii')
            resource = header[132:256].split(b'\0', 1)[0].decode('ascii')
            width, height, glyph_count = struct.unpack_from('<III', header, 0x104)
            if not name or not resource or not (0 < width <= 8192) or not (0 < height <= 8192):
                raise ValueError('DAT 字体面名称或纹理尺寸无效')
            if glyph_count > 100000:
                raise ValueError('DAT 字形数量超过安全上限')
            record_start = pos + DAT_FACE_HEADER_SIZE
            record_end = record_start + glyph_count * DAT_GLYPH_RECORD_SIZE
            if record_end > len(data):
                raise ValueError('DAT 字形记录越界')
            records = [data[k:k + DAT_GLYPH_RECORD_SIZE]
                       for k in range(record_start, record_end, DAT_GLYPH_RECORD_SIZE)]
            pos = record_end

            # 前两个 EUR 字体面没有尾表；最后一面存在原生字距配对数据。
            # 字距配对的格式尚未完全解析，因此保留字节而不修改其结构。
            extra = b''
            if index == face_count - 1:
                extra = data[pos:]
                pos = len(data)
            faces.append(Face(name, resource, header, records, extra))

        if pos != len(data):
            raise ValueError('DAT 存在无法解释的尾部内容')
        return cls(data[:8], faces)

    def to_bytes(self) -> bytes:
        out = bytearray(self.prefix)
        for face in self.faces:
            if len(face.header) != DAT_FACE_HEADER_SIZE:
                raise ValueError('DAT 字体面头部长度错误')
            if face.glyph_count != len(face.glyphs):
                raise ValueError('DAT 字形数量与头部不一致')
            out += face.header
            for record in face.glyphs:
                if len(record) != DAT_GLYPH_RECORD_SIZE:
                    raise ValueError('DAT 字形记录长度错误')
                out += record
            out += face.extra
        return bytes(out)


@dataclass
class TextureFace:
    name: str
    descriptor: bytes
    pixels: bytes

    @property
    def width(self) -> int:
        return struct.unpack_from('<H', self.descriptor, 24)[0]

    @property
    def height(self) -> int:
        return struct.unpack_from('<H', self.descriptor, 26)[0]

    @property
    def size(self) -> int:
        return struct.unpack_from('<I', self.descriptor, 12)[0]

    def mip_sizes(self) -> list[int]:
        if self.width % 4 or self.height % 4:
            raise ValueError('BPR DXT1 纹理尺寸必须是四的倍数')
        return [(self.width >> m) * (self.height >> m) // 2 for m in range(MIP_LEVELS)]


@dataclass
class NativeBpr:
    header: bytes
    faces: list[TextureFace]

    @classmethod
    def parse(cls, data: bytes, face_count: int) -> 'NativeBpr':
        if data[:4] != b'2RZB' or len(data) < BPR_HEADER_SIZE + face_count * BPR_DESCRIPTOR_SIZE:
            raise ValueError('BPR 文件头部不正确')
        table_end = BPR_HEADER_SIZE + face_count * BPR_DESCRIPTOR_SIZE
        descriptors = [data[BPR_HEADER_SIZE + i * BPR_DESCRIPTOR_SIZE:
                            BPR_HEADER_SIZE + (i + 1) * BPR_DESCRIPTOR_SIZE]
                       for i in range(face_count)]
        cursor = table_end
        names = []
        for _ in range(face_count):
            end = data.find(b'\0', cursor)
            if end < 0:
                raise ValueError('BPR 字体面名称缺少结束标记')
            names.append(data[cursor:end].decode('ascii'))
            cursor = end + 1
        faces = []
        for name, desc in zip(names, descriptors):
            offset = struct.unpack_from('<I', desc, 4)[0]
            size = struct.unpack_from('<I', desc, 12)[0]
            start = cursor + offset
            end = start + size
            if end > len(data):
                raise ValueError('BPR 纹理数据越界')
            face = TextureFace(name, desc, data[start:end])
            if sum(face.mip_sizes()) != size:
                raise ValueError('BPR 纹理大小与三层 DXT1 Mipmap 不一致')
            faces.append(face)
        if cursor + sum(f.size for f in faces) != len(data):
            raise ValueError('BPR 出现纹理数据缺口或未知尾数据')
        return cls(data[:BPR_HEADER_SIZE], faces)

    def to_bytes(self) -> bytes:
        # 描述符中的数据位置必须随着扩容后的纹理体积重新计算。
        # 保留每个描述符的其他字段，尤其是格式和字体名称引用信息。
        offset = 0
        descs = bytearray()
        for face in self.faces:
            desc = bytearray(face.descriptor)
            if len(desc) != BPR_DESCRIPTOR_SIZE:
                raise ValueError('BPR 描述符长度错误')
            if face.size != len(face.pixels) or face.size != sum(face.mip_sizes()):
                raise ValueError('BPR 字体面纹理数据不完整')
            struct.pack_into('<I', desc, 4, offset)
            descs += desc
            offset += face.size
        header = bytearray(self.header)
        struct.pack_into('<I', header, 12, offset)
        # 所有字体面保持原顺序，所以字体面名称列表也保持原顺序。
        names = b''.join(face.name.encode('ascii') + b'\0' for face in self.faces)
        if len(header) != BPR_HEADER_SIZE:
            raise ValueError('BPR 原始头部长度错误')
        return bytes(header) + descs + names + b''.join(face.pixels for face in self.faces)


@dataclass(frozen=True)
class SourceGlyph:
    codepoint: int
    x: int
    y: int
    width: int
    height: int
    bearing_x: int
    bearing_y: int
    advance: int


@dataclass
class ChineseAtlas:
    width: int
    height: int
    pixels: bytes
    glyphs: dict[int, SourceGlyph]

    @classmethod
    def from_bfa(cls, data: bytes) -> 'ChineseAtlas':
        if data[:8] != b'BSCNATL1' or len(data) < 64:
            raise ValueError('BFA 字库标识不正确')
        width, height, glyph_count, rec_size, px_offset, px_size = (
            struct.unpack_from('<I', data, k)[0] for k in (16, 20, 32, 36, 40, 44))
        if rec_size != 32 or not (0 < width <= 8192 and 0 < height <= 8192):
            raise ValueError('BFA 字形记录或尺寸不正确')
        if px_size != width * height or px_offset + px_size > len(data):
            raise ValueError('BFA 位图数据不完整')
        glyphs = {}
        for i in range(glyph_count):
            cp, token, x, y, w, h, bx, by, advance, flags, unused0, unused1 = (
                struct.unpack_from('<IIHHHHhhhHII', data, 64 + 32 * i))
            if cp in glyphs or x + w > width or y + h > height:
                raise ValueError('BFA 字形坐标重复或越界')
            glyphs[cp] = SourceGlyph(cp, x, y, w, h, bx, by, advance)
        return cls(width, height, data[px_offset:px_offset + px_size], glyphs)


def split_mipmaps(face: TextureFace) -> list[bytes]:
    sizes = face.mip_sizes()
    pos = 0
    result = []
    for size in sizes:
        result.append(face.pixels[pos:pos + size])
        pos += size
    return result


def make_packed_glyphs(items: Iterable[SourceGlyph], texture_size: int = EXTENDED_WIDTH) -> dict[int, tuple[int, int]]:
    # 三个字体面的原图均占据 512×1024 左上角，保留该区域原样。
    # 中文只放入另外两个区域，避免覆盖原生英文字形和特殊图标。
    regions = [(512, 0, texture_size, 1024), (0, 1024, texture_size, texture_size)]
    glyphs = sorted(items, key=lambda g: g.codepoint)
    allocations = {}
    idx = 0
    for left, top, right, bottom in regions:
        x, y, max_h = left, top, 0
        while idx < len(glyphs):
            glyph = glyphs[idx]
            # 当前采用原版字体的紧密排列方式。BFA 自身包含字形边界，
            # 不人为扩大字符盒，否则 2048 图集容纳不下完整字符集。
            padded_w = glyph.width
            padded_h = glyph.height
            if x + padded_w > right:
                x, y, max_h = left, y + max_h, 0
            if y + padded_h > bottom:
                break
            allocations[glyph.codepoint] = (x, y)
            idx += 1
            x += padded_w
            max_h = max(max_h, padded_h)
    if idx != len(glyphs):
        raise ValueError(f'{texture_size}x{texture_size} 无法装下全部中文：已布局 {idx}/{len(glyphs)} 字形')
    return allocations


def build_alpha_layer(bfa: ChineseAtlas,
                      glyphs: list[SourceGlyph],
                      positions: dict[int, tuple[int, int]],
                      texture_size: int = EXTENDED_WIDTH) -> bytes:
    # 新增字形使用已经实机可见的 BFA 8 位透明度数据，不重新栅格化。
    # 这样避免字体版本变化造成译文字形不一致。
    pixels = bytearray(texture_size * texture_size)
    for glyph in glyphs:
        dx, dy = positions[glyph.codepoint]
        for row in range(glyph.height):
            source = (glyph.y + row) * bfa.width + glyph.x
            dest = (dy + row) * texture_size + dx
            pixels[dest:dest + glyph.width] = bfa.pixels[source:source + glyph.width]
    return bytes(pixels)


def downsample_a8(pixels: bytes, width: int, height: int) -> bytes:
    # 对相邻四个透明度求平均，得到游戏原生 BPR 要求的下一层 Mipmap。
    # 不需要 Pillow 或额外的编码库，只有 Python 标准库即可重建。
    output = bytearray((width // 2) * (height // 2))
    for y in range(0, height, 2):
        a = y * width
        b = (y + 1) * width
        dest = (y // 2) * (width // 2)
        for x in range(0, width, 2):
            output[dest + x // 2] = (
                pixels[a + x] + pixels[a + x + 1] +
                pixels[b + x] + pixels[b + x + 1] + 2) // 4
    return bytes(output)


def encode_dxt1_alpha(pixels: bytes, width: int, height: int) -> bytes:
    # BPR 的每个 4×4 块采用标准 DXT1。使用白色与黑色作为端点，
    # 并以两个插值灰度表示汉字边缘，使 Mipmap 仍具有抗锯齿过渡。
    # 原有 EUR 块稍后直接覆盖回去，不会经历二次有损编码。
    out = bytearray(width * height // 2)
    p = 0
    for y in range(0, height, 4):
        for x in range(0, width, 4):
            indices = 0
            shift = 0
            meaningful = False
            for yy in range(4):
                off = (y + yy) * width + x
                for xx in range(4):
                    value = pixels[off + xx]
                    meaningful |= value != 0
                    # DXT1 四色：白、黑、2/3 白、1/3 白。
                    index = 1 if value < 43 else 3 if value < 128 else 2 if value < 213 else 0
                    indices |= index << shift
                    shift += 2
            if meaningful:
                struct.pack_into('<HHI', out, p, 0xFFFF, 0x0000, indices)
            p += 8
    return bytes(out)


def preserve_original_blocks(new_mip: bytearray, original_mip: bytes,
                             level: int, texture_size: int = EXTENDED_WIDTH) -> None:
    # 原图集放在新纹理左上角，宽高分别从 512×1024 按层级缩减。
    # 每个 DXT1 块 8 字节，逐行复制原始压缩块，确保原版英文及图标
    # 的纹理字节完全不变。
    old_w = 512 >> level
    old_h = 1024 >> level
    new_w = texture_size >> level
    blocks_per_row_old = old_w // 4
    blocks_per_row_new = new_w // 4
    for row in range(old_h // 4):
        src = row * blocks_per_row_old * 8
        dst = row * blocks_per_row_new * 8
        length = blocks_per_row_old * 8
        new_mip[dst:dst + length] = original_mip[src:src + length]


def create_native_record(glyph: SourceGlyph, x: int, y: int, texture_size: int = EXTENDED_WIDTH) -> bytes:
    # 原版 DAT 的每条图像记录采用右侧边界值：码点 U+0041 的图像
    # 位于 0x0042 记录。查找能否命中还取决于 0x0041 是否存在索引记录。
    # 若直接把当前汉字码点作为边界，就会使用下一汉字的图像；
    # 若只生成码点+1，又会让没有紧邻前驱字符的汉字显示缺字符号。
    # 因此构建阶段还必须补齐缺失的前置索引记录，不能只调用本函数。
    # 前四个 float 为归一化 UV；后四个 u16 为原生横向度量。
    if glyph.codepoint >= 0x10FFFF:
        raise ValueError('Unicode 最大码点没有可表示的后继边界')
    return struct.pack('<4f4HI',
                       x / texture_size, y / texture_size,
                       (x + glyph.width) / texture_size,
                       (y + glyph.height) / texture_size,
                       max(0, glyph.bearing_x), glyph.width,
                       max(1, glyph.advance), 0,
                       glyph.codepoint + 1)


def make_native_candidate(dat_raw: bytes, bpr_raw: bytes, bfa_raw: bytes | None = None,
                          *, atlas: ChineseAtlas | None = None,
                          texture_size: int = EXTENDED_WIDTH):
    # 常规发行基线使用 2048 图集，保证与已通过实机测试的生成结果一致。
    # 扩展模式可选择 4096 或 8192，但这是全新纹理规格，必须单独实机验证。
    if texture_size not in (2048, 4096, 8192):
        raise ValueError('纹理边长只能为 2048、4096 或 8192')
    if (atlas is None) == (bfa_raw is None):
        raise ValueError('必须且只能提供现成 BFA 或自行栅格化字形图集')
    native_dat = NativeDat.parse(dat_raw)
    native_bpr = NativeBpr.parse(bpr_raw, len(native_dat.faces))
    bfa = atlas if atlas is not None else ChineseAtlas.from_bfa(bfa_raw)
    expected = ['E_Century_Gothic', 'E_Century_Gothic_Bold', 'E_Microgramma']
    if [f.name for f in native_dat.faces] != expected:
        raise ValueError('当前仅支持 EUR 原版三字体面，不允许误改 BA/EA 或未知版本')
    if [f.name for f in native_bpr.faces] != expected:
        raise ValueError('BPR 与 DAT 字体面名称或顺序不一致')
    if native_dat.to_bytes() != dat_raw or native_bpr.to_bytes() != bpr_raw:
        raise ValueError('原版 DAT/BPR 无法精确往返，禁止扩容')

    # 三个字体面均需要支持中文。菜单、提示和加载窗口可能选择不同字体面，
    # 只扩充前两个字体面会让某些 UI 字符继续变成方框。第三字体面附带的
    # 原生字距配对表不参与字形生成，只保留它的原始字节。
    old_boundaries = (set(native_dat.faces[0].codepoints()) |
                      set(native_dat.faces[1].codepoints()))
    # 当前样本的原生索引采用“右边界”语义：boundary=cp+1 才表示 cp 的图像。
    # 为避免覆盖原版英文字形，必须按 cp+1 判断是否已存在。
    # 普通空格与不换行空格继续交由原版字体处理。
    added = [g for cp, g in sorted(bfa.glyphs.items())
             if cp not in (0x20, 0xA0) and cp < 0x10FFFF
             and cp + 1 not in old_boundaries]
    positions = make_packed_glyphs(added, texture_size)
    source_alpha = build_alpha_layer(bfa, added, positions, texture_size)

    mip_alphas = [source_alpha]
    for level in range(1, MIP_LEVELS):
        mip_alphas.append(downsample_a8(mip_alphas[-1],
                                        texture_size >> (level - 1),
                                        texture_size >> (level - 1)))
    encoded = [encode_dxt1_alpha(a, texture_size >> i, texture_size >> i)
               for i, a in enumerate(mip_alphas)]

    # 第三字体面尾部的字距配对数组为独立数据。最后一条 28 字节记录的
    # Unicode 字段在原版里等于配对项数量（588），它不是可绘制字形。
    # 只移动它的位置、不改字段内容，才能保持原有西文间距与 UI 布局。
    third_pair_data = bytes(native_dat.faces[2].extra)
    third_pair_footer = bytes(native_dat.faces[2].glyphs[-1])
    if len(third_pair_data) != 3528 or len(third_pair_data) % 6:
        raise ValueError('第三字体面的字距配对长度与已确认的 EUR 基线不同')
    if struct.unpack_from('<I', third_pair_footer, 24)[0] != len(third_pair_data) // 6:
        raise ValueError('第三字体面字距配对尾记录不匹配，禁止改写未知结构')

    # 所有字体面共用同一份新增中文字符及布局，因此不需要额外扩大图集。
    # 三个字体面各自保留西文原图，每个面都获得同一套中文纹理与索引。
    inserted_markers_by_face = []
    for face_index in range(3):
        face = native_dat.faces[face_index]
        texture = native_bpr.faces[face_index]
        old_mips = split_mipmaps(texture)
        native_mips = []
        for level, alpha_mip in enumerate(encoded):
            buff = bytearray(alpha_mip)
            preserve_original_blocks(buff, old_mips[level], level, texture_size)
            native_mips.append(bytes(buff))
        texture.pixels = b''.join(native_mips)
        desc = bytearray(texture.descriptor)
        struct.pack_into('<I', desc, 12, len(texture.pixels))
        struct.pack_into('<HH', desc, 24, texture_size, texture_size)
        texture.descriptor = bytes(desc)

        # 原字体面的末尾可能带有特殊哨兵；保留其原始字节和顺序，
        # 只将 Unicode 汉字插入正常字形之后，避免改变原有 glyph owner。
        # 第三字体面的末条非字形记录用于说明字距配对表长度，
        # 必须保留在所有 Unicode 索引和空记录之后，不能混入升序字形表。
        footer = [face.glyphs[-1]] if face_index == 2 else []
        glyph_source = face.glyphs[:-1] if face_index == 2 else face.glyphs
        originals = [r for r in glyph_source if struct.unpack_from('<I', r, 24)[0] != 0]
        sentinels = [r for r in glyph_source if struct.unpack_from('<I', r, 24)[0] == 0]
        scaled = []
        for rec in originals:
            item = bytearray(rec)
            u0, v0, u1, v1 = struct.unpack_from('<4f', item, 0)
            struct.pack_into('<4f', item, 0,
                             u0 * 512 / texture_size, v0 * 1024 / texture_size,
                             u1 * 512 / texture_size, v1 * 1024 / texture_size)
            scaled.append(bytes(item))
        new_entries = [create_native_record(g, *positions[g.codepoint], texture_size) for g in added]
        # 查找流程需要“查询码点本身”和“后一条包含图像的记录”同时存在。
        # 对于连续字符，例如“八”后紧挨着“公”，前一个字形的右边界
        # 自然充当后一个字符的查找索引，不必再插入记录。
        # 但“血”的前一码点没有字形时，0x8840 索引缺席，原生查找
        # 就会返回缺字符号，尽管 0x8841 处已经存储了“血”的图像。
        # 这里补一个只用于索引的记录；其图像复制原生缺字占位图，
        # 避免为并不存在于译文里的“前一码点”虚构一幅中文图像。
        existing_boundaries = {struct.unpack_from('<I', item, 24)[0]
                               for item in scaled + new_entries}
        fallback = bytearray(scaled[0])
        marker_entries = []
        for glyph in added:
            if glyph.codepoint not in existing_boundaries:
                marker = bytearray(fallback)
                struct.pack_into('<I', marker, 24, glyph.codepoint)
                marker_entries.append(bytes(marker))
                existing_boundaries.add(glyph.codepoint)
        # 原始零值尾记录仍留在末尾，既不纳入正序索引，也不重写图像。
        merged = sorted(scaled + new_entries + marker_entries,
                        key=lambda rec: struct.unpack_from('<I', rec, 24)[0])
        inserted_markers_by_face.append(len(marker_entries))
        face.glyphs = merged + sentinels + footer
        hdr = bytearray(face.header)
        struct.pack_into('<III', hdr, 0x104, texture_size, texture_size, len(face.glyphs))
        face.header = bytes(hdr)

    result_dat = native_dat.to_bytes()
    result_bpr = native_bpr.to_bytes()
    check_dat = NativeDat.parse(result_dat)
    check_bpr = NativeBpr.parse(result_bpr, 3)
    if result_dat != check_dat.to_bytes() or result_bpr != check_bpr.to_bytes():
        raise AssertionError('生成后的原生资源不能精确往返')
    third = check_dat.faces[2]
    if third.extra != third_pair_data or third.glyphs[-1] != third_pair_footer:
        raise AssertionError('第三字体面原生字距配对数据或尾部记录改变')
    for face_index in range(3):
        generated = split_mipmaps(check_bpr.faces[face_index])
        original = split_mipmaps(NativeBpr.parse(bpr_raw, 3).faces[face_index])
        for level in range(MIP_LEVELS):
            old_w, old_h = 512 >> level, 1024 >> level
            new_w = texture_size >> level
            for row in range(old_h // 4):
                src = row * (old_w // 4) * 8
                dst = row * (new_w // 4) * 8
                size = (old_w // 4) * 8
                if generated[level][dst:dst + size] != original[level][src:src + size]:
                    raise AssertionError('旧西文字形 DXT1 块被修改')

    manifest = {
        'status': '离线候选，尚未经过游戏实机验证',
        'mode': 'EN / EUR 三字体面全部扩充 / 右边界字形 + 前置码点索引',
        'texture_size': texture_size,
        'runtime_validation': '2048 规格已实机通过；4096 和 8192 规格尚未实机确认',
        'glyph_index_rule': 'lookup_key = codepoint; glyph_record_boundary = codepoint + 1',
        'inserted_index_markers_by_face': inserted_markers_by_face,
        'added_codepoints': len(added),
        'bfa_glyph_total': len(bfa.glyphs),
        'original_face_names': expected,
        'faces': [{'name': f.name, 'width': f.width, 'height': f.height,
                   'glyph_records': len(f.glyphs), 'suffix_bytes': len(f.extra)}
                  for f in check_dat.faces],
        'original_third_kerning_data_unchanged': True,
        'original_third_kerning_footer_unchanged': True,
        'all_three_faces_extended': True,
        'original_english_dxt1_blocks_unchanged': True,
        'sha256': {
            'original_dat': hashlib.sha256(dat_raw).hexdigest(),
            'original_bpr': hashlib.sha256(bpr_raw).hexdigest(),
            'generated_dat': hashlib.sha256(result_dat).hexdigest(),
            'generated_bpr': hashlib.sha256(result_bpr).hexdigest(),
        },
        'known_unverified': [
            '第三字体面原生配对表虽逐字节保持不变，仍需实机确认扩容后的查找行为',
            '已通过的 v0.4 字体面 0/1 应与新字体面 2 在同一场景进行对照',
            '小型 UI 提示是否使用 E_Microgramma 尚需实机测试确认',
        ],
    }
    return result_dat, result_bpr, manifest


def write_candidate(dat_path: Path, bpr_path: Path, bfa_path: Path,
                    output_directory: Path) -> dict:
    generated_dat, generated_bpr, report = make_native_candidate(
        dat_path.read_bytes(), bpr_path.read_bytes(), bfa_path.read_bytes())
    output_directory.mkdir(parents=True, exist_ok=True)
    (output_directory / 'ui_font_eur.dat').write_bytes(generated_dat)
    (output_directory / 'ui_font_eur.bpr').write_bytes(generated_bpr)
    (output_directory / '生成验证报告.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\r\n')
    return report
