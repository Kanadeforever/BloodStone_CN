# -*- coding: utf-8 -*-
"""读取用户授权的 Bond 明文分析 PE，核对五条路径及原生逐字随机效果。"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

BASE = Path(__file__).resolve().parents[1]
PROFILE = BASE / "config" / "profile.json"


def read_sections(data: bytes) -> tuple[int, list[tuple[int, int, int]]]:
    """把 PE 中的虚拟地址转换为磁盘偏移，不能直接把 RVA 当作文件偏移。"""
    if len(data) < 0x200 or data[:2] != b"MZ":
        raise ValueError("文件不是有效的 MZ 可执行映像")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("PE 头签名不正确")
    if struct.unpack_from("<H", data, pe + 4)[0] != 0x14C:
        raise ValueError("目标必须为 PE32/x86")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = pe + 24
    base = struct.unpack_from("<I", data, optional + 28)[0]
    header_length = struct.unpack_from("<H", data, pe + 20)[0]
    table = optional + header_length
    mapping = []
    for index in range(count):
        item = table + 40 * index
        memory_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, item + 8)
        if raw_size:
            mapping.append((base + rva, base + rva + min(memory_size, raw_size), raw_offset))
    return base, mapping


def offset_of(virtual_address: int, sections: list[tuple[int, int, int]]) -> int:
    """精确寻找指定 VA 对应的文件字节，越界立即报错，不能误读相邻区段。"""
    for start, end, raw_offset in sections:
        if start <= virtual_address < end:
            return raw_offset + virtual_address - start
    raise ValueError(f"目标 VA 不在有效的原始区段：0x{virtual_address:08X}")


def verify_dump(binary: bytes, profile: dict) -> dict:
    """只核对转储真实性和静态锚点，不推断 Windows 实机能否成功挂载。"""
    digest = hashlib.sha256(binary).hexdigest()
    # 不同转储重建流程会生成不同的 PE 容器字节，已验证的两份样本都必须
    # 对照五条指令和五份原始路径逐一验证，不能仅依据文件名认定版本。
    known = profile.get("known_dump_sha256", [profile["sha256"]])
    if digest not in known:
        raise ValueError("转储 SHA-256 与当前支持的游戏版本不同")
    base, sections = read_sections(binary)
    if base != int(profile["image_base"], 16):
        raise ValueError("镜像基址与补丁参数不匹配")
    verified = []
    for entry in profile["sites"]:
        instruction_address = int(entry["instruction_va"], 16)
        literal_address = int(entry["target_va"], 16)
        instruction = bytes.fromhex(entry["instruction_bytes"])
        found_instruction = binary[offset_of(instruction_address, sections):][:len(instruction)]
        if found_instruction != instruction:
            raise ValueError(f'{entry["name"]} 的 push 指令不匹配')
        if instruction != b"\x68" + struct.pack("<I", literal_address):
            raise ValueError(f'{entry["name"]} 的字符串指针地址不匹配')
        text = entry["string"].encode("ascii") + b"\x00"
        found_text = binary[offset_of(literal_address, sections):][:len(text)]
        if found_text != text:
            raise ValueError(f'{entry["name"]} 的原始路径文本不匹配')
        verified.append(entry["name"])

    # 文字效果与五处资源重定向相互独立，只有完整匹配才允许在运行时关闭随机字母。
    # 分支表的四个入口分别处理透明度、颜色、随机字母、旋转；这里只允许改第三项。
    effect = profile.get("text_effect")
    effect_verified = False
    if effect:
        checks = (
            ("dispatch_va", "dispatch_bytes"),
            ("random_branch_va", "random_branch_bytes"),
            ("ascii_clamp_va", "ascii_clamp_bytes"),
            ("disabled_target_va", "skip_branch_bytes"),
        )
        for address_key, bytes_key in checks:
            address = int(effect[address_key], 16)
            expected = bytes.fromhex(effect[bytes_key])
            found = binary[offset_of(address, sections):][:len(expected)]
            if found != expected:
                raise ValueError(f"原生随机字母效果签名不匹配：{address_key}")
        table_address = int(effect["jump_table_va"], 16)
        targets = [int(address, 16) for address in effect["original_targets"]]
        actual = struct.unpack_from("<4I", binary, offset_of(table_address, sections))
        if list(actual) != targets:
            raise ValueError("原生逐字动画四分支跳转表不匹配")
        if targets[2] != int(effect["random_branch_va"], 16):
            raise ValueError("随机字母分支不是原生跳转表的第三项")
        effect_verified = True
    return {"verified": verified, "text_effect_verified": effect_verified,
            "sha256": digest, "image_base": f"0x{base:08X}"}


def main() -> None:
    parser = argparse.ArgumentParser(description="验证 Bond 明文转储、五处原生资源入口及随机字母效果")
    parser.add_argument("--dump", type=Path, required=True, help="用户自身运行时转储生成的明文分析 PE")
    arguments = parser.parse_args()
    profile = json.loads(PROFILE.read_text(encoding="utf-8"))
    result = verify_dump(arguments.dump.read_bytes(), profile)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
