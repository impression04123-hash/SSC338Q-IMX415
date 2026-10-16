# 工程验证记录

日期：2026-10-16。测试环境：Windows 的 WSL Ubuntu x86_64，GCC 13.3.0、CMake 3.28.3、FFmpeg 6.1.1（libx265）、Python 3.12.3。原始日志保存在本工程 `build/*-final.log` 与各构建目录的 `Testing/Temporary/LastTest.log`，生成物不进入源码提交。

## 已执行结果

| 项目 | 验证方法 | 结果 |
|---|---|---|
| 主机工程 | `bash scripts/build_host.sh` | 三个应用和全部 C 测试编译成功；CTest 12/12 通过 |
| 严格编译 | 另建 `build-host`，`CMAKE_C_FLAGS=-Werror` | 公共核心、应用和监督器开启 Wall/Wextra/Wpedantic/Wshadow/Wconversion，无告警编译通过 |
| 内存与未定义行为 | `FPV_SANITIZE=ON`、ASan/UBSan、泄漏检查、遇错停止 | CTest 12/12 通过，无 sanitizer 报告 |
| RTP 基本行为 | 单 NAL/FU/AP、丢包、重复、环绕、SSRC、扩展头/padding | 通过 |
| RTP 边界 | FU容量和身份变化、AP原子拒绝及最小layer/TID、回调错误、损坏公开state | 通过；独立的 packetizer 溢出复现用例已转为拒绝输入 |
| 损坏输入压力 | 固定种子 50,000 次输入及 500 个随机 NAL roundtrip | 通过；也在 ASan/UBSan 下运行 |
| 配置 | 默认值/数值边界、未知键、引号/特殊字符、溢出、四份 INI、OSD限制 | 通过；扩展测试 105 项检查 |
| I/O | 3/4字节起始码、EOF/空NAL、4MiB边界、rewind、UDP/UNIX截断后继续接收 | 通过 |
| 视频内容 | FFmpeg生成35帧，UDP和UNIX分别收发，再解码比较framemd5 | 各35帧完整一致 |
| 原样示例运行 | 使用随附的demo INI，由真实fpvctl启动air/ground并读取150帧样例 | 150帧解码完整，两个监督器正常退出 |
| 完整 WFB 视频链路 | fpv-air→wfb_tx→UDP模拟空口→wfb_rx→fpv-ground→FFmpeg | 35帧解码MD5一致，FEC 8/12 |
| RTP 元数据 | 真实UDP抓取、7项方法、多slice、prefix 41/42/44、单层拒绝、多次loop/max_frames | 通过 |
| 运行监督 | 真实子进程、argv、OSD尺寸、有限重启、启动等待失败、信号/锁/其他进程存活 | 普通用户12通过+1仅root测试跳过；WSL root运行13/13通过；未加载真实模块 |
| WFB-ng 构建 | 固定快照 `all_bin` | 5个主机程序成功；保留并记录上游告警 |
| WFB FEC丢包 | 代理每个2/3 block实际丢一个数据fragment，只留另一数据片+校验片 | 4个block共丢4片，8个1017字节载荷全部恢复 |
| WFB错误密钥 | 错误gs.key接收完整会话/数据流 | 记录解密失败，观察期无明文输出，接收器保持运行 |
| 硬件代码 | 对真实MI/MPI头文件 `-fsyntax-only -Wall -Wextra -Werror` | 两后端通过；应用硬件分支语法检查也通过 |
| 暂存部署 | 实际CMake安装、二进制/INI/完整许可逐字节比较、目的目录保护 | 通过；配置在 `/etc/fpv`，无私钥，已有目录拒绝覆盖 |
| 第三方完整性 | `sha256sum -c third_party/SHA256SUMS`（在third_party目录运行） | 1,604个文件全部匹配 |

原始 Annex B 的起始码可能为3或4字节，接收输出统一为4字节，因此输入79,796字节和输出79,802字节不同是预期；解码帧MD5相同。没有用总文件字节相同代替视频内容验证。

## 修复过的可复现问题

测试先复现、实现/修复后重新验证：未实现的配置/RTP接口；单个NAL的非法AP、AP最小TID/layer、packetizer MTU下溢；引号中的负号/空白U32；超过WFB载荷上限的配置；空起始码/全零文件/最后空NAL；AU前缀41/42/44的timestamp/marker；文件多层输入；启动等待阶段绕过重启预算；OSD未显式指定尺寸、视频就绪前挂载及重启后未重新挂载；固定SSRC与重置序号造成重启后拒收（默认改为每次新会话并加入真实进程回归）；OSD不支持的串口速率和尺寸。保留用例防止回归。

## 复验命令

```sh
bash scripts/build_host.sh
cmake -S . -B build/sanitize -DFPV_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sanitize --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/sanitize --output-on-failure
bash scripts/build_wfb.sh
python3 scripts/test_wfb_loopback.py build/wfb-host/bin
python3 tests/test_pipeline.py build/host build/wfb-host/bin
python3 tests/test_stage.py build/host build/wfb-host
sh tests/hardware/syntax-check.sh
(cd third_party && sha256sum -c SHA256SUMS)
```

## 未验证事项

没有可确认匹配的 ARM Linux 工具链/sysroot，也没有连接实物板卡。本次未执行 ARM 交叉链接、真实SDK库ABI验证、内核模块构建/加载、传感器出图/ISP、UART实测、无线Monitor/Injection、射频距离、HDMI实显/热插拔、端到端时延或温升/实飞测试。发现的裸机 `arm-none-eabi` 工具链不能替代 Linux 工具链。

WFB 的 `-D`/`-a` 只是固定版本提供的开发模拟接口；丢片恢复证明FEC实现路径工作，不能证明真实无线干扰环境下的性能。SDK头文件语法通过不能证明二进制库、实际板卡身份或运行时驱动匹配。上板验收清单见 HARDWARE.md 和 DEBUGGING.md。
