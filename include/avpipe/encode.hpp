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
  int64_t bit_rate = 2'000'000;  ///< bits/s, ignored when crf >= 0
  int gop = 12;
  int fps = 30;
  std::string preset;  ///< x264/x265 preset, e.g. "veryfast"; empty = codec default
  int crf = -1;        ///< constant-rate-factor when >= 0
  int threads = 0;     ///< 0 = let libav decide
};

/// Frame sink + packet source around an AVCodecContext.
///
/// `push()` never reports "full": when libav answers EAGAIN it drains the encoder into an
/// internal queue and retries, so the caller does not have to interleave send/receive by hand
/// and cannot lose a frame at that boundary.
class Encoder : public PacketSource {
 public:
  static Expected<std::unique_ptr<Encoder>> open(const StreamInfo& input,
                                                 const EncodeOptions& options);
  ~Encoder() override;

  /// Accepts one frame. The frame is not referenced after return, so the caller may reuse or
  /// destroy it.
  Status push(const Frame& frame);

  /// Tells the encoder no more frames are coming; packets still inside are then available
  /// through `pull()` until it reports end_of_stream.
  Status flush();

  Expected<Packet> pull() override;

  const StreamInfo& out_info() const { return out_info_; }
  std::string codec_name() const;
  int64_t frames_in() const { return frames_in_; }
  int64_t packets_out() const { return packets_out_; }

 private:
  Encoder() = default;

  Status drain_locked(const char* what);

  CodecContextPtr context_;
  StreamInfo out_info_;
  std::deque<Packet> ready_;
  AVRational source_time_base_{1, 30};
  bool flushed_ = false;
  bool finished_ = false;
  int64_t frames_in_ = 0;
  int64_t packets_out_ = 0;
};

}  // namespace avpipe

#endif  // AVPIPE_ENCODE_HPP
