#pragma once

#include <functional>

typedef enum
{
  NONE,
  UP,
  DOWN,
  SELECT,
  PAGE_BACK,
  PAGE_FORWARD,
  LAST_INTERACTION
} UIAction;

typedef std::function<void(UIAction)> ActionCallback_t;
