# -*- coding: utf-8 -*-
"""可选只读核查：用户上传的脱壳分析 PE 与历史中文 LOC 是否匹配既有事实。"""
import argparse
import struct
import zipfile
from pathlib import Path


def read_va(data,va):
    """将 PE32 VA 映射到磁盘字节位置；不执行或修改游戏二进制。"""
    pe=struct.unpack_from('<I',data,0x3c)[0]
    assert data[:2]==b'MZ' and data[pe:pe+4]==b'PE\0\0'
    base=struct.unpack_from('<I',data,pe+24+28)[0]
    sect=pe+24+struct.unpack_from('<H',data,pe+20)[0]
    for i in range(struct.unpack_from('<H',data,pe+6)[0]):
        f=sect+i*40
        virtual_size,rva,raw_size,file_offset=struct.unpack_from('<IIII',data,f+8)
        if base+rva<=va<base+rva+min(virtual_size,raw_size):
            return file_offset+va-base-rva
    raise ValueError('VA 不在文件区段：%08X'%va)


def check_bond(path):
    """确认 NPC 钩子地址真的是指向已知目标的 x86 CALL。"""
    with zipfile.ZipFile(path) as z:
        name=next((x for x in z.namelist() if x.endswith('Bond.unpacked_analysis.exe')),None)
        if not name:raise ValueError('压缩包内没有明文分析 PE')
        data=z.read(name)
    pe=struct.unpack_from('<I',data,0x3c)[0]
    stamp=struct.unpack_from('<I',data,pe+8)[0]
    size=struct.unpack_from('<I',data,pe+24+56)[0]
    assert stamp==0x4ca36144 and size==0x10cc000,(hex(stamp),hex(size))
    site=0x0093A2A1
    raw=read_va(data,site)
    assert data[raw]==0xE8, 'Dialogue CALL 明文字节不是 E8'
    target=(site+5+struct.unpack_from('<i',data,raw+1)[0])&0xffffffff
    assert target==0x00F12820, 'Dialogue CALL 目标地址变动：%08X'%target
    print('[通过] 原生 Dialogue CALL 明文验证：%08X → %08X'%(site,target))
    font_call=0x00E42C21
    font_raw=read_va(data,font_call)
    assert data[font_raw]==0xE8, '原生字体资源 CALL 不是 E8'
    print('[通过] 字体资源调用指令明文验证：%08X'%font_call)


def check_loc(path):
    """只检查源资源结构，不能将历史样本直接当成 v0.9 新发行资源。"""
    with zipfile.ZipFile(path) as z:
        names=[n for n in z.namelist() if n.lower().endswith('_script_dialogue.loc')]
        if len(names)<20:raise ValueError('对白 LOC 数量不足：%u'%len(names))
        for name in names:
            data=z.read(name)
            if data[:4]!=b'OLTX':raise ValueError('不是 OLTX：'+name)
            if len(data)<0x1c+8*24:raise ValueError('LOC 太短：'+name)
    print('[通过] 历史 LOC 的原生对白文件及格式检查：',len(names))


def main():
    parser=argparse.ArgumentParser(description='只读验证上传的 Bond 转储与 LOC 样本')
    parser.add_argument('--bond-zip',type=Path)
    parser.add_argument('--loc-zip',type=Path)
    args=parser.parse_args()
    if args.bond_zip:check_bond(args.bond_zip)
    if args.loc_zip:check_loc(args.loc_zip)
    if not args.bond_zip and not args.loc_zip:parser.error('至少提供一个参考 ZIP')

if __name__=='__main__':main()
