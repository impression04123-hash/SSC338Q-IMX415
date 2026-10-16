# 硬件后端与上板边界

## SDK 和资源归属

空中端面向 Infinity6E MI ABI（SSC338Q），地面端使用 Hi3536DV100 MPI ABI。**NBD8010S-KL-V2 是板卡型号，不能仅凭它确认实际 SoC、修订版、内核与 SDK ABI。** 当前 SDK 来自已下载的固定版本源码集合；上板前必须逐项核对。

后端为每进程单实例。空中端使用 sensor/VIF/VPE/VENC 的 0 号资源，地面使用 VDEC/VO/HDMI 0 号资源。应停止原固件的视频服务并由本程序独占媒体系统。传感器模块加载、设备树/板级 pinmux、供电、USB 和基础驱动沿用匹配固件，不在应用程序中伪造实现。

## 空中端：IMX415 到 H.265

`sigmastar_capture.c` 的真实调用顺序是：

```text
MI_SYS_Init
 → MI_SNR_GetRes / SetRes / SetFps / Enable
 → MI_VIF_SetDevAttr / EnableDev / SetChnPortAttr / EnableChnPort
 → MI_VPE_CreateChannel / StartChannel / SetPortMode / EnablePort
 → MI_SYS_BindChnPort2(VIF → VPE)
 → ISP 参数文件 / 曝光限制
 → MI_VENC_CreateChn / StartRecvPic
 → MI_SYS_BindChnPort2(VPE → VENC)
 → poll / Query / GetStream / ReleaseStream
```

输入分辨率和 FPS 由所选 `sensor_mode` 的 SDK 描述决定。程序检查请求 FPS 在模式范围内，并拒绝输出尺寸超过传感器输入。VIF/VPE channel 保留完整输入，VPE port 才缩放为配置输出尺寸，VENC 使用相同输出尺寸。

编码使用 H.265 CBR，`bitrate_kbps` 乘以 1000 转为 bit/s；显式关闭 B 帧并设置一个参考帧，GOP 来自配置。输入像素为 8-bit YUV420，目标地面链路按 Main 8-bit 验证。曝光上限为一帧周期。`sensor=imx415` 用来查找 `/etc/sensors/imx415.bin` ISP 参数，缺文件时打印提示并沿用固件默认值；它不会替代真正的 IMX415 内核驱动或完成 ISP 标定。

VENC 使用 by-frame 输出。每个 pack 只复制 `[u32Offset,u32Len)` 有效区间，pack 内 `asackInfo` 属于重叠元数据，不再重复复制。先合并整帧，再解析 Annex B，因此可处理起始码跨 pack 边界。要求最后一个 pack 有 `bFrameEnd`；AU 总上限 16 MiB、单 NAL 上限 4 MiB。先完整校验 AU，再调用视频发送回调；最后一个 NAL 设置 AU 尾标记。每次成功 `GetStream` 均配对 `ReleaseStream`，包含分配、解析和回调失败路径。

当前按此 SDK 常见约定把 MI PTS 解释为微秒，再换算 90 kHz；PTS 为零时以帧数/FPS 计时。所附 `u64PTS` 头文件本身未标明单位，**该约定需要在板端比对 PTS 增量与实际帧率确认**，不能据此声称完成 IMU/曝光同步。

## 地面端：RTP 到 HDMI

`hisilicon_display.c` 使用真实的 VB/SYS/VDEC/VO/HDMI API：

```text
检查既有 VDEC 占用
 → VB/SYS 初始化与 H.265 picture / temporal-MV module pools
 → VDEC_CreateChn(VIDEO_MODE_STREAM) / preview 显示模式
 → VO device / layer / channel
 → SYS_Bind(VDEC → VOU) / VDEC_StartRecvStream
 → HDMI_Init / Open / SetAttr / RegisterCallback / Start
 → VDEC_SendStream(完整 Annex B NAL)
```

解码采用 stream 模式，因为一个 NAL 不等于一帧。RTP marker 转为 `bEndOfFrame`。90 kHz 时间戳先扩展处理环绕，再换算 SDK 微秒。`SendStream` 最多等待 100 ms，错误上传到应用，不静默丢掉随机 slice。

VO/HDMI 输出明确支持 `720p60` 或 `1080p60`，RGB 输出，关闭音频和 HDCP。HDMI callback 在拔出时停止，在插入时重新应用配置。callback 与关闭流程之间使用互斥锁。显示端需要支持对应时序并具有 HDMI 输入。

`video.width/height` 是接收码流的最大尺寸，独立于 `output_mode` 显示时序。解码池根据 SDK 宏分配，按最多两个参考帧设计。发送和接收应匹配编码尺寸；第三方文件测试须为地面芯片支持的 H.265 Main 8-bit 码流，不能随意把 Main10、多参考帧或 4K 高帧率文件作为已经兼容的输入。

## 失败回滚与独占要求

两个后端记录每个已完成的初始化阶段，在错误和正常关闭时按依赖顺序回收。地面遇到现有 VDEC channel 就退出，不在启动时执行全局 `SYS_Exit`、遍历销毁所有 VB pool 或强行重置其他通道。

VB/SYS 和 VDEC module pool 仍是 SDK 全局资源，因此该后端需要独占媒体系统。仅检查没有 VDEC 通道不代表没有其他程序占用 VO/VB；不应与原 NVR/摄像头媒体程序同时运行。驱动故障时清理只能尽力执行，需在实物上检查资源是否释放。应用的 open/poll/write/close 在同一个视频线程调用；仅 HDMI SDK callback 并发。

## 验证顺序

1. `sh tests/hardware/syntax-check.sh` 检查真实 SDK 头文件和 SDK 无关的 AU 边界测试。它没有链接厂商库。
2. 使用分别匹配固件的 ARM 工具链/sysroot，交叉链接 `fpv-air` 和 `fpv-ground`。检查 ELF 架构、动态加载器和 `readelf -d` 的库依赖；不添加空 libc/assert/ctype 符号掩盖 ABI 不匹配。
3. 空中端先验证模式、ISP 文件、尺寸、FPS、码率和 GOP，连续录制至少 300 帧；另测试 VPE 缩小输出、PTS 单位、marker 位置和资源重启。
4. 地面先重放已独立验证的码流，再试摄像头；分别验证两种 HDMI 时序、热插拔、错误路径与重启后资源释放。
5. 在有线视频可用后再接 WFB 和 UART。分阶段测量时延、温度和持续运行情况。

空中依赖 MI sensor/sys/vif/vpe/venc/isp、ISP algorithm 和 camera OS wrapper；地面依赖 MPI、HDMI 及其配套库、pthread。Hi3536 SDK 头文件使用 GNU `asm`，需要 GNU C11；主 CMake 已启用该语言扩展。

2026-10-16 已完成两后端实际 SDK 头文件的 `-Wall -Wextra -Werror` 语法检查及 AU 边界的 ASan/UBSan 检查。**ARM 链接、传感器/驱动、实际 HDMI、射频、时延、温升和实飞均未验证。**
