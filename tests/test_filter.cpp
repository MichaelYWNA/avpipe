// SPDX-License-Identifier: MIT
//
// Filter-graph tests: fps conversion and crop, which the plain scaler cannot express.
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

avpipe::EncodeOptions fast_options() {
  avpipe::EncodeOptions options;
  options.codec = "mpeg4";
  options.bit_rate = 400'000;
  options.preset.clear();
  options.gop = 10;
  return options;
}

std::string make_source(int frames, int fps) {
  const std::string path = temp_path("filter-source.avi").string();
  avpipe::GenerateRequest gen;
  gen.output = path;
  gen.encode = fast_options();
  gen.width = 160;
  gen.height = 120;
  gen.fps = fps;
  gen.frames = frames;
  CHECK(avpipe::generate(gen).has_value());
  return path;
}

}  // namespace

TEST_CASE(filter_drops_frames_for_fps_conversion) {
  const std::string source = make_source(30, 30);

  avpipe::TranscodeRequest request;
  request.input = source;
  request.output = temp_path("filter-fps.avi").string();
  request.encode = fast_options();
  request.filter = "fps=15";
  request.encode.fps = 15;
  request.force_fps = true;  // allowed because the filter chain converts the rate

  auto stats = avpipe::transcode(request);
  CHECK(stats.has_value());
  // 30 frames at 30 fps is 1 s; at 15 fps that must come out as ~15 frames.
  CHECK(stats.value().frames_encoded >= 13);
  CHECK(stats.value().frames_encoded <= 17);
  CHECK(stats.value().frames_decoded == 30);
}

TEST_CASE(filter_crops_geometry) {
  const std::string source = make_source(6, 15);

  avpipe::TranscodeRequest request;
  request.input = source;
  request.output = temp_path("filter-crop.avi").string();
  request.encode = fast_options();
  request.filter = "crop=96:72:0:0";

  auto stats = avpipe::transcode(request);
  CHECK(stats.has_value());

  auto report = avpipe::probe(request.output);
  CHECK(report.has_value());
  bool cropped = false;
  for (const std::string& line : report.value().lines) {
    if (line.find("96x72") != std::string::npos) {
      cropped = true;
    }
  }
  CHECK(cropped);
}

TEST_CASE(bad_filter_is_reported) {
  const std::string source = make_source(3, 15);

  avpipe::TranscodeRequest request;
  request.input = source;
  request.output = temp_path("filter-bad.avi").string();
  request.encode = fast_options();
  request.filter = "no_such_filter=1";

  auto stats = avpipe::transcode(request);
  CHECK(!stats.has_value());
  CHECK(stats.error().code == avpipe::Code::ffmpeg);
}

TEST_MAIN()
