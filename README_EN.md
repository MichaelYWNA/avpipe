<div align="center">

# Erde

**A pull-based audio/video pipeline on FFmpeg — video, audio, filters and multi-stream muxing behind one CMake target.**

<p>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-2ea44f"></a>
  <a href="https://github.com/MichaelYWNA/erde/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/MichaelYWNA/erde/actions/workflows/ci.yml/badge.svg?branch=main"></a>
  <img alt="Version" src="https://img.shields.io/badge/version-0.3.0-1f6feb">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-17-00599c">
  <img alt="FFmpeg" src="https://img.shields.io/badge/FFmpeg-6.1-3c873a">
  <img alt="Tests" src="https://img.shields.io/badge/tests-34%20cases%20passing-brightgreen">
  <a href="README.md"><img alt="Language" src="https://img.shields.io/badge/language-中文%20%7C%20English-8957e5"></a>
</p>

<p>
  <a href="#3-quick-start">Quick start</a>
  · <a href="#4-build-and-dependencies">Build</a>
  · <a href="#5-command-reference">Commands</a>
  · <a href="#6-architecture">Architecture</a>
  · <a href="#7-tests-and-measurements">Measurements</a>
  · <a href="#8-known-limitations">Limitations</a>
  · <a href="#9-contributing">Contributing</a>
  · <a href="README.md">中文</a>
</p>

</div>

---

`erde` splits FFmpeg into composable **stages** — demux, decode, filter, scale, encode, mux — and
every stage exposes a single `pull()` that the downstream stage calls. No ROS, no third-party
framework: libav\* via pkg-config, one `cmake` invocation, one command-line binary.

> The reasoning behind the design, the bugs it ran into, and where every number comes from live in
> [docs/DESIGN.md](docs/DESIGN.md). This page only answers what it is, what it does, and how to run it.

## Table of contents

