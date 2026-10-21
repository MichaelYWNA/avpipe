// SPDX-License-Identifier: MIT
#ifndef AVPIPE_ENCODE_HPP
#define AVPIPE_ENCODE_HPP

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include <avpipe/media.hpp>
#include <avpipe/stage.hpp>

namespace avpipe {

struct EncodeOptions {
  std::string codec = "libx264";
  int64_t bit_rate = 2'000'000;  ///< bits/s, ignored when crf >= 0 (video only)
  int gop = 12;
  int fps = 30;
  std::string preset;  ///< x264/x265 preset, e.g. "veryfast"; empty = codec default
  int crf = -1;        ///< constant-rate-factor when >= 0
  int threads = 0;     ///< 0 = let libav decide

  // audio
  int sample_rate = 0;         ///< 0 keeps the input rate
  int channels = 0;            ///< 0 keeps the input layout
  std::string sample_format;   ///< empty = first format the encoder advertises

  // hardware
  std::string hw_device;       ///< "vaapi", "cuda", ... ; empty = software encoding
  std::string hw_device_name;  ///< e.g. "/dev/dri/renderD128"; empty = first available
};

/// Frame sink + packet source around an AVCodecContext, for video or audio.
///
/// `push()` never reports "full": when libav answers EAGAIN it drains the encoder into an
/// internal queue and retries, so the caller does not interleave send/receive by hand and cannot
/// lose a frame at that boundary.
class Encoder : public PacketSource {
 public:
  static Expected<std::unique_ptr<Encoder>> open(const StreamInfo& input,
                                                 const EncodeOptions& options);
  ~Encoder() override;

  Status push(const Frame& frame);
  Status flush();
  Expected<Packet> pull() override;

  const StreamInfo& out_info() const { return out_info_; }
  std::string codec_name() const;
  int64_t frames_in() const { return frames_in_; }
  int64_t packets_out() const { return packets_out_; }

  /// Audio properties the caller needs to configure the resampler / slicer in front of it.
  int audio_frame_size() const { return frame_size_; }
  int audio_sample_rate() const { return sample_rate_; }
  int audio_channels() const { return channels_; }
  AVSampleFormat audio_sample_format() const { return sample_format_; }
  /// True when the encoder context was opened on a hardware device.
  bool is_hardware() const { return static_cast<bool>(hw_device_ctx_); }

 private:
  Encoder() = default;

  Status open_video(const StreamInfo& input, const EncodeOptions& options);
  Status open_audio(const StreamInfo& input, const EncodeOptions& options);
  Status setup_hardware(const EncodeOptions& options);
  Status drain_locked(const char* what);
  Status send_frame(const AVFrame* frame);

  CodecContextPtr context_;
  StreamInfo out_info_;
  std::deque<Packet> ready_;
  BufferRefPtr hw_device_ctx_;
  BufferRefPtr hw_frames_ctx_;
  AVRational source_time_base_{1, 30};
  int frame_size_ = 0;
  int sample_rate_ = 0;
  int channels_ = 0;
  AVSampleFormat sample_format_ = AV_SAMPLE_FMT_NONE;
  bool flushed_ = false;
  bool finished_ = false;
  int64_t frames_in_ = 0;
  int64_t packets_out_ = 0;
};

}  // namespace avpipe

#endif  // AVPIPE_ENCODE_HPP
