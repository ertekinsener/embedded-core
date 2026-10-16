# embedded-core: hardware-independent building blocks for bare-metal C++23
# firmware (README.md). No heap, exceptions or RTTI; host-tested in test/.
#
#   add_subdirectory(<path>/embedded-core)
#   target_link_libraries(<target> embedded_core::logging ...)
#
# The libraries are INTERFACE targets whose sources compile as part of the
# target linking them, with that target's flags and definitions (one build
# per firmware image, no ABI mismatch between differently configured
# libraries).
#
# Targets:
#   embedded_core::ring       ring/mpsc_ring.h, lock-free MPSC record ring
#                             (header only)
#   embedded_core::logging    logging/{format,logger}.h, compile-time checked
#                             formatter and non-blocking logger front end
#   embedded_core::rfc2217    bridge/rfc2217.h, Telnet COM Port Control codec
#   embedded_core::flash_log  storage/flash_log.h, power-loss safe record log
#                             over two flash sectors
cmake_minimum_required(VERSION 3.22)
project(embedded_core LANGUAGES CXX)

# Included by several libraries of one build: the first one defines the
# targets.
if(TARGET embedded_core_ring)
  return()
endif()

set(EMBEDDED_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(embedded_core_ring INTERFACE)
add_library(embedded_core::ring ALIAS embedded_core_ring)
target_include_directories(embedded_core_ring
                           INTERFACE "${EMBEDDED_CORE_DIR}/include")
target_compile_features(embedded_core_ring INTERFACE cxx_std_23)

add_library(embedded_core_logging INTERFACE)
add_library(embedded_core::logging ALIAS embedded_core_logging)
target_sources(
  embedded_core_logging
  INTERFACE "${EMBEDDED_CORE_DIR}/source/logging/format.cc"
            "${EMBEDDED_CORE_DIR}/source/logging/logger.cc")
target_include_directories(embedded_core_logging
                           INTERFACE "${EMBEDDED_CORE_DIR}/include")
target_compile_features(embedded_core_logging INTERFACE cxx_std_23)

add_library(embedded_core_rfc2217 INTERFACE)
add_library(embedded_core::rfc2217 ALIAS embedded_core_rfc2217)
target_sources(embedded_core_rfc2217
               INTERFACE "${EMBEDDED_CORE_DIR}/source/bridge/rfc2217.cc")
target_include_directories(embedded_core_rfc2217
                           INTERFACE "${EMBEDDED_CORE_DIR}/include")
target_compile_features(embedded_core_rfc2217 INTERFACE cxx_std_23)

add_library(embedded_core_flash_log INTERFACE)
add_library(embedded_core::flash_log ALIAS embedded_core_flash_log)
target_sources(embedded_core_flash_log
               INTERFACE "${EMBEDDED_CORE_DIR}/source/storage/flash_log.cc")
target_include_directories(embedded_core_flash_log
                           INTERFACE "${EMBEDDED_CORE_DIR}/include")
target_compile_features(embedded_core_flash_log INTERFACE cxx_std_23)

# Host tests: on by default only when this is the top-level project.
option(EMBEDDED_CORE_BUILD_TESTS "Build the host tests" ${PROJECT_IS_TOP_LEVEL})
if(EMBEDDED_CORE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
