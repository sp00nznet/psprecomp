# Tests are self-contained: no game data, no ROMs, no disc images. Every input
# is either a hand-written instruction encoding or a synthetic buffer built in
# the test itself.

add_executable(test_decode test_decode.c)
target_link_libraries(test_decode PRIVATE allegrex_core)
add_test(NAME decode COMMAND test_decode)

add_executable(test_runtime test_runtime.c)
target_link_libraries(test_runtime PRIVATE psprecomp)
add_test(NAME runtime COMMAND test_runtime)

add_executable(test_container test_container.c)
target_link_libraries(test_container PRIVATE allegrex_core)
add_test(NAME container COMMAND test_container)

# Published standard vectors only (FIPS-197, SP 800-38A, RFC 4493) — no PSP
# key material, no game data.
add_executable(test_crypto test_crypto.c)
target_link_libraries(test_crypto PRIVATE allegrex_core)
add_test(NAME crypto COMMAND test_crypto)

add_executable(test_kirk test_kirk.c)
target_link_libraries(test_kirk PRIVATE allegrex_core)
add_test(NAME kirk COMMAND test_kirk)

add_executable(test_emit test_emit.c)
target_link_libraries(test_emit PRIVATE allegrex_core)
add_test(NAME emit COMMAND test_emit)

add_executable(test_hle test_hle.c)
target_link_libraries(test_hle PRIVATE psprecomp allegrex_core)
add_test(NAME hle COMMAND test_hle)

set(IO_OPEN_ROOT "${CMAKE_CURRENT_BINARY_DIR}/io-open-root")
file(MAKE_DIRECTORY "${IO_OPEN_ROOT}/disc")
file(WRITE "${IO_OPEN_ROOT}/disc/probe.txt" "ok")
add_executable(test_io_open test_io_open.c)
target_link_libraries(test_io_open PRIVATE psprecomp)
target_compile_definitions(test_io_open PRIVATE PSP_TEST_IO_ROOT="${IO_OPEN_ROOT}")
add_test(NAME io_open COMMAND test_io_open)

add_executable(test_scheduler test_scheduler.c)
target_link_libraries(test_scheduler PRIVATE psprecomp allegrex_core)
add_test(NAME scheduler COMMAND test_scheduler)
set_tests_properties(hle scheduler PROPERTIES TIMEOUT 60)

add_executable(test_thread_lifecycle test_thread_lifecycle.c)
target_link_libraries(test_thread_lifecycle PRIVATE psprecomp allegrex_core)
foreach(case delete pinned pending checkpoint0 checkpoint1 checkpoint2 checkpoint3 mask reset)
    add_test(NAME thread_${case} COMMAND test_thread_lifecycle ${case})
    set_tests_properties(thread_${case} PROPERTIES TIMEOUT 60)
endforeach()

add_executable(test_vfpu test_vfpu.c)
target_link_libraries(test_vfpu PRIVATE psprecomp)
add_test(NAME vfpu COMMAND test_vfpu)

add_executable(test_ctors test_ctors.c)
target_link_libraries(test_ctors PRIVATE psprecomp)
add_test(NAME ctors COMMAND test_ctors)

add_executable(test_raster test_raster.c)
target_link_libraries(test_raster PRIVATE psprecomp)
add_test(NAME raster COMMAND test_raster)

add_executable(test_module test_module.c)
target_link_libraries(test_module PRIVATE psprecomp)
add_test(NAME module COMMAND test_module)
