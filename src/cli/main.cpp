// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <erde/config.hpp>
#include <erde/encode.hpp>
#include <erde/pipeline.hpp>

namespace {

constexpr const char* kVersion = "0.3.0";

void print_usage(std::ostream& os) {
  os << "erde " << kVersion << " - pull-based audio/video pipeline on libav*\n"
     << "\n"
     << "usage:\n"
     << "  erde probe <file>\n"
     << "  erde gen <out> [options]\n"
     << "  erde transcode <in> <out> [options]\n"
     << "  erde transcode-audio <in> <out> [options]\n"
     << "  erde bench <in> <out> [--runs N] [options]\n"
     << "\n"
     << "options:\n"
     << "  --codec <name>     encoder name (video: libx264/libx265/mpeg4; audio: aac/libmp3lame)\n"
     << "  --bitrate <rate>   2000k / 2M / 1500000 (default 2000000)\n"
     << "  --preset <name>    x264/x265 preset, e.g. veryfast\n"
     << "  --crf <n>          constant rate factor, overrides --bitrate\n"
     << "  --gop <n>          keyframe interval (default 12)\n"
     << "  --fps <n>          frame rate for gen; for transcode it asserts the rate\n"
     << "  --size <WxH>       target size; inserts a scaler\n"
     << "  --filter <desc>    libavfilter chain for video, e.g. 'fps=15,crop=640:360'\n"
     << "  --audio-filter <d> libavfilter chain for audio, e.g. 'atempo=2.0'\n"
     << "  --audio-codec <n>  audio encoder (default aac)\n"
     << "  --audio-bitrate <r> audio bit rate (default 128k)\n"
     << "  --no-audio         transcode: drop the audio track\n"
     << "  --with-audio       gen: add a tone track next to the video\n"
     << "  --frames <n>       frame count for gen (default 60)\n"
     << "  --audio            gen: produce a sine tone instead of a video pattern\n"
     << "  --seconds <s>      gen --audio: duration (default 2.0)\n"
     << "  --rate <hz>        audio sample rate (default 48000 for gen)\n"
     << "  --channels <n>     audio channel count\n"
     << "  --hw <name>        hardware encode device: vaapi, cuda (default: software)\n"
     << "  --hw-device <path> e.g. /dev/dri/renderD128\n"
     << "  --config <file>    read defaults from an INI file (see docs/DESIGN.md)\n"
     << "  --quiet            only print the final stats\n"
     << "  --verbose          also show libav's own logging\n";
}

struct Options {
  std::string codec{"libx264"};
  bool codec_explicit{false};
  int64_t bit_rate{2'000'000};
  std::string preset{"veryfast"};
  int crf{-1};
  int gop{12};
  int fps{30};
  bool fps_explicit{false};
  int frames{60};
  int width{0};
  int height{0};
  int runs{3};
  bool quiet{false};
  bool verbose{false};
  std::string filter;
  std::string audio_filter;
  bool no_audio{false};
  bool with_audio{false};
  std::string audio_codec{"aac"};
  int64_t audio_bit_rate{128'000};
  bool audio{false};
  double seconds{2.0};
  int sample_rate{0};
  int channels{0};
  std::string hw;
  std::string hw_device;
};

int fail(const erde::Error& error) {
  std::cerr << "erde: " << error.to_string() << "\n";
  return 2;
}

int usage_error(const std::string& message) {
  std::cerr << "erde: " << message << "\n\n";
  print_usage(std::cerr);
  return 1;
}

bool needs_value(const std::string& flag) {
  return flag == "--codec" || flag == "--bitrate" || flag == "--preset" || flag == "--crf" ||
         flag == "--gop" || flag == "--fps" || flag == "--size" || flag == "--frames" ||
         flag == "--runs" || flag == "--config" || flag == "--filter" || flag == "--seconds" ||
         flag == "--rate" || flag == "--channels" || flag == "--hw" || flag == "--hw-device" ||
         flag == "--audio-filter" || flag == "--audio-codec" || flag == "--audio-bitrate";
}

erde::Status apply_option(Options& options, erde::EncodeOptions& encode,
                            const std::string& flag, const std::string& value) {
  using erde::Code;
  using erde::fail;
  if (flag == "--codec") {
    encode.codec = value;
    options.codec_explicit = true;
  } else if (flag == "--bitrate") {
    auto parsed = erde::parse_bit_rate(value);
    if (!parsed) {
      return parsed.error();
    }
    encode.bit_rate = parsed.value();
  } else if (flag == "--preset") {
    encode.preset = value;
  } else if (flag == "--crf") {
    auto parsed = erde::parse_int(value, "--crf");
    if (!parsed) {
      return parsed.error();
    }
    encode.crf = static_cast<int>(parsed.value());
  } else if (flag == "--gop") {
    auto parsed = erde::parse_int(value, "--gop");
    if (!parsed) {
      return parsed.error();
    }
    encode.gop = static_cast<int>(parsed.value());
  } else if (flag == "--fps") {
    auto parsed = erde::parse_int(value, "--fps");
    if (!parsed) {
      return parsed.error();
    }
    options.fps = static_cast<int>(parsed.value());
    options.fps_explicit = true;
    encode.fps = options.fps;
  } else if (flag == "--frames") {
    auto parsed = erde::parse_int(value, "--frames");
    if (!parsed) {
      return parsed.error();
    }
    options.frames = static_cast<int>(parsed.value());
  } else if (flag == "--runs") {
    auto parsed = erde::parse_int(value, "--runs");
    if (!parsed) {
      return parsed.error();
    }
    options.runs = static_cast<int>(parsed.value());
  } else if (flag == "--size") {
    auto parsed = erde::parse_size(value);
    if (!parsed) {
      return parsed.error();
    }
    options.width = parsed.value().first;
    options.height = parsed.value().second;
  } else if (flag == "--filter") {
    options.filter = value;
  } else if (flag == "--seconds") {
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == nullptr || *end != '\0' || parsed <= 0.0) {
      return fail(Code::config, "--seconds: '" + value + "' is not a positive number");
    }
    options.seconds = parsed;
  } else if (flag == "--rate") {
    auto parsed = erde::parse_int(value, "--rate");
    if (!parsed) {
      return parsed.error();
    }
    options.sample_rate = static_cast<int>(parsed.value());
    encode.sample_rate = options.sample_rate;
  } else if (flag == "--channels") {
    auto parsed = erde::parse_int(value, "--channels");
    if (!parsed) {
      return parsed.error();
    }
    options.channels = static_cast<int>(parsed.value());
    encode.channels = options.channels;
  } else if (flag == "--hw") {
    options.hw = value;
    encode.hw_device = value;
  } else if (flag == "--hw-device") {
    options.hw_device = value;
    encode.hw_device_name = value;
  } else if (flag == "--audio-filter") {
    options.audio_filter = value;
  } else if (flag == "--audio-codec") {
    options.audio_codec = value;
  } else if (flag == "--audio-bitrate") {
    auto parsed = erde::parse_bit_rate(value);
    if (!parsed) {
      return parsed.error();
    }
    options.audio_bit_rate = parsed.value();
  } else {
    return fail(Code::invalid_argument, "unknown option " + flag);
  }
  return erde::ok();
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty() || args.front() == "--help" || args.front() == "-h") {
    print_usage(std::cout);
    return args.empty() ? 1 : 0;
  }
  if (args.front() == "--version") {
    std::cout << kVersion << "\n";
    return 0;
  }

