// Amber: talk to your personal agent. Hold the watch button (or the on-screen button) and speak; release to send.
// A short tap on the on-screen button starts listening hands-free; tap again to send. The phone turns it into text and sends it to Amber (Life OS on your tailnet).
// "You said" and Amber's replies are listed below, newest first.
//
// The mic has one owner. When Amber can't get it:
//  - a meeting recording keeps going and Amber listens to the same audio (no popup, the moment is bookmarked)
//  - a music recording: a sheet shows who is listening, with "Pause, talk, resume" or "Save & stop, then talk"
//  - Tuner / Sound / Translate have nothing to lose: opening Amber closes them (a toast says which one)
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "speak.h"
#include "hw.h"
#include "link.h"

static lv_obj_t *s_scr, *s_status, *s_list, *s_talk, *s_sheet;
static lv_timer_t *s_tmr;
static int s_gen = -1;
static bool s_toggle;          // started by a short tap on the screen button (not a hold): the next tap sends
static bool s_resume_rec;      // we paused a recording for Amber: resume it when Amber stops listening
static bool s_was_listening;

static void set_status(const char *t) { if (s_status) lv_label_set_text(s_status, t); }

static void listening_ui(void)
{
    if (link_ptt_shared()) set_status(LV_SYMBOL_AUDIO "  listening (recording continues)");
    else set_status(s_toggle ? LV_SYMBOL_AUDIO "  listening... tap to send" : LV_SYMBOL_AUDIO "  listening... release to send");
    if (s_talk) lv_label_set_text(lv_obj_get_child(s_talk, 0), s_toggle ? LV_SYMBOL_OK "  Send" : LV_SYMBOL_AUDIO "  Hold to talk");
}

static void close_sheet(void) { if (s_sheet) { lv_obj_delete(s_sheet); s_sheet = NULL; } }

static bool start_talk(bool toggle);

// ---- "mic in use" sheet (only for a recording that would lose audio) ----
static void sheet_cancel(lv_event_t *e) { close_sheet(); set_status("Hold to talk to Amber"); }
static void sheet_pause(lv_event_t *e)
{
    close_sheet();
    if (!rec_pause_for_mic()) { set_status("Could not pause the recording"); return; }   // resumes by itself after Amber
    start_talk(true);
}
static void sheet_save(lv_event_t *e)
{
    close_sheet();
    rec_stop();                                  // writes the file and its bookmarks
    ui_msg("Recording saved");
    start_talk(true);
}
static void show_sheet(void)
{
    char who[16], what[40], b[120];
    uint32_t secs = 0, rate = 0;
    close_sheet();
    if (!hw_mic_owner(who, sizeof(who), what, sizeof(what), &secs, &rate)) { start_talk(true); return; }
    s_sheet = lv_obj_create(s_scr);
    lv_obj_set_size(s_sheet, 410, 502);
    lv_obj_set_pos(s_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_sheet, lv_color_hex(0x05080C), 0);
    lv_obj_set_style_border_width(s_sheet, 0, 0);
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *h = ui_label(s_sheet, &lv_font_montserrat_20, 0xFFC857, LV_SYMBOL_AUDIO "  Mic in use");
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 50);
    snprintf(b, sizeof(b), "%s: %s\nfor %lu:%02lu", who, what, (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    lv_obj_t *t = ui_label(s_sheet, &lv_font_montserrat_20, C_TEXT, b);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(t, 360);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 96);
    if (rec_active()) {
        lv_obj_t *p = ui_btn(s_sheet, "Pause, talk, resume", 340, 76, 0x1F7A4D, sheet_pause, NULL);
        lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 196);
        lv_obj_t *sv = ui_btn(s_sheet, "Save & stop, then talk", 340, 76, 0x7A4A12, sheet_save, NULL);
        lv_obj_align(sv, LV_ALIGN_TOP_MID, 0, 284);
    } else {
        lv_obj_t *n = ui_label(s_sheet, &lv_font_montserrat_14, C_DIM, "Close that app, then try again.");
        lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 220);
    }
    lv_obj_t *c = ui_btn(s_sheet, "Keep it running", 340, 70, 0x3A3A3A, sheet_cancel, NULL);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 372);
    printf("AMBER mic-busy who=%s what=%s secs=%lu\n", who, what, (unsigned long)secs);
}

