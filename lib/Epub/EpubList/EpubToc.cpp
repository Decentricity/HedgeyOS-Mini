#include "EpubToc.h"
#include "../RubbishHtmlParser/htmlEntities.h"

#include <sys/stat.h>

static const char *TAG = "PUBINDEX";
#define PADDING 14
#define ITEMS_PER_PAGE 6

void EpubToc::next()
{
  if (!loaded)
  {
    load();
  }
  if (!chapters.empty())
  {
    state.selected_item = (state.selected_item + 1) % chapters.size();
  }
}

void EpubToc::prev()
{
  if (!loaded)
  {
    load();
  }
  if (!chapters.empty())
  {
    const int item_count = chapters.size();
    state.selected_item = (state.selected_item - 1 + item_count) % item_count;
  }
}

void EpubToc::next_page()
{
  if (!loaded)
  {
    load();
  }
  const int item_count = chapters.size();
  if (item_count == 0)
  {
    return;
  }
  const int page_count = (item_count + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
  const int next_page = (state.selected_item / ITEMS_PER_PAGE + 1) % page_count;
  state.selected_item = next_page * ITEMS_PER_PAGE;
}

void EpubToc::prev_page()
{
  if (!loaded)
  {
    load();
  }
  const int item_count = chapters.size();
  if (item_count == 0)
  {
    return;
  }
  const int page_count = (item_count + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
  const int previous_page = (state.selected_item / ITEMS_PER_PAGE - 1 + page_count) % page_count;
  state.selected_item = previous_page * ITEMS_PER_PAGE;
}

bool EpubToc::select_visible_item_at(int y, int page_height)
{
  if (!loaded)
  {
    load();
  }
  const int item_count = chapters.size();
  if (item_count == 0 || y < 0 || y >= page_height)
  {
    return false;
  }
  const int row = y * ITEMS_PER_PAGE / page_height;
  const int selected_item = (state.selected_item / ITEMS_PER_PAGE) * ITEMS_PER_PAGE + row;
  if (selected_item >= item_count)
  {
    return false;
  }
  state.selected_item = selected_item;
  return true;
}

bool EpubToc::load()
{
  ESP_LOGI(TAG, "load");
  if (loaded)
  {
    return true;
  }

  struct stat file_stat;
  if (stat(selected_epub.path, &file_stat) != 0)
  {
    ESP_LOGE(TAG, "Could not stat selected epub");
    return false;
  }
  const uint64_t size = file_stat.st_size;
  const int64_t mtime = file_stat.st_mtime;
  if (!cache.get_chapters(selected_epub.path, size, mtime, chapters))
  {
    renderer->show_busy();
    Epub epub(selected_epub.path);
    if (!epub.load())
    {
      ESP_LOGE(TAG, "Could not load epub index");
      return false;
    }

    chapters.reserve(epub.get_toc_items_count());
    for (int i = 0; i < epub.get_toc_items_count(); i++)
    {
      CachedChapter chapter;
      chapter.title = replace_html_entities(epub.get_toc_item(i).title);
      chapter.spine_index = epub.get_spine_index_for_toc_index(i);
      chapters.push_back(chapter);
    }
    cache.store_chapters(selected_epub.path, size, mtime, selected_epub.title, chapters);
    cache.save();
  }

  loaded = true;
  if (state.selected_item < 0 || state.selected_item >= (int)chapters.size())
  {
    state.selected_item = 0;
  }
  state.previous_rendered_page = -1;
  ESP_LOGI(TAG, "Epub index loaded with %d chapters", chapters.size());
  return true;
}

// TODO - this is currently pretty much a copy of the epub list rendering
// we can fit a lot more on the screen by allowing variable cell heights
// and a lot of the optimisations that are used for the list aren't really
// required as we're not rendering thumbnails
void EpubToc::render()
{
  ESP_LOGD(TAG, "Rendering EPUB index");
  // what page are we on?
  int current_page = state.selected_item / ITEMS_PER_PAGE;
  // show five items per page
  int cell_height = renderer->get_page_height() / ITEMS_PER_PAGE;
  int start_index = current_page * ITEMS_PER_PAGE;
  int ypos = 0;
  // starting a fresh page or rendering from scratch?
  ESP_LOGI(TAG, "Current page is %d, previous page %d, redraw=%d", current_page, state.previous_rendered_page, m_needs_redraw);
  if (current_page != state.previous_rendered_page || m_needs_redraw)
  {
    m_needs_redraw = false;
    renderer->clear_screen();
    state.previous_selected_item = -1;
    // trigger a redraw of the items
    state.previous_rendered_page = -1;
  }
  for (int i = start_index; i < start_index + ITEMS_PER_PAGE && i < (int)chapters.size(); i++)
  {
    // do we need to draw a new page of items?
    if (current_page != state.previous_rendered_page)
    {
      // format the text using a text block
      TextBlock *title_block = new TextBlock(LEFT_ALIGN);
      title_block->add_span(chapters[i].title.c_str(), false, false);
      title_block->layout(renderer, nullptr, renderer->get_page_width());
      // work out the height of the title
      int text_height = cell_height - PADDING;
      int title_height = title_block->line_breaks.size() * renderer->get_line_height();
      // center the title in the cell
      int y_offset = title_height < text_height ? (text_height - title_height) / 2 : 0;
      // draw each line of the index block making sure we don't run over the cell
      int height = 0;
      for (int i = 0; i < title_block->line_breaks.size() && height < text_height; i++)
      {
        title_block->render(renderer, i, 10, ypos + height + y_offset);
        height += renderer->get_line_height();
      }
      // clean up the temporary index block
      delete title_block;
    }
    // clear the selection box around the previous selected item
    if (state.previous_selected_item == i)
    {
      for (int line = 0; line < 3; line++)
      {
        renderer->draw_rect(line, ypos + PADDING / 2 + line, renderer->get_page_width() - 2 * line, cell_height - PADDING - 2 * line, 255);
      }
    }
    // draw the selection box around the current selection
    if (state.selected_item == i)
    {
      for (int line = 0; line < 3; line++)
      {
        renderer->draw_rect(line, ypos + PADDING / 2 + line, renderer->get_page_width() - 2 * line, cell_height - PADDING - 2 * line, 0);
      }
    }
    ypos += cell_height;
  }
  // The chapter list is the only screen with a touch-only close control.
  // Draw it in the unused top margin, leaving the content geometry unchanged.
  const int saved_margin_top = renderer->get_margin_top();
  const int saved_margin_left = renderer->get_margin_left();
  renderer->set_margin_top(0);
  renderer->set_margin_left(0);
  renderer->draw_text(5, 0, "[X]");
  renderer->set_margin_top(saved_margin_top);
  renderer->set_margin_left(saved_margin_left);
  state.previous_selected_item = state.selected_item;
  state.previous_rendered_page = current_page;
}

uint16_t EpubToc::get_selected_toc()
{
  if (!loaded || chapters.empty())
  {
    return 0;
  }
  return chapters[state.selected_item].spine_index;
}

bool EpubToc::is_for(const EpubListItem &item) const
{
  return strcmp(selected_epub.path, item.path) == 0;
}
