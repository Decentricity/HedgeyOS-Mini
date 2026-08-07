#include "BluetoothKeyboardHost.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gap_bt_api.h>
#include <esp_gattc_api.h>
#include <esp_hid_common.h>
#include <esp_hidh.h>
#include <esp_hidh_gattc.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace
{
const char *TAG = "BT_KEYBOARD";
const char *DEVICE_NAME = "HedgeyOS Mini";
const uint16_t CLASSIC_HID_UUID = 0x1124;
const uint32_t STORE_MAGIC = 0x484B4231;
const int MAX_STORED = 5;
const int MAX_SCAN_RESULTS = 24;
const EventBits_t BLE_PARAMS_READY = 1 << 0;
const EventBits_t BLE_SCAN_DONE = 1 << 1;
const EventBits_t OPEN_OK = 1 << 2;
const EventBits_t OPEN_FAILED = 1 << 3;
const EventBits_t BT_SCAN_DONE = 1 << 4;

struct StoredDevice
{
  uint8_t address[6];
  uint8_t transport;
  uint8_t address_type;
};

struct StoredDevices
{
  uint32_t magic;
  uint8_t count;
  uint8_t reserved[3];
  StoredDevice devices[MAX_STORED];
};

struct ScanResult
{
  uint8_t address[6];
  esp_hid_transport_t transport;
  uint8_t address_type;
  int8_t rssi;
  bool likely_keyboard;
  bool has_name;
  char name[40];
};

bool same_address(const uint8_t *left, const uint8_t *right)
{
  return memcmp(left, right, 6) == 0;
}
}

struct BluetoothKeyboardHost::Impl
{
  explicit Impl(NotifyCallback callback) : notify(callback)
  {
    characters = xQueueCreate(512, sizeof(char));
    events = xEventGroupCreate();
    status_mutex = xSemaphoreCreateMutex();
    input_timer = xTimerCreate("bt_key_batch", pdMS_TO_TICKS(140), pdFALSE,
                               this, input_timer_callback);
    stored.magic = STORE_MAGIC;
    stored.count = 0;
    memset(stored.reserved, 0, sizeof(stored.reserved));
    memset(stored.devices, 0, sizeof(stored.devices));
    strcpy(status_text, "Keyboard: off");
  }

  NotifyCallback notify;
  QueueHandle_t characters = nullptr;
  EventGroupHandle_t events = nullptr;
  SemaphoreHandle_t status_mutex = nullptr;
  TimerHandle_t input_timer = nullptr;
  TaskHandle_t manager_task = nullptr;
  bool initialized = false;
  bool accepting = false;
  bool connected = false;
  bool scanning = false;
  bool caps_lock = false;
  volatile bool pairing_requested = false;
  volatile bool connection_requested = false;
  volatile int selected_device = -1;
  esp_hidh_dev_t *device = nullptr;
  uint8_t previous_keys[6] = {};
  char status_text[72] = {};
  StoredDevices stored = {};
  ScanResult scan_results[MAX_SCAN_RESULTS] = {};
  int scan_count = 0;
  portMUX_TYPE scan_lock = portMUX_INITIALIZER_UNLOCKED;

  static Impl *instance;

  void set_status(const char *value, bool refresh = true)
  {
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    strncpy(status_text, value, sizeof(status_text) - 1);
    status_text[sizeof(status_text) - 1] = 0;
    xSemaphoreGive(status_mutex);
    if (accepting && refresh)
      notify(STATUS_CHANGED);
  }

  void set_pairing_code(uint32_t code)
  {
    char message[72];
    snprintf(message, sizeof(message), "Type %06u + Enter", static_cast<unsigned>(code));
    set_status(message);
  }

  std::string get_status() const
  {
    Impl *self = const_cast<Impl *>(this);
    xSemaphoreTake(self->status_mutex, portMAX_DELAY);
    std::string result(self->status_text);
    xSemaphoreGive(self->status_mutex);
    return result;
  }

