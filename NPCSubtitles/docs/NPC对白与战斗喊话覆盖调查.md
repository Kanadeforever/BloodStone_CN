> **历史调查记录**：本文保留战斗喊话资源调查过程。实际实验推进到 v0.1f 后因 VoiceSet 无法可靠辨识而回退，当前发行仅保留 v0.1e 的 NPC 字幕。详见《版本及验证状态.md》。

# NPC 对话索引排除与战斗喊话覆盖调查

## 一、v0.1d 实机证据

用户确认中文 NPC 字幕可见，日志中 22 个脚本对白文件索引出 1,374 条条目。`
NPC_DIALOGUE_INDEX_MISS` 混合了两种语义：资源不存在，以及资源存在但被 `SkipKey` 拦截。

## 二、已精确定位的六个事件

利用原版 `bloodstone.gamedata.loc.complete.zip` 内的 OLTX KeyTable 逐项按 32 位哈希匹配。六个都在 `gamedata/xt/loc/levels/monaco/monaco_stealth_script_dialogue.loc` 中：

| 十进制 hash | 十六进制 hash | 原生 Key | 原因 |
|---|---|---|---|
| 2837524520 | A9213028 | Monaco_01_NICOLE_85 | NICOLE 排除 |
| 3098113697 | B8A976A1 | Monaco_01_BOND_86 | BOND 排除 |
| 1277597592 | 4C269798 | Monaco_01_NICOLE_87 | NICOLE 排除 |
| 1895657820 | 70FD715C | Monaco_01_NICOLE_194 | NICOLE 排除 |
| 928214458 | 37536DBA | Monaco_01_BOND_88 | BOND 排除 |
| 3683402653 | DB8C439D | Monaco_01_NICOLE_89 | NICOLE 排除 |

这不是资源解析失败。v0.1e 日志新增 `NPC_DIALOGUE_FILTERED_HASH`，仍只允许未被硬排除、未发生哈希文本冲突的 NPC 记录进入字幕队列。过滤发生在查表命中之后，不再伪装成 MISS。

## 三、被敌人发现后的喊话属于不同来源的可能性

原版 `gamedata/xt/loc/audio_battlechatter.loc` 中有 2,858 条键，例子：

- `BG_01_SpotKnownThreatTM_01` 等；
- `BG_01_SeenSomethingTM_01` 等；
- `BG_01_SearchHiddenPlayerTM_01` 等；
- `BG_01_PlayerNotFoundTM_01` 等。

源码统计中约 631 个键名包含 `Spot`，但这只是键名文字统计，**不等于运行时有 631 个不同发现事件**。没有声音触发回调/键的实机证据时，无法证明某句消失的喊话确实触发了哪一个键。

## 四、下一轮技术调查路径

1. 在运行时 `Bond.exe` 已解密代码里定位 BattleChatter / VoiceSet / 音频事件进入文本哈希或资源 ID 的入口。优先只读捕获目标场景一次真实事件，不依赖音量或字幕显示结果推断。
2. 对照 `audio_battlechatter.loc` KeyTable，记录真实键或 hash、是否存在中文字串以及声音播放时间戳。
3. 区分敌人发现、战斗、搜索、队友对话及原生过场字幕；对有明确声音事件的类别单独入队。
4. 避免恢复历史上已经封存且未经本次验证的任意 BattleChatter Hook，定位成功后重新按当前 EXE 指纹核对 ABI。
5. 对长对白、多 NPC 并发、重复喊话和无事件时的性能做实机检查。

原则：扩展字幕覆盖不能用字符串包含判断、凭猜测显示所有资源文本，也不能破坏目前用户已确认可见的 NPC 字幕和 v0.9 主汉化。
