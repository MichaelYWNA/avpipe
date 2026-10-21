// SPDX-License-Identifier: MIT
//
// Audio tests: tone generation, resampling and re-encoding, all without a checked-in file.
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

avpipe::EncodeOptions audio_options(int bit_rate = 96'000) {
  avpipe::EncodeOptions options;
  options.codec = "aac";
  options.bit_rate = bit_rate;
  options.preset.clear();
  return options;
}

}  // namespace

TEST_CASE(generates_an_audio_file) {
  avpipe::GenerateRequest request;
  request.output = temp_path("tone.m4a").string();
  request.encode = audio_options();
  request.audio_only = true;
  request.seconds = 1.0;
  request.sample_rate = 48'000;
  request.channels = 2;

  auto stats = avpipe::generate(request);
  CHECK(stats.has_value());
  CHECK(stats.value().frames_encoded > 0);
  CHECK(stats.value().packets_written > 0);
  CHECK(std::filesystem::exists(request.output));

  auto report = avpipe::probe(request.output);
  CHECK(report.has_value());
  CHECK(report.value().packets > 0);
  bool audio_stream = false;
  for (const std::string& line : report.value().lines) {
    if (line.find("audio(") != std::string::npos && line.find("48000 Hz") != std::string::npos) {
      audio_stream = true;
    }
  }
  CHECK(audio_stream);
}

TEST_CASE(resamples_and_reencodes_audio) {
  const std::string source = temp_path("tone-src.m4a").string();
  avpipe::GenerateRequest gen;
  gen.output = source;
  gen.encode = audio_options();
  gen.audio_only = true;
  gen.seconds = 0.5;
  gen.sample_rate = 48'000;
  gen.channels = 2;
  CHECK(avpipe::generate(gen).has_value());

  avpipe::AudioTranscodeRequest request;
  request.input = source;
  request.output = temp_path("tone-out.m4a").string();
  request.encode = audio_options(64'000);
  request.sample_rate = 44'100;
  request.channels = 1;

  auto stats = avpipe::transcode_audio(request);
  CHECK(stats.has_value());
  CHECK(stats.value().frames_decoded > 0);
  CHECK(stats.value().frames_encoded > 0);
  CHECK(stats.value().packets_written > 0);

  auto report = avpipe::probe(request.output);
  CHECK(report.has_value());
  bool converted = false;
  for (const std::string& line : report.value().lines) {
    if (line.find("audio(") != std::string::npos && line.find("44100 Hz") != std::string::npos) {
      converted = true;
    }
  }
  CHECK(converted);
}

TEST_CASE(audio_transcode_rejects_a_video_only_file) {
  const std::string source = temp_path("no-audio.avi").string();
  avpipe::GenerateRequest gen;
  gen.output = source;
  gen.encode = avpipe::EncodeOptions{};
  gen.encode.codec = "mpeg4";
  gen.encode.preset.clear();
  gen.frames = 3;
  gen.width = 64;
  gen.height = 48;
  CHECK(avpipe::generate(gen).has_value());

  avpipe::AudioTranscodeRequest request;
  request.input = source;
  request.output = temp_path("no-audio-out.m4a").string();
  request.encode = audio_options();

  auto stats = avpipe::transcode_audio(request);
  CHECK(!stats.has_value());
  CHECK(stats.error().code == avpipe::Code::unsupported);
}

TEST_MAIN()
