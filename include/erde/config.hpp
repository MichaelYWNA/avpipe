// SPDX-License-Identifier: MIT
#ifndef ERDE_CONFIG_HPP
#define ERDE_CONFIG_HPP

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <erde/error.hpp>

namespace erde {

/// Tiny INI-style configuration reader (sections, `key = value`, `#` comments).
///
/// Written by hand instead of pulling in yaml-cpp or a JSON library: a pipeline description is
/// flat key/value data, and 150 lines of parser with line-numbered errors is easier to reason
/// about (and to unit test) than a dependency tree.
class Config {
 public:
  using Section = std::map<std::string, std::string>;
  using Sections = std::map<std::string, Section>;

  static Expected<Config> parse(std::string_view text);
  static Expected<Config> load(const std::string& path);

  const Sections& sections() const { return sections_; }
  bool has(const std::string& section, const std::string& key) const;

  /// Returns `fallback` when the key is absent.
  std::string get(const std::string& section, const std::string& key,
                  const std::string& fallback = {}) const;

  Expected<std::string> get_required(const std::string& section, const std::string& key) const;
  Expected<int64_t> get_int(const std::string& section, const std::string& key,
                            int64_t fallback) const;

 private:
  Sections sections_;
};

/// "1280x720" / "1280X720" -> {1280, 720}.
Expected<std::pair<int, int>> parse_size(const std::string& text);
/// "2M", "2000k", "1500000" -> bits per second.
Expected<int64_t> parse_bit_rate(const std::string& text);
/// Whole string must be a non-negative integer.
Expected<int64_t> parse_int(const std::string& text, const char* what);

}  // namespace erde

#endif  // ERDE_CONFIG_HPP
