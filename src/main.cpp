#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_sleep.h>
#include "config.h"
#include "BluetoothKeyboardHost.h"
#include "HedgeyNotepad.h"
#include "EpubList/Epub.h"
#include "EpubList/EpubCache.h"
#include "EpubList/EpubList.h"
#include "EpubList/EpubReader.h"
#include "EpubList/EpubToc.h"
#include <hedgehog.h>
#include <hedgeyos_mini.h>
#include <miniz.h>
#include <RubbishHtmlParser/RubbishHtmlParser.h>
#include "boards/Board.h"

#ifdef LOG_ENABLED
// Reference: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/log.html
#define LOG_LEVEL ESP_LOG_INFO
#else
#define LOG_LEVEL ESP_LOG_NONE
#endif
#include <esp_log.h>

extern "C"
{
  void app_main();
}

static const char *TAG = "main";

typedef enum
{
  HOME_SCREEN,
  NOTEPAD_SCREEN,
  SELECTING_EPUB,
  SELECTING_TABLE_CONTENTS,
  READING_EPUB
} UIState;

// HedgeyOS always starts at its home screen after boot or reset.
UIState ui_state = HOME_SCREEN;
// the state data for the epub list and reader
RTC_DATA_ATTR EpubListState epub_list_state;
// the state data for the epub index list
RTC_DATA_ATTR EpubTocState epub_index_state;

void handleEpub(Renderer *renderer, UIAction action);
void handleHome(Renderer *renderer);
void handleNotepad(Renderer *renderer);
void handleEpubList(Renderer *renderer, UIAction action, bool needs_redraw);
void handleEpubTableContents(Renderer *renderer, UIAction action, bool needs_redraw);

static EpubList *epub_list = nullptr;
static EpubReader *reader = nullptr;
static EpubToc *contents = nullptr;
static EpubCache *epub_cache = nullptr;
static HedgeyNotepad *notepad = nullptr;
static BluetoothKeyboardHost *keyboard_host = nullptr;

void save_reading_progress()
{
  if (ui_state != READING_EPUB || !epub_cache ||
      epub_list_state.num_epubs <= 0 ||
      epub_list_state.selected_item < 0 ||
      epub_list_state.selected_item >= epub_list_state.num_epubs)
  {
    return;
  }
  EpubListItem &item = epub_list_state.epub_list[epub_list_state.selected_item];
  epub_cache->store_resume(item.path, item.current_section, item.current_page);
  epub_cache->save();
}

