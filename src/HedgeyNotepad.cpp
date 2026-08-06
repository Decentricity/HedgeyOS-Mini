#include "HedgeyNotepad.h"

#include <Renderer/Renderer.h>
#include <algorithm>
#include <esp_log.h>
#include <stdio.h>
#include <vector>

// Key order and portrait geometry are adapted from M5Stack's official
// M5Paper_FactoryTest EPDGUI_Keyboard (MIT, copyright M5Stack 2020).
// https://github.com/m5stack/M5Paper_FactoryTest/blob/main/src/epdgui/epdgui_keyboard.cpp
namespace
{
const char *TAG = "NOTEPAD";
const char *NOTE_PATH = "/fs/.hedgeyos-mini-note.txt";
const char *NOTE_TEMP_PATH = "/fs/.hedgeyos-mini-note.tmp";
const size_t MAX_NOTE_LENGTH = 4096;

const char LOWER_KEYS[26] = {
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',
    'z', 'x', 'c', 'v', 'b', 'n', 'm'};
const char UPPER_KEYS[26] = {
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M'};
const char NUMBER_KEYS[26] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '-', '/', ':', ';', '(', ')', '$', '&', '@',
    '_', '"', '.', ',', '?', '!', '\''};
const char SYMBOL_KEYS[26] = {
    '[', ']', '{', '}', '#', '%', '^', '*', '+', '=',
    '_', '\\', '|', '~', '<', '>', '$', '&', '@',
    '`', '"', '.', ',', '?', '!', '\''};

const int KEY_WIDTH = 44;
const int KEY_GAP = 8;
const int FIRST_ROW_X = 6;
const int SECOND_ROW_X = 34;
const int THIRD_ROW_X = 86;

bool contains(int touch_x, int touch_y, int x, int y, int width, int height)
{
  return touch_x >= x && touch_x < x + width &&
         touch_y >= y && touch_y < y + height;
}

void draw_key(Renderer *renderer, int x, int y, int width, int height, const char *label)
{
  renderer->draw_rect(x, y, width, height, 0);
  const int label_x = x + (width - renderer->get_text_width(label)) / 2;
  const int label_y = y + (height - renderer->get_line_height()) / 2 - 2;
  renderer->draw_text(label_x, label_y, label);
}
}

HedgeyNotepad::HedgeyNotepad()
{
  load();
}

void HedgeyNotepad::load()
{
  FILE *file = fopen(NOTE_PATH, "r");
  if (!file)
  {
    return;
  }
  fseek(file, 0, SEEK_END);
  const long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (file_size > 0)
  {
    const size_t length = std::min(static_cast<size_t>(file_size), MAX_NOTE_LENGTH);
    text.resize(length);
    text.resize(fread(&text[0], 1, length, file));
  }
  fclose(file);
}

bool HedgeyNotepad::save()
{
  if (!is_dirty)
  {
    return true;
  }
  FILE *file = fopen(NOTE_TEMP_PATH, "w");
  if (!file)
  {
    ESP_LOGE(TAG, "Could not open note temporary file");
    return false;
  }
  const size_t written = fwrite(text.data(), 1, text.size(), file);
  fclose(file);
  if (written != text.size())
  {
    remove(NOTE_TEMP_PATH);
    ESP_LOGE(TAG, "Could not write complete note");
    return false;
  }
  remove(NOTE_PATH);
  if (rename(NOTE_TEMP_PATH, NOTE_PATH) != 0)
  {
    ESP_LOGE(TAG, "Could not publish saved note");
    return false;
  }
  is_dirty = false;
  return true;
}

char HedgeyNotepad::key_character(int index) const
{
  switch (layout)
  {
  case UPPER_ALPHA:
    return UPPER_KEYS[index];
  case NUMBER:
    return NUMBER_KEYS[index];
  case SYMBOL:
    return SYMBOL_KEYS[index];
  case LOWER_ALPHA:
  default:
    return LOWER_KEYS[index];
  }
}

void HedgeyNotepad::draw_note(Renderer *renderer, int keyboard_y)
{
  const int page_width = renderer->get_page_width();
  const int line_height = renderer->get_line_height();
  const int text_x = 12;
  const int text_y = line_height + 10;
  const int text_width = page_width - text_x * 2;
  const int max_lines = std::max(1, (keyboard_y - text_y - 12) / line_height);

  renderer->draw_text(10, 2, "Note");
  renderer->fill_rect(8, line_height + 5, page_width - 16, 1, 0);

  std::vector<std::string> lines(1);
  std::vector<int> widths(1, 0);
  for (std::string::const_iterator it = text.begin(); it != text.end(); ++it)
  {
    if (*it == '\n')
    {
      lines.push_back("");
      widths.push_back(0);
      continue;
    }
    char glyph[2] = {*it, 0};
    const int glyph_width = renderer->get_text_width(glyph);
    if (!lines.back().empty() && widths.back() + glyph_width > text_width)
    {
      lines.push_back("");
      widths.push_back(0);
    }
    lines.back().push_back(*it);
    widths.back() += glyph_width;
  }

  const int first_line = std::max(0, static_cast<int>(lines.size()) - max_lines);
  for (int visible = 0; visible < max_lines; ++visible)
  {
    const int y = text_y + visible * line_height;
    renderer->fill_rect(8, y + line_height + 2, page_width - 16, 1, 0);
    const int line_index = first_line + visible;
    if (line_index < static_cast<int>(lines.size()))
    {
      renderer->draw_text(text_x, y, lines[line_index].c_str());
    }
  }

  const int last_visible = static_cast<int>(lines.size()) - first_line - 1;
  if (last_visible >= 0 && last_visible < max_lines)
  {
    const int cursor_x = std::min(page_width - 12, text_x + widths.back());
    const int cursor_y = text_y + last_visible * line_height + line_height - 2;
    renderer->fill_rect(cursor_x, cursor_y, 9, 2, 0);
  }
}

