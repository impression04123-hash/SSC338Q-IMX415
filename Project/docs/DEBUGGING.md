# 开发与调试路线

## 先按数据经过的顺序定位

| 现象 | 优先检查 | 工具与证据 |
|---|---|---|
| 网卡不存在 | USB 供电、枚举、模块与内核 | `dmesg -w`、`lsusb -t`、`uname -r`、`modinfo 8812eu` |
| 有网卡但空口无视频 | Monitor、信道、带宽、密钥、stream/link ID | `iw dev`、`iw dev wlan0 info`、WFB stdout/stderr |
| WFB 有输出但应用无包 | 本机 UDP/UNIX 端口配置、进程顺序 | `ss -u -a -n -m`、`/proc/net/unix`、应用 JSON 统计 |
| 收到 RTP 但解码错误 | FU 丢片、时间戳、PT、参数集和码流格式 | `tcpdump`、Wireshark、`ffprobe`、ground 的 lost/malformed/abandoned |
| 编码正常但画面黑 | IMX415 模式、VIF 接口、ISP/曝光 | MI 初始化错误阶段、原固件传感器日志、独立录制码流 |
| HDMI 黑屏 | SoC/SDK、VDEC、VO 时序和 EDID | 后端错误码、720p60 回退、连接普通显示器验证 |
| OSD 不更新 | UART 电平/串口、MSP/DisplayPort、字体 | 串口设备权限、msposd 日志、飞控串口设置 |
| 延迟逐渐变大 | 码率过高、USB/链路积压、缓存深度 | WFB 队列/丢包、`ss -m`、CPU/温度、录像时钟测量 |

板端命令依固件是否携带相关工具而定。`fpvctl check` 检查静态配置；`dry-run` 输出 argv；只有 `run` 才会设置无线接口、读密钥和启动进程。先关闭无线/OSD，单独启动视频应用定位。

## 视频与 RTP

先用可控文件替代摄像头。主机自动测试：

```sh
bash scripts/build_host.sh
ctest --test-dir build/host --output-on-failure
python3 tests/test_pipeline.py build/host build/wfb-host/bin
```

最后一条还把同一批 H.265 经过真正的 WFB 加密/FEC 模拟空口，逐帧校验结果。`air_metadata` 专门检查 marker、序号、90 kHz 时间戳、多 slice、循环重放；不能仅用“文件能播放”证明 RTP 元数据正确。

本机抓包示例（在有对应 UDP 流量的那端执行，端口按配置修改）：

```sh
tcpdump -i lo -s 0 -w /tmp/fpv-video.pcap udp port 5600
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,width,height,r_frame_rate -of default=nw=1 received.h265
ffmpeg -v error -i received.h265 -f framemd5 -
```

Wireshark 将该 UDP 流 Decode As RTP，核对 PT=96、SSRC、序号连续性、时间戳和 marker。包头存在扩展/padding 时以实际 RTP header 长度定位 payload。payload type=96 本身不说明编码格式；本项目约定 H.265、无 DONL。空口原始帧另抓 Monitor 网卡；它和 loopback RTP 是不同观察点。

启动日志会打印实际会话 SSRC。默认每次重启空中端都改变它，地面据此立即清理旧接收状态。固定 `random_ssrc=0` 仅用于可重复抓包；重启空中端时应一起重启地面端，不能把相同SSRC下的大幅序号回退当作正常乱序持续发送。

ground 输出的是完整 NAL，不会把缺失 FU 的半个 NAL 送进解码器；同一帧的其他 NAL 仍可能已到达，因此丢包后可能持续花屏直到下一次 IDR/参数集。不要把 NAL 完整性当作整帧无损保证。先使用较低码率、较短 GOP，再逐步调高。

## UART 与 OSD

空中端 `msposd --master /dev/ttyS2 --baudrate 115200 --osd -r 20` 由监督器按配置启动。设备名、波特率和飞控协议必须实测；串口使用匹配的逻辑电平并共地。开启 OSD 前，先在关闭该服务时核验端口数据；不要让两个进程同时读取同一个串口。

msposd 使用其自带 MSP/DisplayPort 解析和 MI 区域叠加。按 `third_party/msposd/README.md` 部署对应字体 PNG 与资源到其查找路径；不是只复制一个二进制就一定能显示。UART 传来的是飞控协议消息，包含什么姿态/电量字段取决于飞控输出。当前工程没有独立原始 IMU 无线通道，也没有 IMU 与曝光的硬件时间同步。

## 系统、资源与恢复

`fpvctl` 把每个进程的输出写入独立日志，并记录启动、退出和重启。ground/air 的 JSON 统计写 stderr，不混进视频内容。先收集首个失败阶段和错误码，再看后续连锁错误。运行目录、日志目录放入 RAM 文件系统可减少闪存写入。

在开发板具备工具时，用 `strace -ff -tt -e trace=network,ioctl,poll,read,write` 定位 socket、设备 ioctl、串口和阻塞点；需要崩溃定位时用对应工具链保留调试符号，配合 gdbserver 和同版本二进制。SDK 错误码查对应头文件，不能用无意义的空符号绕过 ABI 错误。

分别测试：摄像头服务重启、wfb_tx/rx 退出、接收端重启、无线断开恢复、HDMI 拔插和 UART 中断。恢复后观察是否仍有完整帧、时间戳是否合理、锁/端口是否释放。监督器只清理自己的进程组；它不把无线接口自动恢复成之前的模式。

## 时延和稳定性怎么证明

用高帧率相机把真实 LED/毫秒计时器与显示屏同时拍入画面，逐帧计算 glass-to-glass 延迟。记录相机拍摄帧率、测量分辨率、样本量及中位数/P95/最大值；电脑回环不是无线实飞时延。分阶段记录编码、WFB、解码、显示设置，避免将各段平均值直接相加冒充尾延迟。

连续运行并记录实际 FPS、码率、WFB/FEC/错误统计、CPU/内存、温度、USB reset、服务重启次数以及黑屏持续时间。先验证台架，再做带电机噪声的受控测试。没有实测数据时不承诺固定时延、距离或丢包率。

参考：本地 `third_party/wfb-ng/src/{tx,rx}.cpp` 和 `wifibroadcast.hpp`；`third_party/msposd`；`src/platform`；[RFC 7798](https://www.rfc-editor.org/rfc/rfc7798)；文件 AU 边界分类核对 [FFmpeg HEVC parser](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/hevc/parser.c)。
