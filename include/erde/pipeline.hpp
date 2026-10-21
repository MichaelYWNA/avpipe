// SPDX-License-Identifier: MIT
#ifndef ERDE_PIPELINE_HPP
#define ERDE_PIPELINE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <erde/encode.hpp>
#include <erde/error.hpp>
#include <erde/media.hpp>

namespace erde {

/// Counters collected while a pipeline runs. Every field is produced by the stage that owns the
/// work, not estimated afterwards, so "frames in == frames out" is checkable.
struct PipelineStats {
  int64_t packets_read = 0;
  int64_t frames_decoded = 0;
  int64_t frames_converted = 0;
  int64_t frames_encoded = 0;
  int64_t packets_written = 0;
  int64_t audio_frames_decoded = 0;
  int64_t audio_frames_encoded = 0;
  int64_t audio_packets_written = 0;
  int video_streams = 0;
  int audio_streams = 0;
  double wall_ms = 0.0;

  std::string summary() const;
};

struct TranscodeRequest {
  std::string input;
  std::string output;
  EncodeOptions encode;             ///< video encoder settings
  EncodeOptions audio_encode = default_audio_encode();  ///< audio encoder settings
  int width = 0;               ///< 0 keeps the source size
  int height = 0;              ///< 0 keeps the source size
  /// libavfilter description inserted after the video decoder, e.g. "fps=15,crop=640:360".
  std::string filter;
  /// libavfilter description inserted after the audio decoder, e.g. "atempo=2.0,volume=0.5".
  std::string audio_filter;
  bool include_video = true;
  bool include_audio = true;
  /// True when the caller set the frame rate explicitly. Without a filter that changes the frame
  /// rate this pipeline cannot convert 30 fps to 24 fps, so a mismatch is reported instead of
  /// producing a stream whose metadata lies about its cadence.
  bool force_fps = false;
};

struct AudioTranscodeRequest {
  std::string input;
  std::string output;
  EncodeOptions encode = default_audio_encode();
  int sample_rate = 0;  ///< 0 keeps the source rate
  int channels = 0;     ///< 0 keeps the source layout
  std::string filter;   ///< optional libavfilter chain, e.g. "atempo=2.0"
};

struct GenerateRequest {
  std::string output;
  EncodeOptions encode;  ///< video encoder when with_video, audio encoder when audio_only
  EncodeOptions audio_encode = default_audio_encode();
  int width = 640;
  int height = 480;
  int fps = 30;
  int frames = 60;
  /// Tone only (no video stream).
  bool audio_only = false;
  /// Video plus a tone track, muxed into one file.
  bool with_audio = false;
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

/// Multi-stream transcode:
///   video: demux -> decode -> [filter] -> [scale] -> encode
///   audio: demux -> decode -> [filter] -> resample -> slice -> encode
/// both streams then go into one muxer (av_interleaved_write_frame orders the packets).
Expected<PipelineStats> transcode(const TranscodeRequest& request);

/// Audio-only entry point; same chain with the video branch switched off.
Expected<PipelineStats> transcode_audio(const AudioTranscodeRequest& request);

/// Writes a synthetic clip (video pattern, sine tone, or both) so the tool can be exercised and
/// tested without any input file.
Expected<PipelineStats> generate(const GenerateRequest& request);

/// Reads a container and reports its streams and packet counts.
Expected<ProbeReport> probe(const std::string& path);

/// Runs `transcode()` `runs` times and reports the throughput of the whole chain.
Expected<BenchReport> bench(const TranscodeRequest& request, int runs);

}  // namespace erde

#endif  // ERDE_PIPELINE_HPP
