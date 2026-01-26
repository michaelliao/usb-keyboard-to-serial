idf_component_register(SRCS "keyboard_main.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS ""
                       REQUIRES usb_host_hid driver
)
