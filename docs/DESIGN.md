# 设计说明

这份文档记录 `avpipe` 里几个"本可以另一种写法"的决定，以及做这些决定时踩到的实际问题。
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

`avpipe` 里没有"保留最后一个"的决策点：`Decoder::pull()` 一次只返回一帧，其余留在解码器
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
avpipe: ffmpeg[-1]: avcodec_send_frame: Operation not permitted
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

## 7. 测试策略

三个测试程序，都不需要外部素材：

| 测试 | 覆盖 |
|---|---|
| `test_config` | INI 解析、错误分支、size/bitrate 解析、缺失键 |
| `test_media` | `Expected` 语义、`Ptr` 移动、`Frame` 分配/克隆、错误文本 |
| `test_pipeline` | `gen` → `probe` → 带缩放的 `transcode` → `bench`，含帧数守恒与失败路径 |

端到端测试用 `mpeg4` 而不是 `libx264`：`mpeg4` 是 libavcodec 内置的，装不装 x264 都能跑，
测试不依赖宿主环境。

测试框架是自己写的 60 行 `tests/harness.hpp`（注册表 + `CHECK`/`CHECK_EQ` + `main`），
理由同上：`ctest` 应该在任何一台装了编译器和 libav 的机器上直接可用。
