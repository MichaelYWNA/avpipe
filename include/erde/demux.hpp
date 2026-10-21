// SPDX-License-Identifier: MIT
#ifndef ERDE_DEMUX_HPP
#define ERDE_DEMUX_HPP

#include <memory>
#include <string>
#include <vector>

#include <erde/media.hpp>
#include <erde/stage.hpp>

namespace erde {

/// Opens a container or a live device and hands out its packets.
///
/// Deliberately dumb: it does not decode, does not buffer more than one packet, and does not
/// guess which stream the caller wants - `select_stream()` decides that, and every packet from
/// another stream is unreffed immediately so a multi-stream file cannot grow memory.
///
/// `format_name` / `options` are the escape hatch for devices: opening `/dev/video0` needs
/// `format=v4l2` plus options such as `video_size=1280x720,framerate=30`; leaving them empty
/// means "let libav probe the file", which is what every file-based caller wants.
class Demuxer : public PacketSource {
 public:
  static Expected<std::unique_ptr<Demuxer>> open(const std::string& path,
                                                 const std::string& format_name = {},
                                                 const std::string& options = {});
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
  /// True when the input is a live device (no duration, no seeking).
  bool is_live() const;

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

}  // namespace erde

#endif  // ERDE_DEMUX_HPP
