// SPDX-License-Identifier: MIT
//
// Encoder-level tests, including the hardware path. This machine has no /dev/dri, so the VAAPI
// case is expected to fail - what is asserted is that it fails in a *controlled* way with a
// usable message, not that it succeeds. On a box with a GPU the same code takes the other branch
// and the assertion still holds (success is accepted too).
#include "harness.hpp"

#include <string>

#include <erde/encode.hpp>

namespace {

erde::StreamInfo video_template() {
  erde::StreamInfo info;
  info.type = AVMEDIA_TYPE_VIDEO;
  info.stream_index = 0;
  info.time_base = AVRational{1, 30};
  info.params.reset(avcodec_parameters_alloc());
  info.params->codec_type = AVMEDIA_TYPE_VIDEO;
  info.params->width = 128;
  info.params->height = 96;
  info.params->framerate = AVRational{30, 1};
  return info;
}

erde::StreamInfo audio_template() {
  erde::StreamInfo info;
  info.type = AVMEDIA_TYPE_AUDIO;
  info.stream_index = 0;
  info.time_base = AVRational{1, 48000};
  info.params.reset(avcodec_parameters_alloc());
  info.params->codec_type = AVMEDIA_TYPE_AUDIO;
  info.params->sample_rate = 48000;
  av_channel_layout_default(&info.params->ch_layout, 2);
  return info;
}

}  // namespace

TEST_CASE(opens_a_software_video_encoder) {
  const erde::StreamInfo info = video_template();
  erde::EncodeOptions options;
  options.codec = "mpeg4";
  options.preset.clear();

  auto encoder = erde::Encoder::open(info, options);
  CHECK(encoder.has_value());
  CHECK(!encoder.value()->is_hardware());
  CHECK(encoder.value()->out_info().params.get() != nullptr);
  CHECK_EQ(encoder.value()->out_info().params->width, 128);
}

TEST_CASE(opens_an_audio_encoder_and_reports_frame_size) {
  const erde::StreamInfo info = audio_template();
  erde::EncodeOptions options;
  options.codec = "aac";
  options.bit_rate = 64'000;

  auto encoder = erde::Encoder::open(info, options);
  CHECK(encoder.has_value());
  CHECK(encoder.value()->audio_frame_size() > 0);
  CHECK_EQ(encoder.value()->audio_sample_rate(), 48000);
  CHECK_EQ(encoder.value()->audio_channels(), 2);
}

TEST_CASE(unknown_encoder_is_rejected) {
  const erde::StreamInfo info = video_template();
  erde::EncodeOptions options;
  options.codec = "definitely_not_a_codec";

  auto encoder = erde::Encoder::open(info, options);
  CHECK(!encoder.has_value());
  CHECK(encoder.error().code == erde::Code::unsupported);
}

TEST_CASE(hardware_encoder_fails_gracefully_without_a_device) {
  const erde::StreamInfo info = video_template();
  erde::EncodeOptions options;
  options.codec = "libx264";
  options.hw_device = "vaapi";
  options.hw_device_name = "/dev/dri/renderD128";

  auto encoder = erde::Encoder::open(info, options);
  if (encoder.has_value()) {
    // A machine that actually exposes the device: the context must be flagged as hardware.
    CHECK(encoder.value()->is_hardware());
  } else {
    // No device here: the failure must be classified and carry a message, not crash or assert.
    CHECK(encoder.error().code == erde::Code::ffmpeg ||
          encoder.error().code == erde::Code::unsupported);
    CHECK(!encoder.error().message.empty());
  }
}

TEST_MAIN()
