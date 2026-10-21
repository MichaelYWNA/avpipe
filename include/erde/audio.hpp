// SPDX-License-Identifier: MIT
#ifndef ERDE_AUDIO_HPP
#define ERDE_AUDIO_HPP

#include <memory>

#include <erde/media.hpp>
#include <erde/stage.hpp>

namespace erde {

/// Deterministic sine tone used by `erde gen --audio` and by the audio tests, so no audio file
/// has to be checked in.
class ToneSource : public FrameSource {
 public:
  ToneSource(int sample_rate, int channels, AVSampleFormat format, int samples_per_frame,
             int total_frames);
  Expected<Frame> pull() override;

 private:
  int sample_rate_ = 0;
  int channels_ = 0;
  AVSampleFormat format_ = AV_SAMPLE_FMT_FLTP;
  int samples_per_frame_ = 0;
  int total_frames_ = 0;
  int produced_ = 0;
  int64_t samples_emitted_ = 0;
};

/// swresample stage: converts sample rate, channel count and sample format.
///
/// The source layout/format is only known once the first frame arrives, so the context is built
/// lazily and that first frame is held back until the first `pull()`.
class AudioResampler : public FrameSource {
 public:
  static Expected<std::unique_ptr<AudioResampler>> open(FrameSource& source, int sample_rate,
                                                        int channels, AVSampleFormat format);
  ~AudioResampler() override;
  Expected<Frame> pull() override;
  int64_t frames_converted() const { return converted_; }

 private:
  AudioResampler() = default;
  Status configure(const AVFrame& first);
  Expected<Frame> convert(const AVFrame& input);
  Expected<Frame> drain();

  FrameSource* source_ = nullptr;
  int out_sample_rate_ = 0;
  int out_channels_ = 0;
  AVSampleFormat out_format_ = AV_SAMPLE_FMT_FLTP;
  AVChannelLayout out_layout_{};
  SwrContextPtr swr_;
  Frame pending_;
  bool configured_ = false;
  bool upstream_eof_ = false;
  bool drained_ = false;
  int64_t converted_ = 0;
};

/// Repacks frames into the fixed chunk size every audio encoder wants (AAC: 1024 samples, Opus:
/// 960 at 48 kHz). Uses AVAudioFifo so frames larger or smaller than one encoder frame are not
/// dropped.
class AudioSlicer : public FrameSource {
 public:
  static Expected<std::unique_ptr<AudioSlicer>> open(FrameSource& source, int frame_size);
  ~AudioSlicer() override;
  Expected<Frame> pull() override;
  int64_t frames_emitted() const { return frames_emitted_; }

 private:
  AudioSlicer() = default;

  FrameSource* source_ = nullptr;
  int frame_size_ = 0;
  AVSampleFormat format_ = AV_SAMPLE_FMT_NONE;
  int channels_ = 0;
  AudioFifoPtr fifo_;
  bool upstream_eof_ = false;
  bool finished_ = false;
  int64_t frames_emitted_ = 0;
};

}  // namespace erde

#endif  // ERDE_AUDIO_HPP
