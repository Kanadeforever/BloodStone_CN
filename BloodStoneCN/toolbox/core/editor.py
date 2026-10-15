# -*- coding: utf-8 -*-
"""译文编辑模型：界面只改 translate，落盘前复用真实 LOC 导入器验收。"""
from __future__ import annotations

import hashlib
import json
import os
import tempfile
from pathlib import Path

from loc_json import _unique_json_object, import_one, json_bytes
from native_pak import FilePak


class TranslationDocument:
    """保留记录身份和读取指纹，避免 GUI 保存时覆盖外部编辑器的新修改。"""

    def __init__(self, root: Path, relative: str):
        self.root = root.resolve()
        self.path = self.root/'translations'/relative
        base = (self.root/'translations').resolve()
        if not self.path.resolve().is_relative_to(base) or self.path.suffix != '.json':
            raise ValueError('请选择 translations 中的翻译 JSON')
        self._check_path()
        raw = self.path.read_bytes()
        self.fingerprint = hashlib.sha256(raw).hexdigest()
        self.document = json.loads(raw.decode('utf-8-sig'), object_pairs_hook=_unique_json_object)
        self.resource = self.path.relative_to(base).as_posix().removesuffix('.json')
        # 校验模板、原文、资源键和所有记录，损坏的身份字段不会进入可编辑状态。
        with FilePak(self.root/'input/gamedata.pak') as archive:
            self.template = archive.read_name(self.resource)
        import_one(self.template, self.document, self.resource)
        self.dirty = False

    def _check_path(self) -> None:
        """重解析点和符号链接可能改变写入位置，保存前必须重新检查。"""
        for path in (self.path, *self.path.parents):
            if path.is_symlink() or (path.exists() and bool(
                    getattr(path.lstat(), 'st_file_attributes', 0) & 0x400)):
                raise ValueError('译文目录不允许链接或 Windows 目录联接')
            if path == self.root:
                break

    @property
    def entries(self) -> list[dict]:
        return self.document['entries']

    def update(self, index: int, translation: str) -> None:
        """界面提交当前编辑框；其他字段完全保留。"""
        if not isinstance(translation, str):
            raise ValueError('译文必须是文本')
        if self.entries[index]['translate'] != translation:
            self.entries[index]['translate'] = translation
            self.dirty = True

    def matching_indices(self, query: str) -> list[int]:
        """同时搜索资源键、原文和译文，返回真实记录索引供表格定位。"""
        needle = query.casefold().strip()
        return [index for index, entry in enumerate(self.entries)
                if not needle or any(needle in str(entry.get(field, '')).casefold()
                                     for field in ('key', 'hash', 'original', 'translate'))]

    def save(self) -> list:
        """先完成语义验收和外部修改检查，最后一次性替换整份 JSON。"""
        self._check_path()
        if hashlib.sha256(self.path.read_bytes()).hexdigest() != self.fingerprint:
            raise ValueError('此 JSON 已被其他程序修改。请先另存当前编辑内容，再重新载入文件')
        _, warnings = import_one(self.template, self.document, self.resource)
        payload = json_bytes(self.document)
        temporary = None
        try:
            # 临时文件和正式 JSON 位于同一分区，os.replace 不会留下半份文档。
            with tempfile.NamedTemporaryFile(dir=self.path.parent, suffix='.part', delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(payload)
            self._check_path()
            if hashlib.sha256(self.path.read_bytes()).hexdigest() != self.fingerprint:
                raise ValueError('保存期间 JSON 被其他程序修改，未覆盖外部内容')
            os.replace(temporary, self.path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        self.fingerprint = hashlib.sha256(payload).hexdigest()
        self.dirty = False
        return warnings