- [1. What problem it solves](#1-what-problem-it-solves)
- [2. Capabilities](#2-capabilities)
- [3. Quick start](#3-quick-start)
- [4. Build and dependencies](#4-build-and-dependencies)
- [5. Command reference](#5-command-reference)
- [6. Architecture](#6-architecture)
- [7. Tests and measurements](#7-tests-and-measurements)
- [8. Known limitations](#8-known-limitations)
- [9. Contributing](#9-contributing)
- [10. Credits and license](#10-credits-and-license)

## 1. What problem it solves

A typical transcoder pushes frames: the decoder fills a queue, the encoder drains it. Every hand-off
then needs its own watermark logic and a decision about what to drop when the queue is full — and
"keep only the newest element" is exactly where frames and audio samples disappear silently.

`erde` pulls instead:

```cpp
Expected<Frame> Decoder::pull();   // value / need_more / end_of_stream
```

A three-state `pull()` makes "nothing right now" and "nothing forever" distinguishable, so:

- there is no "keep only the latest" decision point at a stage boundary;
- decoder delay, encoder `EAGAIN` and audio frame-size buffering stay inside the stage that owns them;
- inserting a filter or a scaler means swapping a `FrameSource*`, nothing else changes.

## 2. Capabilities

| Area | Implemented | Notes |
|---|---|---|
| Video | demux / decode / filter / scale / encode | libx264, libx265, mpeg4, mjpeg, … |
| Audio | decode / filter / resample / fixed-size slicing / encode | AAC, mp3, opus (`AudioSlicer` aligns frame size) |
| Filters | `--filter`, `--audio-filter` accept any libavfilter chain | `fps=15`, `crop=…`, `atempo=…`, `volume=…` |
| Muxing | several streams into one container | interleaving handled by `av_interleaved_write_frame` |
| Hardware | `--hw vaapi` / `--hw cuda` | upload path implemented; not exercised without a GPU (see [limitations](#8-known-limitations)) |
| ROS 2 | `erde_ros` lifecycle node | node owns the stage graph, publishes `CompressedImage`, exposes diagnostics and a stats service (see [6.5](#65-ros-2-layer-erde_ros)) |

## 3. Quick start

No media file is needed — the tool can generate its own input:

```bash
# 1) a 2 second 640x480 clip with an audio track
./build/erde gen /tmp/av.mp4 --size 640x480 --fps 30 --frames 60 --crf 23 --with-audio --seconds 2

# 2) look inside
./build/erde probe /tmp/av.mp4
#   video(index=0, codec=h264, 640x480, fps=30, tb=1/15360) packets=60
#   audio(index=1, codec=aac, 48000 Hz, tb=1/48000) packets=95

# 3) transcode both streams: video to mpeg4 and 320x240, audio stays aac
./build/erde transcode /tmp/av.mp4 /tmp/out.mp4 --codec mpeg4 --size 320x240 --audio-bitrate 96k

# 4) measure the chain
./build/erde bench /tmp/av.mp4 /tmp/bench.mp4 --codec mpeg4 --size 320x240 --runs 5
```

Common tasks:

| Goal | Command |
|---|---|
| Resize | `erde transcode in.mp4 out.mp4 --size 1280x720` |
| Change codec | `erde transcode in.mp4 out.mp4 --codec libx265 --crf 28 --preset ultrafast` |
| Lower the frame rate | `erde transcode in.mp4 out.mp4 --filter 'fps=15' --fps 15` |
| Crop | `erde transcode in.mp4 out.mp4 --filter 'crop=640:360:0:0'` |
| Drop audio | `erde transcode in.mp4 out.mp4 --no-audio` |
| Audio speed / gain | `erde transcode-audio in.m4a out.m4a --audio-filter 'atempo=2.0'` |
| Resample only | `erde transcode-audio in.m4a out.m4a --rate 44100 --channels 1` |
| Inspect a container | `erde probe in.mp4` |
| Measure throughput | `erde bench in.mp4 out.mp4 --runs 10` |

## 4. Build and dependencies

Only libav\* (six libraries) and CMake ≥ 3.16; GCC 13 and Clang both work:

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

The only artefact is `build/erde`. On this machine (32 vCPU) a clean build takes **11 s** and
`ctest` finishes all cases in **0.5 s**.

## 5. Command reference

### 5.1 `probe`

```bash
./build/erde probe in.mp4
# format=mov,mp4,m4a,3gp,3g2,mj2 duration=2.005s packets=155
#   video(index=0, codec=h264, 640x480, fps=30, tb=1/15360) packets=60
#   audio(index=1, codec=aac, 48000 Hz, tb=1/48000) packets=95
# size=40322 bytes
```

### 5.2 `gen`

| Flag | Effect |
|---|---|
| `--size WxH` / `--fps` / `--frames` | video: gradient plus a moving block |
| `--audio` | tone only (audio file) |
| `--with-audio` | video plus a tone track in one file |
| `--seconds` | tone duration |

### 5.3 `transcode`

| Flag | Effect |
|---|---|
| `--codec` / `--bitrate` / `--crf` / `--preset` / `--gop` | video encoder |
| `--size WxH` | inserts the scaler |
| `--filter <desc>` | inserts a libavfilter chain (before the scaler) |
| `--fps N` | asserts the rate; a mismatch without `fps=` in the filter is an error |
| `--no-audio` | drops the audio stream |
| `--audio-codec` / `--audio-bitrate` / `--audio-filter` | audio stream |
| `--hw` / `--hw-device` | hardware encoding |

### 5.4 `transcode-audio`

```bash
./build/erde transcode-audio in.m4a out.m4a --rate 44100 --channels 1 --bitrate 64k \
  --audio-filter 'atempo=1.25'
```

decode → filter → `swresample` → `AVAudioFifo` slicing → encode → mux. Slicing is mandatory: AAC
only accepts 1024 samples per frame, Opus 960.

### 5.5 `bench`

```bash
./build/erde bench in.mp4 out.mp4 --codec libx264 --preset veryfast --crf 23 --runs 5
# runs=5 frames_per_run=120 total_ms=763.221 fps=786.142
```

### 5.6 Configuration file

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

The parser is hand-written in `src/config.cpp` and reports line numbers:
`erde: pipeline.ini: line 4: expected 'key = value'`.

## 6. Architecture

### 6.1 Data flow

```text
video: demux → decode → [filter] → [scale] → encode ─┐
                                                     ├─ Muxer(add_stream ×N) → container
audio: demux → decode → [filter] → resample → slice → encode ─┘
```

Each branch owns its own `Demuxer` (one demuxer serves one selected stream). Pulls are ordered by
timestamp; `av_interleaved_write_frame()` does the actual interleaving.

### 6.2 Three interfaces

| Interface | Method | Implemented by |
|---|---|---|
| `PacketSource` | `Expected<Packet> pull()` | `Demuxer`, `Encoder` |
| `FrameSource` | `Expected<Frame> pull()` | `Decoder`, `FilterSource`, `ScalingSource`, `AudioResampler`, `AudioSlicer`, `ToneSource` |
| `Muxer` | `add_stream()` / `start()` / `write()` / `close()` | container output |

### 6.3 Layout

```text
include/erde/       public headers: error/expected, RAII Ptr, Frame/Packet, stage interfaces
src/                implementations; src/cli/main.cpp is the entry point
tests/              a 60-line harness plus 7 test programs
docs/DESIGN.md      design trade-offs and the bugs behind them
```

### 6.4 Key decisions

| Decision | Why |
|---|---|
| Three-state `pull()` | "nothing yet" and "nothing ever" must be distinguishable in the type |
| `Expected<T>` instead of exceptions | media errors are recoverable and reportable; exceptions hide control flow |
| RAII `Ptr` with deleter policies | libav frees through both `f(T**)` and `f(T*)` (`sws_freeContext`) |
| Encoder time base `1/fps` | containers want 1/600 (AVI) or 1/15360 (MP4); scaling from a coarse base stays exact |
| Re-read geometry/time base after a filter | `crop` changes size, `fps` changes the time base; the encoder cannot reuse the source values |

### 6.5 ROS 2 layer: `erde_ros`

`erde_core` is a plain C++ library; `erde_ros` is a lifecycle node on top of it - **not** a chain
of `ros2_control` controller plugins. The node owns the pipeline object graph: `on_configure`
builds the chain, `on_activate` starts the timer, `on_deactivate` flushes and stops it. The media
stages stay ordinary classes that unit tests can drive directly.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake --install build --prefix /opt/erde

source /opt/ros/jazzy/setup.bash
export CMAKE_PREFIX_PATH=/opt/erde:$CMAKE_PREFIX_PATH
colcon build --base-paths erde_ros

source install/setup.bash
ros2 launch erde_ros erde.launch.py input:=/tmp/av.mp4
```

| Interface | Meaning |
|---|---|
| topic `~/image/compressed` | `sensor_msgs/CompressedImage`, `format` is the encoder name |
| service `~/stats` | `std_srvs/Trigger` returning `pulled / published / errors / packets` |
| diagnostics | once per second: codec, frame counters; WARN when idle or finished |
| lifecycle | `configure → activate → deactivate → cleanup` |

Measured on this machine: node `active`, `pulled=298 published=278 errors=0` in 10 s; the gap is
x264 lookahead, not dropped frames.

## 7. Tests and measurements

### 7.1 Test index

| Program | Cases | Coverage |
|---|---|---|
| `test_config` | 7 | INI parsing, error paths, size/bitrate parsing, missing keys |
| `test_media` | 6 | `Expected` semantics, `Ptr` moves, `Frame` alloc/clone, error text |
| `test_pipeline` | 5 | `gen → probe → transcode → bench`, frame conservation, failure paths |
| `test_filter` | 3 | `fps=15` halves the frames, `crop` resizes, bad filter rejected |
| `test_audio` | 3 | tone generation, 48k→44.1k mono, video-only input rejected |
| `test_encode` | 4 | software/audio encoder setup, unknown encoder, both hardware branches |
| `test_av` | 6 | dual-stream generate/transcode, `--no-audio`, `atempo` timeline, bad audio filter |

**7 programs / 34 cases**, all green locally and on GitHub Actions (gcc + clang matrix).

### 7.2 Measurements

Ubuntu 24.04 / WSL2 / 32 vCPU / FFmpeg 6.1 / Release, averaged over several `bench` runs.

| Chain | Result |
|---|---|
| 1280×720 → 640×480 mpeg4 | **1210 fps** |
| 1280×720 → 1280×720 libx264 `veryfast` | **786 fps** |
| 1280×720 → `fps=30,scale=640:360` → libx264 | **858 fps** |
| 1280×720 → 1280×720 libx265 `ultrafast` | **219 fps** |
| 10 s AAC 48k stereo → 44.1k mono | **≈ 48 ms** (208× realtime) |
| 2 s A/V file → transcode | 2.005 s, 60 video + 95 audio packets |
| 1.002 s audio through `atempo=2.0` | 0.512 s |

### 7.3 Reproduce

```bash
./build/erde gen /tmp/720.mp4 --size 1280x720 --frames 120 --crf 23
./build/erde bench /tmp/720.mp4 /tmp/out.avi --size 640x480 --codec mpeg4 --runs 5
./build/erde bench /tmp/720.mp4 /tmp/out.mp4 --codec libx264 --preset veryfast --crf 23 --runs 5
./build/erde gen /tmp/tone.m4a --audio --seconds 10
./build/erde transcode-audio /tmp/tone.m4a /tmp/tone-out.m4a --rate 44100 --channels 1
```

These numbers come from one machine; different hardware will differ. The commands are all here so
they can be re-run rather than trusted.

## 8. Known limitations

1. **Hardware encoding is not verified on real silicon**: this machine exposes no `/dev/dri`, so only
   the "device missing → clean error" path is exercised.
2. **Audio tail is padded**: the slicer pads the last frame with silence to the encoder frame size,
   so 1.000 s of audio becomes 1.002 s.
3. **Non-integer frame rates**: the encoder time base is `1/fps` (rounded), so 29.97 drifts by
   milliseconds on long transcodes.
4. **One video plus one audio stream**: multi-camera, multiple audio tracks and subtitles are out of scope.
5. **Single-threaded**: the two branches alternate; there is no thread pool. `bench` measures that chain.

## 9. Contributing

### 9.1 Shared principles

1. **Errors stay classifiable**: return `Expected`/`Status`, never swallow an `int` result.
2. **State belongs to its stage**: pending packet, draining flag and EOF latch stay where they are needed.
3. **Every number must be reproducible**: the throughput table can be regenerated with `bench`.
4. **Before committing**: `cmake --build build -j && ctest --test-dir build && ./build/erde --help`.

### 9.2 Adding a stage

```cpp
// include/erde/my_stage.hpp
class MyStage : public FrameSource {
 public:
  static Expected<std::unique_ptr<MyStage>> open(FrameSource& upstream, /* config */);
  Expected<Frame> pull() override;
};
```

1. Derive from `FrameSource` (frames) or `PacketSource` (packets) and implement `pull()` only;
2. return `Code::end_of_stream` at EOF, `Code::need_more` when temporarily empty, `from_av()` for errors;
3. wire it in `pipeline.cpp` — remember a filter can change the time base and the frame geometry;
4. add a test that needs no external asset (build its input with `gen`).

### 9.3 Status labels

| Label | Meaning |
|---|---|
| ✅ verified | measured or asserted on this machine |
| ⚠️ unverified | implemented, but blocked by missing hardware or environment (e.g. VAAPI) |
| ❌ not implemented | see [known limitations](#8-known-limitations) |

## 10. Credits and license

- All codecs, formats and filters come from [FFmpeg](https://ffmpeg.org/) (libav\*); this project only
  orchestrates them.
- The design rationale and every bug it hit are in [docs/DESIGN.md](docs/DESIGN.md).
- License: **MIT**, see [LICENSE](LICENSE).
