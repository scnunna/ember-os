// App registry. To add an app: write start(screen)/stop(), add one line here.
#include "app.h"

void app_steps_start(lv_obj_t *s);    void app_steps_stop(void);
void app_rec_start(lv_obj_t *s);      void app_rec_stop(void);
void app_tuner_start(lv_obj_t *s);    void app_tuner_stop(void);
void app_metro_start(lv_obj_t *s);    void app_metro_stop(void);
void app_timer_start(lv_obj_t *s);    void app_timer_stop(void);
void app_sound_start(lv_obj_t *s);    void app_sound_stop(void);
void app_remote_start(lv_obj_t *s);   void app_remote_stop(void);
void app_phone_start(lv_obj_t *s);    void app_phone_stop(void);
void app_settings_start(lv_obj_t *s); void app_settings_stop(void);

const app_t APPS[] = {
    {"Steps",    LV_SYMBOL_GPS,       0x1F9D8A, app_steps_start,    app_steps_stop},
    {"Recorder", LV_SYMBOL_AUDIO,     0xD1495B, app_rec_start,      app_rec_stop},
    {"Tuner",    LV_SYMBOL_EDIT,      0xEDAE49, app_tuner_start,    app_tuner_stop},
    {"Metronome",LV_SYMBOL_BELL,      0x8E6CEF, app_metro_start,    app_metro_stop},
    {"Timer",    LV_SYMBOL_REFRESH,   0x3A86FF, app_timer_start,    app_timer_stop},
    {"Sound",    LV_SYMBOL_VOLUME_MAX,0x00A6A6, app_sound_start,    app_sound_stop},
    {"Remote",   LV_SYMBOL_KEYBOARD,  0x5C6BC0, app_remote_start,   app_remote_stop},
    {"Phone",    LV_SYMBOL_BLUETOOTH, 0x2D6CDF, app_phone_start,    app_phone_stop},
    {"Settings", LV_SYMBOL_SETTINGS,  0x6B6B6B, app_settings_start, app_settings_stop},
};
const int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);
