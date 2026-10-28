#!/usr/bin/env python3
"""校验干净工程的目录及 x86/x64 强制发行约束。"""

from pathlib import Path
import sys

# 从验证工具位置向上定位工程，以便解压到任何路径都可运行。
APP = Path(__file__).resolve().parents[2]
PROJECT = APP.parents[1]
cmake = (APP / "CMakeLists.txt").read_text(encoding="utf-8")
builder = (APP / "tools/build_release.py").read_text(encoding="utf-8")
batch = (APP / "build.bat").read_text(encoding="utf-8")
failures = []

# 七个名称必须在 CMake 中分别定义；单份 DLL 复制改名是明确禁止的。
entries = {"JoyShimXInput11":"xinput1_1", "JoyShimXInput12":"xinput1_2",
           "JoyShimXInput13":"xinput1_3", "JoyShimXInput14":"xinput1_4",
           "JoyShimXInput910":"xinput9_1_0", "JoyShimDInput8":"dinput8",
           "JoyShimWinMM":"winmm"}
for target, name in entries.items():
    if f'joyshim_add_proxy({target} "{name}" {name})' not in cmake:
        failures.append(f"缺少独立入口：{name}")
    if f"'{name}'" not in builder:
        failures.append(f"打包器缺少入口：{name}")

if "joyshim_add_proxy(JoyShimVersion" in cmake or "'version'" in builder.split("PROXIES =", 1)[-1].split("\n\n", 1)[0]:
    failures.append("version.dll 已停用，不得保留独立代理目标或发行清单")

# 检查统一目录，并确保编译结果从不同架构自己的构建目录读取。
for path in (PROJECT / "docs", PROJECT / "source", PROJECT / "release", PROJECT / ".build",
             APP / "src", APP / "tests", APP / "tools", APP / "config"):
    if not path.is_dir():
        failures.append(f"缺少目录：{path}")
for token in ("-DCMAKE_SYSTEM_NAME=Windows", "build_host_tests(cmake, ninja)",
              "for arch in ('x86', 'x64')", "'--expect-arch', arch", "check_alias_outputs.py",
              "if not file.is_file()", "'-DBUILD_TESTING=OFF'", "'-DJOYSHIM_BUILD_DIAGNOSTICS=OFF'"):
    if token not in builder:
        failures.append(f"构建器缺少强制双架构检查：{token}")
if 'JoyShimCore OBJECT' not in cmake or 'add_library(JoyShim SHARED' in cmake:
    failures.append("共享核心/独立 DLL 构建契约不成立")
if 'tools\\build_release.py' not in batch:
    failures.append("build.bat 未调用实际双架构构建脚本")

# UTF-8 无 BOM、CRLF 是项目正式文件的统一规范。
for p in PROJECT.rglob('*'):
    if not p.is_file() or any(x in p.parts for x in ('.git', '.build')) or 'SDL3' in p.parts and 'vendor' in p.parts:
        continue
    # 官方 SDL 源码保持上游原样，避免把第三方文件批量转换为项目换行格式。
    # 发行目录将存在真正的 DLL/EXE，不能把二进制内容误判为文本换行错误。
    if p.suffix.lower() not in {'.md', '.py', '.cpp', '.hpp', '.h', '.cmake',
                                '.def', '.toml', '.txt', '.bat'} and p.name not in {'CMakeLists.txt', '.gitignore'}:
        continue
    raw = p.read_bytes()
    if raw.startswith(b'\xef\xbb\xbf'):
        failures.append(f"文件带 UTF-8 BOM：{p}")
    if b'\n' in raw and raw.count(b'\r\n') != raw.count(b'\n'):
        failures.append(f"文件不是完整 CRLF：{p}")
    if p.suffix.lower() == '.bat':
        for line in raw.decode('utf-8').splitlines():
            stripped = line.lstrip().lower()
            if (stripped.startswith('rem ') or stripped.startswith('echo ')) and not line.endswith('  '):
                failures.append(f"批处理注释或输出行末缺少两个空格：{p}")
if failures:
    for failure in failures:
        print('[失败]', failure, file=sys.stderr)
    raise SystemExit(1)
print('目录布局、七入口 x86/x64 独立发行契约、编码及批处理约束检查通过。')
