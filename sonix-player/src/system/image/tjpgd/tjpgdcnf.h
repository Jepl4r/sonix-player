/* Configuration of TJpgDec -- local copy, NOT the one in lvgl/.
 *
 * The only difference from LVGL's: JD_USE_SCALE is ON. Scaled decoding is the
 * whole point of vendoring this: a 3000x3000 cover decodes straight to
 * 375x375 at 1/8 scale in a few hundred kilobytes, instead of needing a 45 MB
 * full-resolution pass that a 64 MB device cannot afford. It is how the stock
 * player copes with poster-sized artwork on the same hardware. */

#define JD_SZBUF        512
/* Output pixel format: 0 = RGB888 */
#define JD_FORMAT       0
/* 1/2, 1/4, 1/8 output scaling -- see above */
#define JD_USE_SCALE    1
/* Saturation table -- faster on this CPU than per-pixel clipping */
#define JD_TBLCLIP      1
#define JD_FASTDECODE   1