  const std::string command = args.front();
  args.erase(args.begin());

  Options options;
  erde::EncodeOptions encode;
  std::vector<std::string> positional;
  std::string config_path;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg.rfind("--", 0) != 0) {
      positional.push_back(arg);
      continue;
    }
    if (arg == "--quiet") {
      options.quiet = true;
      continue;
    }
    if (arg == "--verbose") {
      options.verbose = true;
      continue;
    }
    if (arg == "--audio") {
      options.audio = true;
      continue;
    }
    if (arg == "--with-audio") {
      options.with_audio = true;
      continue;
    }
    if (arg == "--no-audio") {
      options.no_audio = true;
      continue;
    }
    if (!needs_value(arg)) {
      return usage_error("unknown option " + arg);
    }
    if (i + 1 >= args.size()) {
      return usage_error(arg + " needs a value");
    }
    const std::string value = args[++i];
    if (arg == "--config") {
      config_path = value;
      continue;
    }
    if (auto applied = apply_option(options, encode, arg, value); applied) {
      return fail(applied);
    }
  }

  if (!config_path.empty()) {
    auto config = erde::Config::load(config_path);
    if (!config) {
      return fail(config.error());
    }
    const auto& cfg = config.value();
    if (cfg.has("encode", "codec")) {
      encode.codec = cfg.get("encode", "codec");
      options.codec_explicit = true;
    }
    if (cfg.has("encode", "bitrate")) {
      auto parsed = erde::parse_bit_rate(cfg.get("encode", "bitrate"));
      if (!parsed) {
        return fail(parsed.error());
      }
      encode.bit_rate = parsed.value();
    }
    if (cfg.has("encode", "preset")) {
      encode.preset = cfg.get("encode", "preset");
    }
    auto crf = cfg.get_int("encode", "crf", encode.crf);
    if (!crf) {
      return fail(crf.error());
    }
    encode.crf = static_cast<int>(crf.value());
    auto gop = cfg.get_int("encode", "gop", encode.gop);
    if (!gop) {
      return fail(gop.error());
    }
    encode.gop = static_cast<int>(gop.value());
    auto fps = cfg.get_int("video", "fps", options.fps);
    if (!fps) {
      return fail(fps.error());
    }
    options.fps = static_cast<int>(fps.value());
    options.fps_explicit = true;
    encode.fps = options.fps;
    const std::string size = cfg.get("video", "size");
    if (!size.empty()) {
      auto parsed = erde::parse_size(size);
      if (!parsed) {
        return fail(parsed.error());
      }
      options.width = parsed.value().first;
      options.height = parsed.value().second;
    }
    const std::string filter = cfg.get("video", "filter");
    if (!filter.empty()) {
      options.filter = filter;
    }
    auto rate = cfg.get_int("audio", "rate", options.sample_rate);
    if (!rate) {
      return fail(rate.error());
    }
    options.sample_rate = static_cast<int>(rate.value());
    encode.sample_rate = options.sample_rate;
    auto channels = cfg.get_int("audio", "channels", options.channels);
    if (!channels) {
      return fail(channels.error());
    }
    options.channels = static_cast<int>(channels.value());
    encode.channels = options.channels;
  }

  // libav logs encoder banners (x264/x265) straight to stderr; keep the tool's own output clean
  // unless the caller asks for detail.
  av_log_set_level(options.verbose ? AV_LOG_INFO : AV_LOG_ERROR);

  if (command == "probe") {
    if (positional.size() != 1) {
      return usage_error("probe needs exactly one file");
    }
    auto report = erde::probe(positional.front());
    if (!report) {
      return fail(report.error());
    }
    for (const std::string& line : report.value().lines) {
      std::cout << line << "\n";
    }
    std::cout << "size=" << report.value().size_bytes << " bytes\n";
    return 0;
  }

  if (command == "gen") {
    if (positional.size() != 1) {
      return usage_error("gen needs an output file");
    }
    erde::GenerateRequest request;
    request.output = positional.front();
    if (options.audio && !options.codec_explicit) {
      encode.codec = "aac";
    }
    request.encode = encode;
    request.audio_encode = erde::default_audio_encode();
    request.audio_encode.codec = options.audio_codec;
    request.audio_encode.bit_rate = options.audio_bit_rate;
    request.width = options.width > 0 ? options.width : 640;
    request.height = options.height > 0 ? options.height : 480;
    request.fps = options.fps;
    request.frames = options.frames;
    request.audio_only = options.audio;
    request.with_audio = options.with_audio;
    request.seconds = options.seconds;
    request.sample_rate = options.sample_rate > 0 ? options.sample_rate : 48'000;
    request.channels = options.channels > 0 ? options.channels : 2;
    auto stats = erde::generate(request);
    if (!stats) {
      return fail(stats.error());
    }
    if (!options.quiet) {
      std::cout << "generated " << request.output << "\n";
    }
    std::cout << stats.value().summary() << "\n";
    return 0;
  }

  if (command == "transcode-audio") {
    if (positional.size() != 2) {
      return usage_error("transcode-audio needs an input and an output file");
    }
    if (!options.codec_explicit) {
      encode.codec = "aac";
    }
    erde::AudioTranscodeRequest request;
    request.input = positional[0];
    request.output = positional[1];
    request.encode = encode;
    request.sample_rate = options.sample_rate;
    request.channels = options.channels;
    request.filter = options.audio_filter;
    auto stats = erde::transcode_audio(request);
    if (!stats) {
      return fail(stats.error());
    }
    std::cout << stats.value().summary() << "\n";
    return 0;
  }

  if (command == "transcode" || command == "bench") {
    if (positional.size() != 2) {
      return usage_error(command + " needs an input and an output file");
    }
    erde::TranscodeRequest request;
    request.input = positional[0];
    request.output = positional[1];
    request.encode = encode;
    request.audio_encode = erde::default_audio_encode();
    request.audio_encode.codec = options.audio_codec;
    request.audio_encode.bit_rate = options.audio_bit_rate;
    request.audio_encode.sample_rate = options.sample_rate;
    request.audio_encode.channels = options.channels;
    request.width = options.width;
    request.height = options.height;
    request.filter = options.filter;
    request.audio_filter = options.audio_filter;
    request.include_audio = !options.no_audio;
    request.force_fps = options.fps_explicit;

    if (command == "transcode") {
      auto stats = erde::transcode(request);
      if (!stats) {
        return fail(stats.error());
      }
      std::cout << stats.value().summary() << "\n";
      return 0;
    }

    auto report = erde::bench(request, options.runs);
    if (!report) {
      return fail(report.error());
    }
    std::cout << "runs=" << report.value().runs
              << " frames_per_run=" << report.value().frames_per_run
              << " total_ms=" << report.value().total_ms
              << " fps=" << report.value().frames_per_second << "\n";
    return 0;
  }

  return usage_error("unknown command '" + command + "'");
}