namespace
{
const int TOP_BAR_HEIGHT = 54;
const int BOTTOM_SAFE_MARGIN = 5;
const int TOP_BUTTON_WIDTH = 66;
const int TOP_BUTTON_HEIGHT = 48;
const int TOP_BUTTON_GAP = 18;
const int TOP_BUTTON_Y = 3;
const int CLOSE_BUTTON_X = 4;
const int HOME_TILE_WIDTH = 250;
const int HOME_TILE_HEIGHT = 170;
const int HOME_TILE_GAP = 80;

enum TopBarControl
{
  TOP_BAR_NONE,
  TOP_BAR_CLOSE,
  TOP_BAR_PREVIOUS,
  TOP_BAR_NEXT
};

int previous_page_button_x(int screen_width)
{
  return screen_width / 2 - TOP_BUTTON_GAP / 2 - TOP_BUTTON_WIDTH;
}

int next_page_button_x(int screen_width)
{
  return screen_width / 2 + TOP_BUTTON_GAP / 2;
}

void draw_top_bar_controls(Renderer *renderer)
{
  const int saved_margin_top = renderer->get_margin_top();
  const int saved_margin_left = renderer->get_margin_left();
  const int saved_margin_right = renderer->get_margin_right();
  const int screen_width = renderer->get_page_width() +
                           renderer->get_margin_left() + renderer->get_margin_right();
  const int previous_x = previous_page_button_x(screen_width);
  const int next_x = next_page_button_x(screen_width);
  const int half_width = TOP_BUTTON_WIDTH / 2;
  const int center_y = TOP_BUTTON_Y + TOP_BUTTON_HEIGHT / 2;

  renderer->set_margin_top(0);
  renderer->set_margin_left(0);
  renderer->set_margin_right(0);
  renderer->fill_rect(0, 0, screen_width, TOP_BAR_HEIGHT, 255);
  renderer->fill_rect(0, TOP_BAR_HEIGHT - 1, screen_width, 1, 0);

  // Close/back button: a drawn icon instead of a font-dependent ASCII glyph.
  renderer->draw_rect(CLOSE_BUTTON_X, TOP_BUTTON_Y,
                      TOP_BUTTON_WIDTH, TOP_BUTTON_HEIGHT, 0);
  const int close_center_x = CLOSE_BUTTON_X + half_width;
  renderer->fill_triangle(close_center_x - 13, center_y - 14,
                          close_center_x - 9, center_y - 14,
                          close_center_x + 13, center_y + 14, 0);
  renderer->fill_triangle(close_center_x - 13, center_y - 14,
                          close_center_x + 13, center_y + 14,
                          close_center_x + 9, center_y + 14, 0);
  renderer->fill_triangle(close_center_x + 13, center_y - 14,
                          close_center_x + 9, center_y - 14,
                          close_center_x - 13, center_y + 14, 0);
  renderer->fill_triangle(close_center_x + 13, center_y - 14,
                          close_center_x - 13, center_y + 14,
                          close_center_x - 9, center_y + 14, 0);

  renderer->draw_rect(previous_x, TOP_BUTTON_Y,
                      TOP_BUTTON_WIDTH, TOP_BUTTON_HEIGHT, 0);
  renderer->fill_triangle(previous_x + half_width, center_y - 13,
                          previous_x + half_width - 12, center_y + 12,
                          previous_x + half_width + 12, center_y + 12, 0);
  renderer->draw_rect(next_x, TOP_BUTTON_Y,
                      TOP_BUTTON_WIDTH, TOP_BUTTON_HEIGHT, 0);
  renderer->fill_triangle(next_x + half_width, center_y + 13,
                          next_x + half_width - 12, center_y - 12,
                          next_x + half_width + 12, center_y - 12, 0);
  renderer->set_margin_top(saved_margin_top);
  renderer->set_margin_left(saved_margin_left);
  renderer->set_margin_right(saved_margin_right);
}

TopBarControl top_bar_control_at(const UIEvent &event, int screen_width)
{
  if (event.y < TOP_BUTTON_Y ||
      event.y >= TOP_BUTTON_Y + TOP_BUTTON_HEIGHT)
  {
    return TOP_BAR_NONE;
  }
  if (event.x >= CLOSE_BUTTON_X &&
      event.x < CLOSE_BUTTON_X + TOP_BUTTON_WIDTH)
  {
    return TOP_BAR_CLOSE;
  }
  const int previous_x = previous_page_button_x(screen_width);
  const int next_x = next_page_button_x(screen_width);
  if (event.x >= previous_x && event.x < previous_x + TOP_BUTTON_WIDTH)
  {
    return TOP_BAR_PREVIOUS;
  }
  if (event.x >= next_x && event.x < next_x + TOP_BUTTON_WIDTH)
  {
    return TOP_BAR_NEXT;
  }
  return TOP_BAR_NONE;
}

int home_tiles_x(int page_width)
{
  return (page_width - HOME_TILE_WIDTH * 2 - HOME_TILE_GAP) / 2;
}

int home_tiles_y(int page_height)
{
  return page_height - HOME_TILE_HEIGHT - 22;
}

void draw_home_title(Renderer *renderer, int page_width)
{
  static uint8_t title_pixels[hedgeyos_mini_raw_size];
  static uint8_t hedgehog_pixels[hedgehog_raw_size];
  static bool title_is_unpacked = false;
  static bool hedgehog_is_unpacked = false;
  if (!title_is_unpacked)
  {
    title_is_unpacked = tinfl_decompress_mem_to_mem(
                            title_pixels, sizeof(title_pixels),
                            hedgeyos_mini_data, sizeof(hedgeyos_mini_data), 0) !=
                        TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
  }
  if (!hedgehog_is_unpacked)
  {
    hedgehog_is_unpacked = tinfl_decompress_mem_to_mem(
                               hedgehog_pixels, sizeof(hedgehog_pixels),
                               hedgehog_data, sizeof(hedgehog_data), 0) !=
                           TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
  }
  if (!title_is_unpacked || !hedgehog_is_unpacked)
  {
    ESP_LOGE(TAG, "Could not unpack the home artwork");
    return;
  }

  const int start_x = (page_width - hedgeyos_mini_width) / 2;
  const int start_y = 28;
  for (int y = 0; y < hedgeyos_mini_height; ++y)
  {
    for (int x = 0; x < hedgeyos_mini_width; ++x)
    {
      if (title_pixels[y * hedgeyos_mini_stride + x / 8] & (1 << (x % 8)))
      {
        renderer->draw_pixel(start_x + x, start_y + y, 0);
      }
    }
  }

  const int hedgehog_x = (page_width - hedgehog_width) / 2;
  const int hedgehog_y = start_y + hedgeyos_mini_height + 16;
  for (int y = 0; y < hedgehog_height; ++y)
  {
    for (int x = 0; x < hedgehog_width; ++x)
    {
      const int pixel_index = y * hedgehog_width + x;
      const uint8_t packed = hedgehog_pixels[pixel_index / 2];
      const uint8_t gray = ((pixel_index % 2 == 0) ? packed >> 4 : packed & 0x0F) * 17;
      if (gray < 255)
      {
        renderer->draw_pixel(hedgehog_x + x, hedgehog_y + y, gray);
      }
    }
  }
}
}

