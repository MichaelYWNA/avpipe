<div align="center">

# Erde

**基于 FFmpeg 的拉取式音视频转码管线 —— 视频、音频、滤镜、多流混流，一条 CMake 就能编。**

<p>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-2ea44f"></a>
  <a href="https://github.com/MichaelYWNA/erde/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/MichaelYWNA/erde/actions/workflows/ci.yml/badge.svg?branch=main"></a>
  <img alt="Version" src="https://img.shields.io/badge/version-0.3.0-1f6feb">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-17-00599c">
  <img alt="FFmpeg" src="https://img.shields.io/badge/FFmpeg-6.1-3c873a">
  <img alt="Tests" src="https://img.shields.io/badge/tests-34%20cases%20passing-brightgreen">
  <a href="README_EN.md"><img alt="Language" src="https://img.shields.io/badge/language-中文%20%7C%20English-8957e5"></a>
</p>

<p>
  <a href="#3-快速开始">快速开始</a>
  · <a href="#4-构建与依赖">构建</a>
  · <a href="#5-命令参考">命令</a>
  · <a href="#6-架构">架构</a>
  · <a href="#7-测试与实测">实测数据</a>
  · <a href="#8-已知限制">已知限制</a>
  · <a href="#9-贡献与开发">贡献</a>
  · <a href="README_EN.md">English</a>
</p>

</div>

---

`erde` 把 FFmpeg 的解复用、解码、滤镜、缩放、编码、复用拆成可组合的 **stage**，每个 stage 只暴露
一个 `pull()`，由下游主动向上游取数据。它不依赖 ROS、不依赖任何第三方框架，只用 pkg-config 找到
libav\*，一份 `cmake` 就能编出一个命令行工具。

> 设计上的取舍、踩过的坑、以及每个数字的来源都写在 [docs/DESIGN.md](docs/DESIGN.md)；
> 本文只回答"它是什么、能做什么、怎么跑起来"。

## 目录

