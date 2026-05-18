#pragma once

// SmallTV-Ultra physical config (see FLASHING.md)
static constexpr uint16_t SCREEN_W = 240;
static constexpr uint16_t SCREEN_H = 240;

// Backlight is ACTIVE-LOW PWM on GPIO5 (TFT_BL)
// analogWrite(TFT_BL, 0)    = full bright
// analogWrite(TFT_BL, 1023) = off
static constexpr uint16_t BL_FULL = 0;
static constexpr uint16_t BL_OFF  = 1023;

// AP-mode setup network
static constexpr const char* SETUP_AP_SSID = "glimmer-setup";

// mDNS name (for ArduinoOTA + nice URL)
static constexpr const char* MDNS_HOSTNAME = "glimmer";

// Defaults
static constexpr uint32_t DEFAULT_REFRESH_MIN = 5;     // minutes between API fetches
static constexpr uint32_t DEFAULT_CHANNEL_SEC = 8;     // auto-rotate interval
static constexpr uint8_t  DEFAULT_BRIGHTNESS  = 80;    // 0-100 %
