#pragma once

#include "TouchControls.h"
#include <driver/i2c.h>
#include <stdint.h>

class M5PaperTouchControls : public TouchControls
{
private:
  enum
  {
    TOUCH_SDA = 21,
    TOUCH_SCL = 22,
    SCREEN_WIDTH = 540,
    MAX_TAP_DURATION_MS = 750,
    MAX_TAP_MOVEMENT = 50
  };

  ActionCallback_t on_action;
  i2c_port_t i2c_port = I2C_NUM_1;
  uint8_t i2c_address = 0;
  bool touching = false;
  uint16_t touch_start_x = 0;
  uint16_t touch_start_y = 0;
  uint16_t touch_last_x = 0;
  uint16_t touch_last_y = 0;
  int64_t touch_start_time = 0;

  static void touchTask(void *param);
  void run();
  bool initialise();
  void processTouch();
  void finishTap();
  esp_err_t readRegister(uint8_t address, uint16_t reg, uint8_t *data, size_t length);
  esp_err_t writeRegister(uint16_t reg, uint8_t value);

public:
  explicit M5PaperTouchControls(ActionCallback_t on_action);
};