  void load_stored()
  {
    nvs_handle handle;
    if (nvs_open("hedgey_bt", NVS_READONLY, &handle) != ESP_OK)
      return;
    size_t size = sizeof(stored);
    StoredDevices loaded = {};
    if (nvs_get_blob(handle, "keyboards", &loaded, &size) == ESP_OK &&
        size == sizeof(loaded) && loaded.magic == STORE_MAGIC && loaded.count <= MAX_STORED)
      stored = loaded;
    nvs_close(handle);
  }

  void save_stored()
  {
    nvs_handle handle;
    if (nvs_open("hedgey_bt", NVS_READWRITE, &handle) != ESP_OK)
      return;
    if (nvs_set_blob(handle, "keyboards", &stored, sizeof(stored)) == ESP_OK)
      nvs_commit(handle);
    nvs_close(handle);
  }

  int stored_index(const uint8_t *address, esp_hid_transport_t transport) const
  {
    for (int i = 0; i < stored.count; ++i)
      if (stored.devices[i].transport == transport &&
          same_address(stored.devices[i].address, address))
        return i;
    return -1;
  }

  void remember(const uint8_t *address, esp_hid_transport_t transport, uint8_t address_type)
  {
    if (stored_index(address, transport) >= 0 || stored.count >= MAX_STORED)
      return;
    StoredDevice &entry = stored.devices[stored.count++];
    memcpy(entry.address, address, 6);
    entry.transport = transport;
    entry.address_type = address_type;
    save_stored();
    ESP_LOGI(TAG, "Remembered keyboard %d of %d", stored.count, MAX_STORED);
  }

  void add_scan_result(const uint8_t *address, esp_hid_transport_t transport,
                       uint8_t address_type, int8_t rssi, const char *name,
                       bool likely_keyboard, bool has_name)
  {
    portENTER_CRITICAL(&scan_lock);
    for (int i = 0; i < scan_count; ++i)
    {
      if (scan_results[i].transport == transport && same_address(scan_results[i].address, address))
      {
        ScanResult &result = scan_results[i];
        result.rssi = std::max(result.rssi, rssi);
        result.likely_keyboard = result.likely_keyboard || likely_keyboard;
        if (has_name)
        {
          strncpy(result.name, name, sizeof(result.name) - 1);
          result.name[sizeof(result.name) - 1] = 0;
          result.has_name = true;
        }
        portEXIT_CRITICAL(&scan_lock);
        return;
      }
    }
    if (scan_count < MAX_SCAN_RESULTS)
    {
      ScanResult &result = scan_results[scan_count++];
      memcpy(result.address, address, 6);
      result.transport = transport;
      result.address_type = address_type;
      result.rssi = rssi;
      result.likely_keyboard = likely_keyboard;
      result.has_name = has_name;
      strncpy(result.name,
              name ? name
                   : (transport == ESP_HID_TRANSPORT_BT ? "Unnamed Classic device"
                                                        : "Unnamed BLE device"),
              sizeof(result.name) - 1);
      result.name[sizeof(result.name) - 1] = 0;
    }
    else
    {
      const int incoming_priority = likely_keyboard ? 2 : (has_name ? 1 : 0);
      int replace = -1;
      for (int i = 0; i < scan_count; ++i)
      {
        const int priority = scan_results[i].likely_keyboard ? 2 :
                             (scan_results[i].has_name ? 1 : 0);
        if (priority < incoming_priority &&
            (replace < 0 || scan_results[i].rssi < scan_results[replace].rssi))
          replace = i;
      }
      if (replace >= 0)
      {
        ScanResult &result = scan_results[replace];
        memcpy(result.address, address, 6);
        result.transport = transport;
        result.address_type = address_type;
        result.rssi = rssi;
        result.likely_keyboard = likely_keyboard;
        result.has_name = has_name;
        strncpy(result.name, name, sizeof(result.name) - 1);
        result.name[sizeof(result.name) - 1] = 0;
      }
    }
    portEXIT_CRITICAL(&scan_lock);
  }

  int choose_stored_result()
  {
    for (int stored_position = 0; stored_position < stored.count; ++stored_position)
      for (int result_position = 0; result_position < scan_count; ++result_position)
        if (stored.devices[stored_position].transport == scan_results[result_position].transport &&
            same_address(stored.devices[stored_position].address, scan_results[result_position].address))
          return result_position;

    return -1;
  }

