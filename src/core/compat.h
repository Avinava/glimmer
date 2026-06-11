#pragma once
//
// ESP8266 / ESP32 portability shims for glimmer.
//
// glimmer was originally written for the GeekMagic SmallTV-Ultra (ESP8266).
// This header lets the same source build for ESP32 boards (e.g. the
// ESP32-2432S028R "Cheap Yellow Display") by abstracting the framework APIs
// that differ between the two Arduino cores.
//
// SCOPE: only the universally-needed bits (WiFi + mDNS + small helpers) live
// here, because this header is pulled into many translation units. WebServer,
// HTTPClient and the TLS client are included directly in the few files that
// need them (web.*, api.cpp, weather.cpp) — pulling WebServer in everywhere
// clashes with TFT_eSPI's FS usage. Display / backlight / pins: see config.h
// and platformio.ini.

#if defined(ESP32)
  #include <WiFi.h>
  #include <ESPmDNS.h>

  // ESP32 has no getMaxFreeBlockSize(); closest equivalent is largest alloc block.
  static inline uint32_t compatMaxFreeBlock() { return ESP.getMaxAllocHeap(); }
  // mDNS on ESP32 needs no per-loop update() (runs in the background).
  static inline void compatMdnsUpdate() {}
  // WiFi tuning that only exists / matters on the (heap-tight, sleepy) ESP8266.
  static inline void compatWifiTune() { WiFi.setSleep(false); }
  static inline void compatWifiHostname(const char* h) { WiFi.setHostname(h); }
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266mDNS.h>

  static inline uint32_t compatMaxFreeBlock() { return ESP.getMaxFreeBlockSize(); }
  static inline void compatMdnsUpdate() { MDNS.update(); }
  static inline void compatWifiTune() {
      WiFi.setSleepMode(WIFI_NONE_SLEEP);
      WiFi.setOutputPower(20.5);
  }
  static inline void compatWifiHostname(const char* h) { WiFi.hostname(h); }
#endif
