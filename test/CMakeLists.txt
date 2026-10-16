# Host tests of embedded-core (MPSC ring, formatter, logger front end,
# RFC 2217 codec, flash log), built with the host compiler when
# embedded-core is the top-level project:
#
#   cmake -S . -B build && cmake --build build
#   ctest --test-dir build --output-on-failure
#
# The ring test needs libtsan (-DHOST_TEST_TSAN=OFF without it).

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HOST_TEST_TSAN "Build the ring test with ThreadSanitizer" ON)

add_executable(mpsc_ring_test ring/mpsc_ring_test.cc)
target_link_libraries(mpsc_ring_test PRIVATE embedded_core::ring)
target_compile_options(mpsc_ring_test PRIVATE -Wall -Wextra -O2 -g)
if(HOST_TEST_TSAN)
  target_compile_options(mpsc_ring_test PRIVATE -fsanitize=thread)
  target_link_options(mpsc_ring_test PRIVATE -fsanitize=thread)
endif()
add_test(NAME mpsc_ring_test COMMAND mpsc_ring_test)

add_executable(logging_test logging/logging_test.cc)
target_link_libraries(logging_test PRIVATE embedded_core::logging)
target_compile_options(logging_test PRIVATE -Wall -Wextra -O1 -g)
add_test(NAME logging_test COMMAND logging_test)

add_executable(rfc2217_test bridge/rfc2217_test.cc)
target_link_libraries(rfc2217_test PRIVATE embedded_core::rfc2217)
target_compile_options(rfc2217_test PRIVATE -Wall -Wextra -O1 -g)
add_test(NAME rfc2217_test COMMAND rfc2217_test)

add_executable(flash_log_test storage/flash_log_test.cc)
target_link_libraries(flash_log_test PRIVATE embedded_core::flash_log)
target_compile_options(flash_log_test PRIVATE -Wall -Wextra -O1 -g)
add_test(NAME flash_log_test COMMAND flash_log_test)
