idf_component_register(
  SRCS "click_bridge.c" "click_protocol.c" "hid_keyboard.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES bt nvs_flash)

target_compile_options(${COMPONENT_LIB} PRIVATE
  -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes)
