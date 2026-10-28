#!/usr/bin/env python3
"""校验公开 TOML 模板、业务解析器和用户开关之间的最基本一致性。"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PublicConfigContractTests(unittest.TestCase):
    def test_default_file_matches_embedded_template(self) -> None:
        # 外置模板与程序首次启动写出的内置模板必须完全一致。
        # 若只更新其中之一，用户复制文件后可能和自动生成文件行为不同。
        header = (ROOT / 'src/default_config.hpp').read_text(encoding='utf-8')
        matched = re.search(r'R"JOYSHIMTOML\((.*?)\)JOYSHIMTOML"', header, re.S)
        self.assertIsNotNone(matched)
        file_bytes = (ROOT / 'config/template.toml').read_bytes()
        self.assertFalse(file_bytes.startswith(b'\xef\xbb\xbf'))
        self.assertEqual(file_bytes.decode('utf-8').replace('\r\n', '\n'), matched.group(1))
        # 构建器只要求 Python 3.10+，不强制依赖 3.11 新增的 tomllib。
        # 这里只确认公开三组开关；真正的 TOML 词法测试交给 C++ 测试器。
        lines = [line.strip() for line in matched.group(1).splitlines()
                 if line.strip() and not line.lstrip().startswith('#')]
        self.assertEqual(lines, [
            '[Input]', 'Rumble = true',
            '[Compatibility]', 'HideDirectInput = false',
            '[Debug]', 'Log = false',
        ])

    def test_proxy_toml_uses_loaded_dll_basename(self) -> None:
        # 用户可以从七种代理中任选一种放进游戏目录；默认 TOML 必须
        # 跟随实际加载的 DLL 名称变化，不能写死 joyshim.toml。
        parser = (ROOT / 'src/config.cpp').read_text(encoding='utf-8')
        builder = (ROOT / 'tools/build_release.py').read_text(encoding='utf-8')
        inspector = (ROOT / 'tools/validation/check_alias_outputs.py').read_text(encoding='utf-8')
        self.assertIn('GetModuleFileNameW(module, modulePath, MAX_PATH)', parser)
        self.assertIn('BuildModuleConfigStem(', parser)
        self.assertIn('BuildProxyProfileSuffix(', parser)
        self.assertIn('BuildSiblingPath(module, genericName, config.genericTomlPath', parser)
        self.assertNotIn('BuildSiblingPath(module, L"joyshim.toml", config.genericTomlPath', parser)
        self.assertIn("destination / f'{name}.toml'", builder)
        self.assertIn('missing_configs', inspector)
        self.assertIn("folder / 'joyshim.toml'", inspector)

    def test_public_fields_have_runtime_consumers(self) -> None:
        # 词法解析成功不表示功能接线完整；这些检查要求业务解析器
        # 和振动工作线程都实际引用相应的开关。
        parser = (ROOT / 'src/config.cpp').read_text(encoding='utf-8')
        config = (ROOT / 'src/config.hpp').read_text(encoding='utf-8')
        input_worker = (ROOT / 'src/xinput_proxy.cpp').read_text(encoding='utf-8')
        for path in ('Input', 'Compatibility', 'Debug'):
            self.assertIn(f'path[0] == "{path}"', parser)
        self.assertIn('config.enableRumble', parser)
        self.assertIn('config.enableLog', parser)
        self.assertIn('config.project.patch.nativeInput', parser)
        self.assertIn('bool enableRumble{true}', config)
        self.assertIn('bool blockDirectInput{false}', config)
        self.assertIn('config.enableRumble', input_worker)
        self.assertIn('slot.appliedRumbleGeneration = ~std::uint32_t{0}', input_worker)

    def test_strict_parser_has_windows_end_to_end_test(self) -> None:
        # Windows 测试必须调用真正的 LoadRuntimeConfigFile，不得只验证
        # TOML 语法词法器；构建器的独立 --windows-tests 模式会运行它。
        cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        test = (ROOT / 'tests/test_config_file.cpp').read_text(encoding='utf-8')
        self.assertIn('add_executable(test_config_file', cmake)
        self.assertIn('add_test(NAME config_file COMMAND test_config_file)', cmake)
        self.assertIn('LoadRuntimeConfigFile(', test)
        self.assertIn('DefaultTomlTemplateSize', test)
        self.assertIn('"兼容旧版高级配置"', test)

    def test_size_optimizations_keep_static_runtime(self) -> None:
        # 体积优化不能覆盖 GNU MinGW 的静态运行库选项，也不能
        # 将必须的 HIDAPI、Joystick、Haptic 后端关闭。
        cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        for entry in ('SDL_HIDAPI ON', 'SDL_HIDAPI_JOYSTICK ON',
                      'SDL_JOYSTICK ON', 'SDL_HAPTIC ON', 'SDL_STATIC ON',
                      'JOYSHIM_OPTIMIZE_SIZE', 'JOYSHIM_ENABLE_LTO',
                      'check_ipo_supported', 'INTERPROCEDURAL_OPTIMIZATION_RELEASE',
                      '--strip-all', '-static -static-libgcc -static-libstdc++'):
            self.assertIn(entry, cmake)


if __name__ == '__main__':
    unittest.main(verbosity=2)
