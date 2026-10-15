# -*- coding: utf-8 -*-
"""检查独立工具箱并补齐空工作目录，不再复制另一套 Python 程序。"""
from __future__ import annotations
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EMPTY_DIRS = ('input/fonts', 'input/template', 'translations')


def main() -> None:
    """公开包不生成资产；已初始化项目的输入、译文和成品全部保留。"""
    toolbox = ROOT/'toolbox'
    sys.path.insert(0, str(toolbox))
    import toolkit
    toolkit.check_local_trees()
    for relative in EMPTY_DIRS:
        toolkit.local(relative).mkdir(parents=True, exist_ok=True)
    (ROOT/'.build').mkdir(exist_ok=True)
    print('[完成] 工具箱目录检查通过；Python 文件直接运行，无需生成发行副本')


if __name__ == '__main__':
    main()
