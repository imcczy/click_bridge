#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "click_protocol.h"
#include "esp_log.h"
#include "hid_keyboard.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

static const char kTag[] = "click_bridge";
static const char kClickName[] = "Zwift Click";
static const uint8_t kRideOn[] = {'R', 'i', 'd', 'e', 'O', 'n'};

enum { kInvalidHandle = 0, kMaxClickCharacteristics = 10 };

/* NimBLE stores 128-bit UUID bytes least-significant byte first. */
static const ble_uuid128_t kZwiftServiceUuid =
    BLE_UUID128_INIT(0xd1, 0x09, 0xdd, 0xdc, 0x29, 0xfa, 0xe5, 0x86, 0x51, 0x46,
                     0xca, 0x19, 0x01, 0x00, 0x00, 0x00);
static const ble_uuid128_t kZwiftAsyncUuid =
    BLE_UUID128_INIT(0xd1, 0x09, 0xdd, 0xdc, 0x29, 0xfa, 0xe5, 0x86, 0x51, 0x46,
                     0xca, 0x19, 0x02, 0x00, 0x00, 0x00);
static const ble_uuid128_t kZwiftSyncRxUuid =
    BLE_UUID128_INIT(0xd1, 0x09, 0xdd, 0xdc, 0x29, 0xfa, 0xe5, 0x86, 0x51, 0x46,
                     0xca, 0x19, 0x03, 0x00, 0x00, 0x00);
static const ble_uuid128_t kZwiftSyncTxUuid =
    BLE_UUID128_INIT(0xd1, 0x09, 0xdd, 0xdc, 0x29, 0xfa, 0xe5, 0x86, 0x51, 0x46,
                     0xca, 0x19, 0x04, 0x00, 0x00, 0x00);
enum DescriptorTarget {
  kDescriptorAsync,
  kDescriptorSyncTx,
};

enum SubscribeStage {
  kSubscribeAsync,
  kSubscribeSyncTx,
};

static uint16_t click_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t click_svc_start;
static uint16_t click_svc_end;
static uint16_t async_val_handle;
static uint16_t async_cccd_handle;
static uint16_t sync_rx_val_handle;
static uint16_t sync_tx_val_handle;
static uint16_t sync_tx_cccd_handle;
static uint8_t async_properties;
static uint8_t sync_tx_properties;
static uint16_t click_chr_defs[kMaxClickCharacteristics];
static size_t click_chr_count;
static enum DescriptorTarget descriptor_target;
static bool connecting_to_click;
static struct ClickButtons buttons;

// ESP-IDF entry point; other project functions use Google-style PascalCase.
void app_main(void);

static int ClickGapEvent(struct ble_gap_event* event, void* arg);
static void ClickScan(void);
static void SubscribeAndHandshake(void);

static void ResetClickState(void) {
  click_conn_handle = BLE_HS_CONN_HANDLE_NONE;
  click_svc_start = kInvalidHandle;
  click_svc_end = kInvalidHandle;
  async_val_handle = kInvalidHandle;
  async_cccd_handle = kInvalidHandle;
  sync_rx_val_handle = kInvalidHandle;
  sync_tx_val_handle = kInvalidHandle;
  sync_tx_cccd_handle = kInvalidHandle;
  async_properties = 0;
  sync_tx_properties = 0;
  click_chr_count = 0;
  buttons = (struct ClickButtons){0};
}

static bool AdvertisementIsClick(const struct ble_gap_disc_desc* disc) {
  struct ble_hs_adv_fields fields;
  int rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
  if (rc != 0 || fields.name == NULL) {
    return false;
  }
  return fields.name_len == sizeof(kClickName) - 1 &&
         memcmp(fields.name, kClickName, fields.name_len) == 0;
}

static uint16_t CharacteristicEndHandle(uint16_t val_handle) {
  uint16_t end_handle = click_svc_end;
  for (size_t i = 0; i < click_chr_count; ++i) {
    if (click_chr_defs[i] > val_handle && click_chr_defs[i] - 1 < end_handle) {
      end_handle = click_chr_defs[i] - 1;
    }
  }
  return end_handle;
}