void handleHome(Renderer *renderer)
{
  renderer->use_selector_font(false);
  renderer->clear_screen();
  const int page_width = renderer->get_page_width();
  const int page_height = renderer->get_page_height();
  const int tiles_x = home_tiles_x(page_width);
  const int tiles_y = home_tiles_y(page_height);

  draw_home_title(renderer, page_width);

  const int read_center_x = tiles_x + HOME_TILE_WIDTH / 2;
  const int write_center_x = tiles_x + HOME_TILE_WIDTH + HOME_TILE_GAP + HOME_TILE_WIDTH / 2;
  const int icon_y = tiles_y + 14;
  const int margin_left = renderer->get_margin_left();
  const int margin_top = renderer->get_margin_top();

  // Open-book icon.
  renderer->draw_rect(read_center_x - 56, icon_y, 52, 78, 0);
  renderer->draw_rect(read_center_x + 4, icon_y, 52, 78, 0);
  renderer->fill_triangle(read_center_x + margin_left, icon_y + 8 + margin_top,
                          read_center_x - 4 + margin_left, icon_y + margin_top,
                          read_center_x - 4 + margin_left, icon_y + 78 + margin_top, 0);
  renderer->fill_triangle(read_center_x + margin_left, icon_y + 8 + margin_top,
                          read_center_x + 4 + margin_left, icon_y + margin_top,
                          read_center_x + 4 + margin_left, icon_y + 78 + margin_top, 0);
  renderer->draw_text(read_center_x - renderer->get_text_width("Read") / 2,
                      icon_y + 90, "Read");

  // Notepad and pen icon.
  renderer->draw_rect(write_center_x - 48, icon_y, 82, 78, 0);
  renderer->fill_rect(write_center_x - 34, icon_y - 5, 10, 10, 0);
  renderer->fill_rect(write_center_x - 4, icon_y - 5, 10, 10, 0);
  renderer->fill_rect(write_center_x + 26, icon_y - 5, 10, 10, 0);
  renderer->fill_triangle(write_center_x + 4 + margin_left, icon_y + 64 + margin_top,
                          write_center_x + 42 + margin_left, icon_y + 16 + margin_top,
                          write_center_x + 48 + margin_left, icon_y + 22 + margin_top, 0);
  renderer->fill_triangle(write_center_x + 4 + margin_left, icon_y + 64 + margin_top,
                          write_center_x + 48 + margin_left, icon_y + 22 + margin_top,
                          write_center_x + 10 + margin_left, icon_y + 70 + margin_top, 0);
  renderer->draw_text(write_center_x - renderer->get_text_width("Write") / 2,
                      icon_y + 90, "Write");

  draw_top_bar_controls(renderer);
}

