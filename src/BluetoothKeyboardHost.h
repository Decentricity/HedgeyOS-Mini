#pragma once

#include <functional>
#include <stdint.h>
#include <string>

class BluetoothKeyboardHost
{
public:
  typedef std::function<void(bool)> NotifyCallback;

  explicit BluetoothKeyboardHost(NotifyCallback notify);
  void start();
  void start_pairing();
  void set_accepting_input(bool accepting);
  bool drain_text(std::string &text);
  std::string status() const;

private:
  BluetoothKeyboardHost(const BluetoothKeyboardHost &);
  BluetoothKeyboardHost &operator=(const BluetoothKeyboardHost &);

  struct Impl;
  Impl *impl;
};
