# SSC338Q IMX415 无人机数字图传学习手册

从视频采集到 HDMI 显示的源码阅读与分阶段实验

版本 1.0　整理日期 2026 年 10 月 16 日

这套项目要解决的是：让摄像头采集的画面经过硬件编码、无线传输、地面硬件解码，最终出现在显示器或支持 HDMI 输入的 FPV 头显中。学习时先把每一段的输入、输出、配置和观测方法弄清楚，再把它们串起来。

手册依据你已经下载并上传的源码快照编写。它解释现有实现并给出建议实验；没有把参考代码、计划功能或尚未测量的指标写成已经完成的开发成果。

**本地源码根目录**　`E:\Users\Color\Good study\Linux\OpenIPC`

**项目仓库**　https://github.com/impression04123-hash/SSC338Q-IMX415

**阅读约定**　下文代码路径均相对源码根目录。命令分别在 Linux 开发机、空中板或地面板执行，均注明环境。变量和端口示例用于解释接入关系，实际运行需要与所用固件、设备名和密钥对应。

**建议阅读顺序**　先读第 1 至 3 章建立概念，再读第 4 至 10 章对照源码，最后按第 11 至 16 章完成实验和测试。第 17 至 18 章用于查漏补缺和查找入口。

```text
空中端 IMX415 -> SSC338Q 采集编码 -> 本机接入 -> wfb_tx -> RTL8812EU
                                                        |
                                                原始无线帧
                                                        |
地面端 HDMI <- VO <- VDEC <- 本机 UDP <- wfb_rx <- RTL8812EU
```


<!-- page -->

## 01 项目总览

### 你要实现的结果

空中端使用 SSC338Q 和 IMX415 采集并编码视频，RTL8812EU 负责无线收发。地面端使用 NBD8010S-KL-V2 接收，参考海思平台解码程序完成硬解码和显示，通过 HDMI 连接显示器或具有 HDMI 输入的 FPV 头显。

整条视频通路可以分成六段，每一段都应能独立检查。

| 环节 | 主要输入 | 主要输出 | 首先观察什么 |
|---|---|---|---|
| 摄像头输入 | IMX415 像素数据 | 芯片内部图像流 | 是否有帧以及尺寸格式 |
| 图像处理与编码 | 原始图像 | H.265 码流 | 帧率码率和编码返回码 |
| 本机交接 | RTP 视频包 | WFB-ng 输入数据报 | 本地 socket 是否收到数据 |
| 无线链路 | 待发送数据报 | 另一端恢复的数据报 | RSSI FEC 丢包和解密错误 |
| 地面解码 | 重组后的 H.265 NALU | 解码图像 | 送流结果与解码帧计数 |
| 显示输出 | 解码图像 | HDMI 画面 | 输出模式与显示是否连续 |

### 视频链路和飞控链路

飞行姿态控制由飞控负责。飞控读取 IMU，运行控制算法并控制电机；SSC338Q 主要处理图像和视频。UART 可以把飞控的遥测或 OSD 数据交给图传端，但这不意味着视频芯片承担了飞行闭环控制。

戴头显观看视频，也不自动意味着系统具有电子防抖。图像电子防抖、IMU 时间同步以及头部追踪控制，都是需要明确需求后单独实现的功能。

### 第一轮学习目标

- 能画出数据从摄像头到屏幕的流向。
- 能分清本机 UDP、Unix socket 和空中原始无线帧。
- 能指出一个配置来自哪个文件，并知道如何验证它是否生效。
- 遇到黑屏或卡顿时，能先判断问题属于哪一段。

阅读依据　[S01] [S02] [S03] [S04]

## 02 硬件平台和源码范围

### 当前约定的硬件

| 部件 | 当前选用 | 本手册中的职责 |
|---|---|---|
| 空中视频主控 | SSC338Q | 采集 图像处理 硬编码 OSD |
| 图像传感器 | IMX415 | 产生图像数据 |
| 无线模块 | RTL8812EU | 由 USB 接入 并通过驱动收发无线帧 |
| 地面接收板 | NBD8010S-KL-V2 | 接收 解码 HDMI 输出 |
| 显示终端 | 支持 HDMI 输入的显示器或头显 | 显示最终画面 |
| 飞控 | 另行选用 | IMU 采样 姿态控制 遥测和 OSD 数据生成 |

地面板型号按你提供的信息记录。`research/vdec/Makefile` 指向 `sdk/hi3536dv100`，所以本手册讲解的是 Hi3536DV100 平台参考实现。实物板的 SoC、Flash、启动参数和 SDK 版本，仍应通过实物信息及启动日志确认；板卡商品型号与可用固件不能只凭名称互相替代。

### 八个仓库分别怎么用

`research` 用于读懂采集编码和地面硬解显示；`wfb-ng` 用于读懂无线协议；`msposd` 用于读懂 UART 和 OSD。先从这三个仓库入手。

`rtl88x2eu-20230815` 是当前固件配方实际引用的驱动；`rtl8812eu-wfb` 是 WFB-ng 上游推荐的另一个驱动分支，可用于比较。不要把两套驱动同时加载到同一设备上。

`firmware` 负责软件包、芯片库与系统配置的集成；`builder` 提供设备构建配置和覆盖文件。`divinus` 是独立开源推流器，适合进一步理解媒体软件结构，不等同于已经适配到你板上的替代程序。

### 已知边界

Majestic 主程序在固件配方中标注为 `PROPRIETARY`，并以预编译包获取；公开工程不包含它的主体完整源码。芯片 SDK 中也有预编译驱动与动态库。`research/star/star.c` 是可阅读的采集编码参考，不是 Majestic 的完整实现。

