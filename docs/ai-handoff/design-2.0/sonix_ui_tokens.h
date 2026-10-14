/* Sonix UI 2.0 — дизайн-токены (из прототипа prototype/Sonix UI 2.0.dc.html).
 * Экран: 480×800 (R1) / 480×720 (R3 Pro II), RGB565, LVGL.
 * Все размеры — в пикселях экрана. Цвета, зависящие от обложки, задаются
 * в OKLCH (L 0..1, C 0..0.4, H — оттенок обложки 0..360) и пересчитываются
 * в RGB565 один раз при смене трека/экрана: sx_oklch_to_565().
 */
#ifndef SONIX_UI_TOKENS_H
#define SONIX_UI_TOKENS_H

#include <stdint.h>

/* ---------- Каркас ---------- */
#define SX_STATUSBAR_H        44
#define SX_STATUSBAR_FONT     22
#define SX_HEADER_H           76   /* под статус-баром, top = 44 */
#define SX_HEADER_BTN         72   /* круглая цель «назад» и кнопки шапки */
#define SX_TITLE_FONT         34   /* жирный; 30, если название > 12 символов */
#define SX_TITLE_FONT_LONG    30
#define SX_BODY_TOP           120  /* 59 — главная, 44 — страница альбома, 0 — плеер */
#define SX_BODY_PAD_L         15
#define SX_BODY_PAD_R         15   /* 32 — если есть полоса А–Я */
#define SX_EDGE_BACK_W        22   /* зона свайпа «назад» у левого края */

/* ---------- Строка списка ---------- */
#define SX_ROW_H              80   /* «Крупные строки»: 100 */
#define SX_ROW_H_BIG          100
#define SX_ROW_H_COMPACT      64   /* очередь; крупные: 84 */
#define SX_ROW_THUMB          56   /* крупные: 68; компактные: 48 */
#define SX_ROW_THUMB_R        12
#define SX_ROW_R              14
#define SX_ROW_GAP            2
#define SX_ROW_PAD_L          12
#define SX_ROW_INNER_GAP      14
#define SX_ROW_TITLE_FONT     26
#define SX_ROW_SUB_FONT       20
#define SX_ROW_TRAIL_W        72   /* кнопка ☆ / шеврон справа */

/* ---------- Карточка настроек ---------- */
#define SX_CARD_R             16
#define SX_CARD_PAD_V         12
#define SX_CARD_PAD_H         20
#define SX_CARD_MIN_H         60
#define SX_CARD_GAP           8
#define SX_CARD_TITLE_FONT    24
#define SX_CARD_SUB_FONT      21   /* моно-подпись (кодек, сеть): 17 */
#define SX_PILL_H             52
#define SX_PILL_PAD_H         22
#define SX_PILL_FONT          22
#define SX_PILL_GAP           10
#define SX_SWITCH_W           68
#define SX_SWITCH_H           36
#define SX_SWITCH_KNOB        28
#define SX_SWATCH             52

/* ---------- Плитки (главная, Музыка, Стриминг, Сети, Ещё) ---------- */
#define SX_TILE_GAP           12
#define SX_TILE_R             20
#define SX_TILE_H_MAX_HOME    232
#define SX_TILE_H_MAX         204
#define SX_TILE_H_MIN         120  /* высота = (H − top − 88 − 15 − 2·12) / 3, в пределах */
#define SX_TILE_FONT          24

/* ---------- Мини-плеер ---------- */
#define SX_MINI_SLOT_H        88   /* отступы 6 / 12 / 10 */
#define SX_MINI_H             72
#define SX_MINI_R             20
#define SX_MINI_COVER         52
#define SX_MINI_COVER_R       10
#define SX_MINI_PLAY          48   /* белый круг */
#define SX_MINI_PROGRESS_H    3

/* ---------- Плеер (Now Playing) ---------- */
#define SX_NP_PANEL_H         312  /* обложка = H − 312 */
#define SX_NP_PANEL_R         24
#define SX_NP_TITLE_FONT      28
#define SX_NP_ARTIST_FONT     24
#define SX_NP_SEEK_TRACK_H    10
#define SX_NP_SEEK_KNOB       28   /* обводка 5 белым */
#define SX_NP_PLAY            84
#define SX_NP_SKIP            76
#define SX_NP_SIDE_BTN        72
#define SX_NP_UPNEXT_H        52

/* ---------- Слои ---------- */
#define SX_SHEET_R            28
#define SX_SHEET_ITEM_H       64
#define SX_SHEET_ITEM_FONT    24
#define SX_SHEET_TITLE_FONT   26
#define SX_TOAST_H            56
#define SX_TOAST_R            18
#define SX_TOAST_MS           3500
#define SX_VOL_OSD_W          64
#define SX_VOL_OSD_H          300
#define SX_VOL_OSD_MS         1500
#define SX_AZ_LANE_W          28
#define SX_AZ_BUBBLE          104
#define SX_CC_BTN             84

