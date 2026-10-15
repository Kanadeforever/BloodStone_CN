# -*- coding: utf-8 -*-
"""使用 Windows 自带 GDI 从 TTF/OTF/TTC 读取真实字形，不需要外部 Python 包。

输入字体通过 AddFontResourceExW 仅加载到当前进程，使用完立即卸载。
用户字体始终只读，整个过程不需要安装到系统字体目录。
"""
from __future__ import annotations

import ctypes
import struct
import sys
from pathlib import Path

from ctypes import wintypes

# 字形坐标按已实机通过的 BFA 固定盒宽高 36×44 对齐。
CELL_W, CELL_H = 36, 44


def _font_names_and_table(path: Path) -> tuple[list[str], bytes]:
    """从字体自身取得全部可用的族名，不能假设最后一个本地化名称就是 GDI 名称。

    OpenType 的 name ID 1 是传统字体族名，ID 16 是推荐字体族名。
    Windows 可能只使用其中之一；中英文名称也可能不相同，因此必须
    分别尝试，并使用 GDI 实际选中的 name 表确认不是宋体等替代字体。
    TTC 可能有多个子字体，优先选择简体中文 SC 子字体。
    """
    data = path.read_bytes()
    if data[:4] == b'ttcf':
        if len(data) < 16:
            raise ValueError('TTC 文件头部不完整')
        face_count = struct.unpack_from('>I', data, 8)[0]
        if not 1 <= face_count <= 128 or 12 + face_count * 4 > len(data):
            raise ValueError('TTC 子字体索引无效')
        face_offsets = [struct.unpack_from('>I', data, 12 + i * 4)[0]
                        for i in range(face_count)]
    else:
        face_offsets = [0]

    faces = []
    for face_offset in face_offsets:
        if (face_offset + 12 > len(data) or
                data[face_offset:face_offset + 4] not in (b'\0\1\0\0', b'OTTO', b'true')):
            raise ValueError('字体不是有效的 TTF、OTF 或 TTC')
        count = struct.unpack_from('>H', data, face_offset + 4)[0]
        if count > (len(data) - face_offset - 12) // 16:
            raise ValueError('字体表目录超出文件范围')
        name_start = name_length = None
        for i in range(count):
            offset = face_offset + 12 + i * 16
            tag, _, table_offset, length = struct.unpack_from('>4sIII', data, offset)
            if tag == b'name':
                name_start, name_length = table_offset, length
                break
        if name_start is None or name_start + name_length > len(data) or name_length < 6:
            raise ValueError('字体缺少完整的 name 表')
        name_table = data[name_start:name_start + name_length]
        rows = struct.unpack_from('>H', name_table, 2)[0]
        storage = struct.unpack_from('>H', name_table, 4)[0]
        if 6 + rows * 12 > len(name_table) or storage > len(name_table):
            raise ValueError('字体名称记录超出 name 表范围')
        records = []
        for i in range(rows):
            platform, encoding, language, name_id, size, offset = struct.unpack_from(
                '>6H', name_table, 6 + 12 * i)
            if name_id not in (1, 4, 6, 16) or storage + offset + size > len(name_table):
                continue
            raw = name_table[storage + offset:storage + offset + size]
            try:
                value = raw.decode('utf-16-be') if platform in (0, 3) else raw.decode('mac_roman')
            except UnicodeError:
                continue
            value = value.strip('\0 ').strip()
            if not value or len(value) >= 32:
                continue
            # 优先尝试 Windows 英文族名，然后是 Unicode/其他本地化族名。
            # 全名、PostScript 名仅作为最后的候选，避免优先选中错误别名。
            priority = (0 if name_id == 1 else 1 if name_id == 16 else 2 if name_id == 4 else 3,
                        0 if platform == 3 and language == 0x409 else
                        1 if platform == 0 else 2 if language in (0x804, 0x404) else 3)
            records.append((priority, value))
        unique = []
        for _, value in sorted(records, key=lambda item: item[0]):
            if value.casefold() not in {old.casefold() for old in unique}:
                unique.append(value)
        if not unique:
            raise ValueError('字体中没有可供 GDI 使用的族名')
        faces.append((unique, name_table))
    # TTC 合集中的首个子字体可能是 JP/KR，不能错误地生成简体汉字字形。
    selected = next((face for face in faces
                     if any('cjk sc' in name.casefold() or
                            'hans' in name.casefold() or
                            '黑体 sc' in name.casefold() for name in face[0])), None)
    return selected if selected is not None else faces[0]


