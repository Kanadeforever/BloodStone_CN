#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""R8 纯模型测试。全部使用合成占位文本，不包含游戏实际译文。"""
from __future__ import annotations
import struct
import unittest


def unique_map(records, key=lambda s: s):
    out = {}
    bad = set()
    for src, dst in records:
        k = key(src)
        if not k or k in bad:
            continue
        if k not in out:
            out[k] = dst
        elif out[k] != dst:
            del out[k]
            bad.add(k)
    return out, bad


def norm(s):
    return " ".join(s.split())


def prefix_alias(records):
    grouped = {}
    for src, dst in records:
        if len(src) <= 4 and src.isascii() and src.isalpha():
            grouped.setdefault(src, [])
            if dst not in grouped[src]:
                grouped[src].append(dst)
    result = {}
    for src, values in grouped.items():
        if len(values) != 2:
            continue
        a, b = sorted(values, key=len)
        if len(a) < len(b) and b.startswith(a):
            result[src] = b
    return result



def base_label_alias(records):
    grouped = {}
    for src, dst in records:
        if len(src) <= 4 or not src.isascii() or not src.isalpha():
            continue
        grouped.setdefault(src, [])
        if dst not in grouped[src]:
            grouped[src].append(dst)
    result = {}
    for src, values in grouped.items():
        if len(values) < 2:
            continue
        valid = []
        for candidate in values:
            others = [v for v in values if v != candidate]
            if others and all(len(v) > len(candidate) and v.startswith(candidate) for v in others):
                valid.append(candidate)
        if len(valid) == 1:
            result[src] = valid[0]
    return result


def normalized_all(records):
    return unique_map(records, key=norm)


def resolve_compound(text, mapping):
    for delimiter in (" / ", " - "):
        if delimiter not in text:
            continue
        parts = text.split(delimiter)
        if len(parts) < 2 or any(not p.strip() for p in parts):
            return None
        translated = []
        for part in parts:
            core = part.strip()
            if core not in mapping:
                return None
            lead = part[:len(part)-len(part.lstrip())]
            trail = part[len(part.rstrip()):]
            translated.append(lead + mapping[core] + trail)
        return delimiter.join(translated)
    return None


def cross_thread_select(submitted, candidates):
    exact = [(src, dst, 0, seq) for src, dst, seq in candidates if submitted == src]
    pool = exact
    require_same_source = False
    if not pool:
        pool = []
        for src, dst, seq in candidates:
            if len(src) >= 4 and src in submitted:
                pool.append((src, dst, submitted.index(src), seq))
        require_same_source = True
    if not pool:
        return None
    source, dst, at, seq = pool[0]
    for src2, dst2, at2, seq2 in pool[1:]:
        if require_same_source and src2 != source:
            return None
        if dst2 != dst:
            return None
        if seq2 > seq:
            source, dst, at, seq = src2, dst2, at2, seq2
    return submitted[:at] + dst + submitted[at+len(source):]



def precise_printf_apply(source, replacement, submitted):
    # 纯模型：验证“精确 key 选中模板 -> 最终已格式化 SubmitText”的结构恢复。
    # 测试只使用一个 %s；C++ 版本支持多个带可证明 literal 边界的占位符。
    if source.count("%s") != 1 or replacement.count("%s") != 1:
        return None
    src_left, src_right = source.split("%s")
    if not submitted.startswith(src_left) or not submitted.endswith(src_right):
        return None
    begin = len(src_left)
    end = len(submitted) - len(src_right) if src_right else len(submitted)
    capture = submitted[begin:end]
    dst_left, dst_right = replacement.split("%s")
    return dst_left + capture + dst_right

def is_numeric_unit_noise(text):
    s = text.strip()
    if len(s) < 2 or not s[-1].isascii() or not s[-1].isalpha():
        return False
    saw_digit = False
    for c in s[:-1]:
        if c.isdigit():
            saw_digit = True
        elif c not in "+-.,":
            return False
    return saw_digit