/* ---------- Жесты и время ---------- */
#define SX_LONGPRESS_MS       450
#define SX_SWIPE_LOCK_PX      10   /* после этого строка «захвачена» по горизонтали */
#define SX_SWIPE_COMMIT_PX    100
#define SX_SWIPE_MAX_PX       160
#define SX_DRAG_LOCK_PX       12   /* плеер / лист вниз */
#define SX_NP_COLLAPSE_PX     110
#define SX_SHEET_CLOSE_PX     80
#define SX_TOAST_DISMISS_PX   30
#define SX_CC_CLOSE_PX        60   /* вверх */
#define SX_MINI_SWIPE_PX      60   /* по горизонтали; вверх — 40 */
#define SX_EDGE_BACK_PX       70
#define SX_PREV_RESTART_S     3    /* ⏮ после 3 с — к началу трека */
#define SX_ANIM_NAV_MS        180  /* cubic-bezier(.2,.8,.2,1) ≈ lv_anim_path_ease_out */
#define SX_ANIM_SHEET_MS      200
#define SX_ANIM_ROW_SNAP_MS   150

/* ---------- Нейтральная палитра (окраска от обложки выключена) ---------- */
#define SX_D_BG       0x242424
#define SX_D_SURFACE  0x303030
#define SX_D_PRESS    0x404040
#define SX_D_TEXT     0xFFFFFF
#define SX_D_TEXT2    0xA0A0A0
#define SX_L_BG       0xECEDF0
#define SX_L_SURFACE  0xFFFFFF
#define SX_L_PRESS    0xDDDFE4
#define SX_L_TEXT     0x323232
#define SX_L_TEXT2    0x737373

/* ---------- Фиксированные цвета ---------- */
#define SX_TEXT_ON_DARK     0xF1EFE9
#define SX_TEXT_ON_LIGHT    0x25232A
#define SX_STAR_ON_DARK     0xF6D32D
#define SX_STAR_ON_LIGHT    0xB07800
#define SX_STAR_OFF         0x969696
#define SX_CHAIN_OK         0x8FE3C0
#define SX_CHAIN_WARN       0xF6C548
#define SX_DANGER_BG        0xE01B24
#define SX_DANGER_TEXT      0xE5484D
#define SX_TOAST_BG         0xF1EFE9
#define SX_TOAST_TEXT       0x17161A
#define SX_TOAST_ACTION     0x245A8A
#define SX_CC_BG            0x303030
#define SX_CC_BTN_OFF       0x404040
/* Волосяная линия 1 px по верху листа и мини-плеера:
 * тёмная тема — белый 16 % (opa 41), светлая — чёрный 12 % (opa 31). */
#define SX_HAIRLINE_DARK_OPA  41
#define SX_HAIRLINE_LIGHT_OPA 31

/* Пресеты акцента (вместо «от обложки») */
#define SX_ACC_BLUE    0x3584E4
#define SX_ACC_ORANGE  0xFF7800
#define SX_ACC_YELLOW  0xE5A50A   /* текст на нём — 0x1D1D1D */
#define SX_ACC_GREEN   0x2EC27E
#define SX_ACC_PURPLE  0x9141AC
#define SX_ACC_PINK    0xD56199

/* ---------- Палитра от обложки: {L, C} для оттенка h ---------- */
typedef struct { float L, C; } sx_lc_t;
/* Тёмная тема */
static const sx_lc_t SX_T_D_BG       = {0.20f, 0.032f};
static const sx_lc_t SX_T_D_SURFACE  = {0.26f, 0.040f};
static const sx_lc_t SX_T_D_PRESS    = {0.31f, 0.046f};
static const sx_lc_t SX_T_D_TEXT2    = {0.76f, 0.030f};
static const sx_lc_t SX_T_D_ACCENT   = {0.84f, 0.100f};
static const sx_lc_t SX_T_D_ON_ACC   = {0.20f, 0.040f};
static const sx_lc_t SX_T_D_SWIPE_L  = {0.42f, 0.070f};  /* ← «В конец очереди» */
/* Светлая тема */
static const sx_lc_t SX_T_L_BG       = {0.955f, 0.015f};
static const sx_lc_t SX_T_L_PRESS    = {0.90f, 0.025f};
static const sx_lc_t SX_T_L_TEXT2    = {0.45f, 0.030f};
static const sx_lc_t SX_T_L_ACCENT   = {0.50f, 0.140f};  /* текст на нём — белый */
static const sx_lc_t SX_T_L_SWIPE_L  = {0.82f, 0.060f};
/* Всегда тёмные поверхности (плеер, мини-плеер, страница альбома) */
static const sx_lc_t SX_T_NP_ACCENT  = {0.84f, 0.100f};
static const sx_lc_t SX_T_NP_INK     = {0.30f, 0.070f};  /* значок на белой кнопке ▶ */
static const sx_lc_t SX_T_MINI_BG    = {0.30f, 0.060f};
static const sx_lc_t SX_T_NP_GRAD_TOP= {0.40f, 0.090f};  /* фон плеера: сверху */
static const sx_lc_t SX_T_NP_GRAD_BOT= {0.18f, 0.035f};  /*            снизу  */
static const sx_lc_t SX_T_NP_PANEL_T = {0.28f, 0.065f};  /* панель управления: сверху */
static const sx_lc_t SX_T_NP_PANEL_B = {0.14f, 0.030f};  /*                    снизу  */
static const sx_lc_t SX_T_AL_GRAD_0  = {0.52f, 0.100f};  /* альбом: 0 px (и статус-бар) */
static const sx_lc_t SX_T_AL_GRAD_1  = {0.38f, 0.075f};  /*         380 px */
static const sx_lc_t SX_T_AL_GRAD_2  = {0.22f, 0.035f};  /*         900 px */
static const sx_lc_t SX_T_COVER_TONE = {0.50f, 0.090f};  /* плашка до загрузки миниатюры */

#endif
