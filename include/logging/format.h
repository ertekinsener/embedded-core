#ifndef SHARED_INCLUDE_LOGGING_FORMAT_
#define SHARED_INCLUDE_LOGGING_FORMAT_

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace logging {

// Small, allocation-free formatter for log messages, a subset of std::format:
//
//   "{}"      next argument, default form
//   "{:x}"    hex (also "X"), integers and pointers
//   "{:d}"    decimal, integers
//   "{:8}"    right-aligned in 8 columns; "{:08x}" pads with zeros
//   "{{" "}}" literal braces
//
// Arguments: signed/unsigned integers, enums (as their value), bool, char,
// std::string_view / const char*, and pointers ("0x..."). No floating point.
// The format string is checked at compile time against the argument types.

enum class ArgKind : std::uint8_t {
  kSigned,
  kUnsigned,
  kBool,
  kChar,
  kString,
  kPointer,
};

// One type-erased argument. Built on the caller's stack; formatting runs in
// one non-template function so each call site costs little code.
struct Arg {
  ArgKind kind = ArgKind::kUnsigned;
  std::uint8_t size = 8;        // sizeof the original integer (hex width)
  std::uint64_t value = 0;      // integers, bool, char, pointer
  std::string_view text = {};   // kString
};

template <typename T>
concept Formattable =
    std::integral<std::remove_cvref_t<T>> ||
    std::is_enum_v<std::remove_cvref_t<T>> ||
    std::convertible_to<const T&, std::string_view> ||
    std::is_pointer_v<std::remove_cvref_t<T>>;

namespace detail {

template <typename T>
consteval ArgKind KindOf() {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::same_as<U, bool>) {
    return ArgKind::kBool;
  } else if constexpr (std::same_as<U, char>) {
    return ArgKind::kChar;
  } else if constexpr (std::is_enum_v<U>) {
    return std::is_signed_v<std::underlying_type_t<U>> ? ArgKind::kSigned
                                                       : ArgKind::kUnsigned;
  } else if constexpr (std::integral<U>) {
    return std::is_signed_v<U> ? ArgKind::kSigned : ArgKind::kUnsigned;
  } else if constexpr (std::convertible_to<const T&, std::string_view>) {
    return ArgKind::kString;
  } else {
    return ArgKind::kPointer;
  }
}

// Parsed "{...}" replacement field.
struct Spec {
  bool zero_pad = false;
  std::uint8_t width = 0;
  char type = '\0';  // '\0', 'd', 'x' or 'X'
};

inline constexpr std::uint8_t kMaxWidth = 32;

// Parses the replacement field that starts after the '{' at `pos`, and
// leaves `pos` after its '}'. Returns nullopt if it is malformed.
constexpr std::optional<Spec> ParseSpec(std::string_view format,
                                        std::size_t& pos) noexcept {
  Spec spec;
  if (pos < format.size() && format[pos] == ':') {
    ++pos;
    if (pos < format.size() && format[pos] == '0') {
      spec.zero_pad = true;
      ++pos;
    }
    unsigned width = 0;
    while (pos < format.size() && format[pos] >= '0' && format[pos] <= '9') {
      width = width * 10 + static_cast<unsigned>(format[pos] - '0');
      if (width > kMaxWidth) return std::nullopt;
      ++pos;
    }
    spec.width = static_cast<std::uint8_t>(width);
    if (pos < format.size() &&
        (format[pos] == 'd' || format[pos] == 'x' || format[pos] == 'X')) {
      spec.type = format[pos++];
    }
  }
  if (pos >= format.size() || format[pos] != '}') return std::nullopt;
  ++pos;
  return spec;
}

constexpr bool IsNumeric(ArgKind kind) noexcept {
  return kind == ArgKind::kSigned || kind == ArgKind::kUnsigned ||
         kind == ArgKind::kPointer;
}

// Deliberately not constexpr: reaching it while checking a format string at
// compile time makes the call ill-formed, and the compiler error names it.
void FormatStringError(const char* reason);

template <std::size_t N>
consteval void CheckFormat(std::string_view format,
                           const std::array<ArgKind, N>& kinds) {
  std::size_t next = 0;
  for (std::size_t pos = 0; pos < format.size();) {
    const char c = format[pos++];
    if (c == '}') {
      if (pos < format.size() && format[pos] == '}') {
        ++pos;
        continue;
      }
      FormatStringError("unmatched '}' (write \"}}\")");
    }
    if (c != '{') continue;
    if (pos < format.size() && format[pos] == '{') {
      ++pos;
      continue;
    }
    const std::optional<Spec> spec = ParseSpec(format, pos);
    if (!spec) FormatStringError("malformed replacement field");
    if (next >= N) FormatStringError("more {} fields than arguments");
    if (spec->type != '\0' && !IsNumeric(kinds[next])) {
      FormatStringError("d/x/X needs an integer or pointer argument");
    }
    ++next;
  }
  if (next != N) FormatStringError("fewer {} fields than arguments");
}

}  // namespace detail

