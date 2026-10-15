# -*- coding: utf-8 -*-
"""离线关联 NPC 事件与游戏原生字幕 UI 调用，帮助复核去重条件。"""
from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path
import re

# 解析插件自己生成的一行一事件日志；不会访问游戏、注册表或用户目录。
EVENT_RE = re.compile(r"\[ms=(\d+)\].*? (ORIGINAL_DIALOGUE_EVENT|ORIGINAL_NATIVE_UI_SUBMIT_CALL) (.*)$")
FIELD_RE = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)=(0x[0-9A-Fa-f]+|[0-9]+)")


def read_records(path: Path) -> list[dict[str, object]]:
    """返回日志记录，保留原本先后顺序和事件发生时的进程毫秒数。"""
    results = []
    for lineno, line in enumerate(path.read_text('utf-8-sig', errors='replace').splitlines(), 1):
        match = EVENT_RE.search(line)
        if match is None:
            continue
        ms, kind, payload = match.groups()
        fields = dict(FIELD_RE.findall(payload))
        if 'hash' not in fields or 'object' not in fields:
            continue
        results.append({'line': lineno, 'ms': int(ms), 'kind': kind, 'fields': fields})
    return results


def analyze(records: list[dict[str, object]], window_ms: int = 15000) -> dict[str, object]:
    """只归纳观察结果，不把“没有看到提交调用”等同于游戏绝对不会显示。"""
    events = []
    ui_calls = 0
    orphans = 0
    # 同一对象可以重复播放同一句对白。匹配最近一次事件，避免跨场景误关联。
    by_identity: dict[tuple[str, str], list[dict[str, object]]] = defaultdict(list)
    for record in records:
        fields = record['fields']
        identity = (fields['object'], fields['hash'])
        if record['kind'] == 'ORIGINAL_DIALOGUE_EVENT':
            event = {'ms': record['ms'], 'fields': fields, 'native_submit': False, 'line': record['line']}
            events.append(event)
            by_identity[identity].append(event)
        else:
            ui_calls += 1
            candidates = by_identity.get(identity, [])
            matched = None
            for event in reversed(candidates):
                delta = record['ms'] - event['ms']
                if 0 <= delta <= window_ms:
                    matched = event
                    break
            if matched is None:
                orphans += 1
            else:
                matched['native_submit'] = True
    summaries = defaultdict(lambda: {'events': 0, 'ui_observed': 0})
    for event in events:
        fields = event['fields']
        key = (fields['hash'], fields.get('source_1C', '?'),
               fields.get('source_1D', '?'), fields.get('state_7A', '?'))
        summaries[key]['events'] += 1
        summaries[key]['ui_observed'] += int(event['native_submit'])
    return {'events': len(events), 'ui_calls': ui_calls, 'ui_unpaired': orphans, 'summary': dict(summaries)}


def format_report(result: dict[str, object]) -> str:
    """生成简体中文终端报告，便于直接复制到问题反馈。"""
    lines = [f"事件数：{result['events']}；原生 UI 提交调用数：{result['ui_calls']}；未配对调用数：{result['ui_unpaired']}",
             '哈希 | 事件+0x1C | 事件+0x1D | 状态+0x7A | 事件次数 | 观察到原生UI调用']
    for key, stats in sorted(result['summary'].items()):
        h, a, b, state = key
        lines.append(f"{h} | {a} | {b} | {state} | {stats['events']} | {stats['ui_observed']}")
    lines.append('注意：这是调用链的观察记录，不是“画面中显示字幕”的直接证明。')
    lines.append('不同线程、日志采样、其他 UI 提交入口可能使记录无法完全配对。')
    return '\n'.join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description='离线归纳 NPC 对话与原生字幕提交链路')
    parser.add_argument('log', type=Path, help='运行 Bond.exe --log 后生成的 NPCSubtitles.log')
    parser.add_argument('--window-ms', type=int, default=15000, help='同一对象与哈希允许的匹配时间窗口，默认 15000 毫秒')
    args = parser.parse_args()
    if args.window_ms < 0:
        parser.error('匹配窗口不能小于零')
    print(format_report(analyze(read_records(args.log), args.window_ms)))


if __name__ == '__main__':
    main()
