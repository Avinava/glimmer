#pragma once
#include "compat.h"
#if defined(ESP32)
  #include <WebServer.h>
  using WebServerClass = WebServer;
#else
  #include <ESP8266WebServer.h>
  using WebServerClass = ESP8266WebServer;
#endif
#include "storage.h"

namespace Web {
    void begin(Settings& settings);   // sets up routes + OTA on the given mutable settings ref
    void loop();                       // call from main loop
}