void handleNotepad(Renderer *renderer)
{
  if (!notepad)
  {
    notepad = new HedgeyNotepad();
  }
  if (keyboard_host)
  {
    notepad->set_keyboard_status(keyboard_host->status());
  }
  notepad->render(renderer);
  draw_top_bar_controls(renderer);
}

void handleEpub(Renderer *renderer, UIAction action)
{
  renderer->use_selector_font(false);
  if (!reader)
  {
    reader = new EpubReader(epub_list_state.epub_list[epub_list_state.selected_item], renderer);
    reader->load();
  }
  switch (action)
  {
  case UP:
  case PAGE_BACK:
    reader->prev();
    break;
  case DOWN:
  case PAGE_FORWARD:
    reader->next();
    break;
  case PREVIOUS_CHAPTER:
    renderer->show_busy();
    reader->previous_chapter();
    break;
  case NEXT_CHAPTER:
    renderer->show_busy();
    reader->next_chapter();
    break;
  case SHOW_TOC:
    renderer->show_busy();
    ui_state = SELECTING_TABLE_CONTENTS;
    renderer->clear_screen();
    delete reader;
    reader = nullptr;
    if (!contents || !contents->is_for(epub_list_state.epub_list[epub_list_state.selected_item]))
    {
      delete contents;
      contents = new EpubToc(epub_list_state.epub_list[epub_list_state.selected_item],
                             epub_index_state, renderer, *epub_cache);
    }
    contents->load();
    contents->set_needs_redraw();
    handleEpubTableContents(renderer, NONE, true);
    return;
  case SELECT:
    // switch back to main screen
    renderer->show_busy();
    ui_state = SELECTING_EPUB;
    renderer->clear_screen();
    // clear the epub reader away
    delete reader;
    reader = nullptr;
    // force a redraw
    if (!epub_list)
    {
      epub_list = new EpubList(renderer, epub_list_state, *epub_cache);
    }
    handleEpubList(renderer, NONE, true);
    return;
  case NONE:
  default:
    break;
  }
  reader->render();
  draw_top_bar_controls(renderer);
}

void handleEpubTableContents(Renderer *renderer, UIAction action, bool needs_redraw)
{
  renderer->use_selector_font(true);
  if (!contents)
  {
    contents = new EpubToc(epub_list_state.epub_list[epub_list_state.selected_item],
                           epub_index_state, renderer, *epub_cache);
    contents->set_needs_redraw();
    contents->load();
  }
  switch (action)
  {
  case SHOW_BOOKS:
    renderer->show_busy();
    ui_state = SELECTING_EPUB;
    renderer->clear_screen();
    handleEpubList(renderer, NONE, true);
    return;
  case UP:
    contents->prev_page();
    break;
  case DOWN:
    contents->next_page();
    break;
  case SELECT:
    renderer->show_busy();
    // setup the reader state
    ui_state = READING_EPUB;
    // create the reader and load the book
    reader = new EpubReader(epub_list_state.epub_list[epub_list_state.selected_item], renderer);
    reader->set_state_section(contents->get_selected_toc());
    reader->load();
    // switch to reading the epub; retain its chapter list in RAM so returning
    // from reading mode does not reopen and parse the EPUB.
    handleEpub(renderer, NONE);
    return;
  case NONE:
  default:
    break;
  }
  contents->render();
  draw_top_bar_controls(renderer);
}

