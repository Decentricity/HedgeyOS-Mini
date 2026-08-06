#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_sleep.h>
#include "config.h"
#include "EpubList/Epub.h"
#include "EpubList/EpubCache.h"
#include "EpubList/EpubList.h"
#include "EpubList/EpubReader.h"
#include "EpubList/EpubToc.h"
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

const char *TAG = "main";

typedef enum
{
  SELECTING_EPUB,
  SELECTING_TABLE_CONTENTS,
  READING_EPUB
} UIState;

// UI objects and chapter data live only in normal RAM, so the view must also
// start fresh on every boot instead of surviving reset in RTC memory.
UIState ui_state = SELECTING_EPUB;
// the state data for the epub list and reader
RTC_DATA_ATTR EpubListState epub_list_state;
// the state data for the epub index list
RTC_DATA_ATTR EpubTocState epub_index_state;

void handleEpub(Renderer *renderer, UIAction action);
void handleEpubList(Renderer *renderer, UIAction action, bool needs_redraw);
void handleEpubTableContents(Renderer *renderer, UIAction action, bool needs_redraw);

static EpubList *epub_list = nullptr;
static EpubReader *reader = nullptr;
static EpubToc *contents = nullptr;
static EpubCache *epub_cache = nullptr;

void handleEpub(Renderer *renderer, UIAction action)
{
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
  case SHOW_TOC:
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
}

void handleEpubTableContents(Renderer *renderer, UIAction action, bool needs_redraw)
{
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
}

void handleEpubList(Renderer *renderer, UIAction action, bool needs_redraw)
{
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
  case UP:
    epub_list->prev_page();
    break;
  case DOWN:
    epub_list->next_page();
    break;
  case SELECT:
    // switch to reading the epub
    // setup the reader state
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
  case NONE:
  default:
    // nothing to do
    break;
  }
  epub_list->render();
}

void handleTouchInteraction(Renderer *renderer, const UIEvent &event)
{
  const int screen_width = renderer->get_page_width() +
                           renderer->get_margin_left() + renderer->get_margin_right();
  const int screen_height = renderer->get_page_height() +
                            renderer->get_margin_top() + renderer->get_margin_bottom();

  if (ui_state == READING_EPUB)
  {
    const bool top_center = event.y < screen_height / 5 &&
                            event.x >= screen_width / 3 &&
                            event.x < screen_width * 2 / 3;
    if (top_center)
    {
      ESP_LOGI("TOUCH", "Top-center tap -> chapter list");
      handleEpub(renderer, SHOW_TOC);
    }
    else if (event.x < screen_width / 2)
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

  if (ui_state == SELECTING_TABLE_CONTENTS && event.x < 60 && event.y < 60)
  {
    ESP_LOGI("TOUCH", "Chapter close tap -> book list");
    handleEpubTableContents(renderer, SHOW_BOOKS, false);
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
  if (ui_event.action == TOUCH_TAP)
  {
    handleTouchInteraction(renderer, ui_event);
    return;
  }

  switch (ui_state)
  {
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
    // RTC_NOINIT_ATTR can contain an undefined value after a cold boot. The
    // legacy UI drew the book list through this default branch without fixing
    // the state, which left coordinate-based touch dispatch with no valid UI.
    ui_state = SELECTING_EPUB;
    handleEpubList(renderer, ui_event.action, needs_redraw);
    break;
  }
}

// TODO - add the battery level
void draw_battery_level(Renderer *renderer, float voltage, float percentage)
{
  // clear the margin so we can draw the battery in the right place
  renderer->set_margin_top(0);
  int width = 40;
  int height = 20;
  int margin_right = 5;
  int margin_top = 10;
  int xpos = renderer->get_page_width() - width - margin_right;
  int ypos = margin_top;
  int percent_width = width * percentage / 100;
  renderer->fill_rect(xpos, ypos, width, height, 255);
  renderer->fill_rect(xpos + width - percent_width, ypos, percent_width, height, 0);
  renderer->draw_rect(xpos, ypos, width, height, 0);
  renderer->fill_rect(xpos - 4, ypos + height / 4, 4, height / 2, 0);
  // put the margin back
  renderer->set_margin_top(35);
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
  else
  {
    // The RTC book list may have survived a reset from an older firmware. If
    // the persistent index is absent or invalid, rebuild it once from the SD.
    epub_list_state.is_loaded = false;
  }

  // battery details
  ESP_LOGI("main", "Starting battery monitor");
  Battery *battery = board->get_battery();
  if (battery)
  {
    battery->setup();
  }

  // make space for the battery display
  renderer->set_margin_top(35);
  // page margins
  renderer->set_margin_left(10);
  renderer->set_margin_right(10);

  // create a message queue for UI events
  xQueueHandle ui_queue = xQueueCreate(10, sizeof(UIEvent));

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
  }
  ESP_LOGI("main", "Saving state");
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
