#ifndef CLICK_BRIDGE_MAIN_CLICK_PROTOCOL_H_
#define CLICK_BRIDGE_MAIN_CLICK_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ClickButtons {
  bool plus;
  bool minus;
};

// Parses a Click V1 0x37 button message. Absent fields retain their previous
// values. Returns false without modifying buttons for malformed/other messages.
bool ClickParseNotification(const uint8_t* data, size_t length,
                            struct ClickButtons* buttons);

#endif  // CLICK_BRIDGE_MAIN_CLICK_PROTOCOL_H_
