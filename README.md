<div align="center">

# Erde

**基于 FFmpeg 的拉取式音视频转码管线 —— 视频、音频、滤镜、多流混流，一条 CMake 就能编。**
**v0.4.0 起原生支持 Rockchip RK3566 / RK3588 硬件编解码（RKMPP）。**

<p>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-2ea44f"></a>
  <a href="https://github.com/MichaelYWNA/erde/actions/workflows/ci.yml"><img alt="CI" src="https://img.shields.io/badge/CI-passing-brightgreen"></a>
  <img alt="Version" src="https://img.shields.io/badge/version-0.4.0-1f6feb">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-17-00599c">
  <img alt="FFmpeg" src="https://img.shields.io/badge/FFmpeg-6.1%2B%20(incl.%208.x)-3c873a">
  <img alt="Platform" src="https://img.shields.io/badge/aarch64-RK3566%20verified-orange">
  <a href="README_EN.md"><img alt="Language" src="https://img.shields.io/badge/language-中文%20%7C%20English-8957e5"></a>
</p>

<p>
  <a href="#3-快速开始">快速开始</a>
  · <a href="#4-构建与依赖">构建</a>
  · <a href="#5-命令参考">命令</a>
  · <a href="#6-硬件加速">硬件加速</a>
  · <a href="#7-在-rockchip-rk3566-上部署">RK3566 部署</a>
  · <a href="#8-实测数据">实测数据</a>
  · <a href="#9-已知限制">已知限制</a>
</p>

</div>

---

`erde` 把 FFmpeg 的解复用、解码、滤镜、缩放、编码、复用拆成可组合的 **stage**，每个 stage 只暴露
一个 `pull()`，由下游主动向上游取数据。它不依赖 ROS、不依赖任何第三方框架，只用 pkg-config 找到
libav\*，一份 `cmake` 就能编出一个命令行工具。

> 设计取舍、踩过的坑和每个数字的来源写在 [docs/DESIGN.md](docs/DESIGN.md)；
> 本文回答"它是什么、能做什么、怎么在 x86 和 Rockchip 板子上跑起来"。

## 目录