void handleEpubList(Renderer *renderer, UIAction action, bool needs_redraw)
{
  renderer->use_selector_font(true);
  // load up the epub list from the filesystem
  if (!epub_list)
  {
    ESP_LOGI("main", "Creating epub list");
    epub_list = new EpubList(renderer, epub_list_state, *epub_cache);
    if (epub_list->load("/fs/"))
    {
      ESP_LOGI("main", "Epub files loaded");
    }
  }
  if (needs_redraw)
  {
    epub_list->set_needs_redraw();
  }
  // work out what the user wants us to do
  switch (action)
  {
  case SHOW_HOME:
    renderer->show_busy();
    ui_state = HOME_SCREEN;
    handleHome(renderer);
    return;
  case UP:
    epub_list->prev_page();
    break;
  case DOWN:
    epub_list->next_page();
    break;
  case SELECT:
  {
    renderer->show_busy();
    EpubListItem &item = epub_list_state.epub_list[epub_list_state.selected_item];
    uint16_t resume_section = 0;
    uint16_t resume_page = 0;
    if (epub_cache->get_resume(item.path, resume_section, resume_page))
    {
      item.current_section = resume_section;
      item.current_page = resume_page;
      item.pages_in_current_section = 0;
      ui_state = READING_EPUB;
      renderer->clear_screen();
      delete reader;
      reader = nullptr;
      handleEpub(renderer, NONE);
      return;
    }

    // A new book opens at its chapter selector. Books with saved progress
    // resume directly above.
    ui_state = SELECTING_TABLE_CONTENTS;
    // create the reader and load the book
    if (!contents || !contents->is_for(epub_list_state.epub_list[epub_list_state.selected_item]))
    {
      delete contents;
      epub_index_state.selected_item = 0;
      epub_index_state.previous_rendered_page = -1;
      epub_index_state.previous_selected_item = -1;
      contents = new EpubToc(epub_list_state.epub_list[epub_list_state.selected_item],
                             epub_index_state, renderer, *epub_cache);
    }
    contents->load();
    contents->set_needs_redraw();
    handleEpubTableContents(renderer, NONE, true);
    return;
  }
  case NONE:
  default:
    // nothing to do
    break;
  }
  epub_list->render();
  draw_top_bar_controls(renderer);
}

