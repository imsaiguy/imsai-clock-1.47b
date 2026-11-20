// IMSAI display + jelly clock for the Waveshare ESP32-S3-LCD-1.47B (ST7789, no touch), landscape 320x172.
// Three screens, changed by pressing the BOOT button (GPIO 0, the only switch a program can read):
//   1. the "ImsaiGuy" title scene          (RGB LED green)
//   2. the IMSAI 8080 front panel photo with its 44 LEDs animated from a recording of a real machine (RGB LED red)
//   3. the real-time clock, glossy "jelly" digits in a red neon frame (RGB LED blue)
// After a reset it starts on the title scene. No graphics library: only the ESP-IDF esp_lcd ST7789 driver.
//
// The board has no battery-backed clock. The time is set over USB serial right after flashing
// (tools\flash.ps1 / tools\set_time.py send "Z<POSIX time zone>" and "T<seconds since 1970 UTC>");
// "D" makes the board print its frame buffer as hex (tools\grab_frame.py turns it into a PNG).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "fonts.h"
#include "photo.h"
#include "leds.h"
#include "jelly.h"

static const char *TAG = "imsai_clock";

// Waveshare ESP32-S3-LCD-1.47B
#define PIN_MOSI  45
#define PIN_SCK   40
#define PIN_CS    42
#define PIN_DC    41
#define PIN_RST   39
#define PIN_BL    46
#define PIN_RGB   38        // on-board addressable RGB LED (WS2812B), driven with the RMT peripheral
#define PIN_BTN   0         // BOOT button: low while pressed (10K pull-up on the board)

// Landscape: the 172x320 panel is used rotated, so 320 wide by 172 tall
#define W         320
#define H         172
#define GAP_X     0
#define GAP_Y     34        // the 172-px axis sits at offset 34 in the controller's 240-px RAM
#define FB_BYTES  (W * H * sizeof(uint16_t))

_Static_assert(PHOTO_W == W && PHOTO_H == H, "photo.h must match the screen size");

// ------------------------------------------------------------------------------
// Frame buffers: RGB565, byte-swapped because the SPI bus sends the high byte first.
//   fb          what is sent to the display (internal RAM, DMA-capable)
//   scene_store the finished title scene, clock_store the clock's neon background (both in PSRAM,
//               drawn once at start-up, so a screen change is a copy and not half a second of drawing)
// ------------------------------------------------------------------------------
static uint16_t *fb, *scene_store, *clock_store;

static inline uint16_t pack(float r, float g, float b)
{
    r = fminf(fmaxf(r, 0.0f), 255.0f);
    g = fminf(fmaxf(g, 0.0f), 255.0f);
    b = fminf(fmaxf(b, 0.0f), 255.0f);
    uint16_t c = (uint16_t)((((int)(r + 0.5f) >> 3) << 11) | (((int)(g + 0.5f) >> 2) << 5) | ((int)(b + 0.5f) >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

static inline void unpack(uint16_t v, float *r, float *g, float *b)
{
    uint16_t c = (uint16_t)((v >> 8) | (v << 8));
    *r = ((c >> 11) & 31) * (255.0f / 31.0f);
    *g = ((c >> 5) & 63) * (255.0f / 63.0f);
    *b = (c & 31) * (255.0f / 31.0f);
}

static uint16_t *target;    // buffer the drawing helpers (blend and friends) write to

static inline void blend(int x, int y, float a, float r, float g, float b)   // colour over the pixel with coverage a (0..1)
{
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    float orr, og, ob;
    unpack(target[y * W + x], &orr, &og, &ob);
    target[y * W + x] = pack(orr + (r - orr) * a, og + (g - og) * a, ob + (b - ob) * a);
}

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// ------------------------------------------------------------------------------
// Colors: a metallic gold that shimmers along the frame
// ------------------------------------------------------------------------------
static void gold(float t, float *r, float *g, float *b)
{
    float k = 0.5f + 0.5f * sinf(t * 7.0f);        // 0..1 sheen
    *r = 150.0f + (255.0f - 150.0f) * k;
    *g = 108.0f + (232.0f - 108.0f) * k;
    *b = 32.0f + (150.0f - 32.0f) * k;
}

// ------------------------------------------------------------------------------
// Shapes (anti-aliased using distance fields)
// ------------------------------------------------------------------------------
static float sd_rrect(float px, float py, float inset, float radius)
{
    float cx = W / 2.0f, cy = H / 2.0f;
    float hx = W / 2.0f - inset, hy = H / 2.0f - inset;
    float qx = fabsf(px - cx) - (hx - radius);
    float qy = fabsf(py - cy) - (hy - radius);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inner = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (inner < 0 ? inner : 0) - radius;
}

static void stroke_rrect(float inset, float radius, float width)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float d = sd_rrect(x + 0.5f, y + 0.5f, inset, radius);
            float a = 0.5f + width * 0.5f - fabsf(d);
            if (a <= 0) continue;
            float r, g, b;
            gold((x / (float)W + y / (float)H) * 0.5f, &r, &g, &b);
            blend(x, y, a, r, g, b);
        }
    }
}

