# Standalone unit tests for pure plugin logic. Enabled with
# -DLENSLINK_BUILD_TESTS=ON; run with ctest (or the binaries directly).

find_package(Threads REQUIRED)

set(_src ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(test-json-reader test-json-reader.c ${_src}/json-reader.c)
target_include_directories(test-json-reader PRIVATE ${_src})
add_test(NAME json-reader COMMAND test-json-reader)

add_executable(test-camera-state test-camera-state.c ${_src}/camera-state.c
                                 ${_src}/json-reader.c)
target_include_directories(test-camera-state PRIVATE ${_src})
if(NOT WIN32)
  target_link_libraries(test-camera-state PRIVATE m)
endif()
add_test(NAME camera-state COMMAND test-camera-state)

add_executable(test-handshake test-handshake.c ${_src}/handshake.c
                              ${_src}/device-identity.c ${_src}/json-reader.c)
target_include_directories(test-handshake PRIVATE ${_src})
if(NOT WIN32)
  target_link_libraries(test-handshake PRIVATE m)
endif()
add_test(NAME handshake COMMAND test-handshake)

add_executable(test-mdns-parse test-mdns-parse.c ${_src}/mdns-parse.c
                               ${_src}/device-identity.c)
target_include_directories(test-mdns-parse PRIVATE ${_src})
add_test(NAME mdns-parse COMMAND test-mdns-parse)

# The registry only needs libobs's threading header (plain pthreads
# outside Windows) — not the library, which on macOS would drag in OBS's
# bundled FFmpeg at load time. Its provider upcalls are stubbed in the
# test. Windows links libobs for w32-pthreads.
add_executable(test-source-registry test-source-registry.c
                                    ${_src}/source-registry.c
                                    ${_src}/device-identity.c)
target_include_directories(test-source-registry PRIVATE ${_src}
  $<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(test-source-registry PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(test-source-registry PRIVATE OBS::libobs)
endif()
add_test(NAME source-registry COMMAND test-source-registry)

# Fuzz harnesses for the parsers of untrusted input (tests/fuzz/). By
# default each runs under a deterministic mutation driver as an ordinary
# test; configure with -DLENSLINK_FUZZ_LIBFUZZER=ON (clang with libFuzzer)
# to build real libFuzzer binaries instead, run by hand.
option(LENSLINK_FUZZ_LIBFUZZER "Build the fuzz harnesses against libFuzzer" OFF)
set(LENSLINK_FUZZ_ITERATIONS 200000 CACHE STRING
    "Mutations per fuzz harness when run by ctest")

function(lenslink_fuzz name)
  add_executable(fuzz-${name} fuzz/fuzz-${name}.c ${ARGN})
  target_include_directories(fuzz-${name} PRIVATE ${_src} fuzz)
  if(NOT WIN32)
    target_link_libraries(fuzz-${name} PRIVATE m)
  endif()
  if(LENSLINK_FUZZ_LIBFUZZER)
    target_compile_options(fuzz-${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz-${name} PRIVATE -fsanitize=fuzzer)
  else()
    target_sources(fuzz-${name} PRIVATE fuzz/fuzz-driver.c)
    add_test(NAME fuzz-${name}
             COMMAND fuzz-${name} ${LENSLINK_FUZZ_ITERATIONS})
  endif()
endfunction()

lenslink_fuzz(json ${_src}/handshake.c ${_src}/camera-state.c
              ${_src}/json-reader.c ${_src}/device-identity.c)
lenslink_fuzz(mdns ${_src}/mdns-parse.c ${_src}/device-identity.c)

# The dock's logic, split from its widgets so it needs neither Qt nor
# libobs: the scene walk behind the Preview tally, the persisted camera
# order, and the model that turns registry snapshots into cards.
add_executable(test-scene-walk test-scene-walk.c ${_src}/scene-walk.c)
target_include_directories(test-scene-walk PRIVATE ${_src})
add_test(NAME scene-walk COMMAND test-scene-walk)

add_executable(test-rig-order test-rig-order.c ${_src}/rig-order.c
                              ${_src}/scene-walk.c)
target_include_directories(test-rig-order PRIVATE ${_src})
add_test(NAME rig-order COMMAND test-rig-order)

add_executable(test-rig-model test-rig-model.c ${_src}/rig-model.c
                              ${_src}/rig-order.c ${_src}/scene-walk.c
                              ${_src}/camera-state.c ${_src}/json-reader.c
                              ${_src}/device-identity.c)
target_include_directories(test-rig-model PRIVATE ${_src})
# The real English strings: the test fails on a key the model asks for
# that en-US.ini doesn't define.
target_compile_definitions(test-rig-model PRIVATE
  LENSLINK_EN_INI="${CMAKE_CURRENT_SOURCE_DIR}/../data/locale/en-US.ini")
if(NOT WIN32)
  target_link_libraries(test-rig-model PRIVATE m)
endif()
add_test(NAME rig-model COMMAND test-rig-model)