void handleTouchInteraction(Renderer *renderer, const UIEvent &event)
{
  const int screen_width = renderer->get_page_width() +
                           renderer->get_margin_left() + renderer->get_margin_right();
  const TopBarControl top_bar_control = top_bar_control_at(event, screen_width);

  if (top_bar_control == TOP_BAR_CLOSE)
  {
    if (ui_state == NOTEPAD_SCREEN)
    {
      ESP_LOGI("TOUCH", "Notepad close tap -> home");
      if (keyboard_host)
        keyboard_host->set_accepting_input(false);
      notepad->save();
      renderer->show_busy();
      ui_state = HOME_SCREEN;
      handleHome(renderer);
    }
    else if (ui_state == SELECTING_EPUB)
    {
      ESP_LOGI("TOUCH", "Book close tap -> home");
      handleEpubList(renderer, SHOW_HOME, false);
    }
    else if (ui_state == READING_EPUB)
    {
      ESP_LOGI("TOUCH", "Reading close tap -> chapter list");
      handleEpub(renderer, SHOW_TOC);
    }
    else if (ui_state == SELECTING_TABLE_CONTENTS)
    {
      ESP_LOGI("TOUCH", "Chapter close tap -> book list");
      handleEpubTableContents(renderer, SHOW_BOOKS, false);
    }
    return;
  }

  if (top_bar_control == TOP_BAR_PREVIOUS ||
      top_bar_control == TOP_BAR_NEXT)
  {
    const bool previous = top_bar_control == TOP_BAR_PREVIOUS;
    ESP_LOGI("TOUCH", "Top bar arrow -> %s", previous ? "previous" : "next");
    if (ui_state == READING_EPUB)
    {
      handleEpub(renderer, previous ? PREVIOUS_CHAPTER : NEXT_CHAPTER);
    }
    else if (ui_state == SELECTING_TABLE_CONTENTS)
    {
      handleEpubTableContents(renderer, previous ? UP : DOWN, false);
    }
    else if (ui_state == SELECTING_EPUB)
    {
      handleEpubList(renderer, previous ? UP : DOWN, false);
    }
    return;
  }

  // The bar is not part of the page-turning surface.
  if (event.y < TOP_BAR_HEIGHT)
  {
    return;
  }

  if (ui_state == READING_EPUB)
  {
    if (event.x < screen_width / 2)
    {
      ESP_LOGI("TOUCH", "Left tap -> previous page");
      handleEpub(renderer, PAGE_BACK);
    }
    else
    {
      ESP_LOGI("TOUCH", "Right tap -> next page");
      handleEpub(renderer, PAGE_FORWARD);
    }
    return;
  }

  const int content_x = event.x - renderer->get_margin_left();
  const int content_y = event.y - renderer->get_margin_top();
  const int page_width = renderer->get_page_width();
  const int page_height = renderer->get_page_height();

  if (ui_state == HOME_SCREEN)
  {
    const int tiles_x = home_tiles_x(page_width);
    const int tiles_y = home_tiles_y(page_height);
    if (content_x >= tiles_x && content_x < tiles_x + HOME_TILE_WIDTH &&
        content_y >= tiles_y && content_y < tiles_y + HOME_TILE_HEIGHT)
    {
      ESP_LOGI("TOUCH", "Read tile tap -> book list");
      renderer->show_busy();
      ui_state = SELECTING_EPUB;
      renderer->clear_screen();
      handleEpubList(renderer, NONE, true);
    }
    else
    {
      const int write_x = tiles_x + HOME_TILE_WIDTH + HOME_TILE_GAP;
      if (content_x >= write_x && content_x < write_x + HOME_TILE_WIDTH &&
          content_y >= tiles_y && content_y < tiles_y + HOME_TILE_HEIGHT)
      {
        ESP_LOGI("TOUCH", "Write tile tap -> notepad");
        renderer->show_busy();
        ui_state = NOTEPAD_SCREEN;
        if (keyboard_host)
          keyboard_host->start();
        handleNotepad(renderer);
      }
    }
    return;
  }

  if (ui_state == NOTEPAD_SCREEN)
  {
    if (content_x >= 0 && content_x < page_width &&
        content_y >= 0 && content_y < page_height &&
        notepad && notepad->handle_touch(renderer, content_x, content_y))
    {
      handleNotepad(renderer);
    }
    return;
  }

  if (content_x < 0 || content_x >= page_width ||
      content_y < 0 || content_y >= page_height)
  {
    return;
  }

  if (ui_state == SELECTING_TABLE_CONTENTS && contents &&
      contents->select_visible_item_at(content_y, page_height))
  {
    ESP_LOGI("TOUCH", "Selected chapter at y=%d", event.y);
    handleEpubTableContents(renderer, SELECT, false);
  }
  else if (ui_state == SELECTING_EPUB && epub_list &&
           epub_list->select_visible_item_at(content_x, content_y, page_width, page_height))
  {
    ESP_LOGI("TOUCH", "Selected book at y=%d", event.y);
    handleEpubList(renderer, SELECT, false);
  }
}

