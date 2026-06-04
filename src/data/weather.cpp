#include "weather.h"
#include "compat.h"
#include <WiFiClient.h>
#if defined(ESP32)
  #include <HTTPClient.h>
#else
  #include <ESP8266HTTPClient.h>
#endif
#include <ArduinoJson.h>

// Open-Meteo is plain HTTP (and HTTPS); we use HTTP to save TLS memory.
// API doc: https://open-meteo.com/en/docs

namespace Weather {

bool fetch(const Settings& s, WeatherData& out) {
    out.err[0] = '\0';
    if (s.weatherLat == 0.0f && s.weatherLon == 0.0f) {
        snprintf(out.err, sizeof(out.err), "no lat/lon");
        out.valid = false;
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,apparent_temperature,weather_code,relative_humidity_2m,wind_speed_10m"
        "&daily=temperature_2m_min,temperature_2m_max,weather_code"
        "&forecast_days=3"
        "&timezone=auto",
        s.weatherLat, s.weatherLon);

    WiFiClient client;
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(8000);
    if (!http.begin(client, url)) {
        snprintf(out.err, sizeof(out.err), "http begin");
        out.valid = false;
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        snprintf(out.err, sizeof(out.err), "HTTP %d", code);
        http.end();
        out.valid = false;
        return false;
    }

    JsonDocument doc;
    auto err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        snprintf(out.err, sizeof(out.err), "JSON %s", err.c_str());
        out.valid = false;
        return false;
    }

    JsonObject cur = doc["current"].as<JsonObject>();
    if (cur.isNull()) { snprintf(out.err, sizeof(out.err), "no current"); out.valid = false; return false; }
    out.tempC    = cur["temperature_2m"]      | -999.0f;
    out.feelsC   = cur["apparent_temperature"] | out.tempC;
    out.code     = cur["weather_code"]        | 0;
    out.humidity = cur["relative_humidity_2m"]| -1;
    out.windKmh  = cur["wind_speed_10m"]      | -1.0f;

    JsonObject daily = doc["daily"].as<JsonObject>();
    JsonArray  tmins = daily["temperature_2m_min"].as<JsonArray>();
    JsonArray  tmaxs = daily["temperature_2m_max"].as<JsonArray>();
    JsonArray  codes = daily["weather_code"].as<JsonArray>();
    for (int i = 0; i < 3; i++) {
        out.forecast[i].tmin = (i < (int)tmins.size()) ? tmins[i].as<float>() : -999.0f;
        out.forecast[i].tmax = (i < (int)tmaxs.size()) ? tmaxs[i].as<float>() : -999.0f;
        out.forecast[i].code = (i < (int)codes.size()) ? codes[i].as<uint8_t>() : 0;
    }

    out.valid = true;
    return true;
}

const char* describe(uint8_t code) {
    // WMO weather interpretation codes — compressed to short labels
    if (code == 0) return "Clear";
    if (code == 1 || code == 2) return "Mostly clear";
    if (code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code == 80 || code == 81 || code == 82) return "Showers";
    if (code == 85 || code == 86) return "Snow showers";
    if (code >= 95 && code <= 99) return "Thunder";
    return "—";
}

float toDisplay(float c, bool fahrenheit) {
    if (c <= -900.0f) return c;
    return fahrenheit ? (c * 9.0f / 5.0f + 32.0f) : c;
}

}  // namespace Weather
