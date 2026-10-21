# 设计说明

这份文档记录 `Erde` 里几个"本可以另一种写法"的决定，以及做这些决定时踩到的实际问题。
面试里能被追问的正是这些地方。

## 1. 数据流：为什么 stage 是拉取式

管线的形状：

```
Demuxer --packet--> Decoder --frame--> [ScalingSource --frame-->] Encoder --packet--> Muxer
        (PacketSource)        (FrameSource)                (FrameSource)   (PacketSource)
```

三个接口，各一个方法：

```cpp
class PacketSource { virtual Expected<Packet> pull() = 0; };
class FrameSource  { virtual Expected<Frame>  pull() = 0; };
```

`pull()` 的三态语义是关键：

| 返回 | 含义 |
|---|---|
| 值 | 有一个元素 |
| `Code::need_more` | 现在没有，但没结束（例如解码器要更多输入） |
| `Code::end_of_stream` | 永远没有了 |

如果用 `optional` 或 `nullptr` 表示"没有"，就必须再加一个 `finished()` 查询，而且每个调用点
都要记得查——这是错漏的温床。把"暂时没有"和"永远没有"放进类型里，循环就退化成
`for(;;) { auto x = s.pull(); if (!x) { if (eof) break; else continue; } }`。

### 与"推"模式的差别

推送模式（`sink.push(frame)` + 内部队列）需要在每个交接点决定队列满了怎么办。真正危险的
不是丢帧本身，而是**丢帧的决定被埋在一个 `while` 里**：上游（`ffmpeg_pipeline` 的
`EncoderFilter`）就有这样的循环——每拍只保留 `receive` 出来的最后一个元素，注释里假设
"一个 packet 只出一个 frame"。对 1:1 的编解码成立，一旦有 B 帧重排或 `AV_CODEC_CAP_DELAY`
就不成立。

`Erde` 里没有"保留最后一个"的决策点：`Decoder::pull()` 一次只返回一帧，其余留在解码器
里，下一拍继续取。`Encoder::push()` 在 `EAGAIN` 时排空内部队列再重试，也不丢。

## 2. 错误处理：不用异常，但也不返回裸 `int`

libav 全线返回 `int`（0 / 负的 `AVERROR`）。直接用会导致两个问题：错误被忽略，或者错误
信息丢失（`-22` 是什么？）。

```cpp
struct Error { Code code; int av_code; std::string message; };
```

- `Code` 是粗分类，调用方按它分支；`av_code` 保留原始值，`to_string()` 用 `av_strerror`
  翻成可读文本，形如 `ffmpeg[-2]: cannot open '/tmp/x.mp4': No such file or directory`。
- 返回值用 `Expected<T>`（C++17 没有 `std::expected`，所以自己写了 40 行）。`take()` 显式
  移出，避免"读一个已经搬走的 Frame"这种低级错误。
- 异常只在构造/分配失败这种"程序没法继续"的地方用（实际上也没用：`Frame::create()` 也返回
  `Expected`）。

选择理由：媒体处理的错误绝大多数是可恢复/可上报的（文件坏了、编码器不支持、时间戳乱序），
用异常会让控制流藏在 `try` 块里；而 `Expected` 强制每个调用点写清楚失败怎么办。

## 3. RAII：两种 deleter 签名

libav 释放对象有两套约定：

- `void av_frame_free(AVFrame **)`——绝大多数
- `void sws_freeContext(SwsContext *)`——`SwsContext` 等少数

写两个 `Ptr` 太啰嗦，所以 deleter 做成 policy，一个 `release(T*&)` 成员：

```cpp
template <class T, void (*Fn)(T**)> struct free_via_pointer_to_pointer { static void release(T*& p); };
template <class T, void (*Fn)(T*)>  struct free_via_pointer            { static void release(T*& p); };

template <class T, class Deleter> class Ptr { /* move-only, .get(), .out(), .reset() */ };
```

`out()` 暴露 `T**` 供分配用（`av_frame_alloc()` 这种返回指针的用不上，但
`avformat_open_input(&ctx, ...)` 需要）。

## 4. 时间基：一个真实的 bug

第一版把编码器的 `time_base` 设成源流的 `time_base`，理由是"只有它才能精确表示源 pts"。
结果 AVI 输出坏掉：AVI 的 mpeg4 流要 `1/600`，从 `1/15360` 换算过去是 25.6 倍，非整数。

改成 `1/fps`（和 ffmpeg CLI 一样）之后 AVI 正常了，但暴露出第二个问题：
源是 mp4（`1/15360`，30 fps），用户传 `--fps 24`，把 `0,512,1024...` 缩放到 `1/24` 得到
`0,1,1,2...`——重复 pts。libx264 直接拒绝：

