# -*- coding: utf-8 -*-
"""BloodstoneCN 字库创建工具的独立入口。

仅从本工具 input 中读取原生模板、用户 TTF 字体和译文 JSON。
三种字符范围：精确译文、GB2312 一级常用汉字、GBK 所有合法双字节字符。
新字形可由 Windows GDI 从指定字体临时栅格化；无字体时仅精准模式
允许使用用户自行提供的 BFA 作为回退，并严格检查 BFA 是否缺字。
公开发行包不附带任何模板、字形位图或翻译文本。
全部输出只写到本工具 output，不触碰游戏、系统字体和其他文件夹。
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import sys
import os
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = Path(__file__).resolve().parent
if not (CORE / 'native_font.py').is_file() or not (CORE / 'raster_gdi.py').is_file():
    raise SystemExit('[错误] 字库生成核心缺失，请完整解压 toolbox 文件夹')
sys.path.insert(0,str(CORE))
from native_font import (ChineseAtlas, SourceGlyph, NativeDat, NativeBpr,
                         make_native_candidate, make_packed_glyphs)
from raster_gdi import WindowsGlyphRenderer, CELL_W, CELL_H


def inside(path: str | Path) -> Path:
    """所有路径以当前工具真实目录为沙箱根目录。"""
    target=(ROOT/Path(path)).resolve()
    if not target.is_relative_to(ROOT.resolve()):
        raise ValueError('字体工具禁止访问本文件夹以外的路径')
    return target


def verify_local_tree() -> None:
    """拒绝输入或输出树中的符号链接/目录联接，保证不会越界读写。"""
    for name in ('input', 'output'):
        folder = ROOT / name
        if folder.is_symlink() or (hasattr(folder, 'is_junction') and folder.is_junction()):
            raise ValueError(f'{name} 指向了工具目录外部，已拒绝操作')
        for current, directories, files in os.walk(folder, followlinks=False):
            for entry in [*directories, *files]:
                path = Path(current) / entry
                if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
                    raise ValueError(f'禁止在工具目录中使用链接文件或目录：{path.relative_to(ROOT)}')
                if not path.resolve().is_relative_to(ROOT):
                    raise ValueError('检测到工具目录以外的文件，拒绝操作')


def publish_font_files(directory: Path, dat: bytes, bpr: bytes, report: bytes) -> None:
    """先在 output 内写入并校验完整成品，再更新正式文件。"""
    out = inside('output/font')
    out.mkdir(parents=True, exist_ok=True)
    # 旧成品先移入临时备份；中途失败时恢复旧成品，不遗留 DAT/BPR 半套。
    with tempfile.TemporaryDirectory(prefix='.font-pending-', dir=out) as temporary:
        stage = Path(temporary)
        items = [
            (stage / 'ui_font_cnx.dat', directory / 'ui_font_cnx.dat', dat),
            (stage / 'ui_font_cnx.bpr', directory / 'ui_font_cnx.bpr', bpr),
            (stage / '生成结果.json', out / '生成结果.json', report),
        ]
        for staged, _, payload in items:
            staged.write_bytes(payload)
        backups: list[tuple[Path, Path]] = []
        published: list[Path] = []
        try:
            directory.mkdir(parents=True, exist_ok=True)
            for number, (staged, target, _) in enumerate(items):
                if target.exists():
                    backup = stage / f'original-{number}'
                    target.replace(backup)
                    backups.append((backup, target))
                staged.replace(target)
                published.append(target)
        except OSError:
            for target in reversed(published):
                if target.exists():
                    target.unlink()
            for backup, target in reversed(backups):
                backup.replace(target)
            raise


def precise_chars() -> set[int]:
    """只提取 JSON 的 translate，不把英文原文误收录为当前译文。"""
    folder=inside('translations')
    files=list(folder.rglob('*.loc.json'))
    if len(files)!=86:
        raise ValueError(f'精准模式要求 86 份带译文 JSON，当前为 {len(files)}')
    result=set()
    for file in files:
        doc=json.loads(file.read_text(encoding='utf-8-sig'))
        if doc.get('format')!='BSCN-LOC-JSON-v1':
            raise ValueError(f'JSON 格式不受支持：{file.name}')
        for item in doc['entries']:
            value=item.get('translate')
            if not isinstance(value,str):
                raise ValueError(f'{file.name} 译文缺失')
            for char in value:
                cp=ord(char)
                if cp>=128 and cp not in (0xA0,0xFEFF) and not 0xD800<=cp<=0xDFFF:
                    result.add(cp)
    return result


def common_chars() -> set[int]:
    """国家标准 GB2312 一级汉字区，共 3755 字，并补足常用中文标点。"""
    result=set()
    for lead in range(0xB0,0xD8):
        for trail in range(0xA1,0xFF):
            try: text=bytes((lead,trail)).decode('gb2312')
            except UnicodeDecodeError: continue
            if len(text)==1: result.add(ord(text))
    result.update(ord(c) for c in '，。、；：？！“”‘’（）《》〈〉【】「」『』—…·～￥％＋－＝℃°')
    return result


def gbk_chars() -> set[int]:
    """枚举 Python Windows GBK/CP936 对应的所有合法双字节映射，去除重复码点。"""
    result=set()
    for lead in range(0x81,0xFF):
        for trail in range(0x40,0xFF):
            if trail==0x7F: continue
            try: value=bytes((lead,trail)).decode('gbk')
            except UnicodeDecodeError: continue
            if len(value)==1 and ord(value)>=128: result.add(ord(value))
    return result


def select_chars(mode: str) -> set[int]:
    if mode=='precise': return precise_chars()
    if mode=='common': return common_chars()
    if mode=='gbk': return gbk_chars()
    raise ValueError('未知字体生成模式')


def find_font() -> Path | None:
    folder=inside('input/fonts')
    fonts=sorted(p for p in folder.iterdir() if p.is_file() and p.suffix.lower() in ('.ttf','.otf','.ttc'))
    if len(fonts)>1:
        raise ValueError('input\\fonts 只能放一份 TTF、OTF 或 TTC 字体，避免无意选错')
    return fonts[0] if fonts else None


def from_existing_atlas(chars: set[int], original: NativeDat) -> tuple[ChineseAtlas, list[int]]:
    """无字体文件时仅采用确实存在于已知 BFA 或原版字体的字形。"""
    path=inside('input/template/fonts.bfa')
    if not path.is_file():
        raise ValueError('请在 input\\fonts 放一份 TTF/OTF/TTC；公开包不附带字体。已有个人 BFA 时可自行放入 input\\template\\fonts.bfa')
    old=ChineseAtlas.from_bfa(path.read_bytes())
    original_codes={cp-1 for face in original.faces for cp in face.codepoints() if cp>0}
    missing=sorted(chars - set(old.glyphs) - original_codes)
    subset={cp:old.glyphs[cp] for cp in sorted(chars & set(old.glyphs))}
    return ChineseAtlas(old.width,old.height,old.pixels,subset), missing


def from_user_font(font: Path, chars: set[int], pixel_size: int = 32) -> tuple[ChineseAtlas,list[int]]:
    """将 GDI 单字形按固定字格依次排到临时图集，保持所有字符真实可追踪。"""
    if len(chars)>30000:
        raise ValueError('字库字符超过 30000，超出本工具已审计的最大索引范围')
    area=len(chars)*CELL_W*CELL_H
    width=2048
    # 同时核对固定字格的行列，不能只按面积判断，否则临界数量
    # 会因每行剩余空间不足而在本可容纳的下一档图集前提前失败。
    while (width*width<area or math.ceil(len(chars)/(width//CELL_W))*CELL_H>width) and width<8192:
        width*=2
    if width*width<area: raise ValueError('待生成字符过多，8192 临时图集不足')
    cols=width//CELL_W
    rows=math.ceil(len(chars)/cols)
    if rows*CELL_H>width: raise ValueError('临时字体图集不足')
    bitmap=bytearray(width*width)
    glyphs={}
    missing=[]
    with WindowsGlyphRenderer(font, pixel_size=pixel_size) as renderer:
        for index,cp in enumerate(sorted(chars)):
            try:
                small=renderer.render(cp)
            except ValueError as exc:
                # 只有确实缺少字符才能继续统计；字号越界、像素盒过大等
                # 是生成参数错误，必须立即明确提示，不能冒充“字体缺字”。
                if '字体缺少' in str(exc) or '只支持 BMP' in str(exc):
                    missing.append(cp)
                    continue
                raise ValueError(f'渲染 U+{cp:04X} 时失败：{exc}') from exc
            x=(index%cols)*CELL_W
            y=(index//cols)*CELL_H
            for line in range(CELL_H):
                offset=(y+line)*width+x
                bitmap[offset:offset+CELL_W]=small[line*CELL_W:(line+1)*CELL_W]
            glyphs[cp]=SourceGlyph(cp,x,y,CELL_W,CELL_H,0,28,32)
            if (index+1)%1000==0: print(f'[字形] 已处理 {index+1}/{len(chars)}',flush=True)
    return ChineseAtlas(width,width,bytes(bitmap),glyphs),missing


def choose_texture_size(atlas: ChineseAtlas) -> int:
    """优先使用已实机验证的 2048，只有容量不足才扩展到未验证规格。"""
    for size in (2048,4096,8192):
        try:
            make_packed_glyphs(atlas.glyphs.values(),size)
            return size
        except ValueError:
            continue
    raise ValueError('8192 原生字体纹理也无法容纳全部字形，不能截断输出')


def font_pixel_size() -> int:
    """个人设置优先；未保存过配置时采用工具箱公共默认字号。"""
    path=inside('input/font_options.json')
    if not path.is_file():
        path=inside('config/defaults.json')
    if not path.is_file(): return 32
    value=json.loads(path.read_text(encoding='utf-8-sig')).get('pixel_size',32)
    if type(value) is not int or not 16<=value<=34:
        raise ValueError('input/font_options.json 的 pixel_size 必须是 16～34 的整数')
    return value


def run(mode: str) -> dict:
    verify_local_tree()
    template=inside('input/template')
    dat=(template/'ui_font_eur.dat').read_bytes()
    bpr=(template/'ui_font_eur.bpr').read_bytes()
    native=NativeDat.parse(dat)
    chars=select_chars(mode)
    font=find_font()
    if font is None:
        if mode!='precise':
            raise ValueError('常用字和 GBK 全量模式必须把 TTF/OTF/TTC 字体放到 input\\fonts')
        atlas,missing=from_existing_atlas(chars,native)
        source='用户自行提供的 BFA（精准模式回退）'
    else:
        atlas,missing=from_user_font(font,chars,font_pixel_size())
        source=font.name
    if missing:
        samples=','.join(f'U+{cp:04X}' for cp in missing[:20])
        raise ValueError(f'字体源缺少 {len(missing)} 个目标字符（{samples}）。禁止产生缺字成品')
    size=choose_texture_size(atlas)
    print(f'[生成] 模式={mode} 字符={len(chars)} 原生纹理边长={size}',flush=True)
    dat_out,bpr_out,report=make_native_candidate(dat,bpr,atlas=atlas,texture_size=size)
    # 先在内存里完成结构验收。失败时绝不能覆盖此前的可用字库。
    # 结构验收独立重读 DAT/BPR；中文索引不得悄悄发生字形丢失。
    verified=NativeDat.parse(dat_out)
    NativeBpr.parse(bpr_out,len(verified.faces))
    for face in verified.faces:
        keys=set(face.codepoints())
        absent=[cp for cp in chars if cp not in (0x20,0xA0) and cp not in keys]
        if absent: raise AssertionError('存在无法查找的中文码点：'+str(absent[:8]))
    result={'mode':mode,'requested_characters':len(chars),'source':source,
            'texture_size':size,'original_eur_preserved':True,
            'runtime_status':'2048 已实机通过；4096/8192 必须另行实机验证',
            'sha256':{'dat':hashlib.sha256(dat_out).hexdigest(),
                      'bpr':hashlib.sha256(bpr_out).hexdigest()},
            'details':report}
    info=(json.dumps(result,ensure_ascii=False,indent=2)+'\n').replace('\n','\r\n').encode('utf-8')
    publish_font_files(inside('output/font/BSCNContents/fonts'), dat_out, bpr_out, info)
    return result


def main() -> int:
    parser=argparse.ArgumentParser(description='BloodstoneCN 三模式原生 EUR 中文字体创建工具')
    parser.add_argument('mode',choices=('precise','common','gbk'))
    args=parser.parse_args()
    try:
        result=run(args.mode)
        print('[完成] 输出：output\\font\\BSCNContents\\fonts\\ui_font_cnx.dat/.bpr')
        print('[提醒]',result['runtime_status'])
        return 0
    except (ValueError,OSError,AssertionError,KeyError,TypeError,RuntimeError) as exc:
        print('[错误]',exc,file=sys.stderr)
        return 1

if __name__=='__main__':
    raise SystemExit(main())