本次整理已验证源码下载完整性；尚未在目标硬件上确认整套固件可启动、图传可用或达到某项性能。所有实验结果应由后续实测填写。

### 构建环境要匹配什么

Windows 上的目录可以用于保存和阅读源码。编译通常在 Linux 或合适的 WSL 环境中进行：应用程序使用目标 ARM 架构的交叉编译器，配套目标板的头文件、动态库和 ABI；无线驱动还需要目标内核的源码、配置和符号信息。开发机自身的编译器或内核头文件不能自动替代这些依赖。

阅读依据　[S01] [S02] [S05] [S06] [S07]

## 03 先分清数据格式和传输层

### 一张画面会经历什么

传感器先产生像素数据，编码器把它压缩为 H.265 码流。RTP 为媒体数据添加序号等信息；较大的 NALU 可能分成多个 RTP 包。WFB-ng 接收这些数据报，再进行无线链路所需的处理。地面端逆向恢复并交给解码器。

| 名称 | 它解决的问题 | 在当前方案中的位置 |
|---|---|---|
| H.265 | 如何压缩图像 | 编码器到解码器之间的内容格式 |
| NALU | 如何组织压缩码流中的数据单元 | 参数集 图像片段等 |
| RTP | 如何对媒体数据分包并携带序号等信息 | 编码输出与解码接收之间 |
| UDP | 如何在 IP 网络中发送数据报 | 可以用于本机或有线交接 |
| Unix socket | 同一台 Linux 内的进程通信 | 当前空中端默认接入 WFB-ng |
| WFB-ng | 无线帧收发 FEC 加密和链路管理 | 空中无线段 |
| RTL8812EU 驱动 | 如何控制无线硬件 | WFB-ng 与 USB 无线模块之间 |
| HDMI | 如何把解码图像交给显示终端 | 地面输出段 |

### 为什么不能把整条链路都叫 UDP

本机程序之间可以使用 UDP，而 WFB-ng 的直接无线链路通过 Monitor 模式收发原始 802.11 帧。它不需要先建立普通 Wi-Fi 热点关联，也不要求先为这个视频无线段分配 IP。项目也支持独立的 IP 隧道，但它不是视频广播必须依赖的前提。

当前固件空中端使用 `-U rtp_local`，输入是本地 Unix 数据报 socket；地面 `wfb_rx` 可以把恢复的内容通过 UDP 送往本机解码程序。因此两端本机交接方式可以不同。

### RTP 和 RTSP

RTP 负责媒体数据的组织与传送；RTSP 主要用于媒体会话控制。当前主视频路径由编码输出直接对接 WFB-ng，地面本地 UDP 接入硬解码程序，不以 RTSP 建立会话为必要步骤。

阅读依据　[S01] [S08] [S09] [S10]

## 04 空中端采集处理和硬件编码

### MI 接口和内部模块

MI 是 SigmaStar SDK 提供的多媒体软件接口。你的 C 程序通过 `MI_` 开头的函数调用厂商库与驱动，配置芯片的视频硬件。MIPI 是摄像头数据连接接口，两者处于不同层面。

| 模块 | 学习时应理解的职责 | 参考函数 |
|---|---|---|
| SNR | 选择传感器输出模式并启用 | `MI_SNR_SetRes` |
| VIF | 接收摄像头输入并配置输入端口 | `MI_VIF_SetDevAttr` |
| VPE | 组织图像处理通道 配置输出尺寸格式 | `MI_VPE_SetPortMode` |
| ISP | 成像和画质相关参数控制 | `MI_ISP_API_CmdLoadBinFile` |
| VENC | 将图像编码为 H.265 | `MI_VENC_CreateChn` |
| SYS | 系统初始化和模块间通路绑定 | `MI_SYS_BindChnPort2` |

曝光、白平衡、降噪等成像处理涉及 ISP 与对应调参。不同芯片系列对 ISP、VPE 或缩放模块的划分有差异，不能把一种系列的函数签名直接套到另一种系列。

### 参考程序的初始化顺序

`research/star/star.c` 的 `main()` 先初始化系统，然后调用传感器、VIF、VPE 和 VENC 初始化函数。对当前 Infinity6e 参考分支，主要绑定关系是 VIF 到 VPE，再由 VPE 到 VENC。

```text
MI_SYS_Init
  -> mi_snr_init
  -> mi_vif_init
  -> mi_vpe_init
  -> mi_venc_init
  -> mi_venc_config
  -> 循环 read_frame
```

`read_frame()` 等待编码器可读，查询输出包数，调用 `MI_VENC_GetStream()` 获取码流，处理后调用 `MI_VENC_ReleaseStream()` 归还资源。模块绑定让 SDK 组织内部图像流转，应用主要读取编码后的数据。

先观察传感器实际模式、各级输出尺寸、编码器是否持续产出，以及所有接口返回码。不要只凭网络端没有画面就判断无线故障。

阅读依据　[S02] [S11]

## 05 码流分包和 Socket 调用

### 从编码数据到 RTP

`read_frame()` 把编码器输出交给 `create_fragment()`，后者根据大小决定是否分片，再调用 `create_stream()` 发送。参考代码包含 H.265 FU 类型 49 的处理，地面端有对应的分片重组逻辑。

压缩后的一个 NALU 可以由多个数据包承载；一个解码图像也可能涉及多个 NALU。抓到一个 UDP 包不等于收到了完整的一帧画面。

### sendto 和 sendmsg 到底是什么

两者都是发送数据的系统调用。是否使用 UDP，要看 socket 的地址族、类型和协议，而不是只看函数名字。

