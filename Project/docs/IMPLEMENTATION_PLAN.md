# FPV 工程实现计划

> 使用 subagent-driven-development 实现独立模块；协议核心与应用由主执行者完成，硬件后端、运行监督和构建集成分工，所有模块在本会话中集成验证。

**Goal:** 交付可测试且具有真实硬件后端的双端 H.265/WFB-ng 图传工程。
**Architecture:** 公共媒体核心配合 air/ground 应用，C 监督器组织已有无线与 OSD 程序。文件模式验证协议与数据路径，MI/MPI 模式使用相同媒体接口。
**Tech Stack:** C11 POSIX CMake CTest Shell WFB-ng MI MPI。
**Spec:** `docs/DESIGN.md`

## 全局约束

- 用户明确要求直接生成完整工程到 Project，设计与计划写入本目录后继续执行，无额外审批停顿。
- H.265 RTP 90000 Hz，默认包长 1200，NALU 上限 4 MiB；不实现 DONL/PACI。
- 目标运行端无 Python，开发与测试工具可使用 Python。
- 不修改父目录上游仓库，不上传远程，不编造硬件运行结果。

## 任务

- [x] Task 1 公共配置与媒体核心。先建立失败测试，完成 INI 校验、RTP 包头、H.265 打包重组、Annex B 读取、UDP/Unix 交接与日志。API 见 include/fpv。
- [x] Task 2 真实硬件后端。实现 capture_open/poll/close 和 display_open/write/close；复用参考初始化，正确处理失败清理、帧边界、码率与输出尺寸。使用 SDK 头文件语法检查，记录交叉链接依赖。
- [x] Task 3 C 运行监督。实现 fpvctl check/dry-run/run、无线初始化 argv、子进程监督和退出清理；为命令参数、异常退出、重启上限和 PID 所有权建立测试。
- [x] Task 4 构建与上游集成。固定上游快照，提供 CMake 目标、交叉工具链、WFB/驱动/OSD 构建脚本与部署文件，保留声明，输出构建依赖检查。
- [x] Task 5 双端应用与端到端验证。真实 H.265 文件 UDP 回环、分片与丢包实验、配置示例、README、硬件联调步骤及测试报告；完成代码审查和必要修复。

## Review Focus

RTP 扩展与 padding 的边界；序号环绕和 SSRC 重启；FU 丢片后错误拼接；进程退出时误杀其他服务；配置参数被 shell 解释；文件 AU 时间戳与 marker；VDEC stream 模式和 NALU 输入的匹配。
