// SPDX-License-Identifier: MIT
#ifndef ERDE_MUX_HPP
#define ERDE_MUX_HPP

#include <memory>
#include <string>
#include <vector>

#include <erde/media.hpp>

namespace erde {

/// Owns an *output* AVFormatContext, which libav frees through `avformat_free_context()` -
/// not through `avformat_close_input()`, the deleter used for demuxers.
using OutputContextPtr = Ptr<AVFormatContext, free_via_pointer<AVFormatContext, avformat_free_context>>;

/// Writes packets from one or more encoders into a container.
///
/// Lifecycle is explicit because a container header can only be written once every stream is
/// known: `open()` -> `add_stream()` x N -> `start()` -> `write()`... -> `close()`. A single-use
/// muxer that guesses one stream was the old shape; adding audio to a video file needs the
/// multi-stream form, and `av_interleaved_write_frame()` does the actual interleaving, so callers
/// may push video and audio packets in any order.
class Muxer {
 public:
  static Expected<std::unique_ptr<Muxer>> open(const std::string& path);
  ~Muxer();

  /// Registers one output stream. Returns its muxer index (used by `write`).
  Expected<int> add_stream(const StreamInfo& encoded);

  /// Writes the container header and opens the file. Must be called after the last add_stream().
  Status start();

  /// Writes a packet belonging to the stream returned by add_stream().
  Status write(const Packet& packet, int stream_index);
  /// Convenience for single-stream files.
  Status write(const Packet& packet);

  Status close();

  int stream_count() const { return static_cast<int>(streams_.size()); }
  int64_t packets_written() const { return packets_written_; }

 private:
  Muxer() = default;

  struct OutputStream {
    AVStream* stream = nullptr;
    AVRational encoder_time_base{1, 30};
  };

  OutputContextPtr format_;
  std::vector<OutputStream> streams_;
  std::string path_;
  bool started_ = false;
  bool closed_ = false;
  int64_t packets_written_ = 0;
};

}  // namespace erde

#endif  // ERDE_MUX_HPP
