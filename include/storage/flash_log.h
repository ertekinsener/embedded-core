#ifndef EMBEDDED_CORE_INCLUDE_STORAGE_FLASH_LOG_
#define EMBEDDED_CORE_INCLUDE_STORAGE_FLASH_LOG_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// A small blob (the device settings) kept in two flash sectors that survives
// power loss at any moment (docs/storage/README.md). Every Write() appends a
// new record to the active sector; when it is full, the other sector is
// erased and becomes the active one, so the newest complete record always
// stays readable. No RTOS and no hardware: the platform provides the flash
// operations (hal/flash.h on the STM32H7), the host tests a simulation.
namespace storage {

// The flash under the log: two sectors of `sector_size` bytes, programmed in
// aligned units of `program_unit` bytes, each written once between erases
// (the STM32H7 flash word: 32 bytes with ECC).
struct FlashSectors {
  std::size_t sector_size = 0;
  std::size_t program_unit = 0;  // 16..kMaxProgramUnit, a power of two.
  void* context = nullptr;
  // Erases `sector` (0 or 1). May take long (1-2 s on the STM32H7).
  bool (*erase)(void* context, std::size_t sector) noexcept = nullptr;
  // Programs whole units at `offset` (a multiple of program_unit).
  bool (*program)(void* context, std::size_t sector, std::size_t offset,
                  std::span<const std::uint8_t> data) noexcept = nullptr;
  // Copies bytes from the sector; false if they cannot be read (an ECC
  // error after an interrupted program or erase).
  bool (*read)(void* context, std::size_t sector, std::size_t offset,
               std::span<std::uint8_t> out) noexcept = nullptr;
};

// Not thread safe: use from one thread at a time.
class FlashLog {
 public:
  static constexpr std::size_t kMaxProgramUnit = 32;

  explicit FlashLog(const FlashSectors& flash) noexcept : flash_(flash) {}

  FlashLog(const FlashLog&) = delete;
  FlashLog& operator=(const FlashLog&) = delete;

  // Scans both sectors for the newest valid record and the end of the log.
  // Call once before the other functions; false if `flash` is unusable.
  bool Mount() noexcept;

  // Copies the newest record into `buffer`; its size, 0 if there is none,
  // it does not fit or it cannot be read back.
  std::size_t ReadLatest(std::span<std::uint8_t> buffer) const noexcept;

  // Appends `data` (1 byte up to MaxRecordSize()) as the newest record.
  bool Write(std::span<const std::uint8_t> data) noexcept;

  // Erases both sectors: no record until the next Write().
  bool EraseAll() noexcept;

  std::size_t MaxRecordSize() const noexcept;

 private:
  struct Header {
    std::uint32_t magic;
    std::uint32_t sequence;
    std::uint32_t size;
    std::uint32_t crc;
  };

  // The newest valid record of one sector and where its log ends.
  struct SectorScan {
    bool has_record = false;
    std::uint32_t sequence = 0;
    std::size_t record_offset = 0;
    std::uint32_t record_size = 0;
    std::uint32_t record_crc = 0;
    std::size_t end = 0;  // Next free unit; sector_size when unusable.
  };

  SectorScan Scan(std::size_t sector) const noexcept;
  bool ReadHeader(std::size_t sector, std::size_t offset,
                  Header& header) const noexcept;
  bool PayloadCrc(std::size_t sector, std::size_t offset, std::uint32_t size,
                  std::uint32_t& crc) const noexcept;
  std::size_t RoundUp(std::size_t size) const noexcept;

  FlashSectors flash_;
  bool mounted_ = false;
  bool has_record_ = false;
  std::size_t active_ = 0;  // Sector that takes the next record.
  std::size_t end_ = 0;     // Free offset in the active sector.
  std::uint32_t sequence_ = 0;
  std::size_t record_sector_ = 0;
  std::size_t record_offset_ = 0;
  std::uint32_t record_size_ = 0;
  std::uint32_t record_crc_ = 0;
};

// CRC-32 (IEEE 802.3, as zlib); `crc` continues a previous result.
std::uint32_t Crc32(std::span<const std::uint8_t> data,
                    std::uint32_t crc = 0) noexcept;

}  // namespace storage

#endif  // EMBEDDED_CORE_INCLUDE_STORAGE_FLASH_LOG_
