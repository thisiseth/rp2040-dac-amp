cmake_minimum_required(VERSION 3.13)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(rp2040_dac_amp)

add_compile_options(-O3)
add_compile_options(-Wno-stringop-overflow)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# rest of your project

add_executable(rp2040_dac_amp
    main.c
    usb_descriptors.c
    dacamp.c
)

pico_generate_pio_header(rp2040_dac_amp ${CMAKE_CURRENT_LIST_DIR}/hbridge.pio)

# Make sure TinyUSB can find tusb_config.h
target_include_directories(rp2040_dac_amp PUBLIC
    .
)

target_link_libraries(rp2040_dac_amp PUBLIC 
    pico_stdlib 
    pico_unique_id 
    tinyusb_device 
    tinyusb_board
    hardware_pio
    pico_multicore 
    pico_sync
    pico_platform
)

pico_set_binary_type(rp2040_dac_amp copy_to_ram)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(rp2040_dac_amp)