def parse_wrapped_at(text, pos):
    if not text.startswith("~~{", pos):
        return None
    close = text.find("}~~", pos + 3)
    if close < 0 or close == pos + 3:
        return None
    name = text[pos + 3:close]
    if any((ord(c) < 0x21 or ord(c) > 0x7e or c in "{}~") for c in name):
        return None
    return close + 3


def split_ap1(text):
    i = 0
    while i < len(text) and text[i] in " \t": i += 1
    leading = 0
    while True:
        end = parse_wrapped_at(text, i)
        if end is None: break
        leading += 1
        i = end
        while i < len(text) and text[i] in " \t": i += 1
    body_begin = i if leading else 0
    # The model only needs to establish that bare {TOKEN} is not AP1.
    return body_begin if leading else None


def looks_control_island(value):
    value = value.strip()
    if not value or len(value) > 64:
        return False
    if "~~{" in value or ("{" in value and "}" in value):
        return True
    if any(0xE000 <= ord(c) <= 0xF8FF for c in value):
        return True
    if len(value) > 24:
        return False
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 +/|&-()")
    ascii_marker = any(("A" <= c <= "Z") or c.isdigit() for c in value)
    return ascii_marker and all((c in allowed) or ord(c) >= 0x80 for c in value)


def parse_oltx_language(blob: bytes, language=b"en"):
    assert blob[:4] == b"OLTX"
    block_off = block_size = 0
    for i in range(8):
        off = 0x1C + i * 24
        if blob[off:off+2].lower() == language.lower():
            block_off, block_size = struct.unpack_from("<II", blob, off + 16)
            break
    assert block_off and block_size >= 8
    count, stride = struct.unpack_from("<II", blob, block_off)
    assert stride == 8
    out = {}
    for i in range(count):
        h, rel = struct.unpack_from("<II", blob, block_off + 8 + i * 8)
        p = block_off + rel
        units = bytearray()
        while blob[p:p+2] != b"\0\0":
            units += blob[p:p+2]
            p += 2
        out[h] = units.decode("utf-16le")
    return out




def discover_oltx_key_block_model(blob: bytes):
    """Model the R10.1 key-table recovery contract without game text."""
    directory_end = 0x1C + 8 * 24
    key_hint = struct.unpack_from("<I", blob, 0x18)[0]
    header_base = struct.unpack_from("<I", blob, 0x08)[0]
    first_lang = min(
        struct.unpack_from("<I", blob, 0x1C + i * 24 + 16)[0]
        for i in range(8)
        if struct.unpack_from("<I", blob, 0x1C + i * 24 + 16)[0]
    )

    def valid(at):
        if at < directory_end or at + 8 > first_lang:
            return False
        count, stride = struct.unpack_from("<II", blob, at)
        if not count or stride != 8 or at + 8 + count * 8 > first_lang:
            return False
        table_end = at + 8 + count * 8
        for i in range(count):
            _, rel = struct.unpack_from("<II", blob, at + 8 + i * 8)
            pos = at + rel
            if pos < table_end or pos >= first_lang:
                return False
            end = blob.find(b"\0", pos, first_lang)
            if end < 0 or end == pos:
                return False
            try:
                blob[pos:end].decode("ascii")
            except UnicodeDecodeError:
                return False
        return True

    candidates = [
        (key_hint, "HEADER_ABSOLUTE"),
        (key_hint + header_base, "HEADER_PLUS_BASE"),
        (directory_end, "DIRECTORY_END"),
    ]
    for at, mode in candidates:
        if valid(at):
            return at, mode
    for at in range(directory_end, first_lang - 7, 4):
        if valid(at):
            return at, "VALIDATED_SCAN"
    return None


def select_authoritative_loc(values, indexed):
    distinct = []
    for value in values:
        if value not in distinct:
            distinct.append(value)
    if not distinct:
        return None
    if len(distinct) == 1:
        return distinct[0]
    if indexed in distinct:
        return indexed
    return None


