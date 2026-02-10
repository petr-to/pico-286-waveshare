add_library(usbhid INTERFACE)

target_sources(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/usbhid.c
        ${CMAKE_CURRENT_LIST_DIR}/usbhid.h
)

if(KBDUSB)
    set(USB_HID_LIBS pico_pio_usb tinyusb_host tinyusb_board hardware_gpio)
else()
    set(USB_HID_LIBS tinyusb_host tinyusb_board hardware_gpio)
endif()

target_link_libraries(usbhid INTERFACE 
        ${USB_HID_LIBS}
)

target_include_directories(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)
