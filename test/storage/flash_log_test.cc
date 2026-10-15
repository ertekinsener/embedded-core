// Host tests for storage::FlashLog on a simulated flash with the STM32H7
// rules (units programmed once between erases, ECC errors on units cut
// short): records survive remounts and sector switches, and a power loss
// during any program or erase keeps the newest complete record.

#include "storage/flash_log.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
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

constexpr std::size_t kSectorSize = 1024;
constexpr std::size_t kUnit = 32;
constexpr std::size_t kUnits = kSectorSize / kUnit;

// Two sectors. `ops_left` counts unit programs and erases until a simulated
// power loss: that operation is cut short and every later one fails.
struct SimFlash {
  std::array<std::vector<std::uint8_t>, 2> data{
      std::vector<std::uint8_t>(kSectorSize, 0xFF),
      std::vector<std::uint8_t>(kSectorSize, 0xFF)};
  std::array<std::vector<bool>, 2> programmed{std::vector<bool>(kUnits),
                                              std::vector<bool>(kUnits)};
  std::array<std::vector<bool>, 2> ecc_error{std::vector<bool>(kUnits),
                                             std::vector<bool>(kUnits)};
  int ops_left = -1;  // -1: no power loss.
  // A cut unit reads with an ECC error; else it reads as half-programmed
  // data (which only the record CRC catches).
  bool ecc_on_cut = true;
  bool powered = true;
  int erases = 0;
  int violations = 0;  // Programs of a unit that was not erased.

  bool Consume() {
    if (!powered) {
      return false;
    }
    if (ops_left == 0) {
      powered = false;
      return false;
    }
    if (ops_left > 0) {
      --ops_left;
    }
    return true;
  }

  void PowerOn() {
    powered = true;
    ops_left = -1;
  }

  static bool Erase(void* context, std::size_t sector) noexcept {
    auto& flash = *static_cast<SimFlash*>(context);
    const bool was_powered = flash.powered;
    const bool complete = flash.Consume();
    if (!was_powered) {
      return false;
    }
    // An interrupted erase leaves the first half erased, the rest garbage.
    for (std::size_t unit = 0; unit < kUnits; ++unit) {
      if (complete || unit < kUnits / 2) {
        std::memset(&flash.data[sector][unit * kUnit], 0xFF, kUnit);
        flash.programmed[sector][unit] = false;
        flash.ecc_error[sector][unit] = false;
      } else {
        flash.ecc_error[sector][unit] = true;
      }
    }
    ++flash.erases;
    return complete;
  }

  static bool Program(void* context, std::size_t sector, std::size_t offset,
                      std::span<const std::uint8_t> bytes) noexcept {
    auto& flash = *static_cast<SimFlash*>(context);
    if (offset % kUnit != 0 || bytes.size() % kUnit != 0) {
      ++flash.violations;
      return false;
    }
    for (std::size_t done = 0; done < bytes.size(); done += kUnit) {
      const std::size_t unit = (offset + done) / kUnit;
      if (flash.programmed[sector][unit]) {
        ++flash.violations;
        return false;
      }
      const bool was_powered = flash.powered;
      if (!flash.Consume()) {
        if (was_powered) {
          // Cut short: only the first half reached the flash.
          std::memcpy(&flash.data[sector][unit * kUnit], &bytes[done],
                      kUnit / 2);
          flash.programmed[sector][unit] = true;
          flash.ecc_error[sector][unit] = flash.ecc_on_cut;
        }
        return false;
      }
      std::memcpy(&flash.data[sector][unit * kUnit], &bytes[done], kUnit);
      flash.programmed[sector][unit] = true;
    }
    return true;
  }

  static bool Read(void* context, std::size_t sector, std::size_t offset,
                   std::span<std::uint8_t> out) noexcept {
    auto& flash = *static_cast<SimFlash*>(context);
    for (std::size_t i = 0; i < out.size(); ++i) {
      if (flash.ecc_error[sector][(offset + i) / kUnit]) {
        return false;
      }
    }
    std::memcpy(out.data(), &flash.data[sector][offset], out.size());
    return true;
  }

  storage::FlashSectors Sectors() {
    return {.sector_size = kSectorSize,
            .program_unit = kUnit,
            .context = this,
            .erase = &Erase,
            .program = &Program,
            .read = &Read};
  }
};

std::vector<std::uint8_t> Record(int index, std::size_t size) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::uint8_t>(index * 31 + i);
  }
  return bytes;
}

std::vector<std::uint8_t> Latest(const storage::FlashLog& log) {
  std::vector<std::uint8_t> buffer(kSectorSize);
  buffer.resize(log.ReadLatest(buffer));
  return buffer;
}

// What a reboot sees.
std::vector<std::uint8_t> Remounted(SimFlash& flash) {
  storage::FlashLog log(flash.Sectors());
  EXPECT(log.Mount());
  return Latest(log);
}

void TestCrc() {
  const std::string text = "123456789";
  EXPECT(storage::Crc32({reinterpret_cast<const std::uint8_t*>(text.data()),
                         text.size()}) == 0xCBF43926u);
}

