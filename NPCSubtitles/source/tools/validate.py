# -*- coding: utf-8 -*-
"""对完整源码执行本地静态合同测试和编码检查。不能代替真实 Win32 编译。"""
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
PACKAGE=ROOT.parents[1]

def main():
    """只有全部检查完成后才返回成功，不伪装成已编译的 DLL。"""
    bad=[]
    for location in (PACKAGE/'docs',PACKAGE/'source'):
        for path in location.rglob('*'):
            if not path.is_file() or path.suffix.lower() not in ('.cpp','.h','.py','.md','.txt','.tsv','.bat','.json'):
                continue
            raw=path.read_bytes()
            if raw.startswith(b'\xef\xbb\xbf'):
                bad.append(str(path.relative_to(PACKAGE))+': UTF-8 BOM')
            try:
                raw.decode('utf-8')
            except UnicodeDecodeError:
                bad.append(str(path.relative_to(PACKAGE))+': 非 UTF-8')
            if b'\n' in raw.replace(b'\r\n',b''):
                bad.append(str(path.relative_to(PACKAGE))+': 非 CRLF')
    for problem in bad:print('[错误]',problem)
    if bad:return 1
    process=subprocess.run([sys.executable,'-m','unittest','discover','-s',str(ROOT/'tests'),'-v'])
    if process.returncode:return process.returncode
    cpp=subprocess.run([sys.executable,str(ROOT/'tools/check_cpp_brackets.py')])
    if cpp.returncode:return cpp.returncode
    print('[通过] 编译产物 PE、活动源码合同及统一编码检查；Windows 实机仍待验证')
    return 0

if __name__=='__main__':raise SystemExit(main())
