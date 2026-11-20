// wn_pixelnode.c — pixel node: four WS281x / SK6812 outputs fed from Art-Net / sACN universes.
//  Every output has its own task, so a long strip does not hold the others up. A test pattern (action "test") or
//  "identify" overrides the network data on a port.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "wn.h"

static const int GPIO[WN_PORTS] = { CONFIG_WN_PIX1_GPIO, CONFIG_WN_PIX2_GPIO, CONFIG_WN_PIX3_GPIO, CONFIG_WN_PIX4_GPIO };
static const char *CHIPS[] = { "WS2812B", "WS2811", "SK6812", "SK6812-RGBW", "APA102", "UCS2903" }, *ORDERS[] = { "GRB", "RGB", "BRG", "RBG", "GBR", "BGR", "GRBW", "RGBW" };
static const char *PATTERNS[] = { "off", "red", "green", "blue", "white", "chase", "rainbow" };
static char s_test[WN_PORTS][10];          // test pattern per port, "" = follow the network
static wn_strip_cfg_t s_cfg[WN_PORTS]; static bool s_enabled[WN_PORTS];
static char s_chip[WN_PORTS][14], s_order[WN_PORTS][6];

/** read port i from the configuration */
static void read_port(int i) {
    char p[32]; wn_strip_cfg_t *c = &s_cfg[i];
#define NUM(key, def) (snprintf(p, sizeof p, "ports.%d." key, i), wn_cfg_num(p, def))
    snprintf(p, sizeof p, "ports.%d.enabled", i); s_enabled[i] = wn_cfg_bool(p, true);
    snprintf(p, sizeof p, "ports.%d.reverse", i); c->reverse = wn_cfg_bool(p, false);
    snprintf(p, sizeof p, "ports.%d.chip", i); strlcpy(s_chip[i], wn_cfg_str(p, "WS2812B"), sizeof s_chip[i]);
    snprintf(p, sizeof p, "ports.%d.order", i); strlcpy(s_order[i], wn_cfg_str(p, "GRB"), sizeof s_order[i]);
    c->gpio = GPIO[i]; c->chip = s_chip[i]; c->order = s_order[i];
    c->pixels = (int)NUM("pixels", 170); c->group = (int)NUM("group", 1); c->limit = (int)NUM("limit", 100); c->gamma = NUM("gamma", 2.2); c->universe = (int)NUM("universe", i); c->start_channel = (int)NUM("startChannel", 1);
#undef NUM
}
/** tell the receiver which universes all ports need */
static void listen_all(void) {
    static uint16_t u[40]; int n = 0;
    for (int i = 0; i < WN_PORTS; i++) { read_port(i); if (s_enabled[i]) for (int k = 0; k < wn_strip_universes(&s_cfg[i]) && n < 40; k++) u[n++] = s_cfg[i].universe + k; }
    wn_dmx_listen(u, n);
}
static void port_task(void *arg) {
    int i = (int)(intptr_t)arg; wn_strip_t *s = NULL; uint32_t gen = 0;
    for (;;) {
        if (gen != wn_config_generation()) { gen = wn_config_generation(); wn_strip_free(s); s = NULL; if (i == 0) listen_all(); else read_port(i); if (s_enabled[i]) s = wn_strip_new(&s_cfg[i]); }
        double t = esp_timer_get_time() / 1e6;
        if (s_test[i][0]) wn_strip_show_pattern(s, s_test[i], t);
        else if (wn_cfg_bool("identify", false)) wn_strip_show_pattern(s, (int)(t * 3) % 2 ? "white" : "off", t);
        else wn_strip_show_dmx(s);
        vTaskDelay(pdMS_TO_TICKS(25));                        // 40 frames a second at most; a long strip takes longer by itself
    }
}
void wn_device_start(void) { for (int i = 0; i < WN_PORTS; i++) xTaskCreatePinnedToCore(port_task, "wn_port", 4096, (void *)(intptr_t)i, 6, NULL, 1); }

