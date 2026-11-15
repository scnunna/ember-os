#pragma once
// On-watch diagnostics log (/flash/diag.log): see diag.c
void diag_start(void);                       // after storage + RTC are up
void diag_event(const char *fmt, ...);       // one line, timestamped; any task (not from an ISR)
void diag_flush(void);                       // write buffered lines now (before deep sleep / shutdown)
void diag_dump(int kb);                      // USB shell
void diag_clear(void);
