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
/** a frame from sender `key` (Art-Net: its IP address, sACN: a hash of its CID) */
static void take(uint16_t universe, const uint8_t *data, int len, uint32_t key) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wn_universe_t *u = find(universe);
    if (u) {
        if (len > 512) len = 512;
        int64_t now = esp_timer_get_time();
        wn_source_t *s = NULL, *oldest = &u->src[0];
        for (int i = 0; i < WN_SOURCES; i++) { if (u->src[i].last_us && u->src[i].key == key) s = &u->src[i]; if (u->src[i].last_us < oldest->last_us) oldest = &u->src[i]; }
        if (!s) { s = oldest; memset(s, 0, sizeof *s); s->key = key; }        // a new sender takes the slot that was quiet longest
        for (int i = 0; i < 512; i++) { uint8_t v = i < len ? data[i] : 0; if (v != s->data[i] || !s->last_us) u->ltp[i] = v; s->data[i] = v; }   // LTP: the latest change wins
        s->last_us = now; s->frames++;
        memcpy(u->data, s->data, 512); u->frames++; u->last_us = now;
    }
    xSemaphoreGive(s_lock);
}
int wn_dmx_merge(uint16_t universe, bool htp, uint8_t out[512]) {
    int live = 0; xSemaphoreTake(s_lock, portMAX_DELAY);
    wn_universe_t *u = find(universe); int64_t now = esp_timer_get_time(); const wn_source_t *one = NULL;
    if (u) {
        memset(out, 0, 512);
        for (int i = 0; i < WN_SOURCES; i++) { const wn_source_t *s = &u->src[i]; if (!s->last_us || now - s->last_us > WN_LOSS_MS * 1000LL) continue;
            live++; one = s; if (htp) for (int c = 0; c < 512; c++) if (s->data[c] > out[c]) out[c] = s->data[c]; }
        if (live == 1) memcpy(out, one->data, 512); else if (live > 1 && !htp) memcpy(out, u->ltp, 512);
    }
    xSemaphoreGive(s_lock); return live;
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
        if (op == 0x5000 && n > 18) { int len = (b[16] << 8) | b[17]; if (len > n - 18) len = n - 18; take((b[14] | (b[15] << 8)) & 0x7fff, b + 18, len, from.sin_addr.s_addr); }
        else if (op == 0x2000) poll_reply(s, &from);
    }
}
static void sacn_task(void *arg) {
    static uint8_t b[700];
    for (;;) {
        int n = recv(s_acn, b, sizeof b, 0);
        if (n < 126 || memcmp(b + 4, "ASC-E1.17", 9) || b[21] != 4 || b[43] != 2 || b[125] != 0) continue;   // data packet, start code 0
        int count = ((b[123] << 8) | b[124]) - 1; if (count > n - 126) count = n - 126;
        uint32_t key = 2166136261u; for (int i = 22; i < 38; i++) key = (key ^ b[i]) * 16777619u;   // FNV-1a of the CID
        take((b[113] << 8) | b[114], b + 126, count, key);
    }
}
static void stats_task(void *arg) {                           // once a second: frames per second and the mean level
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < SLOTS; i++) if (s_u[i].used) { wn_universe_t *u = &s_u[i]; for (int k = 0; k < WN_SOURCES; k++) { u->src[k].fps = u->src[k].frames; u->src[k].frames = 0; } u->fps = u->frames; u->frames = 0; uint32_t sum = 0; for (int k = 0; k < 512; k += 8) sum += u->data[k]; u->level = u->fps ? sum * 100 / (64 * 255) : 0; }
        xSemaphoreGive(s_lock);
    }
}
/** DMX in → network. Art-Net: ArtDmx to the limited broadcast. sACN: E1.31 data packet to 239.255.hi.lo */
static int s_tx = -1; static uint8_t s_seq;
void wn_dmx_send(uint16_t universe, bool sacn, const uint8_t data[512]) {
    if (s_tx < 0) { s_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); int on = 1; setsockopt(s_tx, SOL_SOCKET, SO_BROADCAST, &on, sizeof on); }
    static uint8_t p[638]; struct sockaddr_in to = { .sin_family = AF_INET };
    if (!sacn) {
        memset(p, 0, 18); memcpy(p, "Art-Net", 8); p[9] = 0x50; p[11] = 14; p[12] = ++s_seq ? s_seq : ++s_seq;
        p[14] = universe & 0xff; p[15] = (universe >> 8) & 0x7f; p[16] = 2; p[17] = 0; memcpy(p + 18, data, 512);
        to.sin_port = htons(6454); to.sin_addr.s_addr = htonl(INADDR_BROADCAST); sendto(s_tx, p, 530, 0, (struct sockaddr *)&to, sizeof to); return;
    }
    static const uint8_t pre[16] = { 0, 0x10, 0, 0, 'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', 0, 0, 0 };
    memset(p, 0, 126); memcpy(p, pre, 16);
    const uint16_t root = 0x7000 | 622, frame = 0x7000 | 600, dmp = 0x7000 | 523;
    p[16] = root >> 8; p[17] = root; p[21] = 4; const char *id = wn_id(); for (int i = 0; i < 16; i++) p[22 + i] = (uint8_t)id[i % 8];   // CID from the device id
    p[38] = frame >> 8; p[39] = frame; p[43] = 2; snprintf((char *)p + 44, 64, "Wincode %s", wn_cfg_str("name", WN_MODEL)); p[108] = 100; p[111] = ++s_seq; p[113] = universe >> 8; p[114] = universe;
    p[115] = dmp >> 8; p[116] = dmp; p[117] = 2; p[118] = 0xa1; p[121] = 1; p[122] = 2; p[123] = 513 >> 8; p[124] = 513 & 0xff; p[125] = 0; memcpy(p + 126, data, 512);
    to.sin_port = htons(5568); to.sin_addr.s_addr = htonl(0xEFFF0000u | universe); sendto(s_tx, p, 638, 0, (struct sockaddr *)&to, sizeof to);
}
void wn_dmx_start(void) {
    s_lock = xSemaphoreCreateMutex();
    s_acn = udp_socket(5568);
    xTaskCreate(artnet_task, "wn_artnet", 4096, NULL, 8, NULL);
    if (s_acn >= 0) xTaskCreate(sacn_task, "wn_sacn", 4096, NULL, 8, NULL);
    xTaskCreate(stats_task, "wn_dmxstat", 3072, NULL, 3, NULL);
}