void HedgeyNotepad::draw_keyboard(Renderer *renderer, int keyboard_y, int row_step)
{
  const int key_height = row_step - 10;
  renderer->use_selector_font(true);

  int key_index = 0;
  const int row_starts[3] = {FIRST_ROW_X, SECOND_ROW_X, THIRD_ROW_X};
  const int row_lengths[3] = {10, 9, 7};
  for (int row = 0; row < 3; ++row)
  {
    for (int column = 0; column < row_lengths[row]; ++column)
    {
      char label[2] = {key_character(key_index++), 0};
      draw_key(renderer,
               row_starts[row] + column * (KEY_WIDTH + KEY_GAP),
               keyboard_y + row * row_step,
               KEY_WIDTH, key_height, label);
    }
  }

  draw_key(renderer, 458, keyboard_y + 2 * row_step, 60, key_height, "DEL");
  const int bottom_y = keyboard_y + 3 * row_step;
  const char *case_label = (layout == UPPER_ALPHA) ? "abc" :
                           (layout == SYMBOL) ? "123" : "ABC";
  const char *number_label = (layout == NUMBER || layout == SYMBOL) ? "ABC" : "123";
  draw_key(renderer, 6, bottom_y, 60, key_height, case_label);
  draw_key(renderer, 70, bottom_y, 60, key_height, number_label);
  draw_key(renderer, 138, bottom_y, 244, key_height, "Space");
  draw_key(renderer, 390, bottom_y, 128, key_height, "Enter");
}

void HedgeyNotepad::render(Renderer *renderer)
{
  renderer->clear_screen();
  renderer->use_selector_font(true);
  const int page_height = renderer->get_page_height();
  const int keyboard_y = page_height / 2 + 4;
  const int row_step = (page_height - keyboard_y - 26) / 4;
  draw_note(renderer, keyboard_y);
  draw_keyboard(renderer, keyboard_y, row_step);
}

bool HedgeyNotepad::handle_touch(Renderer *renderer, int x, int y)
{
  const int page_height = renderer->get_page_height();
  const int keyboard_y = page_height / 2 + 4;
  const int row_step = (page_height - keyboard_y - 26) / 4;
  const int key_height = row_step - 10;
  if (y < keyboard_y)
  {
    return false;
  }

  int key_index = 0;
  const int row_starts[3] = {FIRST_ROW_X, SECOND_ROW_X, THIRD_ROW_X};
  const int row_lengths[3] = {10, 9, 7};
  for (int row = 0; row < 3; ++row)
  {
    for (int column = 0; column < row_lengths[row]; ++column)
    {
      const int key_x = row_starts[row] + column * (KEY_WIDTH + KEY_GAP);
      if (contains(x, y, key_x, keyboard_y + row * row_step, KEY_WIDTH, key_height))
      {
        if (text.size() < MAX_NOTE_LENGTH)
        {
          text.push_back(key_character(key_index));
          is_dirty = true;
        }
        return true;
      }
      ++key_index;
    }
  }

  if (contains(x, y, 458, keyboard_y + 2 * row_step, 60, key_height))
  {
    if (!text.empty())
    {
      text.erase(text.size() - 1);
      is_dirty = true;
    }
    return true;
  }

  const int bottom_y = keyboard_y + 3 * row_step;
  if (contains(x, y, 6, bottom_y, 60, key_height))
  {
    if (layout == LOWER_ALPHA)
      layout = UPPER_ALPHA;
    else if (layout == UPPER_ALPHA)
      layout = LOWER_ALPHA;
    else if (layout == NUMBER)
      layout = SYMBOL;
    else
      layout = NUMBER;
    return true;
  }
  if (contains(x, y, 70, bottom_y, 60, key_height))
  {
    layout = (layout == NUMBER || layout == SYMBOL) ? LOWER_ALPHA : NUMBER;
    return true;
  }
  if (contains(x, y, 138, bottom_y, 244, key_height))
  {
    if (text.size() < MAX_NOTE_LENGTH)
    {
      text.push_back(' ');
      is_dirty = true;
    }
    return true;
  }
  if (contains(x, y, 390, bottom_y, 128, key_height))
  {
    if (text.size() < MAX_NOTE_LENGTH)
    {
      text.push_back('\n');
      is_dirty = true;
    }
    return true;
  }
  return false;
}
