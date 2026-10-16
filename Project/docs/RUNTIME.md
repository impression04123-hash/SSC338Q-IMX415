# 运行监督器：从电脑实验到板端服务

`fpvctl` 按配置启动图传程序、记录日志、处理异常退出，并在收到停止信号时清理自己启动的进程。媒体由 `fpv-air`、WFB-ng 和 `fpv-ground` 传输，监督器负责组织它们的运行顺序。

监督器使用 C11/POSIX 接口，面向 Linux 与 BusyBox，始终在前台运行。目标板运行三个 C 程序不需要 Python；Python 仅用于电脑上的部分集成测试。

## 1. 先认识三个命令

以下命令均在工程根目录执行。`scripts/build_host.sh` 的默认构建目录是 `build/host`：

```sh
bash scripts/build_host.sh
build/host/fpvctl check --config configs/air.ini
build/host/fpvctl dry-run --config configs/air.ini
```

| 命令 | 实际作用 | 是否启动程序、修改网卡 |
|---|---|---|
| `check` | 读取 INI，校验键名、类型、范围、角色与后端等约束 | 否 |
| `dry-run` | 完成配置校验，再按执行顺序输出每条命令的参数数组 | 否 |
| `run` | 执行运行前检查，创建日志和锁，配置无线并监督子进程 | 是，取决于配置 |

`check` 通过只说明配置格式和已实现的静态约束通过，尚未检查二进制、密钥、串口或 SDK 是否可用。`dry-run` 可以在没有密钥、网卡和硬件 SDK 的电脑上使用。它不创建目录、不加锁、不读取密钥、不启动子进程，也不更改接口。

预览采用每行一个 JSON 数组的形式，例如硬件空中端的一条命令：

```json
["/usr/bin/wfb_tx","-K","/etc/fpv/drone.key","-p","0","-i","7669206","-u","5602","-k","8","-n","12","-B","20","-M","2","wlan0"]
```

数组第一个元素是程序路径，其余元素分别对应一个参数。路径中的空格、分号、美元符号和引号保留在所属参数内。监督器通过参数数组直接执行程序，不把配置拼接成 Shell 命令。`--dry-run` 也可用作 `dry-run` 命令的别名，位置同样在 `--config` 前。

## 2. 在电脑上完成一次文件传输

先准备主机构建和真实 H.265 样例：

```sh
bash scripts/build_host.sh
bash scripts/generate_sample.sh
```

样例脚本默认生成 `samples/demo.h265`：320×240、30 FPS、150 帧。文件已经存在时，脚本会拒绝覆盖。

在第一个终端启动地面端监督器：

```sh
build/host/fpvctl run --config configs/demo-ground.ini
```

随后在另一个终端、相同工程根目录启动空中端监督器：

```sh
build/host/fpvctl run --config configs/demo-air.ini
```

两个 demo 配置均关闭无线和 OSD，以本机 UDP `127.0.0.1:5600` 直接连接两个应用。空中端读取 `samples/demo.h265`，地面端写入 `received.h265`。配置中的 `bin_dir=build/host` 与默认主机构建目录一致。

空中端文件回放正常结束后，其监督器返回 0，不会重启回放。地面端设置 `idle_exit_ms=3000`，在启动后或最后一个被解包器接受的 RTP 包之后空闲三秒退出，监督器也返回 0。因此应在地面端启动后三秒内启动空中端。收到的文件可用下列命令播放：

```sh
ffplay received.h265
```

两个角色分别使用 `/tmp/fpv-demo/run/fpv-air.lock` 和 `/tmp/fpv-demo/run/fpv-ground.lock`，可以同时运行；相同角色、相同 `run_dir` 的第二个实例会被拒绝。demo 日志位于 `/tmp/fpv-demo/log`。

文件回放的 AU 边界、时间戳与层限制由媒体实现和对应测试负责；监督器只根据应用退出状态决定完成或重启。

## 3. 四个示例配置分别用于什么

下表描述工程提供的配置文件，并非公共配置结构中所有字段的默认值。

| 配置 | 角色和视频后端 | 本机媒体端口 | 无线 / OSD |
|---|---|---|---|
| `configs/air.ini` | air，`sigmastar`，IMX415，1920×1080 / 30 FPS | UDP 5602，交给本机 `wfb_tx` | 开 / 开 |
| `configs/ground.ini` | ground，`hisilicon`，1920×1080 解码，HDMI `1080p60` | UDP 5600，接收本机 `wfb_rx` 输出 | 开 / 关 |
| `configs/demo-air.ini` | air，`file`，320×240 / 30 FPS | 直接向 UDP 5600 发送 | 关 / 关 |
| `configs/demo-ground.ini` | ground，`file`，输出 `received.h265` | 接收 UDP 5600 | 关 / 关 |