// Starts listening. Returns false (and explains on screen) when it can't.
static bool start_talk(bool toggle)
{
    // an app we just closed (Tuner, Sound) may need a moment to hand the mic back
    for (int i = 0; i < 6; i++) {
        if (link_ptt_start_ex("amber", NULL)) {
            s_toggle = toggle; hw_buzz(25); listening_ui();
            printf("AMBER listening shared=%d toggle=%d\n", link_ptt_shared(), toggle);
            return true;
        }
        if (link_ptt_error() != LINK_PTT_MIC_BUSY || rec_active()) break;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    if (link_ptt_error() == LINK_PTT_MIC_BUSY) show_sheet();
    else set_status("Phone not connected - Amber needs Ember Companion");
    printf("AMBER start failed err=%d\n", link_ptt_error());
    return false;
}

static void stop_talk(void)
{
    if (link_ptt_active() && !strcmp(link_ptt_side(), "amber")) { link_ptt_stop(); hw_buzz(20); set_status("Sent. Amber is thinking..."); }
    s_toggle = false;
    if (s_talk) lv_label_set_text(lv_obj_get_child(s_talk, 0), LV_SYMBOL_AUDIO "  Hold to talk");
}

// Hold = talk while pressed. A short tap = start (toggle mode); the next tap = send.
static void talk_ev(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    static int64_t t_down;
    static bool started_here;
    if (c == LV_EVENT_PRESSED) {
        t_down = lv_tick_get();
        started_here = false;
        if (link_ptt_active()) { stop_talk(); return; }            // tap while listening = send
        started_here = start_talk(false);
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (!started_here || !link_ptt_active()) return;
        if (lv_tick_elaps(t_down) < 400) { s_toggle = true; listening_ui(); return; }   // a tap: keep listening
        stop_talk();
    }
}

static void rebuild(void)
{
    lv_obj_clean(s_list);
    int n = link_amber_count();
    if (!n) {
        lv_obj_t *l = ui_label(s_list, &lv_font_montserrat_20, C_DIM, "Ask Amber to plan your day,\nmove a block, add a task\nor take a note.");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, 330);
        return;
    }
    link_amber_t m;
    for (int i = 0; i < n && link_amber_get(i, &m); i++) {
        char b[330];
        snprintf(b, sizeof(b), "%s%s", m.mine ? "You: " : "Amber: ", m.text);
        lv_obj_t *l = ui_label(s_list, &lv_font_montserrat_20, m.mine ? C_DIM : C_TEXT, b);
        lv_obj_set_width(l, 330);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
}

static void poll(lv_timer_t *t)
{
    bool listening = link_ptt_active() && !strcmp(link_ptt_side(), "amber");
    if (s_was_listening && !listening) {                          // finished (sent, or the 30 s cap)
        s_toggle = false;
        if (s_talk) lv_label_set_text(lv_obj_get_child(s_talk, 0), LV_SYMBOL_AUDIO "  Hold to talk");
        if (s_resume_rec) { s_resume_rec = false; rec_pause(false); ui_msg("Recording resumed"); }
    }
    s_was_listening = listening;
    int g = link_amber_gen();
    if (g == s_gen) return;
    s_gen = g;
    link_amber_t m;
    if (link_amber_get(0, &m) && !m.mine && !listening) set_status("Hold to talk to Amber");
    rebuild();
}

// reply mode: VOICE = Amber speaks through the watch speaker (and shows the text); TEXT = text only.
// The watch's BOOT click toggles it while this app is open; if she is talking, the click mutes her at once.
static lv_obj_t *s_mode;
static void mode_show(void)
{
    if (!s_mode) return;
    bool v = hw_pref_get("speak", 1);
    lv_label_set_text(lv_obj_get_child(s_mode, 0), v ? LV_SYMBOL_VOLUME_MAX "\nVoice" : LV_SYMBOL_MUTE "\nText");
    lv_obj_set_style_bg_color(s_mode, lv_color_hex(v ? 0x1F7A4D : 0x3A3A3A), 0);
}
static void mode_toggle(void)
{
    bool v = !hw_pref_get("speak", 1);
    hw_pref_set("speak", v);
    if (!v && speak_active()) speak_stop();
    hw_buzz(v ? 40 : 20);
    mode_show();
    if (s_status) lv_label_set_text(s_status, v ? "Amber will answer out loud" : "Muted: answers as text only");
}
static void mode_cb(lv_event_t *e) { mode_toggle(); }
void app_amber_button(void) { mode_toggle(); }

void app_amber_start(lv_obj_t *scr)
{
    s_scr = scr;
    ui_title(scr, "Amber");
    s_talk = ui_btn(scr, LV_SYMBOL_AUDIO "  Hold to talk", 226, 84, 0x7A4A12, NULL, NULL);
    lv_obj_align(s_talk, LV_ALIGN_TOP_MID, -58, 74);
    s_mode = ui_btn(scr, "", 104, 84, 0x1E2633, mode_cb, NULL);
    lv_obj_align(s_mode, LV_ALIGN_TOP_MID, 120, 74);
    mode_show();
    lv_obj_add_event_cb(s_talk, talk_ev, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_talk, talk_ev, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_talk, talk_ev, LV_EVENT_PRESS_LOST, NULL);
    s_status = ui_label(scr, &lv_font_montserrat_14, C_DIM, link_ready() ? "Hold to talk (here or the watch button)" : "Phone not connected");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 166);
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, 360, 290);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 192);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    s_gen = -1; s_toggle = false; s_was_listening = false;
    poll(NULL);
    s_tmr = lv_timer_create(poll, 300, NULL);
}

void app_amber_stop(void)
{
    if (link_ptt_active() && !strcmp(link_ptt_side(), "amber")) link_ptt_stop();
    for (int i = 0; i < 30 && link_ptt_active(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_resume_rec) { s_resume_rec = false; rec_pause(false); }   // never leave a recording paused
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
    s_scr = s_status = s_list = s_talk = s_sheet = s_mode = NULL;
}

// Holding the watch button (display lock held): open Amber and listen while held; releasing sends.
static bool s_hold_started;
void amber_hold(bool down)
{
    int idx = -1;
    for (int i = 0; i < APP_COUNT; i++) if (!strcmp(APPS[i].name, "Amber")) idx = i;
    if (idx < 0) return;
    if (!down) {                                                   // released
        if (s_hold_started && link_ptt_active() && !strcmp(link_ptt_side(), "amber")) stop_talk();
        s_hold_started = false;
        return;
    }
    char who[16] = "";
    bool app_mic = hw_mic_owner(who, sizeof(who), NULL, 0, NULL, NULL) && !rec_active() && !link_ptt_active();
    launcher_show_app(idx);                                        // stops Tuner/Sound/Translate: frees their mic
    if (app_mic && strcmp(who, "Amber")) { char m[48]; snprintf(m, sizeof(m), "Closed %s so Amber can listen", who); ui_msg(m); }
    s_hold_started = start_talk(false);
}
