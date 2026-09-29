#pragma once

#include <stdbool.h>

int hid_keyboard_init(void);
void hid_keyboard_reset(void);
void hid_keyboard_start_advertising(void);
void hid_keyboard_send_state(bool plus_pressed, bool minus_pressed);

