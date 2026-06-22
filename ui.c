#include "ui.h"
#include "moonraker.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ---- palette ---------------------------------------------------------- */
#define COL_BG      lv_color_hex(0x12161d)
#define COL_CARD    lv_color_hex(0x1d2430)
#define COL_TEXT    lv_color_hex(0xf2f5f8)
#define COL_MUTED   lv_color_hex(0x8a94a3)
#define COL_NOZZLE  lv_color_hex(0xff7a18)
#define COL_BED     lv_color_hex(0x18b4ff)
#define COL_FAN     lv_color_hex(0x9b8cff)
#define COL_OK      lv_color_hex(0x36d07a)
#define COL_WARN    lv_color_hex(0xffb020)
#define COL_ERR     lv_color_hex(0xff4d4f)

/* ---- widget handles updated each tick --------------------------------- */
static lv_obj_t *conn_dot;
static lv_obj_t *noz_val;
static lv_obj_t *fan_val;
static lv_obj_t *bed_val;
static lv_obj_t *state_lbl;
static lv_obj_t *file_lbl;
static lv_obj_t *bar;
static lv_obj_t *pct_lbl;
static lv_obj_t *time_lbl;
static lv_obj_t *eta_lbl;
static lv_obj_t *layer_lbl;
static lv_obj_t *thumb_img;

/* ---- helpers ---------------------------------------------------------- */
static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

/* "printing" -> "Printing" */
static void titlecase(char *s)
{
    if (s[0]) s[0] = (char)toupper((unsigned char)s[0]);
}

