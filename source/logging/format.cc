#include "logging/format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace logging {

void Writer::AppendNumber(std::uint64_t value, unsigned base, bool upper,
                          std::size_t width, char pad) noexcept {
  const char* const digits =
      upper ? "0123456789ABCDEF" : "0123456789abcdef";
  std::array<char, 20> reversed{};  // 2^64 has 20 decimal digits.
  std::size_t count = 0;
  do {
    reversed[count++] = digits[value % base];
    value /= base;
  } while (value != 0);
  if (width > count) AppendRepeated(pad, width - count);
  while (count > 0) Append(reversed[--count]);
}

namespace {

void AppendArg(Writer& out, const Arg& arg, const detail::Spec& spec) noexcept {
  const char pad = spec.zero_pad ? '0' : ' ';
  const bool hex = spec.type == 'x' || spec.type == 'X';
  const bool upper = spec.type == 'X';

  switch (arg.kind) {
    case ArgKind::kSigned: {
      const auto value = static_cast<std::int64_t>(arg.value);
      if (hex) {
        // The two's complement bits of the original type (std::format would
        // print "-1"); register dumps want exactly that.
        const std::uint64_t mask =
            arg.size >= 8 ? ~std::uint64_t{0}
                          : (std::uint64_t{1} << (arg.size * 8)) - 1;
        out.AppendNumber(arg.value & mask, 16, upper, spec.width, pad);
        return;
      }
      if (value >= 0) {
        out.AppendNumber(arg.value, 10, false, spec.width, pad);
        return;
      }
      // Negate in unsigned arithmetic so INT64_MIN works.
      const std::uint64_t magnitude = 0 - arg.value;
      const std::size_t width = spec.width > 0 ? spec.width - 1 : 0;
      if (spec.zero_pad) {
        out.Append('-');
        out.AppendNumber(magnitude, 10, false, width, '0');
      } else {
        // Count digits to place the sign after the spaces.
        std::size_t digits = 1;
        for (std::uint64_t v = magnitude; v >= 10; v /= 10) ++digits;
        if (width > digits) out.AppendRepeated(' ', width - digits);
        out.Append('-');
        out.AppendNumber(magnitude, 10, false, 0, ' ');
      }
      return;
    }
    case ArgKind::kUnsigned:
      out.AppendNumber(arg.value, hex ? 16 : 10, upper, spec.width, pad);
      return;
    case ArgKind::kPointer:
      if (spec.type == 'd') {
        out.AppendNumber(arg.value, 10, false, spec.width, pad);
        return;
      }
      out.Append("0x");
      out.AppendNumber(arg.value, 16, upper, spec.width, pad);
      return;
    case ArgKind::kBool: {
      const std::string_view text = arg.value != 0 ? "true" : "false";
      if (spec.width > text.size()) {
        out.AppendRepeated(' ', spec.width - text.size());
      }
      out.Append(text);
      return;
    }
    case ArgKind::kChar:
      if (spec.width > 1) out.AppendRepeated(' ', spec.width - 1);
      out.Append(static_cast<char>(arg.value));
      return;
    case ArgKind::kString:
      if (spec.width > arg.text.size()) {
        out.AppendRepeated(' ', spec.width - arg.text.size());
      }
      out.Append(arg.text);
      return;
  }
}

}  // namespace

void FormatTo(Writer& out, std::string_view format,
              std::span<const Arg> args) noexcept {
  std::size_t next = 0;
  for (std::size_t pos = 0; pos < format.size();) {
    const char c = format[pos++];
    if ((c == '{' || c == '}') && pos < format.size() && format[pos] == c) {
      out.Append(c);  // "{{" or "}}"
      ++pos;
      continue;
    }
    if (c != '{') {
      out.Append(c);
      continue;
    }
    // Checked at compile time; stay safe if a caller bypassed that.
    const std::optional<detail::Spec> spec = detail::ParseSpec(format, pos);
    if (!spec || next >= args.size()) {
      out.Append("{?}");
      return;
    }
    AppendArg(out, args[next++], *spec);
  }
}

}  // namespace logging