static void DisconnectClickAfterError(const char* message, int rc) {
  ESP_LOGE(kTag, "%s; rc=%d", message, rc);
  if (click_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    int terminate_rc =
        ble_gap_terminate(click_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (terminate_rc != 0 && terminate_rc != BLE_HS_ENOTCONN) {
      ESP_LOGE(kTag, "Could not disconnect Click; rc=%d", terminate_rc);
    }
  }
}

static uint16_t CccdValueForProperties(uint8_t properties) {
  if (properties & BLE_GATT_CHR_PROP_NOTIFY) {
    return 1;
  }
  if (properties & BLE_GATT_CHR_PROP_INDICATE) {
    return 2;
  }
  return 0;
}

static void HandleClickNotification(struct ble_gap_event* event) {
  if (event->notify_rx.conn_handle != click_conn_handle ||
      event->notify_rx.attr_handle != async_val_handle) {
    return;
  }
  uint16_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
  uint8_t data[32];
  struct ClickButtons next = buttons;
  if (length <= sizeof(data) &&
      os_mbuf_copydata(event->notify_rx.om, 0, length, data) == 0 &&
      ClickParseNotification(data, length, &next)) {
    if (next.plus != buttons.plus || next.minus != buttons.minus) {
      buttons = next;
      HidKeyboardSendState(buttons.plus, buttons.minus);
    }
    // Also start advertising if Click does not update link parameters.
    HidKeyboardStartAdvertising();
  }
}

static void SendRideOn(uint16_t conn_handle) {
  int rc = ble_gattc_write_no_rsp_flat(conn_handle, sync_rx_val_handle, kRideOn,
                                       sizeof(kRideOn));
  if (rc != 0) {
    DisconnectClickAfterError("Could not send RideOn handshake", rc);
  } else {
    ESP_LOGI(kTag, "Ready; press + or - on Click");
  }
}

static int SubscribeWriteComplete(uint16_t conn_handle,
                                  const struct ble_gatt_error* error,
                                  struct ble_gatt_attr* attr, void* arg) {
  (void)attr;
  enum SubscribeStage stage = (enum SubscribeStage)(intptr_t)arg;
  if (error->status != 0) {
    DisconnectClickAfterError("Click subscription failed", error->status);
    return 0;
  }

  if (stage == kSubscribeAsync) {
    uint16_t cccd_value = CccdValueForProperties(sync_tx_properties);
    int rc = ble_gattc_write_flat(conn_handle, sync_tx_cccd_handle, &cccd_value,
                                  sizeof(cccd_value), SubscribeWriteComplete,
                                  (void*)(intptr_t)kSubscribeSyncTx);
    if (rc != 0) {
      DisconnectClickAfterError(
          "Could not subscribe to Click handshake response", rc);
    }
    return 0;
  }

  SendRideOn(conn_handle);
  return 0;
}

static void SubscribeAndHandshake(void) {
  if (async_cccd_handle == kInvalidHandle ||
      sync_tx_cccd_handle == kInvalidHandle ||
      sync_rx_val_handle == kInvalidHandle) {
    DisconnectClickAfterError("Click is missing a required handle",
                              BLE_HS_ENOENT);
    return;
  }
  uint16_t cccd_value = CccdValueForProperties(async_properties);
  int rc = ble_gattc_write_flat(
      click_conn_handle, async_cccd_handle, &cccd_value, sizeof(cccd_value),
      SubscribeWriteComplete, (void*)(intptr_t)kSubscribeAsync);
  if (rc != 0) {
    DisconnectClickAfterError("Could not subscribe to Click buttons", rc);
  }
}

static int DescriptorDiscoveryCallback(uint16_t conn_handle,
                                       const struct ble_gatt_error* error,
                                       uint16_t chr_val_handle,
                                       const struct ble_gatt_dsc* dsc,
                                       void* arg) {
  (void)conn_handle;
  (void)chr_val_handle;
  (void)arg;
  if (error->status == 0) {
    if (ble_uuid_cmp(&dsc->uuid.u,
                     BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
      if (descriptor_target == kDescriptorAsync) {
        async_cccd_handle = dsc->handle;
      } else {
        sync_tx_cccd_handle = dsc->handle;
      }
    }
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    DisconnectClickAfterError("Click descriptor discovery failed",
                              error->status);
    return 0;
  }

  if (descriptor_target == kDescriptorAsync) {
    if (async_cccd_handle == kInvalidHandle) {
      DisconnectClickAfterError("Click async CCCD not found", BLE_HS_ENOENT);
      return 0;
    }
    descriptor_target = kDescriptorSyncTx;
    int rc =
        ble_gattc_disc_all_dscs(click_conn_handle, sync_tx_val_handle,
                                CharacteristicEndHandle(sync_tx_val_handle),
                                DescriptorDiscoveryCallback, NULL);
    if (rc != 0) {
      DisconnectClickAfterError("Could not discover sync TX CCCD", rc);
    }
  } else {
    if (sync_tx_cccd_handle == kInvalidHandle) {
      DisconnectClickAfterError("Click sync TX CCCD not found", BLE_HS_ENOENT);
      return 0;
    }
    SubscribeAndHandshake();
  }
  return 0;
}

static int CharacteristicDiscoveryCallback(uint16_t conn_handle,
                                           const struct ble_gatt_error* error,
                                           const struct ble_gatt_chr* chr,
                                           void* arg) {
  (void)conn_handle;
  (void)arg;
  if (error->status == 0) {
    if (click_chr_count == kMaxClickCharacteristics) {
      DisconnectClickAfterError("Too many Click characteristics",
                                BLE_HS_ENOMEM);
      return BLE_HS_ENOMEM;
    }
    click_chr_defs[click_chr_count++] = chr->def_handle;
    if (ble_uuid_cmp(&chr->uuid.u, &kZwiftAsyncUuid.u) == 0) {
      async_val_handle = chr->val_handle;
      async_properties = chr->properties;
    } else if (ble_uuid_cmp(&chr->uuid.u, &kZwiftSyncRxUuid.u) == 0) {
      sync_rx_val_handle = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &kZwiftSyncTxUuid.u) == 0) {
      sync_tx_val_handle = chr->val_handle;
      sync_tx_properties = chr->properties;
    }
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    DisconnectClickAfterError("Click characteristic discovery failed",
                              error->status);
    return 0;
  }
  if (async_val_handle == kInvalidHandle ||
      sync_rx_val_handle == kInvalidHandle ||
      sync_tx_val_handle == kInvalidHandle ||
      CccdValueForProperties(async_properties) == 0 ||
      CccdValueForProperties(sync_tx_properties) == 0) {
    DisconnectClickAfterError("Click characteristics are incomplete",
                              BLE_HS_ENOENT);
    return 0;
  }

  descriptor_target = kDescriptorAsync;
  int rc = ble_gattc_disc_all_dscs(click_conn_handle, async_val_handle,
                                   CharacteristicEndHandle(async_val_handle),
                                   DescriptorDiscoveryCallback, NULL);
  if (rc != 0) {
    DisconnectClickAfterError("Could not discover Click button CCCD", rc);
  }
  return 0;
}

static int ServiceDiscoveryCallback(uint16_t conn_handle,
                                    const struct ble_gatt_error* error,
                                    const struct ble_gatt_svc* service,
                                    void* arg) {
  (void)arg;
  if (error->status == 0) {
    click_svc_start = service->start_handle;
    click_svc_end = service->end_handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    DisconnectClickAfterError("Click service discovery failed", error->status);
    return 0;
  }
  if (click_svc_start == kInvalidHandle) {
    DisconnectClickAfterError("Zwift service not found", BLE_HS_ENOENT);
    return 0;
  }
  int rc = ble_gattc_disc_all_chrs(conn_handle, click_svc_start, click_svc_end,
                                   CharacteristicDiscoveryCallback, NULL);
  if (rc != 0) {
    DisconnectClickAfterError("Could not discover Click characteristics", rc);
  }
  return 0;
}

static void ConnectToClick(const ble_addr_t* addr) {
  uint8_t own_addr_type;
  int rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    connecting_to_click = false;
    ClickScan();
    return;
  }

  rc = ble_gap_connect(own_addr_type, addr, 30000, NULL, ClickGapEvent, NULL);
  if (rc != 0) {
    connecting_to_click = false;
    ESP_LOGE(kTag, "Could not connect to Click; rc=%d", rc);
    ClickScan();
  }
}

