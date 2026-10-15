# -*- coding: utf-8 -*-
"""统一工具箱的图形入口。窗口交互在主线程，转换工作在后台子进程。"""
from __future__ import annotations

import ctypes
import json
import os
import queue
import subprocess
import sys
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

sys.dont_write_bytecode = True
import toolkit
from editor import TranslationDocument
from jobs import JobRunner

ROOT = Path(__file__).resolve().parent
MODES = {'精准译文字集（推荐）': 'precise', 'GB2312 一级常用字': 'common', '完整 GBK 字集': 'gbk'}


def enable_dpi() -> None:
    """先向 Windows 声明 DPI 感知，避免高分屏将控件当作位图模糊放大。"""
    if os.name == 'nt':
        try:
            ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        except (AttributeError, OSError):
            ctypes.windll.user32.SetProcessDPIAware()


class ToolboxApp:
    def __init__(self, window: tk.Tk):
        self.window = window
        self.runner = JobRunner(ROOT)
        self.busy = False
        self.document = None
        self.current_index = None
        # 登记会改变工作输入的控件，后台转换期间统一禁用并恢复原状态。
        self.controls = []
        self.previous_states = []
        self.finish_action = None
        self.refreshing_table = False
        window.title('BloodstoneCN · 汉化工具箱')
        window.geometry('1120x860')
        window.minsize(960, 740)
        window.protocol('WM_DELETE_WINDOW', self.close)
        self._style()
        self._variables()
        self._build_window()
        self.refresh_status()
        window.after(100, self.poll_jobs)

    def _style(self) -> None:
        """标签和容器使用一致背景，避免传统静态文字的突兀灰底。"""
        style = ttk.Style(self.window)
        if os.name != 'nt':
            style.theme_use('clam')
        self.window.option_add('*Font', ('Microsoft YaHei UI' if os.name == 'nt' else 'Sans', 10))
        style.configure('Title.TLabel', font=('Microsoft YaHei UI' if os.name == 'nt' else 'Sans', 18, 'bold'))
        style.configure('TButton', padding=(12, 6))
        style.configure('Treeview', rowheight=28)

    def _variables(self) -> None:
        """只从工具箱本地读取路径偏好，公开默认配置不保存个人游戏位置。"""
        values = {}
        settings = ROOT/'.caches/gui_settings.json'
        if settings.is_file():
            try:
                values = json.loads(settings.read_text(encoding='utf-8'))
                if not isinstance(values, dict):
                    values = {}
            except (OSError, ValueError):
                pass
        self.game = tk.StringVar(value=values.get('game', ''))
        self.manual = tk.BooleanVar(value=False)
        self.gamedata = tk.StringVar()
        self.root_pak = tk.StringVar()
        self.templates = tk.StringVar()
        self.font = tk.StringVar()
        self.pixel = tk.StringVar(value='32')
        options = ROOT/'input/font_options.json'
        if not options.is_file():
            options = ROOT/'config/defaults.json'
        if options.is_file():
            self.pixel.set(str(json.loads(options.read_text(encoding='utf-8')).get('pixel_size', 32)))
        try:
            default_plugin = str(toolkit.resolve_plugin())
        except ValueError:
            default_plugin = ''
        self.plugin = tk.StringVar(value=values.get('plugin', default_plugin))
        self.mode = tk.StringVar(value=next(iter(MODES)))
        self.resource = tk.StringVar()
        self.search = tk.StringVar()
        self.status = tk.StringVar(value='尚未初始化')
        self.editor_status = tk.StringVar(value='初始化后可在这里编辑每条 translate。')

    def button(self, parent, text: str, command, **grid) -> ttk.Button:
        """创建按钮并登记其状态；command 是点击时才执行的函数。"""
        widget = ttk.Button(parent, text=text, command=command)
        widget.grid(**grid)
        self.controls.append(widget)
        return widget

    def entry(self, parent, variable, row: int, label: str, browse=None) -> None:
        """每行统一放标签、输入框和可选文件按钮，避免路径行错位。"""
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky='w', padx=(0, 10), pady=5)
        entry = ttk.Entry(parent, textvariable=variable)
        entry.grid(row=row, column=1, sticky='ew', pady=5)
        self.controls.append(entry)
        if browse:
            self.button(parent, '选择…', browse, row=row, column=2, padx=(8, 0), pady=5)

    def _build_window(self) -> None:
        """上方是三个工作页，下方保留所有工作页共用的日志和进度。"""
        outer = ttk.Frame(self.window, padding=18)
        outer.pack(fill='both', expand=True)
        ttk.Label(outer, text='BloodstoneCN 汉化工具箱', style='Title.TLabel').pack(anchor='w')
        ttk.Label(outer, text='初始化一次 → 编辑译文与选择字体 → 构建并验收汉化', padding=(0, 7, 0, 12)).pack(anchor='w')
        notebook = ttk.Notebook(outer)
        notebook.pack(fill='both', expand=True)
        self.notebook = notebook
        project, translation, build = [ttk.Frame(notebook, padding=14) for _ in range(3)]
        for page, title in [(project, '1  项目初始化'), (translation, '2  译文编辑'), (build, '3  构建与验收')]:
            notebook.add(page, text=title)
        self._project_page(project)
        self._translation_page(translation)
        self._build_page(build)
        logs = ttk.LabelFrame(outer, text='工作日志', padding=8)
        logs.pack(fill='x', pady=(10, 0))
        self.logs = ScrolledText(logs, height=7, wrap='word', state='disabled', font=('Consolas', 10))
        self.logs.pack(fill='x')
        footer = ttk.Frame(outer)
        footer.pack(fill='x', pady=(8, 0))
        ttk.Label(footer, textvariable=self.status).pack(side='left')
        self.progress = ttk.Progressbar(footer, mode='indeterminate', length=130)
        self.progress.pack(side='right', padx=(8, 0))
        ttk.Button(footer, text='清空日志', command=self.clear_log).pack(side='right')

    def _project_page(self, page) -> None:
        """常规操作只需游戏目录，额外的手动来源按需展开。"""
        page.columnconfigure(0, weight=1)
        group = ttk.LabelFrame(page, text='个人游戏来源', padding=12)
        group.grid(row=0, column=0, sticky='ew')
        group.columnconfigure(1, weight=1)
        self.entry(group, self.game, 0, '游戏目录', lambda: self.pick_directory(self.game))
        ttk.Label(group, text='选择包含 Bond.exe 的目录；原文和字体模板只读提取到工具箱。').grid(row=1, column=0, columnspan=3, sticky='w', pady=6)
        check = ttk.Checkbutton(group, text='手动指定 PAK 或已提取的 EUR 模板', variable=self.manual, command=self.toggle_manual)
        check.grid(row=2, column=0, columnspan=3, sticky='w', pady=6)
        self.controls.append(check)
        self.advanced = ttk.Frame(group)
        self.advanced.grid(row=3, column=0, columnspan=3, sticky='ew')
        self.advanced.columnconfigure(1, weight=1)
        self.entry(self.advanced, self.gamedata, 0, 'gamedata.pak', lambda: self.pick_file(self.gamedata, [('游戏 PAK', '*.pak')]))
        self.entry(self.advanced, self.root_pak, 1, 'root.pak', lambda: self.pick_file(self.root_pak, [('游戏 PAK', '*.pak')]))
        self.entry(self.advanced, self.templates, 2, 'EUR 模板目录', lambda: self.pick_directory(self.templates))
        ttk.Label(self.advanced, text='模板目录非空时优先使用其中的 ui_font_eur.dat / .bpr。').grid(row=3, column=0, columnspan=3, sticky='w')
        self.advanced.grid_remove()
        actions = ttk.Frame(page)
        actions.grid(row=1, column=0, sticky='w', pady=14)
        self.initialize_button = self.button(actions, '初始化个人项目', self.prepare, row=0, column=0, padx=(0, 8))
        self.button(actions, '刷新项目状态', self.refresh_status, row=0, column=1, padx=8)
        self.button(actions, '导出原文副本', lambda: self.launch(['export'], '导出原文副本'), row=0, column=2, padx=8)
        self.button(actions, '打开使用说明', lambda: self.open_path(ROOT/'使用说明.md'), row=0, column=3, padx=8)
        ttk.Label(page, text='初始化将生成 86 份待翻译 JSON 和三面字体模板。已有项目不会被重新初始化覆盖。\n译文、模板、字体和输出都保存在当前 toolbox 内；工具不会自动安装到游戏。', justify='left', wraplength=850).grid(row=2, column=0, sticky='w', pady=10)

    def _translation_page(self, page) -> None:
        """表格保存真实记录索引，下面的两个文本框显示完整多行内容。"""
        page.columnconfigure(0, weight=1)
        page.rowconfigure(1, weight=1)
        bar = ttk.Frame(page)
        bar.grid(row=0, column=0, sticky='ew', pady=(0, 8))
        bar.columnconfigure(1, weight=1)
        ttk.Label(bar, text='资源').grid(row=0, column=0, padx=(0, 8))
        self.resource_box = ttk.Combobox(bar, textvariable=self.resource, state='readonly', width=65)
        self.resource_box.grid(row=0, column=1, sticky='ew')
        self.resource_box.bind('<<ComboboxSelected>>', lambda _event: self.load_resource())
        self.controls.append(self.resource_box)
        self.button(bar, '重新载入', self.reload_resource, row=0, column=2, padx=8)
        self.button(bar, '打开译文目录', lambda: self.open_path(ROOT/'translations'), row=0, column=3)
        ttk.Label(bar, text='搜索').grid(row=1, column=0, padx=(0, 8), pady=(8, 0))
        search = ttk.Entry(bar, textvariable=self.search)
        search.grid(row=1, column=1, sticky='ew', pady=(8, 0))
        search.bind('<Return>', lambda _event: self.filter_rows())
        self.controls.append(search)
        self.button(bar, '筛选', self.filter_rows, row=1, column=2, padx=8, pady=(8, 0))
        self.button(bar, '保存当前文件', self.save_document, row=1, column=3, pady=(8, 0))
        panes = ttk.Panedwindow(page, orient='vertical')
        panes.grid(row=1, column=0, sticky='nsew')
        table_frame = ttk.Frame(panes)
        self.table = ttk.Treeview(table_frame, columns=('key', 'original', 'translate'), show='headings', height=7, selectmode='browse')
        for name, title, width in [('key', '资源键', 190), ('original', '原文', 345), ('translate', '译文', 345)]:
            self.table.heading(name, text=title)
            self.table.column(name, width=width, minwidth=80)
        scroll = ttk.Scrollbar(table_frame, orient='vertical', command=self.table.yview)
        self.table.configure(yscrollcommand=scroll.set)
        self.table.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self.table.bind('<<TreeviewSelect>>', self.select_row)
        self.table.bind('<Button-1>', lambda _event: 'break' if self.busy else None)
        self.table.bind('<Key>', lambda _event: 'break' if self.busy else None)
        panes.add(table_frame, weight=2)
        edit_frame = ttk.Frame(panes, padding=(0, 8, 0, 0))
        edit_frame.columnconfigure(0, weight=1)
        edit_frame.columnconfigure(1, weight=1)
        edit_frame.rowconfigure(1, weight=1)
        ttk.Label(edit_frame, text='原文（只读）').grid(row=0, column=0, sticky='w')
        ttk.Label(edit_frame, text='译文 translate').grid(row=0, column=1, sticky='w', padx=(8, 0))
        self.original_text = ScrolledText(edit_frame, height=5, wrap='word', state='disabled')
        self.translation_text = ScrolledText(edit_frame, height=5, wrap='word', undo=True)
        self.original_text.grid(row=1, column=0, sticky='nsew', pady=4)
        self.translation_text.grid(row=1, column=1, sticky='nsew', padx=(8, 0), pady=4)
        self.controls.append(self.translation_text)
        self.translation_text.bind('<<Modified>>', self.mark_editor_dirty)
        panes.add(edit_frame, weight=1)
        ttk.Label(page, textvariable=self.editor_status).grid(row=2, column=0, sticky='w', pady=(6, 0))

    def _build_page(self, page) -> None:
        """字体设置与构建动作共用一页，完整构建和单项转换各有明确出口。"""
        page.columnconfigure(0, weight=1)
        fonts = ttk.LabelFrame(page, text='个人字体', padding=12)
        fonts.grid(row=0, column=0, sticky='ew')
        fonts.columnconfigure(1, weight=1)
        self.entry(fonts, self.font, 0, '字体文件', lambda: self.pick_file(self.font, [('字体', '*.ttf *.otf *.ttc'), ('全部文件', '*.*')]))
        ttk.Label(fonts, text='像素字号').grid(row=1, column=0, sticky='w', pady=8)
        spin = ttk.Spinbox(fonts, from_=16, to=34, textvariable=self.pixel, width=8)
        spin.grid(row=1, column=1, sticky='w', pady=8)
        self.controls.append(spin)
        self.button(fonts, '导入字体 / 保存字号', self.configure_font, row=1, column=2, padx=(8, 0))
        ttk.Label(fonts, text='原字体只读复制；生成所需字符，不向系统安装字体。留空字体路径时只保存字号。').grid(row=2, column=0, columnspan=3, sticky='w')
        builds = ttk.LabelFrame(page, text='构建选项', padding=12)
        builds.grid(row=1, column=0, sticky='ew', pady=12)
        builds.columnconfigure(1, weight=1)
        ttk.Label(builds, text='字集模式').grid(row=0, column=0, sticky='w', padx=(0, 10))
        modes = ttk.Combobox(builds, textvariable=self.mode, values=list(MODES), state='readonly')
        modes.grid(row=0, column=1, columnspan=2, sticky='ew', pady=5)
        self.controls.append(modes)
        self.entry(builds, self.plugin, 1, 'ASI 插件', lambda: self.pick_file(self.plugin, [('ASI 插件', '*.asi')]))
        actions = ttk.Frame(builds)
        actions.grid(row=2, column=0, columnspan=3, sticky='w', pady=(10, 0))
        self.button(actions, '构建完整汉化', lambda: self.build('build'), row=0, column=0, padx=(0, 8))
        self.button(actions, '验收当前成品', self.verify, row=0, column=1, padx=8)
        self.button(actions, '打开安装目录', lambda: self.open_path(ROOT/'output/Runtime'), row=0, column=2, padx=8)
        self.button(actions, '打开全部输出', lambda: self.open_path(ROOT/'output'), row=0, column=3, padx=8)
        separate = ttk.Frame(page)
        separate.grid(row=2, column=0, sticky='w')
        self.button(separate, '只回编 LOC', lambda: self.build('build-loc'), row=0, column=0, padx=(0, 8))
        self.button(separate, '只生成字库', lambda: self.build('build-font'), row=0, column=1, padx=8)
        ttk.Label(page, text='完整构建自动使用同一份译文回编 LOC、生成字库并验收。\n成功后复制 output/Runtime 中的 ASI 与 BSCNContents，游戏语言选择 EN。\n单独转换无需 ASI；组装安装目录时才需要所选插件。', justify='left', wraplength=880).grid(row=3, column=0, sticky='w', pady=16)

    def toggle_manual(self) -> None:
        self.advanced.grid() if self.manual.get() else self.advanced.grid_remove()

    def pick_file(self, variable, types) -> None:
        if not self.busy:
            value = filedialog.askopenfilename(parent=self.window, filetypes=types)
            if value:
                variable.set(value)

    def pick_directory(self, variable) -> None:
        if not self.busy:
            value = filedialog.askdirectory(parent=self.window, mustexist=True)
            if value:
                variable.set(value)

    def prepare(self) -> None:
        """将用户选定的来源转成后端参数，缺少必填来源时留在当前页面。"""
        arguments = ['prepare']
        if self.manual.get():
            arguments += ['--gamedata', self.gamedata.get().strip()]
            arguments += (['--template-dir', self.templates.get().strip()] if self.templates.get().strip()
                          else ['--root-pak', self.root_pak.get().strip()])
            required = (self.gamedata.get().strip(), self.templates.get().strip() or self.root_pak.get().strip())
        else:
            arguments += ['--game', self.game.get().strip()]
            required = (self.game.get().strip(),)
        if not all(required):
            messagebox.showerror('来源不完整', '请先选择游戏目录，或完整填写手动来源。', parent=self.window)
            return
        self.launch(arguments, '初始化个人项目')

    def configure_font(self) -> None:
        """先校验字号，再让后端复制字体和保存配置；界面不直接覆盖输入。"""
        try:
            pixel = int(self.pixel.get())
            if not 16 <= pixel <= 34:
                raise ValueError()
        except ValueError:
            messagebox.showerror('字号无效', '请输入 16～34 的整数像素字号。', parent=self.window)
            return
        arguments = ['configure-font', '--pixel-size', str(pixel)]
        if self.font.get().strip():
            arguments += ['--font', self.font.get().strip()]
        self.launch(arguments, '保存字体配置', lambda: self.font.set(''))

    def build(self, command: str) -> None:
        arguments = [command, '--mode', MODES[self.mode.get()]]
        if command != 'build-loc':
            # 把界面中当前选定的字体和字号传给同一次构建，避免误用旧配置。
            arguments += ['--pixel-size', self.pixel.get().strip()]
            if self.font.get().strip():
                arguments += ['--font', self.font.get().strip()]
        if command == 'build' and self.plugin.get().strip():
            arguments += ['--plugin', self.plugin.get().strip()]
        self.launch(arguments, '构建完整汉化' if command == 'build' else '单独转换',
                    (lambda: self.font.set('')) if command != 'build-loc' else None)

    def verify(self) -> None:
        arguments = ['verify']
        if self.plugin.get().strip():
            arguments += ['--plugin', self.plugin.get().strip()]
        self.launch(arguments, '验收当前成品')

    def launch(self, arguments: list[str], title: str, finish=None) -> None:
        """启动前先保存编辑器；所有耗时任务复用同一个命令行后端。"""
        if self.busy or not self.confirm_document():
            return
        try:
            toolkit.check_local_trees()
            self.save_settings()
            self.runner.start(arguments)
        except (OSError, ValueError) as exc:
            messagebox.showerror('无法启动', str(exc), parent=self.window)
            return
        self.finish_action = finish
        self.set_busy(True)
        self.status.set('正在执行：'+title)
        self.append_log('\n[开始] '+title)

    def set_busy(self, busy: bool) -> None:
        """记住控件原来的只读状态，任务结束后准确恢复。"""
        self.busy = busy
        if busy:
            self.previous_states = [(widget, str(widget.cget('state'))) for widget in self.controls]
            for widget, _state in self.previous_states:
                widget.configure(state='disabled')
            self.progress.start(15)
        else:
            for widget, state in self.previous_states:
                widget.configure(state=state)
            self.previous_states.clear()
            self.progress.stop()

    def poll_jobs(self) -> None:
        """定时消费后台事件；Tk 的控件修改始终发生在主线程。"""
        try:
            while True:
                kind, value = self.runner.events.get_nowait()
                if kind == 'log':
                    self.append_log(value)
                else:
                    self.set_busy(False)
                    if value == 0 and self.finish_action:
                        self.finish_action()
                    self.finish_action = None
                    self.refresh_status()
                    self.append_log('[完成] 操作成功。' if value == 0 else '[失败] 请查看上方错误；已有完整成品未被失败构建替换。')
                    self.status.set(('操作成功。' if value == 0 else '操作失败，请查看日志。')+' '+self.status.get())
        except queue.Empty:
            pass
        self.window.after(100, self.poll_jobs)

    def refresh_status(self) -> None:
        """刷新文件列表及状态，已有项目禁用初始化按钮以保护译文。"""
        if self.busy:
            return
        try:
            toolkit.check_local_trees()
            ready = (ROOT/'work/project.json').is_file()
            files = sorted(p.relative_to(ROOT/'translations').as_posix() for p in (ROOT/'translations').rglob('*.loc.json'))
            self.resource_box.configure(values=files)
            self.initialize_button.configure(state='disabled' if ready else 'normal')
            fonts = [p.name for p in (ROOT/'input/fonts').glob('*') if p.suffix.lower() in ('.ttf', '.otf', '.ttc')]
            self.status.set(f'{"已初始化" if ready else "尚未初始化"}  |  译文 {len(files)}/86  |  字体：{", ".join(fonts) or "未导入"}  |  {"已有构建，请按需验收" if (ROOT/"output/build_manifest.json").is_file() else "尚无完整成品"}')
            if files and not self.resource.get():
                self.resource.set(files[0])
                self.load_resource()
        except (OSError, ValueError, KeyError) as exc:
            self.status.set('读取状态失败：'+str(exc))

    def save_settings(self) -> None:
        toolkit.check_local_trees()
        cache = ROOT/'.caches'
        cache.mkdir(exist_ok=True)
        (cache/'gui_settings.json').write_bytes(toolkit.json_bytes({'game': self.game.get(), 'plugin': self.plugin.get()}))

    def commit_editor(self) -> None:
        """切换行之前把编辑框内容放回内存，暂不写磁盘。"""
        if self.document is not None and self.current_index is not None:
            self.document.update(self.current_index, self.translation_text.get('1.0', 'end-1c'))

    def confirm_document(self) -> bool:
        """离开文件或开始构建时，明确处理尚未保存的译文。"""
        self.commit_editor()
        if self.document is None or not self.document.dirty:
            return True
        answer = messagebox.askyesnocancel('尚未保存', '保存当前 JSON 的译文修改吗？', parent=self.window)
        if answer is None:
            return False
        if answer:
            return self.save_document()
        # 用户选择不保存时，界面也恢复到磁盘内容；后续构建只会看到同一份译文。
        try:
            self.document = TranslationDocument(ROOT, self.document.resource+'.json')
            self.current_index = None
            self.fill_editor('', '')
            self.filter_rows()
            self.editor_status.set('未保存的修改已放弃，使用磁盘中的译文。')
        except (OSError, ValueError, KeyError, AssertionError) as exc:
            messagebox.showerror('无法恢复译文', str(exc), parent=self.window)
            return False
        return True

    def load_resource(self) -> None:
        if self.busy:
            return
        requested = self.resource.get()
        if not requested:
            return
        if not self.confirm_document():
            if self.document is not None:
                self.resource.set(self.document.resource+'.json')
            return
        try:
            toolkit.check_local_trees()
            document = TranslationDocument(ROOT, requested)
        except (OSError, ValueError, KeyError, AssertionError) as exc:
            messagebox.showerror('无法读取译文', str(exc), parent=self.window)
            if self.document is not None:
                self.resource.set(self.document.resource+'.json')
            return
        self.document = document
        self.current_index = None
        self.fill_editor('', '')
        self.filter_rows()

    def reload_resource(self) -> None:
        self.load_resource()

    def filter_rows(self) -> None:
        """筛选只改变显示，保留内存中的所有记录和已编辑译文。"""
        if self.busy or self.document is None:
            return
        self.commit_editor()
        self.refreshing_table = True
        self.current_index = None
        self.table.delete(*self.table.get_children())
        indices = self.document.matching_indices(self.search.get())
        for index in indices:
            entry = self.document.entries[index]
            values = [str(entry.get(name, '')).replace('\r', '').replace('\n', ' ↵ ') for name in ('key', 'original', 'translate')]
            self.table.insert('', 'end', iid=str(index), values=values)
        self.fill_editor('', '')
        self.refreshing_table = False
        self.editor_status.set(f'{len(indices)} / {len(self.document.entries)} 条记录；只编辑右侧译文，保存时自动校验身份。')

    def select_row(self, _event=None) -> None:
        """先接收上一行内容，再根据真实索引读取下一行，避免筛选后错行。"""
        if self.busy or self.refreshing_table or self.document is None:
            return
        self.commit_editor()
        selection = self.table.selection()
        self.current_index = int(selection[0]) if selection else None
        if self.current_index is not None:
            entry = self.document.entries[self.current_index]
            self.fill_editor(entry['original'], entry['translate'])

    def fill_editor(self, original: str, translation: str) -> None:
        self.original_text.configure(state='normal')
        self.original_text.delete('1.0', 'end')
        self.original_text.insert('1.0', original)
        self.original_text.configure(state='disabled')
        self.translation_text.delete('1.0', 'end')
        self.translation_text.insert('1.0', translation)
        self.translation_text.edit_modified(False)

    def mark_editor_dirty(self, _event=None) -> None:
        if self.translation_text.edit_modified():
            self.editor_status.set('译文已修改，尚未保存。')
            self.translation_text.edit_modified(False)

    def save_document(self) -> bool:
        """交给编辑模型进行原子保存；失败时保留内存编辑内容供用户复制。"""
        if self.busy:
            return False
        if self.document is None:
            return True
        self.commit_editor()
        try:
            toolkit.check_local_trees()
            warnings = self.document.save()
        except (OSError, ValueError, KeyError, AssertionError) as exc:
            messagebox.showerror('译文未保存', str(exc), parent=self.window)
            return False
        if self.current_index is not None and self.table.exists(str(self.current_index)):
            entry = self.document.entries[self.current_index]
            self.table.set(str(self.current_index), 'translate', entry['translate'].replace('\n', ' ↵ '))
        self.editor_status.set(f'已保存；控制标记差异：{len(warnings)} 条。')
        self.append_log('[保存] '+self.document.resource+f'；控制标记差异 {len(warnings)} 条')
        return True

    def append_log(self, line: str) -> None:
        self.logs.configure(state='normal')
        self.logs.insert('end', line+'\n')
        self.logs.see('end')
        self.logs.configure(state='disabled')
        try:
            toolkit.check_local_trees()
            cache = ROOT/'.caches'
            cache.mkdir(exist_ok=True)
            with (cache/'gui.log').open('a', encoding='utf-8', newline='') as stream:
                stream.write(line.replace('\r\n', '\n').replace('\n', '\r\n')+'\r\n')
        except (OSError, ValueError):
            pass

    def clear_log(self) -> None:
        self.logs.configure(state='normal')
        self.logs.delete('1.0', 'end')
        self.logs.configure(state='disabled')
        try:
            toolkit.check_local_trees()
            (ROOT/'.caches/gui.log').unlink(missing_ok=True)
        except (OSError, ValueError):
            pass

    def open_path(self, path: Path) -> None:
        if not path.exists():
            messagebox.showinfo('目录尚未生成', '请先完成对应操作。', parent=self.window)
            return
        if os.name == 'nt':
            os.startfile(path)
        else:
            subprocess.Popen(['open' if sys.platform == 'darwin' else 'xdg-open', str(path)])

    def close(self) -> None:
        if self.busy:
            messagebox.showinfo('正在执行操作', '请等待当前操作结束后关闭，后台转换不会被强制中断。', parent=self.window)
            return
        if self.confirm_document():
            self.save_settings()
            self.window.destroy()


def main() -> None:
    enable_dpi()
    window = tk.Tk()
    try:
        toolkit.check_local_trees()
        ToolboxApp(window)
    except (OSError, ValueError, KeyError) as exc:
        messagebox.showerror('工具箱无法启动', str(exc), parent=window)
        window.destroy()
        raise SystemExit(1)
    window.mainloop()


if __name__ == '__main__':
    main()
