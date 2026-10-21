// SPDX-License-Identifier: MIT
#ifndef AVPIPE_PIPELINE_HPP
#define AVPIPE_PIPELINE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <avpipe/encode.hpp>
#include <avpipe/error.hpp>
#include <avpipe/media.hpp>

namespace avpipe {

/// Counters collected while a pipeline runs. Every field is produced by the stage that owns the
/// work, not estimated afterwards, so "frames in == frames out" is checkable.
struct PipelineStats {
  int64_t packets_read = 0;
  int64_t frames_decoded = 0;
  int64_t frames_converted = 0;
  int64_t frames_encoded = 0;
  int64_t packets_written = 0;
  double wall_ms = 0.0;

  std::string summary() const;
};

struct TranscodeRequest {
  std::string input;
  std::string output;
  EncodeOptions encode;
  int width = 0;   ///< 0 keeps the source size
  int height = 0;  ///< 0 keeps the source size
  /// libavfilter description inserted after the decoder, e.g. "fps=15,crop=640:360".
  std::string filter;
  /// True when the caller set the frame rate explicitly. Without a filter that changes the frame
  /// rate this pipeline cannot convert 30 fps to 24 fps, so a mismatch is reported instead of
  /// producing a stream whose metadata lies about its cadence.
  bool force_fps = false;
};

struct AudioTranscodeRequest {
  std::string input;
  std::string output;
  EncodeOptions encode;
  int sample_rate = 0;  ///< 0 keeps the source rate
  int channels = 0;     ///< 0 keeps the source layout
};

struct GenerateRequest {
  std::string output;
  EncodeOptions encode;
  int width = 640;
  int height = 480;
  int fps = 30;
  int frames = 60;
  /// When true a sine tone is generated instead of a video pattern.
  bool audio = false;
  double seconds = 2.0;
  int sample_rate = 48'000;
  int channels = 2;
};

struct ProbeReport {
  std::string format;
  double duration_s = 0.0;
  int64_t size_bytes = 0;
  int64_t packets = 0;
  std::vector<std::string> lines;
};

struct BenchReport {
  int runs = 0;
  int64_t frames_per_run = 0;
  double total_ms = 0.0;
  double frames_per_second = 0.0;
};

/// demux -> decode -> (filter) -> (scale) -> encode -> mux.
Expected<PipelineStats> transcode(const TranscodeRequest& request);

/// demux -> decode -> resample -> slice -> encode -> mux, for audio-only streams.
Expected<PipelineStats> transcode_audio(const AudioTranscodeRequest& request);

/// Writes a synthetic clip (video pattern or sine tone) so the tool can be exercised and tested
/// without any input file.
Expected<PipelineStats> generate(const GenerateRequest& request);

/// Reads a container and reports its streams and packet counts.
Expected<ProbeReport> probe(const std::string& path);

/// Runs `transcode()` `runs` times and reports the throughput of the whole chain.
Expected<BenchReport> bench(const TranscodeRequest& request, int runs);

}  // namespace avpipe

#endif  // AVPIPE_PIPELINE_HPP
