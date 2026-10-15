# NPCSubtitles 工具说明

本工程包含能真正执行的构建工具和无修改的静态验证脚本。

- `build_clang_cross.py`：使用 Clang-CL/LLD 目标 `i686-pc-windows-msvc` 真正编译并链接 `release/NPCSubtitles.asi`。不需要 Windows SDK 或 d3d9.lib。依赖构建环境提供 clang-cl、lld-link；路径不写死。
- `kernel32.def`：所有 Win32 系统导入由本文件生成最小导入库，不依赖之前版本。
- `validate.py`：检查 UTF-8 无 BOM、CRLF、活动源码合同测试、括号结构；不改游戏。
- `check_cpp_brackets.py`：静态解析活动源码的括号，不能替代真正的编译。
- `inspect_inputs.py`：可选检查用户原始 Bond 运行时转储和历史 LOC（需另外提供原始游戏资料，不进入发行依赖）。

构建时只创建 `.build/` 中间文件，输出在 `release/`，无联网、无全局安装路径。


## 当前正式构建与历史诊断阶段

默认运行 `python tools/build_clang_cross.py` 会构建完整功能 `NPC_DIAG_STAGE=4`，产物为 `release/NPCSubtitles.asi`，不是历史无 Hook 阶段 A。Windows 的 `build.bat` 与 `CMakeLists.txt` 也以完整阶段为默认。诊断阶段 1～3 需要显式传 `--stage`，其产物放在 `release/diagnostics`，不得拿来代替正式版。

`verify_stack_probe.py` 独立解析编译得到的 PE32，核对自制 `_chkstk` 的 ESP 调整、寄存器恢复和编译器的实际调用点。这是机器码检查，不是 Windows 实机运行证明。

`analyze_subtitle_route_log.py` 是历史 v0.1e-log3 原生 UI 探针日志的离线分析器。使用 `Bond.exe --log` 取得普通日志后运行：

```text
python source/NPCSubtitles/tools/analyze_subtitle_route_log.py NPCSubtitles.log
```

分析器只适用于历史双事件日志；当前 ASI 不再安装原生 UI 提交探针，因此新日志不包含第二类事件。该工具不会更改日志、插件、游戏或资源文件。没有配对记录不等于游戏一定不显示字幕。详见 `docs/原生字幕分流验证.md`。

完整工程无需此前发行包即可重建，文字统一使用 UTF-8 无 BOM + CRLF，构建时临时文件放在 `.build/`，编译后按需清理。

## 过场字幕回归测试

`tests/test_cutscene_transition.py` 验证 `CutSceneStart/End` 的原生事件编号、发送入口原始 CALL 的安全检查、x86 跳板寄存器保护、过场期间禁止入队及绘制、`--log` 门控。`tests/queue_native_test.cpp` 还会直接运行活动队列代码，验证三条可见字幕和等待消息清空后不会再次出现，以及后续新对白能重新入队。该测试不要求原版游戏文件。

实际过场事件在不同关卡是否都会触发需要 Windows 实机验证；如果不触发或 Hook 安装失败，不得仅凭静态测试宣称覆盖。
