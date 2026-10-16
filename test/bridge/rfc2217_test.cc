// Host tests for rfc2217::Session: option negotiation, IAC escaping in both
// directions, COM-PORT commands with accepted and refused settings, input
// split at any byte, full output buffers.

#include "bridge/rfc2217.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

int failures = 0;

#define EXPECT(condition)                                            \
  do {                                                               \
    if (!(condition)) {                                              \
      std::printf("%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__,  \
                  #condition);                                       \
      ++failures;                                                    \
    }                                                                \
  } while (false)

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint8_t kIac = 255;
constexpr std::uint8_t kSb = 250;
constexpr std::uint8_t kSe = 240;
constexpr std::uint8_t kWill = 251;
constexpr std::uint8_t kWont = 252;
constexpr std::uint8_t kDo = 253;
constexpr std::uint8_t kDont = 254;

Bytes ComPort(std::uint8_t code, Bytes value) {
  Bytes bytes{kIac, kSb, 44, code};
  for (std::uint8_t byte : value) {
    bytes.push_back(byte);
    if (byte == kIac) {
      bytes.push_back(kIac);
    }
  }
  bytes.push_back(kIac);
  bytes.push_back(kSe);
  return bytes;
}

// A port that accepts 7 or 8 data bits, no mark/space parity, no XON/XOFF.
struct FakePort {
  int applies = 0;
  int purges = 0;
  bool purge_receive = false;
  bool purge_transmit = false;
  rfc2217::LineSettings last;

  static bool Apply(void* context,
                    const rfc2217::LineSettings& settings) noexcept {
    auto& port = *static_cast<FakePort*>(context);
    ++port.applies;
    if (settings.data_bits < 7 || settings.parity == rfc2217::Parity::kMark ||
        settings.parity == rfc2217::Parity::kSpace ||
        settings.flow_control == rfc2217::FlowControl::kXonXoff) {
      return false;
    }
    port.last = settings;
    return true;
  }

  static void Purge(void* context, bool receive, bool transmit) noexcept {
    auto& port = *static_cast<FakePort*>(context);
    ++port.purges;
    port.purge_receive = receive;
    port.purge_transmit = transmit;
  }

  rfc2217::Handler handler() { return {&Apply, &Purge, this}; }
};

struct Result {
  Bytes data;
  Bytes reply;
};

// Feeds `input` one byte per call when `bytewise`, all at once otherwise.
Result Feed(rfc2217::Session& session, const Bytes& input,
            bool bytewise = false) {
  std::array<std::uint8_t, 256> data_buffer;
  std::array<std::uint8_t, 256> reply_buffer;
  rfc2217::ByteSink data(data_buffer);
  rfc2217::ByteSink reply(reply_buffer);
  std::size_t offset = 0;
  while (offset < input.size()) {
    const std::size_t take = bytewise ? 1 : input.size() - offset;
    const std::size_t consumed = session.Receive(
        std::span(input).subspan(offset, take), data, reply);
    EXPECT(consumed == take);
    offset += take;
  }
  return {{data.data().begin(), data.data().end()},
          {reply.data().begin(), reply.data().end()}};
}

void TestNegotiation() {
  FakePort port;
  rfc2217::Session session({}, port.handler());
  std::array<std::uint8_t, 64> buffer;
  rfc2217::ByteSink start(buffer);
  session.Start(start);
  EXPECT((Bytes{start.data().begin(), start.data().end()} ==
          Bytes{kIac, kWill, 0, kIac, kDo, 0, kIac, kWill, 3, kIac, kDo, 3}));

  // Acknowledgements of our offers need no answer; the client's own offer
  // of COM-PORT-OPTION is accepted; ECHO and unknown options are refused.
  Result result = Feed(session, {kIac, kDo, 0, kIac, kWill, 0, kIac, kWill, 44,
                                 kIac, kDo, 1, kIac, kWill, 31});
  EXPECT(result.data.empty());
  EXPECT((result.reply ==
          Bytes{kIac, kDo, 44, kIac, kWont, 1, kIac, kDont, 31}));

  // Repeating an accepted offer does not loop.
  EXPECT(Feed(session, {kIac, kWill, 44}).reply.empty());
  // Withdrawing it is acknowledged once.
  EXPECT((Feed(session, {kIac, kWont, 44}).reply == Bytes{kIac, kDont, 44}));
  EXPECT(Feed(session, {kIac, kWont, 44}).reply.empty());
  // The client may ask us to perform COM-PORT-OPTION as well.
  EXPECT((Feed(session, {kIac, kDo, 44}).reply == Bytes{kIac, kWill, 44}));
}

void TestData() {
  FakePort port;
  rfc2217::Session session({}, port.handler());
  // IAC IAC is a 0xFF data byte; NOP and GA (IAC 241/249) vanish.
  const Bytes input{'a', kIac, kIac, 'b', kIac, 241, 'c', kIac, 249, 0, 13};
  const Bytes expected{'a', 0xFF, 'b', 'c', 0, 13};
  EXPECT(Feed(session, input).data == expected);
  EXPECT(Feed(session, input, true).data == expected);

  // Escape doubles 0xFF and stops when the output is full.
  std::array<std::uint8_t, 4> out;
  rfc2217::ByteSink sink(out);
  const Bytes raw{1, 0xFF, 2, 0xFF};
  EXPECT(rfc2217::Session::Escape(raw, sink) == 3);
  EXPECT((Bytes{sink.data().begin(), sink.data().end()} ==
          Bytes{1, 0xFF, 0xFF, 2}));

  // A full data sink stops Receive(); the rest comes next time.
  std::array<std::uint8_t, 2> small;
  std::array<std::uint8_t, 128> reply_buffer;
  rfc2217::ByteSink data(small);
  rfc2217::ByteSink reply(reply_buffer);
  const Bytes three{'x', 'y', 'z'};
  EXPECT(session.Receive(three, data, reply) == 2);
}

void TestComPort() {
  FakePort port;
  rfc2217::Session session({}, port.handler());

  // SET-BAUDRATE 9600 (0x2580) is applied and confirmed.
  Result result = Feed(session, ComPort(1, {0, 0, 0x25, 0x80}));
  EXPECT(result.reply == ComPort(101, {0, 0, 0x25, 0x80}));
  EXPECT(port.last.baud_rate == 9600);
  // 0 queries; a value with a 0xFF byte is escaped both ways.
  EXPECT(Feed(session, ComPort(1, {0, 0, 0, 0})).reply ==
         ComPort(101, {0, 0, 0x25, 0x80}));
  EXPECT(Feed(session, ComPort(1, {0, 0x01, 0xFF, 0x00}), true).reply ==
         ComPort(101, {0, 0x01, 0xFF, 0x00}));
  EXPECT(session.settings().baud_rate == 0x0001FF00);

  // Refused settings are answered with the value in use.
  EXPECT(Feed(session, ComPort(2, {5})).reply == ComPort(102, {8}));
  EXPECT(Feed(session, ComPort(2, {7})).reply == ComPort(102, {7}));
  EXPECT(Feed(session, ComPort(3, {4})).reply == ComPort(103, {1}));
  EXPECT(Feed(session, ComPort(3, {3})).reply == ComPort(103, {3}));
  EXPECT(Feed(session, ComPort(4, {3})).reply == ComPort(104, {3}));
  EXPECT(session.settings().parity == rfc2217::Parity::kEven);
  EXPECT(session.settings().stop_bits == rfc2217::StopBits::kOneAndHalf);

  // SET-CONTROL: flow control (XON/XOFF refused), BREAK always off, DTR/RTS
  // confirmed, inbound flow follows outbound.
  EXPECT(Feed(session, ComPort(5, {2})).reply == ComPort(105, {1}));
  EXPECT(Feed(session, ComPort(5, {3})).reply == ComPort(105, {3}));
  EXPECT(Feed(session, ComPort(5, {13})).reply == ComPort(105, {16}));
  EXPECT(Feed(session, ComPort(5, {5})).reply == ComPort(105, {6}));
  EXPECT(Feed(session, ComPort(5, {8})).reply == ComPort(105, {8}));
  EXPECT(Feed(session, ComPort(5, {12})).reply == ComPort(105, {12}));

  // Same settings again: no new apply.
  const int applies = port.applies;
  EXPECT(Feed(session, ComPort(3, {3})).reply == ComPort(103, {3}));
  EXPECT(port.applies == applies);

  // Suspend/resume, purge, signature, masks.
  EXPECT(Feed(session, ComPort(8, {})).reply == ComPort(108, {}));
  EXPECT(session.suspended());
  EXPECT(Feed(session, ComPort(9, {})).reply == ComPort(109, {}));
  EXPECT(!session.suspended());
  EXPECT(Feed(session, ComPort(12, {1})).reply == ComPort(112, {1}));
  EXPECT(port.purges == 1 && port.purge_receive && !port.purge_transmit);
  const Bytes signature(session.signature().begin(),
                        session.signature().end());
  EXPECT(Feed(session, ComPort(0, {})).reply == ComPort(100, signature));
  EXPECT(Feed(session, ComPort(0, {'p', 'c'})).reply.empty());

  std::array<std::uint8_t, 128> buffer;
  rfc2217::ByteSink notify(buffer);
  session.NotifyLineState(rfc2217::kLineOverrun, notify);
  EXPECT(notify.data().empty());  // Mask 0: nothing requested.
  EXPECT(Feed(session, ComPort(10, {0x0E})).reply == ComPort(110, {0x0E}));
  session.NotifyLineState(rfc2217::kLineOverrun | rfc2217::kLineBreak, notify);
  EXPECT((Bytes{notify.data().begin(), notify.data().end()} ==
          ComPort(106, {rfc2217::kLineOverrun})));

  // Data around a command stays data.
  Bytes mixed{'A'};
  const Bytes command = ComPort(1, {0, 0, 0, 0});
  mixed.insert(mixed.end(), command.begin(), command.end());
  mixed.push_back('B');
  result = Feed(session, mixed, true);
  EXPECT((result.data == Bytes{'A', 'B'}));
}

}  // namespace

// The SIGNATURE is the application's, cut to kMaxSignatureSize.
void TestSignature() {
  FakePort port;
  rfc2217::Session named({}, port.handler(), "device");
  EXPECT(Feed(named, ComPort(0, {})).reply ==
         ComPort(100, {'d', 'e', 'v', 'i', 'c', 'e'}));

  rfc2217::Session long_name({}, port.handler(),
                             "a signature longer than the limit");
  EXPECT(long_name.signature().size() == rfc2217::Session::kMaxSignatureSize);
  const Result result = Feed(long_name, ComPort(0, {}));
  EXPECT(result.reply.size() <= rfc2217::Session::kMaxReplySize);
}

int main() {
  TestNegotiation();
  TestData();
  TestComPort();
  TestSignature();
  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all rfc2217 tests passed\n");
  return EXIT_SUCCESS;
}
