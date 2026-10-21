// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <avpipe/config.hpp>
#include <avpipe/encode.hpp>
#include <avpipe/pipeline.hpp>

namespace {

constexpr const char* kVersion = "0.1.0";

void print_usage(std::ostream& os) {
  os << "avpipe " << kVersion << " - pull-based audio/video pipeline on libav*\n"
     << "\n"
     << "usage:\n"
     << "  avpipe probe <file>\n"
     << "  avpipe gen <out> [options]\n"
     << "  avpipe transcode <in> <out> [options]\n"
     << "  avpipe bench <in> <out> [--runs N] [options]\n"
     << "\n"
     << "options:\n"
     << "  --codec <name>     encoder name (default libx264; try libx265, mpeg4, mjpeg)\n"
     << "  --bitrate <rate>   2000k / 2M / 1500000 (default 2000000)\n"
     << "  --preset <name>    x264/x265 preset, e.g. veryfast\n"
     << "  --crf <n>          constant rate factor, overrides --bitrate\n"
     << "  --gop <n>          keyframe interval (default 12)\n"
     << "  --fps <n>          frame rate for gen / encoder time base (default 30)\n"
     << "  --size <WxH>       output size; for transcode it inserts a scaler\n"
     << "  --frames <n>       frame count for gen (default 60)\n"
     << "  --config <file>    read defaults from an INI file (see docs/DESIGN.md)\n"
     << "  --quiet            only print the final stats\n"
     << "  --verbose          also show libav's own logging\n";
}

struct Options {
  std::string codec{"libx264"};
  int64_t bit_rate{2'000'000};
  std::string preset{"veryfast"};
  int crf{-1};
  int gop{12};
  int fps{30};
  int frames{60};
  int width{0};
  int height{0};
  int runs{3};
  bool quiet{false};
  bool fps_explicit{false};
};

int fail(const avpipe::Error& error) {
  std::cerr << "avpipe: " << error.to_string() << "\n";
  return 2;
}

int usage_error(const std::string& message) {
  std::cerr << "avpipe: " << message << "\n\n";
  print_usage(std::cerr);
  return 1;
}

bool needs_value(const std::string& flag) {
  return flag == "--codec" || flag == "--bitrate" || flag == "--preset" || flag == "--crf" ||
         flag == "--gop" || flag == "--fps" || flag == "--size" || flag == "--frames" ||
         flag == "--runs" || flag == "--config";
}

avpipe::Status apply_option(Options& options, avpipe::EncodeOptions& encode,
                            const std::string& flag, const std::string& value) {
  using avpipe::fail;
  using avpipe::Code;
  if (flag == "--codec") {
    encode.codec = value;
  } else if (flag == "--bitrate") {
    auto parsed = avpipe::parse_bit_rate(value);
    if (!parsed) {
      return parsed.error();
    }
    encode.bit_rate = parsed.value();
  } else if (flag == "--preset") {
    encode.preset = value;
  } else if (flag == "--crf") {
    auto parsed = avpipe::parse_int(value, "--crf");
    if (!parsed) {
      return parsed.error();
    }
    encode.crf = static_cast<int>(parsed.value());
  } else if (flag == "--gop") {
    auto parsed = avpipe::parse_int(value, "--gop");
    if (!parsed) {
      return parsed.error();
    }
    encode.gop = static_cast<int>(parsed.value());
  } else if (flag == "--fps") {
    auto parsed = avpipe::parse_int(value, "--fps");
    if (!parsed) {
      return parsed.error();
    }
    options.fps = static_cast<int>(parsed.value());
    options.fps_explicit = true;
    encode.fps = options.fps;
  } else if (flag == "--frames") {
    auto parsed = avpipe::parse_int(value, "--frames");
    if (!parsed) {
      return parsed.error();
    }
    options.frames = static_cast<int>(parsed.value());
  } else if (flag == "--runs") {
    auto parsed = avpipe::parse_int(value, "--runs");
    if (!parsed) {
      return parsed.error();
    }
    options.runs = static_cast<int>(parsed.value());
  } else if (flag == "--size") {
    auto parsed = avpipe::parse_size(value);
    if (!parsed) {
      return parsed.error();
    }
    options.width = parsed.value().first;
    options.height = parsed.value().second;
  } else {
    return fail(Code::invalid_argument, "unknown option " + flag);
  }
  return avpipe::ok();
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
  avpipe::EncodeOptions encode;
  std::vector<std::string> positional;
  std::string config_path;
  bool verbose = false;

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
      verbose = true;
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
    auto config = avpipe::Config::load(config_path);
    if (!config) {
      return fail(config.error());
    }
    const auto& cfg = config.value();
    if (cfg.has("encode", "codec")) {
      encode.codec = cfg.get("encode", "codec");
    }
    if (cfg.has("encode", "bitrate")) {
      auto parsed = avpipe::parse_bit_rate(cfg.get("encode", "bitrate"));
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
      auto parsed = avpipe::parse_size(size);
      if (!parsed) {
        return fail(parsed.error());
      }
      options.width = parsed.value().first;
      options.height = parsed.value().second;
    }
  }

  // libav logs encoder banners (x264/x265) straight to stderr; keep the tool's own output clean
  // unless the caller asks for detail.
  av_log_set_level(verbose ? AV_LOG_INFO : AV_LOG_ERROR);

  if (command == "probe") {
    if (positional.size() != 1) {
      return usage_error("probe needs exactly one file");
    }
    auto report = avpipe::probe(positional.front());
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
    avpipe::GenerateRequest request;
    request.output = positional.front();
    request.encode = encode;
    request.width = options.width > 0 ? options.width : 640;
    request.height = options.height > 0 ? options.height : 480;
    request.fps = options.fps;
    request.frames = options.frames;
    auto stats = avpipe::generate(request);
    if (!stats) {
      return fail(stats.error());
    }
    if (!options.quiet) {
      std::cout << "generated " << request.output << "\n";
    }
    std::cout << stats.value().summary() << "\n";
    return 0;
  }

  if (command == "transcode" || command == "bench") {
    if (positional.size() != 2) {
      return usage_error(command + " needs an input and an output file");
    }
    avpipe::TranscodeRequest request;
    request.input = positional[0];
    request.output = positional[1];
    request.encode = encode;
    request.width = options.width;
    request.height = options.height;
    request.force_fps = options.fps_explicit;

    if (command == "transcode") {
      auto stats = avpipe::transcode(request);
      if (!stats) {
        return fail(stats.error());
      }
      std::cout << stats.value().summary() << "\n";
      return 0;
    }

    auto report = avpipe::bench(request, options.runs);
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
