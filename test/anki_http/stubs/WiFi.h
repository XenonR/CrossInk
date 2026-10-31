#pragma once
constexpr int WL_CONNECTED = 3;
struct TestWifi {
  int status() const { return WL_CONNECTED; }
  int RSSI() const { return -50; }
};
inline TestWifi WiFi;
