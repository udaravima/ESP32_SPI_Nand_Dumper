#include "wifi_transport.h"
#include <Arduino.h>

static WiFiServer *s_server = NULL;
static WiFiClient s_client;
static uint16_t s_port = 3333;

bool wifi_transport_init(const wifi_transport_config_t *config) {
  s_port = config->port;

  Serial.printf("[*] Connecting to WiFi '%s'...\n", config->ssid);
  WiFi.begin(config->ssid, config->password);

  int timeout = 60;  // 30 second timeout
  while (WiFi.status() != WL_CONNECTED && timeout-- > 0) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[!] WiFi connection failed!");
    return false;
  }

  Serial.printf("\n[+] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

  s_server = new WiFiServer(s_port);
  s_server->begin();
  s_server->setNoDelay(true);
  Serial.printf("[*] TCP server on port %d\n", s_port);

  return true;
}

bool wifi_transport_wait_client() {
  Serial.println("[*] Waiting for client...");
  while (!(s_client = s_server->available())) {
    delay(10);
  }
  s_client.setNoDelay(true);
  Serial.println("[+] Client connected!");
  return true;
}

bool wifi_transport_client_available() {
  s_client = s_server->available();
  if (s_client) {
    s_client.setNoDelay(true);
    return true;
  }
  return false;
}

void wifi_transport_wait_trigger() {
  Serial.println("[*] Waiting for GO trigger...");
  while (true) {
    if (s_client.available() && s_client.read() == 'G') break;
    delay(1);
  }
  Serial.println("[*] GO received!");
}

size_t wifi_transport_send(const uint8_t *data, size_t len) {
  return s_client.write(data, len);
}

void wifi_transport_close() {
  s_client.flush();
  delay(100);
  s_client.stop();
}

String wifi_transport_get_ip() {
  return WiFi.localIP().toString();
}