- [1. 这个项目解决什么](#1-这个项目解决什么)
- [2. 核心能力](#2-核心能力)
- [3. 快速开始](#3-快速开始)
- [4. 构建与依赖](#4-构建与依赖)
- [5. 命令参考](#5-命令参考)
  - [5.1 `probe`：看清楚一个文件](#51-probe看清楚一个文件)
  - [5.2 `gen`：不需要素材的测试片](#52-gen不需要素材的测试片)
  - [5.3 `transcode`：视频链路（有音轨时一并转）](#53-transcode视频链路有音轨时一并转)
  - [5.4 `transcode-audio`：音频链路](#54-transcode-audio音频链路)
  - [5.5 `bench`：测吞吐](#55-bench测吞吐)
  - [5.6 配置文件](#56-配置文件)
- [6. 架构](#6-架构)
  - [6.1 数据流](#61-数据流)
  - [6.2 三个接口](#62-三个接口)
  - [6.3 目录结构](#63-目录结构)
  - [6.4 关键设计](#64-关键设计)
  - [6.5 ROS 2 层：`erde_ros`](#65-ros-2-层erde_ros)
- [7. 测试与实测](#7-测试与实测)
  - [7.1 测试索引](#71-测试索引)
  - [7.2 实测数据](#72-实测数据)
  - [7.3 复现](#73-复现)
- [8. 已知限制](#8-已知限制)
- [9. 贡献与开发](#9-贡献与开发)
- [10. 致谢与许可](#10-致谢与许可)

## 1. 这个项目解决什么

常见的转码脚本是"三段循环互相推"：解码器把帧推进队列，编码器从队列取。代价是每个交接点都要
自己管水位、自己决定队列满了丢谁——而"丢最新一帧"这类 bug 恰好藏在这个决定里，一旦源帧率或
编码器延迟变化，就会静默丢帧/丢样本。

`erde` 反过来：**下游向上游要数据**。

```cpp
Expected<Frame> Decoder::pull();   // 值 / need_more / end_of_stream
```

`pull()` 用三态表达"有数据、暂时没有、永远没有"，于是：

- 循环里没有"保留最后一个"的决策点，帧不会在 stage 边界被静默丢掉；
- 解码器的 B 帧重排、编码器的 `EAGAIN`、音频切片器的缓冲，都封在各自 stage 内部；
- 插一个滤镜、插一个缩放，只是换掉 `FrameSource*`，别处不用改。

## 2. 核心能力

| 领域 | 已实现 | 说明 |
|---|---|---|
| 视频 | 解复用 / 解码 / 滤镜 / 缩放 / 编码 | libx264、libx265、mpeg4、mjpeg… |
| 音频 | 解码 / 滤镜 / 重采样 / 定长切帧 / 编码 | AAC、mp3、opus…（`AudioSlicer` 负责帧长对齐） |
| 滤镜 | `--filter`、`--audio-filter` 接任意 libavfilter 链 | `fps=15`、`crop=…`、`atempo=…`、`volume=…` |
| 混流 | 一个容器写多路流 | `av_interleaved_write_frame` 负责交织 |
| 硬件编码 | `--hw vaapi` / `--hw cuda` | 设备帧上传已实现，本机无 GPU 未真机验证（见[限制](#8-已知限制)） |
| ROS 2 | `erde_ros` 生命周期节点 | 节点持有 stage 图，发布 `CompressedImage`，带 diagnostics 与 stats 服务（见 [6.5](#65-ros-2-层erde_ros)） |

## 3. 快速开始

编译好后（见[第 4 节](#4-构建与依赖)），不需要任何素材文件就能把整条链路跑一遍：

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
| 看一眼容器 | `erde probe in.mp4` |
| 测这条链路的吞吐 | `erde bench in.mp4 out.mp4 --runs 10` |

## 4. 构建与依赖

只依赖 libav\* 的六个库和 CMake ≥ 3.16（GCC 13 / Clang 均可）：

```bash
# Ubuntu / Debian
sudo apt install -y build-essential cmake pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev libavfilter-dev

git clone https://github.com/MichaelYWNA/erde.git
cd erde
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

编译产物是单个可执行文件 `build/erde`。本机实测（32 vCPU）干净构建 **11 秒**，
`ctest` 全部用例 **0.5 秒**。

## 5. 命令参考

### 5.1 `probe`：看清楚一个文件

```bash
./build/erde probe in.mp4
# format=mov,mp4,m4a,3gp,3g2,mj2 duration=2.005s packets=155
#   video(index=0, codec=h264, 640x480, fps=30, tb=1/15360) packets=60
#   audio(index=1, codec=aac, 48000 Hz, tb=1/48000) packets=95
# size=40322 bytes
```

### 5.2 `gen`：不需要素材的测试片

| 参数 | 作用 |
|---|---|
| `--size WxH` / `--fps` / `--frames` | 视频画面：渐变 + 移动方块 |
| `--audio` | 只生成正弦音（纯音频文件） |
| `--with-audio` | 画面 + 音轨，写进同一个文件 |
| `--seconds` | 音轨时长 |

### 5.3 `transcode`：视频链路（有音轨时一并转）

| 选项 | 作用 |
|---|---|
| `--codec` / `--bitrate` / `--crf` / `--preset` / `--gop` | 视频编码参数 |
| `--size WxH` | 插入缩放 stage |
| `--filter <desc>` | 插入 libavfilter 链（在缩放之前） |
| `--fps N` | 断言帧率；与源不一致且 `--filter` 里没有 `fps=` 时直接报错 |
| `--no-audio` | 丢掉音频流 |
| `--audio-codec` / `--audio-bitrate` / `--audio-filter` | 音频流参数 |
| `--hw` / `--hw-device` | 硬件编码 |

### 5.4 `transcode-audio`：音频链路

```bash
./build/erde transcode-audio in.m4a out.m4a --rate 44100 --channels 1 --bitrate 64k \
  --audio-filter 'atempo=1.25'
```

解码 → 滤镜 → `swresample` 重采样 → `AVAudioFifo` 定长切帧 → 编码 → 复用。切帧是必需的：
AAC 只接受每帧 1024 个样本，Opus 960。

### 5.5 `bench`：测吞吐

```bash
./build/erde bench in.mp4 out.mp4 --codec libx264 --preset veryfast --crf 23 --runs 5
# runs=5 frames_per_run=120 total_ms=763.221 fps=786.142
```

### 5.6 配置文件

命令行之外，也可以用 INI 文件给默认值（`--config pipeline.ini`）：

```ini
[video]
size   = 640x480
fps    = 30
filter = fps=15

[audio]
rate     = 44100
channels = 1
filter   = volume=0.8

[encode]
codec   = mpeg4
bitrate = 1200k
preset  = veryfast
crf     = 23
gop     = 12
```

解析器手写在 `src/config.cpp`，带行号报错：`erde: pipeline.ini: line 4: expected 'key = value'`。

## 6. 架构

### 6.1 数据流

```text
video: demux → decode → [filter] → [scale] → encode ─┐
                                                     ├─ Muxer(add_stream ×N) → 容器
audio: demux → decode → [filter] → resample → slice → encode ─┘
```

两条链各自持有一个 `Demuxer`（一个 demuxer 只服务一路选中的流），按时间轴决定先拉哪条，
`av_interleaved_write_frame()` 负责实际交织。

### 6.2 三个接口

| 接口 | 方法 | 谁实现 |
|---|---|---|
| `PacketSource` | `Expected<Packet> pull()` | `Demuxer`、`Encoder` |
| `FrameSource` | `Expected<Frame> pull()` | `Decoder`、`FilterSource`、`ScalingSource`、`AudioResampler`、`AudioSlicer`、`ToneSource` |
| `Muxer` | `add_stream()` / `start()` / `write()` / `close()` | 容器输出 |

### 6.3 目录结构

```text
include/erde/       公开头文件：error/expected、RAII Ptr、Frame/Packet、stage 接口、各 stage
src/                实现；src/cli/main.cpp 是命令行入口
tests/              60 行测试框架 + 7 个测试程序
docs/DESIGN.md      设计取舍与踩坑记录（时间基、滤镜、多流、Expected/Status）
```

### 6.4 关键设计

| 设计 | 为什么 |
|---|---|
| 三态 `pull()` | "暂时没有"和"永远没有"必须能区分，否则循环里总要额外查 `finished()` |
| `Expected<T>` 代替异常 | 媒体错误绝大多数可恢复可上报；异常会把控制流藏进 `try` 块 |
| RAII `Ptr` + deleter policy | libav 有 `f(T**)` 和 `f(T*)` 两套释放约定（`sws_freeContext`） |
| 编码器时间基 `1/fps` | 容器要 1/600（AVI）或 1/15360（MP4），从粗基缩放才不丢精度 |
| 滤镜后重新测几何/时间基 | `crop` 改尺寸、`fps` 改时间基，编码器不能沿用源的参数 |

### 6.5 ROS 2 层：`erde_ros`

`erde_core` 是纯 C++ 库，`erde_ros` 是架在它上面的生命周期节点——**不是** `ros2_control` 的
controller 插件链：节点自己持有管线对象图，`on_configure` 建链、`on_activate` 起定时器、
`on_deactivate` flush 并停表。这样媒体 stage 仍然是能被单元测试直接驱动的普通类。

```bash
# 1) 装核心库（供 find_package(erde) 使用）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake --install build --prefix /opt/erde

# 2) 编 ROS 2 包
source /opt/ros/jazzy/setup.bash
export CMAKE_PREFIX_PATH=/opt/erde:$CMAKE_PREFIX_PATH
colcon build --base-paths erde_ros

# 3) 跑起来（文件或设备）
source install/setup.bash
ros2 launch erde_ros erde.launch.py input:=/tmp/av.mp4
# 设备输入：
ros2 launch erde_ros erde.launch.py input:=/dev/video0 input_format:=v4l2 \
    input_options:='video_size=1280x720,framerate=30'
```

| 接口 | 说明 |
|---|---|
| 话题 `~/image/compressed` | `sensor_msgs/CompressedImage`，`format` 为编码器名（如 `libx264`） |
| 服务 `~/stats` | `std_srvs/Trigger`，返回 `pulled / published / errors / packets` |
| 诊断 `~/diagnostics` | 每秒一次，含 codec、帧数、解码器计数；未激活或源结束时为 WARN |
| 生命周期 | `configure → activate → deactivate → cleanup`，`autostart:=true` 时自动前两步 |

实测（本机，320×240 25 秒素材）：节点 `active`，10 秒内 `pulled=298 published=278 errors=0`，
差值来自 x264 的 lookahead——不是丢帧。

## 7. 测试与实测

### 7.1 测试索引

| 测试程序 | 用例 | 覆盖 |
|---|---|---|
| `test_config` | 7 | INI 解析、错误分支、size/bitrate 解析、缺失键 |
| `test_media` | 6 | `Expected` 语义、`Ptr` 移动、`Frame` 分配/克隆、错误文本 |
| `test_pipeline` | 5 | `gen → probe → transcode → bench`，帧数守恒、失败路径 |
| `test_filter` | 3 | `fps=15` 帧数减半、`crop` 改尺寸、非法滤镜被拒绝 |
| `test_audio` | 3 | 正弦音生成、48k→44.1k 单声道、纯视频文件被拒绝 |
| `test_encode` | 4 | 软件/音频编码器参数、未知编码器、硬件编码两个分支 |
| `test_av` | 6 | 双流生成与转码、`--no-audio` 丢轨、`atempo` 改时长、非法音频滤镜 |

合计 **7 个测试程序 / 34 个用例**，`ctest` 全绿；GitHub Actions 上 gcc 与 clang 双矩阵跑同一套。
ROS 2 层由 `scripts/ros_smoke.sh` 做端到端冒烟（起节点 → 等话题 → 断言收到 `CompressedImage`），
CI 里单独一个 `ros:jazzy-ros-base` 容器 job 跑它。

### 7.2 实测数据

环境：Ubuntu 24.04 / WSL2 / 32 vCPU / FFmpeg 6.1 / Release，`bench` 多次平均。

| 链路 | 结果 |
|---|---|
| 1280×720 → 640×480 mpeg4 | **1210 fps** |
| 1280×720 → 1280×720 libx264 `veryfast` | **786 fps** |
| 1280×720 → `fps=30,scale=640:360` → libx264 | **858 fps** |
| 1280×720 → 1280×720 libx265 `ultrafast` | **219 fps** |
| 10 s AAC 48k 立体声 → 44.1k 单声道 | **≈ 48 ms**（208× 实时） |
| 2 s A/V 文件 → 转码 | 2.005 s，60 视频包 + 95 音频包 |
| 1.002 s 音频 `atempo=2.0` | 0.512 s |

### 7.3 复现

```bash
./build/erde gen /tmp/720.mp4 --size 1280x720 --frames 120 --crf 23
./build/erde bench /tmp/720.mp4 /tmp/out.avi --size 640x480 --codec mpeg4 --runs 5
./build/erde bench /tmp/720.mp4 /tmp/out.mp4 --codec libx264 --preset veryfast --crf 23 --runs 5
./build/erde gen /tmp/tone.m4a --audio --seconds 10
./build/erde transcode-audio /tmp/tone.m4a /tmp/tone-out.m4a --rate 44100 --channels 1
```

数字是这台机器上量出来的，换机器会有差异；脚本和参数都在上面，可以自己复跑。

## 8. 已知限制

写清楚比藏着好，这些是真没做或没验证：

1. **硬件编码未真机验证**：本机 WSL 没有 `/dev/dri`，只验证了"设备缺失时报错清晰、不崩溃"。
2. **音频末尾补齐**：切片器用静音把最后一帧补到编码器帧长，1.000 s 的音源转出来是 1.002 s。
3. **非整数帧率**：编码器时间基取 `1/fps`（四舍五入），29.97 长时间转码会有毫秒级漂移。
4. **单路视频 + 单路音频**：多机位、多音轨、字幕流不在范围内。
5. **单线程**：两条链交替推进，没有线程池；`bench` 测的是这条单线程链路。

## 9. 贡献与开发

### 9.1 共享原则

1. **错误必须可分类**：一律返回 `Expected`/`Status`，不吞 `int` 返回值。
2. **状态归 stage 所有**：pending packet、draining 标志、EOF 闩锁留在需要它的那层。
3. **每个数字都要能复现**：README 里的吞吐都能用 `bench` 重跑出来。
4. **提交前跑三件事**：`cmake --build build -j && ctest --test-dir build && ./build/erde --help`。

### 9.2 新增一个 stage

```cpp
// include/erde/my_stage.hpp
class MyStage : public FrameSource {
 public:
  static Expected<std::unique_ptr<MyStage>> open(FrameSource& upstream, /* config */);
  Expected<Frame> pull() override;
};
```

1. 继承 `FrameSource`（出帧）或 `PacketSource`（出包），只实现 `pull()`；
2. EOF 返回 `Code::end_of_stream`，暂时没数据返回 `Code::need_more`，其余错误用 `from_av()`；
3. 在 `pipeline.cpp` 里插到链上，注意"滤镜会改时间基/几何"，需要时重新取；
4. 加一个不依赖外部素材的测试（用 `gen` 造输入）。

### 9.3 状态标签

| 标签 | 含义 |
|---|---|
| ✅ 已验证 | 本机跑过并有数字/断言 |
| ⚠️ 未验证 | 代码已实现，缺硬件或环境（例如 VAAPI） |
| ❌ 未实现 | 见[已知限制](#8-已知限制) |

## 10. 致谢与许可

- 编解码、格式、滤镜全部由 [FFmpeg](https://ffmpeg.org/)（libav\*）完成，本项目只做编排；
- 灵感来自把媒体处理拆成 stage 的思路，实现与取舍见 [docs/DESIGN.md](docs/DESIGN.md)；
- 许可证：**MIT**，见 [LICENSE](LICENSE)。