| 创建方式 | 当前代码中的用途 | 实际发送调用 |
|---|---|---|
| `AF_INET`<br>`SOCK_DGRAM` | `star.c` 发送 RTP 数据报 | `sendmsg` |
| `AF_UNIX`<br>`SOCK_DGRAM` | 空中编码程序交接给 WFB-ng | 本地数据报通信 |
| `PF_PACKET`<br>`SOCK_RAW` | WFB-ng 注入原始无线帧 | `sendmsg` |
| `AF_INET`<br>`SOCK_DGRAM` | `wfb_rx` 向本机解码器转发 | `sendto` |

`create_stream()` 把 RTP 头和媒体负载放入两个 `iovec`，再交给 `sendmsg()`。它能分散组织用户态数据，不代表整条链路已实现零拷贝。

### 地址和监听

地面端的 `127.0.0.1:5600` 表示地面板自己。解码器通过 `bind()` 监听该本地 UDP 端口，`wfb_rx` 使用 `sendto()` 投递视频包。无线两端的匹配则由无线参数、逻辑通道与配套密钥完成。

参考程序在 `create_stream()` 中把 RTP 时间戳设置为 0。它可以帮助学习序号和分包，但不能据此直接获得可靠的 RTP 时间戳抖动统计；做媒体时序分析或同步时应补齐正确时间戳设计。

阅读依据　[S02] [S09] [S10] [S12] [S13]

## 06 RTL8812EU 驱动和接口初始化

### 按三个阶段确认设备状态

先看 USB 是否识别设备，再看驱动是否加载并接管设备，最后看无线网络接口是否出现。三者并不等价。接口名称可能是 `wlan0`，也可能由系统另行命名。

```sh
# 在对应的空中板或地面板执行 仅用于观察
uname -a
lsusb
dmesg | tail -n 80
lsmod
ifconfig -a
iw dev
```

这些工具在精简固件中可能不齐全，可先使用固件已有的等价命令。记录输出时要同时保留系统版本、设备角色和测试时间。

| 观察结果 | 下一步检查 |
|---|---|
| USB 列表没有设备 | 供电 接线 USB 主机端和模块本身 |
| USB 有设备但没有无线接口 | USB ID 驱动绑定 初始化日志 |
| 模块提示格式不兼容 | 目标架构 内核版本 配置及编译工具链 |
| 接口出现但不能切换模式 | 实际加载驱动及接口占用情况 |
| 工作一段时间后 USB 重置 | 供电 USB 日志 温度和传输错误 |

### Monitor 和 Injection

Monitor 是无线接口的工作模式，允许接收原始无线帧。Injection 是构造并发送无线帧的能力，应通过真实收发和统计验证。成功切换 Monitor 并不能独立证明发送注入已正常。

`wifibroadcast` 脚本根据 USB ID 识别驱动，并执行驱动加载、接口启用、监听模式和无线参数设置。你已有驱动的固件，首先做集成验证和运行配置；只有确认需要修改驱动实现时，再进入驱动源码适配。

无线功率参数的数值不等同于经过测量的实际发射功率。实验先使用适合模块的保守设置和可靠散热供电，不用提高参数掩盖接口或协议问题。

阅读依据　[S01] [S14] [S15]

## 07 WFB ng 发送端处理流程

### 输入入口

`tx.cpp` 的 `main()` 解析参数。指定 `-U` 时进入 `local_loop_unix()`，打开本地 Unix 数据报 socket；使用 `-u` 时进入 `local_loop_udp()`，监听本地 UDP 输入。随后进入 `data_source()`，等待并读取数据报。

编码程序交给 WFB-ng 的应是已经完成媒体分包的数据，而不是要求 WFB-ng 解释 H.265 图像内容。输入包大小也要符合所选构建的负载上限。

### 会话信息

`Transmitter::init_session()` 建立 FEC 状态并生成随机会话密钥，把逻辑通道标识、FEC 类型和 k/n 等信息组织到加密会话包中。`send_session_key()` 重复公告该信息，使接收端能够取得处理后续数据所需的状态。

预先分发的 `drone.key` 和 `gs.key` 用于保护会话信息；传输中的媒体片段再使用会话密钥保护。这个过程不要求接收端先发起 TCP 建连。

### 视频包发送

```text
data_source 读取本机数据报
  -> Transmitter::send_packet 组织数据块
  -> 满足条件时生成 FEC 冗余片段
  -> send_block_fragment 加密待发送片段
  -> RawSocketTransmitter::inject_packet
  -> Radiotap 头 + 802.11 头 + WFB 数据
  -> 原始套接字 sendmsg
  -> 驱动和 RTL8812EU 发射
```

原始数据片段可随到随发，FEC 冗余还受组块、超时和配置影响；不能笼统地把延迟理解为必须等待完整一组后才发送全部视频。

### 如何理解 FEC

例如 k=8、n=12，表示一组 8 个源片段对应总计 12 个编码片段。在满足该编码恢复条件且收到足够有效片段时，可以恢复组内丢失的内容。这里新增冗余相对源片段是 50%，占总片段的比例约为 33%；实际字节开销还包含头部、加密及分组填充等。

阅读依据　[S12] [S16]

## 08 WFB ng 接收和空地匹配

### 接收端实际做了什么

`Receiver` 使用 libpcap 打开无线接口，设置过滤器以筛选对应逻辑通道。收到无线帧后，提取 WFB 内容并交给 `Aggregator::process_packet()`。

接收会话包时，程序先检查包格式，解密并验证会话内容，再核对 epoch、通道标识及 FEC 类型和参数，建立对应的解密与纠错状态。收到数据包后，先完成校验解密，再整理数据块和片段，必要时用 FEC 恢复缺失片段。