static void dot(float cx, float cy, float rad)
{
    for (int y = (int)(cy - rad - 2); y <= (int)(cy + rad + 2); y++)
        for (int x = (int)(cx - rad - 2); x <= (int)(cx + rad + 2); x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float r, g, b;
            gold(x / (float)W, &r, &g, &b);
            blend(x, y, clamp01(0.5f + rad - d), r, g, b);
        }
}

// Thin ring, only drawn inside the inner border so it never crosses the frame
static void ring_inside(float cx, float cy, float rad, float width, float inner_inset, float inner_r)
{
    for (int y = (int)(cy - rad - 2); y <= (int)(cy + rad + 2); y++)
        for (int x = (int)(cx - rad - 2); x <= (int)(cx + rad + 2); x++) {
            if (sd_rrect(x + 0.5f, y + 0.5f, inner_inset, inner_r) > -2.5f) continue;
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float r, g, b;
            gold(x / (float)W + 0.3f, &r, &g, &b);
            blend(x, y, clamp01(0.5f + width * 0.5f - fabsf(d - rad)), r, g, b);
        }
}

static void diamond(float cx, float cy, float half)
{
    for (int y = (int)(cy - half - 2); y <= (int)(cy + half + 2); y++)
        for (int x = (int)(cx - half - 2); x <= (int)(cx + half + 2); x++) {
            float d = fabsf(x + 0.5f - cx) + fabsf(y + 0.5f - cy);
            float r, g, b;
            gold(x / (float)W + 0.6f, &r, &g, &b);
            blend(x, y, clamp01(0.5f + (half - d) * 0.7071f), r, g, b);
        }
}

// A thin horizontal line that fades out towards both ends
static void fading_line(float x0, float x1, float y, float thick, bool fade_left, bool fade_right)
{
    float len = x1 - x0;
    for (int x = (int)x0; x <= (int)x1; x++) {
        float t = (x - x0) / len;                               // 0..1 along the line
        float fade = 1.0f;
        if (fade_left) fade *= clamp01(t * 2.2f);
        if (fade_right) fade *= clamp01((1.0f - t) * 2.2f);
        for (int yy = (int)(y - 2); yy <= (int)(y + 2); yy++) {
            float r, g, b;
            gold(x / (float)W + 0.2f, &r, &g, &b);
            blend(x, yy, clamp01(0.5f + thick * 0.5f - fabsf(yy + 0.5f - y)) * fade, r, g, b);
        }
    }
}

// A centered ornament: diamond with tapering lines on both sides
static void ornament(float cx, float y, float half_len, float dia)
{
    fading_line(cx - dia - 8 - half_len, cx - dia - 8, y, 1.3f, true, false);
    fading_line(cx + dia + 8, cx + dia + 8 + half_len, y, 1.3f, false, true);
    diamond(cx, y, dia);
    dot(cx - dia - 5, y, 1.3f);
    dot(cx + dia + 5, y, 1.3f);
}

// ------------------------------------------------------------------------------
// Text with anti-aliased bitmap fonts
// ------------------------------------------------------------------------------
static float text_width(const font_t *f, const char *s)
{
    float w = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < f->first || c > f->last) c = '?';
        w += f->glyphs[c - f->first].adv16 / 16.0f;
    }
    return w;
}

