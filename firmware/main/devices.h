#pragma once
// Remembered devices (phones, laptops) that paired with the watch: name, your nickname, last time seen.
// The security keys live in NimBLE's bond store; this list adds the human side and survives reboots (NVS "devs").
#include <stdbool.h>
#include <stdint.h>

#define DEV_MAX 3                                  // = CONFIG_BT_NIMBLE_MAX_BONDS
typedef struct {
    uint8_t type, addr[6];                         // identity address (as bonded)
    char name[32];                                 // its own Bluetooth name, read after pairing ("Pixel 8")
    char nick[24];                                 // your name for it ("" = use name)
    uint32_t last_min;                             // RTC minutes since 2000 when last connected securely
} em_dev_t;

void devices_init(void);
int  devices_count(void);
bool devices_get(int i, em_dev_t *out);
const char *devices_label(const em_dev_t *d);      // nickname, else name, else "Unknown device"
int  devices_current(void);                        // index of the device connected right now, -1 if none
bool devices_bonded(int i);                        // keys still stored (false = must pair again)
void devices_rename(int i, const char *nick);
void devices_forget(int i);                        // deletes the keys too
void devices_prefer(int i);                        // "use this device": drop the current one, accept only i for 2 min
// called by ble.c
void devices_on_secure(uint16_t conn);             // registers/updates the peer, enforces preference, reads its name
void devices_on_disconnect(void);
void devices_set_name_current(const char *name);   // from Ember Companion's phone_info
void devices_print(void);                          // USB shell "devices"
