// Adapted from lewangdev/ai-passport-iphone-agent (MIT), hardware/ai_passport/main/agent_hid.c.
// Added a consumer-control report (media and volume keys) and a single combined API.
#include "sdkconfig.h"

#if CONFIG_PASSPORT_PHONE_HID

#include "phone_hid.h"

#include <string.h>

#include "esp_hidd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char* TAG = "PhoneHid";

// NimBLE's store implementation has no public header (the ESP-IDF examples declare it too).
void ble_store_config_init(void);

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE 2
#define REPORT_ID_CONSUMER 3
#define APPEARANCE_HID 0x03C0

#define MOUSE_REPORT_LEN 5     // buttons, dx, dy, wheel, pan
#define KEYBOARD_REPORT_LEN 8  // modifiers, reserved, key[6]
#define CONSUMER_REPORT_LEN 2  // one 16-bit usage

static const uint8_t REPORT_MAP[] = {
    // Keyboard (report 1)
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, REPORT_ID_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,

    // Mouse (report 2): 3 buttons, X, Y, wheel, AC Pan; all relative
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, REPORT_ID_MOUSE,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,

    // Consumer control (report 3): one 16-bit usage (play/pause, volume, ...)
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, REPORT_ID_CONSUMER,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

static esp_hid_raw_report_map_t s_maps[] = {
    {.data = REPORT_MAP, .len = sizeof(REPORT_MAP)},
};

static esp_hid_device_config_t s_config = {
    .vendor_id = 0x303A,
    .product_id = 0x8001,
    .version = 0x0100,
    .device_name = "XiaoZhi Passport",
    .manufacturer_name = "FoloToy",
    .serial_number = "0001",
    .report_maps = s_maps,
    .report_maps_len = 1,
};

static esp_hidd_dev_t* s_dev;
static volatile phone_hid_state_t s_state = PHONE_HID_OFF;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static const char* s_name = "XiaoZhi Passport";

static int gap_event(struct ble_gap_event* event, void* arg);

static void advertise(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = APPEARANCE_HID;
    fields.appearance_is_present = 1;
    fields.name = (uint8_t*)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;
    static ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return;
    }
    // iOS reads the scan response; an empty one makes the name disappear from the list.
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (uint8_t*)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    rsp.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    rsp.tx_pwr_lvl_is_present = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response fields failed: %d", rc);
        return;
    }
    // Slow advertising: the phone reconnects within a second or two anyway and this
    // leaves more airtime to Wi-Fi (shared antenna) for the voice stream.
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(200);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(300);
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
        return;
    }
    s_state = PHONE_HID_ADVERTISING;
}

static int gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                advertise();
                return 0;
            }
            s_conn = event->connect.conn_handle;
            s_state = PHONE_HID_CONNECTED;
            ESP_LOGI(TAG, "phone connected");
            (void)ble_gap_security_initiate(s_conn);
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "phone disconnected: reason=%d", event->disconnect.reason);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            advertise();
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                s_state = PHONE_HID_READY;
                ESP_LOGI(TAG, "link encrypted, HID ready");
            } else {
                ESP_LOGW(TAG, "encryption failed: %d", event->enc_change.status);
            }
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
                struct ble_sm_io io;
                memset(&io, 0, sizeof(io));
                io.action = BLE_SM_IOACT_NUMCMP;
                io.numcmp_accept = 1;
                ble_sm_inject_io(event->passkey.conn_handle, &io);
            }
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
                advertise();
            }
            return 0;
        default:
            return 0;
    }
}

static void hidd_event(void* handler_args, esp_event_base_t base, int32_t id, void* event_data) {
    (void)handler_args;
    (void)base;
    (void)event_data;
    if (id == ESP_HIDD_START_EVENT) {
        if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
            ESP_LOGE(TAG, "no usable BLE address");
            return;
        }
        advertise();
    }
}

static void host_task(void* param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t phone_hid_start(const char* name) {
    if (s_dev) {
        return ESP_OK;
    }
    if (name && *name) {
        s_name = name;
        s_config.device_name = name;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    // Just Works bonding with LE Secure Connections: one "Pair" tap on the iPhone.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    err = esp_hidd_dev_init(&s_config, ESP_HID_TRANSPORT_BLE, hidd_event, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_svc_gap_device_name_set(s_name);
    ble_svc_gap_device_appearance_set(APPEARANCE_HID);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

phone_hid_state_t phone_hid_state(void) {
    return s_state;
}

// NimBLE's mbuf pool is small; wait for room instead of letting esp_hidd fail.
static bool wait_for_buffers(void) {
    for (int i = 0; i < 40; i++) {
        if (os_msys_num_free() >= 4) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        if (s_state != PHONE_HID_READY) {
            return false;
        }
    }
    return false;
}

static bool send(uint8_t id, uint8_t* report, size_t len) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (s_state != PHONE_HID_READY || !wait_for_buffers()) {
            return false;
        }
        if (esp_hidd_dev_input_set(s_dev, 0, id, report, len) == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

bool phone_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    uint8_t r[MOUSE_REPORT_LEN] = {buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel, 0};
    return send(REPORT_ID_MOUSE, r, sizeof(r));
}

bool phone_hid_key_tap(uint8_t modifiers, uint8_t keycode) {
    uint8_t r[KEYBOARD_REPORT_LEN] = {modifiers, 0, keycode, 0, 0, 0, 0, 0};
    bool ok = send(REPORT_ID_KEYBOARD, r, sizeof(r));
    vTaskDelay(pdMS_TO_TICKS(30));
    uint8_t up[KEYBOARD_REPORT_LEN] = {0};
    return send(REPORT_ID_KEYBOARD, up, sizeof(up)) && ok;
}

bool phone_hid_consumer_tap(uint16_t usage) {
    uint8_t r[CONSUMER_REPORT_LEN] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
    bool ok = send(REPORT_ID_CONSUMER, r, sizeof(r));
    vTaskDelay(pdMS_TO_TICKS(40));
    uint8_t up[CONSUMER_REPORT_LEN] = {0, 0};
    return send(REPORT_ID_CONSUMER, up, sizeof(up)) && ok;
}

esp_err_t phone_hid_forget(void) {
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    int rc = ble_store_clear();
    ESP_LOGI(TAG, "bonds cleared: %d", rc);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

#endif  // CONFIG_PASSPORT_PHONE_HID