static void draw_text(const font_t *f, const char *s, float x, int baseline, float r, float g, float b, float alpha)
{
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < f->first || c > f->last) c = '?';
        const glyph_t *gl = &f->glyphs[c - f->first];
        int gx = (int)lrintf(x) + gl->xoff, gy = baseline + gl->yoff;
        for (int j = 0; j < gl->h; j++)
            for (int i = 0; i < gl->w; i++)
                blend(gx + i, gy + j, f->bitmap[gl->off + j * gl->w + i] * (alpha / 255.0f), r, g, b);
        x += gl->adv16 / 16.0f;
    }
}

static void draw_text_centered(const font_t *f, const char *s, int baseline, float r, float g, float b)
{
    float x = (W - text_width(f, s)) / 2.0f;
    draw_text(f, s, x + 1.5f, baseline + 1, 8, 4, 18, 0.55f);   // soft shadow
    draw_text(f, s, x, baseline, r, g, b, 1.0f);
}

// ------------------------------------------------------------------------------
// The picture
// ------------------------------------------------------------------------------
static void render_scene(void)
{
    // Deep midnight-blue gradient with a faint glow in the middle
    for (int y = 0; y < H; y++) {
        float v = y / (float)(H - 1);
        for (int x = 0; x < W; x++) {
            float dx = (x - W / 2.0f) / (W / 2.0f), dy = (y - H / 2.0f) / (H / 2.0f);
            float glow = expf(-(dx * dx * 1.2f + dy * dy * 2.0f)) * 22.0f;
            target[y * W + x] = pack(9 + 14 * v + glow * 0.6f, 12 + 0 * v + glow * 0.7f, 36 + 12 * v + glow);
        }
    }

    // Double frame: a bold outer line and a fine inner line
    stroke_rrect(3.0f, 18.0f, 2.6f);
    stroke_rrect(9.0f, 12.0f, 1.0f);

    // Corner flourishes
    const float cx[4] = {20.0f, W - 20.0f, 20.0f, W - 20.0f};
    const float cy[4] = {20.0f, 20.0f, H - 20.0f, H - 20.0f};
    for (int i = 0; i < 4; i++) {
        dot(cx[i], cy[i], 2.3f);
        ring_inside(cx[i], cy[i], 6.0f, 1.0f, 9.0f, 12.0f);
        ring_inside(cx[i], cy[i], 10.0f, 0.8f, 9.0f, 12.0f);
    }

    // Ornaments top and bottom, diamonds on both sides
    ornament(W / 2.0f, 24.0f, 70.0f, 4.5f);
    ornament(W / 2.0f, H - 24.0f, 70.0f, 4.5f);
    diamond(17.0f, H / 2.0f, 4.0f);
    diamond(W - 17.0f, H / 2.0f, 4.0f);

    // Title, divider and subtitle
    draw_text_centered(&font_title, "ImsaiGuy", 92, 255, 241, 208);
    fading_line(60.0f, W - 60.0f, 106.0f, 1.0f, true, true);
    diamond(W / 2.0f, 106.0f, 3.2f);
    draw_text_centered(&font_sub, "ESP32-S3  -  Waveshare 1.47B", 134, 214, 176, 96);
}

// ------------------------------------------------------------------------------
// Animated front-panel LEDs
// The photo has all LEDs switched off. Every frame, a glow is added on top of each LED site.
// The glow's colors come from a lit LED in the original photo (difference to a dark LED);
// the brightness of every LED in every frame was measured from a recording of a real IMSAI.
// ------------------------------------------------------------------------------
#define GLOW_R    7                       // glow reaches 7 px from the LED center
#define LUT_STEP  8                       // lookup entries per pixel of distance
#define LUT_N     (GLOW_R * LUT_STEP + 2)

static float glow_lut[LUT_N][3];          // RGB increase at the LED center and out to GLOW_R px

