# 汉化工具箱使用说明

本目录是完整的独立 Python 工具箱。`gui.py` 提供图形壳，`toolkit.py` 提供同一套命令行工作流，`core/` 保存实际转换实现。无需复制 source，也不需要运行游戏中的插件来导出文本或生成字库。

## 启动与环境

使用 Windows、64 位 Python 3.10+，并在 Python 安装时保留 Tcl/Tk 组件。双击本目录 `start.bat`；也可以执行 `python -B gui.py`。工具只用标准库，字体栅格化使用系统 GDI，无 pip 和下载步骤。

工具箱可以位于游戏目录下的独立子目录，不能覆盖游戏原有文件或占用 cache 等原始资源目录。个人配置、缓存和日志只在本目录生成，不写注册表或系统目录。初始包中没有任何游戏输入。

## 三个页面

### 1. 项目初始化

选择含 Bond.exe 的游戏目录，点击“初始化个人项目”。工具只读 `cache/gamedata.pak`、`cache/root.pak`，提取完整的 86 份待译 JSON 和 EUR 三面字库模板。原文、模板、来源记录作为一组保存，任何一步失败都不发布半套项目。

原始 gamedata 的完整路径索引和 root 的目录父项索引均已支持。游戏路径布局不同或其他 root.pak 布局不兼容时，勾选手动来源，选择 gamedata.pak，并提供 root.pak 或本人游戏提取的 EUR 模板目录。模板目录有值时优先使用它。

已经初始化的项目不允许覆盖。要开始另一个独立项目，请重新解压一份工具箱。“导出原文副本”只生成 `output/loc/untranslated_json`，不重置正在翻译的内容；已有原文副本也不会被覆盖。

### 2. 译文编辑

选择资源文件，按原文、译文或资源键搜索，选中一行后在右侧编辑 translate。左侧原文只读。支持多行和 Unicode；换行不需要手写转义。点击“保存当前文件”才写入 JSON。

保存前执行真实导入校验：资源身份、原文、哈希、记录数和字符串格式必须正确。控制标记差异会显示数量。外部编辑器若已修改该 JSON，GUI 拒绝覆盖；可复制当前编辑框中的文本保留工作，再重新载入磁盘内容。

切换文件、开始构建或关闭窗口时，未保存修改会提示保存、放弃或取消操作。放弃后界面恢复磁盘内容，构建读取的也是这份内容。允许用自己的翻译工具批量编辑 `translations/`，但只能修改 translate，完整保留 86 文件及其身份字段。

### 3. 构建与验收

选择一份 TTF、OTF 或 TTC，设置 16～34 的整数像素字号，默认 32。可先点击“导入字体 / 保存字号”，也可直接点击“构建完整汉化”；后者会自动使用界面上的选择。字体只复制到本目录，生成时私有加载，不安装到系统。

完整构建使用唯一的 translations 同时生成 LOC 和字库，全部检查通过后更新 `output/Runtime`。失败保留上次完整成品；输入若已改变，必须重新构建再安装。

默认精准字集最接近原项目已验证的 2048 方案。常用字和完整 GBK 可能生成 4096／8192 图集，离线通过不代表游戏已验证。

“只回编 LOC”和“只生成字库”保留独立操作能力，分别输出到 `output/loc/` 和 `output/font/`，不会自动更新完整安装目录。“验收当前成品”重新核对当前磁盘输入、成品清单和原生结构。

耗时操作在后台执行，日志显示在窗口底部；操作期间按钮和编辑框禁用，避免同时修改。关闭窗口不会强制中断正在执行的构建。清空日志按钮同时清空界面与本地 GUI 日志。

## 工作文件位置

| 本目录内的路径 | 内容 |
|---|---|
| `input/gamedata.pak` | 游戏的原文结构副本（工具自动复制） |
| `input/template/` | 原始 EUR DAT/BPR；可选个人 BFA |
| `input/fonts/` | 当前选定字体的副本 |
| `input/font_options.json` | 字号设置；没有时采用 `config/defaults.json` |
| `translations/` | 唯一人工译文源（工具自动导出） |
| `work/project.json` | 初始化输入指纹与来源 |
| `output/loc/` | 原文副本、LOC 单项构建报告和回编结果 |
| `output/font/` | 字库单项报告与生成结果 |
| `output/Runtime/` | 完整构建通过后生成的安装目录 |
| `output/build_manifest.json` | 完整构建的输入／成品指纹与验收结果 |
| `.caches/` | 运行暂存、GUI 路径偏好及 `gui.log` |

`release/Runtime` 仅是完整工程附带的 ASI 程序模板，不是工具箱的一部分。完整构建默认在它和 `input/plugin/BloodstoneCN.asi` 中寻找插件，优先使用后者。也可直接在 GUI 选择 ASI；只有组装安装目录才需要它。

## 命令行

在本目录运行：

```bat
python -B -X utf8 toolkit.py prepare --game "D:\Games\007 Blood Stone"
python -B -X utf8 toolkit.py configure-font --font "D:\Fonts\MyFont.ttf" --pixel-size 32
python -B -X utf8 toolkit.py build --mode precise
python -B -X utf8 toolkit.py verify
python -B -X utf8 toolkit.py export
python -B -X utf8 toolkit.py build-loc
python -B -X utf8 toolkit.py build-font --mode precise
python -B -X utf8 toolkit.py status
```

单独分发工具箱时，完整构建可加 `--plugin "D:\Plugins\BloodstoneCN.asi"`；也可使用 `build --font ... --pixel-size 32` 一次配置并构建。初始化失败若提示原始 EUR 模板不兼容，可使用：

```bat
python -B -X utf8 toolkit.py prepare --gamedata "D:\Game\cache\gamedata.pak" --template-dir "D:\MyTemplates"
```

默认不联网、不安装到游戏、不重封 PAK。公开再分发请回到完整工程使用 `source/tools/package_release.py`，避免把个人输入和生成物压入公开包。
