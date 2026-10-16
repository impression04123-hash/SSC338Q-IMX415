# 第三方来源与许可

`src/platform/sigmastar_capture.c` 和 `hisilicon_display.c` 的 SDK 初始化流程参考 OpenIPC/research，保留其声明，适用文本见 `third_party/sdk/RESEARCH-LICENSE`。新增公共核心、控制器与应用并不改变这些原始贡献的归属。

完整提交号和父仓库下载记录见 `third_party/SOURCES.json`；实际复制组件列在 `copied_components`，并非记录中的所有仓库都已复制。逐文件校验值见 `third_party/SHA256SUMS`。

| 目录 | 上游与固定提交 | 声明 |
| --- | --- | --- |
| wfb-ng | svpcom/wfb-ng, ae2122c1fb70cd215bbc2e1b517b7b91f9401220 | 保留 LICENSE.txt 及源文件声明 |
| msposd | OpenIPC/msposd, ad0175ce01c4e09b3eb4223fd683735f9df22715 | 保留 LICENSE、bmp/lib/LICENSE 及各依赖声明 |
| rtl88x2eu-20230815 | libc0607/rtl88x2eu-20230815, 12977e4013ecafbae993b3b08d46bff87d6d7c67 | 保留源文件中的 Realtek/GPL 声明；该快照没有独立 LICENSE 文件 |
| sdk/*/include | OpenIPC/research, 30e6fa0a546e218f0a83eae6e48c17f91a76081c | 参考仓库 LICENSE 单独保存为 RESEARCH-LICENSE |
| sdk/*/lib | OpenIPC/firmware, 1d9c834e09db66967d6fc2899e5acd89b6446482 | 原始目标 SDK .so 文件；仓库 LICENSE 和包配方附带保存 |

本工程不重新许可任何第三方组件。SDK 头文件和二进制可能有各自厂商条款，不能仅凭参考仓库/固件仓库的 MIT 声明推定这些文件全部采用 MIT。使用与再分发前应依据文件声明和原厂 SDK 协议确认权限；这里保留可追溯来源，不作额外授权。

快照不含 `.git`、生成对象、内核模块、Python 缓存。SDK `.so` 是有意保留的厂商链接依赖，不是本机编译产物。未复制内核源码、完整工具链或 sysroot。重新生成：在 Windows 或 Linux 安装 Python 3 和 Git 后运行 `python scripts/vendor_sources.py <包含上述固定提交仓库的父目录>`。脚本先校验提交及已跟踪文件的修改状态，再复制；改变文件选择时应在全新工程目录重建，脚本不递归清理旧内容。