硬件示例使用 `wlan0`、`8812eu` 模块、信道 161、20 MHz、MCS 2、FEC 8/12、`link_id=7669206`、`radio_port=0`。这些是需要按实物、驱动和使用地区调整的示例参数。空中端 `sensor_mode=0` 的可用分辨率和帧率须由板端传感器驱动确认；地面端参考 SDK 是 Hi3536DV100，仍须核对实际板卡及 ABI。

空中端 OSD 示例使用 `/dev/ttyS2`、115200 波特率和 20 FPS。启用后会启动独立的 `msposd`；串口配置、飞控 MSP/DisplayPort 设置和字体等资源需要提前准备。监督器当前只支持 air 角色启用 OSD。

监督器还会把视频尺寸明确传给 OSD，例如 `--size 1920x1080`，对应上游 `--size/-z` 使用小写 `x` 分隔宽高的格式。这样 OSD 使用本工程的视频配置，不依赖旧 `majestic.yaml` 中的尺寸。上游 `set_resolution` 超出范围时会回退尺寸；本工程在启用 OSD 时提前拒绝宽度不在 1280～3840、高度不在 720～2160 的配置，避免发生静默回退。OSD 刷新率允许 1～50 FPS。

启用 OSD 时，串口波特率也必须属于所附 msposd 支持的集合：9600、19200、38400、57600、115200、230400、460800、500000、921600 或 1500000。视频 FPS 和 OSD 刷新率是独立配置；示例分别为 30 和 20。

硬件示例的日志目录为 `/var/log/fpv`，锁目录为 `/run/fpv`，程序目录为 `/usr/bin`。demo 使用 `/tmp` 下的目录，可由普通用户运行。

## 4. `run` 在启动前检查什么

实际运行时，监督器会依次检查：

- 计划使用的每个程序是否可执行。
- air 的文件后端输入是否是可读的普通文件。
- 启用无线时，当前身份是否为 root，密钥是否为可读的 64 字节普通文件。
- 无线 UDP 交接时，`host` 与 `bind_host` 是否都为 `127.0.0.1`。
- air 启用 OSD 时，串口路径是否为可读写的字符设备。

这些检查通过后，才会创建运行目录、取得角色锁、创建日志目录并启动相关命令。配置合法不代表传感器、编码器、解码器或 HDMI 已完成初始化；SDK 初始化错误仍由应用报告，并进入受限重启流程。

无线网卡可能在 `modprobe` 后才出现，因此监督器根据实际初始化命令的退出状态判断接口配置是否成功。模块、`ip` 或 `iw` 的初始化失败会终止本次启动，不会反复执行整组网卡初始化命令。对应错误可在日志中查看。

## 5. 无线初始化和进程启动顺序

启用无线时，监督器按下列顺序执行独立命令：

1. `modprobe DRIVER`
2. `ip link set dev IFACE down`
3. `iw dev IFACE set type monitor`
4. `iw dev IFACE set channel CHANNEL HT20`
5. 当 `txpower_mbm>0` 时执行 `iw dev IFACE set txpower fixed MBM`
6. `ip link set dev IFACE up`

目前支持 20 MHz 和 40 MHz；40 MHz 使用 `HT40+`。`txpower_mbm` 为 0 或 -1 时保留驱动当前功率设置。每条初始化命令的超时时间为十秒。监督器退出后保留已经配置好的接口状态，不自动恢复之前的模式和信道。

空中端的服务顺序为：

```text
wfb_tx → 等待本机输入端口/套接字就绪 → fpv-air → 等待视频初始化就绪 → 可选 msposd
```

OSD 的参数形式为 `msposd --master DEVICE --baudrate BAUD --size WIDTHxHEIGHT --osd -r FPS`，其中宽高来自 `video.width` 与 `video.height`。

启用 OSD 时，监督器等待 `fpv-air` 输出完整、独立的一行 `FPV_READY`，之后才启动 `msposd`。硬件后端在采集初始化成功、VPE 已创建后输出此行，文件后端在打开输入并读到有效的初始视频单元后输出。这样 OSD 建立 region 时对应的视频处理对象已经存在。仅启动进程、日志中包含同名片段或尚未换行的内容，都不会触发 OSD；就绪行仍照常保存在视频日志里。

地面端的服务顺序为：

```text
fpv-ground → 等待本机接收端口/套接字就绪 → wfb_rx
```

先启动数据接收方，可以减少程序刚启动时向尚不存在的本机端点发送数据的问题。无线关闭时直接启动本角色应用；若 air 的 OSD 开启，同样等待视频初始化就绪后启动 `msposd`。

