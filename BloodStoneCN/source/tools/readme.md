# 开发工具使用说明

所有开发脚本直接放在本目录，不再区分“工程工具”和“插件工具”两层。脚本按自身位置找到工程，不要求特定当前目录。

| 文件 | 用途 |
|---|---|
| bootstrap_toolbox.py | 检查独立 toolbox 路径并补齐空工作目录，不覆盖个人输入 |
| run_tests.py | 统一执行 source/tests 的全部回归，临时数据放根 .build |
| package_release.py | 按 config/public_files.json 白名单打包完整公开工程，不携带游戏输入 |
| build_clang_cross.py | 用本机已有 clang-cl/lld-link 编译 x86 ASI，输出到根 release/Runtime |
| verify_dump.py | 只读核验自己提供的明文 EXE，默认签名位于 source/config/profile.json |

命令示例以工程根为当前目录：

```text
python -B source/tools/bootstrap_toolbox.py
python -B source/tools/run_tests.py
python -B source/tools/package_release.py
python -B source/tools/build_clang_cross.py
python -B source/tools/verify_dump.py --help
```

检查和测试不要求游戏资源；真实旧工程、双 PAK 和明文 EXE 对照通过对应环境变量明确提供，不自动搜索或下载。交叉编译器也不会自动获取。

公开白名单只列明确审定的源码、程序和文档，不递归收集个人目录。新增公共文件需同步白名单；ASI 指纹改变时先审核再更新固定值，不关闭检查。完整目录、构建方式与替换映射见相邻 source/readme.md 和 source/docs/完整接档.md。
