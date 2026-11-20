// wn_pixels.c — one pixel output: WS2812B / WS2811 / SK6812 / SK6812-RGBW / UCS2903 timing on the RMT peripheral.
//  The mapping from DMX is the Node Manager's renderPixels() (vdevice.js): channels arrive as R, G, B (, W) per
//  controlled pixel; the first universe starts at start_channel, every following universe at channel 1; a universe
//  carries 170 RGB or 127 RGBW pixels. group = physical pixels per controlled pixel; limit and gamma are applied here.
//  APA102 (clocked, on SPI) is not built yet: such a port stays dark.
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "led_strip.h"
#include "wn.h"

static const char *TAG = "wn_pixels";
struct wn_strip { led_strip_handle_t h; wn_strip_cfg_t c; int cpp; bool rgbw; uint8_t lut[256]; uint8_t map[3]; };

int wn_chip_channels(const char *chip) { return chip && !strcmp(chip, "SK6812-RGBW") ? 4 : 3; }
int wn_strip_universes(const wn_strip_cfg_t *c) {
    int cpp = wn_chip_channels(c->chip), per = 510 / cpp, n = (c->pixels + c->group - 1) / (c->group > 0 ? c->group : 1);
    return n <= 0 ? 1 : (n + per - 1) / per;
}
wn_strip_t *wn_strip_new(const wn_strip_cfg_t *cfg) {
    if (cfg->pixels <= 0 || cfg->gpio < 0 || (cfg->chip && !strcmp(cfg->chip, "APA102"))) return NULL;
    wn_strip_t *s = calloc(1, sizeof *s); if (!s) return NULL;
    s->c = *cfg; if (s->c.group < 1) s->c.group = 1; s->cpp = wn_chip_channels(cfg->chip); s->rgbw = s->cpp == 4;
    // The driver is always asked for G, R, B on the wire; `order` says where the chip really wants red, green and blue,
    // so the colours are swapped before they are handed over.
    const char *o = cfg->order && strlen(cfg->order) >= 3 ? cfg->order : "GRB";
    for (int k = 0; k < 3; k++) s->map[k] = o[k] == 'R' ? 0 : o[k] == 'G' ? 1 : 2;       // wire position k carries this colour
    double g = cfg->gamma >= 1.0 ? cfg->gamma : 1.0, lim = (cfg->limit < 1 ? 1 : cfg->limit > 100 ? 100 : cfg->limit) / 100.0;
    for (int i = 0; i < 256; i++) s->lut[i] = (uint8_t)lround(pow(i / 255.0, g) * 255.0 * lim);
    led_strip_config_t sc = { .strip_gpio_num = cfg->gpio, .max_leds = cfg->pixels, .led_model = s->rgbw ? LED_MODEL_SK6812 : LED_MODEL_WS2812,
        .color_component_format = s->rgbw ? LED_STRIP_COLOR_COMPONENT_FMT_GRBW : LED_STRIP_COLOR_COMPONENT_FMT_GRB };
    led_strip_rmt_config_t rc = { .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000, .mem_block_symbols = 64 };
    if (led_strip_new_rmt_device(&sc, &rc, &s->h) != ESP_OK) { ESP_LOGE(TAG, "output on GPIO %d could not be made", cfg->gpio); free(s); return NULL; }
    led_strip_clear(s->h);
    return s;
}
void wn_strip_free(wn_strip_t *s) { if (!s) return; led_strip_clear(s->h); led_strip_del(s->h); free(s); }

/** physical pixel i gets this colour (0..255 each, before limit and gamma) */
static void put(wn_strip_t *s, int i, int r, int g, int b, int w) {
    if (s->c.reverse) i = s->c.pixels - 1 - i;
    uint8_t col[3] = { s->lut[r], s->lut[g], s->lut[b] }, wire[3] = { col[s->map[0]], col[s->map[1]], col[s->map[2]] };
    // led_strip_set_pixel(red, green, blue) sends green first: give it the wire order as (second, first, third)
    if (s->rgbw) led_strip_set_pixel_rgbw(s->h, i, wire[1], wire[0], wire[2], s->lut[w]); else led_strip_set_pixel(s->h, i, wire[1], wire[0], wire[2]);
}
void wn_strip_show_dmx(wn_strip_t *s) {
    if (!s) return;
    uint8_t d[512];
    int per = 510 / s->cpp, first = (513 - s->c.start_channel) / s->cpp; if (first > per) first = per; if (first < 0) first = 0;
    int loaded = -1; bool ok = false;
    for (int i = 0; i < s->c.pixels; i++) {
        int k = i / s->c.group, u = k < first ? 0 : 1 + (k - first) / per, ch = k < first ? s->c.start_channel - 1 + k * s->cpp : ((k - first) % per) * s->cpp;
        if (u != loaded) { loaded = u; ok = wn_dmx_copy(s->c.universe + u, d); }
        if (!ok) put(s, i, 0, 0, 0, 0); else put(s, i, d[ch], d[ch + 1], d[ch + 2], s->rgbw ? d[ch + 3] : 0);
    }
    led_strip_refresh(s->h);
}
void wn_strip_show_pattern(wn_strip_t *s, const char *p, double t) {
    if (!s) return;
    for (int i = 0; i < s->c.pixels; i++) {
        int k = i / s->c.group, r = 0, g = 0, b = 0;
        if (!strcmp(p, "red")) r = 255; else if (!strcmp(p, "green")) g = 255; else if (!strcmp(p, "blue")) b = 255; else if (!strcmp(p, "white")) r = g = b = 255;
        else if (!strcmp(p, "off")) { }
        else if (!strcmp(p, "chase")) { if ((k + (int)(t * 12)) % 8 == 0) r = g = b = 255; }
        else { double h = fmod(k / 60.0 + t * 0.3, 1.0) * 6.0; int c[3]; for (int n = 0; n < 3; n++) { double a = fmod((n == 0 ? 5 : n == 1 ? 3 : 1) + h, 6.0), v = a < 4 - a ? a : 4 - a; c[n] = (int)((v < 0 ? 0 : v > 1 ? 1 : v) * 255); } r = c[0]; g = c[1]; b = c[2]; }   // rainbow
        put(s, i, r, g, b, 0);
    }
    led_strip_refresh(s->h);
}
