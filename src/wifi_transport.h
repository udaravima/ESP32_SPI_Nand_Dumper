#ifndef WIFI_TRANSPORT_H
#define WIFI_TRANSPORT_H

#include <WiFi.h>

// WiFi TCP transport for streaming data to a PC client.
// Handles connection, handshake, and data sending.

typedef struct {
  const char *ssid;
  const char *password;
  uint16_t port;
} wifi_transport_config_t;

// Initialize WiFi and start TCP server. Blocks until connected.
// Returns true on success.
bool wifi_transport_init(const wifi_transport_config_t *config);

// Wait for a client to connect. Blocks indefinitely.
// Returns true when client is connected.
bool wifi_transport_wait_client();

// Wait for the 'G' (GO) trigger from the client.
void wifi_transport_wait_trigger();

// Send a buffer of data to the connected client.
// Returns number of bytes sent.
size_t wifi_transport_send(const uint8_t *data, size_t len);

// Flush, close connection, and clean up.
void wifi_transport_close();

// Get the local IP address as a string.
String wifi_transport_get_ip();

#endif // WIFI_TRANSPORT_H
