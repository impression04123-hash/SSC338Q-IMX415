# NBD8010S-KL-V2：芯片与 SDK 选型

核对日期：2026-10-16。

## 先区分板卡名称、芯片和平台目录

NBD8010S-KL-V2 的产品规格书将主处理器标为 **Hi3536E**。这是一份产品级规格，实物完整型号与修订仍需核对芯片丝印、启动日志和固件信息，不能把板卡型号直接当作 SDK 版本。

本工程采用的 `hi3536dv100` 来自 OpenIPC 的参考 SDK/固件平台目录名。OpenIPC/research 的 vdec 发布说明同时列出 HI3536DV100、HI3536EV100；其 EV100 使用笔记也采用 `hi3536dv100` 平台标识。因此目录名包含 D 不代表 E 平台一定不可用，也不能反向证明两者的任意内核、库和固件可互换。

资料来源：

- [雄迈产品页](https://www.xiongmaitech.com/product/product-detail/4/252/470)
- [NBD8010S-KL-V2 产品规格书镜像](https://tehno32.ru/sites/default/files/doc/product_xm/NBD8010S-KL-V2.pdf)
- [OpenIPC/research 组件发布说明](https://github.com/OpenIPC/research/releases)
- [OpenIPC 的 Hi3536EV100 使用笔记](https://github.com/OpenIPC/sandbox-fpv/blob/master/en_notes_start_hi3536ev100.md)

## 三类相关开发包

| 开发资料 | 用途 | 本项目的处理 |
|---|---|---|
| Hi3536E 对应的原厂 MPP SDK＋雄迈板级 BSP | 板端硬解、VO/HDMI、驱动、内核和板级初始化 | 向板卡供应方索要匹配硬件修订与原固件版本的包。本次未获取或确认完整原厂包 |
| OpenIPC Hi3536D/E 开发组件 | 参考头文件、媒体库、解码显示例程、固件构建配方及工具链入口 | 工程已经收集相关头文件和媒体库，按此实现参考后端；仍须上板核对 ABI |
| 雄迈 NetSDK／设备客户端 SDK | 客户端连接、管理原厂设备 | 面向另一类开发任务，不能替代板端 MPP 完成自定义 RTP→VDEC→VO→HDMI |

雄迈客户端 SDK 的官方入口：[设备客户端 SDK](https://www.xiongmaitech.com/service/down_detail/83/185.html)、[NETSDK for CMS Client](https://www.xiongmaitech.com/en/index.php/service/down_detail/83/185)。

## 当前仓库已经具备什么

工程目录内：

```text
third_party/sdk/hisilicon/include/   MPI、VDEC、VB、VO、HDMI 等头文件
third_party/sdk/hisilicon/lib/       libmpi.so、libhdmi.so 及配套库
src/platform/hisilicon_display.c    地面解码/显示参考后端
cmake/arm-linux-toolchain.cmake     交叉工具链/sysroot 配置入口
```

外层源码集合中：

```text
research/sdk/hi3536dv100/                         参考头文件
research/vdec/                                  解码、显示示例
firmware/general/package/hisilicon-osdrv-hi3536dv100/  驱动/媒体库及安装配方
research/build.sh                               上游工具链下载与编译入口
```

上游构建脚本指定 `toolchain.hisilicon-hi3536dv100`，编译器入口为 `bin/arm-linux-gcc`。它是工具链包的标识，不能仅凭名称把任意系统的 sysroot 与现有媒体库混合使用。[构建脚本](https://github.com/OpenIPC/research/blob/master/build.sh)、[工具链发布入口](https://github.com/OpenIPC/firmware/releases/tag/toolchain)。

这些资料是开发组件集合，**不是已取得完整原厂 SDK/BSP**。工程新增代码的主机测试、头文件语法检查，不等于已完成 ARM 链接或证明实际板卡可用。

## 向供应方索取的清单

可直接使用以下需求说明：

> 需要 NBD8010S-KL-V2、Hi3536E 板端二次开发资料：与该硬件修订和固件版本对应的 MPP SDK、板级 BSP、内核源码及配置、交叉工具链/sysroot，以及 VDEC、VO、HDMI 的示例和编译说明。需要板端开发资料，而不只是设备客户端 NetSDK。

拿到资料后核对 SoC 完整型号、内核与模块版本、libc/浮点 ABI、动态加载器和板上 MPP 运行版本。板上可先查看 `uname -r`、`cat /proc/version` 和启动日志；`/proc/cpuinfo` 未必能区分具体 SoC 修订。MPP 版本可通过匹配库的 `HI_MPI_SYS_GetVersion` 查询。核对后再决定继续使用现有组件，或替换参考 SDK/库并调整后端。

本次没有下载未核实来源的所谓“完整 SDK”，没有刷写板卡，没有验证原厂固件与 OpenIPC 媒体库可直接混用。
