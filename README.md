# avpipe

基于 libav\* 的**拉取式**音视频转码管线：解复用 → 解码 → （可选）libavfilter → （可选）缩放 →
编码 → **多流混流**，全部 C++17，不依赖 ROS、不依赖任何第三方框架，只用 pkg-config 找到
`libavformat` / `libavcodec` / `libavutil` / `libswscale` / `libswresample` / `libavfilter`。

自带合成片源与合成音源，所以不需要任何素材文件就能把整条链路跑通并测出数字。

---

## 状态

| 项目 | 结果 |
|---|---|
| 构建 | CMake + GCC 13，`-Wall -Wextra -Wpedantic` 零警告 |
| 测试 | 7 个测试程序、34 个用例，`ctest` 全绿 |
| 视频守恒 | 120 帧进 → 120 帧出（`--filter fps=15` 时 120 → 60，时长仍是 4 s） |
| 音视频混流 | 2 s 的 A/V 文件转码后仍是 2.005 s、两路流各 60 / 95 个包 |
| 音频链路 | 10 s AAC 48 kHz 立体声 → 44.1 kHz 单声道，**约 48 ms**（≈ 208× 实时） |
| 音频滤镜 | `atempo=2.0` 把 1.002 s 压到 **0.512 s** |
| CI | GitHub Actions，gcc + clang 双矩阵，含 CLI 冒烟 |

实测吞吐（Ubuntu 24.04 / WSL2 / 32 vCPU / FFmpeg 6.1 / Release，`avpipe bench` 多次平均）：

| 链路 | 吞吐 |
|---|---|
| 1280×720 → 640×480 mpeg4 | **1210 fps** |
| 1280×720 → 1280×720 libx264 `veryfast` | **786 fps** |
| 1280×720 → `fps=30,scale=640:360` → libx264 `veryfast` | **858 fps** |
| 1280×720 → 1280×720 libx265 `ultrafast` | **219 fps** |
| 10 s AAC 48k 立体声 → 44.1k mono | **≈ 48 ms**（208× 实时） |

---

## 能力

| 领域 | 已实现 | 说明 |
|---|---|---|
| 视频 | 解复用 / 解码 / 滤镜 / 缩放 / 编码 | 软件编码：libx264、libx265、mpeg4、mjpeg… |
| 音频 | 解码 / 滤镜 / 重采样 / 定长切帧 / 编码 | AAC / mp3 / opus…，`transcode-audio` 单音频入口 |
| 混流 | 多流 muxer：视频 + 音频写进同一个文件 | `av_interleaved_write_frame` 负责实际交织 |
| 硬件编码 | `--hw vaapi` / `--hw cuda` + 设备帧上传 | 本机无 `/dev/dri`，只验证了失败路径（见限制） |

### 两条链、一个 muxer

```
video: demux → decode → [filter] → [scale] → encode ─┐
                                                     ├─ Muxer(add_stream ×N) → 容器
audio: demux → decode → [filter] → resample → slice → encode ─┘
```

`Muxer` 的生命周期是显式的：`open()` → `add_stream()` × N → `start()` → `write(packet, index)`
→ `close()`。容器头只能在所有流都注册之后写一次，所以不能"一边写一边加流"。

两条链各自持有一个 `Demuxer`：一个 demuxer 只能选一路流，而要按时间轴交错拉取，两路必须能
独立前进。

### 帧率转换与滤镜

`--size` 只能改分辨率。帧率转换是 filter 的事：`--filter 'fps=15'` 把 120 帧 30 fps 的 4 秒
片子变成 60 帧 15 fps，**时长不变**。不写 filter 而直接 `--fps 24` 会明确报错，而不是产出一个
时间轴撒谎的文件。音频同理：`--audio-filter 'atempo=2.0'` 把 1.002 s 压到 0.512 s。

---

## 快速开始

依赖（Ubuntu 24.04）：

