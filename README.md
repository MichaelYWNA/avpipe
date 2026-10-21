# avpipe

基于 libav\* 的**拉取式**音视频转码管线：解复用 → 解码 → （可选）libavfilter → （可选）缩放 →
编码 → 复用，全部 C++17，不依赖 ROS、不依赖任何第三方框架，只用 pkg-config 找到
`libavformat` / `libavcodec` / `libavutil` / `libswscale` / `libswresample` / `libavfilter`。

自带合成片源与合成音源，所以不需要任何素材文件就能把整条链路跑通并测出数字。

---

## 状态

| 项目 | 结果 |
|---|---|
| 构建 | CMake + GCC 13，`-Wall -Wextra -Wpedantic` 零警告 |
| 测试 | 6 个测试程序、28 个用例，`ctest` 全绿 |
| 视频守恒 | 120 帧进 → 120 帧出（`--filter fps=15` 时 120 → 60，时长仍是 4 s） |
| 音频链路 | 10 s AAC 48 kHz 立体声 → 44.1 kHz 单声道，**约 48 ms**（≈ 208× 实时） |
| 时间轴 | 4.000 s 源 → 4.000 s 出，时间基转换不漂移 |
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
| 视频 | 解复用 / 解码 / 缩放 / 编码 / 复用 | 软件编码：libx264、libx265、mpeg4、mjpeg… |
| 滤镜 | `--filter "fps=15,crop=640:360"` 任意 libavfilter 链 | 帧率转换只有在这里才成立 |
| 音频 | 重采样（swresample）+ 定长切帧（`AVAudioFifo`）+ 编码 | `transcode-audio`，AAC / mp3 / opus… |
| 硬件编码 | `--hw vaapi` / `--hw cuda` + 系统内存到设备帧的上传 | 本机无 `/dev/dri`，只验证了失败路径（见限制） |

### 为什么需要 filter 阶段

`--size` 只能改分辨率，不能改帧率：丢帧/补帧是 filter 的事。`fps=15` 插进去之后，
120 帧 30 fps 的 4 秒片子变成 60 帧 15 fps，**时长不变**——

```
$ avpipe transcode 720p.mp4 out.mp4 --filter 'fps=15' --fps 15
packets read=120 frames decoded=120 frames encoded=60 packets written=60
$ avpipe probe out.mp4
format=mov,mp4,... duration=4s packets=60
  video(index=0, codec=h264, 1280x720, fps=15, tb=1/15360) packets=60
```

不写 `--filter` 而直接 `--fps 24` 会明确报错，而不是产出一个时间轴撒谎的文件。

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

视频链路（不需要素材）：

```bash
./build/avpipe gen /tmp/src.mp4 --size 1280x720 --fps 30 --frames 120 --crf 23
./build/avpipe probe /tmp/src.mp4
./build/avpipe transcode /tmp/src.mp4 /tmp/out.avi --size 640x480 --codec mpeg4
./build/avpipe bench /tmp/src.mp4 /tmp/out.avi --size 640x480 --codec mpeg4 --runs 5
```

音频链路（同样不需要素材）：

```bash
./build/avpipe gen /tmp/tone.m4a --audio --seconds 10 --rate 48000 --channels 2
./build/avpipe transcode-audio /tmp/tone.m4a /tmp/tone-out.m4a --rate 44100 --channels 1 --bitrate 64k
./build/avpipe probe /tmp/tone-out.m4a
#   audio(index=0, codec=aac, 44100 Hz, tb=1/44100) packets=45
```

滤镜与硬件：

```bash
./build/avpipe transcode in.mp4 out.mp4 --filter 'fps=15,crop=640:360' --fps 15
./build/avpipe transcode in.mp4 out.mp4 --hw vaapi --hw-device /dev/dri/renderD128
```

## 命令

| 命令 | 作用 |
|---|---|
| `avpipe probe <file>` | 容器、每路流的编码/分辨率/帧率/采样率/时间基、包数、大小 |
| `avpipe gen <out>` | 合成测试片；`--audio` 生成正弦音 |
| `avpipe transcode <in> <out>` | 视频链路；`--size` 插缩放，`--filter` 插滤镜 |
| `avpipe transcode-audio <in> <out>` | 音频链路；`--rate` / `--channels` 重采样 |
| `avpipe bench <in> <out>` | 重复转码并打印 fps |

通用选项：`--codec` / `--bitrate` / `--preset` / `--crf` / `--gop` / `--fps` / `--size` /
`--filter` / `--frames` / `--audio` / `--seconds` / `--rate` / `--channels` / `--hw` /
`--hw-device` / `--config` / `--quiet` / `--verbose`。

## 配置

```ini
[video]
size   = 640x480
fps    = 30          # gen 用；transcode 时与源不符会被拒绝，除非 filter 里有 fps=
filter = fps=15      # 任意 libavfilter 链

[audio]
rate     = 44100
channels = 1

[encode]
codec   = mpeg4      # 或 libx264 / libx265 / aac / libmp3lame
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
tests/             极简测试框架 + 6 个测试程序（config/media/pipeline/filter/audio/encode）
docs/DESIGN.md     设计取舍：拉取模型、时间基、滤镜、音频、硬件上传、测试策略
```

## 已知限制

1. **音视频不混流**：`transcode` 只处理视频流，`transcode-audio` 只处理音频流；一个输出文件里
   同时有音视频需要多流 muxer，尚未实现。
2. **音频末尾会补齐**：切片器把最后一帧用静音补到编码器帧长（AAC 1024 样本），所以 1.000 s
   的音源转出来是 1.002 s。要精确就得允许编码器接受短帧或做 gapless 元数据。
3. **硬件编码未在真机验证**：本机 WSL 没有 `/dev/dri`，只验证了"设备缺失时报错清晰、不崩溃"。
   VAAPI 的设备/帧池初始化与 `av_hwframe_transfer_data` 上传路径是按 FFmpeg 6.1 文档写的，
   有 GPU 的机器上需要再跑一次。
4. **29.97 这类非整数帧率**：编码器时间基取 `1/fps`（四舍五入），长时间转码会有毫秒级漂移。
5. **单流、单线程**：多路流、线程池、零拷贝跨进程不在范围内。
6. **音频没有滤镜链**：`--filter` 目前只接到视频链路。

## 许可证

MIT，见 [LICENSE](LICENSE)。
