# avpipe

基于 libav\* 的**拉取式**音视频转码管线：解复用 → 解码 → （可选）缩放 → 编码 → 复用，全部用
C++17 写成，不依赖 ROS、不依赖任何第三方框架，只用 pkg-config 找到 `libavformat` /
`libavcodec` / `libavutil` / `libswscale`。

自带合成片源，所以不需要任何素材文件就能把整条链路跑通并测出数字。

---

## 状态

| 项目 | 结果 |
|---|---|
| 构建 | CMake + GCC 13，`-Wall -Wextra -Wpedantic` 零警告 |
| 测试 | 3 个测试程序、18 个用例，`ctest` 全绿（0.1 s） |
| 正确性 | 120 帧进 → 120 帧出（`packets read=120 frames decoded=120 frames encoded=120 packets written=120`） |
| 时间轴 | 4.000 s 源 → 4.000 s 出，容器时间基转换不漂移 |

实测吞吐（Ubuntu 24.04 / WSL2 / 32 vCPU / FFmpeg 6.1 / Release，`avpipe bench` 取多次平均）：

| 链路 | 吞吐 |
|---|---|
| 1280×720 H.264 → 640×480 mpeg4 | **1210 fps** |
| 1280×720 H.264 → 1280×720 libx264 `veryfast` `crf 23` | **680 fps** |
| 1280×720 H.264 → 1280×720 libx265 `ultrafast` `crf 28` | **219 fps** |

复现：

```bash
avpipe gen /tmp/720p.mp4 --size 1280x720 --frames 120 --crf 23
avpipe bench /tmp/720p.mp4 /tmp/out.avi --size 640x480 --codec mpeg4 --runs 5
```

---

## 为什么是"拉取式"

常见写法是三个循环互相推：解码器把帧推进队列，编码器从队列取。代价是每个交接点都要自己
管水位、自己判断"队列满了该丢谁"，而"丢最新一帧"这种 bug 恰恰藏在里面。

这里反过来：**下游向上游要数据**。

```
Muxer.write(packet)  <-  Encoder.pull()  <-  Scaler.pull()  <-  Decoder.pull()  <-  Demuxer.pull()
```

每个 stage 只暴露 `pull()`，返回三态之一：有数据 / `need_more`（暂时没有，但没结束）/
`end_of_stream`（永远没有了）。好处：

- **不会丢帧**：编码器 `EAGAIN` 时，`Encoder::push()` 自己把内部包排空再重试，调用方不需要
  在 send/receive 之间手工穿插，也就没有"只保留最新一个"的决策点。
- **N:M 关系有地方放**：一个 packet 可能出 0 个或多个 frame（B 帧重排、编解码器 delay），
  这些状态（pending packet、draining 标志、EOF 闩锁）都在 `Decoder` 内部，不污染管线代码。
- **可组合**：`ScalingSource` 只是又一个 `FrameSource`，插在解码器和编码器中间，别处不用改。

细节和取舍写在 [docs/DESIGN.md](docs/DESIGN.md)。

---

## 快速开始

依赖（Ubuntu 24.04）：

```bash
sudo apt install -y build-essential cmake pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

构建与测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

跑一条完整链路（不需要任何素材）：

```bash
# 1) 造一段 4 秒 720p 测试片
./build/avpipe gen /tmp/src.mp4 --size 1280x720 --fps 30 --frames 120 --crf 23

# 2) 看看它到底是什么
./build/avpipe probe /tmp/src.mp4
# format=mov,mp4,m4a,3gp,3g2,mj2 duration=4s packets=120
#   video(index=0, codec=h264, 1280x720, fps=30, tb=1/15360) packets=120

# 3) 转成 480p mpeg4
./build/avpipe transcode /tmp/src.mp4 /tmp/out.avi --size 640x480 --codec mpeg4 --bitrate 1200k
# packets read=120 frames decoded=120 frames scaled=120 frames encoded=120 packets written=120 wall=103.8 ms

# 4) 测吞吐
./build/avpipe bench /tmp/src.mp4 /tmp/out.avi --size 640x480 --codec mpeg4 --runs 5
# runs=5 frames_per_run=120 total_ms=495.698 fps=1210.41
```

## 命令

| 命令 | 作用 |
|---|---|
| `avpipe probe <file>` | 打印容器、每路流的编码/分辨率/帧率/时间基、包数、文件大小 |
| `avpipe gen <out>` | 生成合成测试片（YUV420P 梯度 + 移动方块），当作可复现的测试素材 |
| `avpipe transcode <in> <out>` | 完整链路，`--size` 会插入缩放 stage |
| `avpipe bench <in> <out>` | 把转码重复 N 次并打印 fps |

通用选项：`--codec` / `--bitrate` / `--preset` / `--crf` / `--gop` / `--fps` / `--size` /
`--frames` / `--runs` / `--config` / `--quiet` / `--verbose`。`avpipe --help` 有完整说明。

## 配置

除了命令行，也可以用 INI 文件给默认值（`--config pipeline.ini`）：

```ini
[video]
size = 640x480          # 转码时插入缩放到 640x480
fps  = 30               # gen 用；transcode 时若与源不同会直接报错（见"已知限制"）

[encode]
codec   = mpeg4         # libx264 / libx265 / mpeg4 / mjpeg ...
bitrate = 1200k         # 1200k、2M、1200000 都行
preset  = veryfast      # 交给编码器本身，x264/x265 认识
crf     = 23            # 给了 crf 就不看 bitrate
gop     = 12
```

解析器是手写的（`src/config.cpp`，约 150 行），带行号报错：

```
avpipe: /path/pipeline.ini: line 4: expected 'key = value'
```

## 目录

```
include/avpipe/    公开头文件：error/expected、RAII Ptr、Frame/Packet、stage 接口、各 stage
src/               实现；cli/main.cpp 是命令行入口
tests/             自带的极简测试框架 + 3 个测试程序
docs/DESIGN.md     设计取舍：拉取模型、时间基、错误处理、为什么不用异常
```

## 已知限制

写清楚比藏着好，这些是真没做：

1. **只处理视频**。音频链路的 `AudioConverter`/`AudioFifo` 没实现，`transcode` 遇到纯音频输入
   会报 `no video stream`。
2. **不做帧率转换**。没有 filter 图，30 fps 转 24 fps 需要丢/补帧，所以 `--fps` 与源不一致时
   直接报错而不是产出时间轴撒谎的文件。
3. **单流、单线程**。多路流、线程池、零拷贝跨进程都不在范围内。
4. **29.97 这类非整数帧率**：编码器时间基取 `1/fps`（`fps` 四舍五入），长时间转码会有毫秒级
   漂移；要精确得用 `1001/30000` 之类的有理时间基，属于下一步。
5. **不支持硬件编码**。`--codec h264_vaapi` 这类名字能传，但设备初始化没做，会失败。

## 许可证

MIT，见 [LICENSE](LICENSE)。
