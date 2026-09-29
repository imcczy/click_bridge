#ifndef CLICK_BRIDGE_MAIN_HID_KEYBOARD_H_
#define CLICK_BRIDGE_MAIN_HID_KEYBOARD_H_

#include <stdbool.h>

// Call after nimble_port_init(), before starting the NimBLE host task.
// Returns zero on success, otherwise a NimBLE host error code.
int HidKeyboardInit(void);

// The functions below run only on the NimBLE host event queue.
void HidKeyboardReset(void);
void HidKeyboardStartAdvertising(void);

// Sends K for plus and I for minus over an encrypted, subscribed HID link.
// Retains the current state until the iPad is ready; false/false releases keys.
void HidKeyboardSendState(bool plus_pressed, bool minus_pressed);

#endif  // CLICK_BRIDGE_MAIN_HID_KEYBOARD_H_
