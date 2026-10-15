# NPCSubtitles v0.1e-log6：源码构建与测试

## 项目作用

本模块独立为原版 Bond.exe 未提交原生 UI 字幕的 NPC 对话补充中文文本。直接读取 `BSCNContents\loc` 与游戏已加载的 EUR 字形资源，使用游戏现有 D3D9 设备绘制。字形、字号、遮罩与图形 Hook 沿用已实机通过的 v0.1e；独立三行对白队列替代了旧的单消息覆盖缓存。

## 从零编译

完整发行根目录执行 `source\build_all.bat`；也可在装有 Clang-CL 与 LLD 的环境执行：

```text
python source/NPCSubtitles/tools/build_clang_cross.py
```

本工程不联网，不要求外置 Windows SDK，不依赖此前发行包。默认编译完整功能 Win32/x86 `release/NPCSubtitles.asi`；中间文件放入 `.build` 并自动清理。Windows 也保留 Visual Studio MSVC Win32 与 CMake 的构建入口。阶段 1～3 仅供异常诊断，不可误作正式字幕发行。

## 当前运行逻辑

在已经实机验证的 `0x0093A2A1` CALL 读取事件 `+0x1C`。值 0 为游戏原生字幕通路，禁止再次入队；值 1 为 NPC 环境对白，再执行 LOC 哈希查询。读取失败时选择静默，不猜测。旧语义签名和额外原生 UI CALL Hook 均不参与运行，避免不必要的热路径开销和兼容风险。

正常 `Bond.exe` 不写日志；`Bond.exe --log` 启用详细日志。发行无需 INI、TSV 或其他单独规则配置。旧 `NPCSubtitlesSemanticRoutes.tsv` 可删除。工具 `tools/validate.py` 校验实际源码与 PE，`tools/verify_stack_probe.py` 验证 x86 栈帧辅助函数，`tools/analyze_subtitle_route_log.py` 仅供分析历史 v0.1e-log3 的原生 UI 探针日志，不再是运行时依赖。

实机回归要特别检查手柄图标 MOD、已显示原生字幕的剧情对白是否重复、NPC 环境字幕是否仍出现、帧率变化和视口缩放。项目本身不能在交叉编译环境执行 Windows 游戏测试。其他完整信息见 `../../docs/完整接档.md`、`../../docs/原生字幕分流验证.md`。

## 多消息字幕队列及测试

`src/subtitle_queue.h` 是独立的队列调度实现。最多三条同时显示，超过三条进入容量十六条的 FIFO 等待队列；多消息共享最多三行。`tests/queue_native_test.cpp` 使用普通 C++ 编译器直接验证队列代码；`tests/test_three_line_queue.py` 将该验证加入自动化测试。队列之外的原生 `+0x1C` 分流、字体捕获、EndScene 和 Reset 地址未改变。

三行视觉效果、手柄图标 MOD 与性能已由用户在 v0.1e-log5 实机确认。当前 v0.1e-log6 新增原生过场事件监听和队列清空，尚待实机验证；回退版本是已实机通过的 v0.1e-log5。

## 过场字幕清空

游戏明文 EXE 发送 `CutSceneStart`（`0xA2193087`）和 `CutSceneEnd`（`0xA2193088`）消息。活动代码 `native_standalone.cpp` 在版本和原始指令核对后，对 `0x00E46E70` 消息派发函数安装跳板，只有这两类事件才执行字幕清空或恢复；其他事件直接执行原始指令。原生剧情字幕分流仍使用 `+0x1C`，发生剧情接管时会额外清空未过期的环境对白。

过场状态切换期间会清空三条活动消息与十六条等待消息；原生 C++ 队列测试覆盖清空与恢复。用户实机需要确认这条原生事件链是否包含遇到问题的过场类型，不能将静态机器码证据当作游戏验证结果。
