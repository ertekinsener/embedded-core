# Host-side tests for the hardware-independent code in shared/ (TX/log ring,
# formatter, logger front end, RFC 2217 codec of the Ethernet-UART bridge,
# settings flash log). Not part of the firmware builds; configure
# separately with the host compiler:
#
#   cmake -S shared/test -B build/host-test && cmake --build build/host-test
#   ctest --test-dir build/host-test --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(shared_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HOST_TEST_TSAN "Build the ring test with ThreadSanitizer" ON)

set(shared_dir "${CMAKE_CURRENT_SOURCE_DIR}/..")

enable_testing()

add_executable(mpsc_ring_test ring/mpsc_ring_test.cc)
target_include_directories(mpsc_ring_test PRIVATE "${shared_dir}/include")
target_compile_options(mpsc_ring_test PRIVATE -Wall -Wextra -O2 -g)
if(HOST_TEST_TSAN)
  target_compile_options(mpsc_ring_test PRIVATE -fsanitize=thread)
  target_link_options(mpsc_ring_test PRIVATE -fsanitize=thread)
endif()
add_test(NAME mpsc_ring_test COMMAND mpsc_ring_test)

add_executable(
  logging_test logging/logging_test.cc "${shared_dir}/source/logging/format.cc"
               "${shared_dir}/source/logging/logger.cc")
target_include_directories(logging_test PRIVATE "${shared_dir}/include")
target_compile_options(logging_test PRIVATE -Wall -Wextra -O1 -g)
add_test(NAME logging_test COMMAND logging_test)

add_executable(rfc2217_test bridge/rfc2217_test.cc
                            "${shared_dir}/source/bridge/rfc2217.cc")
target_include_directories(rfc2217_test PRIVATE "${shared_dir}/include")
target_compile_options(rfc2217_test PRIVATE -Wall -Wextra -O1 -g)
add_test(NAME rfc2217_test COMMAND rfc2217_test)

add_executable(flash_log_test storage/flash_log_test.cc
                              "${shared_dir}/source/storage/flash_log.cc")
target_include_directories(flash_log_test PRIVATE "${shared_dir}/include")
target_compile_options(flash_log_test PRIVATE -Wall -Wextra -O1 -g)
add_test(NAME flash_log_test COMMAND flash_log_test)