  static void input_timer_callback(TimerHandle_t timer)
  {
    Impl *self = static_cast<Impl *>(pvTimerGetTimerID(timer));
    if (self->accepting)
      self->notify(INPUT_READY);
  }

  void queue_character(char character)
  {
    if (!accepting)
      return;
    if (xQueueSend(characters, &character, 0) == pdTRUE)
      xTimerReset(input_timer, 0);
  }

  char translate_key(uint8_t key, uint8_t modifiers)
  {
    const bool shift = (modifiers & 0x22) != 0;
    if (key >= 0x04 && key <= 0x1D)
    {
      char value = 'a' + key - 0x04;
      if (shift != caps_lock)
        value -= 'a' - 'A';
      return value;
    }
    if (key >= 0x1E && key <= 0x27)
    {
      static const char normal[] = "1234567890";
      static const char shifted[] = "!@#$%^&*()";
      return shift ? shifted[key - 0x1E] : normal[key - 0x1E];
    }
    switch (key)
    {
    case 0x28: return '\n';
    case 0x2A: return '\b';
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
    }
  }

  void handle_keyboard_report(const uint8_t *data, size_t length)
  {
    const size_t offset = length == 9 ? 1 : 0;
    if (length < offset + 8)
      return;
    const uint8_t modifiers = data[offset];
    const uint8_t *keys = data + offset + 2;
    for (int i = 0; i < 6; ++i)
    {
      const uint8_t key = keys[i];
      if (!key || std::find(previous_keys, previous_keys + 6, key) != previous_keys + 6)
        continue;
      if (key == 0x39)
        caps_lock = !caps_lock;
      else if (key == 0x2B)
      {
        queue_character(' '); queue_character(' '); queue_character(' '); queue_character(' ');
      }
      else
      {
        const char character = translate_key(key, modifiers);
        if (character)
          queue_character(character);
      }
    }
    memcpy(previous_keys, keys, 6);
  }

  static void hidh_callback(void *, esp_event_base_t, int32_t id, void *event_data)
  {
    Impl *self = instance;
    esp_hidh_event_data_t *event = static_cast<esp_hidh_event_data_t *>(event_data);
    if (!self)
      return;
    if (id == ESP_HIDH_OPEN_EVENT)
    {
      self->device = event->open.dev;
      self->connected = true;
      const uint8_t *address = esp_hidh_dev_bda_get(self->device);
      const esp_hid_transport_t transport = esp_hidh_dev_transport_get(self->device);
      uint8_t address_type = 0;
      for (int i = 0; i < self->scan_count; ++i)
        if (self->scan_results[i].transport == transport && same_address(address, self->scan_results[i].address))
          address_type = self->scan_results[i].address_type;
      self->remember(address, transport, address_type);
      char message[72];
      snprintf(message, sizeof(message), "Keyboard: %.54s",
               esp_hidh_dev_name_get(self->device) ? esp_hidh_dev_name_get(self->device) : "connected");
      self->set_status(message);
      xEventGroupSetBits(self->events, OPEN_OK);
    }
    else if (id == ESP_HIDH_INPUT_EVENT && event->input.usage == ESP_HID_USAGE_KEYBOARD)
      self->handle_keyboard_report(event->input.data, event->input.length);
    else if (id == ESP_HIDH_CLOSE_EVENT)
    {
      const bool was_active = event->close.dev == self->device;
      esp_hidh_dev_free(event->close.dev);
      if (was_active)
      {
        self->device = nullptr;
        self->connected = false;
        memset(self->previous_keys, 0, sizeof(self->previous_keys));
        self->set_status(self->pairing_requested ? "Keyboard: pairing..." :
                                                   "Keyboard: disconnected");
      }
      xEventGroupSetBits(self->events, OPEN_FAILED);
    }
  }

