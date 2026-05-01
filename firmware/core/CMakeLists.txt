cmake_minimum_required(VERSION 3.13)

# Host-only test build for the portable core library.
# Use:
#   cmake -S . -B build-host
#   cmake --build build-host
#   ./build-host/waxwing_test
#
# The core/ directory contains platform-agnostic C used by every
# firmware target (Pico W, CardPuter, etc.). HAL boundaries are
# defined by core/filestore.h and core/hal_crypto.h; tests link
# against stubs in tests/ rather than real implementations.

project(waxwing_core C)

enable_testing()

set(CORE_SOURCES
    attest_cache.c
    attestations.c
    cborencode.c
    cbor_decode.c
    commands.c
    encounters.c
    identity.c
    manifest_counter.c
    mesh_state.c
    peer_sync.c
    peer_table.c
    ssid_scan.c
    thirdparty/sha256/sha256.c
)

set(TEST_SOURCES
    tests/test_runner.c
    tests/test_attest_cache.c
    tests/test_attestations.c
    tests/test_commands.c
    tests/test_commands_scan.c
    tests/test_encounters.c
    tests/test_identity.c
    tests/test_manifest_counter.c
    tests/test_mesh_state.c
    tests/test_peer_sync.c
    tests/test_peer_table.c
    tests/test_sha256.c
    tests/test_ssid_scan.c
    tests/mock_filestore.c
    tests/stub_ble.c
    tests/stub_hal_crypto.c
    tests/stub_ssid_scan.c
)

# Prefix core sources so consumers see them as core/<file> from the parent
# include root (firmware/), matching the #include "core/foo.h" convention.
list(TRANSFORM CORE_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

add_executable(waxwing_test ${CORE_SOURCES} ${TEST_SOURCES})

# firmware/ as include root so #include "core/foo.h" resolves;
# tests/ for in-suite headers (mock_filestore.h, stub_hal_crypto.h, test.h).
target_include_directories(waxwing_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

add_test(NAME waxwing_test COMMAND waxwing_test)