static int ClickGapEvent(struct ble_gap_event* event, void* arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_DISC:
      if (!connecting_to_click && AdvertisementIsClick(&event->disc)) {
        ble_addr_t addr = event->disc.addr;
        connecting_to_click = true;
        ble_gap_disc_cancel();
        ConnectToClick(&addr);
      }
      return 0;
    case BLE_GAP_EVENT_CONNECT: {
      connecting_to_click = false;
      if (event->connect.status != 0) {
        ESP_LOGW(kTag, "Click connection failed; status=%d",
                 event->connect.status);
        ClickScan();
        return 0;
      }
      ResetClickState();
      click_conn_handle = event->connect.conn_handle;
      ESP_LOGI(kTag, "Click connected");
      int rc =
          ble_gattc_disc_svc_by_uuid(click_conn_handle, &kZwiftServiceUuid.u,
                                     ServiceDiscoveryCallback, NULL);
      if (rc != 0) {
        DisconnectClickAfterError("Could not discover Zwift service", rc);
      }
      return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      if (event->disconnect.conn.conn_handle != click_conn_handle) {
        return 0;
      }
      ESP_LOGW(kTag, "Click disconnected; reason=%d", event->disconnect.reason);
      // A lost release notification must not leave an iPad key held down.
      HidKeyboardSendState(false, false);
      ResetClickState();
      ClickScan();
      return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
      HandleClickNotification(event);
      return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
      if (event->conn_update.conn_handle == click_conn_handle &&
          event->conn_update.status == 0) {
        ESP_LOGI(kTag, "Click link stable; enabling HID advertising");
        HidKeyboardStartAdvertising();
      }
      return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      if (!connecting_to_click &&
          click_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ClickScan();
      }
      return 0;
    default:
      return 0;
  }
}

