#pragma once
#include <stdbool.h>

// Local-only Wi-Fi hotspot + tiny web server: browse/download/delete recordings, set the clock from your phone.
// No internet, no cloud. Phone joins the watch's hotspot, opens http://192.168.4.1
void wifi_ap_start(void);      // WPA2 hotspot "Watch-XXXX" with a random password kept in NVS
void wifi_ap_stop(void);
bool wifi_ap_active(void);
const char *wifi_ap_ssid(void);
const char *wifi_ap_pass(void);
int  wifi_ap_clients(void);