def canonical_source(text):
    value = norm(text)
    colon = value.find(":", 1)
    if colon < 0 or colon >= 64:
        return value
    speaker = value[:colon].rstrip()
    body = value[colon + 1:].lstrip()
    if not speaker or not body or not any(c.isascii() and c.isalpha() for c in speaker):
        return value
    return speaker + ": " + body


def has_visible_ascii_outside_markup(text):
    i = 0
    while i < len(text):
        if text.startswith("~~{", i):
            close = text.find("}~~", i + 3)
            if close >= 0:
                i = close + 3
                continue
        if text[i] == "{":
            close = text.find("}", i + 1)
            if close > i + 1:
                i = close + 1
                continue
        if text[i] == "~":
            close = text.find("~", i + 1, min(len(text), i + 25))
            if close > i + 1:
                i = close + 1
                continue
        if text[i] == "%":
            end = i + 1
            conversions = set("diuoxXfFeEgGaAcCsSpn")
            while end < len(text) and end - i <= 24:
                if text[end] == "%":
                    break
                if text[end] in conversions:
                    i = end + 1
                    break
                end += 1
            if i > end:
                continue
        if text[i].isascii() and text[i].isalpha():
            return True
        i += 1
    return False


def is_changing_numeric_status(text):
    if ":" not in text:
        return False
    prefix, suffix = text.split(":", 1)
    if not prefix or not any(c.isascii() and c.isalpha() for c in prefix):
        return False
    if any(not (c.isascii() and (c.isalpha() or c in " _-")) for c in prefix):
        return False
    saw_digit = False
    for c in suffix:
        if c.isdigit():
            saw_digit = True
        elif c not in " \t.,:-+/%":
            return False
    return saw_digit

def uidialogue_post_copy(source, replacement, capacity=320):
    # 模拟最终对象缓冲：原函数已经完成，R5 只允许完整 replacement
    # 在 capacity-1 内覆盖；过长必须保持 source，禁止截断。
    if replacement is None or replacement == source:
        return source, False
    if len(replacement) >= capacity:
        return source, False
    return replacement, True


def count_ascii_letters(text):
    return sum(1 for c in text if c.isascii() and c.isalpha())


def count_cjk(text):
    return sum(1 for c in text if 0x3400 <= ord(c) <= 0x4DBF or
               0x4E00 <= ord(c) <= 0x9FFF or 0xF900 <= ord(c) <= 0xFAFF)


def safe_source_fallback(source, replacement):
    if not source or not replacement or source == replacement:
        return False
    if any(c.isascii() and c.isalpha() for c in source) and count_cjk(replacement) == 0:
        return False
    src_ascii = count_ascii_letters(source)
    if src_ascii < 24:
        return True
    dst_ascii = count_ascii_letters(replacement)
    dst_cjk = count_cjk(replacement)
    if dst_ascii >= 16 and dst_ascii * 100 >= src_ascii * 70 and dst_cjk <= 4:
        return False
    return True


def wrapped_token_ending_at(text, end):
    if end < 7 or not text[:end].endswith("}~~"):
        return False
    search = max(0, end - 70)
    begin = text.rfind("~~{", search, end)
    return begin >= 0 and parse_wrapped_at(text, begin) == end


def structured_occurrence(text, needle):
    if not needle or len(needle) > len(text):
        return None
    if text == needle:
        return 0
    localized_prefix_allowed = count_cjk(text) > 0
    selected = None
    search = 0
    while search < len(text):
        at = text.find(needle, search)
        if at < 0:
            break
        before = at
        while before > 0 and text[before - 1] in " \t\r\n":
            before -= 1
        if before == 0 or wrapped_token_ending_at(text, before):
            strong_before = True
        else:
            marker = text[before - 1]
            strong_before = marker in ":：" or (localized_prefix_allowed and marker in "-/")
        after = at + len(needle)
        while after < len(text) and text[after] in " \t\r\n":
            after += 1
        token_after = after < len(text) and parse_wrapped_at(text, after) is not None
        strong_after = (after == len(text) or token_after or text[after] in ":：" or
                        (localized_prefix_allowed and text[after] in "-/"))
        if strong_before and strong_after:
            if selected is not None:
                return None
            selected = at
        search = at + 1
    return selected




