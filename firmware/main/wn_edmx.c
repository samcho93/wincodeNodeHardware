// wn_edmx.c — eDMX node: two DMX512 ports on UART1 and UART2, each with an RS-485 transceiver.
//  Out (network → DMX): the port sends its universe at `rate` frames a second — break (≥ 92 µs) by inverting the TX
//  line, mark-after-break, start code 0 and 512 slots at 250 kbit/s 8N2. Several senders on one universe are merged
//  (HTP: highest value per channel, LTP: latest change). After WN_LOSS_MS without a sender: hold the last frame, or
//  blackout. A test pattern or "identify" overrides the network data.
//  In (DMX → network): the port listens on its line (a break starts a frame) and sends every frame as ArtDmx or sACN.
//  RDM is not built yet: `rdm` is kept in the configuration and ignored.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "wn.h"

static const char *TAG = "wn_edmx";
static const struct { uart_port_t uart; int tx, rx, de; } PORT[WN_PORTS] = {
    { UART_NUM_1, CONFIG_WN_DMX1_TX_GPIO, CONFIG_WN_DMX1_RX_GPIO, CONFIG_WN_DMX1_DE_GPIO },
    { UART_NUM_2, CONFIG_WN_DMX2_TX_GPIO, CONFIG_WN_DMX2_RX_GPIO, CONFIG_WN_DMX2_DE_GPIO },
};
static const char *PATTERNS[] = { "off", "red", "green", "blue", "white", "chase", "rainbow" };
typedef struct { bool enabled, in, htp, blackout, sacn; int universe, rate; } port_cfg_t;
static port_cfg_t s_cfg[WN_PORTS];
static char s_test[WN_PORTS][10];
static volatile const char *s_state[WN_PORTS];
static volatile int s_sources[WN_PORTS];
static QueueHandle_t s_rxq[WN_PORTS];

