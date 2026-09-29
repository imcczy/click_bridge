idf_component_register(SRCS "click_bridge.c" "hid_keyboard.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash)
