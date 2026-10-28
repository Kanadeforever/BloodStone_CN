#!/usr/bin/env python3
"""验证正式发行只含代理 DLL，并拒绝外部 MinGW 运行库依赖。"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BUILDER = (ROOT / "tools/build_release.py").read_text(encoding="utf-8")
SPEC = importlib.util.spec_from_file_location(
    "joyshim_pe_validation", ROOT / "tools/validation/inspect_pe.py"
)
assert SPEC is not None and SPEC.loader is not None
PE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PE
SPEC.loader.exec_module(PE)


class ReleaseContractTests(unittest.TestCase):
    def test_mingw_dll_runtime_is_linked_statically(self) -> None:
        # 只静态链接 GCC/标准 C++ 库仍可能引入 libwinpthread-1.dll。
        # MinGW 链接全部静态运行库，而原生 Windows API 仍使用系统 DLL。
        self.assertIn("target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)", CMAKE)
        self.assertIn('if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")', CMAKE)
        self.assertIn("target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)", CMAKE)
        self.assertIn('set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")', CMAKE)

    def test_optional_executables_are_disabled_for_release(self) -> None:
        # 发行模式必须关闭测试和诊断 EXE；所有这些目标只能显式启用。
        self.assertIn('option(BUILD_TESTING "启用 CTest 单元测试（开发专用）" OFF)', CMAKE)
        self.assertIn('option(JOYSHIM_BUILD_DIAGNOSTICS "编译手柄与代理诊断工具（开发专用）" OFF)', CMAKE)
        self.assertIn('if(BUILD_TESTING)\nadd_executable(test_mapping', CMAKE)
        self.assertIn('if(JOYSHIM_BUILD_DIAGNOSTICS)\nadd_executable(JoyShimGamepadProbe', CMAKE)
        self.assertIn("'-DBUILD_TESTING=OFF'", BUILDER)
        self.assertIn("'-DJOYSHIM_BUILD_DIAGNOSTICS=OFF'", BUILDER)
        self.assertNotIn("for name in UTILITIES", BUILDER)
        self.assertIn("def run_windows_tests(", BUILDER)
        self.assertIn("if args.windows_tests or args.wine_tests:", BUILDER)

    def test_external_runtime_is_rejected(self) -> None:
        # PE 导入表中的库名不区分大小写；拒绝常见的 MinGW 动态运行库。
        imports = ["KERNEL32.dll", "LIBWINPTHREAD-1.DLL", "libstdc++-6.dll", "libgcc_s_dw2-1.dll"]
        self.assertEqual(PE.check_runtime_imports(imports),
                         ["libgcc_s_dw2-1.dll", "libstdc++-6.dll", "LIBWINPTHREAD-1.DLL"])

    def test_windows_system_imports_are_allowed(self) -> None:
        # Windows 自带系统 DLL 不应误判为需要额外安装的 MinGW 依赖。
        self.assertEqual(PE.check_runtime_imports(
            ["KERNEL32.dll", "USER32.dll", "SETUPAPI.dll", "MSVCRT.dll"]), [])
        self.assertEqual(PE.check_runtime_imports(["VCRUNTIME140.dll"]), ["VCRUNTIME140.dll"])

    def test_release_does_not_copy_exe(self) -> None:
        # 源码中的诊断程序依然可单独构建，但绝不复制到正式发行目录。
        self.assertNotIn("shutil.copy2(results[arch] / f'{name}.exe'", BUILDER)
        self.assertIn("unexpected = list((stage / arch).glob('*.exe'))", BUILDER)


if __name__ == "__main__":
    unittest.main(verbosity=2)
