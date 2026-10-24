<div align="center">

# Erde

**基于 FFmpeg 的拉取式（pull-based）音视频转码管线**

C++17 核心库 + 命令行工具，另有可选的 ROS 2 生命周期节点；支持 RK3566/RK3588 硬件编解码（RKMPP）。

</div>

> 版本 **0.4.0** ｜ 许可 MIT ｜ 已在 LubanCat-1（RK3566, aarch64）与 x86_64 上构建并实测

---

## 目录

- [0. 30 秒认识它](#0-30-秒认识它)
- [1. 仓库里有什么](#1-仓库里有什么)
- [2. 它解决什么问题](#2-它解决什么问题)
- [3. 数据流](#3-数据流)
- [4. 依赖关系：核心、CLI、ROS 三层](#4-依赖关系核心cliros-三层)
- [5. 构建与安装](#5-构建与安装)
- [6. 命令行完整参考](#6-命令行完整参考)
- [7. C++ 库 API](#7-c-库-api)
- [8. ROS 2 层 erde_ros](#8-ros-2-层-erde_ros)
- [9. 硬件加速与 RK3566 部署](#9-硬件加速与-rk3566-部署)
- [10. 实测数据](#10-实测数据)
- [11. 测试与 CI](#11-测试与-ci)
- [12. 已知限制与路线图](#12-已知限制与路线图)
- [13. 致谢与许可](#13-致谢与许可)

---

## 0. 30 秒认识它

| 是 | 不是 |
|---|---|
| 一个把 FFmpeg 拆成可组合 stage 的 C++17 库（`erde_core`） | 不是 FFmpeg 的封装或重写，底层就是 libav* |
| 一个可直接用的命令行工具（`erde`）：probe / gen / transcode / transcode-audio / bench | 不是转码服务，没有 HTTP 接口和任务队列 |
| 一个**可选**的 ROS 2 生命周期节点（`erde_ros`），发布 `sensor_msgs/CompressedImage` | 不是 ROS 的替代品，也不是 ros2_control 控制器 |
| 支持软件编码，以及 VAAPI / NVENC / **RKMPP（RK3566/RK3588）** | 不负责视频采集驱动，也不负责显示 |
| 一份 CMake、pkg-config 找到 libav 就能编 | 不需要 Qt、不需要 GStreamer、不需要 Boost |

**一句话**：给它一个输入文件（或摄像头设备），按你给的滤镜/尺寸/编码器转出一个新文件，并且**任何一帧都不会在 stage 交接处被静默丢掉**。
它把这件事做成了可复用、可测试、可移植到 ARM 板子上的库。

---

## 1. 仓库里有什么

```text
erde/
├── include/erde/            # 公共头文件（对外 API，纯 C++17）
│   ├── stage.hpp            # 两个基础接口：PacketSource / FrameSource，各只有一个 pull()
│   ├── demux.hpp            # Demuxer：容器 -> AVPacket
│   ├── decode.hpp           # Decoder：AVPacket -> AVFrame
│   ├── filter.hpp           # FilterSource：libavfilter 图（视频/音频通用）
│   ├── scale.hpp            # ScalingSource：libswscale 改尺寸/像素格式
│   ├── encode.hpp           # Encoder：AVFrame -> AVPacket（含硬件编码）+ EncodeOptions
│   ├── audio.hpp            # ToneSource / AudioResampler / AudioSlicer
│   ├── mux.hpp              # Muxer：多流写同一个容器
│   ├── media.hpp            # Frame / Packet / StreamInfo 与 RAII 别名
│   ├── handle.hpp           # 两种 deleter 签名的 Ptr<T, Deleter>
│   ├── error.hpp            # Error / Code / Expected<T> / Status
│   ├── config.hpp           # INI 配置解析
│   └── pipeline.hpp         # 门面层：transcode / transcode_audio / generate / probe / bench
├── src/                     # 上述头文件的实现，外加 src/cli/main.cpp
├── tests/                   # 7 个可执行测试（config / media / pipeline / filter / audio / encode / av）
├── erde_ros/                # 可选的 ROS 2 ament_cmake 包（生命周期节点）
│   ├── package.xml          # 依赖 rclcpp / rclcpp_lifecycle / sensor_msgs / std_srvs / diagnostic_updater
│   ├── CMakeLists.txt       # find_package(erde) + ament_target_dependencies
│   ├── include/erde_ros/erde_node.hpp
│   ├── src/erde_node.cpp    # 生命周期回调里建/拆整条 stage 链
│   ├── src/main.cpp         # autostart=true 时自己 configure + activate
│   ├── launch/erde.launch.py
│   └── config/erde_params.yaml
├── docs/DESIGN.md           # 设计取舍与踩坑记录（时间基、滤镜、音频、多流）
├── scripts/ros_smoke.sh     # ROS 层端到端冒烟：起节点 -> 等 active -> 等 CompressedImage
├── cmake/erde-config.cmake.in  # 供 find_package(erde) 使用
└── .github/workflows/ci.yml # ubuntu-24.04 × {gcc, clang} 构建 + ctest + CLI 冒烟
```

---

## 2. 它解决什么问题

常见的转码代码是“推”的：解码器把帧推进队列，编码器从队列取。每个交接点都要自己决定“队列满了丢谁”——
而**丢帧的决定往往被埋在一个 while 循环里**，一旦源帧率、B 帧重排或编码器延迟变化，就会静默丢帧或丢样本，且没有任何报错。

erde 反过来：**下游向上游要数据**，`pull()` 用三态表达结果：

| 返回 | 含义 |
|---|---|
| 值 | 有数据 |
| `Code::need_more` | 现在没有，但没结束（解码器要更多输入 / 编码器暂时要不到包） |
| `Code::end_of_stream` | 永远没有了 |

于是循环里**没有“保留最后一个”的决策点**：解码器的 B 帧重排、编码器的 `EAGAIN`、音频切片器的缓冲，全部封在各自 stage 内部。
插一个滤镜、插一个缩放，只是换掉一个 `FrameSource*`，别处一行不用改。

---

## 3. 数据流

```text
视频链：Demuxer -packet-> Decoder -frame-> [FilterSource] -frame-> [ScalingSource] -frame-> Encoder -packet-> Muxer
音频链：Demuxer -packet-> Decoder -frame-> [FilterSource] -frame-> AudioResampler -frame-> AudioSlicer -frame-> Encoder -packet-> Muxer
```

- 方括号里的 stage **只在需要时存在**（有 `--filter` 才有滤镜，有 `--size` 才有缩放）。
- 两条链各有自己的 `Demuxer`（同一文件开两次代价很小），因为视频 30 fps、音频 43 Hz 的节拍不同，共享一个 reader 会互相阻塞。
- 两条链按时间轴交替取包（`video.last_time <= audio.last_time`），所以送进 muxer 的包天然有序，`av_interleaved_write_frame()` 只需处理有限重排。

---

## 4. 依赖关系：核心、CLI、ROS 三层

这一节专门澄清一个容易误读的说法：**“erde 不依赖 ROS”只对核心成立，对仓库不成立。**

| 层 | 是什么 | 依赖 | 能脱离其它层单独用吗 |
|---|---|---|---|
| `erde_core`（静态库） | 所有 stage + pipeline 门面 | 只有 6 个 libav* 库 + C++17 标准库 | 可以，编进你自己的程序 |
| `erde`（CLI） | `src/cli/main.cpp` | `erde_core` | 可以，独立可执行文件 |
| `erde_ros`（ROS 2 包） | 生命周期节点，发布 `CompressedImage` | `erde_core` + rclcpp / rclcpp_lifecycle / sensor_msgs / std_srvs / diagnostic_updater / launch_ros | 可以，不编它就不需要装 ROS |

准确的说法是：**核心零框架依赖，ROS 2 是仓库里可选的一层。** 只用命令行或把 `erde_core` 链进自己的程序，完全不需要装 ROS；
想用 ROS 2，就编 `erde_ros` 这个包。

---

## 5. 构建与安装

### 5.1 依赖

```bash
# Debian / Ubuntu
sudo apt install -y build-essential cmake pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev libavfilter-dev
```

**FFmpeg 版本：≥ 6.1**。代码用到 `AVChannelLayout` / `av_channel_layout_*`（5.1 引入），所以 Ubuntu 20.04 自带的 FFmpeg 4.2 **编不过**
（这也是 RK3566 上必须自己编 FFmpeg 的原因，见第 9 节）。向上兼容：本仓库已在 **FFmpeg 8.1.2** 上构建并跑完全部测试。

### 5.2 编译核心与 CLI

```bash
git clone https://github.com/MichaelYWNA/erde.git
cd erde
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure     # 7/7
```

产物：`build/erde`（CLI）、`build/liberde_core.a`。

> `ctest --test-dir build` 需要 CMake ≥ 3.20；Ubuntu 20.04 自带 3.16，请用 `cd build && ctest`。

### 5.3 安装成库（给 `erde_ros` 或你自己的工程用）

```bash
sudo cmake --install build --prefix /usr/local
# 安装：liberde_core.a / include/erde/*.hpp / lib/cmake/erde/{erdeConfig,erdeTargets}.cmake
```

之后别的工程里 `find_package(erde REQUIRED)` 加 `target_link_libraries(x PRIVATE erde::erde_core)` 即可。

### 5.4 编译 ROS 2 包（可选）

```bash
mkdir -p ~/ros2_ws/src && cp -r erde_ros ~/ros2_ws/src/
cd ~/ros2_ws
colcon build --packages-select erde_ros --cmake-args -DCMAKE_PREFIX_PATH="/usr/local;/opt/ros/$ROS_DISTRO"
source install/setup.bash
```

### 5.5 在 RK3566 上编译

见 [第 9 节](#9-硬件加速与-rk3566-部署)，那里有完整顺序和两个必踩的坑。

---

## 6. 命令行完整参考

### 6.1 五个子命令

| 子命令 | 作用 | 典型用途 |
|---|---|---|
| `erde probe <file>` | 打印容器格式、时长、每条流的编码/分辨率/帧率/时间基、包数 | 先看清楚素材 |
| `erde gen <out>` | **不需要素材**，生成测试片（视频图案、正弦音，或两者混流） | CI 与自测 |
| `erde transcode <in> <out>` | 视频链，有音轨时一并转 | 主功能 |
| `erde transcode-audio <in> <out>` | 音频链（可重采样 / 变速 / 调音量 / 改声道） | 音频处理 |
| `erde bench <in> <out>` | 同一转换跑 N 次，报总耗时与 fps | 测吞吐 |

### 6.2 全部参数

| 参数 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `--codec` | `libx264` / `libx265` / `mpeg4` / `mjpeg` 等 | `libx264` | 视频编码器；配 `--hw` 时表示“软件对应物” |
| `--bitrate` | `2000k` / `2M` / `1500000` | `2000000` | 目标码率 |
| `--preset` | `veryfast` 等 | `veryfast` | 仅 x264/x265；**rkmpp 下自动忽略** |
| `--crf` | 整数 | 关 | 质量优先；**rkmpp 下自动忽略** |
| `--gop` | 整数 | `12` | 关键帧间隔 |
| `--fps` | 整数 | 源帧率 | `gen` 用它定帧率；`transcode` 用它**校验**源帧率 |
| `--size` | `WxH` | 源尺寸 | 插入 `ScalingSource` |
| `--filter` | libavfilter 描述 | 空 | 如 `fps=15,crop=640:360` |
| `--audio-filter` | libavfilter 描述 | 空 | 如 `atempo=2.0,volume=0.5` |
| `--audio-codec` | `aac` / `libmp3lame` 等 | `aac` | 音频编码器 |
| `--audio-bitrate` | 同 `--bitrate` | `128k` | 音频码率 |
| `--no-audio` | 开关 | 关 | 丢弃音轨 |
| `--with-audio` | 开关 | 关 | `gen`：视频 + 正弦音一起出 |
| `--audio` | 开关 | 关 | `gen`：只出音频 |
| `--frames` | 整数 | `60` | `gen` 帧数 |
| `--seconds` | 浮点 | `2.0` | `gen --audio` 时长 |
| `--rate` | 整数 | `48000` | 采样率 |
| `--channels` | 整数 | 源/2 | 声道数 |
| `--hw` | `rkmpp` / `vaapi` / `cuda` | 空（软件） | 硬件编码后端 |
| `--hw-device` | 路径 | 空 | vaapi 用，如 `/dev/dri/renderD128` |
| `--input-format` | `v4l2` / `pulse` / `alsa` 等 | 自动探测 | 设备输入时的解复用器 |
| `--input-options` | 字符串 | 空 | 如 `video_size=1280x720,framerate=30` |
| `--config` | 路径 | 空 | 从 INI 读默认值 |
| `--runs` | 整数 | `3` | `bench` 次数 |
| `--quiet` | 开关 | 关 | 只打印最终统计 |
| `--verbose` | 开关 | 关 | 同时打开 libav 日志 |

### 6.3 配置文件（INI）

```ini
# erde.conf —— 只提供默认值，命令行显式参数优先
[encode]
codec   = libx264
bitrate = 4M
preset  = veryfast
crf     = 23
gop     = 24

[video]
fps    = 30
size   = 1280x720
filter = fps=30
```

用 `erde transcode in.mp4 out.mp4 --config erde.conf` 加载。三个约定：

- 命令行 > 配置文件 > 内置默认值；
- 未知键会被**静默忽略**（没有 schema 校验，这是已知取舍）；
- 解析错误带行号，例如 `erde.conf:3: expected key=value`。

### 6.4 常用示例

```bash
# 1) 造素材（不需要任何输入文件）
./build/erde gen /tmp/av.mp4 --codec mpeg4 --size 1280x720 --fps 30 --frames 90 --with-audio --seconds 3

# 2) 看清素材
./build/erde probe /tmp/av.mp4
# format=mov,mp4,m4a,3gp,3g2,mj2 duration=3.008s packets=232
#   video(index=0, codec=mpeg4, 1280x720, fps=30, tb=1/15360) packets=90
#   audio(index=1, codec=aac, 48000 Hz, tb=1/48000) packets=142

# 3) 缩放 + 换编码器
./build/erde transcode in.mp4 out.mp4 --codec libx265 --crf 28 --preset ultrafast --size 1280x720

# 4) 降帧率（必须有 filter，否则报错并说明原因）
./build/erde transcode in.mp4 out.mp4 --filter "fps=15" --fps 15

# 5) 裁剪画面
./build/erde transcode in.mp4 out.mp4 --filter "crop=640:360:0:0"

# 6) 音频变速 + 重采样
./build/erde transcode-audio in.m4a out.m4a --audio-filter "atempo=2.0" --rate 44100 --channels 1

# 7) 摄像头 → 文件（板卡上 /dev/video0 可用时）
./build/erde transcode /dev/video0 out.mp4 --input-format v4l2 \
  --input-options "video_size=1280x720,framerate=30" --fps 30 --no-audio

# 8) 测吞吐
./build/erde bench in.mp4 out.mp4 --codec mpeg4 --size 640x360 --runs 10

# 9) RK3566 硬件编码
./build/erde transcode in.mp4 out.mp4 --codec libx264 --hw rkmpp --size 1920x1080
```

---

## 7. C++ 库 API

### 7.1 错误与返回值：Expected

不用异常（媒体错误绝大多数是可上报的），也不返回裸 `int`（会丢信息、易被忽略）：

```cpp
struct Error { Code code; int av_code; std::string message; };
enum class Code { ok, end_of_stream, need_more, invalid_argument, config, ffmpeg, io, unsupported };

template <class T> class Expected {          // 40 行，替代 C++17 缺失的 std::expected
  bool has_value() const; explicit operator bool() const;
  T& value(); const Error& error() const; T take();   // take() 显式移出
};
using Status = Error;                        // code == ok 即成功
```

失败打印形如 `erde: ffmpeg[-2]: cannot open /tmp/x.mp4: No such file or directory`（`av_code` 保留原始 `AVERROR`，`to_string()` 用 `av_strerror` 翻成人话）。

### 7.2 两个基础接口

```cpp
class PacketSource { public: virtual Expected<Packet> pull() = 0; };
class FrameSource  { public: virtual Expected<Frame>  pull() = 0; };
```

### 7.3 每个 stage 的职责

| 类 | 输入 → 输出 | 关键方法 / 注意点 |
|---|---|---|
| `Demuxer` | 文件/设备 → `Packet` | `open(path, format, options)`、`select_stream(type)`、`find(type)`、`is_live()`、`duration_seconds()` |
| `Decoder` | `Packet` → `Frame` | `open(PacketSource&, StreamInfo)`、`frames_decoded()`；内部处理 B 帧重排与 `EAGAIN` |
| `FilterSource` | `Frame` → `Frame` | `open(source, "fps=15", time_base)`；**先取一帧建图**，`output_time_base()` 供编码器用 |
| `ScalingSource` | `Frame` → `Frame` | `open(source, w, h)`；尺寸与格式都相同时**直接透传**，不建 sws |
| `Encoder` | `Frame` → `Packet` | `open(StreamInfo, EncodeOptions)`、`push()`、`flush()`、`is_hardware()`、`codec_name()` |
| `AudioResampler` | `Frame` → `Frame` | 第一帧到达才建 `SwrContext`；格式已匹配则透传；EOF 时倒出延迟样本 |
| `AudioSlicer` | `Frame` → `Frame` | 用 `AVAudioFifo` 把随机帧长切成编码器要求的固定帧长；末帧用静音补齐 |
| `ToneSource` | 无 → `Frame` | `gen --audio` 用的正弦波发生器 |
| `Muxer` | `Packet` → 容器 | `open()` → `add_stream()` ×N → `start()` → `write(pkt, idx)` → `close()` |

### 7.4 门面层

```cpp
struct TranscodeRequest {
  std::string input, output;
  EncodeOptions encode;                       // 视频：codec / bit_rate / gop / fps / preset / crf / hw_device
  EncodeOptions audio_encode = default_audio_encode();  // 默认 aac 128k
  int width = 0, height = 0;                  // 0 = 保持源尺寸
  std::string filter, audio_filter;           // libavfilter 描述
  bool include_video = true, include_audio = true;
  std::string input_format, input_options;    // 设备输入
  bool force_fps = false;                     // 显式 --fps 时为 true
};

Expected<PipelineStats> transcode(const TranscodeRequest&);
Expected<PipelineStats> transcode_audio(const AudioTranscodeRequest&);
Expected<PipelineStats> generate(const GenerateRequest&);
Expected<ProbeReport>   probe(const std::string& path, format = {}, options = {});
Expected<BenchReport>   bench(...);
```

`PipelineStats` 的每个计数都由真正干活的 stage 产出（不是事后估算），所以“进 == 出”可以核对：
`packets_read / frames_decoded / frames_converted / frames_encoded / packets_written / audio_* / wall_ms`。

### 7.5 时间基约定（最容易踩的地方）

| 位置 | 时间基 | 为什么 |
|---|---|---|
| 解码器输出帧 `pts` | **源流**时间基（如 mp4 = 1/15360） | libav 就是这么给的 |
| 编码器 `time_base` | **1/fps**（音频 1/sample_rate） | 容器自己会挑时间基（AVI 要 1/600），粗基换算才精确 |
| 滤镜输出 | 由滤镜图决定，用 `av_buffersink_get_time_base()` 问 | `fps=15` 会改成 1/15，不问就会重复 pts |
| 写容器时 | 每流各自的容器时间基 | MP4 改成 1/15360、AVI 1/600，必须逐流缩放 |

`Encoder::push()` 里始终做一次 `av_rescale_q(src_tb → enc_tb)`：漏掉它，2 秒的片子会写成 1006.97 秒。

### 7.6 在自己的程序里用

```cpp
#include <erde/pipeline.hpp>

int main() {
  erde::TranscodeRequest req;
  req.input  = "in.mp4";
  req.output = "out.mp4";
  req.encode.codec     = "libx264";
  req.encode.hw_device = "rkmpp";     // 空字符串 = 软件编码
  req.encode.bit_rate  = 4000000;
  req.width = 1280;
  req.height = 720;
  req.filter = "fps=30";

  auto stats = erde::transcode(req);
  if (!stats) {
    std::fprintf(stderr, "erde: %s\n", stats.error().to_string().c_str());
    return 1;
  }
  std::printf("%s\n", stats.value().summary().c_str());
}
```

编译：`g++ -std=c++17 main.cpp $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale libswresample libavfilter) -lerde_core`

---

## 8. ROS 2 层 erde_ros

一个**生命周期节点** `erde`：`on_configure` 建整条 stage 链，`on_activate` 才创建定时器，`on_deactivate` 刷编码器，`on_cleanup` 释放全部资源。
定时器只存在于 activate 与 deactivate 之间。

### 8.1 参数

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `input` | string | 空（**必填**） | 文件路径或设备，如 `/tmp/a.mp4`、`/dev/video0` |
| `input_format` | string | 空 | 设备解复用器：`v4l2` / `pulse` / `alsa` |
| `input_options` | string | 空 | 如 `video_size=1280x720,framerate=30` |
| `filter` | string | 空 | libavfilter 链 |
| `width` / `height` | int | 0 | 0 = 源尺寸 |
| `codec` | string | `libx264` | 编码器 |
| `hw` | string | 空 | **`rkmpp`（RK3566/RK3588）/ `vaapi` / `cuda`** |
| `hw_device` | string | 空 | vaapi 设备节点 |
| `bitrate` | int | 2000000 | 码率 |
| `gop` | int | 12 | 关键帧间隔 |
| `fps` | int | 30 | 拉取节拍（等于发布频率），应 ≥ 源帧率 |
| `frame_id` | string | `camera` | 消息 header |
| `topic` | string | `~/image/compressed` | 发布话题（私有名，解析为 `/erde/image/compressed`） |
| `autostart` | bool | true | 启动即 configure + activate，不等 `ros2 lifecycle set` |

### 8.2 话题与服务

| 名称 | 类型 | 说明 |
|---|---|---|
| `~/image/compressed` | `sensor_msgs/msg/CompressedImage` | 每帧一个包；`format` 字段填编码器名（如 `h264_rkmpp`） |
| `~/stats` | `std_srvs/srv/Trigger` | 返回统计（帧数、包数、错误计数） |
| `~/transition_event` | `lifecycle_msgs/msg/TransitionEvent` | 生命周期节点自带 |
| `/diagnostics` | `diagnostic_msgs/msg/DiagnosticArray` | 1 Hz，由 `diagnostic_updater` 发布 |

### 8.3 构建、运行与冒烟

```bash
colcon build --packages-select erde_ros --cmake-args -DCMAKE_PREFIX_PATH="/usr/local;/opt/ros/$ROS_DISTRO"
source install/setup.bash

# 文件输入
ros2 launch erde_ros erde.launch.py input:=/tmp/av.mp4 codec:=libx264 hw:=rkmpp topic:=/erde/image

# 摄像头（板卡有 /dev/video0 时）
ros2 launch erde_ros erde.launch.py input:=/dev/video0 input_format:=v4l2 \
  input_options:="video_size=1280x720,framerate=30" hw:=rkmpp width:=1280 height:=720

# 看数据
ros2 topic hz /erde/image
ros2 service call /erde/stats std_srvs/srv/Trigger "{}"

# 生命周期手动控制（autostart:=false 时）
ros2 lifecycle set /erde configure && ros2 lifecycle set /erde activate

# 一行冒烟（自带生成素材 + 等 active + 等 CompressedImage）
scripts/ros_smoke.sh
```

### 8.4 跨机器通信（板卡与电脑之间）

DDS 默认靠组播发现，**点对点链路（USB 网卡 RNDIS、网线直连）上经常发现不了**。三种做法：

| 做法 | 命令 / 配置 | 好处 | 坏处 |
|---|---|---|---|
| 单机调试 | `export ROS_LOCALHOST_ONLY=1` | 零配置、最快 | 跨机器看不到对端 |
| 静态对端 | `export ROS_STATIC_PEERS="169.254.128.2"`（两端都设，填对方 IP） | 不依赖组播，配置轻 | IP 变了要改；节点多了要逐个填 |
| 纯 unicast | Fast DDS XML（关组播 + `initialPeersList`） | 最稳，适合长期部署 | 配置重，新节点不会自动发现 |

两者都要 `ROS_DOMAIN_ID` 一致（如 42）。另外：**能 ping 通但互相看不见，先看 Windows 防火墙有没有挡入站 UDP**。

---

## 9. 硬件加速与 RK3566 部署

### 9.1 三种后端

| 后端 | 编码器名 | 需要设备上下文 | 帧格式 | 备注 |
|---|---|---|---|---|
| 软件 | `libx264` / `libx265` / `mpeg4` | 否 | yuv420p | 通用 |
| `--hw vaapi` | `h264_vaapi` / `hevc_vaapi` | **是**（`AVHWFramesContext` + 上传） | `AV_PIX_FMT_VAAPI` | x86 Intel/AMD |
| `--hw cuda` | `h264_nvenc` / `hevc_nvenc` | 是 | cuda | x86 NVIDIA |
| **`--hw rkmpp`** | **`h264_rkmpp` / `hevc_rkmpp` / `mjpeg_rkmpp`** | **否** | **NV12 系统内存** | **RK3566 / RK3588** |

### 9.2 rkmpp 为什么是特殊的硬件路径

`rkmpp` **不是一个 `AVHWDeviceType`**。`ffmpeg-rockchip` 提供的是一组普通编码器，它们直接吃 NV12，所以 erde 为它单开了一条路径（`Encoder::open_video()` + `Encoder::push()`）：

1. `--hw rkmpp` 按 `libx264` / `libx265` 映射到 `h264_rkmpp` / `hevc_rkmpp`；
2. 校验设备类型时**跳过** `av_hwdevice_find_type_by_name`（它查不到 rkmpp）；
3. `ctx->pix_fmt = AV_PIX_FMT_NV12`，并且**不建** `AVHWDeviceContext` / `AVHWFramesContext`；
4. `preset` / `crf` **不下发**，否则 `avcodec_open2` 会因未知 AVOption 失败；
5. 进来的是 yuv420p 时，用缓存的 `SwsContext` 转 NV12，再 `avcodec_send_frame`。

### 9.3 RK3566（LubanCat-1）完整部署

环境：RK3566（4×Cortex-A55 @1.8GHz，4GB RAM），Ubuntu 20.04，kernel 4.19，**无 eMMC、系统在 TF 卡**。

**第 0 步：确认 VPU 节点在**

```bash
cat /proc/device-tree/model; echo      # EmbedFire LubanCat-1
ls /dev/mpp_service /dev/rga /dev/dri   # 三个都在，才有硬件编解码
```

**第 1 步：MPP 必须 ≥ 1.3.9（第一个必踩的坑）**

野火镜像自带 `librockchip-mpp-dev 1.3.8`，`ffmpeg-rockchip` 的 configure 会直接拒绝：

```text
ERROR: rockchip_mpp >= 1.3.9 not found using pkg-config
# 真实原因：/usr/lib/aarch64-linux-gnu/pkgconfig/rockchip_mpp.pc 里写的是 Version: 1.3.8
```

```bash
sudo apt install -y git cmake meson ninja-build pkg-config gcc g++ libdrm-dev libasound2-dev libv4l-dev

git clone -b jellyfin-mpp --depth=1 https://github.com/nyanmisaka/mpp.git
cmake -S mpp -B mpp/build -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON -DBUILD_TEST=OFF
sudo cmake --build mpp/build -j4 && sudo cmake --install mpp/build && sudo ldconfig
```

**第 2 步：编 ffmpeg-rockchip（第二个坑：约 40 到 60 分钟）**

```bash
git clone --depth=1 https://github.com/nyanmisaka/ffmpeg-rockchip.git
cd ffmpeg-rockchip
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig \
./configure --prefix=/usr/local --enable-gpl --enable-version3 \
  --enable-libdrm --enable-rkmpp --enable-rkrga --enable-alsa --enable-libv4l2 \
  --disable-doc --enable-shared
make -j4 && sudo make install && sudo ldconfig

# 验证
/usr/local/bin/ffmpeg -hide_banner -encoders | grep rkmpp
#  V..... h264_rkmpp   Rockchip MPP ... H264 encoder
#  V..... hevc_rkmpp   Rockchip MPP ... HEVC encoder
#  V..... mjpeg_rkmpp  Rockchip MPP ... MJPEG encoder
```

> **为什么用 `--prefix=/usr/local`**：不覆盖 apt 的 FFmpeg 4.2；编 erde 时把 `PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:...` 放最前即可。
> **注意**：想让软件链路也能用 `--codec libx264`，configure 要加 `--enable-libx264`（需先装 `libx264-dev`）。

**第 3 步：编 erde**

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig
pkg-config --modversion libavcodec      # 62.x = FFmpeg 8.x，60.x = FFmpeg 6.x
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
cd build && ctest --output-on-failure   # 7/7
```

**第 4 步：跑**

```bash
./build/erde gen /tmp/av.mp4 --codec mpeg4 --size 1280x720 --frames 90 --with-audio --seconds 3
./build/erde transcode /tmp/av.mp4 /tmp/hw720.mp4  --codec libx264 --hw rkmpp --size 1280x720
./build/erde transcode /tmp/av.mp4 /tmp/hw1080.mp4 --codec libx264 --hw rkmpp --size 1920x1080
```

**第 5 步：ROS 2**

| 系统 | 可用发行版 | 注意 |
|---|---|---|
| Ubuntu 20.04 | Foxy（本项目实测） | ROS 官方 key 已过期，源加 `[trusted=yes]`；USTC 镜像没有 foxy arm64 包，用 `packages.ros.org` |
| Ubuntu 22.04 | Humble（推荐重烧系统） | 官方支持，常规装法即可 |

```bash
sudo apt install -y ros-foxy-ros-base ros-foxy-diagnostic-updater python3-colcon-common-extensions
```

板子没有外网时，可以让 apt 走电脑上的 HTTP 代理：把 `Acquire::http::Proxy "http://PC_IP:8888/";` 写进 `/etc/apt/apt.conf.d/99proxy`。

---

## 10. 实测数据

### 10.1 x86_64（开发机）

- 32 vCPU 干净构建约 **11 秒**，`ctest` 全部用例约 **0.5 秒**。

### 10.2 RK3566（LubanCat-1，aarch64）

输入：`erde gen` 生成的 1280×720@30fps / 3.0 秒 / 90 帧 + AAC 音轨；耗时是**端到端**（解复用 + 软件解码 + 可选缩放 + 编码 + 复用）。

| 任务 | 输出 | 耗时 | 说明 |
|---|---|---|---|
| `gen` 720p 90 帧 + 音轨 | mpeg4 | 2.65 s | 生成测试素材 |
| 软编 720p | mpeg4 1280×720 | 2.45 s | 无缩放 |
| 软编 720p→480p | mpeg4 854×480 | 1.86 s | 含 sws 缩放 |
| **硬编 720p** | **h264_rkmpp** | **1.73 s** | VPU 编码 |
| **硬编 1080p** | **h264_rkmpp** | **3.91 s** | 720p 源放大到 1080p |
| **硬编 HEVC 720p** | **hevc_rkmpp** | **1.75 s** | VPU 编码 |
| `ctest` | — | 2.05 s | 7/7 通过 |
| ROS 节点发布 | `CompressedImage` @30 Hz | 177 帧 / 5.878 s | `ros2 bag` 实测计数 |

读法：源是 **mpeg4 软件解码**，所以端到端时间里解码和缩放占比不小；纯编码吞吐比表格更好。
RK3566 的 VPU 规格是 H.264/H.265 解码 4K@60、编码 1080p@60，硬件编码才是这块板的正解。

---

## 11. 测试与 CI

`ctest` 共 7 个可执行用例：

| 用例 | 覆盖 |
|---|---|
| `config` | INI 解析、`parse_size` / `parse_bit_rate`、错误行号 |
| `media` | `Frame` / `Packet` 生命周期、`clone()` 对空帧的回退 |
| `pipeline` | 端到端转码、多流交织、参数校验 |
| `filter` | 滤镜改时间基和尺寸、首帧探测 |
| `audio` | 重采样、切片补齐、`atempo` |
| `encode` | 编码器打开与失败路径（含硬件缺失时优雅失败） |
| `av` | 音视频同步、包数一致性 |

CI（`.github/workflows/ci.yml`）：`ubuntu-24.04` × `{gcc, clang}`，跑构建 + `ctest` + 一整套 CLI 冒烟
（gen / probe / transcode / bench、`fps=15` 帧数校验、纯音频、`atempo` 时长减半、音视频混流）。

ROS 层另有 `scripts/ros_smoke.sh`：自己生成素材，起节点，等 `active`，等 `CompressedImage`。

---

## 12. 已知限制与路线图

**已知限制**

1. **解码仍是软件**：`Decoder` 走 `avcodec_find_decoder()`；`ffmpeg-rockchip` 的 `h264_rkmpp` 解码器未接线，4K 源会先卡在软解。
2. **rkmpp 不传 `preset` / `crf`**：用 RKMPP 自己的码控（默认 CBR + `bit_rate`）；要 VBR 需补 `rc_mode` 映射。
3. **vaapi / cuda 未真机验证**：上传路径已实现，但手上没有对应 GPU 的机器。
4. **单线程 stage 图**：一条管线一个线程；多路并发请起多进程（`erde_ros` 一个节点一条管线）。
5. **没有音频硬编**：RK3566 无音频编码硬件，AAC / MP3 始终软件编码。
6. **配置文件无 schema 校验**：未知键静默忽略（见 `docs/DESIGN.md` 第 6 节）。
7. **本身不做帧率转换**：`--fps` 与源差超过 0.5 且没有 `fps=` 滤镜时直接返回 `unsupported` 并说明原因，因为“元数据说 24fps、内容是 30fps”比报错更糟。

**路线图**

- 解码侧接 `h264_rkmpp` / `hevc_rkmpp`（真零拷贝需要 DRM_PRIME 帧，配合 RGA 做缩放）；
- `--hw rkmpp` 暴露 `rc_mode` / `quality` 等 MPP 码控参数；
- 把 `--hw` 提升为一等参数（目前是 `EncodeOptions` 的一个字段）；
- 多实例调度与零拷贝管线（DMABUF 传递）。

---

## 13. 致谢与许可

- FFmpeg 及其 `libav*`；Rockchip 的 MPP / RGA；
- [nyanmisaka/ffmpeg-rockchip](https://github.com/nyanmisaka/ffmpeg-rockchip)、[nyanmisaka/mpp](https://github.com/nyanmisaka/mpp) 提供了 Rockchip 平台可用的编解码栈；
- 板卡侧编译与实测环境：野火 LubanCat-1（RK3566）；
- 许可：[MIT](LICENSE)。
