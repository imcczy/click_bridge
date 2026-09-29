#include "click_protocol.h"

#include <assert.h>
#include <stdio.h>

static void TestPressReleaseSequence(void) {
  static const uint8_t kFrames[][5] = {
      {0x37, 0x08, 0x01, 0x10, 0x01}, {0x37, 0x08, 0x00, 0x10, 0x01},
      {0x37, 0x08, 0x01, 0x10, 0x01}, {0x37, 0x08, 0x01, 0x10, 0x00},
      {0x37, 0x08, 0x00, 0x10, 0x00}, {0x37, 0x08, 0x01, 0x10, 0x01},
  };
  static const struct ClickButtons kExpected[] = {
      {false, false}, {true, false}, {false, false},
      {false, true},  {true, true},  {false, false},
  };
  struct ClickButtons buttons = {0};
  for (size_t i = 0; i < sizeof(kFrames) / sizeof(kFrames[0]); ++i) {
    assert(ClickParseNotification(kFrames[i], sizeof(kFrames[i]), &buttons));
    assert(buttons.plus == kExpected[i].plus);
    assert(buttons.minus == kExpected[i].minus);
  }
}

static void TestPartialAndUnknownFields(void) {
  struct ClickButtons buttons = {.minus = true};
  static const uint8_t kPlus[] = {0x37, 0x08, 0x00};
  assert(ClickParseNotification(kPlus, sizeof(kPlus), &buttons));
  assert(buttons.plus && buttons.minus);

  // Unknown varint fields do not hide the following button fields.
  static const uint8_t kUnknown[] = {0x37, 0x18, 0xac, 0x02, 0x10, 0x01};
  assert(ClickParseNotification(kUnknown, sizeof(kUnknown), &buttons));
  assert(buttons.plus && !buttons.minus);
}

static void ExpectRejected(const uint8_t* data, size_t length) {
  struct ClickButtons buttons = {.plus = false, .minus = true};
  assert(!ClickParseNotification(data, length, &buttons));
  assert(!buttons.plus && buttons.minus);
}

static void TestMalformedMessages(void) {
  static const uint8_t kWrongOpcode[] = {0x15, 0x08, 0x00};
  static const uint8_t kTruncatedKey[] = {0x37, 0x08, 0x00, 0x80};
  static const uint8_t kTruncatedValue[] = {0x37, 0x08, 0x00, 0x10, 0x80};
  static const uint8_t kInvalidButton[] = {0x37, 0x08, 0x00, 0x10, 0x02};
  static const uint8_t kUnsupportedWireType[] = {0x37, 0x0a, 0x00};
  static const uint8_t kInvalidField[] = {0x37, 0x00, 0x00};
  static const uint8_t kNoButtons[] = {0x37, 0x18, 0x01};
  static const uint8_t kOverflow[] = {0x37, 0x18, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0x02};
  static const uint8_t kNarrowingOverflow[] = {0x37, 0x08, 0x80, 0x80,
                                               0x80, 0x80, 0x10};
  ExpectRejected(NULL, 0);
  ExpectRejected(kWrongOpcode, sizeof(kWrongOpcode));
  ExpectRejected(kTruncatedKey, sizeof(kTruncatedKey));
  ExpectRejected(kTruncatedValue, sizeof(kTruncatedValue));
  ExpectRejected(kInvalidButton, sizeof(kInvalidButton));
  ExpectRejected(kUnsupportedWireType, sizeof(kUnsupportedWireType));
  ExpectRejected(kInvalidField, sizeof(kInvalidField));
  ExpectRejected(kNoButtons, sizeof(kNoButtons));
  ExpectRejected(kOverflow, sizeof(kOverflow));
  ExpectRejected(kNarrowingOverflow, sizeof(kNarrowingOverflow));
}

static void TestInvalidInputDoesNotChangeState(void) {
  uint32_t seed = 1;
  for (size_t sample = 0; sample < 50000; ++sample) {
    uint8_t data[32] = {0x37};
    size_t length = sample % sizeof(data);
    for (size_t i = 1; i < length; ++i) {
      seed = seed * 1664525u + 1013904223u;
      data[i] = (uint8_t)(seed >> 24);
    }
    struct ClickButtons buttons = {.plus = true};
    if (!ClickParseNotification(data, length, &buttons)) {
      assert(buttons.plus && !buttons.minus);
    }
  }
}

int main(void) {
  TestPressReleaseSequence();
  TestPartialAndUnknownFields();
  TestMalformedMessages();
  TestInvalidInputDoesNotChangeState();
  puts("Click protocol tests passed (including 50,000 generated-input trials)");
  return 0;
}