static void init_glow(void)
{
    // radial profile of a lit LED minus a dark one, in photo pixels (bin r covers radius r..r+1)
    static const float tab[12][3] = {
        {91, 203, 150}, {109, 201, 151}, {121, 186, 149}, {119, 122, 100}, {86, 36, 27}, {81, 10, 10},
        {78, 7, 10}, {60, 1, 5}, {32, 0, 0}, {18, 0, 0}, {1, 0, 0}, {0, 0, 0}};
    const float k = 320.0f / 539.0f;      // screen pixels per photo pixel
    static float raw[LUT_N][3];
    for (int i = 0; i < LUT_N; i++) {
        float rp = (i / (float)LUT_STEP) / k - 0.5f;
        for (int c = 0; c < 3; c++) {
            if (rp < 0) raw[i][c] = tab[0][c];
            else if (rp >= 11) raw[i][c] = 0;
            else { int lo = (int)rp; float f = rp - lo; raw[i][c] = tab[lo][c] * (1 - f) + tab[lo + 1][c] * f; }
        }
    }
    // soften like the photo was softened by scaling it down (Gaussian, sigma 0.45 px)
    const float sigma = 0.45f * LUT_STEP;
    for (int i = 0; i < LUT_N; i++)
        for (int c = 0; c < 3; c++) {
            float a = 0, ws = 0;
            for (int j = -8; j <= 8; j++) {
                int s = i + j; if (s < 0) s = -s;                  // mirror at the center
                float w = expf(-(j * j) / (2 * sigma * sigma));
                a += w * (s < LUT_N ? raw[s][c] : 0.0f); ws += w;
            }
            glow_lut[i][c] = a / ws;
        }
}

// Draw all LEDs for animation frame `f` into buf (which otherwise holds the plain photo)
static void draw_leds(uint16_t *buf, int f)
{
    for (int i = 0; i < LED_COUNT; i++) {            // erase what the previous frame drew here
        int cx = (int)floorf(led_xy[i][0]), cy = (int)floorf(led_xy[i][1]);
        int x0 = cx - GLOW_R, x1 = cx + GLOW_R, y0 = cy - GLOW_R, y1 = cy + GLOW_R;
        x0 = x0 < 0 ? 0 : x0;
        x1 = x1 > W - 1 ? W - 1 : x1;
        y0 = y0 < 0 ? 0 : y0;
        y1 = y1 > H - 1 ? H - 1 : y1;
        for (int y = y0; y <= y1; y++)
            memcpy(&buf[y * W + x0], &photo_rgb565[y * W + x0], (x1 - x0 + 1) * sizeof(uint16_t));
    }
    for (int i = 0; i < LED_COUNT; i++) {
        float lv = led_anim[f][i] * (1.0f / 255.0f);
        if (lv <= 0.01f) continue;
        float lx = led_xy[i][0], ly = led_xy[i][1];
        int cx = (int)floorf(lx), cy = (int)floorf(ly);
        for (int y = cy - GLOW_R; y <= cy + GLOW_R; y++) {
            if (y < 0 || y >= H) continue;
            for (int x = cx - GLOW_R; x <= cx + GLOW_R; x++) {
                if (x < 0 || x >= W) continue;
                float dx = x + 0.5f - lx, dy = y + 0.5f - ly;
                float pos = sqrtf(dx * dx + dy * dy) * LUT_STEP;
                if (pos >= GLOW_R * LUT_STEP) continue;
                int p0 = (int)pos; float fr = pos - p0;
                float r, g, b;
                unpack(buf[y * W + x], &r, &g, &b);
                r += lv * (glow_lut[p0][0] * (1 - fr) + glow_lut[p0 + 1][0] * fr);
                g += lv * (glow_lut[p0][1] * (1 - fr) + glow_lut[p0 + 1][1] * fr);
                b += lv * (glow_lut[p0][2] * (1 - fr) + glow_lut[p0 + 1][2] * fr);
                buf[y * W + x] = pack(r, g, b);
            }
        }
    }
}

// ------------------------------------------------------------------------------
// Background: a red neon tube bent into a rounded rectangle, glowing on both sides.
// Per pixel: dark midnight-blue ground (darker outside the sign), a red glow that falls off with the
// distance from the tube, the saturated red tube itself and a pale hot core along its centre.
// ------------------------------------------------------------------------------
#define TUBE_INSET    7.0f      // distance of the tube's centre line from the screen edge
#define TUBE_RADIUS   16.0f     // bend radius at the corners
#define TUBE_HALF_W   2.1f      // half the tube's thickness
#define CORE_HALF_W   0.85f     // half the thickness of the hot core

