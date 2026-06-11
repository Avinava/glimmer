#pragma once

// Physical panel config.
//   ESP8266 SmallTV-Ultra : 240x240 ST7789, backlight ACTIVE-LOW PWM on GPIO5.
//   ESP32  CYD (2432S028R): 240x320 ILI9341, backlight ACTIVE-HIGH PWM on GPIO21.
// glimmer's UI is laid out for a 240-wide canvas; on the taller CYD panel the
// content fills the top and the remainder stays background-black.
#if defined(ESP32)
// CYD in landscape (rotation 1): 320 wide x 240 tall.
static constexpr uint16_t SCREEN_W = 320;
static constexpr uint16_t SCREEN_H = 240;
// Active-HIGH backlight: 255 = full bright, 0 = off (8-bit analogWrite).
static constexpr uint16_t BL_FULL = 255;
static constexpr uint16_t BL_OFF  = 0;
#else
static constexpr uint16_t SCREEN_W = 240;
static constexpr uint16_t SCREEN_H = 240;
// Active-LOW backlight: 0 = full bright, 1023 = off.
static constexpr uint16_t BL_FULL = 0;
static constexpr uint16_t BL_OFF  = 1023;
#endif

// AP-mode setup network
static constexpr const char* SETUP_AP_SSID = "glimmer-setup";

// mDNS name (for ArduinoOTA + nice URL)
static constexpr const char* MDNS_HOSTNAME = "glimmer";

// Defaults
static constexpr uint32_t DEFAULT_REFRESH_MIN = 5;     // minutes between API fetches
static constexpr uint32_t DEFAULT_CHANNEL_SEC = 8;     // auto-rotate interval
static constexpr uint8_t  DEFAULT_BRIGHTNESS  = 80;    // 0-100 %
