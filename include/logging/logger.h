#ifndef SHARED_INCLUDE_LOGGING_LOGGER_
#define SHARED_INCLUDE_LOGGING_LOGGER_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "logging/format.h"

namespace logging {

// Logger front end, one instance per core. Each call formats one line on the
// caller's stack and hands it to the sink as a single record:
//
//   [    12.345678] I cm7 eth: link up, 100 Mbit/s
//
// (seconds.microseconds of the shared clock, level, core, tag, message).
// It never blocks and works from any context, including interrupts; if the
// sink is full the line is dropped and counted, and the next line that gets
// through is preceded by "W <core> log: <N> messages dropped".
//
//   logging::Info("eth", "link up, {} Mbit/s", speed);
//   logging::Error("uart", "dma error, isr={:08x}", isr);

enum class Level : std::uint8_t { kError, kWarning, kInfo, kDebug };

// Writes one complete line. Returns false if it could not take it now.
struct Sink {
  using Function = bool (*)(void* context,
                            std::span<const std::uint8_t> line) noexcept;

  Function function = nullptr;
  void* context = nullptr;
};

struct Config {
  std::string_view core;  // e.g. "cm7"; must outlive the logger
  Sink sink;
  std::uint64_t (*clock_us)() noexcept = nullptr;  // nullptr: no timestamp
  Level level = Level::kInfo;
};

// Longest line, including "\r\n". Longer messages end in "...".
inline constexpr std::size_t kMaxLineLength = 160;

// Call once, before any other thread or interrupt logs. Lines logged before
// count as dropped.
void Init(const Config& config) noexcept;

// Lines above `level` are discarded before formatting.
void SetLevel(Level level) noexcept;
Level GetLevel() noexcept;

// Lines lost because the sink was full and not reported yet.
std::uint32_t PendingDrops() noexcept;

namespace detail {

bool Enabled(Level level) noexcept;
void Write(Level level, std::string_view tag, std::string_view format,
           std::span<const Arg> args) noexcept;

template <typename... Args>
void Log(Level level, std::string_view tag, std::string_view format,
         const Args&... args) noexcept {
  if (!Enabled(level)) {
    return;
  }
  const std::array<Arg, sizeof...(Args)> erased{MakeArg(args)...};
  Write(level, tag, format, erased);
}

}  // namespace detail

template <typename... Args>
void Error(std::string_view tag, FormatString<Args...> format,
           const Args&... args) noexcept {
  detail::Log(Level::kError, tag, format.get(), args...);
}

template <typename... Args>
void Warning(std::string_view tag, FormatString<Args...> format,
             const Args&... args) noexcept {
  detail::Log(Level::kWarning, tag, format.get(), args...);
}

template <typename... Args>
void Info(std::string_view tag, FormatString<Args...> format,
          const Args&... args) noexcept {
  detail::Log(Level::kInfo, tag, format.get(), args...);
}

template <typename... Args>
void Debug(std::string_view tag, FormatString<Args...> format,
           const Args&... args) noexcept {
  detail::Log(Level::kDebug, tag, format.get(), args...);
}

}  // namespace logging

#endif  // SHARED_INCLUDE_LOGGING_LOGGER_