  static void ble_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
  {
    Impl *self = instance;
    if (!self)
      return;
    if (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT)
      xEventGroupSetBits(self->events, BLE_PARAMS_READY);
    else if (event == ESP_GAP_BLE_SCAN_RESULT_EVT)
    {
      if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT)
        xEventGroupSetBits(self->events, BLE_SCAN_DONE);
      else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT)
      {
        uint8_t uuid_length = 0;
        uint8_t *uuid_data = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                       ESP_BLE_AD_TYPE_16SRV_CMPL, &uuid_length);
        if (!uuid_data)
          uuid_data = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                               ESP_BLE_AD_TYPE_16SRV_PART, &uuid_length);
        bool advertises_hid = false;
        for (uint8_t offset = 0; uuid_data && offset + 1 < uuid_length; offset += 2)
          if (static_cast<uint16_t>(uuid_data[offset] | (uuid_data[offset + 1] << 8)) ==
              ESP_GATT_UUID_HID_SVC)
            advertises_hid = true;
        uint8_t appearance_length = 0;
        uint8_t *appearance_data = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                             ESP_BLE_AD_TYPE_APPEARANCE,
                                                             &appearance_length);
        uint16_t appearance = appearance_data && appearance_length >= 2
                                  ? appearance_data[0] | (appearance_data[1] << 8) : 0;
        const bool likely_keyboard = advertises_hid ||
                                     appearance == ESP_HID_APPEARANCE_KEYBOARD;
        uint8_t name_length = 0;
        uint8_t *name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                  ESP_BLE_AD_TYPE_NAME_CMPL, &name_length);
        if (!name)
          name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                          ESP_BLE_AD_TYPE_NAME_SHORT, &name_length);
        const bool has_name = name && name_length;
        char name_text[40];
        snprintf(name_text, sizeof(name_text), "BLE %02X:%02X:%02X:%02X:%02X:%02X",
                 param->scan_rst.bda[0], param->scan_rst.bda[1],
                 param->scan_rst.bda[2], param->scan_rst.bda[3],
                 param->scan_rst.bda[4], param->scan_rst.bda[5]);
        if (name && name_length)
        {
          const size_t copy_length = std::min<size_t>(name_length, sizeof(name_text) - 1);
          memcpy(name_text, name, copy_length);
          name_text[copy_length] = 0;
        }
        // Keep non-HID BLE devices out of the chooser so Classic HID keyboards
        // are not drowned by phones/headsets advertising only GAP/GATT.
        if (likely_keyboard)
          self->add_scan_result(param->scan_rst.bda, ESP_HID_TRANSPORT_BLE,
                                param->scan_rst.ble_addr_type, param->scan_rst.rssi,
                                name_text, likely_keyboard, has_name);
      }
    }
    else if (event == ESP_GAP_BLE_PASSKEY_NOTIF_EVT)
      self->set_pairing_code(param->ble_security.key_notif.passkey);
    else if (event == ESP_GAP_BLE_NC_REQ_EVT)
    {
      self->set_pairing_code(param->ble_security.key_notif.passkey);
      esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
    }
    else if (event == ESP_GAP_BLE_SEC_REQ_EVT)
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
    else if (event == ESP_GAP_BLE_AUTH_CMPL_EVT && param->ble_security.auth_cmpl.success)
      self->set_status("Keyboard: paired", false);
  }

  static bool eir_has_hid_uuid(const uint8_t *eir)
  {
    if (!eir)
      return false;
    uint8_t length = 0;
    uint8_t *data = esp_bt_gap_resolve_eir_data(const_cast<uint8_t *>(eir),
                                                ESP_BT_EIR_TYPE_CMPL_16BITS_UUID, &length);
    if (!data)
      data = esp_bt_gap_resolve_eir_data(const_cast<uint8_t *>(eir),
                                          ESP_BT_EIR_TYPE_INCMPL_16BITS_UUID, &length);
    for (uint8_t offset = 0; data && offset + 1 < length; offset += 2)
      if (static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8)) == CLASSIC_HID_UUID)
        return true;
    return false;
  }

  static void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
  {
    Impl *self = instance;
    if (!self)
      return;
    if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT)
    {
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED)
        xEventGroupSetBits(self->events, BT_SCAN_DONE);
    }
    else if (event == ESP_BT_GAP_DISC_RES_EVT)
    {
      uint32_t cod_value = 0;
      int8_t rssi = 0;
      const uint8_t *name = nullptr;
      uint8_t name_length = 0;
      bool has_hid_uuid = false;
      const uint8_t *eir = nullptr;
      for (int i = 0; i < param->disc_res.num_prop; ++i)
      {
        esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
        if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME)
        {
          name = static_cast<const uint8_t *>(prop->val);
          name_length = strlen(reinterpret_cast<const char *>(name));
        }
        else if (prop->type == ESP_BT_GAP_DEV_PROP_RSSI)
          rssi = *static_cast<int8_t *>(prop->val);
        else if (prop->type == ESP_BT_GAP_DEV_PROP_COD)
          memcpy(&cod_value, prop->val, sizeof(uint32_t));
        else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR)
        {
          eir = static_cast<const uint8_t *>(prop->val);
          has_hid_uuid = eir_has_hid_uuid(eir);
          if (!name)
          {
            uint8_t length = 0;
            uint8_t *data = esp_bt_gap_resolve_eir_data(const_cast<uint8_t *>(eir),
                                                        ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &length);
            if (!data)
              data = esp_bt_gap_resolve_eir_data(const_cast<uint8_t *>(eir),
                                                  ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &length);
            if (data && length)
            {
              name = data;
              name_length = length;
            }
          }
          if (!has_hid_uuid)
            has_hid_uuid = eir_has_hid_uuid(eir);
        }
      }
      const uint8_t major = (cod_value & ESP_BT_COD_MAJOR_DEV_BIT_MASK) >>
                            ESP_BT_COD_MAJOR_DEV_BIT_OFFSET;
      const bool peripheral = major == ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
      // Cyberdeck2024 advertises as a phone with Classic HID 0x1124. Inquiry
      // often lacks the UUID in EIR, so include phone/computer majors too.
      const bool host_like = major == ESP_BT_COD_MAJOR_DEV_PHONE ||
                             major == ESP_BT_COD_MAJOR_DEV_COMPUTER;
      const bool likely_keyboard = peripheral || has_hid_uuid;
      if (!(likely_keyboard || host_like || has_hid_uuid))
        return;
      if (!(name && name_length) && !likely_keyboard && !has_hid_uuid)
        return;

      char name_text[40];
      snprintf(name_text, sizeof(name_text), "BT %02X:%02X:%02X:%02X:%02X:%02X",
               param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
               param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5]);
      bool has_name = false;
      if (name && name_length)
      {
        const size_t copy_length = std::min<size_t>(name_length, sizeof(name_text) - 1);
        memcpy(name_text, name, copy_length);
        name_text[copy_length] = 0;
        has_name = true;
      }
      self->add_scan_result(param->disc_res.bda, ESP_HID_TRANSPORT_BT, 0, rssi,
                            name_text, likely_keyboard || has_hid_uuid || host_like,
                            has_name);
    }
    else if (event == ESP_BT_GAP_KEY_NOTIF_EVT)
      self->set_pairing_code(param->key_notif.passkey);
    else if (event == ESP_BT_GAP_CFM_REQ_EVT)
    {
      self->set_pairing_code(param->cfm_req.num_val);
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
    }
    else if (event == ESP_BT_GAP_PIN_REQ_EVT)
    {
      esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
    }
    else if (event == ESP_BT_GAP_AUTH_CMPL_EVT && param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
      self->set_status("Keyboard: paired", false);
  }

  bool initialize()
  {
    if (initialized)
      return true;
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK)
    {
      set_status("Keyboard: NVS error");
      return false;
    }
    load_stored();
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&controller_config) != ESP_OK ||
        esp_bt_controller_enable(ESP_BT_MODE_BTDM) != ESP_OK ||
        esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK)
    {
      set_status("Keyboard: Bluetooth error");
      return false;
    }

    esp_bt_dev_set_device_name(DEVICE_NAME);
    esp_ble_gap_set_device_name(DEVICE_NAME);

    esp_bt_sp_param_t sp_param = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t bt_iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(sp_param, &bt_iocap, sizeof(bt_iocap));
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {};
    esp_bt_gap_set_pin(pin_type, 0, pin_code);
    esp_bt_gap_register_callback(bt_gap_callback);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    esp_ble_gap_register_callback(ble_gap_callback);
    esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);

    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t ble_iocap = ESP_IO_CAP_OUT;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &ble_iocap, sizeof(ble_iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));

    esp_hidh_config_t hidh_config = {};
    hidh_config.callback = hidh_callback;
    if (esp_hidh_init(&hidh_config) != ESP_OK)
    {
      set_status("Keyboard: HID error");
      return false;
    }
    initialized = true;
    return true;
  }

  void connect_result(int selected)
  {
    if (selected < 0 || selected >= scan_count)
    {
      set_status("Keyboard: device no longer available");
      return;
    }
    if (stored_index(scan_results[selected].address, scan_results[selected].transport) < 0 &&
        stored.count >= MAX_STORED)
    {
      set_status("Keyboard: 5 paired (limit)");
      return;
    }
    if (connected && device)
    {
      xEventGroupClearBits(events, OPEN_FAILED);
      esp_hidh_dev_close(device);
      xEventGroupWaitBits(events, OPEN_FAILED, pdTRUE, pdTRUE, pdMS_TO_TICKS(2000));
    }
    char message[72];
    snprintf(message, sizeof(message), "Connecting: %.57s", scan_results[selected].name);
    set_status(message);
    xEventGroupClearBits(events, OPEN_OK | OPEN_FAILED);
    esp_hidh_dev_t *opening = esp_hidh_dev_open(scan_results[selected].address,
                                                scan_results[selected].transport,
                                                scan_results[selected].address_type);
    if (!opening)
    {
      set_status("Keyboard: connection failed");
      return;
    }
    EventBits_t result = xEventGroupWaitBits(events, OPEN_OK | OPEN_FAILED, pdTRUE,
                                              pdFALSE, pdMS_TO_TICKS(20000));
    if (!(result & OPEN_OK))
    {
      esp_hidh_dev_close(opening);
      set_status("Keyboard: connection failed");
    }
  }

  void scan_and_connect(bool pairing)
  {
    if (!pairing && stored.count == 0)
    {
      set_status("Keyboard: tap here to pair");
      return;
    }

    scan_count = 0;
    xEventGroupClearBits(events, BLE_PARAMS_READY | BLE_SCAN_DONE | BT_SCAN_DONE |
                                  OPEN_OK | OPEN_FAILED);
    set_status(pairing ? "Keyboard: pairing..." : "Keyboard: reconnecting...", false);

    bool ble_started = false;
    esp_ble_scan_params_t params = {};
    params.scan_type = BLE_SCAN_TYPE_ACTIVE;
    params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    params.scan_interval = 0x50;
    params.scan_window = 0x30;
    params.scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE;
    if (esp_ble_gap_set_scan_params(&params) == ESP_OK)
    {
      xEventGroupWaitBits(events, BLE_PARAMS_READY, pdTRUE, pdTRUE, pdMS_TO_TICKS(1500));
      if (esp_ble_gap_start_scanning(5) == ESP_OK)
        ble_started = true;
    }
    if (!ble_started)
      xEventGroupSetBits(events, BLE_SCAN_DONE);

    bool classic_started = false;
    // ~5 seconds of Classic inquiry (duration unit is 1.28s)
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 4, 0) == ESP_OK)
      classic_started = true;
    if (!classic_started)
      xEventGroupSetBits(events, BT_SCAN_DONE);

    xEventGroupWaitBits(events, BLE_SCAN_DONE | BT_SCAN_DONE, pdTRUE, pdTRUE,
                        pdMS_TO_TICKS(9000));
    esp_ble_gap_stop_scanning();
    esp_bt_gap_cancel_discovery();

    if (pairing)
    {
      std::sort(scan_results, scan_results + scan_count,
                [](const ScanResult &left, const ScanResult &right)
                {
                  if (left.likely_keyboard != right.likely_keyboard)
                    return left.likely_keyboard;
                  if (left.transport != right.transport)
                    return left.transport == ESP_HID_TRANSPORT_BT;
                  return left.rssi > right.rssi;
                });
      set_status(scan_count ? "Keyboard: select a device" :
                              "Keyboard: none found; tap to retry", false);
      notify(DEVICES_READY);
      return;
    }

    const int selected = choose_stored_result();
    if (selected < 0)
    {
      set_status("Keyboard: saved device not found");
      return;
    }
    connect_result(selected);
  }

  static void manager_entry(void *parameter)
  {
    Impl *self = static_cast<Impl *>(parameter);
    if (self->initialize() && self->accepting)
    {
      do
      {
        if (self->pairing_requested)
        {
          self->pairing_requested = false;
          self->scan_and_connect(true);
        }
        else if (self->connection_requested)
        {
          self->connection_requested = false;
          self->connect_result(self->selected_device);
        }
        else if (!self->connected)
          self->scan_and_connect(false);
      } while (self->accepting &&
               (self->pairing_requested || self->connection_requested));
    }
    self->manager_task = nullptr;
    vTaskDelete(nullptr);
  }
};