static inline uint16_t pack_dither(float r, float g, float b, int x, int y, float amount)   // ordered dither hides the banding in the glow
{
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    float t = (bayer[y & 3][x & 3] - 7.5f) / 16.0f * amount;
    return pack(r + t * 8.0f, g + t * 4.0f, b + t * 8.0f);
}

static void render_neon(uint16_t *dst)
{
    for (int y = 0; y < H; y++) {
        float v = y / (float)(H - 1);
        for (int x = 0; x < W; x++) {
            float dx = (x - W / 2.0f) / (W / 2.0f), dy = (y - H / 2.0f) / (H / 2.0f);
            float glow0 = expf(-(dx * dx * 1.2f + dy * dy * 2.0f)) * 22.0f;
            float r = 9 + 14 * v + glow0 * 0.6f, g = 12 + glow0 * 0.7f, b = 36 + 12 * v + glow0;

            float d = sd_rrect(x + 0.5f, y + 0.5f, TUBE_INSET, TUBE_RADIUS);   // < 0 inside the sign
            float dist = fabsf(d);                                              // distance from the tube's centre line
            float outside = clamp01((d - 1.5f) / 2.5f);                         // 0 inside, 1 beyond the tube
            float dark = 1.0f - 0.75f * outside;                                // the ground beyond the sign is nearly black
            r *= dark; g *= dark; b *= dark;

            float gl = 0.80f * expf(-dist / 3.0f) + 0.30f * expf(-dist / 10.0f);   // red light spilling from the tube
            gl *= (d < 0) ? 1.0f : 0.7f;
            r += 255.0f * gl; g += 24.0f * gl; b += 20.0f * gl;

            float a = clamp01(0.5f + TUBE_HALF_W - dist);                       // the tube
            r += (255.0f - r) * a; g += (48.0f - g) * a; b += (38.0f - b) * a;
            float c = clamp01(0.5f + CORE_HALF_W - dist) * 0.95f;               // its hot core
            r += (255.0f - r) * c; g += (232.0f - g) * c; b += (224.0f - b) * c;

            dst[y * W + x] = pack_dither(r, g, b, x, y, clamp01(gl * 5.0f));   // dither only where the glow is, not on the flat ground
        }
    }
}

// ------------------------------------------------------------------------------
// The digits
// ------------------------------------------------------------------------------
#define CELLS     8
static const char cell_kind[CELLS + 1] = "DD:DD:DD";            // D = digit, : = colon

static int clock_x0(void)  { return (W - CLOCK_W) / 2; }
static int clock_y0(void)  { return (H - DIGIT_H) / 2; }                // top of the digit box (sprites reach JELLY_PAD above it)
static int band_y0(void)   { int y = clock_y0() - JELLY_PAD - 1; return y < 0 ? 0 : y; }
static int band_y1(void)   { int y = clock_y0() + DIGIT_H + JELLY_PAD + 1; return y > H - 1 ? H - 1 : y; }

// "Jelly" digits: each sprite pixel holds coverage, tone, highlight and glow (see tools/gen_jelly.py).
// The colour comes from the position, like the letters of the JELLY picture: a light and a dark shade per cell.
static const uint8_t jelly_light[CELLS][3] = {
    {255, 150, 215}, {255, 170, 135}, {255, 190, 110}, {255, 215, 90}, {225, 245, 90}, {190, 240, 100}, {150, 235, 110}, {120, 235, 215} };
static const uint8_t jelly_dark[CELLS][3] = {
    {190, 15, 125},  {210, 40, 40},   {220, 90, 20},   {225, 125, 10}, {130, 180, 10}, {90, 170, 30},   {30, 145, 45},   {10, 135, 150} };

