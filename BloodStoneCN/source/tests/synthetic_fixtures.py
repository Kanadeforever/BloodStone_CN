# -*- coding: utf-8 -*-
"""用程序生成虚构资源用于格式回归；不包含从游戏提取的文本、图像或字形。"""
from __future__ import annotations
import struct
import zlib
from pathlib import Path

FACE_NAMES = ['E_Century_Gothic', 'E_Century_Gothic_Bold', 'E_Microgramma']
NAME_KEY = b'VXo40j3@$%\\%`x'
LANGS = ('en', 'fr', 'de', 'it', 'ja', 'pl', 'ru', 'es')


def align(number: int) -> int:
    return (number+511)//512*512


def make_pak(resources: dict[str, bytes]) -> bytes:
    names = bytearray()
    offsets = {}
    for name in resources:
        offsets[name] = len(names)
        names += name.encode('ascii')+b'\0'
    encrypted = bytes(value ^ NAME_KEY[i % len(NAME_KEY)] for i, value in enumerate(names))
    table = align(516+len(encrypted))
    output = bytearray(align(table+12+40*len(resources)))
    output[:4] = b'2KAP'
    struct.pack_into('<II', output, 4, 2, 512)
    struct.pack_into('<I', output, 512, len(encrypted))
    output[516:516+len(encrypted)] = encrypted
    struct.pack_into('<I', output, table, len(resources))
    for i, (name, raw) in enumerate(resources.items()):
        start = table+12+40*i
        packed = zlib.compress(raw)
        struct.pack_into('<I', output, start, offsets[name])
        struct.pack_into('<I', output, start+8, len(raw))
        struct.pack_into('<q', output, start+20, len(output)//512)
        struct.pack_into('<II', output, start+32, len(packed), 1)
        output += packed
        output += bytes(align(len(output))-len(output))
    return bytes(output)


def make_loc(count: int) -> bytes:
    output = bytearray(0xDC)
    output[:4] = b'OLTX'
    keys = bytearray(struct.pack('<II', count, 8))+bytearray(count*8)
    for i in reversed(range(count)):
        key = f'SYNTHETIC_{i:04}'.encode('ascii')+b'\0'
        struct.pack_into('<II', keys, 8+8*(count-1-i), i+1, len(keys))
        keys += key
    output += keys
    for index, language in enumerate(LANGS):
        block = bytearray(struct.pack('<II', count, 8))+bytearray(count*8)
        for i in range(count):
            text = f'{language} synthetic entry {i}'.encode('utf-16le')+b'\0\0'
            struct.pack_into('<II', block, 8+8*i, i+1, len(block))
            block += text
        item = 0x1C+24*index
        output[item:item+2] = language.encode('ascii')
        struct.pack_into('<II', output, item+16, len(output), len(block))
        output += block
    return bytes(output)


def make_tree_pak(resources: dict[str, bytes]) -> bytes:
    """生成虚构目录索引封包，父目录故意放在子项后面以覆盖前向引用。"""
    nodes = {'': None}
    for path, payload in resources.items():
        parts = path.split('/')
        for length in range(1, len(parts)):
            nodes.setdefault('/'.join(parts[:length]), None)
        nodes[path] = payload
    ordered = list(reversed(list(nodes.items())))
    indices = {path: index for index, (path, _) in enumerate(ordered)}
    names = bytearray(b'\0')
    offsets = {}
    for path, _ in ordered:
        label = path.split('/')[-1] if path else '\\'
        if label not in offsets:
            offsets[label] = len(names)
            names += label.encode('utf-8') + b'\0'
    encrypted = bytes(value ^ NAME_KEY[i % len(NAME_KEY)] for i, value in enumerate(names))
    table = align(516 + len(encrypted))
    output = bytearray(align(table + 12 + 40 * len(ordered)))
    output[:4] = b'2KAP'
    struct.pack_into('<II', output, 4, 3, 512)
    struct.pack_into('<I', output, 512, len(encrypted))
    output[516:516 + len(encrypted)] = encrypted
    struct.pack_into('<II', output, table, len(ordered), len(ordered) * 40)
    for index, (path, payload) in enumerate(ordered):
        record = table + 12 + index * 40
        label = path.split('/')[-1] if path else '\\'
        parent_path = path.rpartition('/')[0]
        parent_offset = indices[parent_path] * 40 if path else 0xFFFFFFFF
        struct.pack_into('<II', output, record, offsets[label], parent_offset)
        struct.pack_into('<q', output, record + 20, -1)
        if payload is not None:
            compressed = zlib.compress(payload)
            struct.pack_into('<I', output, record + 8, len(payload))
            struct.pack_into('<q', output, record + 20, len(output) // 512)
            struct.pack_into('<I', output, record + 32, len(compressed))
            output += compressed
            output += bytes(align(len(output)) - len(output))
        # 末尾字段保留零，确保读取器不会把它误当作可变记录的扩展标记。
    return bytes(output)


def make_templates() -> tuple[bytes, bytes]:
    dat = bytearray(struct.pack('<II', 0xFFFFFFFF, 3))
    headers = []
    bpr_names = bytearray()
    pixels = bytearray()
    for index, name in enumerate(FACE_NAMES):
        hdr = bytearray(296)
        hdr[:len(name)] = name.encode('ascii')
        resource = 'synthetic_font'
        hdr[132:132+len(resource)] = resource.encode('ascii')
        glyphs = [struct.pack('<4f4HI', 0., 0., 1/512, 1/1024, 0, 1, 1, 0, cp)
                  for cp in [*range(32, 257), 0]]
        if index == 2:
            glyphs += [bytes(24)+struct.pack('<I', 588)]
        struct.pack_into('<III', hdr, 0x104, 512, 1024, len(glyphs))
        dat += hdr+b''.join(glyphs)
        if index == 2:
            dat += bytes(3528)
        desc = bytearray(28)
        image = b''.join(struct.pack('<HHI', 0xFFFF, index, 0x12345678+level)
                         * ((512 >> level)*(1024 >> level)//16) for level in range(3))
        struct.pack_into('<I', desc, 4, len(pixels))
        struct.pack_into('<I', desc, 12, len(image))
        struct.pack_into('<HH', desc, 24, 512, 1024)
        headers.append(bytes(desc))
        pixels += image
        bpr_names += name.encode('ascii')+b'\0'
    header = bytearray(20)
    header[:4] = b'2RZB'
    struct.pack_into('<I', header, 12, len(pixels))
    return bytes(dat), bytes(header)+b''.join(headers)+bytes(bpr_names)+bytes(pixels)


def make_bfa(codepoint: int = 0x4E2D) -> bytes:
    header = bytearray(64)
    header[:8] = b'BSCNATL1'
    for offset, value in ((16,36), (20,44), (32,1), (36,32), (40,96), (44,36*44)):
        struct.pack_into('<I', header, offset, value)
    glyph = struct.pack('<IIHHHHhhhHII', codepoint, 0, 0, 0, 36, 44, 0, 28, 32, 0, 0, 0)
    # 虚构的条纹矩形，既不是字体字形，也不是任何游戏纹理。
    return bytes(header)+glyph+bytes(255 if x%7 == 0 else 0 for _ in range(44) for x in range(36))


def make_game(directory: Path) -> None:
    cache = directory/'cache'
    cache.mkdir(parents=True)
    resources = {('gamedata/xt/loc/common.loc' if i == 0 else
                  f'gamedata/xt/loc/synthetic_{i:02}.loc'): make_loc(93 if i < 63 else 92)
                 for i in range(86)}
    (cache/'gamedata.pak').write_bytes(make_pak({'\\'+name.replace('/', '\\'): raw for name, raw in resources.items()}))
    dat, bpr = make_templates()
    (cache/'root.pak').write_bytes(make_tree_pak({'Ui/Data/ui_font_eur.dat': dat,
                                          'Ui/Data/ui_font_eur.bpr': bpr,
                                          'synthetic/unrelated.bin': b'NOT A GAME ASSET'}))
