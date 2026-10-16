# SSC338Q IMX415 图传工程设计

日期 2026-10-16。目标是在本目录交付可构建、可测试、可部署的 C11 与 Shell 工程，贯通 SSC338Q/IMX415、RTL8812EU/WFB-ng、NBD8010S-KL-V2 地面硬解与 HDMI，并对接现有 MSP DisplayPort OSD。

## 实现边界

运行端不依赖 Python。电脑端提供文件重放发送和文件接收，用同一套 RTP/H.265 核心验证真实 UDP 传输；硬件后端直接调用 SigmaStar MI 或海思 MPI。Hi3536DV100 是当前地面参考 SDK，实物板 SoC 与 SDK ABI 仍须匹配。编译与语法检查不等于上板验证。

WFB-ng、RTL8812EU 驱动和 msposd 采用已下载的上游快照，保留来源和声明。工程不另造不兼容的无线协议，也不把 UART OSD 描述成原始 IMU 同步或电子防抖。

## 架构

- `fpv-air`：文件或 MI 采集后端取得 H.265 NALU，用公共 RTP 模块打包后经 UDP 或 Linux abstract Unix socket 交给 WFB-ng；支持无无线的直接 UDP 实验。
- `fpv-ground`：接收 RTP，严格解析长度、序号、单 NALU、AP 和 FU；把完整 Annex B NALU 写文件或送海思硬解后端，经 VDEC→VO→HDMI 显示。
- `fpvctl`：读取 INI，校验配置，按角色配置无线接口并以 argv 启动相关进程；前台监督、有限重启、信号清理、独立日志和单实例锁。`dry-run --config PATH` 仅展示命令。
- `msposd`：由控制器按需启动，通过 UART 处理 MSP/DisplayPort 和 OSD；不接管飞控闭环。
- `third_party`：WFB-ng、msposd、EU 驱动及所需参考 SDK 文件，版本记录来自父目录 `SOURCES.json`。

## 数据约定

只实现选定方案的 H.265，RTP payload type 默认 96，时钟 90000 Hz。packetizer 发单 NALU 和 FU；depacketizer 支持单 NALU、AP、FU，不支持 DONL 与 PACI。输出 NALU 带四字节 Annex B 起始码。应用单 NALU 上限 4 MiB，默认 RTP 包长 1200 字节；无线模式不允许超过固定 WFB 版本的 3993 字节载荷上限。

FU 检出丢片、时间戳或 SSRC 变化会丢弃不完整 NALU。迟到乱序包直接丢弃，不做乱序重排，已连续接收的分片不因此失效。序号环绕按 16 位处理。重复包不重复输出，超时清理未完成分片。AP 在完整验证后才输出首个 NAL，错误输入不能造成越界读写。

发送端使用正确递增的 RTP 序号和媒体时间戳，不延续参考程序时间戳恒为零的行为。硬件通路每个编码输出帧共用时间戳，最后一个 NALU 的最后一个 RTP 包设置 marker；文件通路识别 AU 边界并按 FPS 重放，仅支持单层 layer 0，拒绝多层 HEVC。AU 前缀分类对照 FFmpeg HEVC parser 的 frame-end 逻辑检查，实作没有复制其代码。

默认 `transport.random_ssrc=1`，空中程序每次启动从 `/dev/urandom` 取得新的会话SSRC；地面通过SSRC变化丢弃旧分片并立即同步新序号。`random_ssrc=0` 只用于固定SSRC诊断，此时使用配置中的 `ssrc`，发送重启时需同时重启接收。接收端不按自身配置的ssrc过滤视频。

## 配置与进程

INI 分为 `system`、`video`、`transport`、`wireless`、`osd`、`runtime`。air 的 UDP 端口是其本机 WFB 输入端口或直接发送目标；ground 的 UDP 端口是解码接收端口。无线 `radio_port` 与 UDP 端口分开。密钥不放进源码，分别配置 `drone.key` 和 `gs.key`。

后台进程均由控制器记录 PID，退出时只清理自己的子进程，不使用 killall。使用前景运行模式适配 BusyBox init。默认本机实验配置关闭无线和 OSD，不在开发机测试时改变真实无线接口。

## 目录

`include/fpv` 公共 API；`src/common` 媒体与配置；`src/air` 和 `src/ground` 应用；`src/platform` 硬件后端；`src/ctl` 监督器；`configs` 角色配置；`cmake` 工具链；`scripts` 构建部署；`tests` 单元及进程集成测试；`docs` 设计、源码说明和测试记录。

## 完成判据

本机能编译和运行文件→RTP/UDP→接收文件，验证 NALU 内容；覆盖损坏输入、丢包、乱序、重复、序号环绕、超时与重启。硬件后端有真实 SDK 调用并完成可用环境中的语法检查；SDK 目标交叉链接和现场供电、驱动、传感器、HDMI 验证单独记录。提供完整命令和依赖检查，不用空函数冒充硬件实现。
