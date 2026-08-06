#pragma once

#include <vector>
#ifndef UNIT_TEST
#include <esp_log.h>
#else
#define vTaskDelay(t)
#define ESP_LOGE(args...)
#define ESP_LOGI(args...)
#define ESP_LOGD(args...)
#endif
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <algorithm>

#include "Epub.h"
#include "EpubCache.h"
#include "Renderer/Renderer.h"
#include "../RubbishHtmlParser/blocks/TextBlock.h"
#include "./State.h"

class Epub;
class Renderer;

class EpubToc
{
private:
  Renderer *renderer;
  EpubListItem &selected_epub;
  EpubTocState &state;
  EpubCache &cache;
  std::vector<CachedChapter> chapters;
  bool loaded = false;
  bool m_needs_redraw = false;

public:
  EpubToc(EpubListItem &selected_epub, EpubTocState &state, Renderer *renderer,
          EpubCache &cache)
      : renderer(renderer), selected_epub(selected_epub), state(state), cache(cache){};
  ~EpubToc() {}
  bool load();
  void next();
  void prev();
  void next_page();
  void prev_page();
  bool select_visible_item_at(int y, int page_height);
  void render();
  void set_needs_redraw() { m_needs_redraw = true; }
  uint16_t get_selected_toc();
  bool is_for(const EpubListItem &item) const;
};
