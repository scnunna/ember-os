#pragma once
#include <stdint.h>
#include <stdbool.h>

// Bluetooth LE peripheral speaking the Chronos app protocol (Android, local only - no cloud).
#define BLE_NOTIF_MAX 8
typedef struct { uint8_t icon; char text[96]; char time[8]; } ble_notif_t;

void ble_start(void);                 // init stack + advertise (if enabled in prefs)
void ble_set_enabled(bool on);
bool ble_enabled(void);
bool ble_connected(void);
void ble_test_radio_off(bool off);
void ble_link_fast(bool fast);        // fast connection events (audio) or the low-power idle rhythm
void ble_link_slow_later(int ms);
int  ble_link_interval_ms(void);   // USB test shell: "radio off" / "radio on"
int  ble_notif_count(void);
int  ble_notif_unread(void);
void ble_notif_mark_seen(void);           // total received since boot
int  ble_notif_get(int back, ble_notif_t *out);   // back=0 newest ... returns 1 if present
int  ble_phone_batt(void);            // -1 unknown
void ble_send_steps(uint32_t steps);
void ble_music(uint16_t cmd);         // MUSIC_* below
void ble_find_phone(bool on);
const char *ble_name(void);
// ---- pairing (Ember Link security, S1) ----
void ble_pair_open(void);             // delete the bonded phone and accept a new one for 120 s
bool ble_pair_window_open(void);
bool ble_bonded(void);

#define MUSIC_PLAY     0x9D00
#define MUSIC_PAUSE    0x9D01
#define MUSIC_PREV     0x9D02
#define MUSIC_NEXT     0x9D03
#define MUSIC_TOGGLE   0x9900
#define VOLUME_UP      0x99A1
#define VOLUME_DOWN    0x99A2
#define VOLUME_MUTE    0x99A3
