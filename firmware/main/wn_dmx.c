// wn_dmx.c — show data from the network: Art-Net (UDP 6454) and sACN / E1.31 (UDP 5568, multicast).
//  The device says which universes it needs (wn_dmx_listen); frames for other universes are dropped. Both protocols
//  feed the same table, whichever the configuration names: a console may send either.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/igmp.h"
#include "wn.h"

#define SLOTS 40
static const char *TAG = "wn_dmx";
static wn_universe_t s_u[SLOTS];
static SemaphoreHandle_t s_lock;
static int s_acn = -1;

static wn_universe_t *find(uint16_t universe) { for (int i = 0; i < SLOTS; i++) if (s_u[i].used && s_u[i].universe == universe) return &s_u[i]; return NULL; }
static void take(uint16_t universe, const uint8_t *data, int len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wn_universe_t *u = find(universe);
    if (u) { if (len > 512) len = 512; memcpy(u->data, data, len); if (len < 512) memset(u->data + len, 0, 512 - len); u->frames++; u->last_us = esp_timer_get_time(); }
    xSemaphoreGive(s_lock);
}
static void membership(uint16_t universe, bool join) {
    if (s_acn < 0 || universe < 1 || universe > 63999) return;
    struct ip_mreq m = { 0 }; m.imr_multiaddr.s_addr = htonl(0xEFFF0000u | universe); m.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(s_acn, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof m);
}
void wn_dmx_listen(const uint16_t *universes, int n) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SLOTS; i++) if (s_u[i].used) { bool keep = false; for (int k = 0; k < n; k++) if (universes[k] == s_u[i].universe) keep = true; if (!keep) { membership(s_u[i].universe, false); s_u[i].used = false; } }
    for (int k = 0; k < n; k++) if (!find(universes[k])) for (int i = 0; i < SLOTS; i++) if (!s_u[i].used) { memset(&s_u[i], 0, sizeof s_u[i]); s_u[i].used = true; s_u[i].universe = universes[k]; membership(universes[k], true); break; }
    xSemaphoreGive(s_lock);
}
bool wn_dmx_copy(uint16_t universe, uint8_t out[512]) {
    bool ok = false; xSemaphoreTake(s_lock, portMAX_DELAY);
    wn_universe_t *u = find(universe);
    if (u && u->last_us && esp_timer_get_time() - u->last_us < WN_LOSS_MS * 1000LL) { memcpy(out, u->data, 512); ok = true; }
    xSemaphoreGive(s_lock); return ok;
}
void wn_dmx_stat(uint16_t universe, uint32_t *fps, uint8_t *level) {
    xSemaphoreTake(s_lock, portMAX_DELAY); wn_universe_t *u = find(universe); *fps = u ? u->fps : 0; *level = u ? u->level : 0; xSemaphoreGive(s_lock);
}

static int udp_socket(uint16_t port) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s < 0) return -1;
    int on = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on); setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    return s;
}
/** ArtPollReply: lets consoles and Art-Net tools list the node */
static void poll_reply(int s, const struct sockaddr_in *to) {
    static uint8_t r[239]; memset(r, 0, sizeof r); memcpy(r, "Art-Net", 8); r[9] = 0x21;
    uint32_t ip = ipaddr_addr(wn_net_ip()); memcpy(r + 10, &ip, 4); r[14] = 0x36; r[15] = 0x19;
    r[23] = 0xd0;                                             // status: indicators normal, address set by network
    wn_config_lock(); snprintf((char *)r + 26, 18, "%s", wn_cfg_str("name", WN_MODEL)); snprintf((char *)r + 44, 64, "Wincode %s %s", WN_MODEL, wn_cfg_str("name", "")); wn_config_unlock();
    snprintf((char *)r + 108, 64, "#0001 [0000] %s fw " WN_FW_VERSION, wn_id());
    r[173] = 1; r[174] = 0x80; r[182] = 0x80;                 // one output port carrying DMX-style data
    struct sockaddr_in dst = *to; dst.sin_port = htons(6454);
    sendto(s, r, sizeof r, 0, (struct sockaddr *)&dst, sizeof dst);
}
static void artnet_task(void *arg) {
    int s = udp_socket(6454); if (s < 0) { ESP_LOGE(TAG, "Art-Net socket"); vTaskDelete(NULL); }
    static uint8_t b[600]; struct sockaddr_in from; socklen_t fl;
    for (;;) {
        fl = sizeof from; int n = recvfrom(s, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || memcmp(b, "Art-Net", 8)) continue;
        uint16_t op = b[8] | (b[9] << 8);
        if (op == 0x5000 && n > 18) { int len = (b[16] << 8) | b[17]; if (len > n - 18) len = n - 18; take((b[14] | (b[15] << 8)) & 0x7fff, b + 18, len); }
        else if (op == 0x2000) poll_reply(s, &from);
    }
}
static void sacn_task(void *arg) {
    static uint8_t b[700];
    for (;;) {
        int n = recv(s_acn, b, sizeof b, 0);
        if (n < 126 || memcmp(b + 4, "ASC-E1.17", 9) || b[21] != 4 || b[43] != 2 || b[125] != 0) continue;   // data packet, start code 0
        int count = ((b[123] << 8) | b[124]) - 1; if (count > n - 126) count = n - 126;
        take((b[113] << 8) | b[114], b + 126, count);
    }
}
static void stats_task(void *arg) {                           // once a second: frames per second and the mean level
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < SLOTS; i++) if (s_u[i].used) { wn_universe_t *u = &s_u[i]; u->fps = u->frames; u->frames = 0; uint32_t sum = 0; for (int k = 0; k < 512; k += 8) sum += u->data[k]; u->level = u->fps ? sum * 100 / (64 * 255) : 0; }
        xSemaphoreGive(s_lock);
    }
}
void wn_dmx_start(void) {
    s_lock = xSemaphoreCreateMutex();
    s_acn = udp_socket(5568);
    xTaskCreate(artnet_task, "wn_artnet", 4096, NULL, 8, NULL);
    if (s_acn >= 0) xTaskCreate(sacn_task, "wn_sacn", 4096, NULL, 8, NULL);
    xTaskCreate(stats_task, "wn_dmxstat", 3072, NULL, 3, NULL);
}
