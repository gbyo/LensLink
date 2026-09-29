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

# The registry only needs libobs's threading header (plain pthreads
# outside Windows) — not the library, which on macOS would drag in OBS's
# bundled FFmpeg at load time. Its provider upcalls are stubbed in the
# test. Windows links libobs for w32-pthreads.
add_executable(test-source-registry test-source-registry.c
                                    ${_src}/source-registry.c)
target_include_directories(test-source-registry PRIVATE ${_src}
  $<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(test-source-registry PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(test-source-registry PRIVATE OBS::libobs)
endif()
add_test(NAME source-registry COMMAND test-source-registry)