恢复后的有效载荷经 `AggregatorUDPv4::send_to_socket()` 使用 `sendto()` 发往本机解码程序。它输出的是原始输入数据报的内容，不是仍带无线头或加密头的空口包。

### 配置如何对应

| 配置项 | 两端的关系 | 常见理解错误 |
|---|---|---|
| 无线信道和带宽 | 需要匹配接收条件 | 只修改一端 |
| `link_id` | 对应同一条逻辑链路 | 与频率信道混为一谈 |
| `radio_port` 或 `-p` | 同一视频流需要对应 | 当成 UDP 端口 |
| 配套密钥 | 同一批生成并分别部署 | 把不同批密钥混用 |
| 本地 UDP 端口 | 各端程序交接处分别对应 | 要求两台机器所有端口相同 |
| MCS 和 FEC | 主要由发送配置决定并要求接收兼容 | 所有字段都机械地两端设相同 |

逻辑通道由 `channel_id = (link_id << 8) + radio_port` 组合。接收端会从有效会话信息中取得 FEC 参数，不能把 FEC 理解为只靠两端静态配置同时猜测。

### 建立可用链路的判据

驱动和监听状态正常后，要继续观察是否收到有效会话、数据包是否增加、解密错误是否异常、是否有成功恢复的输出。RSSI 有数值只能说明收到过可解析的无线信息，不足以证明视频链路已经端到端可用。

阅读依据　[S13] [S16] [S17]

## 09 地面硬解码和 HDMI 显示

### 解码程序入口

`S98vdec` 读取 `/etc/vdec.conf`，再启动 `vdec`。`research/vdec/main.c` 初始化多媒体系统和输出，创建 VDEC 通道，并将 VDEC 直接绑定到 VO。该参考实现的这条绑定中不经过 VPSS。

```text
读取 codec 输出模式和监听端口
  -> 初始化 VO 和 HDMI
  -> HI_MPI_VDEC_CreateChn
  -> HI_MPI_SYS_Bind 绑定 VDEC 到 VO
  -> HI_MPI_VDEC_StartRecvStream
  -> UDP bind 后循环 recv
  -> decode_frame 解析和重组
  -> HI_MPI_VDEC_SendStream
  -> 硬件解码 VO 输出 HDMI 显示
```

### 码流重组和错误处理

`udp_stream.c` 包含 H.264 FU-A 和 H.265 FU 的重组，依据起始与结束标志拼接，并恢复 Annex B 起始码。返回的数据应理解为待送解码器的 NALU 内容，不应把函数名中的 frame 简化成任何场景下的完整图像帧。

这是简化参考解析器。它没有展示完善的 RTP 序号丢失处理，也不能假设已经覆盖任意 RTP 扩展、聚合打包和所有异常包。后续开发可补充长度检查、缓存边界、序号连续性、丢片丢弃和恢复策略，再验证所用编码器输出是否匹配。

### HDMI 与延迟

程序涉及显示终端能力查询、输出模式配置及热插拔处理。先确认终端支持的分辨率和刷新率，再检查 VO 输出和 HDMI 状态。显示器支持 HDMI 输入，也不保证所有模式都有相同显示延迟。

参考代码设置了预览显示模式及显示缓冲参数。这些是调试入口，不能把某个缓冲数字直接当作端到端延迟结果。HDMI 能力结构中的延迟字段也不等于摄像头到屏幕的实测延迟。

阅读依据　[S03] [S04] [S18] [S19]

## 10 UART 飞控数据和 OSD

### 两条需要分开的工作

飞控内部读取 IMU 并完成姿态控制；图传端接收飞控提供的信息并显示。若只是飞行状态叠加，SSC338Q 不需要接管飞控的高频 IMU 采样和电机控制。

MSP DisplayPort 主要传递字符、位置等 OSD 绘制指令。MSP 遥测还可能提供姿态等字段；`msposd` 当前代码也包含姿态请求处理。两者都不自动等于原始加速度计、陀螺仪样本及其精确时间戳。

### 当前可阅读的代码

```text
飞控 UART 输出
  -> msposd 串口配置与接收
  -> serial_read_cb
  -> MSP 协议解析
  -> DisplayPort 指令处理或对应遥测处理
  -> OSD 更新并叠加到视频画面
```

先看 `msposd.c` 的串口事件处理，再看 `osd/msp/msp.c` 和 `msp_displayport.c`。需要明确串口设备、波特率、飞控侧协议配置、端口复用和刷新率。先验证串口字节和协议，再检查绘制。

### 原始 IMU 和电子防抖属于扩展开发

如果目标是利用 IMU 做电子防抖，需要进一步确定样本内容、坐标系、采样率、曝光时间、视频帧时间戳、相机和 IMU 外参，以及两端时钟的对应关系。仅仅接上一根 UART 或显示人工地平线，并不能证明已完成视频稳定算法。

### 用回放提高可重复性

仓库的 `HowToSimulateInFlightMSPData.txt` 给出 INAV SITL、socat 虚拟串口和录制数据回放思路。可在地面复现同一段 OSD 输入，检查协议状态、异常数据和刷新行为，再对接飞控实机。回放环境和固件版本需要记录。

阅读依据　[S20] [S21] [S22]

## 11 启动脚本和必须对齐的配置

### 两端启动关系

空中端 `wifibroadcast start` 的顺序为：读取配置、加载模块、配置 MTU 和媒体分包设置、配置无线接口；首次运行还会设置视频参数，再停止旧图传进程，启动视频广播、IP 隧道和遥测相关进程。理解主视频路径时，可以先把后两类辅助数据单独观察。

