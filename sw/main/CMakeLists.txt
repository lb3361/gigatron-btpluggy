set(srcs "main.c"
         "gap.c"
         "hid.c"
         "hid_parser.c"
         "keyboard.c"
         "gamepad.c"
         "led.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES esp_hid esp_driver_rmt esp_driver_gpio
                       PRIV_REQUIRES nvs_flash esp_timer)
