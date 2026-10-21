// SPDX-License-Identifier: MIT
#ifndef AVPIPE_STAGE_HPP
#define AVPIPE_STAGE_HPP

#include <avpipe/error.hpp>
#include <avpipe/media.hpp>

namespace avpipe {

/// A stage that produces compressed packets (demuxer, encoder, packet receiver).
class PacketSource {
 public:
  virtual ~PacketSource() = default;
  /// Returns `Code::end_of_stream` once no further packet can ever be produced, and
  /// `Code::need_more` when the caller should try again after pushing/producing more input.
  virtual Expected<Packet> pull() = 0;
};

/// A stage that produces raw frames (decoder, scaler, synthetic generator).
class FrameSource {
 public:
  virtual ~FrameSource() = default;
  virtual Expected<Frame> pull() = 0;
};

}  // namespace avpipe

#endif  // AVPIPE_STAGE_HPP