```
[mpeg4 @ ...] Invalid pts (2) <= last (2)
erde: ffmpeg[-1]: avcodec_send_frame: Operation not permitted
```

真正的结论不是"调个参数"，而是**这个管线没有 filter 图，就不该声称能做帧率转换**。所以现在：

- `transcode` 默认用源帧率（从 `codecpar->framerate` 读）；
- 显式指定的 `--fps` 与源差超过 0.5 时，直接返回 `unsupported` 并说明原因；
- `Encoder::push()` 仍然保留 pts 重缩放（源 tb → 编码器 tb），因为两者一般不等。

另一个时间基 bug 在同一个地方：不去 rescale 的话，2 秒的片子写出来是 1006.97 秒——因为
`pts` 是 `1/15360` 单位，编码器按 `1/30` 解释，正好差 512 倍。

## 5. `av_frame_clone` 的坑

`av_frame_clone()` 在 FFmpeg 6.1 上对**没有 buffer 的 frame 返回 NULL**（它内部是
`av_frame_alloc` + `av_frame_ref`，而 `av_frame_ref` 拒绝无数据的帧）。20 行 C 程序验证过：

```
alloc=0x...  clone of empty frame=(nil)  clone of buffered frame=0x...
```

所以 `Frame::clone()` 在 `av_frame_clone` 失败时回退到 `av_frame_alloc` +
`av_frame_copy_props`。这个分支平时不触发（解码出来的帧都有 buffer），但写测试时第一下就撞上了。

## 6. 配置格式

手写 INI 解析而不是引 yaml-cpp / nlohmann-json：

- 管线配置是扁平的键值对，没有嵌套需求；
- 依赖越少，别人 clone 下来越快能跑（`pkg-config` + 编译器就够）；
- 手写解析器能给出带行号的错误，这对"配置文件写错了"这种最高频的失败最有用。

代价是没有 schema 校验，未知键会被静默忽略——这是已知取舍，写在 README 的限制里。

## 7. 滤镜图：两处必须先"看一眼再建"

libavfilter 的 `buffer`/`abuffer` 需要输入格式（宽高、像素格式、时间基，或采样率、声道布局、
采样格式），而这些只有拿到第一帧才知道。所以 `FilterSource::open()` 先向上游要一帧，用它的
属性建图，这一帧留到第一次 `pull()` 再喂进去。

第二处更隐蔽：**滤镜会改时间基**。`fps=15` 让图输出 1/15 网格的帧，但 `TranscodeRequest`
里带的是**源文件**的时间基 1/30。编码器若按 1/30 → 1/15 再缩放一次，`0,1,2…` 会变成
`0,0,1,1…`，libx264 直接拒绝（`Invalid pts (1) <= last (1)`）。

修法不是"别缩放"，而是问图要它的输出时间基：

```cpp
out_time_base_ = av_buffersink_get_time_base(buffer_sink_);
```

同理，`crop=320:240` 会改输出尺寸，编码器若还按源的 640×480 打开就对不上。所以视频链路在
滤镜之后先取一帧，用它的宽高/时间基去构造编码器参数，这一帧在循环开始前先送进去。

## 8. 音频：三个 stage，各有各的麻烦

`demux → decode → AudioResampler → AudioSlicer → encode → mux`

- **AudioResampler**（libswresample）：源采样率/声道/格式同样要等第一帧才知道，所以
  `swr_alloc_set_opts2` + `swr_init` 放在第一帧到达时；帧已经符合目标格式时直接透传，不建
  SwrContext。EOF 时还要 `swr_convert(..., nullptr, 0)` 把重采样器内部的延迟样本倒出来。
- **AudioSlicer**（`AVAudioFifo`）：AAC 只接受每帧 1024 个样本，Opus 960。源帧大小随机，
  所以必须缓冲重组，**这也是不能像上游 `EncoderFilter` 那样"每拍只留最后一帧"的地方**——
  那会直接丢样本。最后一帧不足 1024 时用静音补齐并 `memset` 清零（否则补的是未初始化内存）。
- **编码器顺序**：必须先开编码器，才知道 `frame_size` / 采样格式 / 声道数，再据此建
  resampler 和 slicer。这个"先开编码器再建前级"的顺序和视频链路正好相反，容易写反。

## 9. 硬件编码：上传路径与失败路径

`--hw vaapi` 做三件事：

1. `av_hwdevice_ctx_create()` 打开设备（默认 `/dev/dri/renderD128`）；
2. VAAPI 需要显式的帧池：`av_hwframe_ctx_alloc` + `AVHWFramesContext{format=VAAPI,
   sw_format=NV12, width, height}` + `av_hwframe_ctx_init`，再挂到 `ctx->hw_frames_ctx`；
