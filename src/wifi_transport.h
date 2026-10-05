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

// True once the TCP server is up (WiFi connected and listening). Until then the
// other client calls are inert, so callers can retry init instead of crashing.
bool wifi_transport_ready();

// Non-blocking: accept a waiting client if one is present.
// Returns true if a client was accepted (into the internal handle), else false.
bool wifi_transport_client_available();

// Read exactly `n` bytes from the client, waiting at most `timeout_ms`.
// Returns the count read: less than `n` on timeout or disconnect. This is the
// session link's read (src/nand_session.h).
size_t wifi_transport_read(uint8_t *buf, size_t n, uint32_t timeout_ms);

// Send a buffer of data to the connected client.
// Returns number of bytes sent.
size_t wifi_transport_send(const uint8_t *data, size_t len);

// Flush, close connection, and clean up.
void wifi_transport_close();

// Get the local IP address as a string.
String wifi_transport_get_ip();

#endif // WIFI_TRANSPORT_H
