#!/usr/bin/env python3
from __future__ import annotations
import json, struct, sys
from pathlib import Path

def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]

def main():
    if len(sys.argv)!=2:
        print('用法: verify_pe.py <BloodstoneCN.asi>'); return 2
    p=Path(sys.argv[1]); b=p.read_bytes()
    if len(b)<0x100 or b[:2]!=b'MZ':
        print('[FAIL] 非 MZ/PE 文件'); return 1
    pe=u32(b,0x3c)
    if pe+0x100>len(b) or b[pe:pe+4]!=b'PE\0\0':
        print('[FAIL] PE 签名无效'); return 1
    fh=pe+4; machine=u16(b,fh); sections=u16(b,fh+2); chars=u16(b,fh+18)
    opt=fh+20; magic=u16(b,opt); entry=u32(b,opt+16); image_size=u32(b,opt+56); dllchars=u16(b,opt+70)
    sec_off=opt+u16(b,fh+16)
    sec=[]
    for i in range(sections):
        o=sec_off+i*40
        name=b[o:o+8].split(b'\0',1)[0].decode('ascii','replace')
        vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8)
        sec.append({'name':name,'virtual_size':vs,'virtual_address':va,'raw_size':rs,'raw_pointer':rp})
    def rva_to_off(rva):
        for s in sec:
            size=max(s['virtual_size'],s['raw_size'])
            if s['virtual_address']<=rva<s['virtual_address']+size:
                return s['raw_pointer']+(rva-s['virtual_address'])
        return rva if rva<len(b) else None
    imports=[]
    # PE32 data directory starts at OptionalHeader+96
    imp_rva=u32(b,opt+96+8*1); imp_size=u32(b,opt+96+8*1+4)
    if imp_rva:
        off=rva_to_off(imp_rva)
        if off is not None:
            for _ in range(128):
                if off+20>len(b): break
                orig,ts,fc,name_rva,first=struct.unpack_from('<IIIII',b,off)
                if not any((orig,ts,fc,name_rva,first)): break
                no=rva_to_off(name_rva)
                if no is not None and no<len(b):
                    end=b.find(b'\0',no,min(len(b),no+260))
                    if end!=-1: imports.append(b[no:end].decode('ascii','replace'))
                off+=20
    ok=machine==0x14c and magic==0x10b and bool(chars & 0x2000) and entry!=0
    size=len(b)
    if size>220*1024: size_state='ALARM_GT_220K'
    elif size>160*1024: size_state='WARN_GT_160K'
    else: size_state='NORMAL_RANGE'
    report={'status':'PASS' if ok else 'FAIL','path':str(p),'size':size,'size_state':size_state,
            'machine':machine,'optional_magic':magic,'dll':bool(chars&0x2000),'entry_rva':entry,
            'image_size':image_size,'dll_characteristics':dllchars,'imports':imports,'sections':sec}
    print(json.dumps(report,ensure_ascii=False,indent=2))
    if size_state!='NORMAL_RANGE':
        print('[警告] 文件体积进入回归警戒区；请检查 CRT/STL/链接选项，不要只因能启动就忽略。')
    return 0 if ok else 1
if __name__=='__main__': raise SystemExit(main())