static void draw_sprite(const jglyph_t *g, int cell_x, int top, int cell, bool glow_pass)
{
    const uint8_t *lt = jelly_light[cell], *dk = jelly_dark[cell];
    const uint8_t *p = &jelly_px[g->off];
    for (int j = 0; j < g->h; j++) {
        int y = top - JELLY_PAD + j;
        for (int i = 0; i < g->w; i++, p += 4) {
            int x = cell_x + g->xoff + i;
            if (glow_pass) {
                if (p[3]) blend(x, y, p[3] * (0.45f / 255.0f), dk[0] * 0.9f, dk[1] * 0.9f, dk[2] * 0.9f);
                continue;
            }
            if (!p[0]) continue;
            float t = p[1] * (1.0f / 255.0f), hl = p[2] * (1.0f / 255.0f);
            float r = dk[0] + (lt[0] - dk[0]) * t, gg = dk[1] + (lt[1] - dk[1]) * t, b = dk[2] + (lt[2] - dk[2]) * t;
            blend(x, y, p[0] * (1.0f / 255.0f), r + (255.0f - r) * hl, gg + (255.0f - gg) * hl, b + (255.0f - b) * hl);
        }
    }
}

static void draw_time(const char *text)                            // text is "HH:MM:SS"
{
    target = fb;
    for (int y = band_y0(); y <= band_y1(); y++)                   // clean background for the whole digit band
        memcpy(&fb[y * W], &clock_store[y * W], W * sizeof(uint16_t));
    for (int pass = 0; pass < 2; pass++) {                         // glow of all cells first, then the gel bodies
        int x = clock_x0();
        for (int c = 0; c < CELLS; c++) {
            bool colon = cell_kind[c] == ':';
            const jglyph_t *g = &jelly_glyphs[colon ? 10 : text[c] - '0'];
            draw_sprite(g, x, clock_y0(), c, pass == 0);
            x += colon ? COLON_CELL_W : DIGIT_CELL_W;
        }
    }
}

// ------------------------------------------------------------------------------
// Display transfer. After the first full picture of a screen, the photo and the clock only send the
// rows that change (the rows with LEDs, or the band with the digits).
// ------------------------------------------------------------------------------
static SemaphoreHandle_t tx_done;                 // given when the last transfer has left the SPI bus
static esp_lcd_panel_handle_t panel;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(tx_done, &woken);
    return woken == pdTRUE;
}

static void wait_sent(void) { xSemaphoreTake(tx_done, portMAX_DELAY); }   // fb may only change once the last transfer is done

static void send_rows(int y0, int y1)             // rows y0..y1 of fb; call wait_sent() first
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y0, W, y1 + 1, fb + y0 * W));
}

// ------------------------------------------------------------------------------
// On-board RGB LED: shows which screen is up. This board's LED takes its bytes in R, G, B order.
// ------------------------------------------------------------------------------
#define RGB_LEVEL 24        // brightness cap per colour (0..255); the LED is very bright at full power
static rmt_channel_handle_t rgb_chan;
static rmt_encoder_handle_t rgb_enc;

static void rgb_init(void)
{
    rmt_tx_channel_config_t cc = { .gpio_num = PIN_RGB, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000,
                                   .mem_block_symbols = 64, .trans_queue_depth = 1 };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cc, &rgb_chan));
    rmt_bytes_encoder_config_t ec = { .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
                                      .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
                                      .flags.msb_first = 1 };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&ec, &rgb_enc));
    ESP_ERROR_CHECK(rmt_enable(rgb_chan));
}

static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t px[3] = { r, g, b };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(rgb_chan, rgb_enc, px, sizeof px, &tx));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(rgb_chan, portMAX_DELAY));
}

// ------------------------------------------------------------------------------
// Setting kept in flash (NVS): the POSIX time-zone string (so daylight saving works).
// tools\flash.ps1 / tools\set_time.py send it over USB serial.
// ------------------------------------------------------------------------------
#define NVS_NS "clock"
static char cfg_tz[64] = "UTC0";

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof cfg_tz;
    nvs_get_str(h, "tz", cfg_tz, &n);
    nvs_close(h);
}

