// SPDX-License-Identifier: MIT
//
// Multi-stream tests: one file carrying video *and* audio through a single muxer, plus the audio
// filter chain. Everything is generated in-process, so no media file is checked in.
#include "harness.hpp"

#include <filesystem>
#include <string>

#include <avpipe/encode.hpp>
#include <avpipe/pipeline.hpp>

namespace {

std::filesystem::path temp_path(const std::string& name) {
  const auto dir = std::filesystem::temp_directory_path() / "avpipe-tests";
  std::filesystem::create_directories(dir);
  const auto path = dir / name;
  std::filesystem::remove(path);
  return path;
}

avpipe::EncodeOptions video_options() {
  avpipe::EncodeOptions options;
  options.codec = "mpeg4";
  options.bit_rate = 400'000;
  options.preset.clear();
  options.gop = 10;
  return options;
}

/// Counts the stream lines of a probe report and returns their packet counts.
struct StreamCounts {
  int video = -1;
  int audio = -1;
  int64_t video_packets = 0;
  int64_t audio_packets = 0;
};

StreamCounts parse(const avpipe::ProbeReport& report) {
  StreamCounts counts;
  for (const std::string& line : report.lines) {
    const std::size_t packets_at = line.find("packets=");
    const int64_t packets = packets_at == std::string::npos
                                ? 0
                                : std::stoll(line.substr(packets_at + 8));
    if (line.find("video(") != std::string::npos) {
      ++counts.video;
      counts.video_packets = packets;
    } else if (line.find("audio(") != std::string::npos) {
      ++counts.audio;
      counts.audio_packets = packets;
    }
  }
  return counts;
}

}  // namespace

TEST_CASE(generates_a_file_with_both_streams) {
  avpipe::GenerateRequest request;
  request.output = temp_path("av-gen.mp4").string();
  request.encode = video_options();
  request.audio_encode = avpipe::default_audio_encode();
  request.with_audio = true;
  request.seconds = 1.0;
  request.width = 160;
  request.height = 120;
  request.fps = 15;
  request.frames = 15;

  auto stats = avpipe::generate(request);
  CHECK(stats.has_value());
  CHECK_EQ(stats.value().video_streams, 1);
  CHECK_EQ(stats.value().audio_streams, 1);
  CHECK(stats.value().frames_encoded > 0);
  CHECK(stats.value().audio_frames_encoded > 0);
  CHECK(stats.value().audio_packets_written > 0);

  auto report = avpipe::probe(request.output);
  CHECK(report.has_value());
  const StreamCounts counts = parse(report.value());
  CHECK_EQ(counts.video, 0);
  CHECK_EQ(counts.audio, 0);
  CHECK(counts.video_packets > 0);
  CHECK(counts.audio_packets > 0);
}

TEST_CASE(transcodes_both_streams_into_one_file) {
  const std::string source = temp_path("av-source.mp4").string();
  avpipe::GenerateRequest gen;
  gen.output = source;
  gen.encode = video_options();
  gen.audio_encode = avpipe::default_audio_encode();
  gen.with_audio = true;
  gen.seconds = 1.0;
  gen.width = 160;
  gen.height = 120;
  gen.fps = 15;
  gen.frames = 15;
  CHECK(avpipe::generate(gen).has_value());

  avpipe::TranscodeRequest request;
  request.input = source;
  request.output = temp_path("av-out.mp4").string();
  request.encode = video_options();
  request.audio_encode = avpipe::default_audio_encode();
  request.audio_encode.bit_rate = 64'000;

  auto stats = avpipe::transcode(request);
  CHECK(stats.has_value());
  CHECK_EQ(stats.value().video_streams, 1);
  CHECK_EQ(stats.value().audio_streams, 1);
  CHECK_EQ(stats.value().frames_encoded, int64_t{15});
  CHECK(stats.value().audio_frames_encoded > 0);
  CHECK(stats.value().audio_packets_written > 0);

  auto report = avpipe::probe(request.output);
  CHECK(report.has_value());
  const StreamCounts counts = parse(report.value());
  CHECK_EQ(counts.video, 0);
  CHECK_EQ(counts.audio, 0);
  CHECK(counts.video_packets > 0);
  CHECK(counts.audio_packets > 0);
  // The container duration should still be about one second.
  CHECK(report.value().duration_s > 0.8);
  CHECK(report.value().duration_s < 1.3);
}

TEST_CASE(drops_audio_when_asked) {
  const std::string source = temp_path("av-source2.mp4").string();
  avpipe::GenerateRequest gen;
  gen.output = source;
  gen.encode = video_options();
  gen.with_audio = true;
  gen.seconds = 1.0;
  gen.width = 128;
  gen.height = 96;
  gen.fps = 15;
  gen.frames = 15;
  CHECK(avpipe::generate(gen).has_value());

  avpipe::TranscodeRequest request;
  request.input = source;
  request.output = temp_path("av-videoonly.mp4").string();
  request.encode = video_options();
  request.include_audio = false;

  auto stats = avpipe::transcode(request);
  CHECK(stats.has_value());
  CHECK_EQ(stats.value().video_streams, 1);
  CHECK_EQ(stats.value().audio_streams, 0);

  auto report = avpipe::probe(request.output);
  CHECK(report.has_value());
  const StreamCounts counts = parse(report.value());
  CHECK_EQ(counts.video, 0);
  CHECK(counts.audio < 0);  // no audio stream at all
}

TEST_CASE(audio_filter_changes_the_timeline) {
  const std::string source = temp_path("av-atempo-src.m4a").string();
  avpipe::GenerateRequest gen;
  gen.output = source;
  gen.audio_only = true;
  gen.audio_encode = avpipe::default_audio_encode();
  gen.seconds = 1.0;
  CHECK(avpipe::generate(gen).has_value());

  avpipe::AudioTranscodeRequest plain;
  plain.input = source;
  plain.output = temp_path("av-atempo-plain.m4a").string();
  plain.encode = avpipe::default_audio_encode();
  auto plain_stats = avpipe::transcode_audio(plain);
  CHECK(plain_stats.has_value());

  avpipe::AudioTranscodeRequest fast;
  fast.input = source;
  fast.output = temp_path("av-atempo-fast.m4a").string();
  fast.encode = avpipe::default_audio_encode();
  fast.filter = "atempo=2.0";  // plays twice as fast -> half the duration
  auto fast_stats = avpipe::transcode_audio(fast);
  CHECK(fast_stats.has_value());

  auto plain_report = avpipe::probe(plain.output);
  auto fast_report = avpipe::probe(fast.output);
  CHECK(plain_report.has_value());
  CHECK(fast_report.has_value());
  CHECK(plain_report.value().duration_s > 0.8);
  CHECK(fast_report.value().duration_s < 0.75);
  CHECK(fast_report.value().duration_s > 0.25);
}

TEST_CASE(audio_filter_rejects_unknown_filter) {
  const std::string source = temp_path("av-badfilter-src.m4a").string();
  avpipe::GenerateRequest gen;
  gen.output = source;
  gen.audio_only = true;
  gen.audio_encode = avpipe::default_audio_encode();
  gen.seconds = 0.5;
  CHECK(avpipe::generate(gen).has_value());

  avpipe::AudioTranscodeRequest request;
  request.input = source;
  request.output = temp_path("av-badfilter-out.m4a").string();
  request.encode = avpipe::default_audio_encode();
  request.filter = "no_such_audio_filter=1";

  auto stats = avpipe::transcode_audio(request);
  CHECK(!stats.has_value());
  CHECK(stats.error().code == avpipe::Code::ffmpeg);
}

TEST_MAIN()
