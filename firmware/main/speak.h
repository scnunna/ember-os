#pragma once
#include <stdint.h>
#include <stdbool.h>
// Amber speaks on the watch speaker (Ember Link v1.5, see speak.c)
void speak_begin(const char *id, int n);   // CMD "say"
void speak_feed(const uint8_t *blk, int len);   // SPK characteristic write
void speak_end(void);                       // CMD "say_end"
void speak_stop(void);                      // CMD "say_stop" / BOOT press
bool speak_active(void);