def read_font_family(path: Path) -> str:
    """兼容调用者：返回第一个 Windows 优先字体族名。"""
    return _font_names_and_table(path)[0][0]


class POINT(ctypes.Structure):
    _fields_ = [('x', wintypes.LONG), ('y', wintypes.LONG)]


class GLYPHMETRICS(ctypes.Structure):
    _fields_ = [('gmBlackBoxX', wintypes.UINT), ('gmBlackBoxY', wintypes.UINT),
                ('gmptGlyphOrigin', POINT), ('gmCellIncX', wintypes.SHORT),
                ('gmCellIncY', wintypes.SHORT)]


class FIXED(ctypes.Structure):
    _fields_ = [('fract', wintypes.WORD), ('value', ctypes.c_short)]


class MAT2(ctypes.Structure):
    _fields_ = [('eM11', FIXED), ('eM12', FIXED), ('eM21', FIXED), ('eM22', FIXED)]


class WindowsGlyphRenderer:
    """将轮廓字体栅格化为与既有 BFA 相同的 36×44 灰度字形。"""
    def __init__(self, font_path: Path, pixel_size: int = 32):
        if sys.platform != 'win32':
            raise RuntimeError('TTF 栅格化需要 Windows GDI；非 Windows 可使用现成 BFA 模式')
        self.path = font_path
        self.gdi = ctypes.WinDLL('gdi32', use_last_error=True)
        self.gdi.AddFontResourceExW.argtypes = [wintypes.LPCWSTR,wintypes.DWORD,ctypes.c_void_p]
        self.gdi.AddFontResourceExW.restype = ctypes.c_int
        self.gdi.RemoveFontResourceExW.argtypes = [wintypes.LPCWSTR,wintypes.DWORD,ctypes.c_void_p]
        self.gdi.RemoveFontResourceExW.restype = wintypes.BOOL
        self.gdi.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
        self.gdi.CreateCompatibleDC.restype = ctypes.c_void_p
        self.gdi.DeleteDC.argtypes = [ctypes.c_void_p]
        self.gdi.DeleteDC.restype = wintypes.BOOL
        self.gdi.CreateFontW.argtypes = [ctypes.c_int]*5 + [wintypes.DWORD]*8 + [wintypes.LPCWSTR]
        self.gdi.CreateFontW.restype = ctypes.c_void_p
        self.gdi.SelectObject.argtypes = [ctypes.c_void_p,ctypes.c_void_p]
        self.gdi.SelectObject.restype = ctypes.c_void_p
        self.gdi.DeleteObject.argtypes = [ctypes.c_void_p]
        self.gdi.DeleteObject.restype = wintypes.BOOL
        self.gdi.GetTextFaceW.argtypes = [ctypes.c_void_p,ctypes.c_int,wintypes.LPWSTR]
        self.gdi.GetTextFaceW.restype = ctypes.c_int
        self.gdi.GetGlyphIndicesW.argtypes = [ctypes.c_void_p,wintypes.LPCWSTR,ctypes.c_int,
                                              ctypes.POINTER(wintypes.WORD),wintypes.DWORD]
        self.gdi.GetGlyphIndicesW.restype = wintypes.DWORD
        self.gdi.GetGlyphOutlineW.argtypes = [ctypes.c_void_p,wintypes.UINT,wintypes.UINT,
                                               ctypes.POINTER(GLYPHMETRICS),wintypes.DWORD,
                                               ctypes.c_void_p,ctypes.POINTER(MAT2)]
        self.gdi.GetGlyphOutlineW.restype = wintypes.DWORD
        self.names, self.expected_name_table = _font_names_and_table(font_path)
        self.name = self.names[0]
        self.gdi.GetFontData.argtypes = [ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                         ctypes.c_void_p, wintypes.DWORD]
        self.gdi.GetFontData.restype = wintypes.DWORD
        self.loaded = False
        self.hdc = None
        self.font = None
        self.previous = None
        try:
            # FR_PRIVATE=0x10 仅影响当前进程；不将字体安装到系统目录。
            if not self.gdi.AddFontResourceExW(str(font_path), 0x10, None):
                raise OSError('GDI 无法注册输入字体，请确认 OTF/TTF/TTC 文件完整')
            self.loaded = True
            self.hdc = self.gdi.CreateCompatibleDC(None)
            if not self.hdc:
                raise OSError('创建字体渲染上下文失败')
            # Windows 的 GDI 可能识别英文名而不识别 name 表中的中文别名。
            # 尝试全部真实字体名，每次都检查 GDI 选中的原始 name 表。
            errors = []
            for candidate in self.names:
                created = self.gdi.CreateFontW(-pixel_size, 0, 0, 0, 400,
                                               0, 0, 0, 1, 0, 0, 5, 0, candidate)
                if not created:
                    errors.append(f'{candidate}：CreateFontW 失败')
                    continue
                old = self.gdi.SelectObject(self.hdc, created)
                if not old:
                    self.gdi.DeleteObject(created)
                    errors.append(f'{candidate}：SelectObject 失败')
                    continue
                if self.previous is None:
                    self.previous = old
                family = ctypes.create_unicode_buffer(256)
                reported = (family.value if self.gdi.GetTextFaceW(self.hdc, len(family), family)
                            else '未知字体')
                # SFNT name 表来自输入字体自身，若 GDI 落到“宋体”，内容必不同。
                # 只要表一致，即使 GDI 返回的是同一字体的中文别名也允许。
                table_tag = int.from_bytes(b'name', 'little')
                actual_size = self.gdi.GetFontData(self.hdc, table_tag, 0, None, 0)
                verified = False
                if actual_size == len(self.expected_name_table):
                    actual = ctypes.create_string_buffer(actual_size)
                    read = self.gdi.GetFontData(self.hdc, table_tag, 0, actual, actual_size)
                    verified = (read == actual_size and
                                actual.raw == self.expected_name_table)
                # Windows 文档限定 GetFontData 面向 TrueType，部分 CFF OTF
                # 无法查询 name 表。仅当它明确返回 GDI_ERROR，且 GetTextFace
                # 与输入字体中的真实别名一致时，才使用已注册的私有字体。
                # 若能读到 name 表但内容不同，则无论名称多相似都拒绝。
                unavailable = (actual_size == 0xFFFFFFFF)
                aliases = {value.casefold() for value in self.names}
                matched_name = reported.casefold() in aliases
                if verified or (unavailable and matched_name):
                    self.font = created
                    self.name = reported
                    break
                # 下一次尝试前恢复原字体；否则被选中的句柄无法删除。
                self.gdi.SelectObject(self.hdc, self.previous)
                self.gdi.DeleteObject(created)
                errors.append(f'{candidate}→{reported}（字体数据不匹配）')
            if self.font is None:
                detail = '；'.join(errors[:8])
                raise ValueError('Windows 未按输入文件加载字体；已尝试族名：' +
                                 '、'.join(self.names) + '。实际结果：' + detail)

        except Exception:
            self.close()
            raise

    def close(self):
        """总是恢复 GDI 状态并卸载当前进程的私有字体。"""
        if self.hdc and self.previous: self.gdi.SelectObject(self.hdc,self.previous)
        if self.font: self.gdi.DeleteObject(self.font)
        if self.hdc: self.gdi.DeleteDC(self.hdc)
        if self.loaded: self.gdi.RemoveFontResourceExW(str(self.path),0x10,None)
        self.font=self.hdc=self.previous=None
        self.loaded=False

    def __enter__(self): return self
    def __exit__(self,*args): self.close()

    def _read_outline(self, cp: int, mode: int) -> bytes:
        """读取一种 GDI 字形位图，并根据实际返回长度校验每一行。

        GDI 返回的长度不一定等于“宽×高”：灰度行需要按四字节对齐，
        单色位图还需要把八个像素压在一个字节内。因此必须分别计算
        行距和最后一行真正需要的字节数，验证成功后才允许读取。
        """
        metrics = GLYPHMETRICS()
        # 这是单位变换矩阵，表示按输入字体原始大小取字形，不额外扭曲。
        transform = MAT2(FIXED(0, 1), FIXED(0, 0), FIXED(0, 0), FIXED(0, 1))
        first_size = self.gdi.GetGlyphOutlineW(
            self.hdc, cp, mode, ctypes.byref(metrics), 0, None,
            ctypes.byref(transform))
        if first_size == 0xFFFFFFFF:
            raise ValueError(f'GDI 位图长度查询失败（模式 {mode}）')

        # 字格固定为 36×44。先限定返回长度，防止异常的 GDI 数据引起
        # 几百兆内存分配；一次字形只需要约几千字节。
        limit = (CELL_W + 3) * (CELL_H + 3) * 4
        if first_size > limit:
            raise ValueError(f'GDI 返回异常大的字形位图：{first_size} 字节')
        buf = ctypes.create_string_buffer(max(first_size, 1))
        returned = self.gdi.GetGlyphOutlineW(
            self.hdc, cp, mode, ctypes.byref(metrics), first_size, buf,
            ctypes.byref(transform))
        if returned == 0xFFFFFFFF:
            raise ValueError(f'GDI 位图读取失败（模式 {mode}）')

        width = int(metrics.gmBlackBoxX)
        height = int(metrics.gmBlackBoxY)
        if width > CELL_W or height > CELL_H:
            raise ValueError(f'U+{cp:04X} 字形超出 {CELL_W}×{CELL_H} 单字格，请换更小字号')
        if returned > first_size:
            raise ValueError(f'GDI 位图返回长度 {returned} 超过分配长度 {first_size}')

        canvas = bytearray(CELL_W * CELL_H)
        # 一些合法字符（例如全角空格）确实没有可见墨迹。
        # 只有字形边界为空时才允许输出空白；数据丢失不能伪装成空白。
        if width == 0 or height == 0:
            return bytes(canvas)
        if returned == 0:
            raise ValueError(f'GDI 报告有字形边界，但没有返回位图数据（模式 {mode}）')

        if mode in (6, 5):
            # 灰度模式每像素一字节，行宽向上对齐到四字节。
            stride = (width + 3) & ~3
            row_bytes = width
        elif mode == 1:
            # 单色模式每字节代表八个像素，仍然需要四字节行对齐。
            row_bytes = (width + 7) // 8
            stride = (row_bytes + 3) & ~3
        else:
            raise ValueError(f'未知 GDI 位图模式：{mode}')

        # 最后一行的对齐填充可不存在，但所有真实像素必须完整。
        needed = (height - 1) * stride + row_bytes
        if returned < needed:
            raise ValueError(
                f'GDI 位图字节不足：模式={mode}，尺寸={width}×{height}，'
                f'行距={stride}，至少需要={needed}，实际返回={returned}')

        x0 = (CELL_W - width) // 2
        y0 = max(0, 35 - int(metrics.gmptGlyphOrigin.y))
        if y0 + height > CELL_H:
            raise ValueError(f'U+{cp:04X} 字形底部越界，请换更小字号')

        raw = memoryview(buf.raw)[:returned]
        for y in range(height):
            input_row = y * stride
            output_row = (y + y0) * CELL_W + x0
            for x in range(width):
                if mode == 1:
                    # 单色位图从每个字节的最高位开始排列八个像素。
                    value = (raw[input_row + (x // 8)] >> (7 - x % 8)) & 1
                    alpha = 255 if value else 0
                else:
                    # GDI 灰度等级分别为 0～64 和 0～16。
                    level = raw[input_row + x]
                    alpha = min(255, level * (4 if mode == 6 else 16))
                canvas[output_row + x] = alpha
        return bytes(canvas)

    def render(self, cp: int) -> bytes:
        """读取真实字形；位图模式失败时降级，不允许越界或生成伪字形。

        优先采用原本的 64 级灰度，避免改变已验证的中文字形。
        遇到 GDI 返回长度不足、抗锯齿接口不可用等情况，才依次
        尝试 16 级灰度和单色位图。三种模式都失败就报告具体字符。
        """
        if not 0 < cp < 0x10000:
            raise ValueError('此原生字体模式只支持 BMP Unicode 字符')
        glyph_index = wintypes.WORD()
        result = self.gdi.GetGlyphIndicesW(
            self.hdc, chr(cp), 1, ctypes.byref(glyph_index), 1)
        if result == 0xFFFFFFFF:
            raise OSError(f'GDI 字形索引查询失败：U+{cp:04X}')
        if glyph_index.value == 0xFFFF:
            raise ValueError(f'字体缺少 U+{cp:04X} 字形')

        errors = []
        for mode in (6, 5, 1):
            try:
                image = self._read_outline(cp, mode)
                if mode != 6:
                    self.fallback_count = getattr(self, 'fallback_count', 0) + 1
                    if self.fallback_count <= 5:
                        print(f'[字形兼容] U+{cp:04X} 使用 GDI 模式 {mode} 读取，'
                              '灰度模式未提供完整数据', flush=True)
                return image
            except ValueError as exc:
                # 字形尺寸不合适不是 GDI 位图模式的问题；降级也不能修复
                # 超出单字格的字符，因此立即报告，提示用户调整字号。
                if '单字格' in str(exc) or '字形底部越界' in str(exc):
                    raise
                errors.append(f'{mode}: {exc}')
        raise ValueError(f'U+{cp:04X} 三种 GDI 位图模式全部失败：' + '；'.join(errors))
