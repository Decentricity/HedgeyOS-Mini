#ifndef UNIT_TEST
#include <esp_log.h>
#else
#define vTaskDelay(t)
#define ESP_LOGE(args...)
#define ESP_LOGI(args...)
#define ESP_LOGD(args...)
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <algorithm>
#include "EpubList.h"
#include "Epub.h"
#include "EpubCache.h"
#include "Renderer/Renderer.h"
#include "../RubbishHtmlParser/blocks/TextBlock.h"
#include "../RubbishHtmlParser/htmlEntities.h"

static const char *TAG = "PUBLIST";

#define PADDING 8
#define EPUB_COLUMNS 2
#define EPUB_ROWS 8
#define EPUBS_PER_PAGE (EPUB_COLUMNS * EPUB_ROWS)
#define MAX_TITLE_LINES 3

void EpubList::next()
{
  state.selected_item = (state.selected_item + 1) % state.num_epubs;
}

void EpubList::prev()
{
  state.selected_item = (state.selected_item - 1 + state.num_epubs) % state.num_epubs;
}

void EpubList::next_page()
{
  if (state.num_epubs == 0)
  {
    return;
  }
  const int page_count = (state.num_epubs + EPUBS_PER_PAGE - 1) / EPUBS_PER_PAGE;
  const int next_page = (state.selected_item / EPUBS_PER_PAGE + 1) % page_count;
  state.selected_item = next_page * EPUBS_PER_PAGE;
}

void EpubList::prev_page()
{
  if (state.num_epubs == 0)
  {
    return;
  }
  const int page_count = (state.num_epubs + EPUBS_PER_PAGE - 1) / EPUBS_PER_PAGE;
  const int previous_page = (state.selected_item / EPUBS_PER_PAGE - 1 + page_count) % page_count;
  state.selected_item = previous_page * EPUBS_PER_PAGE;
}

bool EpubList::select_visible_item_at(int x, int y, int page_width, int page_height)
{
  if (state.num_epubs == 0 || x < 0 || x >= page_width || y < 0 || y >= page_height)
  {
    return false;
  }
  const int column = x * EPUB_COLUMNS / page_width;
  const int row = y * EPUB_ROWS / page_height;
  const int selected_item = (state.selected_item / EPUBS_PER_PAGE) * EPUBS_PER_PAGE +
                            row * EPUB_COLUMNS + column;
  if (selected_item >= state.num_epubs)
  {
    return false;
  }
  state.selected_item = selected_item;
  return true;
}

