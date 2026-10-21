// SPDX-License-Identifier: MIT
#include "harness.hpp"

#include <erde/config.hpp>

TEST_CASE(parses_sections_and_values) {
  const char* text =
      "# pipeline description\n"
      "[video]\n"
      "size = 1280x720   # trailing comment\n"
      "fps = 30\n"
      "\n"
      "[encode]\n"
      "codec=libx264\n"
      "preset = veryfast\n";

  auto parsed = erde::Config::parse(text);
  CHECK(parsed.has_value());
  const auto& config = parsed.value();
  CHECK(config.has("video", "size"));
  CHECK_EQ(config.get("video", "size"), std::string("1280x720"));
  CHECK_EQ(config.get_int("video", "fps", 0).value(), int64_t{30});
  CHECK_EQ(config.get("encode", "codec"), std::string("libx264"));
  CHECK_EQ(config.get("encode", "missing", "fallback"), std::string("fallback"));
  CHECK(!config.has("encode", "size"));
}

TEST_CASE(rejects_key_outside_section) {
  auto parsed = erde::Config::parse("size = 1280x720\n");
  CHECK(!parsed.has_value());
  CHECK(parsed.error().code == erde::Code::config);
  CHECK(parsed.error().message.find("outside") != std::string::npos);
}

TEST_CASE(rejects_malformed_line) {
  auto parsed = erde::Config::parse("[video]\n1280x720\n");
  CHECK(!parsed.has_value());
  CHECK(parsed.error().message.find("line 2") != std::string::npos);
}

TEST_CASE(rejects_bad_section_header) {
  auto parsed = erde::Config::parse("[video\nsize = 1x1\n");
  CHECK(!parsed.has_value());
  CHECK(parsed.error().message.find("section") != std::string::npos);
}

TEST_CASE(parses_sizes) {
  auto size = erde::parse_size("1920x1080");
  CHECK(size.has_value());
  CHECK_EQ(size.value().first, 1920);
  CHECK_EQ(size.value().second, 1080);

  CHECK(!erde::parse_size("1920").has_value());
  CHECK(!erde::parse_size("0x10").has_value());
  CHECK(!erde::parse_size("axb").has_value());
}

TEST_CASE(parses_bit_rates) {
  CHECK_EQ(erde::parse_bit_rate("2000k").value(), int64_t{2'000'000});
  CHECK_EQ(erde::parse_bit_rate("2M").value(), int64_t{2'000'000});
  CHECK_EQ(erde::parse_bit_rate("1500000").value(), int64_t{1'500'000});
  CHECK(!erde::parse_bit_rate("fast").has_value());
  CHECK(!erde::parse_bit_rate("").has_value());
}

TEST_CASE(reports_missing_required_key) {
  auto parsed = erde::Config::parse("[encode]\ncodec = libx264\n");
  CHECK(parsed.has_value());
  auto required = parsed.value().get_required("encode", "bitrate");
  CHECK(!required.has_value());
  CHECK(required.error().message.find("bitrate") != std::string::npos);
}

TEST_MAIN()
