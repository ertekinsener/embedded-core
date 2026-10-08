// Host tests for ring::MpscRing: deterministic edge cases, then a stress test
// with several producer threads and one consumer that checks every record
// arrives whole, once, and in per-producer order. Build with test/CMakeLists.txt
// (ThreadSanitizer by default).

#include "ring/mpsc_ring.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <thread>
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

std::span<const std::uint8_t> Bytes(const std::vector<std::uint8_t>& v) {
  return {v.data(), v.size()};
}

std::vector<std::uint8_t> Filled(std::size_t size, std::uint8_t value) {
  return std::vector<std::uint8_t>(size, value);
}

void TestRejectsInvalid() {
  ring::MpscRing<64, 4> ring;
  EXPECT(!ring.TryWrite(Bytes(Filled(1, 1))));  // Not attached.
  alignas(4) std::array<std::uint8_t, 64> storage{};
  ring.Attach(storage);
  EXPECT(!ring.TryWrite({}));
  EXPECT(!ring.TryWrite(Bytes(Filled(33, 1))));  // > kMaxRecordSize.
  EXPECT(ring.TryWrite(Bytes(Filled(32, 1))));
  EXPECT(!ring.Peek() || ring.Peek()->bytes.size() == 32);
}

void TestBatchesAndRelease() {
  ring::MpscRing<64, 8> ring;
  std::array<std::uint8_t, 64> storage{};
  ring.Attach(storage);
  EXPECT(ring.IsEmpty());
  EXPECT(!ring.Peek());

  EXPECT(ring.TryWrite(Bytes(Filled(10, 'a'))));
  EXPECT(ring.TryWrite(Bytes(Filled(20, 'b'))));
  auto batch = ring.Peek();
  EXPECT(batch && batch->records == 2 && batch->bytes.size() == 30);
  EXPECT(batch->bytes.data() == storage.data());
  EXPECT(batch->bytes[9] == 'a' && batch->bytes[10] == 'b');
  ring.Release(*batch);
  EXPECT(ring.IsEmpty());
  EXPECT(!ring.Peek());
}

void TestSkipsInsteadOfSplitting() {
  ring::MpscRing<64, 8> ring;
  std::array<std::uint8_t, 64> storage{};
  ring.Attach(storage);

  // Head at 50: a 20-byte record does not fit before the end.
  EXPECT(ring.TryWrite(Bytes(Filled(25, 'a'))));
  EXPECT(ring.TryWrite(Bytes(Filled(25, 'b'))));
  ring.Release(*ring.Peek());
  EXPECT(ring.TryWrite(Bytes(Filled(20, 'c'))));
  auto batch = ring.Peek();
  EXPECT(batch && batch->records == 1 && batch->bytes.size() == 20);
  EXPECT(batch->bytes.data() == storage.data());  // Started over at 0.
  ring.Release(*batch);

  // Skipped bytes are free again: 44 bytes fit after position 20.
  EXPECT(ring.TryWrite(Bytes(Filled(32, 'd'))));
  EXPECT(ring.TryWrite(Bytes(Filled(12, 'e'))));
  batch = ring.Peek();
  EXPECT(batch && batch->records == 2 && batch->bytes.size() == 44);
}

void TestStopsAtBufferEnd() {
  ring::MpscRing<64, 8> ring;
  std::array<std::uint8_t, 64> storage{};
  ring.Attach(storage);

  EXPECT(ring.TryWrite(Bytes(Filled(32, 'a'))));
  ring.Release(*ring.Peek());
  // One record ends exactly at the buffer end, the next starts at 0.
  EXPECT(ring.TryWrite(Bytes(Filled(32, 'b'))));
  EXPECT(ring.TryWrite(Bytes(Filled(8, 'c'))));
  auto batch = ring.Peek();
  EXPECT(batch && batch->records == 1 && batch->bytes.size() == 32);
  ring.Release(*batch);
  batch = ring.Peek();
  EXPECT(batch && batch->records == 1 && batch->bytes.data() == storage.data());
}

void TestFull() {
  ring::MpscRing<64, 4> ring;
  std::array<std::uint8_t, 64> storage{};
  ring.Attach(storage);

  // Out of bytes.
  EXPECT(ring.TryWrite(Bytes(Filled(32, 'a'))));
  EXPECT(ring.TryWrite(Bytes(Filled(30, 'b'))));
  EXPECT(!ring.TryWrite(Bytes(Filled(3, 'c'))));
  EXPECT(ring.TryWrite(Bytes(Filled(2, 'c'))));
  ring.Release(*ring.Peek());

  // Out of record slots.
  for (int i = 0; i < 4; ++i) EXPECT(ring.TryWrite(Bytes(Filled(1, 'd'))));
  EXPECT(!ring.TryWrite(Bytes(Filled(1, 'e'))));
}

