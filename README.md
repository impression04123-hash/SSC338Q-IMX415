# SSC338Q + IMX415 无人机数字图传

面向 SSC338Q + IMX415 空中端、NBD8010S-KL-V2 地面端及 RTL8812EU 无线模块的软件工程、学习资料与源码快照。

## 视频链路

IMX415 → SSC338Q 采集与 H.265 编码 → RTP/UDP → WFB-ng → RTL8812EU 无线链路 → 地面端 WFB-ng → RTP 重组 → VDEC 硬件解码 → VO → HDMI → 显示器或支持 HDMI 输入的 FPV 头显。

飞控通过 UART 与空中端交换 MSP/MSP DisplayPort 数据，用于飞行信息及 OSD。原始 IMU 数据转发和视频时间同步应按实际协议另行实现。

## 目录

| 目录 | 内容 |
|---|---|
| [Project](Project/) | 可构建的双端 H.265/WFB-ng 工程、SDK参考后端、配置与测试 |
| [学习手册](学习手册/) | 项目总体学习手册，提供 Markdown 与 PDF |
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

## 可构建工程与验证

从 [Project/README.md](Project/README.md) 开始；目标运行程序使用 C11/POSIX，包含空中采集编码、H.265 RTP、WFB-ng服务编排、UART/MSP DisplayPort OSD、地面硬解/HDMI和进程监督。

在 Linux/WSL 中，从仓库根目录运行：

```sh
cd Project
bash scripts/build_host.sh
```

已完成的主机验证为 CTest 12/12、ASan/UBSan 12/12、root 运行监督 13/13。UDP、UNIX和WFB开发模拟空口三条路径各35帧H.265，解码帧MD5一致；WFB实际丢片FEC恢复及错误密钥拒绝测试通过。条件、日志位置与未验证事项见 [测试报告](Project/docs/TEST_REPORT.md)。ARM交叉链接及实际传感器、无线和HDMI仍需上板验证。

工程按普通文件目录收录，未嵌入第二个Git仓库，构建产物、测试日志和私钥不随源码上传。

## 地面板卡与 SDK

NBD8010S-KL-V2 的产品规格标注 Hi3536E；本工程使用 OpenIPC `hi3536dv100` 参考平台组件，需核对实际芯片修订、固件和SDK ABI。MPP/BSP与雄迈客户端NetSDK用途不同，详细来源、获取入口和向供应方索取的清单见 [SDK选型说明](Project/docs/SDK_SELECTION.md)。
