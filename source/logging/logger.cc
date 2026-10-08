#include "logging/logger.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "logging/format.h"

namespace logging {
namespace {

constexpr std::array<char, 4> kLevelLetters{'E', 'W', 'I', 'D'};
constexpr std::string_view kEllipsis = "...";
constexpr std::string_view kLineEnd = "\r\n";

// Written by Init() before `ready` is set, read-only afterwards.
Config config{};
std::atomic<bool> ready{false};
std::atomic<std::uint8_t> max_level{std::to_underlying(Level::kInfo)};
std::atomic<std::uint32_t> dropped{0};

// Formats one complete line into `buffer` and returns its length.
std::size_t BuildLine(std::span<char, kMaxLineLength> buffer, Level level,
                      std::string_view tag, std::string_view format,
                      std::span<const Arg> args) noexcept {
  Writer out(std::span<char>(buffer).first(kMaxLineLength - kLineEnd.size()));
  if (config.clock_us != nullptr) {
    const std::uint64_t now = config.clock_us();
    out.Append('[');
    out.AppendNumber(now / 1'000'000, 10, false, 5, ' ');
    out.Append('.');
    out.AppendNumber(now % 1'000'000, 10, false, 6, '0');
    out.Append("] ");
  }
  out.Append(kLevelLetters[std::to_underlying(level)]);
  out.Append(' ');
  out.Append(config.core);
  out.Append(' ');
  out.Append(tag);
  out.Append(": ");
  FormatTo(out, format, args);

  std::size_t size = out.size();
  if (out.truncated()) {
    for (std::size_t i = 0; i < kEllipsis.size(); ++i) {
      buffer[size - kEllipsis.size() + i] = kEllipsis[i];
    }
  }
  for (const char c : kLineEnd) buffer[size++] = c;
  return size;
}

bool Emit(std::span<const char> line) noexcept {
  return config.sink.function(
      config.sink.context,
      {reinterpret_cast<const std::uint8_t*>(line.data()), line.size()});
}

}  // namespace

void Init(const Config& new_config) noexcept {
  config = new_config;
  max_level.store(std::to_underlying(new_config.level),
                  std::memory_order_relaxed);
  ready.store(new_config.sink.function != nullptr, std::memory_order_release);
}

void SetLevel(Level level) noexcept {
  max_level.store(std::to_underlying(level), std::memory_order_relaxed);
}

Level GetLevel() noexcept {
  return static_cast<Level>(max_level.load(std::memory_order_relaxed));
}

std::uint32_t PendingDrops() noexcept {
  return dropped.load(std::memory_order_relaxed);
}

namespace detail {

bool Enabled(Level level) noexcept {
  return std::to_underlying(level) <=
         max_level.load(std::memory_order_relaxed);
}

void Write(Level level, std::string_view tag, std::string_view format,
           std::span<const Arg> args) noexcept {
  if (!ready.load(std::memory_order_acquire)) {
    dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::array<char, kMaxLineLength> buffer;

  // Report earlier losses first, so the gap shows up where it happened.
  if (const std::uint32_t lost = dropped.load(std::memory_order_relaxed);
      lost != 0) {
    const std::array<Arg, 1> count{MakeArg(lost)};
    const std::size_t size = BuildLine(buffer, Level::kWarning, "log",
                                       "{} messages dropped", count);
    if (!Emit(std::span(buffer).first(size))) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      return;  // Still full: this line would be lost too.
    }
    // Drops counted meanwhile stay for the next report.
    dropped.fetch_sub(lost, std::memory_order_relaxed);
  }

  const std::size_t size = BuildLine(buffer, level, tag, format, args);
  if (!Emit(std::span(buffer).first(size))) {
    dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace detail

}  // namespace logging
