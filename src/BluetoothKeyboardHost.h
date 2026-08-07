#pragma once

#include <functional>
#include <stdint.h>
#include <string>
#include <vector>

class BluetoothKeyboardHost
{
public:
  enum Event
  {
    STATUS_CHANGED,
    INPUT_READY,
    DEVICES_READY
  };

  struct DeviceInfo
  {
    int id;
    std::string name;
    int rssi;
    bool likely_keyboard;
    bool paired;
    bool classic;
  };

  typedef std::function<void(Event)> NotifyCallback;

  explicit BluetoothKeyboardHost(NotifyCallback notify);
  void start();
  void start_pairing();
  std::vector<DeviceInfo> devices() const;
  bool connect_device(int id);
  void set_accepting_input(bool accepting);
  bool drain_text(std::string &text);
  std::string status() const;

private:
  BluetoothKeyboardHost(const BluetoothKeyboardHost &);
  BluetoothKeyboardHost &operator=(const BluetoothKeyboardHost &);

  struct Impl;
  Impl *impl;
};
