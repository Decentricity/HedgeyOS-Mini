#pragma once

#include <string>
#include <vector>

class Renderer;

class BluetoothDeviceOverlay
{
public:
  struct Device
  {
    int id;
    std::string name;
    std::string detail;
  };

  void show_scanning();
  void set_devices(const std::vector<Device> &devices);
  void hide();
  bool visible() const;
  void next_page();
  void previous_page();
  int device_at(int x, int y, int page_width, int page_height) const;
  void render(Renderer *renderer) const;

private:
  static const int ROWS_PER_PAGE = 7;
  static const int LIST_TOP = 48;
  static const int ROW_HEIGHT = 58;

  bool is_visible = false;
  bool is_scanning = false;
  int page = 0;
  std::vector<Device> items;
};