每次等待端点或视频初始化就绪最多三秒。无线模式启动前发现本机交接端点已被占用，会拒绝启动；启动过程中尚未就绪或相关服务提前异常退出，会按照该服务的剩余重启预算处理。文件应用在等待期间正常结束，仍视为实验完成并返回 0。

端点就绪仅表示本机 UDP 端口已绑定，或 abstract Unix 数据报套接字可以连接。它不能证明传感器已经出图、无线链路已经接通、HDMI 已有画面或 MSP 数据已经到达。地面应用会先绑定套接字，再初始化显示后端，因此尤其不能把套接字就绪当作 HDMI 就绪。

视频初始化就绪也不代表第一帧已经送达接收端；它只为 OSD 提供初始化先后顺序。

## 6. 分清本机 UDP 端口与无线流编号

| 配置或 WFB 参数 | 含义 | 硬件示例 |
|---|---|---|
| `transport.port` / WFB `-u` | TX 的本机 UDP 输入端口，或 RX 的本机 UDP 输出端口 | air 5602，ground 5600 |
| `wireless.radio_port` / WFB `-p` | 无线协议中的流编号 | 两端均为 0 |
| `wireless.link_id` / WFB `-i` | 24 位链路标识 | 两端均为 7669206 |
| WFB TX `-k/-n` | FEC 数据片数 / 总片数 | 8 / 12 |
| WFB TX `-B/-M` | 发射带宽 / MCS | 20 / 2 |

两端本机 UDP 端口可以不同，配对的无线流编号和链路标识需要一致。空中端把 RTP 送到本机 5602，WFB 通过无线传输后，地面端从本机 5600 接收恢复的 RTP。

`transport.random_ssrc` 默认开启，每次空中端进程启动使用新的 RTP SSRC，让接收端把重启后的序号和时间戳识别为新会话。诊断时可设置 `random_ssrc=0` 使用固定的 `transport.ssrc`；此时空中端重启后也需同步重启接收端，避免旧会话状态丢弃回退的包序号。

密钥不包含在源码中。使用 `wfb_keygen` 生成同一对 `drone.key` 与 `gs.key`，分别安装到空中端 `/etc/fpv/drone.key` 和地面端 `/etc/fpv/gs.key`。两个文件内容不同，必须来自同一次配对生成；监督器只验证文件可读和长度，不验证配对关系。

也可将本机媒体交接改为 Linux abstract Unix 数据报套接字：

```ini
[transport]
type=unix
unix_name=fpv-video
```

WFB 会使用 `-U fpv-video`，不再使用 `-u`。配置名称不带开头的 `@` 或 NUL，程序自行补上 abstract 命名空间的 NUL；名称最长 106 字节，以兼容所附 WFB-ng。它不会在磁盘上生成套接字文件。每块设备上，应用和 WFB 使用相同本机名称；空中端与地面端之间可使用不同名称。

## 7. 退出、重启与进程归属

每个子进程都有独立进程组。向前台监督器发送 SIGTERM/SIGINT，或在终端按 Ctrl+C，会按启动的逆序清理它拥有的进程组。每组先收到 SIGTERM，约 200 ms 后仍存在的成员会收到 SIGKILL。监督器在发送清理信号后才回收组长 PID，以降低 PID 被复用时误操作其他进程的风险。

清理不按全系统进程名匹配，不使用 `killall`。子程序应保持前台模式；自行脱离原进程组、另建会话的程序不在这套所有权约定内。

`runtime.restart_limit` 表示每个服务在首次启动之外最多还能重启多少次。例如 `restart_limit=3`，最多启动四次。计数不会因运行一段时间后自动清零。初始化用的 `modprobe/ip/iw` 命令失败直接终止启动；它们不使用服务重启预算。

服务失败后，重启等待时间为：

```text
restart_delay_ms × min(已使用的重启次数, 10)
```

因此 `restart_delay_ms=1000` 时，首次重启约等待一秒，第二次约等待两秒，最多增长到十秒。等待过程中仍响应停止信号。通常只重启退出的服务，不重新配置整块网卡；预算耗尽时停止监督器拥有的全部服务。

air 开启 OSD 时，`msposd` 依赖当前视频实例。`fpv-air` 异常退出后，监督器先停止并清理旧 `msposd`，再启动新视频实例，等待新的 `FPV_READY` 后重建 OSD。先完成旧 region 的清理，可以避免旧 OSD 的退出动作破坏新视频实例中的 region。由视频重启引起的 OSD 重建不消耗 OSD 自己的失败预算；OSD 自身异常退出仍按原有预算计数，且此前已用次数不会因视频重启而清零。

