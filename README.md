# 007 Blood Stone 汉化补丁

> PC《007：血石 / 007 Blood Stone》非官方民间汉化

[![游戏版本](https://img.shields.io/badge/游戏版本-PC-blue)](https://github.com/Kanadeforever/BloodStone_CN/releases)
[![汉化版本](https://img.shields.io/badge/汉化版本-v1.0.0-green)](https://github.com/Kanadeforever/BloodStone_CN/releases)
[![平台](https://img.shields.io/badge/平台-Windows-lightgrey)](https://github.com/Kanadeforever/BloodStone_CN)
[![License](https://img.shields.io/badge/许可-免费分享%20%7C%20禁止商用-red)](https://github.com/Kanadeforever/BloodStone_CN)

---

## 简介

《James Bond 007: Blood Stone》是 Bizarre Creations 开发、Activision 发行的第三人称动作冒险游戏。

本仓库提供该游戏 **PC 版**的汉化补丁。**当前汉化版本为 v1.0.0**，汉化文本经多轮审校已为最终版，但仓库内工具附带的说明文本可能存在早期描述并未修正（也不打算修正了），请以本段对话作为文本质量的最终描述。

---

## 功能特性

- **界面与字幕汉化**：主菜单、选项、关卡信息、对话字幕等核心文本已替换为中文
- **NPC对话汉化**：恢复了游戏发行版中被官方隐藏的NPC对话字幕（全球仅此一家，别无分号）。
- **可选组件**：
  - DXVK 阴影修复（改善部分画面异常）。
  - 手柄支持模组（全球仅此一家，别无分号，PCGW的上传亦是汉化组成员上传的）。
  - 无边框窗口全屏插件（技术人员自制项目，来自[BorderLimited](https://github.com/Kanadeforever/BorderLimited)）。
  - 跳过启动 LOGO。
  - 嵌入式 ASI 载入补丁（汉化不生效时使用的备用方案）。

---

## 安装

见补丁内使用说明。

---

## 发行包目录概览

```
BloodStone_CN/
├── 完全不懂的小白用这个_直接覆盖/
│   ├── scripts/
│   │   ├── BSCNContents/
│   │   ├── BloodstoneCN.asi
│   │   └── NPCSubtitles.asi
│   ├── Bond.exe
│   └── Bond.exe.bak
├── 有基础的用这个_自选需要的组件/
│   ├── [备用]如果汉化无效请使用这个工具为Bond.exe添加载入ASI的能力/
│   │   ├── runtime/
│   │   │   └── python/
│   │   ├── 放入Bond.exe/
│   │   ├── pe_asi_patch.py
│   │   ├── 使用说明.md
│   │   └── 运行补丁.bat
│   ├── 修复阴影错误_DXVK/
│   │   ├── d3d9.dll
│   │   └── dxvk.conf
│   ├── 手柄支持模组/
│   │   ├── CN_Data/
│   │   │   └── BondGamepadIcons.asi
│   │   ├── Third_Party_Licenses/
│   │   ├── dinput8.dll
│   │   ├── joyshim.toml
│   │   ├── readme.txt
│   │   └── SDL3.dll
│   ├── 无边框窗口全屏插件/
│   │   └── CN_Data/
│   │       ├── BorderLimited.ini
│   │       ├── BorderLimited.x86.asi
│   │       └── BorderLimitedConfig.exe
│   ├── 汉化模组/
│   │   ├── CN_Data/
│   │   │   ├── BSCNContents/
│   │   │   ├── BloodstoneCN.asi
│   │   │   └── NPCSubtitles.asi
│   │   ├── languages.txt
│   │   ├── version.dll
│   │   └── version.ini
│   └── 跳过LOGO/
│       └── CN_Data/
│           └── ui/
│               └── movies/
│                   ├── AttractMode.bik
│                   ├── atvi.bik
│                   └── BizSplash.bik
└── 汉化补丁使用说明.txt
```

---

## 常见问题（FAQ）

见补丁内使用说明。

---

## 免责声明

1. 本汉化补丁为**非官方民间汉化作品**，仅用于学习交流、语言研究与个人备份测试，不提供游戏本体，也不替代正版游戏。
2. 游戏本体及相关商标、角色、文本、图像、音频、视频等素材版权归原开发商、发行商及其他合法权利人所有。本补丁与上述权利人无隶属、授权或合作关系。
3. 本补丁**免费分享，禁止商用**：禁止售卖、捆绑销售、付费下载或以其他方式牟利。转载或二次发布时请保留本说明及免责声明，并注明来源。
4. 使用前请**自行备份**游戏目录及相关文件。因覆盖、替换、删除文件、安装第三方组件、模组冲突、杀软误报、系统环境差异、游戏版本不匹配、联机使用或其他操作造成的游戏损坏、存档丢失、系统异常等后果，由使用者自行承担。
5. 下载、安装或使用本补丁，即视为您已阅读、理解并同意本免责声明。如不同意，请勿使用并删除本补丁。

请支持正版游戏。

---

## 致谢

- Bizarre Creations：游戏开发组（也感谢你们开发了 Project Gotham Racing/哥谭赛车计划/世界街头赛车 系列）
- [Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)： 插件加载器。
- ChatGPT & DeepSeek：手柄插件、手柄图标模组、汉化技术实现。
- [DXVK ](https://github.com/doitsujin/DXVK)：解决游戏新系统/设备下的图形问题。
- [SDL](https://github.com/libsdl-org/SDL)：手柄模组底层技术支持。
- [Zacksly](https://zacksly.itch.io)：手柄图标作者。
- 西班牙语翻译组：定位文本文件。

---

## 反馈

如遇到问题，请在 [Issues](https://github.com/Kanadeforever/BloodStone_CN/issues) 中反馈，并尽量附上：游戏版本、操作系统、安装方案、是否安装其他模组、问题截图/日志、已尝试的排查步骤；如信息不够详细，本项目维护者将不予受理。
