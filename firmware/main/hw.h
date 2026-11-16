#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// ---- init (call once, after bsp_i2c_init) ----
void hw_init(void);

// ---- power (AXP2101) ----
typedef struct { int pct; int mv; bool present; bool charging; bool vbus; } batt_t;
void hw_batt(batt_t *b);
int  hw_pwrkey_short(void);              // power button clicked since last call? 1/0, -1 = PMU not readable
void hw_pmu_regs(uint8_t out[4]);        // 0x22 0x27 0x41 0x49 for the shell

// ---- clock (PCF85063) - stores LOCAL time ----
bool hw_rtc_get(struct tm *t);          // false if RTC unset/unreadable
void hw_rtc_set(const struct tm *t);
bool hw_rtc_valid(void);

// ---- steps (software counter on QMI8658) ----
uint32_t hw_steps_today(void);
void hw_steps_history(uint32_t out[7]); // [0]=today, [1]=yesterday ...
void hw_steps_save(void);
bool hw_imu_ok(void);
void hw_imu_get_g(float *x, float *y, float *z);
// ---- haptics (vibration motor on GPIO18; pref "haptic", default on) ----
void hw_buzz(int ms);                              // one pulse (non-blocking)
void hw_buzz_pattern(int count, int on_ms, int off_ms);
// ---- raise-to-wake (pref "raise", default on) ----
typedef struct { int64_t t_notup, t_up_since; bool armed; float px, py, pz; } raise_t;   // p* = previous sample   // t_notup = last time the screen faced away
bool hw_raise_feed(raise_t *r, float x, float y, float z, int64_t t_ms);   // pure detector: true = wrist raised
int  hw_raise_selftest(void);                      // synthetic sequences; returns number of failures
uint32_t hw_imu_still_ms(void);       // how long the wrist has been still (adaptive polling + deep-sleep decision)
bool hw_imu_wom_arm(void);            // configure QMI8658 wake-on-motion -> INT1 (GPIO21) before deep sleep

// ---- storage: SD if present, else internal flash FAT ----
const char *hw_storage_root(void);      // "/sdcard" or "/flash"
bool hw_storage_is_sd(void);
void hw_storage_remount(void);
void hw_storage_info(uint64_t *total, uint64_t *free_bytes);

// ---- audio ----
bool hw_mic_open(uint32_t rate, float gain_db);
int  hw_mic_read_mono(int16_t *out, int frames);   // returns frames read (0 on error)
void hw_mic_close(void);
bool hw_mic_open_as(uint32_t rate, float gain_db, const char *who, const char *what);   // who/what show in "mic in use"
bool hw_mic_owner(char *who, size_t wn, char *what, size_t tn, uint32_t *secs, uint32_t *rate);   // false = mic free
// sharing: while someone owns the mic at `rate`, one listener can receive a copy of every frame they read
bool hw_mic_tap_open(uint32_t rate);
int  hw_mic_tap_read(int16_t *out, int frames, int timeout_ms);
void hw_mic_tap_close(void);
bool hw_spk_open(uint32_t rate, int vol);
void hw_spk_write(const int16_t *mono, int frames);
void hw_spk_close(void);
void hw_beep(int hz, int ms, int vol);             // blocking helper

// ---- settings (NVS) ----
int  hw_pref_get(const char *key, int def);
void hw_pref_set(const char *key, int val);
size_t hw_blob_get(const char *key, void *buf, size_t cap);   // returns bytes read (0 = none)
void hw_blob_set(const char *key, const void *buf, size_t n);
