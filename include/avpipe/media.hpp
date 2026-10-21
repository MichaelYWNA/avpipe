// SPDX-License-Identifier: MIT
#ifndef AVPIPE_MEDIA_HPP
#define AVPIPE_MEDIA_HPP

#include <string>

#include <avpipe/error.hpp>
#include <avpipe/ffmpeg.hpp>
#include <avpipe/handle.hpp>

namespace avpipe {

using FramePtr = Ptr<AVFrame, free_via_pointer_to_pointer<AVFrame, av_frame_free>>;
using PacketPtr = Ptr<AVPacket, free_via_pointer_to_pointer<AVPacket, av_packet_free>>;
using CodecParamsPtr =
    Ptr<AVCodecParameters, free_via_pointer_to_pointer<AVCodecParameters, avcodec_parameters_free>>;
using CodecContextPtr =
    Ptr<AVCodecContext, free_via_pointer_to_pointer<AVCodecContext, avcodec_free_context>>;
using FormatContextPtr =
    Ptr<AVFormatContext, free_via_pointer_to_pointer<AVFormatContext, avformat_close_input>>;
using SwsContextPtr = Ptr<SwsContext, free_via_pointer<SwsContext, sws_freeContext>>;
using SwrContextPtr = Ptr<SwrContext, free_via_pointer_to_pointer<SwrContext, swr_free>>;
using AudioFifoPtr = Ptr<AVAudioFifo, free_via_pointer<AVAudioFifo, av_audio_fifo_free>>;
using FilterGraphPtr =
    Ptr<AVFilterGraph, free_via_pointer_to_pointer<AVFilterGraph, avfilter_graph_free>>;
using BufferRefPtr = Ptr<AVBufferRef, free_via_pointer_to_pointer<AVBufferRef, av_buffer_unref>>;

/// Owned AVFrame. Construction goes through `Frame::create()` so a null AVFrame can never be
/// wrapped by accident; an empty()` Frame is one that holds no AVFrame at all.
class Frame {
 public:
  Frame() = default;

  static Expected<Frame> create();
  static Expected<Frame> clone(const Frame& other);

  AVFrame* get() const { return ptr_.get(); }
  AVFrame* operator->() const { return ptr_.get(); }
  explicit operator bool() const { return static_cast<bool>(ptr_); }
  bool empty() const { return !ptr_; }
  /// Releases the underlying AVFrame and leaves this object empty.
  void reset() { ptr_.reset(); }

  /// Pixel format name if this looks like a video frame, sample format name if it looks like
  /// audio, empty otherwise. AVFrame has no type tag, so the heuristic mirrors what libav
  /// itself does: width/height for video, nb_samples for audio.
  std::string format_name() const;
  std::string describe() const;

 private:
  FramePtr ptr_;
};

/// Owned AVPacket.
class Packet {
 public:
  Packet() = default;

  static Expected<Packet> create();

  AVPacket* get() const { return ptr_.get(); }
  AVPacket* operator->() const { return ptr_.get(); }
  explicit operator bool() const { return static_cast<bool>(ptr_); }
  bool empty() const { return !ptr_; }
  /// Releases the underlying AVPacket and leaves this object empty.
  void reset() { ptr_.reset(); }

  std::string describe() const;

 private:
  PacketPtr ptr_;
};

/// Owned AVCodecParameters plus the stream-level metadata a decoder/encoder needs.
struct StreamInfo {
  AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
  int stream_index = -1;
  AVRational time_base{1, 1};
  CodecParamsPtr params;

  const char* type_name() const;
  std::string codec_name() const;
  std::string describe() const;
};

/// Name of a pixel format, or "none" when unknown.
std::string pixel_format_name(int format);
/// Name of a sample format, or "none" when unknown.
std::string sample_format_name(int format);

}  // namespace avpipe

#endif  // AVPIPE_MEDIA_HPP
