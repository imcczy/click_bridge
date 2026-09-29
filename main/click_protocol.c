#include "click_protocol.h"

static bool ReadVarint(const uint8_t* data, size_t length, size_t* offset,
                       uint64_t* value) {
  uint64_t result = 0;
  for (unsigned int shift = 0; shift < 64 && *offset < length; shift += 7) {
    uint8_t byte = data[(*offset)++];
    if (shift == 63 && (byte & 0xfe) != 0) {
      return false;
    }
    result |= (uint64_t)(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return true;
    }
  }
  return false;
}

bool ClickParseNotification(const uint8_t* data, size_t length,
                            struct ClickButtons* buttons) {
  if (data == NULL || buttons == NULL || length < 3 || data[0] != 0x37) {
    return false;
  }

  struct ClickButtons next = *buttons;
  bool has_buttons = false;
  size_t offset = 1;
  while (offset < length) {
    uint64_t key;
    uint64_t value;
    if (!ReadVarint(data, length, &offset, &key) || key == 0 ||
        key > UINT32_MAX || (key & 0x07) != 0 ||
        !ReadVarint(data, length, &offset, &value)) {
      return false;
    }
    uint64_t field = key >> 3;
    if (field != 1 && field != 2) {
      continue;
    }
    if (value > 1) {
      return false;
    }
    // Click V1 encodes pressed as zero, released as one.
    if (field == 1) {
      next.plus = value == 0;
    } else {
      next.minus = value == 0;
    }
    has_buttons = true;
  }
  if (has_buttons) {
    *buttons = next;
  }
  return has_buttons;
}
