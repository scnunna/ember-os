#pragma once
// Life manager check-ins: today's plan from Life OS + "On it?" prompts (docs/LIFE_MANAGER_PLAN.md)
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cJSON.h"

#define SCHED_MAX 24
#define SCHED_CIQ 48
#define SCHED_SLOTS 8
#define SCHED_IDS 6          // block ids sent with one check-in

typedef struct { char title[40]; int s, e; bool done; } sched_view_t;

void sched_start(void (*on_new_answer)(void));   // loads the stored plan + queue, starts the check-in task
bool sched_set(const cJSON *sched_msg, char *err, size_t errn);
void sched_cfg(const cJSON *ci_cfg_msg);
void sched_ack(uint32_t upto);
int  sched_pending(cJSON *arr, int max);        // appends queued "ci" messages to arr; returns how many
uint32_t sched_date(void);
const char *sched_gen(void);
int  sched_count(void);
int  sched_queued(void);
uint32_t sched_next_s(void);                    // seconds to the next check-in today (0 = none)
void sched_now_text(char *out, size_t n);       // "Now: X  until 11:00" / "Next: X  14:00" / ""
bool sched_block(int i, sched_view_t *v);       // false past the end, or when the plan is not today's
// slot minute 1440 = end of day (asked at 00:00 for the day that ended)
bool sched_slot(int k, int *minute, char *ans);   // check-in slot k of the stored plan; ans 0 / 'o' 'f' 'z' 'm'
bool sched_today(void);                         // the stored plan is today's
int  sched_adherence(void);                     // 0..100, -1 = nothing answered yet
void sched_print(void);                         // USB shell
void sched_test_minute(int min);
void sched_reset(void);                         // tests: forget plan + queue                // tests: pretend the clock reads this minute (-1 = real clock)
