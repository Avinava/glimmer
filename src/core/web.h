#pragma once
#include <ESP8266WebServer.h>
#include "storage.h"

namespace Web {
    void begin(Settings& settings);   // sets up routes + OTA on the given mutable settings ref
    void loop();                       // call from main loop
}