static void nvs_save(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static void apply_tz(void)
{
    setenv("TZ", cfg_tz, 1);
    tzset();
}

// ------------------------------------------------------------------------------
// Time: set over USB serial right after flashing ("T"); the chip's own timer keeps it from then on.
// If nothing has set it yet, the clock starts from the firmware's build time.
// ------------------------------------------------------------------------------
static void set_time_utc(time_t secs)
{
    struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static time_t build_time(void)                                     // the build machine's local time, read in the clock's zone
{
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char m[4] = {0};
    int d, y, hh, mm, ss;
    sscanf(__DATE__, "%3s %d %d", m, &d, &y);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char *p = strstr(mon, m);
    struct tm t = { .tm_year = y - 1900, .tm_mon = p ? (int)(p - mon) / 3 : 0, .tm_mday = d, .tm_hour = hh, .tm_min = mm, .tm_sec = ss,
                    .tm_isdst = -1 };
    return mktime(&t);
}

static void dump_frame(void)                                       // "FB" row lines, then "END", all in hex
{
    char *line = malloc(2 * W * 2 + 16);
    if (!line) return;
    usb_serial_jtag_write_bytes("FBBEGIN\n", 8, pdMS_TO_TICKS(200));
    for (int y = 0; y < H; y++) {
        int n = sprintf(line, "R%03d", y);
        const uint8_t *p = (const uint8_t *)&fb[y * W];
        for (int i = 0; i < W * 2; i++) n += sprintf(line + n, "%02x", p[i]);
        line[n++] = '\n';
        int sent = 0;
        while (sent < n) {
            int chunk = n - sent > 256 ? 256 : n - sent;
            int k = usb_serial_jtag_write_bytes(line + sent, chunk, pdMS_TO_TICKS(500));
            if (k <= 0) { free(line); return; }
            sent += k;
        }
    }
    usb_serial_jtag_write_bytes("FBEND\n", 6, pdMS_TO_TICKS(200));
    free(line);
}

static void poll_serial(void)                                      // called from the main loop; never blocks
{
    static char buf[160];
    static int len;
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
        if (c == '\n' || c == '\r') {
            buf[len] = 0;
            if (len > 1 && buf[0] == 'T') {                        // "T<seconds since 1970 UTC>"
                time_t t = (time_t)strtoll(buf + 1, NULL, 10);
                set_time_utc(t);
                struct tm tm;
                localtime_r(&t, &tm);
                ESP_LOGI(TAG, "Time set to %04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
            } else if (len > 1 && buf[0] == 'Z') {                 // "Z<POSIX time-zone string>", e.g. PST8PDT,M3.2.0,M11.1.0
                strlcpy(cfg_tz, buf + 1, sizeof cfg_tz);
                nvs_save("tz", cfg_tz);
                apply_tz();
                ESP_LOGI(TAG, "Time zone set to %s", cfg_tz);
            } else if (len == 1 && buf[0] == 'D') {
                wait_sent();
                dump_frame();
                xSemaphoreGive(tx_done);
            }
            len = 0;
        } else if (len < (int)sizeof(buf) - 1) {
            buf[len++] = (char)c;
        }
    }
}

// ------------------------------------------------------------------------------
// The three screens
// ------------------------------------------------------------------------------
typedef enum { SHOW_SCENE, SHOW_PHOTO, SHOW_CLOCK, SHOW_COUNT } show_t;
static const char *const show_name[SHOW_COUNT] = { "ImsaiGuy title scene", "IMSAI front panel", "clock" };

static show_t show;
static int led_y0, led_y1;                        // screen rows that contain LED glow
static int last_k = -1;                           // photo: the LED animation frame on screen
static time_t last_sec = -1;                      // clock: the second on screen

static int led_frame_now(void)                    // the recording plays on in real time, whether or not it is on screen
{
    return (int)((esp_timer_get_time() * LED_FPS / 1000000) % LED_FRAMES);
}

static void draw_clock_now(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    char text[9];
    snprintf(text, sizeof text, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    draw_time(text);
}

static void enter_show(show_t s)                  // put a screen up in full
{
    show = s;
    wait_sent();                                  // fb may only change once the last transfer is done
    int64_t t0 = esp_timer_get_time();
    switch (s) {
    case SHOW_SCENE:
        memcpy(fb, scene_store, FB_BYTES);
        rgb_set(0, RGB_LEVEL, 0);
        break;
    case SHOW_PHOTO:
        memcpy(fb, photo_rgb565, FB_BYTES);
        last_k = led_frame_now();
        draw_leds(fb, last_k);
        rgb_set(RGB_LEVEL, 0, 0);
        break;
    default:
        memcpy(fb, clock_store, FB_BYTES);
        last_sec = time(NULL);
        draw_clock_now(last_sec);
        rgb_set(0, 0, RGB_LEVEL);
        break;
    }
    send_rows(0, H - 1);
    ESP_LOGI(TAG, "Showing %s (%d ms to prepare)", show_name[s], (int)((esp_timer_get_time() - t0) / 1000));
}

void app_main(void)
{
    fb = heap_caps_malloc(FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    scene_store = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    clock_store = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb || !scene_store || !clock_store) {
        ESP_LOGE(TAG, "Out of memory for the frame buffers (fb %p, scene %p, clock %p)", fb, scene_store, clock_store);
        return;
    }
    // Draw the title scene and the neon background once; each takes a while to render
    int64_t t0 = esp_timer_get_time();
    target = fb;
    render_scene();
    memcpy(scene_store, fb, FB_BYTES);
    render_neon(fb);
    memcpy(clock_store, fb, FB_BYTES);
    ESP_LOGI(TAG, "Rendered the scene and the neon background in %d ms", (int)((esp_timer_get_time() - t0) / 1000));

    init_glow();
    led_y0 = H;
    led_y1 = 0;
    for (int i = 0; i < LED_COUNT; i++) {
        int c = (int)floorf(led_xy[i][1]);
        if (c - GLOW_R < led_y0) led_y0 = c - GLOW_R;
        if (c + GLOW_R > led_y1) led_y1 = c + GLOW_R;
    }
    if (led_y0 < 0) led_y0 = 0;
    if (led_y1 > H - 1) led_y1 = H - 1;
    tx_done = xSemaphoreCreateBinary();
    xSemaphoreGive(tx_done);

    esp_err_t ne = nvs_flash_init();
    if (ne == ESP_ERR_NVS_NO_FREE_PAGES || ne == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ne = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ne);
    nvs_load();
    apply_tz();
    ESP_LOGI(TAG, "Saved time zone: %s (nvs_flash_init: %s)", cfg_tz, esp_err_to_name(ne));

    rgb_init();
    usb_serial_jtag_driver_config_t uc = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    uc.tx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&uc));

    gpio_config_t btn = { .pin_bit_mask = 1ULL << PIN_BTN, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    ESP_ERROR_CHECK(gpio_config(&btn));
    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_BL, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_BL, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK, .mosi_io_num = PIN_MOSI, .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = W * H * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS, .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL));
    esp_lcd_panel_dev_config_t panel_cfg = { .reset_gpio_num = PIN_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, GAP_X, GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    time_t bt = build_time(), t_boot = time(NULL);
    if (t_boot < bt) {                             // nothing has set the clock yet: start at the build time
        set_time_utc(bt);
        ESP_LOGI(TAG, "Clock was not set: starting at the build time");
    } else {
        ESP_LOGI(TAG, "Clock kept its time across the reset");
    }
    ESP_LOGI(TAG, "Free internal heap %u bytes, free PSRAM %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    enter_show(SHOW_SCENE);                        // after a reset: the title scene first

    int64_t low_since = -1;                        // when the button went down (-1: it is up)
    bool armed = true;                             // one press = one change; re-armed when the button is released
    while (true) {
        poll_serial();

        int64_t now_us = esp_timer_get_time();
        if (gpio_get_level(PIN_BTN) == 0) {
            if (low_since < 0) low_since = now_us;
            if (armed && now_us - low_since >= 30000) {    // held down for 30 ms: a real press, not contact bounce
                armed = false;
                enter_show((show_t)((show + 1) % SHOW_COUNT));
                continue;
            }
        } else {
            low_since = -1;
            armed = true;
        }

        if (show == SHOW_PHOTO) {
            int k = led_frame_now();
            if (k != last_k) {
                wait_sent();
                draw_leds(fb, k);
                send_rows(led_y0, led_y1);         // only the rows that contain LEDs
                last_k = k;
                continue;
            }
        } else if (show == SHOW_CLOCK) {
            time_t now = time(NULL);
            if (now != last_sec) {
                wait_sent();
                draw_clock_now(now);
                send_rows(band_y0(), band_y1());   // only the band with the digits
                last_sec = now;
                continue;
            }
        }
        vTaskDelay(1);
    }
}
