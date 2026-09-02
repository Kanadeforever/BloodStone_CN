#!/usr/bin/env python3
from __future__ import annotations
import hashlib, os, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'src'; DOCS=ROOT/'docs'
W11_SHA={
'src/external_loc.cpp':'27da2d023d5b68c4b39c4605232788bd0725ea54b016bf092a87f65b1c61102c',
'src/external_loc.h':'542b8c43b6695adfc233592f12a0026ad4895af5c7276797f9a97a7e68613e0d',
'src/submit_text_bridge.cpp':'1efc7fbecfcadfd6b9825bf90283b965ea8d3896f7c3b64f718464e67ec5d67c',
'src/submit_text_bridge.h':'1ff9866d61774ed52dd30b0222262212239851e0a4e62068dcf01504a81b9042',
'src/text_coverage_resolver.cpp':'f7d3ffa5360d329243ef21b7924447a5db3ad2a6039423bb58639e4483af1d01',
'src/text_coverage_resolver.h':'150a999f06b9b4730d28c3b6650c625371595e186e472988e90810f1c45ec88e',
'src/hooks.cpp':'c7b0b03930c47f1e64cc72f911079df2a29bd475305509ac1e04f111f46838ea',
'src/hooks.h':'f478ea7a3f7d3b08c48a509695ebee29a83b0bbfad8796f8b181fef519767076',
'src/native_font_bridge.cpp':'899f5a59d4ec6375d698cd0b718a823d3a1a52a2d092b4cba69986edae6b4f17',
'src/font_atlas.cpp':'24834ec9cf577649e9349dc53aa35342e915f44575d60d1607273e28e7586cf9',
'src/interop_host.cpp':'45a508fd05ded2e8d0927f0e5c5c019e44b0d714127a753af527937d9f973dec',
'src/log.cpp':'3fc54eb1552d6d4734a9b6bbdea8a86e9697aa17df9b4c15f4fd0fb12b7e104d',
}
def sha(p:Path): return hashlib.sha256(p.read_bytes()).hexdigest()
def run(cmd):
    env=os.environ.copy(); env['PYTHONDONTWRITEBYTECODE']='1'
    q=subprocess.run(cmd,cwd=ROOT,text=True,capture_output=True,env=env)
    return q.returncode==0,q.stdout+q.stderr
def braces(t:str):
    stack=[]; i=0; line=1; col=1; state='code'; quote=''
    while i<len(t):
        c=t[i]; n=t[i+1] if i+1<len(t) else ''
        if state=='code':
            if c=='/' and n=='/': state='line'; i+=2; col+=2; continue
            if c=='/' and n=='*': state='block'; i+=2; col+=2; continue
            if c in {'"',"'"}: state='str'; quote=c
            elif c=='{': stack.append((line,col))
            elif c=='}':
                if not stack: return False,f'多余 }} {line}:{col}'
                stack.pop()
        elif state=='line':
            if c=='\n': state='code'
        elif state=='block':
            if c=='*' and n=='/': state='code'; i+=2; col+=2; continue
        elif state=='str':
            if c=='\\': i+=2; col+=2; continue
            if c==quote: state='code'
        if c=='\n': line+=1; col=1
        else: col+=1
        i+=1
    return (not stack, 'balanced' if not stack else f'未闭合 {{ {stack[-1][0]}:{stack[-1][1]}')
checks=[]
def add(n,ok,d=''): checks.append((n,bool(ok),d))
for p in sorted(SRC.glob('*.cpp')):
    ok,d=braces(p.read_text(encoding='utf-8')); add('C++ 花括号结构：'+p.name,ok,d)
ok,out=run([sys.executable,'-m','unittest','discover','-s','tests','-v']); add('现有 unittest',ok,'PASS' if ok else out[-1500:])
ok,out=run([sys.executable,str(ROOT/'tests/R10_4原有文本解析模型.py')]); add('R10.4 原有文本解析模型',ok and 'Ran 31 tests' in out,'31/31 PASS' if ok else out[-1500:])
for rel,h in W11_SHA.items():
    p=ROOT/rel; add('W1.1 文本/生命周期语义冻结：'+rel,p.exists() and sha(p)==h,sha(p) if p.exists() else 'missing')
allsrc='\n'.join(p.read_text(encoding='utf-8') for p in SRC.glob('*') if p.suffix in {'.cpp','.h'})
for forbidden in ['IsAuthoritativePointer','SUBMIT_AUTHORITATIVE_FASTPATH_ACTIVE','ThreadCoverageCache','TEXT_COVERAGE_PARTIAL_REJECT','TEXT_COVERAGE_PARTIAL_FINAL_GUARD','PRISTINE_SOURCE_RETRY']:
    add('W1.2/W1.2a 失败语义已撤销：'+forbidden,forbidden not in allsrc)
ini=(ROOT/'config/BloodstoneCN.ini').read_text(encoding='utf-8')
add('高频诊断默认关闭',all(x in ini for x in ['LogMisses=0','LogActionTokens=0','LifecycleProbe=0']))
plugin=(SRC/'plugin.cpp').read_text(encoding='utf-8')
add('W1.2b 启动标识','R10.4 W1.2b CoverageRollback PerfSafe' in plugin)
add('中文不回归契约','text_semantics=W1_1_EXACT' in plugin and 'chinese_regression=FORBIDDEN' in plugin)
add('D3D 仍早于 LOC/BLI',plugin.find('hooks::Install(')>=0 and plugin.find('hooks::Install(')<plugin.find('external_loc_.Initialize('))
submit=(SRC/'submit_text_bridge.cpp').read_text(encoding='utf-8')
add('SubmitText 字体门禁保留','FONT_GATE_SKIP_SUBMIT' in submit and '!overrides_->rendering_ready()' in submit)
loc=(SRC/'loc_text_bridge.cpp').read_text(encoding='utf-8'); add('GetText 地址冻结','0x00E87740u' in loc); add('SubmitText 地址冻结','0x00E85900u' in submit)
md_out=[p for p in ROOT.rglob('*.md') if DOCS not in p.parents]; add('所有 Markdown 仅位于 docs',not md_out,','.join(str(p.relative_to(ROOT)) for p in md_out))
add('无 __pycache__',not any(p.is_dir() and p.name=='__pycache__' for p in ROOT.rglob('__pycache__')))
# build bat GBK and safe root
try:
    bat=(ROOT/'构建_MSVC_x86.bat').read_bytes().decode('gbk')
    add('构建 BAT 安全 ROOT','for %%I in ("%~dp0.") do set "ROOT=%%~fI"' in bat and 'set "BUILD=%ROOT%\\build"' in bat)
except Exception as e: add('构建 BAT GBK',False,str(e))
failed=[x for x in checks if not x[1]]
lines=['BloodstoneCN R10.4 W1.2b 源码验证结果','='*42,f'总检查项：{len(checks)}',f'通过：{len(checks)-len(failed)}',f'失败：{len(failed)}','']
for n,ok,d in checks: lines.append(f"[{'PASS' if ok else 'FAIL'}] {n}"+(f' — {d}' if d else ''))
lines+=['','说明：当前容器未执行 Windows MSVC Win32 完整链接；仍需用户本机运行 构建_MSVC_x86.bat。']
(DOCS/'源码验证结果.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')
print('\n'.join(lines)); raise SystemExit(1 if failed else 0)
