#include "BluetoothDeviceOverlay.h"

#include <Renderer/Renderer.h>
#include <algorithm>
#include <cstdio>

void BluetoothDeviceOverlay::show_scanning()
{
  is_visible = true;
  is_scanning = true;
  page = 0;
  items.clear();
}

void BluetoothDeviceOverlay::set_devices(const std::vector<Device> &devices)
{
  is_visible = true;
  is_scanning = false;
  page = 0;
  items = devices;
}

void BluetoothDeviceOverlay::hide()
{
  is_visible = false;
  is_scanning = false;
}

bool BluetoothDeviceOverlay::visible() const
{
  return is_visible;
}

void BluetoothDeviceOverlay::next_page()
{
  const int pages = std::max(1, (static_cast<int>(items.size()) + ROWS_PER_PAGE - 1) /
                                ROWS_PER_PAGE);
  if (page + 1 < pages)
    ++page;
}

void BluetoothDeviceOverlay::previous_page()
{
  if (page > 0)
    --page;
}

int BluetoothDeviceOverlay::device_at(int x, int y, int page_width, int page_height) const
{
  if (!is_visible || is_scanning || x < 8 || x >= page_width - 8 ||
      y < LIST_TOP || y >= std::min(page_height, LIST_TOP + ROWS_PER_PAGE * ROW_HEIGHT))
    return -1;
  const int index = page * ROWS_PER_PAGE + (y - LIST_TOP) / ROW_HEIGHT;
  return index >= 0 && index < static_cast<int>(items.size()) ? items[index].id : -1;
}

void BluetoothDeviceOverlay::render(Renderer *renderer) const
{
  renderer->clear_screen();
  renderer->use_selector_font(true);
  const int width = renderer->get_page_width();
  const int height = renderer->get_page_height();
  renderer->draw_text(10, 4, "Select Bluetooth device");
  renderer->fill_rect(8, 40, width - 16, 1, 0);

  if (is_scanning)
  {
    renderer->draw_text(10, LIST_TOP + 18, "Scanning for nearby BLE devices...");
  }
  else if (items.empty())
  {
    renderer->draw_text(10, LIST_TOP + 18, "No BLE devices found.");
    renderer->draw_text(10, LIST_TOP + 62, "Put keyboard in pairing mode.");
    renderer->draw_text(10, LIST_TOP + 96, "Close with X, then tap status to rescan.");
  }
  else
  {
    const int first = page * ROWS_PER_PAGE;
    const int last = std::min(static_cast<int>(items.size()), first + ROWS_PER_PAGE);
    for (int index = first; index < last; ++index)
    {
      const int y = LIST_TOP + (index - first) * ROW_HEIGHT;
      renderer->draw_rect(8, y, width - 16, ROW_HEIGHT - 5, 0);
      std::string name = items[index].name;
      while (!name.empty() && renderer->get_text_width(name.c_str()) > width - 36)
        name.erase(name.size() - 1);
      renderer->draw_text(18, y + 2, name.c_str());
      renderer->draw_text(18, y + 27, items[index].detail.c_str());
    }
  }

  char page_text[32];
  const int pages = std::max(1, (static_cast<int>(items.size()) + ROWS_PER_PAGE - 1) /
                                ROWS_PER_PAGE);
  snprintf(page_text, sizeof(page_text), "Page %d/%d", page + 1, pages);
  renderer->draw_text(width - renderer->get_text_width(page_text) - 10,
                      height - renderer->get_line_height() - 2, page_text);
  renderer->use_selector_font(false);
}