- [1. 这个项目解决什么](#1-这个项目解决什么)
- [2. 核心能力](#2-核心能力)
- [3. 快速开始](#3-快速开始)
- [4. 构建与依赖](#4-构建与依赖)
- [5. 命令参考](#5-命令参考)
- [6. 硬件加速](#6-硬件加速)
- [7. 在 Rockchip RK3566 上部署](#7-在-rockchip-rk3566-上部署)
- [8. 实测数据](#8-实测数据)
- [9. 已知限制](#9-已知限制)
- [10. 致谢与许可](#10-致谢与许可)

## 1. 这个项目解决什么

常见的转码脚本是"三段循环互相推"：解码器把帧推进队列，编码器从队列取。代价是每个交接点都要
自己管水位、自己决定队列满了丢谁——而"丢最新一帧"这类 bug 恰好藏在这个决定里，一旦源帧率或
编码器延迟变化，就会静默丢帧/丢样本。

`erde` 反过来：**下游向上游要数据**。

```cpp
Expected<Frame> Decoder::pull();   // 值 / need_more / end_of_stream
```

三态表达"有数据、暂时没有、永远没有"，于是：循环里没有"保留最后一个"的决策点，帧不会在 stage
边界被静默丢掉；解码器的 B 帧重排、编码器的 `EAGAIN`、音频切片器的缓冲，全部封在各自 stage 内部；
插一个滤镜、插一个缩放，只是换掉 `FrameSource*`，别处不用改。

## 2. 核心能力

| 领域 | 已实现 | 说明 |
|---|---|---|
| 视频 | 解复用 / 解码 / 滤镜 / 缩放 / 编码 | `libx264`、`libx265`、`mpeg4`、`mjpeg`… |
| 音频 | 解码 / 滤镜 / 重采样 / 定长切帧 / 编码 | AAC、mp3、opus…（`AudioSlicer` 负责帧长对齐） |
| 滤镜 | `--filter`、`--audio-filter` 接任意 libavfilter 链 | `fps=15`、`crop=…`、`atempo=…`、`volume=…` |
| 混流 | 一个容器写多路流 | `av_interleaved_write_frame` 负责交织 |
| 硬件编码 | `--hw vaapi` / `--hw cuda` | 设备帧上传路径已实现（x86 有 GPU 时可验证） |
| **硬件编码（嵌入式）** | **`--hw rkmpp`** | **RK3566 / RK3588 的 VPU，走 `h264_rkmpp` / `hevc_rkmpp`，已在 LubanCat-1 实测** |
| ROS 2 | `erde_ros` 生命周期节点 | 节点持有 stage 图，发布 `CompressedImage`，带 diagnostics 与 stats 服务 |

## 3. 快速开始

编译好后（见第 4 节），不需要素材就能把整条链路跑一遍：

```bash
# 1) 造一个 2 秒、带音轨的 640x480 测试片
./build/erde gen /tmp/av.mp4 --size 640x480 --fps 30 --frames 60 --crf 23 --with-audio --seconds 2

# 2) 看清楚它里面有什么
./build/erde probe /tmp/av.mp4
#   video(index=0, codec=h264, 640x480, fps=30, tb=1/15360) packets=60
#   audio(index=1, codec=aac, 48000 Hz, tb=1/48000) packets=95

# 3) 两路一起转：视频转 mpeg4 并缩放，音频保持 aac
./build/erde transcode /tmp/av.mp4 /tmp/out.mp4 --codec mpeg4 --size 320x240 --audio-bitrate 96k

# 4) 测吞吐
./build/erde bench /tmp/av.mp4 /tmp/bench.mp4 --codec mpeg4 --size 320x240 --runs 5
```

> 注意：`--codec libx264` 需要 FFmpeg 编译时带 `--enable-libx264`。自己编 `ffmpeg-rockchip` 时
> 若没加这个开关，软件链路请用 `--codec mpeg4` / `mjpeg`，硬件链路用 `--codec libx264 --hw rkmpp`
> （映射到 `h264_rkmpp`，不需要 libx264）。

常见任务对照：

| 想做什么 | 直接这样跑 |
|---|---|
| 换个分辨率 | `erde transcode in.mp4 out.mp4 --size 1280x720` |
| 换编码器 | `erde transcode in.mp4 out.mp4 --codec libx265 --crf 28 --preset ultrafast` |
| 降帧率 | `erde transcode in.mp4 out.mp4 --filter 'fps=15' --fps 15` |
| 裁剪画面 | `erde transcode in.mp4 out.mp4 --filter 'crop=640:360:0:0'` |
| 丢掉音轨 | `erde transcode in.mp4 out.mp4 --no-audio` |
| 音频变速 / 调音量 | `erde transcode-audio in.m4a out.m4a --audio-filter 'atempo=2.0'` |
| 只重采样 | `erde transcode-audio in.m4a out.m4a --rate 44100 --channels 1` |
| 硬件 H.264 编码（RK3566） | `erde transcode in.mp4 out.mp4 --codec libx264 --hw rkmpp --size 1920x1080` |
| 硬件 H.265 编码（RK3566） | `erde transcode in.mp4 out.mp4 --codec libx265 --hw rkmpp --size 1920x1080` |
| 看一眼容器 | `erde probe in.mp4` |
| 测这条链路的吞吐 | `erde bench in.mp4 out.mp4 --runs 10` |

## 4. 构建与依赖

依赖 libav\* 的六个库、CMake ≥ 3.16、支持 C++17 的编译器：

```bash
# Ubuntu / Debian（x86_64）
sudo apt install -y build-essential cmake pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev libavfilter-dev

git clone https://github.com/MichaelYWNA/erde.git
cd erde
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

**FFmpeg 版本要求：≥ 6.1**。代码使用 `AVChannelLayout` / `av_channel_layout_*`（5.1 引入），
因此 Ubuntu 20.04 自带的 **FFmpeg 4.2 无法编译**——这是 RK3566 部署必须自己编 FFmpeg 的根本原因（第 7 节）。
反过来向上是兼容的：本项目在 **FFmpeg 8.1.2**（`ffmpeg-rockchip` 当前 master）上实测编译、测试全过。

> `ctest --test-dir build` 需要 CMake ≥ 3.20；Ubuntu 20.04 自带 3.16，请 `cd build && ctest`。

## 5. 命令参考

```text
erde probe <file>
erde gen <out> [options]
erde transcode <in> <out> [options]
erde transcode-audio <in> <out> [options]
erde bench <in> <out> [--runs N] [options]

--codec <name>      编码器（视频：libx264/libx265/mpeg4；音频：aac/libmp3lame）
--bitrate <rate>    2000k / 2M / 1500000
--preset <name>     x264/x265 preset（rkmpp 下自动忽略）
--crf <n>           质量优先，覆盖 --bitrate（rkmpp 下自动忽略）
--gop <n>           关键帧间隔
--fps <n>           gen 的帧率；transcode 用来校验源帧率
--size <WxH>        插入 scaler
--filter <desc>     libavfilter 链
--audio-filter <d>  音频滤镜链
--no-audio          丢弃音轨
--hw <name>         硬件编码：rkmpp（RK3566/RK3588）、vaapi、cuda
--hw-device <path>  vaapi 设备节点，如 /dev/dri/renderD128
```

## 6. 硬件加速

### 6.1 vaapi / cuda（x86）

`--hw vaapi` 做三件事：`av_hwdevice_ctx_create` 开设备 → 建 `AVHWFramesContext`（NV12 上传池）
→ 每帧 `av_hwframe_transfer_data` 上传后送编码器。`--hw cuda` 走 `h264_nvenc` / `hevc_nvenc`。

### 6.2 rkmpp（Rockchip RK3566 / RK3588）

关键事实：**`rkmpp` 不是一个 `AVHWDeviceType`**，`ffmpeg-rockchip` 提供的是普通编码器
`h264_rkmpp` / `hevc_rkmpp` / `mjpeg_rkmpp`，它们直接吃 **NV12 系统内存帧**，不需要设备上下文。
所以 erde 里为它单开了一条路径：

| 环节 | vaapi / cuda | **rkmpp** |
|---|---|---|
| 编码器选择 | `h264_vaapi` / `h264_nvenc` | `h264_rkmpp` / `hevc_rkmpp` |
| 设备上下文 | 必须建 `AVHWDeviceContext` | **不建**（没有设备句柄） |
| 帧格式 | `AV_PIX_FMT_VAAPI` 硬件表面 | **NV12 系统内存** |
| 上传步骤 | `av_hwframe_transfer_data` | **无**，改为 `sws_scale` 做格式转换 |
| x264 参数 | `preset` / `crf` 照常传 | **自动不下发**（否则 `avcodec_open2` 报未知选项） |

实现位置：`Encoder::open_video()`（编码器名映射、`pix_fmt` 选择、跳过 hwdevice）+ `Encoder::push()`
（NV12 转换，复用 `SwsContextPtr`）。

```bash
# RK3566：H.264 硬件编码
./build/erde transcode in.mp4 out.mp4 --codec libx264 --hw rkmpp --size 1280x720
# RK3566：H.265 硬件编码
./build/erde transcode in.mp4 out.mp4 --codec libx265 --hw rkmpp --size 1920x1080
```

### 6.3 支持矩阵

| 平台 | 编码 | 状态 |
|---|---|---|
| x86_64 + Intel/AMD GPU | `h264_vaapi` | 代码路径完整，无 GPU 机器未真机验证 |
| x86_64 + NVIDIA | `h264_nvenc` | 同上 |
| **RK3566（LubanCat-1）** | **`h264_rkmpp` / `hevc_rkmpp`** | **实测通过，见第 8 节** |
| RK3588 | `h264_rkmpp` / `hevc_rkmpp` | 逻辑相同（VPU 更强），未实测 |

> 解码侧目前走软件解码（`avcodec_find_decoder`）。`ffmpeg-rockchip` 也提供 `h264_rkmpp` 解码器，
> 需要硬件解码时可在 pipeline 里把 decoder 换成按名字查找（见第 9 节）。

## 7. 在 Rockchip RK3566 上部署

以下步骤在 **LubanCat-1（RK3566，4×Cortex-A55 @1.8GHz，4GB RAM，Ubuntu 20.04 / kernel 4.19）**
上逐条跑通，非理论值。

### 7.1 环境确认

```bash
cat /proc/device-tree/model; echo        # EmbedFire LubanCat-1
uname -m                                 # aarch64
ls /dev/mpp_service /dev/rga /dev/dri    # 三个节点都在，VPU/RGA 才有戏
dpkg -l | grep -E 'mpp|rga'              # librockchip-mpp-dev / librga-dev
```

### 7.2 编译带 RKMPP 的 FFmpeg

用 [ffmpeg-rockchip](https://github.com/nyanmisaka/ffmpeg-rockchip)（支持 MPP 编解码 + RGA 滤镜）。

**坑 1：MPP 版本必须 ≥ 1.3.9。** 野火镜像自带 `librockchip-mpp-dev 1.3.8`，`configure` 会直接报
`ERROR: rockchip_mpp >= 1.3.9 not found using pkg-config`（`rockchip_mpp.pc` 里写的是 1.3.8）。
解决：编 [jellyfin-mpp 分支](https://github.com/nyanmisaka/mpp) 到 `/usr/local`。

```bash
sudo apt install -y git cmake pkg-config gcc g++ libdrm-dev libasound2-dev libv4l-dev nasm

# MPP（≥1.3.9）
git clone -b jellyfin-mpp --depth=1 https://github.com/nyanmisaka/mpp.git
cmake -S mpp -B mpp/build -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON -DBUILD_TEST=OFF
sudo cmake --build mpp/build -j4 && sudo cmake --install mpp/build && sudo ldconfig

# FFmpeg（本仓库实测版本 8.1.2，配置项来自野火文档）
git clone --depth=1 https://github.com/nyanmisaka/ffmpeg-rockchip.git
cd ffmpeg-rockchip
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig \
./configure --prefix=/usr/local --enable-gpl --enable-version3 \
  --enable-libdrm --enable-rkmpp --enable-rkrga --enable-alsa --enable-libv4l2 \
  --disable-doc --enable-shared
make -j4 && sudo make install && sudo ldconfig

# 验证
/usr/local/bin/ffmpeg -hide_banner -encoders | grep rkmpp
#  V..... h264_rkmpp   Rockchip MPP (Media Process Platform) H264 encoder
#  V..... hevc_rkmpp   Rockchip MPP (Media Process Platform) HEVC encoder
#  V..... mjpeg_rkmpp  Rockchip MPP (Media Process Platform) MJPEG encoder
```

> **坑 2**：`--prefix=/usr/local` 是为了不覆盖 apt 的 4.2；编 erde 时把
> `PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:...` 放最前即可。
> **坑 3**：4×A55 上 `make -j4` 大约 **40–60 分钟**，别以为卡死了。

### 7.3 编译 erde

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig
pkg-config --modversion libavcodec      # 62.x（FFmpeg 8.x）或 60.x（FFmpeg 6.x）

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
cd build && ctest --output-on-failure   # 7/7 全过
```

### 7.4 跑通

```bash
./build/erde gen /tmp/av.mp4 --codec mpeg4 --size 1280x720 --fps 30 --frames 90 --with-audio --seconds 3
./build/erde probe /tmp/av.mp4

# 软件链路
./build/erde transcode /tmp/av.mp4 /tmp/sw.mp4 --codec mpeg4 --size 854x480

# 硬件链路（RK3566 VPU）
./build/erde transcode /tmp/av.mp4 /tmp/hw720.mp4  --codec libx264 --hw rkmpp --size 1280x720
./build/erde transcode /tmp/av.mp4 /tmp/hw1080.mp4 --codec libx264 --hw rkmpp --size 1920x1080
```

### 7.5 ROS 2 与 `erde_ros`

| 系统 | 可用发行版 | 说明 |
|---|---|---|
| Ubuntu 20.04 | **Foxy**（实测） | 官方源已过 EOL，key 过期需 `[trusted=yes]`；USTC 镜像无 arm64 foxy 包，用 `packages.ros.org` |
| Ubuntu 22.04 | **Humble**（推荐新装） | 官方支持，无需绕路 |

```bash
sudo apt install -y ros-foxy-ros-base ros-foxy-diagnostic-updater \
  python3-colcon-common-extensions
source /opt/ros/foxy/setup.bash

mkdir -p ~/ros2_ws/src && cp -r erde_ros ~/ros2_ws/src/
cd ~/ros2_ws
colcon build --packages-select erde_ros --cmake-args \
  -DCMAKE_PREFIX_PATH="/usr/local;$CMAKE_PREFIX_PATH"
source install/setup.bash
ros2 launch erde_ros erde.launch.py input:=/tmp/av.mp4 \
  codec:=libx264 hw:=rkmpp filter:='fps=15' topic:=/erde/image
```

> `erde_ros` 依赖 `rclcpp`、`rclcpp_lifecycle`、`sensor_msgs`、`std_srvs`、`diagnostic_updater`、
> `launch_ros`；`diagnostic_updater` 不在 `ros-base` 里，需要单独装。
> `find_package(erde)` 要求先 `cmake --install . --prefix /usr/local`（erde 不是 rosdep key）。

## 8. 实测数据

**LubanCat-1 / RK3566（4×A55 @1.8GHz，4GB，TF 卡，Ubuntu 20.04，FFmpeg 8.1.2 + MPP 1.3.9）**
输入统一为 `erde gen` 生成的 1280×720@30fps / 3.0s / 90 帧 + AAC 音轨；耗时为**端到端**
（解复用 + 软件解码 + 可选缩放 + 编码 + 复用），单位秒。

| 任务 | 输出 | 耗时 | 说明 |
|---|---|---|---|
| `gen` 720p 90 帧 + 音轨 | mpeg4 | 2.65 s | 造测试素材 |
| `transcode` 720p | mpeg4 720p | 2.45 s | 纯软件，无缩放 |
| `transcode` 720p→480p | mpeg4 854×480 | 1.86 s | 软件 + sws 缩放 |
| `transcode` **硬编** 720p | **h264_rkmpp** | **1.73 s** | VPU 编码 |
| `transcode` **硬编** 1080p | **h264_rkmpp** | **3.91 s** | 720p 源 → sws 放大到 1080p |
| `transcode` **硬编** HEVC 720p | **hevc_rkmpp** | **1.75 s** | VPU 编码 |
| `ctest` | — | 2.05 s | 7/7 通过 |

读法提醒：源是 **mpeg4 软件解码**，所以端到端时间里解码 + 缩放占比不小；纯编码吞吐比表格更乐观。
想在 x86 上对比 `libx264`，需要 FFmpeg 带 `--enable-libx264` 重新编译（本次 ARM 构建未开）。

## 9. 已知限制

1. **解码仍走软件**：`h264_rkmpp` 解码器尚未接线；4K 片源先软解会成为瓶颈。
2. **`--hw rkmpp` 不传 `preset`/`crf`**：RKMPP 用自己的码控（默认 CBR + `bit_rate`），要 VBR 需要补
   `rc_mode` 选项映射。
3. **vaapi/cuda 未真机验证**：手上没有对应的 GPU 机器，上传路径只做过编译期验证。
4. **单线程 stage 图**：一条管线一个线程，多路并发请起多进程（`erde_ros` 每节点一条管线）。
5. **音频硬编未做**：RK3566 无音频编码硬件，音频始终软件编码（AAC/MP3）。

## 10. 致谢与许可

- FFmpeg 及其 `libav*`；Rockchip 的 MPP / RGA；[nyanmisaka/ffmpeg-rockchip](https://github.com/nyanmisaka/ffmpeg-rockchip)
  与 [nyanmisaka/mpp](https://github.com/nyanmisaka/mpp) 提供了 Rockchip 平台可用的编解码栈。
- 板卡侧编译与实测环境：野火 LubanCat-1（RK3566）。
- 许可：[MIT](LICENSE)。
