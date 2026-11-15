#pragma once
// Ember Link v1: the watch side of docs/EMBER_LINK.md (BLE service eb3f0001-..., JSON messages, ADPCM audio).
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "host/ble_hs.h"

// ---- GATT glue (used by ble.c) ----
extern const ble_uuid128_t LINK_UUID_SVC, LINK_UUID_CMD, LINK_UUID_EVT, LINK_UUID_AUDIO;
extern uint16_t link_evt_handle, link_audio_handle;
int  link_cmd_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg);
int  link_noop_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg);
void link_on_connect(uint16_t conn);
void link_on_disconnect(void);
void link_on_subscribe(uint16_t attr, bool notify);
void link_on_secure(bool secure);      // link encrypted + authenticated + bonded
void link_start(void);
bool link_ready(void);                  // the Ember Companion app is connected and listening

// ---- notifications from the phone ----
#define LINK_NOTIF_MAX 8
typedef struct { int id; char app[24]; char title[48]; char body[200]; char replies[3][24]; int nrep; char time[8]; } link_notif_t;
int  link_notif_count(void);            // currently stored (newest first via link_notif_get)
bool link_notif_get(int i, link_notif_t *out);
int  link_notif_unread(void);
void link_notif_mark_seen(void);
void link_notif_reply(int id, int idx);
void link_notif_dismiss(int id);

// ---- other watch -> phone ----
void link_send_steps(void);
void link_send_practice(void);
void link_music(const char *cmd);       // toggle next prev vol_up vol_down
void link_find_phone(bool on);

// ---- weather (from the phone) ----
bool link_weather(char *out, size_t n); // "-2°C  Snow" ; false if none / older than 3 h

// ---- translation ----
bool link_ptt_start(bool me);           // starts mic -> ADPCM -> AUDIO stream (false if not connected / mic busy)
bool link_ptt_start_ex(const char *side, const char *ctx_json);   // side "me" / "them" / "amber"; ctx = JSON object or NULL
void link_ptt_stop(void);

// ---- Amber (personal agent): what the phone heard + replies ----
#define LINK_AMBER_MAX 10
typedef struct { char text[304]; bool mine; } link_amber_t;          // mine = "You said"
int  link_amber_count(void);
bool link_amber_get(int i, link_amber_t *out);    // 0 = newest
int  link_amber_gen(void);                         // changes on every new line
bool link_ptt_active(void);
void link_tr_state(char *me, char *them, char *src, size_t srcn, char *dst, size_t dstn, int *gen);   // gen increments per new result

// ---- self-test hook ----
int  link_adpcm_selftest(char *hex_out, size_t n);   // encodes the reference test signal, hex of block 1
void link_inject(const char *json);     // feed a message as if it came from the phone (tests, trusted)
void link_inject_untrusted(const char *json);   // same, but treated like the radio (needs on-watch confirmation)
