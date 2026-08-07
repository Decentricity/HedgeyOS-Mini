#include "BluetoothKeyboardHost.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
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
const uint32_t STORE_MAGIC = 0x484B4231;
const int MAX_STORED = 5;
const int MAX_SCAN_RESULTS = 24;
const EventBits_t BLE_PARAMS_READY = 1 << 0;
const EventBits_t BLE_SCAN_DONE = 1 << 1;
const EventBits_t OPEN_OK = 1 << 2;
const EventBits_t OPEN_FAILED = 1 << 3;

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
      notify(false);
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
                       uint8_t address_type, int8_t rssi, const char *name)
  {
    portENTER_CRITICAL(&scan_lock);
    for (int i = 0; i < scan_count; ++i)
    {
      if (scan_results[i].transport == transport && same_address(scan_results[i].address, address))
      {
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
      strncpy(result.name, name ? name : "Bluetooth keyboard", sizeof(result.name) - 1);
    }
    portEXIT_CRITICAL(&scan_lock);
  }

  int choose_result()
  {
    for (int stored_position = 0; stored_position < stored.count; ++stored_position)
      for (int result_position = 0; result_position < scan_count; ++result_position)
        if (stored.devices[stored_position].transport == scan_results[result_position].transport &&
            same_address(stored.devices[stored_position].address, scan_results[result_position].address))
          return result_position;

    if (stored.count >= MAX_STORED)
      return -1;
    int strongest = -1;
    for (int i = 0; i < scan_count; ++i)
      if (strongest < 0 || scan_results[i].rssi > scan_results[strongest].rssi)
        strongest = i;
    return strongest;
  }

  static void input_timer_callback(TimerHandle_t timer)
  {
    Impl *self = static_cast<Impl *>(pvTimerGetTimerID(timer));
    if (self->accepting)
      self->notify(true);
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
        self->set_status("Keyboard: disconnected");
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
        uint16_t uuid = uuid_data && uuid_length >= 2 ? uuid_data[0] | (uuid_data[1] << 8) : 0;
        uint8_t appearance_length = 0;
        uint8_t *appearance_data = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                             ESP_BLE_AD_TYPE_APPEARANCE,
                                                             &appearance_length);
        uint16_t appearance = appearance_data && appearance_length >= 2
                                  ? appearance_data[0] | (appearance_data[1] << 8) : 0;
        if (uuid == ESP_GATT_UUID_HID_SVC || appearance == ESP_HID_APPEARANCE_KEYBOARD)
        {
          uint8_t name_length = 0;
          uint8_t *name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                    ESP_BLE_AD_TYPE_NAME_CMPL, &name_length);
          char name_text[40] = "Bluetooth keyboard";
          if (name && name_length)
          {
            const size_t copy_length = std::min<size_t>(name_length, sizeof(name_text) - 1);
            memcpy(name_text, name, copy_length);
            name_text[copy_length] = 0;
          }
          self->add_scan_result(param->scan_rst.bda, ESP_HID_TRANSPORT_BLE,
                                param->scan_rst.ble_addr_type, param->scan_rst.rssi, name_text);
        }
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
        esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK ||
        esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK)
    {
      set_status("Keyboard: Bluetooth error");
      return false;
    }
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
    esp_ble_gap_set_device_name("HedgeyOS Mini");

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

  void scan_and_connect()
  {
    scan_count = 0;
    xEventGroupClearBits(events, BLE_PARAMS_READY | BLE_SCAN_DONE |
                                  OPEN_OK | OPEN_FAILED);
    set_status("Keyboard: searching...", false);
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
      if (esp_ble_gap_start_scanning(5) != ESP_OK)
        xEventGroupSetBits(events, BLE_SCAN_DONE);
    }
    else
      xEventGroupSetBits(events, BLE_SCAN_DONE);
    xEventGroupWaitBits(events, BLE_SCAN_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(7000));

    const int selected = choose_result();
    if (selected < 0)
    {
      set_status(stored.count >= MAX_STORED ? "Keyboard: 5 paired; none found" :
                                             "Keyboard: none found");
      return;
    }
    char message[72];
    snprintf(message, sizeof(message), "Connecting: %.57s", scan_results[selected].name);
    set_status(message, false);
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
                                              pdFALSE, pdMS_TO_TICKS(12000));
    if (!(result & OPEN_OK))
    {
      esp_hidh_dev_close(opening);
      set_status("Keyboard: connection failed");
    }
  }

  static void manager_entry(void *parameter)
  {
    Impl *self = static_cast<Impl *>(parameter);
    if (self->initialize() && self->accepting && !self->connected)
      self->scan_and_connect();
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
    impl->notify(false);
  else
    impl->set_status("Keyboard: starting...");
  if (!impl->connected && !impl->manager_task)
    xTaskCreate(Impl::manager_entry, "bt_keyboard", 8192, impl, 2, &impl->manager_task);
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
