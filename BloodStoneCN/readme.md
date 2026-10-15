# BloodstoneCN M1 v1.2 公开汉化工具箱

版本：M1-v1.2-Toolkit-public3-PakFix，2026-10-15。工具箱整合 LOC 转换、三面原生字库生成和离线验收；游戏运行时仍使用原封保留的 v1.2 ASI。

## 目录职责

| 目录 | 内容 |
|---|---|
| `toolbox/` | 独立运行的统一工具箱，包含 GUI、转换核心、个人工作输入和输出 |
| `source/` | ASI 源码、构建入口，以及开发、测试和公开打包工具 |
| `release/Runtime/` | 已编译 ASI 程序模板；不放任何工具 |
| `docs/` | 正式技术文档、完整接档和审计记录 |
| `.build/` | 开发检查产生的临时文件 |

工具代码只在 toolbox 维护一份，不再分成 LocTool、FontTool 或源文件与发行脚本两套副本。**公开 ZIP 不带游戏资产、原文、译文、字体或汉化成品。**

## 使用

1. 解压到独立文件夹（允许位于游戏目录下），使用 Windows 和自带 Tkinter 的 **64 位 Python 3.10+**；不需要 pip、第三方 Python 包或编译器。
2. 双击 `toolbox/start.bat`，在“项目初始化”中选择自己的游戏目录，点击“初始化个人项目”。原始 PAK 只读。
3. 在“译文编辑”中选择资源，编辑右侧译文并保存；也可用外部工具编辑 `toolbox/translations` 中的 `translate`。
4. 在“构建与验收”中选择个人字体及字号，点击“构建完整汉化”。界面选择的字体和字号会直接用于本次构建，也可提前导入。
5. 成功后打开 `toolbox/output/Runtime`，只把其中的 `BloodstoneCN.asi` 和 `BSCNContents` 同级安装到游戏，使用现有 x86 ASI 加载器，游戏语言设为 EN。

初始化只做一次，已有项目会拒绝覆盖；之后修改译文或字体，再次构建即可。GUI 也提供原文副本导出、单独回编 LOC、单独生成字库、重新验收、打开工作目录和清空日志。

`toolbox` 可以整目录单独复制，转换流程不依赖 source。完整包默认找到 `release/Runtime/BloodstoneCN.asi`；若只复制工具箱，则在 GUI 选择自己的 ASI，或将 ASI 放入 `toolbox/input/plugin/`。单独导出、回编 LOC 和生成字库不需要 ASI。

工具使用说明见 `toolbox/使用说明.md`；完整步骤见 `docs/从零汉化指南.md`；接手开发先读 `docs/完整接档.md`。

对外重新发布只使用 `python -B source/tools/package_release.py`。该命令按明确白名单打包，使用后的个人模板、翻译、字体和输出也不会进入公开 ZIP。不要直接压缩已经使用过的工作目录。

同输入的技术路线保持：86 LOC／7,975 条原生文本、原始资源键、其他七种语言、EUR 三字体面、原西文和图标、按需日志及原生文字效果修复。实际双 PAK 的自动初始化已通过原始样本验证；Windows GUI／GDI 和游戏效果仍需实机验收，离线边界见审计文档。
