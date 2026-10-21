// SPDX-License-Identifier: MIT
#ifndef ERDE_DECODE_HPP
#define ERDE_DECODE_HPP

#include <memory>

#include <erde/media.hpp>
#include <erde/stage.hpp>

namespace erde {

/// Turns packets from a PacketSource into frames.
///
/// The reason this class exists instead of a loop in `transcode()`: a decoder is an N:M device.
/// `send_packet()` may be rejected while output is pending, and one packet can yield zero or
/// several frames (B-frame reordering, codec delay). Keeping the state here - pending packet,
/// draining flag, EOF latch - is what makes "never silently drop a frame" possible.
class Decoder : public FrameSource {
 public:
  static Expected<std::unique_ptr<Decoder>> open(PacketSource& source, const StreamInfo& info);
  ~Decoder() override;

  Expected<Frame> pull() override;

  int64_t frames_decoded() const { return frames_; }
  const StreamInfo& info() const { return info_; }

 private:
  Decoder() = default;

  PacketSource* source_ = nullptr;
  CodecContextPtr context_;
  StreamInfo info_;
  Packet pending_;
  bool draining_ = false;
  bool finished_ = false;
  int64_t frames_ = 0;
};

}  // namespace erde

#endif  // ERDE_DECODE_HPP