```bash
sudo apt install -y build-essential cmake pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

音视频混流（不需要素材）：

```bash
# 造一个 2 秒、带音轨的 640x480 测试片
./build/avpipe gen /tmp/av.mp4 --size 640x480 --fps 30 --frames 60 --crf 23 --with-audio --seconds 2
./build/avpipe probe /tmp/av.mp4
#   video(index=0, codec=h264, 640x480, fps=30, tb=1/15360) packets=60
#   audio(index=1, codec=aac, 48000 Hz, tb=1/48000) packets=95

# 两路一起转：视频转 mpeg4 并缩放，音频保持 aac
./build/avpipe transcode /tmp/av.mp4 /tmp/out.mp4 --codec mpeg4 --size 320x240 --audio-bitrate 96k

# 只要画面
./build/avpipe transcode /tmp/av.mp4 /tmp/v.mp4 --codec mpeg4 --no-audio

# 音频滤镜
./build/avpipe transcode /tmp/av.mp4 /tmp/f.mp4 --codec mpeg4 --audio-filter 'volume=0.5'
```

音频单独处理：

```bash
./build/avpipe gen /tmp/tone.m4a --audio --seconds 10 --rate 48000 --channels 2
./build/avpipe transcode-audio /tmp/tone.m4a /tmp/tone-out.m4a --rate 44100 --channels 1
./build/avpipe transcode-audio /tmp/tone.m4a /tmp/tone-fast.m4a --audio-filter 'atempo=2.0'
```

## 命令

| 命令 | 作用 |
|---|---|
| `avpipe probe <file>` | 容器、每路流的编码/分辨率/帧率/采样率/时间基、包数、大小 |
| `avpipe gen <out>` | 合成测试片；`--audio` 纯音，`--with-audio` 画面加音轨 |
| `avpipe transcode <in> <out>` | 视频（有音轨时一并转），`--no-audio` 丢掉音频 |
| `avpipe transcode-audio <in> <out>` | 音频链路 |
| `avpipe bench <in> <out>` | 重复转码并打印 fps |

通用选项：`--codec` / `--bitrate` / `--preset` / `--crf` / `--gop` / `--fps` / `--size` /
`--filter` / `--audio-filter` / `--audio-codec` / `--audio-bitrate` / `--no-audio` /
`--frames` / `--audio` / `--with-audio` / `--seconds` / `--rate` / `--channels` /
`--hw` / `--hw-device` / `--config` / `--quiet` / `--verbose`。

## 配置

```ini
[video]
size   = 640x480
fps    = 30          # gen 用；transcode 时与源不符会被拒绝，除非 filter 里有 fps=
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

解析器手写在 `src/config.cpp`（约 150 行），带行号报错。

## 目录

```
include/avpipe/    公开头文件：error/expected、RAII Ptr、Frame/Packet、stage 接口、各 stage
src/               实现；cli/main.cpp 是命令行入口
tests/             极简测试框架 + 7 个测试程序（config/media/pipeline/filter/audio/encode/av）
docs/DESIGN.md     设计取舍：拉取模型、时间基、滤镜、音频、多流混流、硬件上传、测试策略
```

## 已知限制

1. **音频末尾会补齐**：切片器把最后一帧用静音补到编码器帧长（AAC 1024 样本），所以 1.000 s
   的音源转出来是 1.002 s。要精确就得允许编码器接受短帧或做 gapless 元数据。
2. **硬件编码未在真机验证**：本机 WSL 没有 `/dev/dri`，只验证了"设备缺失时报错清晰、不崩溃"。
3. **29.97 这类非整数帧率**：编码器时间基取 `1/fps`（四舍五入），长时间转码有毫秒级漂移。
4. **只处理单路视频 + 单路音频**：多机位、多音轨、字幕流不在范围内。
5. **单线程**：两条链交替推进，没有线程池；`bench` 测的是这条单线程链路的吞吐。

## 许可证

MIT，见 [LICENSE](LICENSE)。