static void read_port(int i) {
    char p[32]; port_cfg_t *c = &s_cfg[i];
    snprintf(p, sizeof p, "ports.%d.enabled", i); c->enabled = wn_cfg_bool(p, true);
    snprintf(p, sizeof p, "ports.%d.dir", i); c->in = !strcmp(wn_cfg_str(p, "out"), "in");
    snprintf(p, sizeof p, "ports.%d.merge", i); c->htp = strcmp(wn_cfg_str(p, "htp"), "ltp") != 0;
    snprintf(p, sizeof p, "ports.%d.onLoss", i); c->blackout = !strcmp(wn_cfg_str(p, "hold"), "blackout");
    snprintf(p, sizeof p, "ports.%d.protocol", i); c->sacn = !strcmp(wn_cfg_str(p, "artnet"), "sacn");
    snprintf(p, sizeof p, "ports.%d.universe", i); c->universe = (int)wn_cfg_num(p, i);
    snprintf(p, sizeof p, "ports.%d.rate", i); c->rate = (int)wn_cfg_num(p, 40);
}
static void listen_all(void) {
    uint16_t u[WN_PORTS]; int n = 0;
    for (int i = 0; i < WN_PORTS; i++) { read_port(i); if (s_cfg[i].enabled && !s_cfg[i].in) u[n++] = s_cfg[i].universe; }
    wn_dmx_listen(u, n);
}
static void pattern(const char *p, uint8_t *out, double t) {
    int k = (int)(t * 8) % 512;
    for (int c = 0; c < 512; c++)
        out[c] = !strcmp(p, "white") ? 255 : !strcmp(p, "red") ? (c % 3 == 0) * 255 : !strcmp(p, "green") ? (c % 3 == 1) * 255 : !strcmp(p, "blue") ? (c % 3 == 2) * 255
               : !strcmp(p, "chase") ? (c == k) * 255 : !strcmp(p, "off") ? 0 : (uint8_t)((c * 4 + (int)(t * 60)) % 256);
}
/** one DMX frame on the line: break, mark-after-break, start code + 512 slots */
static void send_frame(int i, const uint8_t *slots) {
    static uint8_t buf[WN_PORTS][513];
    uart_port_t u = PORT[i].uart;
    uart_wait_tx_done(u, pdMS_TO_TICKS(40));
    uart_set_line_inverse(u, UART_SIGNAL_TXD_INV); esp_rom_delay_us(120);    // break: the line held low
    uart_set_line_inverse(u, UART_SIGNAL_INV_DISABLE); esp_rom_delay_us(16); // mark after break
    buf[i][0] = 0; memcpy(buf[i] + 1, slots, 512);
    uart_write_bytes(u, buf[i], 513);
}
static void port_task(void *arg) {
    int i = (int)(intptr_t)arg; uint32_t gen = 0; static uint8_t out[WN_PORTS][512], rx[WN_PORTS][513]; bool was_in = false; int got = -1;
    for (;;) {
        if (gen != wn_config_generation()) {
            gen = wn_config_generation(); if (i == 0) listen_all(); else read_port(i);
            gpio_set_level(PORT[i].de, s_cfg[i].in ? 0 : 1);                  // transceiver direction
            if (s_cfg[i].in != was_in) { uart_flush_input(PORT[i].uart); xQueueReset(s_rxq[i]); was_in = s_cfg[i].in; got = -1; }
        }
        port_cfg_t *c = &s_cfg[i];
        if (!c->enabled) { s_state[i] = "off"; s_sources[i] = 0; vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (c->in) {                                                          // DMX in: a break starts a frame
            s_state[i] = "input"; s_sources[i] = 0; uart_event_t e;
            if (xQueueReceive(s_rxq[i], &e, pdMS_TO_TICKS(100)) != pdTRUE) continue;
            if (e.type == UART_BREAK) { if (got > 1) wn_dmx_send((uint16_t)c->universe, c->sacn, rx[i] + 1); got = 0; }
            else if (e.type == UART_DATA && got >= 0) { int n = uart_read_bytes(PORT[i].uart, rx[i] + got, 513 - got > (int)e.size ? e.size : 513 - got, 0); if (n > 0) got += n; if (got >= 513) { if (rx[i][0] == 0) wn_dmx_send((uint16_t)c->universe, c->sacn, rx[i] + 1); got = -1; } }
            else if (e.type == UART_FIFO_OVF || e.type == UART_BUFFER_FULL) { uart_flush_input(PORT[i].uart); xQueueReset(s_rxq[i]); got = -1; }
            continue;
        }
        double t = esp_timer_get_time() / 1e6;
        if (s_test[i][0]) { pattern(s_test[i], out[i], t); s_state[i] = "test"; }
        else if (wn_cfg_bool("identify", false)) { pattern((int)(t * 3) % 2 ? "white" : "off", out[i], t); s_state[i] = "test"; }
        else { uint8_t m[512]; int live = wn_dmx_merge((uint16_t)c->universe, c->htp, m); s_sources[i] = live;
            if (live) { memcpy(out[i], m, 512); s_state[i] = "live"; } else if (c->blackout) { memset(out[i], 0, 512); s_state[i] = "blackout"; } else s_state[i] = "hold"; }
        send_frame(i, out[i]);
        int rate = c->rate < 1 ? 1 : c->rate > 44 ? 44 : c->rate;
        vTaskDelay(pdMS_TO_TICKS(1000 / rate > 23 ? 1000 / rate - 23 : 1));    // a frame takes about 23 ms on the wire
    }
}

void wn_device_start(void) {
    for (int i = 0; i < WN_PORTS; i++) {
        uart_config_t u = { .baud_rate = 250000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_2, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
        ESP_ERROR_CHECK(uart_driver_install(PORT[i].uart, 1024, 1024, 20, &s_rxq[i], 0));
        ESP_ERROR_CHECK(uart_param_config(PORT[i].uart, &u));
        ESP_ERROR_CHECK(uart_set_pin(PORT[i].uart, PORT[i].tx, PORT[i].rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        gpio_reset_pin(PORT[i].de); gpio_set_direction(PORT[i].de, GPIO_MODE_OUTPUT); gpio_set_level(PORT[i].de, 1);
        s_state[i] = "off";
        xTaskCreatePinnedToCore(port_task, "wn_dmxport", 4096, (void *)(intptr_t)i, 7, NULL, 1);
    }
    ESP_LOGI(TAG, "%d DMX ports", WN_PORTS);
}

cJSON *wn_device_defaults(void) {
    cJSON *c = cJSON_CreateObject(), *ports = cJSON_CreateArray();
    for (int i = 0; i < WN_PORTS; i++) { cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject(p, "enabled", true); cJSON_AddStringToObject(p, "dir", "out"); cJSON_AddStringToObject(p, "protocol", "artnet"); cJSON_AddNumberToObject(p, "universe", i);
        cJSON_AddStringToObject(p, "merge", "htp"); cJSON_AddNumberToObject(p, "rate", 40); cJSON_AddStringToObject(p, "onLoss", "hold"); cJSON_AddBoolToObject(p, "rdm", true);
        cJSON_AddItemToArray(ports, p); }
    cJSON_AddItemToObject(c, "ports", ports); cJSON_AddItemToObject(c, "reroute", cJSON_CreateArray());
    return c;
}
static void range(cJSON *o, const char *key, double lo, double hi, double def) {
    cJSON *v = cJSON_GetObjectItem(o, key); double x = cJSON_IsNumber(v) ? v->valuedouble : def; x = x < lo ? lo : x > hi ? hi : x;
    if (v) cJSON_ReplaceItemInObject(o, key, cJSON_CreateNumber(x)); else cJSON_AddNumberToObject(o, key, x);
}
static void one_of(cJSON *o, const char *key, const char *a, const char *b) {
    cJSON *v = cJSON_GetObjectItem(o, key); if (cJSON_IsString(v) && (!strcmp(v->valuestring, a) || !strcmp(v->valuestring, b))) return;
    if (v) cJSON_ReplaceItemInObject(o, key, cJSON_CreateString(a)); else cJSON_AddStringToObject(o, key, a);
}
void wn_device_clamp(cJSON *c) {
    cJSON *ports = cJSON_GetObjectItem(c, "ports"); if (!cJSON_IsArray(ports)) return;
    while (cJSON_GetArraySize(ports) > WN_PORTS) cJSON_DeleteItemFromArray(ports, WN_PORTS);
    cJSON *p; cJSON_ArrayForEach(p, ports) { range(p, "universe", 0, 63999, 0); range(p, "rate", 1, 44, 40); one_of(p, "dir", "out", "in"); one_of(p, "protocol", "artnet", "sacn"); one_of(p, "merge", "htp", "ltp"); one_of(p, "onLoss", "hold", "blackout"); }
    if (!cJSON_IsArray(cJSON_GetObjectItem(c, "reroute"))) cJSON_ReplaceItemInObject(c, "reroute", cJSON_CreateArray());
}
void wn_device_status(cJSON *s, cJSON *alarms) {
    cJSON *ports = cJSON_CreateArray(); int nodata = -1;
    for (int i = 0; i < WN_PORTS; i++) {
        uint32_t fps = 0; uint8_t level = 0; if (!s_cfg[i].in) wn_dmx_stat((uint16_t)s_cfg[i].universe, &fps, &level);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject(p, "enabled", s_cfg[i].enabled); cJSON_AddNumberToObject(p, "universe", s_cfg[i].universe); cJSON_AddNumberToObject(p, "fps", fps); cJSON_AddNumberToObject(p, "level", level);
        if (s_test[i][0]) cJSON_AddStringToObject(p, "test", s_test[i]); else cJSON_AddNullToObject(p, "test");
        cJSON_AddNumberToObject(p, "sources", s_sources[i]); cJSON_AddStringToObject(p, "state", (const char *)s_state[i]);
        cJSON_AddItemToArray(ports, p);
        if (s_cfg[i].enabled && !s_cfg[i].in && !fps && nodata < 0) nodata = s_cfg[i].universe;
    }
    cJSON_AddItemToObject(s, "ports", ports);
    if (nodata >= 0) { cJSON *a = cJSON_CreateObject(); char t[48]; snprintf(t, sizeof t, "No data on universe %d", nodata); cJSON_AddStringToObject(a, "code", "nodata"); cJSON_AddStringToObject(a, "text", t); cJSON_AddItemToArray(alarms, a); }
}
bool wn_device_action(const char *action, const cJSON *b, const char **error) {
    if (strcmp(action, "test")) return false;
    const cJSON *port = cJSON_GetObjectItem(b, "port"), *pat = cJSON_GetObjectItem(b, "pattern"); int i = cJSON_IsNumber(port) ? port->valueint : 0;
    if (i < 0 || i >= WN_PORTS) { *error = "no such port"; return true; }
    if (cJSON_IsString(pat)) for (size_t k = 0; k < sizeof PATTERNS / sizeof PATTERNS[0]; k++) if (!strcmp(pat->valuestring, PATTERNS[k])) { strlcpy(s_test[i], k ? PATTERNS[k] : "", sizeof s_test[i]); return true; }
    *error = "unknown pattern"; return true;
}
