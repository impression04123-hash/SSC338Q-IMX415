# SSC338Q · IMX415 无线高清图传工程

面向 **SSC338Q + IMX415 空中端 → RTL8812EU / WFB-ng → NBD8010S-KL-V2 地面端 → HDMI 显示屏或头显** 的 C11 工程。软件包含采集编码、H.265 RTP 收发、地面硬解显示、无线服务编排和 UART MSP/DisplayPort OSD 集成。

板卡规格标注主控为 **Hi3536E**；地面后端采用 OpenIPC `hi3536dv100` 平台的参考 SDK/媒体库，实物芯片修订、固件与 SDK ABI 仍需核对。SDK 种类、获取入口和匹配方法见 [SDK_SELECTION.md](docs/SDK_SELECTION.md)。当前交付完成主机编译、协议/进程测试和 SDK 头文件检查；**尚未完成 ARM 目标链接及实物上板验证，不能视为已经可飞的固件。**

## 数据链路

```text
IMX415 → Sensor/VIF → VPE/ISP → H.265 VENC → fpv-air
                                                  ↓ RTP / 本机 UDP 或 abstract UNIX
飞控 UART → msposd → MI_RGN OSD                  wfb_tx
                                                  ↓ 加密 + FEC + 802.11 注入
                                       RTL8812EU 无线链路
                                                  ↓ Monitor 接收
                                               wfb_rx
                                                  ↓ 恢复的 RTP / 本机 UDP 或 UNIX
fpv-ground → H.265 解包 → Hi3536DV100 VDEC → VO → HDMI → 屏幕/头显
```

两端网卡工作在 Monitor/Injection 模式，按相同信道、带宽、link_id、radio_port 和配对密钥通信，不需要连接 Wi-Fi 热点。应用的 UDP 端口是本机媒体交接端口；真正的空口使用 WFB-ng 原始 802.11 帧。

空中端默认 `transport.random_ssrc=1`，每次启动创建新的 RTP 会话，使地面端立即识别重启并重置序号状态。接收端无需配置相同 SSRC。诊断时可设 `random_ssrc=0` 使用指定 `ssrc`；这种模式下重启发送端应同时重启接收端，避免新旧序号混淆。

UART 使用 msposd 读取飞控 MSP/DisplayPort，把 OSD 绘入视频。姿态等信息由飞控提供；本工程不读取原始 IMU 做闭环控制、电子防抖或视频时间同步。HDMI 输出要求显示屏/眼镜具有相应 HDMI **输入**及支持的时序。

## 目录与入口

| 路径 | 内容 |
|---|---|
| `src/air/main.c` | 文件回放/MI 采集，AU 时间戳与 RTP 发送 |
| `src/ground/main.c` | RTP 接收、统计、文件输出/硬解显示 |
| `src/common/rtp.c` | RFC 7798 单 NAL、FU 分片、AP 接收、序号/超时处理 |
| `src/common/config.c` | 严格 INI 解析、类型及范围校验 |
| `src/common/io.c` | UDP/abstract UNIX、Annex B 流式读取 |
| `src/platform/` | SigmaStar MI 与海思 MPI 的真实 SDK 调用 |
| `src/ctl/fpvctl.c` | 配置检查、命令预览、无线配置、服务监督 |
| `configs/` | air、ground 及关闭无线/OSD 的电脑实验配置 |
| `third_party/` | 固定版本 WFB-ng、msposd、EU 驱动和 SDK |
| `cmake/`、`scripts/` | 交叉编译、组件构建及暂存部署 |
| `tests/` | 协议边界、损坏输入、I/O、进程、真实 H.265 测试 |
| `docs/` | 设计、构建、硬件、运行、调试及测试报告 |

## 先在电脑跑通

在 WSL Ubuntu/Linux 中运行。需要 CMake、GCC、Make、Python3 和带 libx265 的 FFmpeg。运行端三个 C 程序不依赖 Python。WFB 主机测试还需要 libpcap、libsodium、libevent 的开发包。

```sh
cd '/mnt/e/Users/Color/Good study/Linux/OpenIPC/Project'
bash scripts/build_host.sh
test -f samples/demo.h265 || bash scripts/generate_sample.sh
```

先运行地面端，再在另一个终端运行空中端：

```sh
build/host/fpv-ground --config configs/demo-ground.ini
build/host/fpv-air --config configs/demo-air.ini
```

地面端收到 `received.h265`，停止发送后按示例空闲时间退出。可用 `ffplay received.h265` 播放。`ctest` 的 `video_pipeline` 会自动生成真实 H.265，在 UDP/UNIX 两条链路各传输 35 帧，并逐帧比较 FFmpeg 解码后的 MD5。

使用监督器体验配置校验与服务运行：

```sh
build/host/fpvctl check --config configs/demo-air.ini
build/host/fpvctl dry-run --config configs/demo-air.ini
build/host/fpvctl run --config configs/demo-air.ini
```

`run` 保持前台，Ctrl+C 清理自己的进程组。文件模式正常结束会带动监督器干净退出；硬件服务意外结束按配置进行有限重启。

## 验证真正的 WFB-ng

```sh
bash scripts/build_wfb.sh
python3 scripts/test_wfb_loopback.py build/wfb-host/bin
```

这是固定版本 WFB 的真实程序、配对密钥和 FEC 编解码，通过 UDP 模拟空口，不需要网卡。测试包含逐包一致、每个 FEC block 丢一个数据片仍恢复、错误密钥拒绝输出；无线驱动与射频性能需要实物验证。

## 上板顺序

1. 核对两端 SoC、内核、动态加载器、libc/浮点 ABI；按 [BUILD.md](docs/BUILD.md) 分别交叉构建。电脑二进制不能直接复制到 ARM 板运行。
2. 保留原固件和传感器支持，确认 IMX415 驱动/模式/ISP 参数文件及 RTL8812EU USB 枚举、模块 vermagic。停止占用 MI/MPP 的原视频服务。
3. 先关闭无线和 OSD，以有线 UDP 验证 MI 编码及地面 VDEC/VO/HDMI；确认尺寸、码率、FPS 和 H.265 Main 8-bit。
4. 生成一次配对密钥，分别安全保存 air 的 `drone.key` 和 ground 的 `gs.key`，再启用无线链路。两个文件不同，必须来自同一次 keygen。
5. 单独验证 UART 电平、串口设备和飞控 MSP/DisplayPort 设置，安装 msposd 所需字体资源，然后启用空中端 OSD。
6. 按 [DEBUGGING.md](docs/DEBUGGING.md) 记录时延、丢包、FEC 恢复、温度、重启及 HDMI 热插拔结果。

详细接口限制见 [HARDWARE.md](docs/HARDWARE.md)，服务启动/日志见 [RUNTIME.md](docs/RUNTIME.md)，实际验证结果见 [TEST_REPORT.md](docs/TEST_REPORT.md)。
