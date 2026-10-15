#include "storage/flash_log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace storage {
namespace {

// "SET1" in memory (little-endian); a different value ends a sector's log.
constexpr std::uint32_t kMagic = 0x31544553;
constexpr std::uint32_t kErased = 0xFFFFFFFF;
// Bytes read at a time while checking a record.
constexpr std::size_t kReadChunk = 32;

// True if `a` is newer than `b`, also across a wrap of the counter.
bool Newer(std::uint32_t a, std::uint32_t b) noexcept {
  return static_cast<std::int32_t>(a - b) > 0;
}

}  // namespace

std::uint32_t Crc32(std::span<const std::uint8_t> data,
                    std::uint32_t crc) noexcept {
  crc = ~crc;
  for (std::uint8_t byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

bool FlashLog::Mount() noexcept {
  const std::size_t unit = flash_.program_unit;
  mounted_ = false;
  if (unit < sizeof(Header) || unit > kMaxProgramUnit ||
      (unit & (unit - 1)) != 0 || flash_.sector_size < 2 * unit ||
      flash_.sector_size % unit != 0 || flash_.erase == nullptr ||
      flash_.program == nullptr || flash_.read == nullptr) {
    return false;
  }
  const std::array<SectorScan, 2> scans{Scan(0), Scan(1)};
  std::size_t newest = 0;
  if (scans[1].has_record &&
      (!scans[0].has_record || Newer(scans[1].sequence, scans[0].sequence))) {
    newest = 1;
  }
  const SectorScan& scan = scans[newest];
  active_ = newest;
  end_ = scan.end;
  has_record_ = scan.has_record;
  sequence_ = scan.sequence;
  record_sector_ = newest;
  record_offset_ = scan.record_offset;
  record_size_ = scan.record_size;
  record_crc_ = scan.record_crc;
  mounted_ = true;
  return true;
}

std::size_t FlashLog::ReadLatest(
    std::span<std::uint8_t> buffer) const noexcept {
  if (!mounted_ || !has_record_ || buffer.size() < record_size_) {
    return 0;
  }
  const std::span<std::uint8_t> out = buffer.first(record_size_);
  if (!flash_.read(flash_.context, record_sector_,
                   record_offset_ + flash_.program_unit, out) ||
      Crc32(out) != record_crc_) {
    return 0;
  }
  return out.size();
}

bool FlashLog::Write(std::span<const std::uint8_t> data) noexcept {
  if (!mounted_ || data.empty() || data.size() > MaxRecordSize()) {
    return false;
  }
  const std::size_t unit = flash_.program_unit;
  const std::size_t record_bytes = unit + RoundUp(data.size());
  if (end_ + record_bytes > flash_.sector_size) {
    const std::size_t other = 1 - active_;
    if (!flash_.erase(flash_.context, other)) {
      return false;
    }
    active_ = other;
    end_ = 0;
  }

  const std::size_t offset = end_;
  const Header header{
      .magic = kMagic,
      .sequence = has_record_ ? sequence_ + 1 : 1,
      .size = static_cast<std::uint32_t>(data.size()),
      .crc = Crc32(data),
  };
  std::array<std::uint8_t, kMaxProgramUnit> block;
  block.fill(0xFF);
  std::memcpy(block.data(), &header, sizeof(header));
  // The header goes first: a record cut short by a power loss has a valid
  // header and a wrong CRC, so the scan still finds the next one.
  if (!flash_.program(flash_.context, active_, offset,
                      std::span(block).first(unit))) {
    end_ = flash_.sector_size;  // Unknown state: switch on the next write.
    return false;
  }
  end_ = offset + record_bytes;

  const std::size_t whole = data.size() / unit * unit;
  if (whole != 0 && !flash_.program(flash_.context, active_, offset + unit,
                                    data.first(whole))) {
    return false;
  }
  if (whole != data.size()) {
    block.fill(0xFF);
    std::copy(data.begin() + whole, data.end(), block.begin());
    if (!flash_.program(flash_.context, active_, offset + unit + whole,
                        std::span(block).first(unit))) {
      return false;
    }
  }

  has_record_ = true;
  sequence_ = header.sequence;
  record_sector_ = active_;
  record_offset_ = offset;
  record_size_ = header.size;
  record_crc_ = header.crc;
  return true;
}

bool FlashLog::EraseAll() noexcept {
  if (!mounted_) {
    return false;
  }
  const bool erased = flash_.erase(flash_.context, 0) &&
                      flash_.erase(flash_.context, 1);
  if (!erased) {
    Mount();  // Whatever is left.
    return false;
  }
  has_record_ = false;
  active_ = 0;
  end_ = 0;
  return true;
}

std::size_t FlashLog::MaxRecordSize() const noexcept {
  return flash_.sector_size - flash_.program_unit;
}

FlashLog::SectorScan FlashLog::Scan(std::size_t sector) const noexcept {
  const std::size_t unit = flash_.program_unit;
  const std::size_t size = flash_.sector_size;
  SectorScan scan;
  std::size_t offset = 0;
  while (offset + unit <= size) {
    Header header{};
    if (!ReadHeader(sector, offset, header)) {
      scan.end = size;
      return scan;
    }
    if (header.magic == kErased && header.sequence == kErased &&
        header.size == kErased && header.crc == kErased) {
      scan.end = offset;
      return scan;
    }
    if (header.magic != kMagic || header.size == 0 ||
        header.size > size - offset - unit) {
      scan.end = size;  // Not ours: do not append behind it.
      return scan;
    }
    std::uint32_t crc = 0;
    if (PayloadCrc(sector, offset + unit, header.size, crc) &&
        crc == header.crc &&
        (!scan.has_record || Newer(header.sequence, scan.sequence))) {
      scan.has_record = true;
      scan.sequence = header.sequence;
      scan.record_offset = offset;
      scan.record_size = header.size;
      scan.record_crc = header.crc;
    }
    offset += unit + RoundUp(header.size);
  }
  scan.end = std::min(offset, size);
  return scan;
}

bool FlashLog::ReadHeader(std::size_t sector, std::size_t offset,
                          Header& header) const noexcept {
  std::array<std::uint8_t, sizeof(Header)> bytes{};
  if (!flash_.read(flash_.context, sector, offset, bytes)) {
    return false;
  }
  std::memcpy(&header, bytes.data(), sizeof(header));
  return true;
}

bool FlashLog::PayloadCrc(std::size_t sector, std::size_t offset,
                          std::uint32_t size,
                          std::uint32_t& crc) const noexcept {
  std::array<std::uint8_t, kReadChunk> chunk{};
  crc = 0;
  for (std::size_t done = 0; done < size;) {
    const std::size_t take = std::min<std::size_t>(chunk.size(), size - done);
    const std::span<std::uint8_t> part = std::span(chunk).first(take);
    if (!flash_.read(flash_.context, sector, offset + done, part)) {
      return false;
    }
    crc = Crc32(part, crc);
    done += take;
  }
  return true;
}

std::size_t FlashLog::RoundUp(std::size_t size) const noexcept {
  const std::size_t unit = flash_.program_unit;
  return (size + unit - 1) / unit * unit;
}

}  // namespace storage