def precise_printf_capture_apply(container_source, submitted, value_source, value_dest):
    # 模拟 R8 ORIGINAL_KEY_CONTEXT_PAIR：value_source 必须恰好是另一条最近
    # precise printf 模板匹配出的完整 capture，绝不按普通 substring 处理。
    if container_source.count("%s") != 1:
        return None
    left, right = container_source.split("%s")
    if not submitted.startswith(left) or not submitted.endswith(right):
        return None
    end = len(submitted) - len(right) if right else len(submitted)
    capture = submitted[len(left):end]
    if capture != value_source:
        return None
    return left + value_dest + right

def neutral_speaker(text):
    if not text or len(text) > 4:
        return False
    marker = False
    for c in text:
        if ("A" <= c <= "Z") or c.isdigit():
            marker = True
            continue
        if c in ".-_":
            continue
        return False
    return marker


def printf_agreement(template_pairs, submitted):
    outputs = []
    for source, replacement in template_pairs:
        if source.count("%s") != 1 or replacement.count("%s") != 1:
            continue
        left, right = source.split("%s")
        if len(left) + len(right) < 3:
            continue
        if not submitted.startswith(left) or not submitted.endswith(right):
            continue
        end = len(submitted) - len(right) if right else len(submitted)
        capture = submitted[len(left):end]
        dleft, dright = replacement.split("%s")
        outputs.append(dleft + capture + dright)
    if not outputs or any(value != outputs[0] for value in outputs[1:]):
        return None
    return outputs[0]


