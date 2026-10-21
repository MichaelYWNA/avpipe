// SPDX-License-Identifier: MIT
#include <avpipe/media.hpp>

#include <cerrno>
#include <sstream>

namespace avpipe {

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

std::string av_error_string(int av_code) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  if (av_strerror(av_code, buf, sizeof(buf)) < 0) {
    return "unknown libav error " + std::to_string(av_code);
  }
  return std::string(buf);
}

std::string Error::to_string() const {
  std::ostringstream os;
  if (code == Code::ffmpeg) {
    os << "ffmpeg[" << av_code << "]: " << message << ": " << av_error_string(av_code);
  } else {
    os << message;
  }
  return os.str();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

Expected<Frame> Frame::create() {
  Frame frame;
  frame.ptr_.reset(av_frame_alloc());
  if (!frame.ptr_) {
    return from_av(AVERROR(ENOMEM), "Frame::create");
  }
  return frame;
}

Expected<Frame> Frame::clone(const Frame& other) {
  if (other.empty()) {
    return fail(Code::invalid_argument, "Frame::clone: source frame is empty");
  }
  Frame out;
  out.ptr_.reset(av_frame_clone(other.get()));
  if (!out.ptr_) {
    // av_frame_clone() is av_frame_alloc() + av_frame_ref(), and av_frame_ref() refuses a frame
    // that owns no buffer (verified against libavutil 58: a freshly allocated AVFrame clones to
    // NULL). A descriptor-only frame is still worth copying, so fall back to a props copy.
    out.ptr_.reset(av_frame_alloc());
    if (!out.ptr_) {
      return from_av(AVERROR(ENOMEM), "Frame::clone");
    }
    if (const int ret = av_frame_copy_props(out.get(), other.get()); ret < 0) {
      return from_av(ret, "Frame::clone (props only)");
    }
  }
  return out;
}

std::string Frame::format_name() const {
  if (empty()) {
    return {};
  }
  const AVFrame* f = get();
  if (f->width > 0 && f->height > 0) {
    return pixel_format_name(f->format);
  }
  if (f->nb_samples > 0) {
    return sample_format_name(f->format);
  }
  return {};
}

std::string Frame::describe() const {
  if (empty()) {
    return "frame[none]";
  }
  const AVFrame* f = get();
  std::ostringstream os;
  if (f->width > 0 && f->height > 0) {
    os << "frame[" << pixel_format_name(f->format) << " " << f->width << "x" << f->height
       << " pts=" << f->pts << "]";
  } else {
    os << "frame[" << sample_format_name(f->format) << " nb_samples=" << f->nb_samples
       << " pts=" << f->pts << "]";
  }
  return os.str();
}

// ---------------------------------------------------------------------------
// Packet
// ---------------------------------------------------------------------------

Expected<Packet> Packet::create() {
  Packet packet;
  packet.ptr_.reset(av_packet_alloc());
  if (!packet.ptr_) {
    return from_av(AVERROR(ENOMEM), "Packet::create");
  }
  return packet;
}

std::string Packet::describe() const {
  if (empty()) {
    return "packet[none]";
  }
  std::ostringstream os;
  os << "packet[size=" << get()->size << " pts=" << get()->pts << " dts=" << get()->dts
     << " key=" << ((get()->flags & AV_PKT_FLAG_KEY) ? 1 : 0) << "]";
  return os.str();
}

// ---------------------------------------------------------------------------
// StreamInfo
// ---------------------------------------------------------------------------

const char* StreamInfo::type_name() const {
  const char* name = av_get_media_type_string(type);
  return name != nullptr ? name : "unknown";
}

std::string StreamInfo::codec_name() const {
  if (!params) {
    return "none";
  }
  const AVCodecDescriptor* desc = avcodec_descriptor_get(params->codec_id);
  if (desc != nullptr) {
    return desc->name;
  }
  const char* name = avcodec_get_name(params->codec_id);
  return name != nullptr ? name : "unknown";
}

std::string StreamInfo::describe() const {
  std::ostringstream os;
  os << type_name() << "(index=" << stream_index << ", codec=" << codec_name();
  if (params && type == AVMEDIA_TYPE_VIDEO) {
    os << ", " << params->width << "x" << params->height;
    if (params->framerate.num > 0) {
      os << ", fps=" << av_q2d(params->framerate);
    }
  }
  if (params && type == AVMEDIA_TYPE_AUDIO) {
    os << ", " << params->sample_rate << " Hz";
  }
  os << ", tb=" << time_base.num << "/" << time_base.den << ")";
  return os.str();
}

std::string pixel_format_name(int format) {
  const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
  return name != nullptr ? name : "none";
}

std::string sample_format_name(int format) {
  const char* name = av_get_sample_fmt_name(static_cast<AVSampleFormat>(format));
  return name != nullptr ? name : "none";
}

}  // namespace avpipe