static void ClickScan(void) {
  if (click_conn_handle != BLE_HS_CONN_HANDLE_NONE || connecting_to_click ||
      ble_gap_disc_active()) {
    return;
  }
  uint8_t own_addr_type;
  int rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    ESP_LOGE(kTag, "Could not determine scan address type; rc=%d", rc);
    return;
  }
  /* Active scan at 10% duty cycle; scanning stops after Click connects. */
  struct ble_gap_disc_params params = {
      .itvl = 160,
      .window = 16,
      .filter_policy = 0,
      .limited = 0,
      .passive = 0,
      .filter_duplicates = 1,
  };
  ESP_LOGI(kTag, "Scanning for Click (wake it with a button press)");
  rc =
      ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, ClickGapEvent, NULL);
  if (rc != 0) {
    ESP_LOGE(kTag, "Could not start Click scan; rc=%d", rc);
  }
}

static void HostReset(int reason) {
  ESP_LOGE(kTag, "NimBLE reset; reason=%d", reason);
  connecting_to_click = false;
  ResetClickState();
  HidKeyboardReset();
}

static void HostSync(void) {
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGE(kTag, "Could not create BLE identity address; rc=%d", rc);
    return;
  }
  ClickScan();
}

static void HostTask(void* param) {
  (void)param;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ResetClickState();
  ESP_ERROR_CHECK(nimble_port_init());
  int rc = HidKeyboardInit();
  if (rc != 0) {
    ESP_LOGE(kTag, "HID initialization failed; rc=%d", rc);
    ESP_ERROR_CHECK(ESP_FAIL);
  }
  ble_hs_cfg.reset_cb = HostReset;
  ble_hs_cfg.sync_cb = HostSync;
  nimble_port_freertos_init(HostTask);
}