地面端需要启动无线接收与 `vdec`。通常先让地面接收、解码和 HDMI 输出进入就绪状态，再开始空中发送，便于观察第一个有效会话和第一批视频数据。

### 当前快照的实际差异

| 位置 | 当前代码或配置 | 本项目需要核对的内容 |
|---|---|---|
| 空中广播脚本 | `-U rtp_local` | 编码程序必须向对应 Unix socket 交数据 |
| `star.c` 参考程序 | 使用 UDP 和 `sendmsg` | 若采用它 应给 WFB-ng 配置 UDP 输入 |
| `vdec.conf` | `codec=h264` | H.265 方案应改为对应的 h265 配置 |
| 地面参考 defconfig | AU 驱动及旧 WIFIBROADCAST 包 | EU 与新版 WFB-ng 需要进一步适配 |
| WFB-ng 包配方 | 固定提交 ae2122c1fb70 | 运行程序和配置参数应与构建版本对应 |
| Majestic 包配方 | 获取预编译 master 发布包 | 板上实际版本需记录 不能只看配方提交 |

### 查看配置和参数

```sh
# 在对应的目标板执行 用于确认实际配置
cat /etc/wfb.yaml
cat /etc/vdec.conf
ps
```

配置文件的位置和服务名称以实际固件为准。`wfb.yaml` 是当前集成脚本的配置；上游通用 Linux 服务常使用 `wifibroadcast.cfg`，两套配置语法和启动方式不能混用。

`-p` 是无线流编号，`-u` 在发送端指定 UDP 输入端口，在接收端指定 UDP 输出端口。看到同一个选项名时，应先确认自己正在看发送端还是接收端。

阅读依据　[S01] [S05] [S06] [S18] [S19] [S23]

## 12 用一个最小例子理解端口

### 选择便于学习的 UDP 接入方式

本节示例选择“空中编码程序 UDP 输出到本机 WFB-ng，地面 WFB-ng UDP 输出到本机解码器”。这是一个供学习和联调用的接入方案，与当前默认 `-U rtp_local` 路径不同。它们需要选择并对齐，不能直接并行启动占用相同接口或端口的多套服务。

```text
空中板本机
star 或适配后的编码程序 -> 127.0.0.1:5602 -> wfb_tx
                                     |
                           原始无线帧传输
                                     |
地面板本机
wfb_rx -> 127.0.0.1:5600 -> vdec -> HDMI
```

### 启动命令示意

前提是目标板已有对应版本程序和配套密钥，接口已按相同无线信道、兼容带宽及 Monitor 模式配置好。以下命令展示进程间接入关系，不负责完成所有设备初始化。

```sh
# 地面板 指定本机 UDP 输出目标
wfb_rx -K /etc/gs.key -i 7669206 -p 0 \
  -c 127.0.0.1 -u 5600 wlan0

# 空中板 监听编码程序的本地 UDP 输入
wfb_tx -K /etc/drone.key -i 7669206 -p 0 \
  -u 5602 -B 20 -M 2 -k 8 -n 12 wlan0
```

这里的 `7669206`、MCS 和 FEC 参数是学习示例，不能当作对所有环境的最佳设置。接口名与密钥路径必须替换为实际值。

`star` 参考程序的位置参数分别对应码率值、目标 IP 和目标端口；程序按其源码约定换算码率。若使用该程序，需要让它的目标对应 `127.0.0.1:5602`。实际构建、传感器模式和 SDK 库依赖应先完成验证。

密钥由 `wfb_keygen` 一次生成配套的 `drone.key` 和 `gs.key`，再分别部署。重新生成一端而继续使用另一批的对端文件，会导致会话解密失败。

阅读依据　[S02] [S12] [S13] [S17]

## 13 按阶段完成实验

### 实验一 只验证采集编码

在空中端确认传感器模式、视频尺寸和帧率，观察编码器返回码和输出计数。通过现有程序的码流导出功能，或在明确编码数据格式后保存输出，在开发机检查码流是否可识别和解码。

完成判据是持续获得可识别的目标编码数据，并记录真实尺寸、帧率和码率。不要以进程仍在运行作为唯一成功标准。

### 实验二 有线旁路验证解码显示

保留相同媒体打包格式，把 RTP/UDP 直接经有线网络送到地面解码程序。先绕过 WFB-ng，检查 NALU 重组、H.265 解码和 HDMI 显示。发送目标使用地面板有线接口的实际 IP 与监听端口。

完成判据是画面连续、解码送流正常、显示模式正确。若该步骤失败，先解决媒体和显示问题，再进入无线实验。

### 实验三 独立验证无线数据通路

用小规模、带序号且符合包长限制的测试数据，验证本地输入、空口接收、解密和本地输出。测试数据接收程序与视频解码程序分开使用，避免将非视频数据交给 VDEC。

记录有效会话、输入包数、恢复后的输出包数、丢包与解密错误。完成判据是能重复解释一次发送到接收的过程。

### 实验四 接入真实视频

恢复目标视频输入并控制码率，观察无线恢复包数、不可恢复丢包、解码错误和显示冻结是否同时出现。一次只改变一个配置，例如码率、MCS 或 FEC，保留修改前后的日志。

### 实验五 对接 UART 和 OSD

先使用 SITL 或数据回放复现 OSD，再连接飞控。分别验证串口收字节、协议解析和画面更新，确认是否有端口占用和波特率不匹配。

### 每次实验应保存什么

保存源码及固件版本、关键配置、设备连接方式、日志、现象、结论和下一步。对于失败也要保留样本；它们能帮助后续判断某次改动到底解决了什么。