class ModelTests(unittest.TestCase):
    def test_ambiguous_never_first_candidate(self):
        m, bad = unique_map([("SAME", "DST_A"), ("SAME", "DST_B"), ("ONE", "DST_C")])
        self.assertNotIn("SAME", m)
        self.assertIn("SAME", bad)
        self.assertEqual(m["ONE"], "DST_C")

    def test_data_driven_short_prefix_alias(self):
        aliases = prefix_alias([("Ab", "X"), ("Ab", "XY"), ("Longer", "P"), ("Longer", "PQ")])
        self.assertEqual(aliases, {"Ab": "XY"})

    def test_non_prefix_ambiguity_stays_excluded(self):
        self.assertEqual(prefix_alias([("Ab", "X"), ("Ab", "Y")]), {})

    def test_ap1_accepts_only_wrapped_form(self):
        self.assertIsNotNone(split_ap1("~~{BTN}~~ BODY"))
        self.assertIsNone(split_ap1("{BTN} BODY"))

    def test_control_island_rejects_ordinary_prose(self):
        self.assertFalse(looks_control_island("friend"))
        self.assertFalse(looks_control_island("working with our enemies"))
        self.assertFalse(looks_control_island("\u4E00"))
        self.assertTrue(looks_control_island("L"))
        self.assertTrue(looks_control_island("L or R"))
        self.assertTrue(looks_control_island("L \u4E00 R"))
        self.assertTrue(looks_control_island("~~{BTN}~~"))
        self.assertTrue(looks_control_island("\ue001"))

    def test_short_recent_mapping_not_substring_applied(self):
        source = "or"
        current = "friend or working"
        may_substring_replace = current == source or len(source) >= 4
        self.assertFalse(may_substring_replace)

    def test_data_driven_base_label_alias(self):
        aliases = base_label_alias([
            ("CityName", "BASE"),
            ("CityName", "BASE_A"),
            ("CityName", "BASE_B"),
        ])
        self.assertEqual(aliases, {"CityName": "BASE"})

    def test_base_label_conflict_stays_excluded(self):
        aliases = base_label_alias([
            ("CityName", "LEFT"),
            ("CityName", "RIGHT"),
        ])
        self.assertEqual(aliases, {})

    def test_normalized_alias_uses_all_records(self):
        safe, bad = normalized_all([("Speaker ", "DST"), ("Speaker", "DST")])
        self.assertEqual(safe["Speaker"], "DST")
        self.assertNotIn("Speaker", bad)
        safe, bad = normalized_all([("Speaker ", "DST_A"), ("Speaker", "DST_B")])
        self.assertNotIn("Speaker", safe)
        self.assertIn("Speaker", bad)

    def test_compound_segments_are_all_or_nothing(self):
        mapping = {"LEFT": "DST_L", "RIGHT": "DST_R", "DONE": "DST_D", "TARGET": "DST_T"}
        self.assertEqual(resolve_compound("LEFT / RIGHT", mapping), "DST_L / DST_R")
        self.assertEqual(resolve_compound("DONE - TARGET", mapping), "DST_D - DST_T")
        self.assertIsNone(resolve_compound("LEFT / MISSING", mapping))


    def test_precise_printf_uses_selected_key_template(self):
        self.assertEqual(
            precise_printf_apply("STATE - %s", "DST:%s", "STATE - ARG"),
            "DST:ARG")
        self.assertIsNone(
            precise_printf_apply("STATE - %s", "DST:%s", "OTHER - ARG"))

    def test_cross_thread_deferred_requires_unique_agreement(self):
        self.assertEqual(
            cross_thread_select("SRC", [("SRC", "DST", 1), ("SRC", "DST", 2)]),
            "DST")
        self.assertIsNone(
            cross_thread_select("SRC", [("SRC", "DST_A", 1), ("SRC", "DST_B", 2)]))
        self.assertEqual(
            cross_thread_select("PFX LONGSRC SFX", [("LONGSRC", "DST", 3)]),
            "PFX DST SFX")
        self.assertIsNone(
            cross_thread_select("AAAA BBBB", [("AAAA", "DST", 1), ("BBBB", "DST", 2)]))

    def test_numeric_unit_noise_filter(self):
        for value in ["49m", "143m", "1m", "-12.5m"]:
            self.assertTrue(is_numeric_unit_noise(value))
        for value in ["Mission Failed", "S.A.V.", "MP7", "2560x1440", "Mouse1"]:
            self.assertFalse(is_numeric_unit_noise(value))

    def test_uidialogue_post_copy_is_all_or_nothing(self):
        value, changed = uidialogue_post_copy("SRC", "DST")
        self.assertEqual((value, changed), ("DST", True))
        too_long = "X" * 320
        value, changed = uidialogue_post_copy("SRC", too_long)
        self.assertEqual((value, changed), ("SRC", False))
        value, changed = uidialogue_post_copy("SRC", "SRC")
        self.assertEqual((value, changed), ("SRC", False))

    def test_r7_single_current_loc_overrides_stale_index_snapshot(self):
        self.assertEqual(select_authoritative_loc(["CUR_DST"], "OLD_DST"), "CUR_DST")

    def test_r7_multi_loc_uses_index_only_as_context_fingerprint(self):
        self.assertEqual(select_authoritative_loc(["DST_A", "DST_B"], "DST_B"), "DST_B")
        self.assertIsNone(select_authoritative_loc(["NEW_A", "NEW_B"], "OLD_DST"))
        self.assertIsNone(select_authoritative_loc([], "OLD_DST"))

    def test_r7_speaker_separator_canonicalization(self):
        self.assertEqual(canonical_source("Speaker : Body text"), "Speaker: Body text")
        self.assertEqual(canonical_source("Speaker:Body text"), "Speaker: Body text")
        self.assertEqual(canonical_source("Speaker:   Body   text"), "Speaker: Body text")

    def test_r7_diagnostics_ignore_control_token_names(self):
        self.assertFalse(has_visible_ascii_outside_markup("~~{BUTTON}~~ 测试"))
        self.assertFalse(has_visible_ascii_outside_markup("测试 {BUTTON}"))
        self.assertFalse(has_visible_ascii_outside_markup("测试 ~ffffffff~~~{BUTTON}~~"))
        self.assertFalse(has_visible_ascii_outside_markup("%d / %d 情报"))
        self.assertTrue(has_visible_ascii_outside_markup("English ~~{BUTTON}~~"))
        self.assertTrue(has_visible_ascii_outside_markup("MP7"))

    def test_r7_translated_numeric_status_diagnostic_filter(self):
        self.assertTrue(is_changing_numeric_status("Timer: 00:14.983"))
        self.assertTrue(is_changing_numeric_status("Value: -12.5%"))
        self.assertFalse(is_changing_numeric_status("Objective: TARGET"))
        self.assertFalse(is_changing_numeric_status("MP7"))

    def test_r8_partial_english_fallback_guard(self):
        source = "This is a deliberately long English source sentence for the guard"
        polluted = "This is a deliberately long English source sentence for the 防 guard"
        self.assertFalse(safe_source_fallback(source, polluted))
        self.assertTrue(safe_source_fallback(source, "译文 P99"))

    def test_r8_structured_occurrence_rejects_mid_sentence_word(self):
        self.assertIsNone(structured_occurrence("friend or working", "or"))
        self.assertEqual(structured_occurrence("Speaker: BODY", "BODY"), 9)
        self.assertEqual(structured_occurrence("前缀：BODY", "BODY"), 3)
        self.assertIsNone(structured_occurrence("Objective Complete - BODY", "BODY"))
        self.assertEqual(structured_occurrence("已完成 - BODY", "BODY"), 6)
        self.assertEqual(structured_occurrence("BODY   ~~{CAPSLOCK}~~", "BODY"), 0)
        self.assertEqual(structured_occurrence("~~{RETURN}~~   BODY", "BODY"), 15)

    def test_r8_recent_precise_printf_capture_pair(self):
        self.assertEqual(
            precise_printf_capture_apply(
                "Objective Complete - %s",
                "Objective Complete - ARG",
                "ARG", "DST"),
            "Objective Complete - DST")
        self.assertIsNone(
            precise_printf_capture_apply(
                "Objective Complete - %s",
                "ordinary prose ARG here",
                "ARG", "DST"))

    def test_r8_neutral_speaker_identifier(self):
        self.assertTrue(neutral_speaker("M"))
        self.assertTrue(neutral_speaker("Q-1"))
        self.assertFalse(neutral_speaker("Bond"))
        self.assertFalse(neutral_speaker("m"))

    def test_r8_printf_runtime_agreement(self):
        pairs = [("STATE - %s", "结果：%s"), ("STATE - %s", "结果：%s")]
        self.assertEqual(printf_agreement(pairs, "STATE - ARG"), "结果：ARG")
        conflict = [("STATE - %s", "结果A：%s"), ("STATE - %s", "结果B：%s")]
        self.assertIsNone(printf_agreement(conflict, "STATE - ARG"))

    def test_r8_fixed_point_structured_tail_model(self):
        # 第一轮只得到已本地化 carrier + 英语 tail；第二轮只能在强结构边界
        # 把精确上下文选中的 tail 替换进去，普通句中词不会被处理。
        current = "已更新：TAIL"
        at = structured_occurrence(current, "TAIL")
        self.assertEqual(at, 4)
        current = current[:at] + "译文" + current[at + 4:]
        self.assertEqual(current, "已更新：译文")
        self.assertIsNone(structured_occurrence("prose tail inside prose", "tail"))

    def test_synthetic_bli3_shape(self):
        records = [(0x1, "SRC_A", "DST_A", 0), (0x2, "SRC_B", "DST_B", 1)]
        payload = bytearray()
        for h, s, r, f in records:
            sb = s.encode("utf-16le"); rb = r.encode("utf-16le")
            payload += struct.pack("<IIII", h, len(sb)//2, len(rb)//2, f) + sb + rb
        blob = struct.pack("<8sIIIIII", b"BSCNLI3\0", 3, 32, len(records), len(payload), len(records), 0) + payload
        magic, ver, hs, count, size, changed, reserved = struct.unpack_from("<8sIIIIII", blob, 0)
        self.assertEqual((magic, ver, hs, count, size, changed, reserved),
                         (b"BSCNLI3\0", 3, 32, 2, len(payload), 2, 0))


    def test_r10_1_oltx_key_hint_plus_header_base(self):
        # Historical OLTX shape: header+0x18 is 0xCC while the actual key block
        # begins at 0xDC.  The 0x10 header-base field recovers the real table.
        directory_end = 0x1C + 8 * 24
        self.assertEqual(directory_end, 0xDC)
        key_block = 0xDC
        key = b"NPC_CM1_LINE_0\0"
        key_table_size = 8 + 8
        key_rel = key_table_size
        lang_block = key_block + key_table_size + len(key)
        text = "DST".encode("utf-16le") + b"\0\0"
        lang_table_size = 8 + 8
        blob = bytearray(lang_block + lang_table_size + len(text))
        blob[:4] = b"OLTX"
        struct.pack_into("<I", blob, 0x08, 0x10)
        struct.pack_into("<I", blob, 0x18, 0xCC)
        rec = 0x1C
        blob[rec:rec+2] = b"en"
        struct.pack_into("<II", blob, rec + 16, lang_block, lang_table_size + len(text))
        struct.pack_into("<II", blob, key_block, 1, 8)
        struct.pack_into("<II", blob, key_block + 8, 0x12345678, key_rel)
        blob[key_block + key_rel:key_block + key_rel + len(key)] = key
        struct.pack_into("<II", blob, lang_block, 1, 8)
        struct.pack_into("<II", blob, lang_block + 8, 0x12345678, lang_table_size)
        blob[lang_block + lang_table_size:] = text
        self.assertEqual(discover_oltx_key_block_model(bytes(blob)),
                         (0xDC, "HEADER_PLUS_BASE"))

    def test_synthetic_oltx_en_block(self):
        header = bytearray(0x1C + 8 * 24)
        header[:4] = b"OLTX"
        block_off = len(header)
        text = "DST".encode("utf-16le") + b"\0\0"
        table_size = 8 + 8
        block = bytearray(table_size + len(text))
        struct.pack_into("<II", block, 0, 1, 8)
        struct.pack_into("<II", block, 8, 0x12345678, table_size)
        block[table_size:] = text
        rec = 0x1C
        header[rec:rec+2] = b"en"
        struct.pack_into("<II", header, rec + 16, block_off, len(block))
        parsed = parse_oltx_language(bytes(header + block))
        self.assertEqual(parsed, {0x12345678: "DST"})

    def test_pointer_difference_is_not_content_translation(self):
        source = "SAME TEXT"
        effective = "".join(["SAME", " TEXT"])
        self.assertIsNot(source, effective)
        self.assertEqual(source, effective)
        self.assertFalse(source != effective)

    def test_speaker_translation_requires_body(self):
        speaker = "DST_SPEAKER"
        body = None
        translated = None if body is None else speaker + ": " + body
        self.assertIsNone(translated)

    def test_all_record_printf_accepts_only_unique_replacement_template(self):
        grouped = {}
        ambiguous = set()
        for src, dst in [("STATE %s", "DST %s"), ("STATE %s", "DST %s"),
                         ("BAD %s", "A %s"), ("BAD %s", "B %s")]:
            if src in ambiguous:
                continue
            if src not in grouped:
                grouped[src] = dst
            elif grouped[src] != dst:
                del grouped[src]
                ambiguous.add(src)
        self.assertEqual(grouped["STATE %s"], "DST %s")
        self.assertNotIn("BAD %s", grouped)
        self.assertIn("BAD %s", ambiguous)

if __name__ == "__main__":
    unittest.main()
