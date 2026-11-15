#pragma once
#include "cJSON.h"
// Guitar practice log: /flash/practice.csv, one line per session: yyyymmdd,HH:MM,minutes,tool,bpm_max,changes
void practice_log(const char *tool, int minutes, int bpm_max, int changes);
cJSON *practice_json(int max_sessions);      // newest last, as Ember Link "sessions" array
int practice_week_minutes(void);             // last 7 days
int practice_streak(void);                   // consecutive days up to today with any practice