3. 每帧 `av_hwframe_get_buffer` 拿设备表面，`av_hwframe_transfer_data` 从系统内存上传，
   然后把这张表面送进编码器——硬件编码器不接受普通 `AVFrame`。

**没验证的部分要说清楚**：本机 WSL 没有 `/dev/dri`，所以只跑通了失败分支。测试
`hardware_encoder_fails_gracefully_without_a_device` 断言的是"要么成功且 `is_hardware()`
为真，要么失败且错误码与消息都合理"——两种机器上都能过，但只有有 GPU 的机器才会走第一条。

还有一个顺序坑：先校验设备名再解析编码器名。反过来的话 `hw=nosuchdev` 会报
`encoder 'libx264_nosuchdev' is not available`，把人往错误的编码器问题上引。

## 10. 多流混流：两条链、一个 muxer

`Muxer` 改成显式生命周期，因为容器头只能在所有流注册完之后写一次：

```cpp
open() -> add_stream(info) x N -> start() -> write(packet, stream_index) ... -> close()
```

`add_stream()` 记住每个流的 **编码器时间基**（`1/fps` 或 `1/sample_rate`），`write()` 时再把它
缩放成该流在容器里的时间基。MP4 会把流时间基改成 `1/15360`、AVI 改成 `1/600`，所以缩放必须
按流做，不能全局一个。

两条链各自持有一个 `Demuxer`：一个 demuxer 只服务一路选中的流，而视频 30 fps、音频 43 Hz 的
节拍不同，共享一个 reader 会互相阻塞。两个 `AVFormatContext` 打开同一文件很便宜。

拉取顺序按时间轴走：

```cpp
const bool take_video = have_video && (!have_audio || video.last_time <= audio.last_time);
```

这样送给 muxer 的包本来就有序，`av_interleaved_write_frame()` 只需要处理有限的重排。
两条链都结束后各自 `flush()`，把编码器里剩余的包倒出来再写 trailer。

第一版在音频缺失时会 **段错误**：`flush_chain()` 直接调 `chain.encoder->flush()`，而纯视频
文件里音频链的 encoder 是 `nullptr`。现在 `flush_chain()` 和 `write_ready_packets()` 都先判空。

## 11. 一个 `Expected` 与 `Status` 混用的坑（导致了空错误信息）

`Status` 就是 `Error`（`code == ok` 表示成功），而 `Expected<T>` 用同一个 `Error` 表示失败。
于是这个写法是错的：

```cpp
if (auto drained = drain_locked(...); drained) {   // 有错？
  ...                                               // 走这里
} else {
  return drained;      // 把 "成功(ok)" 当成 Expected<T> 的错误返回
}
```

返回的 `Expected<T>` 没有值，`error().code` 却是 0，调用方于是报告一个 **消息为空的失败**：

```
erde:  [code=0]
```

修法是不要在 `Expected<T>` 的返回路径上返回 `Status`，而是在 `Status` 上有错时才提前返回：

```cpp
if (auto drained = drain_locked(...); drained) {
  return drained;                 // 真错误
}
if (!ready_.empty()) { ... }      // 正常路径
```

同一类错误在 `Encoder::send_frame()` 的 `EAGAIN` 分支里也有一份（`if (drained) continue; else
return drained;` —— 逻辑正好反了），一并改掉。这个 bug 只在"音频链路第一次 pull 到空"时才
触发，所以是加了音频之后才暴露出来的。

## 12. 测试策略

七个测试程序，都不需要外部素材：

| 测试 | 覆盖 |
|---|---|
| `test_config` | INI 解析、错误分支、size/bitrate 解析、缺失键 |
| `test_media` | `Expected` 语义、`Ptr` 移动、`Frame` 分配/克隆、错误文本 |
| `test_pipeline` | `gen` → `probe` → 带缩放的 `transcode` → `bench`，含帧数守恒与失败路径 |
| `test_filter` | `fps=15` 帧数减半、`crop` 改尺寸、非法滤镜描述被拒绝 |
| `test_audio` | 正弦音生成、48k 立体声 → 44.1k 单声道、纯视频文件被拒绝 |
| `test_encode` | 软件编码器、音频编码器参数、未知编码器、硬件编码的成功/失败两分支 |
| `test_av` | 双流生成/转码、`--no-audio` 丢轨、`atempo` 改变时长、非法音频滤镜 |

端到端测试用 `mpeg4` 而不是 `libx264`：`mpeg4` 是 libavcodec 内置的，装不装 x264 都能跑，
测试不依赖宿主环境；音频用例同理只用 `aac`（libavcodec 内置）。

测试框架是自己写的 60 行 `tests/harness.hpp`（注册表 + `CHECK`/`CHECK_EQ` + `main`），
理由同上：`ctest` 应该在任何一台装了编译器和 libav 的机器上直接可用。
