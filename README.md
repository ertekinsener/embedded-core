# embedded-core

Hardware-independent building blocks for bare-metal C++23 firmware: no heap,
no exceptions, no RTTI, all host-tested. Used by the
[stm32_webserver](https://github.com/ertekinsener/stm32_webserver) firmware
(both cores of an STM32H755), its Zephyr port and the PC build of
embedded-webserver.

| Target | Header | What |
|---|---|---|
| `embedded_core::ring` | `ring/mpsc_ring.h` | `ring::MpscRing<kCapacity, kRecordSlots>`: lock-free multi-producer, single-consumer ring of variable-size records. Producers reserve with one CAS, the consumer peeks the committed records as one contiguous span (one DMA transfer) and releases them. Storage attached at runtime (e.g. a DMA section). Header only. |
| `embedded_core::logging` | `logging/logger.h`, `logging/format.h` | `logging::Info/Warning/Error/Debug(tag, "fmt {}", args...)`: format string checked at compile time (`{}`, `{:x}`, `{:08x}`, no float), any context, never blocks; the line is formatted on the caller's stack and handed to a sink (function pointer + context) with a timestamp from a clock callback. Dropped lines are counted and reported. |
| `embedded_core::rfc2217` | `bridge/rfc2217.h` | `rfc2217::Session`: Telnet COM Port Control (RFC 2217) server codec: option negotiation, IAC escaping, line settings through a handler, line-state notifications, signature. |
| `embedded_core::flash_log` | `storage/flash_log.h` | `storage::FlashLog`: power-loss safe record log over two flash sectors, through `storage::FlashSectors` (erase, program and read of the platform). |

## Using it

```cmake
add_subdirectory(<path>/embedded-core)
target_link_libraries(firmware embedded_core::logging embedded_core::ring)
```

The targets are INTERFACE libraries whose sources compile as part of the
target that links them, with its flags (`-mcpu`, `-fno-exceptions`, ...) and
definitions. Several libraries of one build may add this directory: the
first one defines the targets. Link them into the executable (or into one
static library that keeps them `PRIVATE`), not into several libraries that
end up in the same image, or the sources are compiled more than once.

## Host tests

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

The ring test runs with ThreadSanitizer (4 producer threads, one consumer);
`-DHOST_TEST_TSAN=OFF` without `libtsan`. `EMBEDDED_CORE_BUILD_TESTS` builds
the tests when this is not the top-level project.
