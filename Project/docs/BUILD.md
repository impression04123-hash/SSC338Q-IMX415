# 构建与暂存

构建环境为 Linux 或 WSL Ubuntu。目标运行程序不依赖 Python；Python 3 仅用于开发测试和来源快照工具。主机需要 CMake、C11 编译器、GNU Make、pkg-config；WFB 另需 C++11、libsodium、libpcap、libevent 开发包。H.265 回放实验另需 FFmpeg。脚本不会安装包、刷机或修改主机服务。

```sh
cd '/mnt/e/Users/Color/Good study/Linux/OpenIPC/Project'
bash scripts/build_host.sh
bash scripts/build_wfb.sh
python3 scripts/test_wfb_loopback.py build/wfb-host/bin
(cd third_party && sha256sum -c SHA256SUMS)
```

WFB 脚本只调用上游 `all_bin`，避免触发上游 Python 服务测试或自动创建密钥。它在独立副本内编译，输出 `build/wfb-host/bin`。本工程开发验证已在 WSL Ubuntu x86_64 成功编译五个 WFB 程序；上游 radiotap 的 packed-member 和 tx_cmd 的 write 返回值告警仍存在。

## 两端交叉编译

Air 的 SigmaStar 工具链/sysroot 与 Ground 的 Hi3536DV100 工具链/sysroot 必须分开，且 libc、浮点 ABI、CPU 指令集、动态加载器与实际固件一致。下面 `/opt/...` 均为待替换示例路径，不表示这些工具链已安装。SDK 库只是链接输入，不能代替匹配 sysroot。

```sh
cmake -S . -B build/air \
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-linux-toolchain.cmake \
  -DFPV_TOOLCHAIN_PREFIX=/opt/sigmastar/bin/arm-openipc-linux-musleabihf- \
  -DFPV_SYSROOT=/opt/sigmastar/arm-openipc-linux-musleabihf/sysroot \
  -DFPV_ENABLE_SIGMASTAR=ON -DBUILD_TESTING=OFF
cmake --build build/air --parallel 2

cmake -S . -B build/ground \
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-linux-toolchain.cmake \
  -DFPV_TOOLCHAIN_PREFIX=/opt/hisilicon/bin/arm-hisiv300-linux- \
  -DFPV_SYSROOT=/opt/hisilicon/target \
  -DFPV_ENABLE_HISILICON=ON -DBUILD_TESTING=OFF
cmake --build build/ground --parallel 2
```

主 CMake 在定义 `fpv_core` 后 `include(cmake/platforms.cmake)` 并调用 `fpv_add_platforms()`。函数根据选项定义 `fpv_sigmastar` 或 `fpv_hisilicon` 静态目标，携带 SDK include、绝对库路径及 rpath-link；应用链接对应目标。禁止同时开启两个平台。没有目标编译器时，仅完成头文件语法验证不等于成功交叉链接，更不等于上板验证。

WFB 交叉编译示例；分别对 air、ground 使用各自工具链执行，不要混用生成物：

```sh
export CROSS_COMPILE=/opt/sigmastar/bin/arm-openipc-linux-musleabihf-
export FPV_SYSROOT=/opt/sigmastar/arm-openipc-linux-musleabihf/sysroot
export PKG_CONFIG_LIBDIR="$FPV_SYSROOT/usr/lib/pkgconfig:$FPV_SYSROOT/usr/share/pkgconfig"
bash scripts/build_wfb.sh build/wfb-air
export CC="${CROSS_COMPILE}gcc"
bash scripts/build_msposd.sh build/msposd-air
```

目标 sysroot 中必须先具备目标 ABI 的 libsodium、libpcap、libevent_core 和 `.pc`。交叉模式清空 PKG_CONFIG_PATH，限定 PKG_CONFIG_LIBDIR 和 PKG_CONFIG_SYSROOT_DIR，防止回退主机库。使用自定义 CC/CXX 时仍须设置 FPV_SYSROOT；工具链路径/sysroot 请使用无空格路径或无空格符号链接，因为上游 Makefile 的 flags 会经 shell 再解释。msposd 脚本按上游 `star6e` 目标使用它随附的 `sdk/infinity6`（OSD API），DRV 指向 SigmaStar 库副本；与采集 API 的 infinity6e 头文件用途不同。

```sh
KSRC=/opt/kernel-ssc338q ARCH=arm \
  CROSS_COMPILE=/opt/sigmastar/bin/arm-openipc-linux-musleabihf- \
  bash scripts/build_wifi_driver.sh build/wifi-air
```

驱动需要与设备完全对应且完成配置/prepare 的 kernel build tree，不能用开发机内核头文件替代。输出模块位于 build/wifi-air/modules；脚本不加载模块，不执行上游 dkms 安装脚本。

## 不带无线电的真实 WFB 回环

该固定版本的 TX 隐藏选项 `-D` 使用 UDP 模拟无线输出，RX 的 `-a` 接收其聚合格式。已运行 `test_wfb_loopback.py`：随机生成临时 drone/gs 密钥，`wfb_tx -D 15601 -K drone.key -u 15600 -p 0 -k 2 -n 3 -T 20 dummy0` 连接 `wfb_rx -a 15601 -K gs.key -c 127.0.0.1 -u <接收端口> -p 0`，8 个 1017 字节 UDP 载荷逐包一致。经过实际加密/解密和 FEC 编码/解码，未模拟无线衰落或验证无线驱动。测试脚本现在全部使用动态空闲端口，并分别运行完整转发与真实丢包场景。UDP 代理按固定版 17 字节 wrxfwd_t 头后的网络序 data_nonce 解出 block_idx/fragment_idx，每个 2/3 FEC block 丢弃数据 fragment 0，仅转发数据 fragment 1 与冗余 fragment 2；4 个 block 各丢 1 包后，8 个原始 UDP 载荷仍逐字节一致。独立错误 gs.key 接收器收到全部会话和数据流量后保持运行，观察期内没有明文输出。脚本自动清理自己的进程、socket 与临时密钥；无 root 权限需求。上面的固定端口命令仅用于说明上游选项。

## 暂存与部署

```sh
bash scripts/stage_target.sh build/air build/stage-air-new build/wfb-air
# 如已编译 OSD，再显式加入暂存包：
install -Dm755 build/msposd-air/bin/msposd build/stage-air-new/usr/bin/msposd
```

stage_target 拒绝已存在的目的目录，只在新目录通过 DESTDIR 收集主工程安装输出及可选 WFB 程序，不写目标设备。主工程安装目录由顶层 CMake 管理。驱动模块和 SDK 运行库应按实际固件核验后再加入适当目录；不要覆盖已有固件库。独立保存两端密钥并核对配置、设备节点、网卡名称、内核版本，然后自行选择传输方式。没有执行交叉硬件构建、刷写或板端验收。
