# BloodstoneCN 插件源码

本目录只有一个 ASI 项目，取消按项目名重复套文件夹。Python 汉化工具箱仍独立位于工程根 toolbox，不在本目录复制另一份。

## 直接看哪里

| 位置 | 内容 |
|---|---|
| src/ | 插件 C++、Win32 头文件与命令行解析器 |
| config/ | 指令签名 profile、链接导入定义 kernel32.def、公开打包白名单 |
| tools/ | 开发检查、测试入口、打包、交叉编译和明文转储核验 |
| tests/ | 全部 Python 回归和 C++ 命令行测试 |
| build.bat | 唯一 Windows 构建入口 |

## 怎么构建

在 MSVC x86 开发命令提示符运行 `source\build.bat`，编译插件后检查工具箱并执行回归。插件输出到工程根 `release/Runtime/BloodstoneCN.asi`，编译临时文件在 `.build/BloodstoneCN`，成功后自动清理。脚本不固定编译器绝对路径、不联网安装依赖。

如果只检查已验收工程，运行 `source\build.bat --check-only`；这条命令无需重新编译 ASI。也可以直接执行：

```text
python -B source/tools/bootstrap_toolbox.py
python -B source/tools/run_tests.py
```

Python 使用 3.10+ 标准库。交叉编译可执行 `python -B source/tools/build_clang_cross.py`，需要本机已具备 clang-cl 和 lld-link；导入定义直接从 config 读取。

正式公开包冻结已验收 ASI 的 SHA-256。换编译器重编后如字节变化，先审核程序，再明确更新 tools/package_release.py 中的固定指纹，不删除检查。

公开打包：`python -B source/tools/package_release.py`。工具说明见 tools/使用说明.md，完整接档见 docs/完整接档.md。游戏 EXE、明文转储及任何游戏资源均不在源码包中；可选真实对照仍通过 BSCN_PRIVATE_REFERENCE、BSCN_PAK_REFERENCE 和 BSCN_BOND_UNPACKED 提供自己的文件。
