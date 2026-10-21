// SPDX-License-Identifier: MIT
#include <avpipe/config.hpp>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace avpipe {

namespace {

std::string_view trim(std::string_view text) {
  auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

Expected<Config> Config::parse(std::string_view text) {
  Config config;
  std::string section;
  int line_no = 0;

  while (!text.empty()) {
    ++line_no;
    const std::size_t newline = text.find('\n');
    std::string_view line = newline == std::string_view::npos ? text : text.substr(0, newline);
    text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);

    if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
      line = line.substr(0, hash);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    if (line.front() == '[') {
      if (line.back() != ']' || line.size() < 3) {
        return fail(Code::config, "line " + std::to_string(line_no) + ": malformed section header");
      }
      section = std::string(trim(line.substr(1, line.size() - 2)));
      if (section.empty()) {
        return fail(Code::config, "line " + std::to_string(line_no) + ": empty section name");
      }
      config.sections_[section];  // create even when the section stays empty
      continue;
    }

    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      return fail(Code::config,
                  "line " + std::to_string(line_no) + ": expected 'key = value'");
    }
    const std::string key(trim(line.substr(0, eq)));
    const std::string value(trim(line.substr(eq + 1)));
    if (key.empty()) {
      return fail(Code::config, "line " + std::to_string(line_no) + ": empty key");
    }
    if (section.empty()) {
      return fail(Code::config, "line " + std::to_string(line_no) + ": key '" + key +
                                    "' outside of any section");
    }
    config.sections_[section][key] = value;
  }

  return config;
}

Expected<Config> Config::load(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return fail(Code::io, "cannot read config '" + path + "'");
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  auto parsed = parse(buffer.str());
  if (!parsed) {
    return fail(Code::config, path + ": " + parsed.error().message);
  }
  return parsed.take();
}

bool Config::has(const std::string& section, const std::string& key) const {
  const auto sec = sections_.find(section);
  if (sec == sections_.end()) {
    return false;
  }
  return sec->second.find(key) != sec->second.end();
}

std::string Config::get(const std::string& section, const std::string& key,
                        const std::string& fallback) const {
  const auto sec = sections_.find(section);
  if (sec == sections_.end()) {
    return fallback;
  }
  const auto it = sec->second.find(key);
  return it == sec->second.end() ? fallback : it->second;
}

Expected<std::string> Config::get_required(const std::string& section,
                                           const std::string& key) const {
  if (!has(section, key)) {
    return fail(Code::config, "missing '" + key + "' in [" + section + "]");
  }
  return get(section, key);
}

Expected<int64_t> Config::get_int(const std::string& section, const std::string& key,
                                  int64_t fallback) const {
  if (!has(section, key)) {
    return fallback;
  }
  return parse_int(get(section, key), ("[" + section + "] " + key).c_str());
}

Expected<int64_t> parse_int(const std::string& text, const char* what) {
  if (text.empty()) {
    return fail(Code::config, std::string(what) + ": empty value");
  }
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0') {
    return fail(Code::config, std::string(what) + ": '" + text + "' is not an integer");
  }
  return static_cast<int64_t>(value);
}

Expected<std::pair<int, int>> parse_size(const std::string& text) {
  const std::size_t x = text.find_first_of("xX");
  if (x == std::string::npos) {
    return fail(Code::config, "size '" + text + "' must look like 1280x720");
  }
  auto width = parse_int(text.substr(0, x), "size width");
  auto height = parse_int(text.substr(x + 1), "size height");
  if (!width) {
    return width.error();
  }
  if (!height) {
    return height.error();
  }
  if (width.value() <= 0 || height.value() <= 0) {
    return fail(Code::config, "size '" + text + "' must be positive");
  }
  return std::make_pair(static_cast<int>(width.value()), static_cast<int>(height.value()));
}

Expected<int64_t> parse_bit_rate(const std::string& text) {
  if (text.empty()) {
    return fail(Code::config, "bit rate is empty");
  }
  const char suffix = text.back();
  int64_t multiplier = 1;
  std::string digits = text;
  if (suffix == 'k' || suffix == 'K') {
    multiplier = 1000;
    digits.pop_back();
  } else if (suffix == 'm' || suffix == 'M') {
    multiplier = 1000 * 1000;
    digits.pop_back();
  }
  auto value = parse_int(digits, "bit rate");
  if (!value) {
    return value.error();
  }
  if (value.value() < 0) {
    return fail(Code::config, "bit rate must not be negative");
  }
  return value.value() * multiplier;
}

}  // namespace avpipe
