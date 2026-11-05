cmake_minimum_required(VERSION 3.13)

# PicoCalc (Pico 2 W) Meshtastic client.
# Produces build/picocalc_meshtastic.uf2 for use with the PicoCalc UF2 loader
# (copy it into the `pico2-apps` folder of the SD card).

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()

if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/.deps/pico-sdk)
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/.deps/pico-sdk)
endif()

include(pico_sdk_import.cmake)

project(picocalc_meshtastic C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(APP_VERSION "1.0.0")

file(GLOB PROTO_SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/proto/meshtastic/*.pb.c)

add_executable(picocalc_meshtastic
    src/main.c
    src/hw/lcd.c
    src/hw/keyboard.c
    src/hw/sdcard.c
    src/hw/sound.c
    src/storage.c
    src/log.c
    src/bt_glue.c
    src/mesh/ble_link.c
    src/mesh/mesh.c
    src/mesh/hw_names.c
    src/ui/gfx.c
    src/ui/font_6x12.c
    src/ui/font_8x16.c
    src/ui/ui.c
    src/ui/widgets.c
    src/ui/scr_messages.c
    src/ui/scr_nodes.c
    src/ui/scr_map.c
    src/ui/scr_channels.c
    src/ui/scr_connect.c
    src/ui/scr_settings.c
    src/ui/scr_stats.c
    src/ui/scr_log.c
    src/ui/scr_help.c
    third_party/fatfs/ff.c
    third_party/fatfs/ffunicode.c
    third_party/nanopb/pb_common.c
    third_party/nanopb/pb_encode.c
    third_party/nanopb/pb_decode.c
    ${PROTO_SOURCES}
)

target_include_directories(picocalc_meshtastic PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/proto
    ${CMAKE_CURRENT_LIST_DIR}/third_party/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/third_party/nanopb
)

target_compile_definitions(picocalc_meshtastic PRIVATE
    APP_VERSION="${APP_VERSION}"
    PB_FIELD_32BIT=1
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    CYW43_LWIP=0
)

target_compile_options(picocalc_meshtastic PRIVATE -Wall -Wno-unused-function -Wno-format-truncation)
set_source_files_properties(${PROTO_SOURCES}
    third_party/fatfs/ff.c third_party/fatfs/ffunicode.c
    PROPERTIES COMPILE_OPTIONS "-w")

# Note: pico_btstack_cyw43 is intentionally NOT linked. It would place the
# bonding database in the last sectors of flash, which is unsafe when running
# under the PicoCalc UF2 loader. src/bt_glue.c provides btstack_cyw43_init()
# with bonding keys stored on the SD card instead.
target_link_libraries(picocalc_meshtastic
    pico_stdlib
    pico_rand
    hardware_spi
    hardware_i2c
    hardware_dma
    hardware_pwm
    pico_cyw43_arch_poll
    pico_btstack_base
    pico_btstack_ble
    pico_btstack_run_loop_async_context
    pico_btstack_hci_transport_cyw43
)

pico_set_program_name(picocalc_meshtastic "Meshtastic")
pico_set_program_description(picocalc_meshtastic "Meshtastic BLE client for PicoCalc")
pico_set_program_version(picocalc_meshtastic "${APP_VERSION}")

pico_enable_stdio_usb(picocalc_meshtastic 1)
pico_enable_stdio_uart(picocalc_meshtastic 0)

pico_add_extra_outputs(picocalc_meshtastic)
