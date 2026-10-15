# -*- coding: utf-8 -*-
"""静态核验 Win32/x86 栈探测例程，避免重新引入只探测、不调整 ESP 的实现。"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

# 当前编译器的辅助函数以保存三个易失寄存器开始，并在恢复旧寄存器之前
# 把原始返回地址搬到真正分配好的新栈帧。两端签名必须同时存在。
PROLOGUE = bytes.fromhex('50 51 52 8D 4C 24 10')
EPILOGUE = bytes.fromhex('89 D4 5A 59 58 C3')


def text_section(binary: bytes) -> tuple[int, bytes]:
    """读取 PE 中真实 .text 虚拟地址及文件数据，不把磁盘偏移误认为运行时地址。"""
    if len(binary) < 512 or binary[:2] != b'MZ':
        raise ValueError('文件不是有效 PE')
    pe = struct.unpack_from('<I', binary, 0x3C)[0]
    if binary[pe:pe+4] != b'PE\0\0':
        raise ValueError('PE 签名不正确')
    if struct.unpack_from('<H', binary, pe+4)[0] != 0x14C:
        raise ValueError('不是 Win32/i386')
    base = struct.unpack_from('<I', binary, pe+24+28)[0]
    table = pe+24+struct.unpack_from('<H', binary, pe+20)[0]
    sections = struct.unpack_from('<H', binary, pe+6)[0]
    for index in range(sections):
        pos = table+index*40
        name = binary[pos:pos+8].split(b'\0',1)[0]
        virtual_size, rva, length, offset = struct.unpack_from('<IIII', binary, pos+8)
        if name == b'.text':
            return base+rva, binary[offset:offset+length]
    raise ValueError('缺少 .text 代码节')


def verify(binary: bytes) -> dict:
    """核对辅助函数、编译器的实际调用站点，以及数学上的栈迁移目标。"""
    base, code = text_section(binary)
    location = code.find(PROLOGUE)
    if location < 0:
        raise ValueError('缺少预期的寄存器保存和栈帧入口')
    end = code.find(EPILOGUE, location+len(PROLOGUE), location+180)
    if end < 0:
        raise ValueError('栈探测函数未转移 ESP 或未恢复三个寄存器')
    helper = base+location
    segment = code[location:end+len(EPILOGUE)]
    if bytes.fromhex('8D 51 F0') not in segment:
        raise ValueError('未找到新栈顶上用于恢复寄存器的 16 字节区')
    if segment.count(bytes.fromhex('84 01')):
        raise ValueError('意外的测试指令；请核查栈探测版本')
    calls = []
    for i in range(len(code)-4):
        if code[i] != 0xE8:
            continue
        displacement = struct.unpack_from('<i', code, i+1)[0]
        if base+i+5+displacement == helper:
            calls.append(base+i)
    if len(calls) < 2:
        raise ValueError(f'至少应有两处由编译器生成的大栈帧调用，目前 {len(calls)} 处')
    # 编译器 CALL 把返回地址放在 S-4，保存三个寄存器后是 S-16；
    # 辅助函数搬到 T-16 ... T-4，然后 pop 三次加 ret 使最终 ESP=T=S-N。
    models = []
    for frame in (0x1110, 0x164C, 0x1000, 0x1010):
        initial_esp = 0x00700000
        helper_saved_esp = initial_esp-4-12
        new_stack = initial_esp-frame-16
        final_esp = new_stack+3*4+4
        assert final_esp == initial_esp-frame
        models.append({'frame_bytes': frame, 'final_esp': hex(final_esp),
                       'saved_esp': hex(helper_saved_esp)})
    return {'valid': True, 'helper_va': hex(helper), 'compiler_call_sites':
            [hex(address) for address in calls], 'stack_models': models}


def main() -> None:
    parser=argparse.ArgumentParser(description='验证 PE32 栈探测辅助函数的 ESP 调整和调用站点')
    parser.add_argument('asi',type=Path)
    args=parser.parse_args()
    print(json.dumps(verify(args.asi.read_bytes()),ensure_ascii=False,indent=2))


if __name__ == '__main__':
    main()
