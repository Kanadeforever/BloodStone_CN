# -*- coding: utf-8 -*-
"""针对 Windows GDI 位图长度异常的独立故障回归。"""
from __future__ import annotations

import ctypes
import sys
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2] / 'toolbox/core'
sys.path.insert(0, str(SOURCE))
from raster_gdi import CELL_H, CELL_W, GLYPHMETRICS, WindowsGlyphRenderer


class StubGDI:
    """只模拟字形位图接口，控制“返回长度”与“实际字形边界”。"""

    def __init__(self, width: int, height: int, first: int, returned: int):
        self.width, self.height = width, height
        self.first, self.returned = first, returned

    def GetGlyphOutlineW(self, hdc, cp, mode, metrics_ref, capacity, output, transform):
        metric = ctypes.cast(metrics_ref, ctypes.POINTER(GLYPHMETRICS)).contents
        metric.gmBlackBoxX, metric.gmBlackBoxY = self.width, self.height
        metric.gmptGlyphOrigin.y = 35
        if not capacity:
            return self.first
        ctypes.memmove(output, bytes([32]) * min(self.returned, capacity),
                       min(self.returned, capacity))
        return self.returned


class GdiBufferTests(unittest.TestCase):
    def make_renderer(self, width: int, height: int, first: int, returned: int):
        """绕过 Windows 字体注册，只隔离测试位图边界读取部分。"""
        renderer = object.__new__(WindowsGlyphRenderer)
        renderer.hdc = 1
        renderer.gdi = StubGDI(width, height, first, returned)
        return renderer

    def test_truncated_gray_glyph_is_explicit_error(self):
        # 行距为 8，第二行需要 5 字节，读取前必须明确检查长度。
        r = self.make_renderer(5, 2, 10, 10)
        with self.assertRaisesRegex(ValueError, 'GDI 位图字节不足'):
            r._read_outline(0x4E2D, 6)

    def test_last_row_padding_not_required(self):
        # 只要实际像素完整，最后一行即使没有多余填充也允许。
        r = self.make_renderer(5, 2, 13, 13)
        image = r._read_outline(0x4E2D, 6)
        self.assertEqual(len(image), CELL_W * CELL_H)
        self.assertGreater(sum(image), 0)

    def test_empty_glyph_has_no_buffer_access(self):
        r = self.make_renderer(0, 0, 0, 0)
        self.assertEqual(r._read_outline(0x3000, 6), bytes(CELL_W * CELL_H))

    def test_mono_mode_checks_stride(self):
        r = self.make_renderer(9, 3, 9, 9)
        with self.assertRaisesRegex(ValueError, 'GDI 位图字节不足'):
            r._read_outline(0x4E2D, 1)


if __name__ == '__main__':
    unittest.main()
