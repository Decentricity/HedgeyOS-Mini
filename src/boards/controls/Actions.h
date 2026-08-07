#pragma once

#include <functional>
#include <stdint.h>

typedef enum
{
  NONE,
  UP,
  DOWN,
  SELECT,
  PAGE_BACK,
  PAGE_FORWARD,
  PREVIOUS_CHAPTER,
  NEXT_CHAPTER,
  SHOW_TOC,
  SHOW_BOOKS,
  SHOW_HOME,
  TOUCH_TAP,
  KEYBOARD_INPUT,
  KEYBOARD_STATUS,
  KEYBOARD_DEVICES,
  LAST_INTERACTION
} UIAction;

typedef std::function<void(UIAction)> ActionCallback_t;

typedef struct
{
  UIAction action;
  int16_t x;
  int16_t y;
} UIEvent;

typedef std::function<void(UIEvent)> UIEventCallback_t;
