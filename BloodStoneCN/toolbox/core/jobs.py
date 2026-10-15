# -*- coding: utf-8 -*-
"""GUI 后台任务：子进程产生输出，线程排队，主界面负责读取队列。"""
from __future__ import annotations

import os
import queue
import subprocess
import sys
import threading
from pathlib import Path


class JobRunner:
    """一次只执行一个工作流，Windows 子进程不额外弹出控制台。"""

    def __init__(self, root: Path):
        self.root = root.resolve()
        self.events = queue.Queue()
        self.running = False

    def start(self, arguments: list[str]) -> None:
        if self.running:
            raise ValueError('当前操作尚未结束，请等待完成')
        self.running = True
        threading.Thread(target=self._run, args=(list(arguments),), daemon=True).start()

    def _run(self, arguments: list[str]) -> None:
        """只传递文本和返回码；后台线程不直接访问任何 Tk 控件。"""
        code = 1
        try:
            executable = Path(sys.executable)
            if executable.name.lower() == 'pythonw.exe':
                executable = executable.with_name('python.exe')
            options = {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {}
            process = subprocess.Popen(
                [str(executable), '-u', '-B', str(self.root/'toolkit.py'), *arguments],
                cwd=self.root, env=dict(os.environ, PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1'),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                encoding='utf-8', errors='replace', **options)
            with process.stdout:
                for line in process.stdout:
                    self.events.put(('log', line.rstrip('\r\n')))
            code = process.wait()
        except Exception as exc:
            self.events.put(('log', '[错误] 无法启动工作流：'+str(exc)))
        finally:
            self.running = False
            self.events.put(('done', code))
