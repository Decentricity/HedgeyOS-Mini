#include "M5PaperTouchControls.h"

#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>

namespace
{
const char *TAG = "M5P_TOUCH";
const uint16_t GT911_PRODUCT_ID = 0x8140;
const uint16_t GT911_TOUCH_STATUS = 0x814E;
const uint16_t GT911_FIRST_POINT = 0x8150;
}

M5PaperTouchControls::M5PaperTouchControls(ActionCallback_t on_action)
    : on_action(on_action)
{
  if (initialise())
  {
    xTaskCreate(touchTask, "m5paper_touch", 4096, this, 1, nullptr);
  }
}

bool M5PaperTouchControls::initialise()
{
  i2c_config_t config = {};
  config.mode = I2C_MODE_MASTER;
  config.sda_io_num = static_cast<gpio_num_t>(TOUCH_SDA);
  config.sda_pullup_en = GPIO_PULLUP_ENABLE;
  config.scl_io_num = static_cast<gpio_num_t>(TOUCH_SCL);
  config.scl_pullup_en = GPIO_PULLUP_ENABLE;
  config.master.clk_speed = 100000;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 3, 0)
  config.clk_flags = 0;
#endif

  esp_err_t result = i2c_param_config(i2c_port, &config);
  if (result != ESP_OK)
  {
    ESP_LOGE(TAG, "Could not configure touch I2C: %s", esp_err_to_name(result));
    return false;
  }

  result = i2c_driver_install(i2c_port, config.mode,
                              0, 0, 0);
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
  {
    ESP_LOGE(TAG, "Could not start touch I2C: %s", esp_err_to_name(result));
    return false;
  }

  vTaskDelay(pdMS_TO_TICKS(100));

  uint8_t product_id[4] = {};
  const uint8_t candidate_addresses[] = {0x14, 0x5D};
  for (uint8_t address : candidate_addresses)
  {
    if (readRegister(address, GT911_PRODUCT_ID, product_id, sizeof(product_id)) == ESP_OK)
    {
      i2c_address = address;
      ESP_LOGI(TAG, "GT911 ready at 0x%02x (product %.4s)", i2c_address, product_id);
      writeRegister(GT911_TOUCH_STATUS, 0);
      return true;
    }
  }

  ESP_LOGE(TAG, "GT911 touch controller was not found");
  return false;
}

void M5PaperTouchControls::touchTask(void *param)
{
  static_cast<M5PaperTouchControls *>(param)->run();
}

void M5PaperTouchControls::run()
{
  while (true)
  {
    processTouch();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void M5PaperTouchControls::processTouch()
{
  uint8_t status = 0;
  if (readRegister(i2c_address, GT911_TOUCH_STATUS, &status, 1) != ESP_OK ||
      (status & 0x80) == 0)
  {
    return;
  }

  const uint8_t point_count = status & 0x0F;
  if (point_count > 0 && point_count <= 5)
  {
    uint8_t point[8] = {};
    if (readRegister(i2c_address, GT911_FIRST_POINT, point, sizeof(point)) == ESP_OK)
    {
      const uint16_t x = static_cast<uint16_t>(point[0] | (point[1] << 8));
      const uint16_t y = static_cast<uint16_t>(point[2] | (point[3] << 8));
      if (!touching)
      {
        touching = true;
        touch_start_x = x;
        touch_start_y = y;
        touch_start_time = esp_timer_get_time();
      }
      touch_last_x = x;
      touch_last_y = y;
    }
  }
  else if (point_count == 0 && touching)
  {
    finishTap();
    touching = false;
  }

  writeRegister(GT911_TOUCH_STATUS, 0);
}

void M5PaperTouchControls::finishTap()
{
  const int64_t duration_ms = (esp_timer_get_time() - touch_start_time) / 1000;
  const int movement_x = abs(static_cast<int>(touch_last_x) - static_cast<int>(touch_start_x));
  const int movement_y = abs(static_cast<int>(touch_last_y) - static_cast<int>(touch_start_y));

  if (duration_ms <= MAX_TAP_DURATION_MS &&
      movement_x <= MAX_TAP_MOVEMENT &&
      movement_y <= MAX_TAP_MOVEMENT)
  {
    const UIAction action = touch_last_x < SCREEN_WIDTH / 2 ? PAGE_BACK : PAGE_FORWARD;
    ESP_LOGI(TAG, "Tap at %u,%u -> %s", touch_last_x, touch_last_y,
             action == PAGE_FORWARD ? "next page" : "previous page");
    on_action(action);
  }
}

esp_err_t M5PaperTouchControls::readRegister(uint8_t address, uint16_t reg,
                                             uint8_t *data, size_t length)
{
  i2c_cmd_handle_t command = i2c_cmd_link_create();
  i2c_master_start(command);
  i2c_master_write_byte(command, (address << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(command, reg >> 8, true);
  i2c_master_write_byte(command, reg & 0xFF, true);
  i2c_master_start(command);
  i2c_master_write_byte(command, (address << 1) | I2C_MASTER_READ, true);
  if (length > 1)
  {
    i2c_master_read(command, data, length - 1, I2C_MASTER_ACK);
  }
  i2c_master_read_byte(command, data + length - 1, I2C_MASTER_NACK);
  i2c_master_stop(command);
  const esp_err_t result = i2c_master_cmd_begin(i2c_port, command, pdMS_TO_TICKS(50));
  i2c_cmd_link_delete(command);
  return result;
}

esp_err_t M5PaperTouchControls::writeRegister(uint16_t reg, uint8_t value)
{
  i2c_cmd_handle_t command = i2c_cmd_link_create();
  i2c_master_start(command);
  i2c_master_write_byte(command, (i2c_address << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(command, reg >> 8, true);
  i2c_master_write_byte(command, reg & 0xFF, true);
  i2c_master_write_byte(command, value, true);
  i2c_master_stop(command);
  const esp_err_t result = i2c_master_cmd_begin(i2c_port, command, pdMS_TO_TICKS(50));
  i2c_cmd_link_delete(command);
  return result;
}