void TestSlotReuse() {
  // Stamps left from the previous lap must not look committed.
  ring::MpscRing<64, 4> ring;
  std::array<std::uint8_t, 64> storage{};
  ring.Attach(storage);
  for (int i = 0; i < 4; ++i) EXPECT(ring.TryWrite(Bytes(Filled(4, 'a'))));
  ring.Release(*ring.Peek());
  EXPECT(ring.TryWrite(Bytes(Filled(4, 'b'))));
  auto batch = ring.Peek();
  EXPECT(batch && batch->records == 1);
}

// Record layout for the stress test: producer, sequence (4 bytes), length
// byte, then a payload derived from all three.
constexpr int kProducers = 4;
constexpr std::uint32_t kRecordsPerProducer = 200'000;

std::uint8_t PayloadByte(std::uint8_t producer, std::uint32_t sequence,
                         std::size_t index) {
  return static_cast<std::uint8_t>(producer * 31 + sequence * 7 + index);
}

void TestStress() {
  ring::MpscRing<1024, 32> ring;
  alignas(32) static std::array<std::uint8_t, 1024> storage{};
  ring.Attach(storage);

  std::atomic<int> producers_done{0};
  std::array<std::uint64_t, kProducers> dropped{};

  auto produce = [&](std::uint8_t producer) {
    std::mt19937 random(producer);
    std::uniform_int_distribution<std::size_t> length(6, 120);
    std::array<std::uint8_t, 128> record{};
    for (std::uint32_t sequence = 0; sequence < kRecordsPerProducer;) {
      const std::size_t size = length(random);
      record[0] = producer;
      std::memcpy(&record[1], &sequence, sizeof(sequence));
      record[5] = static_cast<std::uint8_t>(size);
      for (std::size_t i = 6; i < size; ++i) {
        record[i] = PayloadByte(producer, sequence, i);
      }
      if (ring.TryWrite(std::span(record).first(size))) {
        ++sequence;
      } else {
        ++dropped[producer];  // Retry the same sequence: none may be lost.
        std::this_thread::yield();
      }
    }
    producers_done.fetch_add(1);
  };

  std::array<std::uint32_t, kProducers> next_sequence{};
  std::vector<std::uint8_t> carry;  // Must stay empty: batches end on records.
  std::uint64_t batches = 0;
  std::uint64_t records = 0;

  auto consume = [&] {
    while (true) {
      const bool done = producers_done.load() == kProducers;
      const auto batch = ring.Peek();
      if (!batch) {
        if (done && ring.IsEmpty()) return;
        std::this_thread::yield();
        continue;
      }
      // The batch must lie inside the storage.
      EXPECT(batch->bytes.data() >= storage.data());
      EXPECT(batch->bytes.data() + batch->bytes.size() <=
             storage.data() + storage.size());

      // "DMA" copies the batch, then the record contents are checked.
      std::vector<std::uint8_t> copy(batch->bytes.begin(), batch->bytes.end());
      std::size_t offset = 0;
      std::uint16_t parsed = 0;
      while (offset < copy.size()) {
        const std::uint8_t producer = copy[offset];
        std::uint32_t sequence = 0;
        std::memcpy(&sequence, &copy[offset + 1], sizeof(sequence));
        const std::size_t size = copy[offset + 5];
        if (producer >= kProducers || size < 6 ||
            offset + size > copy.size()) {
          EXPECT(false && "malformed record");
          return;
        }
        EXPECT(sequence == next_sequence[producer]);
        next_sequence[producer] = sequence + 1;
        for (std::size_t i = 6; i < size; ++i) {
          if (copy[offset + i] != PayloadByte(producer, sequence, i)) {
            EXPECT(false && "corrupted payload");
            return;
          }
        }
        offset += size;
        ++parsed;
      }
      EXPECT(parsed == batch->records);
      ++batches;
      records += parsed;
      ring.Release(*batch);
    }
  };

  std::thread consumer(consume);
  std::vector<std::thread> threads;
  for (int p = 0; p < kProducers; ++p) {
    threads.emplace_back(produce, static_cast<std::uint8_t>(p));
  }
  for (auto& t : threads) t.join();
  consumer.join();

  for (int p = 0; p < kProducers; ++p) {
    EXPECT(next_sequence[p] == kRecordsPerProducer);
  }
  std::uint64_t total_dropped = 0;
  for (auto d : dropped) total_dropped += d;
  std::printf("stress: %llu records in %llu batches (%.2f per batch), "
              "%llu full retries\n",
              static_cast<unsigned long long>(records),
              static_cast<unsigned long long>(batches),
              static_cast<double>(records) / static_cast<double>(batches),
              static_cast<unsigned long long>(total_dropped));
}

}  // namespace

int main() {
  TestRejectsInvalid();
  TestBatchesAndRelease();
  TestSkipsInsteadOfSplitting();
  TestStopsAtBufferEnd();
  TestFull();
  TestSlotReuse();
  TestStress();
  if (failures != 0) {
    std::printf("FAILED: %d\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("OK\n");
  return EXIT_SUCCESS;
}
