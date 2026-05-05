cmake_minimum_required(VERSION 3.13)

# Host-only test build: skip the entire Pico SDK / cross-compile path.
# Use:  cmake -S . -B build-host -DWAXWING_HOST_BUILD=ON
# This lets the test target build with the host compiler even when
# PICO_SDK_PATH is exported in the user's shell.
option(WAXWING_HOST_BUILD "Build host tests only (skip Pico SDK / firmware)" OFF)

set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
if(NOT WAXWING_HOST_BUILD AND DEFINED ENV{PICO_SDK_PATH})
    include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

    # Set the project name
    project(waxwing_mesh C CXX ASM)

    # Initialize the Pico SDK
    pico_sdk_init()

    # Set the source files
    set(SOURCES
        src/hw/pico-w/main.c
        src/hw/pico-w/utils.c
        src/hw/pico-w/ble.c
        src/hw/pico-w/ble_client.c
        src/hw/pico-w/diskio.c
        src/hw/pico-w/filestore_fatfs.c
        src/hw/pico-w/hal_crypto_pico.c
        src/hw/pico-w/ssid_scan_pico.c
        ../core/attest_cache.c
        ../core/attest_query.c
        ../core/attestations.c
        ../core/cborencode.c
        ../core/cbor_decode.c
        ../core/commands.c
        ../core/encounter_record.c
        ../core/encounters.c
        ../core/identity.c
        ../core/manifest_counter.c
        ../core/mesh_state.c
        ../core/peer_sync.c
        ../core/peer_table.c
        ../core/ssid_scan.c

         # FatFS (local copy with custom ffconf.h).
        src/thirdparty/fatfs/ff.c
        src/thirdparty/fatfs/ffsystem.c
        src/thirdparty/fatfs/ffunicode.c

         # Monocypher: standard Ed25519 (RFC 8032, curve25519 + SHA-512)
         # used by hal_crypto_pico.c. monocypher-ed25519.c re-uses
         # primitives from monocypher.c — both files required.
         # Lives in firmware/core/thirdparty/ so the CardPuter port
         # can use the same code; firmware/ is on the include path.
        ../core/thirdparty/monocypher/monocypher.c
        ../core/thirdparty/monocypher/monocypher-ed25519.c

         # Vendored SHA-256 (FIPS 180-4) shared with CardPuter and host
         # tests. Used to hash file contents at write time so manifest
         # entries carry a real content hash.
        ../core/thirdparty/sha256/sha256.c
     )

    # Create the executable
    add_executable(${PROJECT_NAME}
         ${SOURCES}
     )

    # Add pico_stdlib library which aggregates commonly used features
    target_link_libraries(${PROJECT_NAME}
        pico_stdlib
        pico_cyw43_arch_lwip_poll
        pico_btstack_cyw43
        pico_btstack_ble
     )

    # Add include directories
    # ${CMAKE_CURRENT_SOURCE_DIR}/.. is firmware/ — makes #include "core/foo.h"
    # resolve to the shared firmware/core/ tree.
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hw/pico-w)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/thirdparty/fatfs)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${PICO_SDK_PATH}/lib/btstack/src)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${PICO_SDK_PATH}/lib/btstack/platform/embedded)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_arch/include)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${PICO_SDK_PATH}/lib/cyw43-driver/src)

    # Compile GATT file to generate ATT server database
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GATT_COMPILER "${PICO_SDK_PATH}/lib/btstack/tool/compile_gatt.py")
    set(GATT_DB "${CMAKE_BINARY_DIR}/waxwing_ble_att_server.h")
    add_custom_command(
        OUTPUT ${GATT_DB}
        COMMAND ${Python3_EXECUTABLE} ${GATT_COMPILER} ${CMAKE_CURRENT_SOURCE_DIR}/src/hw/pico-w/waxwing_ble.gatt ${GATT_DB}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/hw/pico-w/waxwing_ble.gatt ${GATT_COMPILER}
        COMMENT "Compiling GATT database"
     )
    add_custom_target(waxwing_gatt DEPENDS ${GATT_DB})
    add_dependencies(${PROJECT_NAME} waxwing_gatt)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR})

    # Enable UART0 for stdio
    pico_enable_stdio_uart(${PROJECT_NAME} 1)
    # Enable USB for stdio
    pico_enable_stdio_usb(${PROJECT_NAME} 1)

    # Create map/bin/hex/uf2 files
    pico_add_extra_outputs(${PROJECT_NAME})

    # Add URL for setting via pico_set_program_url
    pico_set_program_name(${PROJECT_NAME} "waxwing_mesh")
    pico_set_program_version(${PROJECT_NAME} "0.1.0")
    pico_set_program_description(${PROJECT_NAME} "Waxwing Mesh Firmware for Pico W")
    pico_set_program_url(${PROJECT_NAME} "https://github.com/yourusername/waxwing")

else()
     # No SDK: skip firmware target, set minimal project for test-only builds.
    project(waxwing C)
endif()

# Host-only tests live in firmware/core/ — see firmware/core/CMakeLists.txt.
