#pragma once

#include <vector>

#include "./State.h"

class Epub;
class EpubCache;
class Renderer;

class EpubList
{
private:
  Renderer *renderer;
  EpubListState &state;
  EpubCache &cache;
  bool m_needs_redraw = false;

public:
  EpubList(Renderer *renderer, EpubListState &state, EpubCache &cache)
      : renderer(renderer), state(state), cache(cache){};
  ~EpubList() {}
  bool load(const char *path);
  void set_needs_redraw() { m_needs_redraw = true; }
  void next();
  void prev();
  void next_page();
  void prev_page();
  bool select_visible_item_at(int x, int y, int page_width, int page_height);
  void show_touch_feedback();
  void render();
};