## 14 分层调试和定位方法

### 先找数据在哪一段停止

| 现象 | 优先检查 | 有帮助的证据 |
|---|---|---|
| 空中没有编码输出 | Sensor VIF VPE VENC | 各接口返回码和帧计数 |
| WFB 输入计数不增长 | 本机端口或 Unix socket 对接 | 进程参数和本机抓包 |
| 没有有效会话 | 信道 驱动 链路编号 密钥 | 接收日志和会话解密错误 |
| FEC 恢复增多但视频正常 | 无线质量和冗余余量 | 同时间窗口的恢复与丢包 |
| 丢包并伴随花屏冻结 | 码率容量和分片完整性 | 链路统计 RTP 序号 码流样本 |
| UDP 有数据但解码失败 | 编码格式和重组 | NALU 送流返回码 解码统计 |
| 解码有输出但屏幕黑 | VO HDMI 和显示模式 | 输出配置 热插拔 终端能力 |
| 周期性无线失效 | USB 供电 温度 服务重启 | 内核日志与事件时间对照 |

### 抓包和码流检查

```sh
# 地面板已有 tcpdump 时 在明文的本机交接处抓包
tcpdump -i lo -nn -s 0 -w /tmp/video-rx.pcap udp port 5600

# Linux 开发机 检查已经保存好的裸 H.265 文件
ffprobe -hide_banner -f hevc capture.h265
```

把 pcap 带到开发机用 Wireshark 分析，重点看序号、包到达间隔和丢包位置。空口包带有 WFB 封装与加密，不能假设随意抓到空口数据就能直接解析出 RTP 视频。FFprobe 检查的是正确导出的码流文件，不是将 pcap 文件直接改后缀。

### 更深入的工具

`wfb-cli` 或收发进程统计用于观察链路；`strace` 可辅助判断用户态在等串口还是 socket；`usbmon` 可调查 USB 请求与传输问题；逻辑分析仪检查 UART 电平时序及波特率。是否可用取决于目标内核和固件，重型分析尽量放在开发机完成。

当前集成脚本把多个进程输出重定向到 `/dev/null`。定位问题时应先避免重复启动同一个服务，再让对应进程前台运行或把输出保存到日志文件，这样才能看到会话、发送和解码错误。

阅读依据　[S24] [S25] [S26]

## 15 延迟和稳定性如何验证

### 测量真正的端到端延迟

用高速摄像设备同框拍到真实计时器或 LED 变化，以及显示器上对应的变化。按拍摄帧数差和拍摄帧率换算延迟，记录重复样本及拍摄分辨能力。这包含采集、ISP、编码、排队、无线、解码和显示各段的共同影响。

Ping 反映的网络往返时间不能替代这项测量。即使网络数据很快，编码缓冲或显示器后处理仍可能让画面变慢。

### 排队为什么重要

相同画面若在某一级持续积压，系统可能表现为视频平滑但越来越滞后。应同时观察输入输出帧率、缓冲水位和丢弃策略。60 fps 对应约 16.7 ms 的帧周期，这是时间单位换算，不是本项目已经达到的帧率或延迟。

### 稳定性要看哪些指标

| 指标 | 记录方式 | 解释时需要注意 |
|---|---|---|
| 端到端延迟 | 多个重复样本及中位数等统计 | 记录测量误差和显示终端 |
| 冻结次数和最长冻结 | 视频记录或事件日志 | 说明观察时间与判定规则 |
| 无线丢包和 FEC 恢复 | 同时间窗口的增量 | 区分恢复成功与最终丢失 |
| 恢复时间 | 受控中断后的时间记录 | 与编码参数集及关键帧有关 |
| 解码错误 | VDEC 返回码和帧计数 | 区分输入问题与解码配置 |
| 资源与硬件状态 | CPU 内存 温度 USB 日志 | 配合故障时间分析 |

### 一个可重复的记录方案

先完成固定场景的短时基线，再逐步延长连续运行时间。对同一组配置重复测试，改变码率、MCS 或 FEC 时保留对照条件；环境变化也应记录。没有实测时使用“待测”，不要用源码参数推导出飞行距离、长期稳定性或最终延迟结论。

## 16 学习路线和可交付的开发任务

### 六个学习阶段

| 阶段 | 主要学习内容 | 可留下的结果 |
|---|---|---|
| 一 | Linux 进程 文件 日志和设备 | 设备信息及启动日志 |
| 二 | 视频模块 MI 接口和编码 | 初始化调用图与码流样本 |
| 三 | RTP UDP Unix socket | 端口关系图和本机交接实验 |
| 四 | 无线驱动 WFB-ng 会话与 FEC | 成功收发日志及配置对照 |
| 五 | 解码 VO HDMI UART 和 OSD | 显示与串口回放记录 |
| 六 | 系统定位 延迟和稳定性 | 可复现实验报告与实测表 |

### 适合继续实现的软件任务

为每一级添加清晰的错误日志与输入输出计数；把配置中的接口名、码流格式和端口集中管理；完善 RTP 解析的边界及序号检查；建立码流与 MSP 数据的回放流程；记录 WFB 链路指标并与解码错误按时间关联。

这些任务应在当前板级条件验证后选择实施。只有确实需要调整内核模块、USB 绑定或无线帧处理时，才把相应工作称为驱动修改；仅配置现有驱动时，描述为驱动集成与联调更准确。

### 建议的工程目录

```text
docs/       架构说明 源码笔记 配置约定
configs/    空中端与地面端的配置模板
tools/      状态采集和数据回放工具
patches/    对上游的具体修改及原因
tests/      人工实验步骤与记录模板
results/    实际测试报告与必要样本
```