| 情况 | 监督器行为 / 返回码 |
|---|---|
| 文件后端的 air/ground 正常返回 0 | 视为实验完成，清理其余子进程并返回 0 |
| 服务非零退出、被信号终止，或持续服务意外返回 0 | 按剩余预算重启；耗尽后返回 1 |
| 运行前检查、初始化、锁或日志等运行错误 | 失败退出，通常返回 1 |
| 监督器收到 SIGTERM/SIGINT | 清理拥有的子进程，返回 0 |
| 命令用法或配置错误 | 返回 2 |

## 8. 单实例锁与日志

监督器在整个运行期间持有 `run_dir/fpv-ROLE.lock` 的 Linux `flock` 锁。锁文件中记录的 PID 方便诊断；锁的有效性由内核决定。进程正常退出或崩溃后，内核会释放锁。

锁文件不会随退出删除，避免并发实例因锁文件被替换而锁住不同的文件。看到文件还在不代表服务仍在运行。停止服务时应向服务管理器记录的前台进程发送信号，不能仅凭可能过期的 PID 文件执行停止操作。

每个程序的标准输出和标准错误合并到自己的文件：

```text
log_dir/ROLE-PROCESS.log
```

例如 air 硬件配置会生成 `air-wfb_tx.log`、`air-fpv-air.log`、`air-msposd.log`，网卡初始化信息写入 `air-modprobe.log`、`air-ip.log` 和 `air-iw.log`。

日志按约 1 MiB 的大小轮转：当前文件达到下一次写入会超出限制时，改名为 `.log.1`，覆盖旧备份，再创建当前文件。正常由监督器生成的每个角色/程序名最多保留一个当前文件和一个备份，各不超过 1 MiB；同名的多条初始化命令共用相应日志名称。监督器读取子进程管道、写入日志，写入错误会报告并进入失败处理。

监督器自身的启动、退出和重启消息写到它的标准错误，交给终端或服务管理器保存。板端长期运行时可把 `log_dir` 放在易失性文件系统，减少闪存写入。

## 9. 程序路径与 BusyBox 接入

`runtime.bin_dir` 会作为所有程序名称的前缀，包括 `modprobe`、`ip` 和 `iw`。硬件示例设置 `/usr/bin`，因此这些工具也必须能在 `/usr/bin` 找到；若固件把它们放在 `/sbin` 或 `/usr/sbin`，可建立正确链接，或设置空的 `bin_dir=`，由 PATH 查找全部程序。测试正是利用独立的 `bin_dir` 放置假工具，不会调用真实网卡初始化程序。

相对的配置、媒体、程序、日志和运行目录均相对于当前工作目录，而不是 INI 所在目录。电脑 demo 要从工程根目录执行；板端部署建议统一使用绝对路径。

`scripts/init.d/S95fpv` 是前台入口，使用 `exec` 把自身替换为监督器。`run` 与 `start` 行为一致；它也接受 `check` 和 `dry-run`。默认程序为 `/usr/bin/fpvctl`、默认配置为 `/etc/fpv/air.ini`，可用第二个位置参数指定配置，也可通过 `FPVCTL` 环境变量指定监督器路径。

安装时保留脚本可执行权限。BusyBox `inittab` 示例：

```text
::respawn:/etc/init.d/S95fpv run /etc/fpv/air.ini
```

脚本没有后台化、`stop` 或 `restart` 分支，需要由记录前台 PID 的服务管理器负责停止和重启。`respawn` 管理器可能在监督器预算耗尽后再次拉起它；若要求限制整套服务的累计重启次数，还需配置服务管理器自己的频率限制。

## 10. 主机验证命令

默认主机构建会运行 CTest，也可单独重跑：

```sh
ctest --test-dir build/host --output-on-failure
python3 tests/runtime/test_runtime.py build/host/fpvctl
python3 tests/test_air_metadata.py build/host/fpv-air
```

运行监督测试使用临时目录和真实的受控子进程，覆盖参数完整性、特殊字符路径、WFB `-u/-p/-U`、ground 先于 RX 启动、OSD 预览与实际启动的 `--size` 参数、完整就绪行门控（含分段读取、超长日志和伪就绪文本）、视频重启时先清理旧 OSD 再等待新就绪、OSD 独立失败预算、重启上限、SIGTERM 进程组清理、无关进程存活、角色锁及文件正常结束。

其中一个“WFB 在端点就绪前崩溃”的测试需要 root 通过无线运行前检查；普通用户运行时会跳过它。若需要单独覆盖这一项，可在 Linux/WSL 中执行：

```sh
sudo python3 tests/runtime/test_runtime.py build/host/fpvctl
```

该测试仍使用临时目录中的假 `modprobe/ip/iw/wfb_tx`，不会加载真实模块或配置真实无线接口。测试通过证明已覆盖相应的主机行为；实际传感器、无线射频、UART OSD 和 HDMI 效果仍需上板验证。