bool EpubList::load(const char *path)
{
  if (state.is_loaded)
  {
    ESP_LOGI(TAG, "Already loaded books");
    return true;
  }
  // trigger a proper redraw
  state.previous_rendered_page = -1;
  // read in the list of epubs
  state.num_epubs = 0;
  DIR *dir;
  struct dirent *ent;
  bool busy_shown = false;
  cache.begin_scan();
  if ((dir = opendir(path)) != NULL)
  {
    while ((ent = readdir(dir)) != NULL)
    {
      ESP_LOGD(TAG, "Found file: %s", ent->d_name);
      // ignore any hidden files starting with "." and any directories
      if (ent->d_name[0] == '.' || ent->d_type == DT_DIR)
      {
        continue;
      }
      int name_length = strlen(ent->d_name);
      if (name_length < 5 || strcmp(ent->d_name + name_length - 5, ".epub") != 0)
      {
        continue;
      }
      const std::string epub_path = std::string(path) + ent->d_name;
      struct stat file_stat;
      if (stat(epub_path.c_str(), &file_stat) != 0)
      {
        ESP_LOGE(TAG, "Could not stat epub %s", ent->d_name);
        continue;
      }

      const uint64_t size = file_stat.st_size;
      const int64_t mtime = file_stat.st_mtime;
      const CachedBook *cached = cache.find(epub_path, size, mtime);
      std::string title;
      if (cached && !cached->title.empty())
      {
        title = cached->title;
      }
      else
      {
        if (!busy_shown)
        {
          renderer->show_busy();
          busy_shown = true;
        }
        ESP_LOGD(TAG, "Indexing epub title %s", ent->d_name);
        Epub epub(epub_path);
        if (!epub.load_title())
        {
          ESP_LOGE(TAG, "Failed to load epub title %s", ent->d_name);
          continue;
        }
        title = replace_html_entities(epub.get_title());
      }

      cache.store_title(epub_path, size, mtime, title);
      EpubListItem &item = state.epub_list[state.num_epubs];
      strncpy(item.path, epub_path.c_str(), MAX_PATH_SIZE - 1);
      item.path[MAX_PATH_SIZE - 1] = '\0';
      strncpy(item.title, title.c_str(), MAX_TITLE_SIZE - 1);
      item.title[MAX_TITLE_SIZE - 1] = '\0';
      // Reading progress is restored from the validated SD cache only when
      // this book is opened; never trust stale RTC values after a restart.
      item.current_section = 0;
      item.current_page = 0;
      item.pages_in_current_section = 0;
      state.num_epubs++;
      if (state.num_epubs == MAX_EPUB_LIST_SIZE)
      {
        ESP_LOGE(TAG, "Too many epubs, max is %d", MAX_EPUB_LIST_SIZE);
        break;
      }
    }
    closedir(dir);
    cache.finish_scan();
    cache.save();
    std::sort(
        state.epub_list,
        state.epub_list + state.num_epubs,
        [](const EpubListItem &a, const EpubListItem &b)
        {
          return strcmp(a.title, b.title) < 0;
        });
  }
  else
  {
    /* could not open directory */
    perror("");
    ESP_LOGE(TAG, "Could not open directory %s", path);
    return false;
  }
  // sanity check our state
  if (state.selected_item >= state.num_epubs)
  {
    state.selected_item = 0;
    state.previous_rendered_page = -1;
    state.previous_selected_item = -1;
  }
  state.is_loaded = true;
  return true;
}

void EpubList::render()
{
  ESP_LOGD(TAG, "Rendering EPUB list");
  const int current_page = state.selected_item / EPUBS_PER_PAGE;
  const int page_width = renderer->get_page_width();
  const int page_height = renderer->get_page_height();
  const int cell_width = page_width / EPUB_COLUMNS;
  const int cell_height = page_height / EPUB_ROWS;
  const int start_index = current_page * EPUBS_PER_PAGE;
  ESP_LOGI(TAG, "Current page is %d, previous page %d, redraw=%d", current_page, state.previous_rendered_page, m_needs_redraw);

  if (current_page != state.previous_rendered_page || m_needs_redraw)
  {
    m_needs_redraw = false;
    renderer->clear_screen();
    state.previous_selected_item = -1;
    state.previous_rendered_page = -1;
  }

  for (int i = start_index; i < start_index + EPUBS_PER_PAGE && i < state.num_epubs; i++)
  {
    const int position = i - start_index;
    const int column = position % EPUB_COLUMNS;
    const int row = position / EPUB_COLUMNS;
    const int xpos = column * cell_width;
    const int ypos = row * cell_height;

    if (current_page != state.previous_rendered_page)
    {
      ESP_LOGI(TAG, "Rendering item %d", i);
      renderer->draw_rect(xpos + 2, ypos + 2, cell_width - 4, cell_height - 4, 0);

      const int text_xpos = xpos + PADDING;
      const int text_width = cell_width - PADDING * 2;
      TextBlock *title_block = new TextBlock(CENTER_ALIGN);
      title_block->add_span(state.epub_list[i].title, false, false);
      title_block->layout(renderer, nullptr, text_width);

      const int visible_lines = std::min((int)title_block->line_breaks.size(), MAX_TITLE_LINES);
      const int title_height = visible_lines * renderer->get_line_height();
      int text_ypos = ypos + (cell_height - title_height) / 2;
      for (int line = 0; line < visible_lines; line++)
      {
        title_block->render(renderer, line, text_xpos, text_ypos);
        text_ypos += renderer->get_line_height();
      }
      if ((int)title_block->line_breaks.size() > MAX_TITLE_LINES)
      {
        renderer->draw_text(xpos + cell_width - PADDING - renderer->get_text_width("..."),
                            ypos + cell_height - PADDING - renderer->get_line_height(), "...");
      }
      delete title_block;
    }

  }

  state.previous_rendered_page = current_page;
}
