// SPDX-License-Identifier: MIT
// avpipe - a small, pull-based audio/video pipeline on top of libav*.
#ifndef AVPIPE_ERROR_HPP
#define AVPIPE_ERROR_HPP

#include <optional>
#include <string>
#include <utility>

namespace avpipe {

/// Coarse classification of a failure. `need_more` and `end_of_stream` are not failures in
/// the pull loop, which is why they live next to the real error codes instead of being
/// signalled by an exception: a stage that has no output *yet* and a stage that has no output
/// *ever* again must be distinguishable at the call site.
enum class Code {
  ok = 0,
  end_of_stream,   ///< no more data will ever come out of this stage
  need_more,       ///< nothing available right now, but the stage is not finished
  invalid_argument,
  config,          ///< bad configuration value (file, flag or derived from the input)
  ffmpeg,          ///< a libav call failed; `Error::av_code` carries the AVERROR value
  io,
  unsupported,
};

/// Human readable form of a libav error code, including the "Invalid data found ..." style
/// detail that av_strerror provides.
std::string av_error_string(int av_code);

struct Error {
  Code code = Code::ok;
  int av_code = 0;
  std::string message;

  Error() = default;
  Error(Code c, std::string msg, int av = 0)
      : code(c), av_code(av), message(std::move(msg)) {}

  bool ok() const { return code == Code::ok; }
  explicit operator bool() const { return code != Code::ok; }

  /// "ffmpeg: <message>: <av_strerror>" / "<message>"
  std::string to_string() const;
};

inline Error ok() { return Error{}; }
inline Error fail(Code c, std::string msg) { return Error(c, std::move(msg)); }
inline Error from_av(int av_code, std::string what) {
  return Error(Code::ffmpeg, std::move(what), av_code);
}

/// Minimal `expected` for C++17 (std::expected is C++23). Holds either a value or an Error.
/// Moving out of it is explicit through `take()` so a caller cannot accidentally read a
/// half-moved Frame.
template <class T>
class Expected {
 public:
  Expected(T value) : value_(std::move(value)) {}                  // NOLINT(google-explicit-constructor)
  Expected(Error error) : error_(std::move(error)) {}              // NOLINT(google-explicit-constructor)

  bool has_value() const { return value_.has_value(); }
  explicit operator bool() const { return has_value(); }

  const Error& error() const { return error_; }

  T& value() { return *value_; }
  const T& value() const { return *value_; }
  T take() { return std::move(*value_); }

  T value_or(T fallback) const { return value_ ? *value_ : std::move(fallback); }

 private:
  std::optional<T> value_;
  Error error_;
};

/// Operations that only report success/failure use Error directly; alias for readability.
using Status = Error;

}  // namespace avpipe

#endif  // AVPIPE_ERROR_HPP
