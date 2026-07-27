#pragma once
#include <stdbool.h>

// Connect to WiFi in station mode and block until an IP is obtained.
void net_wifi_start(const char *ssid, const char *pass);

// Local IP as a string (valid after net_wifi_start returns). Empty if not connected.
const char *net_wifi_ip(void);

// Current AP RSSI in dBm (0 if unknown).
int net_wifi_rssi(void);
