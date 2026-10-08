#ifndef SHARED_INCLUDE_RING_MPSC_RING_
#define SHARED_INCLUDE_RING_MPSC_RING_

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace ring {

// Lock-free multi-producer, single-consumer ring of variable-size records,
// built so that the consumer can hand runs of records to DMA in one transfer.
//
// Producers (threads or interrupts of any priority on the same core) reserve
// bytes and a record slot with one CAS on a packed 32-bit word, copy their
// record, then publish it by stamping the slot. They never wait: TryWrite()
// returns false when there is no room. A record is never split; if it does
// not fit before the end of the buffer, the rest of the buffer is skipped.
//
// The single consumer collects the committed records from the tail onwards
// (Peek), sends them and frees them (Release). It never waits either: a
// record that is reserved but not committed yet ends the batch, and the
// records behind it go out in a later batch, so the output keeps the
// reservation order.
//
// The storage is attached at runtime because DMA buffers live in their own
// section (DMA_BUFFER); the control words must be in Normal memory, which
// LDREX/STREX need.
//
// All producers must run on one core: the reservation CAS relies on that
// core's local exclusive monitor (the STM32H7 has no documented global one).
// The consumer may run on the other core: it never does a read-modify-write
// on a shared word, it only reads stamps and the head and stores the tail.
//
//   ring::MpscRing<2048, 64> ring;
//   ring.Attach(storage);                     // before any producer runs
//   ring.TryWrite(bytes);                     // any context
//   if (auto batch = ring.Peek()) {           // consumer
//     Send(batch->bytes);
//     ring.Release(*batch);                   // when the send completed
//   }
template <std::size_t kCapacity, std::size_t kRecordSlots>
class MpscRing {
 public:
  // Positions and record counts are free-running 16-bit counters. A
  // reservation can span up to three buffer lengths (used + skipped + new),
  // which must stay below 2^16 to compare unambiguously.
  static_assert(std::has_single_bit(kCapacity) && kCapacity <= 16384,
                "capacity must be a power of two <= 16384");
  static_assert(std::has_single_bit(kRecordSlots) && kRecordSlots >= 2 &&
                    kRecordSlots <= 16384,
                "record slots must be a power of two in [2, 16384]");

  // Bytes of storage, and so the largest possible Batch.
  static constexpr std::size_t kStorageSize = kCapacity;
  // Largest record an empty ring always accepts, wherever its head is.
  static constexpr std::size_t kMaxRecordSize = kCapacity / 2;

  // Committed records that are contiguous in memory, oldest first.
  struct Batch {
    std::span<const std::uint8_t> bytes;
    std::uint16_t records = 0;
    std::uint16_t end = 0;  // Data position after the last record.
  };

  constexpr MpscRing() = default;
  MpscRing(const MpscRing&) = delete;
  MpscRing& operator=(const MpscRing&) = delete;

  // Sets the data storage. Call once, before any producer or consumer runs;
  // until then every TryWrite() fails.
  void Attach(std::span<std::uint8_t, kCapacity> storage) noexcept {
    data_ = storage.data();
  }

  // Appends `record` as one unit. Fails if it is empty, larger than
  // kMaxRecordSize, or there is not enough room. Safe from any context.
  bool TryWrite(std::span<const std::uint8_t> record) noexcept;

  // Consumer only: the committed records at the tail that can go out in one
  // transfer, or nullopt if the oldest record is not committed yet. The
  // records stay in the ring until Release().
  std::optional<Batch> Peek() const noexcept;

  // Consumer only: frees a batch returned by the last Peek().
  void Release(const Batch& batch) noexcept;

  // True if no record is reserved or waiting to be released.
  bool IsEmpty() const noexcept {
    return Records(head_.load(std::memory_order_acquire)) ==
           Records(tail_.load(std::memory_order_acquire));
  }

 private:
  static constexpr std::uint32_t kMask = kCapacity - 1;
  static constexpr std::uint32_t kSlotMask = kRecordSlots - 1;
  static constexpr std::uint32_t kCounterMask = 0xFFFF;

  struct Slot {
    // (sequence + 1) of the record once it is committed. Earlier laps leave
    // a different value, so the slot never needs clearing.
    std::atomic<std::uint32_t> stamp{0};
    // end << 16 | length. Written before the stamp is released.
    std::atomic<std::uint32_t> extent{0};
  };

  static constexpr std::uint32_t Pack(std::uint32_t high,
                                      std::uint32_t low) noexcept {
    return (high & kCounterMask) << 16 | (low & kCounterMask);
  }
  static constexpr std::uint32_t High(std::uint32_t word) noexcept {
    return word >> 16;
  }
  static constexpr std::uint32_t Low(std::uint32_t word) noexcept {
    return word & kCounterMask;
  }
  static constexpr std::uint32_t Records(std::uint32_t word) noexcept {
    return High(word);
  }
  static constexpr std::uint32_t Position(std::uint32_t word) noexcept {
    return Low(word);
  }

  std::uint8_t* data_ = nullptr;
  // Reserved: record count << 16 | data position. Producers only (CAS).
  std::atomic<std::uint32_t> head_{0};
  // Released: record count << 16 | data position. Consumer only.
  std::atomic<std::uint32_t> tail_{0};
  std::array<Slot, kRecordSlots> slots_{};
};

template <std::size_t kCapacity, std::size_t kRecordSlots>
bool MpscRing<kCapacity, kRecordSlots>::TryWrite(
    std::span<const std::uint8_t> record) noexcept {
  if (data_ == nullptr || record.empty() || record.size() > kMaxRecordSize) {
    return false;
  }
  const auto length = static_cast<std::uint32_t>(record.size());

  std::uint32_t head = 0;
  std::uint32_t start = 0;
  std::uint32_t end = 0;
  while (true) {
    // Tail first: it never passes a head read later, so head - tail is the
    // real fill level (or more, which only makes the check conservative).
    const std::uint32_t tail = tail_.load(std::memory_order_acquire);
    head = head_.load(std::memory_order_relaxed);

    const std::uint32_t position = Position(head);
    const std::uint32_t contiguous = kCapacity - (position & kMask);
    start = length <= contiguous ? position : position + contiguous;
    end = start + length;
    const std::uint32_t used_bytes = (end - Position(tail)) & kCounterMask;
    const std::uint32_t used_records =
        (Records(head) - Records(tail)) & kCounterMask;
    if (used_bytes > kCapacity || used_records >= kRecordSlots) {
      return false;
    }
    if (head_.compare_exchange_weak(head, Pack(Records(head) + 1, end),
                                    std::memory_order_relaxed)) {
      break;
    }
  }

  // [start, end) and the slot now belong to this producer alone.
  Slot& slot = slots_[Records(head) & kSlotMask];
  slot.extent.store(Pack(end, length), std::memory_order_relaxed);
  std::memcpy(data_ + (start & kMask), record.data(), length);
  slot.stamp.store((Records(head) + 1) & kCounterMask,
                   std::memory_order_release);
  return true;
}

template <std::size_t kCapacity, std::size_t kRecordSlots>
auto MpscRing<kCapacity, kRecordSlots>::Peek() const noexcept
    -> std::optional<Batch> {
  if (data_ == nullptr) {
    return std::nullopt;
  }
  const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
  std::uint32_t sequence = Records(tail);
  std::uint32_t first = 0;
  std::uint32_t position = 0;
  std::uint32_t bytes = 0;
  std::uint32_t count = 0;

  while (count < kRecordSlots) {
    const Slot& slot = slots_[sequence & kSlotMask];
    if (slot.stamp.load(std::memory_order_acquire) !=
        ((sequence + 1) & kCounterMask)) {
      break;  // Not reserved, or reserved and still being written.
    }
    const std::uint32_t extent = slot.extent.load(std::memory_order_relaxed);
    const std::uint32_t end = High(extent);
    const std::uint32_t length = Low(extent);
    const std::uint32_t start = (end - length) & kCounterMask;
    if (count == 0) {
      first = start;
    } else if (start != position || (position & kMask) == 0) {
      break;  // Skipped space, or the previous record ended the buffer.
    }
    position = end;
    bytes += length;
    ++count;
    ++sequence;
  }

  if (count == 0) {
    return std::nullopt;
  }
  return Batch{.bytes = {data_ + (first & kMask), bytes},
               .records = static_cast<std::uint16_t>(count),
               .end = static_cast<std::uint16_t>(position)};
}

template <std::size_t kCapacity, std::size_t kRecordSlots>
void MpscRing<kCapacity, kRecordSlots>::Release(const Batch& batch) noexcept {
  const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
  // Moving the position to the batch end also frees any skipped space
  // before the batch.
  tail_.store(Pack(Records(tail) + batch.records, batch.end),
              std::memory_order_release);
}

}  // namespace ring

#endif  // SHARED_INCLUDE_RING_MPSC_RING_
