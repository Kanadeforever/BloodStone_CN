#!/usr/bin/env python3
"""在不依赖 Windows SDK 的环境下，验证单 DLL 专版的源码组合约束。"""

from pathlib import Path
import sys

# 从当前脚本反向定位工程根目录，避免写死任何开发机器的绝对路径。
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "src"


def require(condition: bool, message: str) -> None:
    """一旦发现关键接口或构建边界缺失就终止，避免发布不完整源码。"""
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    # 确认内嵌 SDL3、按键贴图和专版 DInput8 导出定义都存在。
    require((ROOT / "tools/vendor/SDL3/include/SDL3/SDL.h").is_file(), "缺少内嵌 SDL3")
    require((SOURCES / "gamepad_icons/generated/EmbeddedIcons.generated.h").is_file(), "缺少图标图集")
    require((SOURCES / "exports_dinput8_x86.def").is_file(), "缺少 DInput8 导出")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require('joyshim_add_proxy(JoyShimDInput8 "dinput8" dinput8)' in cmake,
            "构建必须只生成 dinput8")
    require(cmake.count("joyshim_add_proxy(") == 1,
            "发现意外的第二个代理构建入口")
    require('BSCN_SINGLE_DLL=1' in cmake, "两组源码没有统一启用专版分支")

    entry = (SOURCES / "xinput_proxy.cpp").read_text(encoding="utf-8")
    icons = (SOURCES / "gamepad_icons/BondGamepadIcons.cpp").read_text(encoding="utf-8")
    require("BondOneReadControllerInfo" in entry and "BondOneReadControllerInfo" in icons,
            "图标必须读取 JoyShim 已发布的槽位")
    require("BondIconsProcessAttach" in entry and "BondIconsProcessAttach" in icons,
            "单 DLL 图标入口缺失")
    require("BondIconsProcessDetach" in entry and "BondIconsProcessDetach" in icons,
            "单 DLL 卸载入口缺失")
    require("-ps" in icons and "-ns" in icons and "-xb" in icons and "-auto" in icons,
            "缺少命令行图标模式")

    logger = (SOURCES / "bond_one_log.cpp").read_text(encoding="utf-8")
    require("CREATE_ALWAYS" in logger and 'L".log"' in logger,
            "日志必须以 DLL 同名文件开始新会话")
    stadia = (SOURCES / "managed_devices.cpp").read_text(encoding="utf-8")
    require("IsUniquelyManagedStadia" in stadia,
            "Stadia 去重必须遵守唯一实例匹配")
    require("SDL_GetGamepadStringForType" in entry and "GetGamepadMapping" in entry,
            "手柄识别必须复用内嵌 SDL 类型和映射信息")
    require(not (ROOT / "config").exists(), "专版不得携带外置 TOML/INI 配置")

    # 仅检查来源数据库中有 USB 与蓝牙映射；这不代表实际连接已通过测试。
    database = (ROOT / "tools/vendor/SDL3/src/joystick/SDL_gamepad_db.h").read_text(encoding="utf-8")
    require('03000000d118000000940000' in database, "缺少 Stadia 有线数据库映射")
    require('05000000d118000000940000' in database, "缺少 Stadia 蓝牙数据库映射")
    print("[通过] 单 DLL 构建边界、同名清空日志、SDL3 双连接识别、图标入口与零配置检查")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"[失败] {exc}", file=sys.stderr)
        raise SystemExit(1)
