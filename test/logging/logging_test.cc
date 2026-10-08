// Host tests for logging::FormatTo and the logger front end (line layout,
// level filter, truncation, dropped-line reporting).

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "logging/format.h"
#include "logging/logger.h"

namespace {

int failures = 0;

#define EXPECT_EQ(actual, expected)                                         \
  do {                                                                      \
    const auto a_ = (actual);                                               \
    const auto e_ = (expected);                                             \
    if (!(a_ == e_)) {                                                      \
      std::printf("%s:%d: EXPECT_EQ(%s, %s) failed\n", __FILE__, __LINE__,  \
                  #actual, #expected);                                      \
      ++failures;                                                           \
    }                                                                       \
  } while (false)

template <typename... Args>
std::string Format(logging::FormatString<Args...> format,
                   const Args&... args) {
  std::array<char, 128> buffer{};
  const std::size_t size = logging::FormatTo(buffer, format, args...);
  return std::string(buffer.data(), size);
}

enum class Color : std::uint8_t { kRed = 2 };

void TestFormat() {
  EXPECT_EQ(Format("plain"), "plain");
  EXPECT_EQ(Format("{} {} {}", 1, -2, 3U), "1 -2 3");
  EXPECT_EQ(Format("{:x} {:X} {:08x}", 255, 0xABCU, 0x1234), "ff ABC 00001234");
  EXPECT_EQ(Format("{:x}", std::int32_t{-1}), "ffffffff");
  EXPECT_EQ(Format("{:x}", std::int8_t{-1}), "ff");
  EXPECT_EQ(Format("{:5}|{:05}|{:5}", 42, -42, -42), "   42|-0042|  -42");
  EXPECT_EQ(Format("{}", INT64_MIN), "-9223372036854775808");
  EXPECT_EQ(Format("{}", UINT64_MAX), "18446744073709551615");
  EXPECT_EQ(Format("{} {}", true, false), "true false");
  EXPECT_EQ(Format("{}{}", 'o', 'k'), "ok");
  EXPECT_EQ(Format("[{:6}]", std::string_view("ab")), "[    ab]");
  const char* null_text = nullptr;
  EXPECT_EQ(Format("{} {}", "lit", null_text), "lit (null)");
  EXPECT_EQ(Format("{}", Color::kRed), "2");
  EXPECT_EQ(Format("{:x}", reinterpret_cast<void*>(0x2000'0000)),
            "0x20000000");
  EXPECT_EQ(Format("{{}} {{{}}}", 7), "{} {7}");

  // Truncation: the writer stops at the buffer end and says so.
  std::array<char, 4> small{};
  logging::Writer out(small);
  out.Append("abcdef");
  EXPECT_EQ(out.size(), 4U);
  EXPECT_EQ(out.truncated(), true);

  // Compile-time checks (uncomment one to see the error):
  // Format("{}");               // fewer arguments than fields
  // Format("{}", 1, 2);         // more arguments than fields
  // Format("{:x}", "text");     // hex of a string
  // Format("{:q}", 1);          // malformed field
}

struct FakeSink {
  std::vector<std::string> lines;
  bool full = false;

  static bool Write(void* context, std::span<const std::uint8_t> line) noexcept {
    auto& self = *static_cast<FakeSink*>(context);
    if (self.full) return false;
    self.lines.emplace_back(reinterpret_cast<const char*>(line.data()),
                            line.size());
    return true;
  }
};

std::uint64_t now_us = 0;
std::uint64_t FakeClock() noexcept { return now_us; }

void TestLogger() {
  FakeSink sink;

  logging::Info("x", "before init");  // Counted as dropped.
  EXPECT_EQ(logging::PendingDrops(), 1U);

  logging::Init({.core = "cm7",
                 .sink = {&FakeSink::Write, &sink},
                 .clock_us = &FakeClock,
                 .level = logging::Level::kInfo});
  now_us = 12'345'678;
  logging::Info("eth", "link up, {} Mbit/s", 100);
  EXPECT_EQ(sink.lines.size(), 2U);
  EXPECT_EQ(sink.lines[0], "[   12.345678] W cm7 log: 1 messages dropped\r\n");
  EXPECT_EQ(sink.lines[1], "[   12.345678] I cm7 eth: link up, 100 Mbit/s\r\n");
  EXPECT_EQ(logging::PendingDrops(), 0U);

  // Level filter.
  logging::Debug("x", "hidden");
  EXPECT_EQ(sink.lines.size(), 2U);
  logging::SetLevel(logging::Level::kDebug);
  logging::Debug("x", "shown");
  EXPECT_EQ(sink.lines.size(), 3U);

  // Full sink: lines are counted, then reported before the next one.
  sink.full = true;
  logging::Error("x", "lost {}", 1);
  logging::Error("x", "lost {}", 2);
  EXPECT_EQ(logging::PendingDrops(), 2U);
  sink.full = false;
  now_us = 99'000'001;
  logging::Warning("x", "back");
  EXPECT_EQ(sink.lines.size(), 5U);
  EXPECT_EQ(sink.lines[3], "[   99.000001] W cm7 log: 2 messages dropped\r\n");
  EXPECT_EQ(sink.lines[4], "[   99.000001] W cm7 x: back\r\n");

  // Truncation keeps the line length and the line end.
  const std::string long_text(300, 'a');
  logging::Info("x", "{}", std::string_view(long_text));
  const std::string& line = sink.lines.back();
  EXPECT_EQ(line.size(), logging::kMaxLineLength);
  EXPECT_EQ(line.substr(line.size() - 5), std::string("...\r\n"));

  // No clock: no timestamp.
  logging::Init({.core = "cm4", .sink = {&FakeSink::Write, &sink}});
  logging::Info("t", "{}", 1);
  EXPECT_EQ(sink.lines.back(), "I cm4 t: 1\r\n");
}

}  // namespace

int main() {
  TestFormat();
  TestLogger();
  if (failures != 0) {
    std::printf("FAILED: %d\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("OK\n");
  return EXIT_SUCCESS;
}