void handleUserInteraction(Renderer *renderer, const UIEvent &ui_event, bool needs_redraw)
{
  if (ui_event.action == KEYBOARD_INPUT)
  {
    if (ui_state == NOTEPAD_SCREEN && keyboard_host && notepad)
    {
      std::string input;
      if (keyboard_host->drain_text(input) && notepad->insert_text(input))
        handleNotepad(renderer);
    }
    return;
  }

  if (ui_event.action == KEYBOARD_STATUS)
  {
    if (ui_state == NOTEPAD_SCREEN)
      handleNotepad(renderer);
    return;
  }

  if (ui_event.action == TOUCH_TAP)
  {
    handleTouchInteraction(renderer, ui_event);
    return;
  }

  switch (ui_state)
  {
  case HOME_SCREEN:
    handleHome(renderer);
    break;
  case NOTEPAD_SCREEN:
    handleNotepad(renderer);
    break;
  case READING_EPUB:
    handleEpub(renderer, ui_event.action);
    break;
  case SELECTING_TABLE_CONTENTS:
    handleEpubTableContents(renderer, ui_event.action, needs_redraw);
    break;
  case SELECTING_EPUB:
    handleEpubList(renderer, ui_event.action, needs_redraw);
    break;
  default:
    ui_state = HOME_SCREEN;
    handleHome(renderer);
    break;
  }
}

// TODO - add the battery level
void draw_battery_level(Renderer *renderer, float voltage, float percentage)
{
  const int saved_margin_top = renderer->get_margin_top();
  const int saved_margin_left = renderer->get_margin_left();
  const int saved_margin_right = renderer->get_margin_right();
  const int screen_width = renderer->get_page_width() +
                           renderer->get_margin_left() + renderer->get_margin_right();
  renderer->set_margin_top(0);
  renderer->set_margin_left(0);
  renderer->set_margin_right(0);
  int width = 40;
  int height = 20;
  int xpos = screen_width - width - 6;
  int ypos = (TOP_BAR_HEIGHT - height) / 2;
  int percent_width = width * percentage / 100;
  renderer->fill_rect(xpos, ypos, width, height, 255);
  renderer->fill_rect(xpos + width - percent_width, ypos, percent_width, height, 0);
  renderer->draw_rect(xpos, ypos, width, height, 0);
  renderer->fill_rect(xpos - 4, ypos + height / 4, 4, height / 2, 0);
  renderer->set_margin_top(saved_margin_top);
  renderer->set_margin_left(saved_margin_left);
  renderer->set_margin_right(saved_margin_right);
}

