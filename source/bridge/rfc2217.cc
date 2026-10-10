#include "bridge/rfc2217.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>

namespace rfc2217 {
namespace {

// Telnet (RFC 854, 855) and COM-PORT-OPTION (RFC 2217) codes.
constexpr std::uint8_t kSe = 240;
constexpr std::uint8_t kSb = 250;
constexpr std::uint8_t kWill = 251;
constexpr std::uint8_t kWont = 252;
constexpr std::uint8_t kDo = 253;
constexpr std::uint8_t kDont = 254;
constexpr std::uint8_t kIac = 255;

constexpr std::uint8_t kOptionBinary = 0;
constexpr std::uint8_t kOptionSuppressGoAhead = 3;
constexpr std::uint8_t kOptionComPort = 44;

// Client-to-server command codes; the server answers with code + 100.
constexpr std::uint8_t kSignatureCommand = 0;
constexpr std::uint8_t kSetBaudRate = 1;
constexpr std::uint8_t kSetDataSize = 2;
constexpr std::uint8_t kSetParity = 3;
constexpr std::uint8_t kSetStopSize = 4;
constexpr std::uint8_t kSetControl = 5;
constexpr std::uint8_t kNotifyLineState = 6;
constexpr std::uint8_t kFlowControlSuspend = 8;
constexpr std::uint8_t kFlowControlResume = 9;
constexpr std::uint8_t kSetLineStateMask = 10;
constexpr std::uint8_t kSetModemStateMask = 11;
constexpr std::uint8_t kPurgeData = 12;
constexpr std::uint8_t kServerOffset = 100;

void PushCommand(ByteSink& reply, std::uint8_t verb, std::uint8_t option) {
  reply.Push(kIac);
  reply.Push(verb);
  reply.Push(option);
}

// IAC SB COM-PORT-OPTION <code> <value, IAC doubled> IAC SE.
void PushComPort(ByteSink& reply, std::uint8_t code,
                 std::span<const std::uint8_t> value) {
  reply.Push(kIac);
  reply.Push(kSb);
  reply.Push(kOptionComPort);
  reply.Push(code);
  for (std::uint8_t byte : value) {
    reply.Push(byte);
    if (byte == kIac) {
      reply.Push(kIac);
    }
  }
  reply.Push(kIac);
  reply.Push(kSe);
}

void PushComPort(ByteSink& reply, std::uint8_t code, std::uint8_t value) {
  PushComPort(reply, code, std::span(&value, 1));
}

std::uint32_t LoadBigEndian(std::span<const std::uint8_t> bytes) {
  return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
         (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
}

}  // namespace

void Session::Start(ByteSink& reply) noexcept {
  for (const auto& [option, options] :
       {std::pair{kOptionBinary, &binary_},
        std::pair{kOptionSuppressGoAhead, &suppress_go_ahead_}}) {
    options->local = Option::kWantYes;
    PushCommand(reply, kWill, option);
    options->remote = Option::kWantYes;
    PushCommand(reply, kDo, option);
  }
}

std::size_t Session::Receive(std::span<const std::uint8_t> input,
                             ByteSink& data, ByteSink& reply) noexcept {
  std::size_t consumed = 0;
  for (std::uint8_t byte : input) {
    if (reply.free() < kMaxReplySize) {
      break;
    }
    switch (state_) {
      case State::kData:
        if (byte == kIac) {
          state_ = State::kIac;
        } else if (!data.Push(byte)) {
          return consumed;
        }
        break;
      case State::kIac:
        switch (byte) {
          case kIac:  // Escaped 0xFF data byte.
            if (!data.Push(kIac)) {
              return consumed;
            }
            state_ = State::kData;
            break;
          case kWill:
            state_ = State::kWill;
            break;
          case kWont:
            state_ = State::kWont;
            break;
          case kDo:
            state_ = State::kDo;
            break;
          case kDont:
            state_ = State::kDont;
            break;
          case kSb:
            subnegotiation_size_ = 0;
            subnegotiation_truncated_ = false;
            state_ = State::kSubnegotiation;
            break;
          default:  // NOP, GA, AYT, ...: no data, nothing to answer.
            state_ = State::kData;
            break;
        }
        break;
      case State::kWill:
      case State::kWont:
      case State::kDo:
      case State::kDont:
        Negotiate(state_, byte, reply);
        state_ = State::kData;
        break;
      case State::kSubnegotiation:
        if (byte == kIac) {
          state_ = State::kSubnegotiationIac;
        } else if (subnegotiation_size_ < subnegotiation_.size()) {
          subnegotiation_[subnegotiation_size_++] = byte;
        } else {
          subnegotiation_truncated_ = true;
        }
        break;
      case State::kSubnegotiationIac:
        if (byte == kSe) {
          HandleSubnegotiation(reply);
          state_ = State::kData;
        } else {
          // IAC IAC inside SB is a 0xFF value byte.
          if (subnegotiation_size_ < subnegotiation_.size()) {
            subnegotiation_[subnegotiation_size_++] = byte;
          } else {
            subnegotiation_truncated_ = true;
          }
          state_ = State::kSubnegotiation;
        }
        break;
    }
    ++consumed;
  }
  return consumed;
}

std::size_t Session::Escape(std::span<const std::uint8_t> input,
                            ByteSink& output) noexcept {
  std::size_t consumed = 0;
  for (std::uint8_t byte : input) {
    const std::size_t needed = byte == kIac ? 2 : 1;
    if (output.free() < needed) {
      break;
    }
    output.Push(byte);
    if (byte == kIac) {
      output.Push(kIac);
    }
    ++consumed;
  }
  return consumed;
}

void Session::NotifyLineState(std::uint8_t state, ByteSink& reply) noexcept {
  const std::uint8_t reported = state & line_state_mask_;
  if (reported != 0) {
    PushComPort(reply, kNotifyLineState + kServerOffset, reported);
  }
}

Session::Options* Session::Find(std::uint8_t option) noexcept {
  switch (option) {
    case kOptionBinary:
      return &binary_;
    case kOptionSuppressGoAhead:
      return &suppress_go_ahead_;
    case kOptionComPort:
      return &com_port_;
    default:
      return nullptr;
  }
}

// RFC 1143 without the queue: agree to what we support, refuse the rest,
// and answer only state changes, so negotiation cannot loop.
void Session::Negotiate(State verb, std::uint8_t option,
                        ByteSink& reply) noexcept {
  Options* options = Find(option);
  switch (verb) {
    case State::kWill:  // The client offers to perform `option`.
      if (options == nullptr) {
        PushCommand(reply, kDont, option);
      } else if (options->remote == Option::kNo) {
        options->remote = Option::kYes;
        PushCommand(reply, kDo, option);
      } else {
        options->remote = Option::kYes;  // Acknowledges our DO.
      }
      break;
    case State::kWont:
      if (options != nullptr && options->remote != Option::kNo) {
        if (options->remote == Option::kYes) {
          PushCommand(reply, kDont, option);
        }
        options->remote = Option::kNo;
      }
      break;
    case State::kDo:  // The client asks us to perform `option`.
      if (options == nullptr) {
        PushCommand(reply, kWont, option);  // E.g. ECHO.
      } else if (options->local == Option::kNo) {
        options->local = Option::kYes;
        PushCommand(reply, kWill, option);
      } else {
        options->local = Option::kYes;
      }
      break;
    case State::kDont:
      if (options != nullptr && options->local != Option::kNo) {
        if (options->local == Option::kYes) {
          PushCommand(reply, kWont, option);
        }
        options->local = Option::kNo;
      }
      break;
    default:
      break;
  }
}

void Session::HandleSubnegotiation(ByteSink& reply) noexcept {
  if (subnegotiation_size_ < 2 || subnegotiation_[0] != kOptionComPort) {
    return;  // Other options' subnegotiation is not supported.
  }
  const std::span<const std::uint8_t> value =
      std::span<const std::uint8_t>(subnegotiation_)
          .subspan(2, subnegotiation_size_ - 2);
  HandleComPort(subnegotiation_[1], value, reply);
}

bool Session::TryApply(const LineSettings& settings) noexcept {
  if (settings == settings_) {
    return true;
  }
  if (handler_.apply == nullptr ||
      !handler_.apply(handler_.context, settings)) {
    return false;
  }
  settings_ = settings;
  return true;
}

void Session::HandleComPort(std::uint8_t command,
                            std::span<const std::uint8_t> value,
                            ByteSink& reply) noexcept {
  const std::uint8_t answer = command + kServerOffset;
  const std::uint8_t first = value.empty() ? 0 : value[0];
  LineSettings wanted = settings_;
  switch (command) {
    case kSignatureCommand:
      if (value.empty() && !subnegotiation_truncated_) {
        PushComPort(reply, answer,
                    {reinterpret_cast<const std::uint8_t*>(kSignature.data()),
                     kSignature.size()});
      }
      return;  // A client's own signature needs no answer.
    case kSetBaudRate: {
      if (value.size() != 4) {
        return;
      }
      if (const std::uint32_t baud = LoadBigEndian(value); baud != 0) {
        wanted.baud_rate = baud;
        TryApply(wanted);
      }
      const std::uint32_t baud = settings_.baud_rate;
      const std::array<std::uint8_t, 4> bytes{
          static_cast<std::uint8_t>(baud >> 24),
          static_cast<std::uint8_t>(baud >> 16),
          static_cast<std::uint8_t>(baud >> 8),
          static_cast<std::uint8_t>(baud)};
      PushComPort(reply, answer, bytes);
      return;
    }
    case kSetDataSize:
      if (first >= 5 && first <= 8) {
        wanted.data_bits = first;
        TryApply(wanted);
      }
      PushComPort(reply, answer, settings_.data_bits);
      return;
    case kSetParity:
      if (first >= 1 && first <= 5) {
        wanted.parity = static_cast<Parity>(first);
        TryApply(wanted);
      }
      PushComPort(reply, answer,
                  static_cast<std::uint8_t>(settings_.parity));
      return;
    case kSetStopSize:
      if (first >= 1 && first <= 3) {
        wanted.stop_bits = static_cast<StopBits>(first);
        TryApply(wanted);
      }
      PushComPort(reply, answer,
                  static_cast<std::uint8_t>(settings_.stop_bits));
      return;
    case kSetControl: {
      std::uint8_t result = first;
      if (first >= 1 && first <= 3) {  // Outbound (and both) flow control.
        wanted.flow_control = static_cast<FlowControl>(first);
        TryApply(wanted);
        result = static_cast<std::uint8_t>(settings_.flow_control);
      } else if (first == 0 || (first >= 17 && first <= 19)) {
        result = static_cast<std::uint8_t>(settings_.flow_control);
      } else if (first >= 13 && first <= 16) {  // Inbound: same setting.
        result = settings_.flow_control == FlowControl::kHardware ? 16 : 14;
      } else if (first == 4 || first == 5 || first == 6) {
        result = 6;  // BREAK is not supported: always off.
      }
      // DTR (7-9) and RTS (10-12): no such lines are wired; the requested
      // state is confirmed so clients that set them on open carry on.
      PushComPort(reply, answer, result);
      return;
    }
    case kFlowControlSuspend:
      suspended_ = true;
      PushComPort(reply, answer, std::span<const std::uint8_t>{});
      return;
    case kFlowControlResume:
      suspended_ = false;
      PushComPort(reply, answer, std::span<const std::uint8_t>{});
      return;
    case kSetLineStateMask:
      line_state_mask_ = first;
      PushComPort(reply, answer, first);
      return;
    case kSetModemStateMask:  // No modem lines: never notified.
      PushComPort(reply, answer, first);
      return;
    case kPurgeData:
      if (first >= 1 && first <= 3 && handler_.purge != nullptr) {
        handler_.purge(handler_.context, first != 2, first != 1);
      }
      PushComPort(reply, answer, first);
      return;
    default:
      return;  // Server-to-client codes and unknown commands.
  }
}

}  // namespace rfc2217
