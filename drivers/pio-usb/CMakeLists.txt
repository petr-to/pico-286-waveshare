add_library(pico_pio_usb INTERFACE)

set(pio_usb_dir ${CMAKE_CURRENT_LIST_DIR})

# Generate PIO headers
pico_generate_pio_header(pico_pio_usb ${pio_usb_dir}/usb_tx.pio)
pico_generate_pio_header(pico_pio_usb ${pio_usb_dir}/usb_rx.pio)

target_sources(pico_pio_usb INTERFACE
    ${pio_usb_dir}/pio_usb.c
    ${pio_usb_dir}/pio_usb_device.c
    ${pio_usb_dir}/pio_usb_host.c
    ${pio_usb_dir}/usb_crc.c
)

target_link_libraries(pico_pio_usb INTERFACE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
)

target_include_directories(pico_pio_usb INTERFACE ${pio_usb_dir})

# Enable all warnings
target_compile_options(pico_pio_usb INTERFACE -Wall -Wextra)