上游代码保留其来源、版本与声明，自己的代码和补丁单独记录。一次清楚的修复应能说明触发条件、原因、具体改动和验证证据；上传一份源码快照不等于完成这些开发工作。

## 17 复习问题和容易混淆的点

### 空中端和地面端是否必须有 IP

直接 WFB-ng 视频无线段不依赖两端 IP。各板本机可以使用 UDP，另建 IP 隧道或有线管理网络也可以有 IP；先明确正在讨论哪一段。

### 看到 sendto 就说明一定是 UDP 吗

不能这样判断。先看 socket 的地址族和类型。本项目地面 UDP 转发确实调用 `sendto`，空中原始帧发送和 `star.c` 的 UDP 发送都可以调用 `sendmsg`。

### 为什么同一项目既有 Unix socket 又有 UDP

它们都是进程通信选择。本地 Unix socket 不经过 IP 网络；UDP 可以用于本机或跨机器。编码程序与 WFB 输入形式对应即可，地面输出形式可以独立配置。

### 接收端必须静态写相同 FEC 参数吗

当前 WFB-ng 接收代码从验证后的会话包获得相应 FEC 参数并初始化。实际互通仍要求兼容协议与构建，不能忽略版本差异。

### RSSI 很好为什么仍然卡

继续检查干扰与丢包、USB 传输、码率容量、RTP 分片、编码缓存、解码错误和显示缓冲。单个信号强度数值不能覆盖整条链路。

### 有 OSD 人工地平线是否就是防抖

人工地平线是一种姿态可视化；电子防抖需要图像变换、时间关系与标定等处理。显示姿态不能代替稳定画面。

### VDEC VO 和 HDMI 分别是什么

VDEC 把压缩视频解成图像；VO 负责视频输出通路；HDMI 将最终显示信号送到终端。网络收到数据、解码成功和屏幕出画面是三个可分别验证的状态。

### 现在最值得先做什么

先确认实物平台和实际固件版本，再完成可保存码流、可有线显示、可无线收发三个最小实验。每做完一段，都留下配置、样本和能够解释结果的记录。

## 18 源码地图和版本记录

### 首先打开这些文件

| 主题 | 源码路径 | 阅读入口 |
|---|---|---|
| 空中启动 | `firmware/general/package/wifibroadcast-ng/files/wifibroadcast` | start 和 load 系列函数 |
| 无线默认配置 | `firmware/general/package/wifibroadcast-ng/files/wfb.yaml` | wireless 和 broadcast |
| 采集编码 | `research/star/star.c` | main read_frame create_stream |
| 芯片 API | `research/star/include/infinity6e/` | VIF VPE VENC SYS 头文件 |
| WFB 发送 | `wfb-ng/src/tx.cpp` | main data_source Transmitter |
| WFB 接收 | `wfb-ng/src/rx.cpp` | Receiver Aggregator |
| 无线协议定义 | `wfb-ng/src/wifibroadcast.hpp` | 会话头 数据头和参数定义 |
| 密钥生成 | `wfb-ng/src/keygen.c` | 生成和保存配套密钥 |
| 地面主程序 | `research/vdec/main.c` | 初始化 UDP 循环和 VDEC 送流 |
| 地面重组 | `research/vdec/udp_stream.c` | decode_frame |
| 解码启动配置 | `firmware/general/package/vdec-openipc/files/` | S98vdec 和 vdec.conf |
| 飞控串口 | `msposd/msposd.c` | serial_read_cb 和串口配置 |
| OSD 协议 | `msposd/osd/msp/` | MSP 和 DisplayPort 解析 |
| OSD 回放 | `msposd/HowToSimulateInFlightMSPData.txt` | SITL PTY 和数据回放 |

所有固定提交和源 URL 以 `SOURCES.json` 为准。源码快照和当前在线上游可能随时间发生差异，阅读时先核对提交，再讨论行为。

<!-- page -->

### 本次阅读所用版本

| 目录 | 上游提交 |
|---|---|
| `research` | `30e6fa0a546e` |
| `wfb-ng` | `ae2122c1fb70` |
| `msposd` | `ad0175ce01c4` |
| `rtl88x2eu-20230815` | `12977e4013ec` |
| `rtl8812eu-wfb` | `48e6e449e089` |
| `divinus` | `10409f41031e` |
| `builder` | `214075f0c426` |
| `firmware` | `1d9c834e09db` |

本项目仓库的首次整理提交为 `8200fc6bef85`。下列仓库内源码链接固定到该提交，外部手册链接则可能随时间更新。


### 参考资料

