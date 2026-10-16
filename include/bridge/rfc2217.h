#ifndef EMBEDDED_CORE_INCLUDE_BRIDGE_RFC2217_
#define EMBEDDED_CORE_INCLUDE_BRIDGE_RFC2217_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Server side of Telnet with the COM Port Control Option (RFC 2217) for the
// Ethernet-UART bridge (docs/bridge/README.md): splits the byte stream from
// the client into UART data and Telnet commands, answers option negotiation
// and COM-PORT commands, and escapes UART data for the client. No I/O and no
// RTOS: the bridge feeds it bytes and sends what it produces.
namespace rfc2217 {

// Values as on the wire (RFC 2217, SET-PARITY etc.).
enum class Parity : std::uint8_t {
  kNone = 1,
  kOdd = 2,
  kEven = 3,
  kMark = 4,
  kSpace = 5,
};
enum class StopBits : std::uint8_t { kOne = 1, kTwo = 2, kOneAndHalf = 3 };
enum class FlowControl : std::uint8_t {
  kNone = 1,
  kXonXoff = 2,
  kHardware = 3,
};

struct LineSettings {
  std::uint32_t baud_rate = 115'200;
  std::uint8_t data_bits = 8;  // 5..8, not counting parity
  Parity parity = Parity::kNone;
  StopBits stop_bits = StopBits::kOne;
  FlowControl flow_control = FlowControl::kNone;

  friend bool operator==(const LineSettings&,
                         const LineSettings&) = default;
};

// NOTIFY-LINESTATE bits the bridge can report.
inline constexpr std::uint8_t kLineOverrun = 0x02;
inline constexpr std::uint8_t kLineParityError = 0x04;
inline constexpr std::uint8_t kLineFramingError = 0x08;
inline constexpr std::uint8_t kLineBreak = 0x10;

struct Handler {
  // Applies `settings` to the port; false keeps the current ones, which are
  // then reported back to the client as the value in use.
  bool (*apply)(void* context, const LineSettings& settings) noexcept =
      nullptr;
  // PURGE-DATA: drop buffered data of the port.
  void (*purge)(void* context, bool receive, bool transmit) noexcept =
      nullptr;
  void* context = nullptr;
};

// Bytes appended to a fixed buffer.
class ByteSink {
 public:
  explicit ByteSink(std::span<std::uint8_t> buffer) noexcept
      : buffer_(buffer) {}

  bool Push(std::uint8_t byte) noexcept {
    if (size_ == buffer_.size()) {
      return false;
    }
    buffer_[size_++] = byte;
    return true;
  }
  std::size_t free() const noexcept { return buffer_.size() - size_; }
  std::span<const std::uint8_t> data() const noexcept {
    return std::span<const std::uint8_t>(buffer_).first(size_);
  }
  void Clear() noexcept { size_ = 0; }

 private:
  std::span<std::uint8_t> buffer_;
  std::size_t size_ = 0;
};

class Session {
 public:
  // Room `reply` must have before Receive() takes another byte: the
  // longest reply to one byte of input (a COM-PORT answer, every byte
  // escaped, or a SIGNATURE).
  static constexpr std::size_t kMaxReplySize = 64;

  // The SIGNATURE reported to clients is cut to this length, so that its
  // answer (every byte escaped) fits kMaxReplySize.
  static constexpr std::size_t kMaxSignatureSize = 24;
  static constexpr std::string_view kDefaultSignature = "rfc2217";

  // `signature` must outlive the session (e.g. a string literal).
  Session(const LineSettings& current, const Handler& handler,
          std::string_view signature = kDefaultSignature) noexcept
      : settings_(current),
        handler_(handler),
        signature_(signature.substr(0, kMaxSignatureSize)) {}

  // The option offers to send when the connection opens (BINARY and
  // SUPPRESS-GO-AHEAD in both directions).
  void Start(ByteSink& reply) noexcept;

  // Processes bytes from the client. UART data goes to `data`, protocol
  // answers to `reply`. Stops early when `data` is full or `reply` has less
  // than kMaxReplySize free; returns the number of input bytes consumed.
  std::size_t Receive(std::span<const std::uint8_t> input, ByteSink& data,
                      ByteSink& reply) noexcept;

  // Copies UART data for the client, doubling 0xFF (IAC). Returns the
  // number of input bytes consumed (stops when `output` is full).
  static std::size_t Escape(std::span<const std::uint8_t> input,
                            ByteSink& output) noexcept;

  // Appends NOTIFY-LINESTATE for the bits of `state` the client asked for
  // (SET-LINESTATE-MASK); nothing if none. `reply` needs kMaxReplySize.
  void NotifyLineState(std::uint8_t state, ByteSink& reply) noexcept;

  // FLOWCONTROL-SUSPEND from the client: stop sending it UART data.
  bool suspended() const noexcept { return suspended_; }
  const LineSettings& settings() const noexcept { return settings_; }
  std::string_view signature() const noexcept { return signature_; }

 private:
  enum class State : std::uint8_t {
    kData,
    kIac,
    kWill,
    kWont,
    kDo,
    kDont,
    kSubnegotiation,
    kSubnegotiationIac,
  };
  // RFC 1143 states, without the queue bits (requests are never revoked).
  enum class Option : std::uint8_t { kNo, kYes, kWantYes };
  struct Options {
    Option local = Option::kNo;   // We perform it (WILL).
    Option remote = Option::kNo;  // The client performs it (DO).
  };

  void Negotiate(State verb, std::uint8_t option, ByteSink& reply) noexcept;
  void HandleSubnegotiation(ByteSink& reply) noexcept;
  void HandleComPort(std::uint8_t command, std::span<const std::uint8_t> value,
                     ByteSink& reply) noexcept;
  bool TryApply(const LineSettings& settings) noexcept;
  Options* Find(std::uint8_t option) noexcept;

  LineSettings settings_;
  Handler handler_;
  std::string_view signature_;
  State state_ = State::kData;
  std::array<std::uint8_t, 32> subnegotiation_{};
  std::size_t subnegotiation_size_ = 0;
  bool subnegotiation_truncated_ = false;
  Options binary_;
  Options suppress_go_ahead_;
  Options com_port_;
  std::uint8_t line_state_mask_ = 0;
  bool suspended_ = false;
};

}  // namespace rfc2217

#endif  // EMBEDDED_CORE_INCLUDE_BRIDGE_RFC2217_
