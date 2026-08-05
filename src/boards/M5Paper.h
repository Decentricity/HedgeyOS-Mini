#pragma once

#include "Board.h"

class M5Paper : public Board
{
public:
  virtual void power_up();
  virtual void prepare_to_sleep();
  virtual Renderer *get_renderer();
  virtual void stop_filesystem();
  virtual ButtonControls *get_button_controls(xQueueHandle ui_queue);
  virtual TouchControls *get_touch_controls(Renderer *renderer, xQueueHandle ui_queue);
};