/* ---- periodic refresh ------------------------------------------------- */
static void ui_tick(lv_timer_t *t)
{
    (void)t;
    printer_status_t s;
    moonraker_get(&s);

    lv_obj_set_style_bg_color(conn_dot, s.online ? COL_OK : COL_ERR, 0);

    char buf[64];
    snprintf(buf, sizeof buf, "%.0f / %.0f \xC2\xB0""C", s.nozzle_temp, s.nozzle_target);
    lv_label_set_text(noz_val, buf);
    snprintf(buf, sizeof buf, "%.0f / %.0f \xC2\xB0""C", s.bed_temp, s.bed_target);
    lv_label_set_text(bed_val, buf);
    snprintf(buf, sizeof buf, "%.0f%%", s.fan_speed * 100.0);
    lv_label_set_text(fan_val, buf);

    char state[32];
    snprintf(state, sizeof state, "%s", s.state[0] ? s.state : "unknown");
    titlecase(state);
    lv_label_set_text(state_lbl, state);

    lv_color_t sc = COL_MUTED;
    if      (!strcmp(s.state, "printing")) sc = COL_OK;
    else if (!strcmp(s.state, "paused"))   sc = COL_WARN;
    else if (!strcmp(s.state, "error"))    sc = COL_ERR;
    else if (!strcmp(s.state, "complete")) sc = COL_BED;
    lv_obj_set_style_text_color(state_lbl, sc, 0);

    lv_label_set_text(file_lbl, s.filename[0] ? s.filename : "---");

    int pct = (int)(s.progress * 100.0 + 0.5);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    snprintf(buf, sizeof buf, "%d%%", pct);
    lv_label_set_text(pct_lbl, buf);

    long el = (long)s.print_duration;
    snprintf(buf, sizeof buf, "Elapsed %ld:%02ld:%02ld",
             el / 3600, (el % 3600) / 60, el % 60);
    lv_label_set_text(time_lbl, buf);

    if (s.remaining >= 0.0) {
        long rm = (long)s.remaining;
        snprintf(buf, sizeof buf, "ETA %ld:%02ld:%02ld",
                 rm / 3600, (rm % 3600) / 60, rm % 60);
    } else {
        snprintf(buf, sizeof buf, "ETA --:--:--");
    }
    lv_label_set_text(eta_lbl, buf);

    if (s.layer_total > 0)
        snprintf(buf, sizeof buf, "Layer %d / %d", s.layer_current, s.layer_total);
    else
        snprintf(buf, sizeof buf, "Layer --");
    lv_label_set_text(layer_lbl, buf);

    /* swap in a new thumbnail only when the downloaded path changes */
    static char shown[256] = "";
    if (strcmp(s.thumb_path, shown) != 0) {
        snprintf(shown, sizeof shown, "%s", s.thumb_path);
        if (s.thumb_path[0]) {
            char src[300];
            snprintf(src, sizeof src, "A:%s", s.thumb_path);
            lv_image_set_src(thumb_img, src);
            lv_obj_align(thumb_img, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_remove_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---- screen construction --------------------------------------------- */
void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* header ---------------------------------------------------------- */
    lv_obj_t *hdr = make_label(scr, &lv_font_montserrat_28, COL_TEXT,
                               "KLIPPER STATUS");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 2, 2);

    conn_dot = lv_obj_create(scr);
    lv_obj_set_size(conn_dot, 16, 16);
    lv_obj_set_style_radius(conn_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(conn_dot, 0, 0);
    lv_obj_set_style_bg_color(conn_dot, COL_MUTED, 0);
    lv_obj_align(conn_dot, LV_ALIGN_TOP_RIGHT, -4, 10);

    /* temperature + fan cards (sized by % so they fit any fb resolution) */
    const lv_coord_t card_h = 138;

    lv_obj_t *noz_card = make_card(scr, LV_PCT(32), card_h);
    lv_obj_align(noz_card, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_obj_t *noz_cap = make_label(noz_card, &lv_font_montserrat_24,
                                   COL_NOZZLE, "NOZZLE");
    lv_obj_align(noz_cap, LV_ALIGN_TOP_LEFT, 0, 0);
    noz_val = make_label(noz_card, &lv_font_montserrat_36, COL_TEXT, "-- / --");
    lv_obj_align(noz_val, LV_ALIGN_LEFT_MID, 0, 12);

    lv_obj_t *bed_card = make_card(scr, LV_PCT(32), card_h);
    lv_obj_align(bed_card, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_t *bed_cap = make_label(bed_card, &lv_font_montserrat_24,
                                   COL_BED, "BED");
    lv_obj_align(bed_cap, LV_ALIGN_TOP_LEFT, 0, 0);
    bed_val = make_label(bed_card, &lv_font_montserrat_36, COL_TEXT, "-- / --");
    lv_obj_align(bed_val, LV_ALIGN_LEFT_MID, 0, 12);

    lv_obj_t *fan_card = make_card(scr, LV_PCT(32), card_h);
    lv_obj_align(fan_card, LV_ALIGN_TOP_RIGHT, 0, 44);
    lv_obj_t *fan_cap = make_label(fan_card, &lv_font_montserrat_24,
                                   COL_FAN, "FAN");
    lv_obj_align(fan_cap, LV_ALIGN_TOP_LEFT, 0, 0);
    fan_val = make_label(fan_card, &lv_font_montserrat_36, COL_TEXT, "--");
    lv_obj_align(fan_val, LV_ALIGN_LEFT_MID, 0, 12);

    /* job card -------------------------------------------------------- */
    lv_obj_t *job = make_card(scr, LV_PCT(100), 240);
    lv_obj_align(job, LV_ALIGN_TOP_LEFT, 0, 200);

    /* left column: print thumbnail (hidden until one is downloaded) */
    thumb_img = lv_image_create(job);
    lv_obj_align(thumb_img, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);

    /* right column: text/progress, offset past the thumbnail (800px layout) */
    const lv_coord_t tx = 200;        /* left edge of the text column */
    const lv_coord_t tw = 532;        /* width of the text column     */

    state_lbl = make_label(job, &lv_font_montserrat_36, COL_MUTED, "Connecting");
    lv_obj_align(state_lbl, LV_ALIGN_TOP_LEFT, tx, 0);

    pct_lbl = make_label(job, &lv_font_montserrat_36, COL_TEXT, "0%");
    lv_obj_align(pct_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);

    file_lbl = make_label(job, &lv_font_montserrat_24, COL_TEXT, "---");
    lv_label_set_long_mode(file_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(file_lbl, tw);
    lv_obj_align(file_lbl, LV_ALIGN_TOP_LEFT, tx, 44);

    bar = lv_bar_create(job);
    lv_obj_set_size(bar, tw, 32);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, tx, 84);
    lv_obj_set_style_radius(bar, 16, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x39414d), 0);  /* empty track */
    lv_obj_set_style_bg_color(bar, COL_OK, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 16, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    time_lbl = make_label(job, &lv_font_montserrat_24, COL_MUTED, "Elapsed --:--:--");
    lv_obj_align(time_lbl, LV_ALIGN_TOP_LEFT, tx, 138);

    eta_lbl = make_label(job, &lv_font_montserrat_24, COL_MUTED, "ETA --:--:--");
    lv_obj_align(eta_lbl, LV_ALIGN_TOP_RIGHT, 0, 138);

    layer_lbl = make_label(job, &lv_font_montserrat_24, COL_MUTED, "Layer --");
    lv_obj_align(layer_lbl, LV_ALIGN_TOP_LEFT, tx, 174);

    lv_timer_create(ui_tick, 500, NULL);
}