void main_task(void *param)
{
  // start the board up
  ESP_LOGI("main", "Powering up the board");
  Board *board = Board::factory();
  board->power_up();
  // create the renderer for the board
  ESP_LOGI("main", "Creating renderer");
  Renderer *renderer = board->get_renderer();
  // bring the file system up - SPIFFS or SDCard depending on the defines in platformio.ini
  ESP_LOGI("main", "Starting file system");
  board->start_filesystem();
  epub_cache = new EpubCache("/fs/.atomic14-epub-cache-v1.bin");
  if (epub_cache->load())
  {
    // If load recovered a validated temporary file after an interrupted
    // rename, promote it back to the canonical cache path now.
    epub_cache->save();
  }
  // The framebuffer and C++ UI objects do not survive every kind of reset,
  // even when this RTC flag does. Rebuild the lightweight list from the SD
  // cache and force its first page to redraw on every boot.
  epub_list_state.is_loaded = false;
  epub_list_state.previous_rendered_page = -1;
  epub_list_state.previous_selected_item = -1;

  // battery details
  ESP_LOGI("main", "Starting battery monitor");
  Battery *battery = board->get_battery();
  if (battery)
  {
    battery->setup();
  }

  // Reserve a consistent control bar and a few physical pixels at the bottom.
  renderer->set_margin_top(TOP_BAR_HEIGHT);
  renderer->set_margin_bottom(BOTTOM_SAFE_MARGIN);
  // page margins
  renderer->set_margin_left(10);
  renderer->set_margin_right(10);

  // create a message queue for UI events
  xQueueHandle ui_queue = xQueueCreate(20, sizeof(UIEvent));
  keyboard_host = new BluetoothKeyboardHost(
      [ui_queue](bool has_input)
      {
        const UIEvent event = {has_input ? KEYBOARD_INPUT : KEYBOARD_STATUS, 0, 0};
        xQueueSend(ui_queue, &event, 0);
      });

  // set the controls up
  ESP_LOGI("main", "Setting up controls");
  ButtonControls *button_controls = board->get_button_controls(ui_queue);
  TouchControls *touch_controls = board->get_touch_controls(renderer, ui_queue);

  ESP_LOGI("main", "Controls configured");
  // work out if we were woken from deep sleep
  if (button_controls->did_wake_from_deep_sleep())
  {
    // restore the renderer state - it should have been saved when we went to sleep...
    bool hydrate_success = renderer->hydrate();
    const UIEvent ui_event = {button_controls->get_deep_sleep_action(), 0, 0};
    handleUserInteraction(renderer, ui_event, !hydrate_success);
  }
  else
  {
    // reset the screen
    renderer->reset();
    // make sure the UI is in the right state
    const UIEvent ui_event = {NONE, 0, 0};
    handleUserInteraction(renderer, ui_event, true);
  }

  // draw the battery level before flushing the screen
  if (battery)
  {
    draw_battery_level(renderer, battery->get_voltage(), battery->get_percentage());
  }
  touch_controls->render(renderer);
  renderer->flush_display();

  // keep track of when the user last interacted and go to sleep after N seconds
  int64_t last_user_interaction = esp_timer_get_time();
  while (esp_timer_get_time() - last_user_interaction < 120 * 1000 * 1000)
  {
    UIEvent ui_event = {NONE, 0, 0};
    // wait for something to happen for 60 seconds
    if (xQueueReceive(ui_queue, &ui_event, pdMS_TO_TICKS(60000)) == pdTRUE)
    {
      if (ui_event.action != NONE)
      {
        // something happened!
        if (ui_event.action != KEYBOARD_STATUS)
          last_user_interaction = esp_timer_get_time();
        // show feedback on the touch controls
        touch_controls->renderPressedState(renderer, ui_event.action);
        handleUserInteraction(renderer, ui_event, false);

        // make sure to clear the feedback on the touch controls
        touch_controls->render(renderer);
      }
    }
    // update the battery level - do this even if there is no interaction so we
    // show the battery level even if the user is idle
    if (battery)
    {
      ESP_LOGI("main", "Battery Level %f, percent %d", battery->get_voltage(), battery->get_percentage());
      draw_battery_level(renderer, battery->get_voltage(), battery->get_percentage());
    }
    renderer->flush_display();
    // Persist only after the requested page has actually reached the panel.
    save_reading_progress();
  }
  ESP_LOGI("main", "Saving state");
  if (notepad)
  {
    notepad->save();
  }
  if (keyboard_host)
  {
    keyboard_host->set_accepting_input(false);
  }
  // save the state of the renderer
  renderer->dehydrate();
  // turn off the filesystem
  board->stop_filesystem();
  // get ready to go to sleep
  board->prepare_to_sleep();
  ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());
  ESP_LOGI("main", "Entering deep sleep");
  // configure deep sleep options
  button_controls->setup_deep_sleep();
  vTaskDelay(pdMS_TO_TICKS(500));
  // go to sleep
  esp_deep_sleep_start();
}

void app_main()
{
  // Logging control
  esp_log_level_set("main", LOG_LEVEL);
  esp_log_level_set("EPUB", LOG_LEVEL);
  esp_log_level_set("PUBLIST", LOG_LEVEL);
  esp_log_level_set("ZIP", LOG_LEVEL);
  esp_log_level_set("JPG", LOG_LEVEL);
  esp_log_level_set("TOUCH", LOG_LEVEL);

  // dump out the epub list state
  ESP_LOGI("main", "epub list state num_epubs=%d", epub_list_state.num_epubs);
  ESP_LOGI("main", "epub list state is_loaded=%d", epub_list_state.is_loaded);
  ESP_LOGI("main", "epub list state selected_item=%d", epub_list_state.selected_item);

  ESP_LOGI("main", "Memory before main task start %d", esp_get_free_heap_size());
  xTaskCreatePinnedToCore(main_task, "main_task", 32768, NULL, 1, NULL, 1);
}
