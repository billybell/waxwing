cmake_minimum_required(VERSION 3.13)

# Check if PICO_SDK_PATH is set via environment variable
if(NOT DEFINED ENV{PICO_SDK_PATH})
    message(FATAL_ERROR "PICO_SDK_PATH environment variable not set. Please set it to the path of the Raspberry Pi Pico SDK before running cmake.")
endif()

# Set PICO_SDK_PATH as a CMake variable from the environment variable
set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")

# Include the Pico SDK (must be done BEFORE project())
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# Set the project name
project(waxwing_mesh C CXX ASM)

# Initialize the Pico SDK
pico_sdk_init()

# Set the source files
set(SOURCES
    src/main.c
    src/identity.c
    src/utils.c
    src/hw/pico-w/ble.c
    src/diskio.c
    src/hw/pico-w/filestore_fatfs.c
    src/core/cborencode.c
    src/core/cbor_decode.c
    src/core/commands.c

    # FatFS (local copy with custom ffconf.h).
    src/fatfs/ff.c
    src/fatfs/ffsystem.c
    src/fatfs/ffunicode.c
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
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hw/pico-w)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/fatfs)
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
