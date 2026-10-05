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

bool wifi_transport_ready() {
  return s_server != NULL;
}

bool wifi_transport_client_available() {
  if (!s_server) return false;   // WiFi never came up — don't deref a null server
  s_client = s_server->available();
  if (s_client) {
    s_client.setNoDelay(true);
    return true;
  }
  return false;
}

size_t wifi_transport_read(uint8_t *buf, size_t n, uint32_t timeout_ms) {
  size_t got = 0;
  unsigned long start = millis();
  while (got < n) {
    int avail = s_client.available();
    if (avail > 0) {
      int k = s_client.read(buf + got, n - got < (size_t)avail ? n - got : (size_t)avail);
      if (k > 0) { got += k; start = millis(); continue; }
    }
    if (!s_client.connected() || millis() - start >= timeout_ms) break;
    delay(1);
  }
  return got;
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