- [S01 空中图传启动脚本](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/wifibroadcast-ng/files/wifibroadcast)
- [S02 SigmaStar 采集编码参考](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/research/star/star.c)
- [S03 海思地面解码和 HDMI 主程序](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/research/vdec/main.c)
- [S04 地面 RTP 和 NALU 重组](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/research/vdec/udp_stream.c)
- [S05 Hi3536DV100 地面构建参考配置](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/builder/devices/common/br-ext-chip-hisilicon/configs/hi3536dv100_wfbng_defconfig)
- [S06 WFB ng 固件软件包配方](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/wifibroadcast-ng/wifibroadcast-ng.mk)
- [S07 Majestic 固件软件包配方](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/majestic/majestic.mk)
- [S08 WFB ng 原理和使用说明](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/README.md)
- [S09 Linux UDP 手册](https://man7.org/linux/man-pages/man7/udp.7.html)
- [S10 Linux packet socket 手册](https://man7.org/linux/man-pages/man7/packet.7.html)
- [S11 Infinity6e MI 接口头文件](https://github.com/impression04123-hash/SSC338Q-IMX415/tree/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/research/star/include/infinity6e)
- [S12 WFB ng 发送端实现](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/src/tx.cpp)
- [S13 WFB ng 接收端实现](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/src/rx.cpp)
- [S14 固件引用的 EU 驱动说明](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/rtl88x2eu-20230815/README.md)
- [S15 WFB 作者推荐的 EU 驱动说明](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/rtl8812eu-wfb/README.md)
- [S16 WFB 无线协议和数据头定义](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/src/wifibroadcast.hpp)
- [S17 WFB ng 官方 Setup HOWTO](https://github.com/svpcom/wfb-ng/wiki/Setup-HOWTO)
- [S18 地面解码默认配置](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/vdec-openipc/files/vdec.conf)
- [S19 地面解码启动脚本](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/vdec-openipc/files/S98vdec)
- [S20 MSP OSD 串口与主程序](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/msposd/msposd.c)
- [S21 INAV SITL 和 MSP 数据回放方法](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/msposd/HowToSimulateInFlightMSPData.txt)
- [S22 MSP 与 DisplayPort 协议实现](https://github.com/impression04123-hash/SSC338Q-IMX415/tree/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/msposd/osd/msp)
- [S23 空中无线默认配置](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/wifibroadcast-ng/files/wfb.yaml)
- [S24 WFB 链路统计 CLI 实现](https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/wfb_ng/cli.py)
- [S25 Wireshark RTP 分析文档](https://www.wireshark.org/docs/wsug_html_chunked/ChTelRTP.html)
- [S26 Linux usbmon 调试文档](https://docs.kernel.org/usb/usbmon.html)

[S01]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/wifibroadcast-ng/files/wifibroadcast "空中图传启动脚本"
[S02]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/research/star/star.c "SigmaStar 采集编码参考"
[S03]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/research/vdec/main.c "海思地面解码和 HDMI 主程序"
[S04]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/research/vdec/udp_stream.c "地面 RTP 和 NALU 重组"
[S05]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/builder/devices/common/br-ext-chip-hisilicon/configs/hi3536dv100_wfbng_defconfig "Hi3536DV100 地面构建参考配置"
[S06]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/wifibroadcast-ng/wifibroadcast-ng.mk "WFB ng 固件软件包配方"
[S07]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/majestic/majestic.mk "Majestic 固件软件包配方"
[S08]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/README.md "WFB ng 原理和使用说明"
[S09]: https://man7.org/linux/man-pages/man7/udp.7.html "Linux UDP 手册"
[S10]: https://man7.org/linux/man-pages/man7/packet.7.html "Linux packet socket 手册"
[S11]: https://github.com/impression04123-hash/SSC338Q-IMX415/tree/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/research/star/include/infinity6e "Infinity6e MI 接口头文件"
[S12]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/src/tx.cpp "WFB ng 发送端实现"
[S13]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/src/rx.cpp "WFB ng 接收端实现"
[S14]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/rtl88x2eu-20230815/README.md "固件引用的 EU 驱动说明"
[S15]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/rtl8812eu-wfb/README.md "WFB 作者推荐的 EU 驱动说明"
[S16]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/src/wifibroadcast.hpp "WFB 无线协议和数据头定义"
[S17]: https://github.com/svpcom/wfb-ng/wiki/Setup-HOWTO "WFB ng 官方 Setup HOWTO"
[S18]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/vdec-openipc/files/vdec.conf "地面解码默认配置"
[S19]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/vdec-openipc/files/S98vdec "地面解码启动脚本"
[S20]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/msposd/msposd.c "MSP OSD 串口与主程序"
[S21]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/msposd/HowToSimulateInFlightMSPData.txt "INAV SITL 和 MSP 数据回放方法"
[S22]: https://github.com/impression04123-hash/SSC338Q-IMX415/tree/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/msposd/osd/msp "MSP 与 DisplayPort 协议实现"
[S23]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/firmware/general/package/wifibroadcast-ng/files/wfb.yaml "空中无线默认配置"
[S24]: https://github.com/impression04123-hash/SSC338Q-IMX415/blob/8200fc6bef85f7dcccb7bc854ded2f2c54e9dc16/wfb-ng/wfb_ng/cli.py "WFB 链路统计 CLI 实现"
[S25]: https://www.wireshark.org/docs/wsug_html_chunked/ChTelRTP.html "Wireshark RTP 分析文档"
[S26]: https://docs.kernel.org/usb/usbmon.html "Linux usbmon 调试文档"


<!-- page -->

## 附录 实验记录模板

### 基本信息

实验编号　待填写

测试日期和地点　待填写

目标和完成判据　待填写

空中板 SoC 固件 内核和程序版本　待填写

地面板 SoC 固件 内核和程序版本　待填写

无线模块 驱动版本 信道 带宽 MCS FEC　待填写

编码格式 分辨率 帧率 码率 GOP　待填写

HDMI 输出模式 显示器或头显型号　待填写

### 操作和证据

基线配置和唯一修改项　待填写

执行步骤与命令所在设备　待填写

日志 抓包 码流和视频证据路径　待填写

观察时间 样本数 与异常次数　待填写

### 结果

端到端延迟与测量方法　待测

FEC 恢复 不可恢复丢包 解密错误　待测

解码错误 最长冻结 与恢复时间　待测

CPU 内存 温度 与 USB 异常　待测

### 结论和下一步

本次证据支持的结论　待填写

仍未排除的问题　待填写

下一次只改变的因素　待填写