void TestBasics() {
  SimFlash flash;
  storage::FlashLog log(flash.Sectors());
  EXPECT(log.Mount());
  EXPECT(Latest(log).empty());
  EXPECT(log.MaxRecordSize() == kSectorSize - kUnit);

  EXPECT(log.Write(Record(1, 40)));
  EXPECT(Latest(log) == Record(1, 40));
  EXPECT(log.Write(Record(2, 64)));
  EXPECT(Latest(log) == Record(2, 64));
  EXPECT(Remounted(flash) == Record(2, 64));

  // Too small a buffer, empty and oversized records.
  std::array<std::uint8_t, 10> small{};
  EXPECT(log.ReadLatest(small) == 0);
  EXPECT(!log.Write({}));
  EXPECT(!log.Write(Record(3, kSectorSize)));
  EXPECT(log.Write(Record(4, log.MaxRecordSize())));
  EXPECT(Remounted(flash) == Record(4, log.MaxRecordSize()));

  EXPECT(log.EraseAll());
  EXPECT(Latest(log).empty());
  EXPECT(Remounted(flash).empty());
  EXPECT(log.Write(Record(5, 1)));
  EXPECT(Remounted(flash) == Record(5, 1));
  EXPECT(flash.violations == 0);

  // Unusable geometry.
  storage::FlashSectors bad = flash.Sectors();
  bad.program_unit = 24;
  storage::FlashLog bad_log(bad);
  EXPECT(!bad_log.Mount());
  EXPECT(!bad_log.Write(Record(6, 4)));
}

// Many writes of varying size: the log switches sectors over and over, and
// every remount finds the newest record.
void TestSectorSwitches() {
  SimFlash flash;
  storage::FlashLog log(flash.Sectors());
  EXPECT(log.Mount());
  for (int i = 0; i < 200; ++i) {
    const auto record = Record(i, 1 + (i * 37) % 300);
    EXPECT(log.Write(record));
    if (i % 7 == 0) {
      EXPECT(Remounted(flash) == record);
    }
    // Continue on a fresh mount now and then, like after a reboot.
    if (i % 13 == 0) {
      EXPECT(log.Mount());
      EXPECT(Latest(log) == record);
    }
  }
  EXPECT(flash.erases > 10);
  EXPECT(flash.violations == 0);
}

// Cuts the power at every operation of a write sequence in turn; after the
// reboot the newest complete record must be there and writing must work.
void TestPowerLoss() {
  for (int cut = 0; cut < 120; ++cut) {
    SimFlash flash;
    flash.ecc_on_cut = cut % 2 == 0;
    storage::FlashLog log(flash.Sectors());
    EXPECT(log.Mount());
    std::vector<std::uint8_t> last_complete;
    std::vector<std::uint8_t> cut_short;  // The write the power loss hit.
    flash.ops_left = cut / 2;
    for (int i = 0; i < 12; ++i) {
      const auto record = Record(i, 100 + i * 20);
      if (log.Write(record)) {
        last_complete = record;
      } else if (cut_short.empty()) {
        cut_short = record;
      }
    }
    flash.PowerOn();
    // The newest complete record, or the cut one if all its data still
    // made it (the cut hit padding); never anything older or broken.
    const auto found = Remounted(flash);
    EXPECT(found == last_complete || found == cut_short);

    storage::FlashLog rebooted(flash.Sectors());
    EXPECT(rebooted.Mount());
    for (int i = 0; i < 20; ++i) {
      const auto record = Record(100 + i, 150);
      EXPECT(rebooted.Write(record));
      EXPECT(Latest(rebooted) == record);
    }
    EXPECT(Remounted(flash) == Record(119, 150));
    EXPECT(flash.violations == 0);
  }
}

// An interrupted erase while switching sectors keeps the record in the
// active one. Records of 300 bytes take 352: two fit into a sector.
void TestPowerLossDuringErase() {
  SimFlash flash;
  storage::FlashLog log(flash.Sectors());
  EXPECT(log.Mount());
  EXPECT(log.Write(Record(1, 300)));
  EXPECT(log.Write(Record(2, 300)));
  EXPECT(flash.erases == 0);
  flash.ops_left = 0;  // The third write starts with erasing sector 1.
  EXPECT(!log.Write(Record(3, 300)));
  EXPECT(flash.erases == 1);
  flash.PowerOn();
  EXPECT(Remounted(flash) == Record(2, 300));

  storage::FlashLog rebooted(flash.Sectors());
  EXPECT(rebooted.Mount());
  EXPECT(rebooted.Write(Record(4, 300)));
  EXPECT(flash.erases == 2);
  EXPECT(Remounted(flash) == Record(4, 300));
  EXPECT(flash.violations == 0);
}

}  // namespace

int main() {
  TestCrc();
  TestBasics();
  TestSectorSwitches();
  TestPowerLoss();
  TestPowerLossDuringErase();
  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all flash log tests passed\n");
  return EXIT_SUCCESS;
}
