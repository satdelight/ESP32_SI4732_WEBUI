#pragma once
#include <Arduino.h>

namespace WebUI {
  // Startet WiFi (WiFiManager) und den Async-Webserver
  void begin();

  // Optional (derzeit leer)
  void loop();
}