cJSON *wn_device_defaults(void) {
    cJSON *c = cJSON_CreateObject(), *ports = cJSON_CreateArray();
    cJSON_AddStringToObject(c, "protocol", "artnet");
    for (int i = 0; i < WN_PORTS; i++) { cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject(p, "enabled", true); cJSON_AddStringToObject(p, "chip", "WS2812B"); cJSON_AddStringToObject(p, "order", "GRB"); cJSON_AddNumberToObject(p, "pixels", 170); cJSON_AddNumberToObject(p, "universe", i);
        cJSON_AddNumberToObject(p, "startChannel", 1); cJSON_AddNumberToObject(p, "group", 1); cJSON_AddBoolToObject(p, "reverse", false); cJSON_AddNumberToObject(p, "limit", 100); cJSON_AddNumberToObject(p, "gamma", 2.2);
        cJSON_AddItemToArray(ports, p); }
    cJSON_AddItemToObject(c, "ports", ports);
    return c;
}
static void range(cJSON *o, const char *key, double lo, double hi, double def) {
    cJSON *v = cJSON_GetObjectItem(o, key); double x = cJSON_IsNumber(v) ? v->valuedouble : def; x = x < lo ? lo : x > hi ? hi : x;
    if (v) cJSON_ReplaceItemInObject(o, key, cJSON_CreateNumber(x)); else cJSON_AddNumberToObject(o, key, x);
}
static void member(cJSON *o, const char *key, const char **list, int n) {
    cJSON *v = cJSON_GetObjectItem(o, key); if (cJSON_IsString(v)) for (int i = 0; i < n; i++) if (!strcmp(v->valuestring, list[i])) return;
    if (v) cJSON_ReplaceItemInObject(o, key, cJSON_CreateString(list[0])); else cJSON_AddStringToObject(o, key, list[0]);
}
void wn_device_clamp(cJSON *c) {
    const char *protocols[] = { "artnet", "sacn" }; member(c, "protocol", protocols, 2);
    cJSON *ports = cJSON_GetObjectItem(c, "ports"); if (!cJSON_IsArray(ports)) return;
    while (cJSON_GetArraySize(ports) > WN_PORTS) cJSON_DeleteItemFromArray(ports, WN_PORTS);   // this board has four outputs
    cJSON *p; cJSON_ArrayForEach(p, ports) { range(p, "pixels", 0, 4096, 170); range(p, "group", 1, 64, 1); range(p, "universe", 0, 63999, 0); range(p, "startChannel", 1, 512, 1); range(p, "limit", 1, 100, 100); range(p, "gamma", 1.0, 3.0, 2.2);
        member(p, "chip", CHIPS, 6); member(p, "order", ORDERS, 8); }
}
void wn_device_status(cJSON *s, cJSON *alarms) {
    cJSON *ports = cJSON_CreateArray(); bool nodata = false; int which = 0;
    for (int i = 0; i < WN_PORTS; i++) {
        wn_strip_cfg_t *c = &s_cfg[i]; uint32_t fps = 0; uint8_t level = 0; wn_dmx_stat(c->universe, &fps, &level);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject(p, "enabled", s_enabled[i]); cJSON_AddNumberToObject(p, "universe", c->universe); cJSON_AddNumberToObject(p, "universes", wn_strip_universes(c)); cJSON_AddNumberToObject(p, "pixels", c->pixels);
        cJSON_AddNumberToObject(p, "fps", fps); cJSON_AddNumberToObject(p, "level", level);
        cJSON_AddNumberToObject(p, "amps", ((int)(c->pixels * wn_chip_channels(c->chip) * 0.02 * (level / 100.0) * (c->limit / 100.0) * 10)) / 10.0);   // estimate: 20 mA per colour at full
        if (s_test[i][0]) cJSON_AddStringToObject(p, "test", s_test[i]); else cJSON_AddNullToObject(p, "test");
        cJSON_AddItemToArray(ports, p);
        if (s_enabled[i] && !fps && !nodata) { nodata = true; which = c->universe; }
    }
    cJSON_AddItemToObject(s, "ports", ports);
    if (nodata) { cJSON *a = cJSON_CreateObject(); char t[48]; snprintf(t, sizeof t, "No data on universe %d", which); cJSON_AddStringToObject(a, "code", "nodata"); cJSON_AddStringToObject(a, "text", t); cJSON_AddItemToArray(alarms, a); }
}
bool wn_device_action(const char *action, const cJSON *b, const char **error) {
    if (strcmp(action, "test")) return false;
    const cJSON *port = cJSON_GetObjectItem(b, "port"), *pat = cJSON_GetObjectItem(b, "pattern"); int i = cJSON_IsNumber(port) ? port->valueint : 0;
    if (i < 0 || i >= WN_PORTS) { *error = "no such port"; return true; }
    if (cJSON_IsString(pat)) for (size_t k = 0; k < sizeof PATTERNS / sizeof PATTERNS[0]; k++) if (!strcmp(pat->valuestring, PATTERNS[k])) { strlcpy(s_test[i], k ? PATTERNS[k] : "", sizeof s_test[i]); return true; }
    *error = "unknown pattern"; return true;
}
