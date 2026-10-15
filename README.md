# SSC338Q + IMX415 无人机数字图传

面向 SSC338Q + IMX415 空中端、NBD8010S-KL-V2 地面端及 RTL8812EU 无线模块的软件开发资料与源码快照。

## 视频链路

IMX415 → SSC338Q 采集与 H.265 编码 → RTP/UDP → WFB-ng → RTL8812EU 无线链路 → 地面端 WFB-ng → RTP 重组 → VDEC 硬件解码 → VO → HDMI → 显示器或支持 HDMI 输入的 FPV 头显。

飞控通过 UART 与空中端交换 MSP/MSP DisplayPort 数据，用于飞行信息及 OSD。原始 IMU 数据转发和视频时间同步应按实际协议另行实现。

## 目录

| 目录 | 内容 |
|---|---|
| [research](research/) | SigmaStar 采集编码、RTP 和海思地面解码/HDMI 参考实现 |
| [wfb-ng](wfb-ng/) | 无线收发、FEC、加密认证和链路统计 |
| [msposd](msposd/) | UART、MSP DisplayPort、OSD 与 SITL/回放说明 |
| [rtl88x2eu-20230815](rtl88x2eu-20230815/) | 当前固件配方引用的 RTL8812EU/RTL8822EU 驱动 |
| [rtl8812eu-wfb](rtl8812eu-wfb/) | WFB-ng 上游推荐的 EU 驱动，供差异对照 |
| [divinus](divinus/) | 开源视频采集编码和推流器参考 |
| [firmware](firmware/) | 固件构建工程、芯片库、软件包和启动配置 |
| [builder](builder/) | 设备构建配置及覆盖文件 |

建议先阅读 [源码导读](源码导读.md)，其中列出了各模块的入口文件、调试方法及构建范围。

## 开发关注点

- RTL8812EU 模块集成、USB 与无线接口初始化，以及 Monitor/Injection 联调。
- Sensor/VIF/VPE/VENC 视频链路、H.265 编码配置和 RTP/UDP 接口。
- WFB-ng 信道、带宽、MCS、FEC、密钥和链路统计。
- 地面端 RTP/NALU 重组、VDEC 送流、VO 和 HDMI 显示。
- UART/MSP DisplayPort 协议与 OSD 叠加。
- 有线旁路、码流回放、链路抓包、SITL 测试和端到端延迟测量。

