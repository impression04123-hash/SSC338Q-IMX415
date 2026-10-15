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

以上为开发范围说明，具体适配改动、测试条件和实测结果应随实际开发另行记录。本次导入不代表已在目标板完成编译或运行验证。

## 版本与第三方来源

本仓库收集第三方公开项目的指定版本快照，便于统一阅读、适配和联调。第三方源码的原作者归属、版权及许可证保留；上传和整理这些快照不改变原代码的归属。

- [SOURCES.json](SOURCES.json)：上游 URL、版本和完整提交号。
- [THIRD_PARTY.md](THIRD_PARTY.md)：各子项目来源和许可证说明。
- [VERIFICATION.json](VERIFICATION.json)：原始下载的完整性检查记录，不代表目标硬件测试报告。

WFB-ng 固定到当前下载的固件配方所选提交 `ae2122c1fb70cd215bbc2e1b517b7b91f9401220`。Majestic 主体及部分芯片库以预编译组件提供，并不包含完整公开源码。通用地面端配置仍需针对 EU 驱动及所用固件进行核对和适配。
