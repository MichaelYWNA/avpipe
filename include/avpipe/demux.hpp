// SPDX-License-Identifier: MIT
#ifndef AVPIPE_DEMUX_HPP
#define AVPIPE_DEMUX_HPP

#include <memory>
#include <string>
#include <vector>

#include <avpipe/media.hpp>
#include <avpipe/stage.hpp>

namespace avpipe {

/// Opens a container and hands out its packets.
///
/// Deliberately dumb: it does not decode, does not buffer more than one packet, and does not
/// guess which stream the caller wants - `select_stream()` decides that, and every packet from
/// another stream is unreffed immediately so a multi-stream file cannot grow memory.
class Demuxer : public PacketSource {
 public:
  static Expected<std::unique_ptr<Demuxer>> open(const std::string& path);
  ~Demuxer() override;

  Expected<Packet> pull() override;

  /// Restricts `pull()` to one stream. Without a selection every packet is returned.
  Status select_stream(AVMediaType type);
  /// Index of the selected stream, or -1.
  int selected_index() const { return selected_; }

  const std::vector<StreamInfo>& streams() const { return streams_; }
  /// nullptr when the container has no stream of that type.
  const StreamInfo* find(AVMediaType type) const;

  double duration_seconds() const;
  std::string format_name() const;

  int64_t packets_read() const { return packets_read_; }

 private:
  Demuxer() = default;

  FormatContextPtr format_;
  std::vector<StreamInfo> streams_;
  Packet scratch_;
  int selected_ = -1;
  bool eof_ = false;
  int64_t packets_read_ = 0;
};

}  // namespace avpipe

#endif  // AVPIPE_DEMUX_HPP
