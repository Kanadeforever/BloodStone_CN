# -*- coding: utf-8 -*-
"""一个入口运行公开工具、运行时签名以及 GDI 缓冲回归；不下载任何依赖。"""
from __future__ import annotations
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
(SOURCE.parent/'.build').mkdir(exist_ok=True)
suite = unittest.TestSuite()
for directory in ('tests',):
    suite.addTests(unittest.TestLoader().discover(str(SOURCE/directory), pattern='test_*.py'))
result = unittest.TextTestRunner(verbosity=2).run(suite)
raise SystemExit(0 if result.wasSuccessful() else 1)
