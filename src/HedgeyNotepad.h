#pragma once

#include <string>

class Renderer;

class HedgeyNotepad
{
public:
  HedgeyNotepad();
  void render(Renderer *renderer);
  bool handle_touch(Renderer *renderer, int x, int y);
  bool save();

private:
  enum KeyboardLayout
  {
    LOWER_ALPHA,
    UPPER_ALPHA,
    NUMBER,
    SYMBOL
  };

  std::string text;
  KeyboardLayout layout = LOWER_ALPHA;
  bool is_dirty = false;

  void load();
  void draw_note(Renderer *renderer, int keyboard_y);
  void draw_keyboard(Renderer *renderer, int keyboard_y, int row_step);
  char key_character(int index) const;
};