BluetoothKeyboardHost::Impl *BluetoothKeyboardHost::Impl::instance = nullptr;

BluetoothKeyboardHost::BluetoothKeyboardHost(NotifyCallback notify)
    : impl(new Impl(notify))
{
  Impl::instance = impl;
}

void BluetoothKeyboardHost::start()
{
  impl->accepting = true;
  if (impl->connected)
    impl->notify(STATUS_CHANGED);
  else
    impl->set_status("Keyboard: starting...");
  if (!impl->connected && !impl->manager_task)
    xTaskCreate(Impl::manager_entry, "bt_keyboard", 8192, impl, 2, &impl->manager_task);
}

void BluetoothKeyboardHost::start_pairing()
{
  impl->accepting = true;
  impl->pairing_requested = true;
  impl->set_status("Keyboard: scanning...", false);
  if (!impl->manager_task)
    xTaskCreate(Impl::manager_entry, "bt_keyboard", 8192, impl, 2, &impl->manager_task);
}

std::vector<BluetoothKeyboardHost::DeviceInfo> BluetoothKeyboardHost::devices() const
{
  std::vector<DeviceInfo> result;
  ScanResult snapshot[MAX_SCAN_RESULTS] = {};
  StoredDevices saved = {};
  portENTER_CRITICAL(&impl->scan_lock);
  const int count = impl->scan_count;
  memcpy(snapshot, impl->scan_results, sizeof(ScanResult) * count);
  saved = impl->stored;
  portEXIT_CRITICAL(&impl->scan_lock);
  result.reserve(count);
  for (int i = 0; i < count; ++i)
  {
    DeviceInfo info;
    info.id = i;
    info.name = snapshot[i].name;
    info.rssi = snapshot[i].rssi;
    info.likely_keyboard = snapshot[i].likely_keyboard;
    info.paired = false;
    info.classic = snapshot[i].transport == ESP_HID_TRANSPORT_BT;
    for (int stored_index = 0; stored_index < saved.count; ++stored_index)
      if (saved.devices[stored_index].transport == snapshot[i].transport &&
          same_address(saved.devices[stored_index].address, snapshot[i].address))
        info.paired = true;
    result.push_back(info);
  }
  return result;
}

bool BluetoothKeyboardHost::connect_device(int id)
{
  if (id < 0 || id >= impl->scan_count)
    return false;
  impl->selected_device = id;
  impl->connection_requested = true;
  impl->set_status("Keyboard: connecting...");
  if (!impl->manager_task)
    xTaskCreate(Impl::manager_entry, "bt_keyboard", 8192, impl, 2, &impl->manager_task);
  return true;
}

void BluetoothKeyboardHost::set_accepting_input(bool accepting)
{
  impl->accepting = accepting;
}

bool BluetoothKeyboardHost::drain_text(std::string &text)
{
  char character;
  bool any = false;
  while (xQueueReceive(impl->characters, &character, 0) == pdTRUE)
  {
    text.push_back(character);
    any = true;
  }
  return any;
}

std::string BluetoothKeyboardHost::status() const
{
  return impl->get_status();
}
