// SPDX-License-Identifier: MIT
#ifndef AVPIPE_MUX_HPP
#define AVPIPE_MUX_HPP

#include <memory>
#include <string>

#include <avpipe/media.hpp>

namespace avpipe {

/// Owns an *output* AVFormatContext, which libav frees through `avformat_free_context()` -
/// not through `avformat_close_input()`, the deleter used for demuxers.
using OutputContextPtr = Ptr<AVFormatContext, free_via_pointer<AVFormatContext, avformat_free_context>>;

/// Writes packets produced by an Encoder into a container.
class Muxer {
 public:
  static Expected<std::unique_ptr<Muxer>> open(const std::string& path, const StreamInfo& encoded);
  ~Muxer();

  Status write(const Packet& packet);
  /// Writes the trailer and closes the file. Safe to call twice.
  Status close();

  int64_t packets_written() const { return packets_written_; }

 private:
  Muxer() = default;

  OutputContextPtr format_;
  AVStream* stream_ = nullptr;
  AVRational encoder_time_base_{1, 30};
  std::string path_;
  bool header_written_ = false;
  bool closed_ = false;
  int64_t packets_written_ = 0;
};

}  // namespace avpipe

#endif  // AVPIPE_MUX_HPP