// A format string checked at compile time against Args. Like
// std::format_string: deduction happens on the arguments only.
template <typename... Args>
class BasicFormatString {
 public:
  template <typename S>
    requires std::convertible_to<const S&, std::string_view>
  consteval BasicFormatString(const S& text)  // NOLINT: implicit by design
      : text_(text) {
    detail::CheckFormat(text_, std::array<ArgKind, sizeof...(Args)>{
                                   detail::KindOf<Args>()...});
  }

  constexpr std::string_view get() const noexcept { return text_; }

 private:
  std::string_view text_;
};

template <typename... Args>
using FormatString = BasicFormatString<std::type_identity_t<Args>...>;

template <Formattable T>
constexpr Arg MakeArg(const T& value) noexcept {
  using U = std::remove_cvref_t<T>;
  constexpr ArgKind kind = detail::KindOf<T>();
  if constexpr (kind == ArgKind::kString) {
    if constexpr (std::is_pointer_v<U>) {
      if (value == nullptr) return {.kind = kind, .text = "(null)"};
    }
    return {.kind = kind, .text = std::string_view(value)};
  } else if constexpr (kind == ArgKind::kPointer) {
    return {.kind = kind,
            .value = reinterpret_cast<std::uintptr_t>(value)};
  } else if constexpr (std::is_enum_v<U>) {
    return {.kind = kind,
            .size = sizeof(U),
            .value = static_cast<std::uint64_t>(std::to_underlying(value))};
  } else {
    // Signed values are sign-extended; `size` lets hex print only their bits.
    return {.kind = kind,
            .size = sizeof(U),
            .value = static_cast<std::uint64_t>(value)};
  }
}

// Appends text to a fixed buffer and remembers whether anything was cut.
class Writer {
 public:
  explicit Writer(std::span<char> buffer) noexcept : buffer_(buffer) {}

  void Append(char c) noexcept {
    if (size_ < buffer_.size()) {
      buffer_[size_++] = c;
    } else {
      truncated_ = true;
    }
  }
  void Append(std::string_view text) noexcept {
    for (const char c : text) Append(c);
  }
  void AppendRepeated(char c, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) Append(c);
  }
  // Unsigned number in base 10 or 16, padded to `width`.
  void AppendNumber(std::uint64_t value, unsigned base, bool upper,
                    std::size_t width, char pad) noexcept;

  std::size_t size() const noexcept { return size_; }
  bool truncated() const noexcept { return truncated_; }
  std::span<char> buffer() const noexcept { return buffer_; }

 private:
  std::span<char> buffer_;
  std::size_t size_ = 0;
  bool truncated_ = false;
};

// Formats `format` with already checked arguments.
void FormatTo(Writer& out, std::string_view format,
              std::span<const Arg> args) noexcept;

// Convenience for callers outside the logger (and tests).
template <typename... Args>
std::size_t FormatTo(std::span<char> buffer, FormatString<Args...> format,
                     const Args&... args) noexcept {
  Writer out(buffer);
  const std::array<Arg, sizeof...(Args)> erased{MakeArg(args)...};
  FormatTo(out, format.get(), erased);
  return out.size();
}

}  // namespace logging

#endif  // SHARED_INCLUDE_LOGGING_FORMAT_
