// wn_api.c — what the Node Manager talks to: discovery on UDP 7788 and the HTTP API.
//    GET /api/info · GET /api/config · PUT /api/config · GET /api/status · POST /api/action
//  Every answer is JSON with CORS open; unknown fields are ignored; errors are 4xx with { ok: false, error }.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "wn.h"

static const char *TAG = "wn_api";

static cJSON *info(void) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "api", WN_API_VERSION); cJSON_AddStringToObject(j, "id", wn_id()); cJSON_AddStringToObject(j, "type", WN_TYPE); cJSON_AddStringToObject(j, "model", WN_MODEL);
    cJSON_AddStringToObject(j, "name", wn_cfg_str("name", WN_MODEL)); cJSON_AddStringToObject(j, "fw", WN_FW_VERSION); cJSON_AddStringToObject(j, "hw", WN_HW_REV); cJSON_AddNumberToObject(j, "ports", WN_PORTS);
    return j;
}
static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *j) {
    char *text = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    httpd_resp_set_status(req, status); httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store"); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS"); httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    esp_err_t r = httpd_resp_sendstr(req, text ? text : "{}"); cJSON_free(text); return r;
}
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg) {
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", false); cJSON_AddStringToObject(j, "error", msg); return send_json(req, status, j);
}
static cJSON *read_body(httpd_req_t *req) {
    int len = req->content_len; if (len <= 0) return cJSON_CreateObject(); if (len > 16384) return NULL;
    char *buf = malloc(len + 1); if (!buf) return NULL;
    int got = 0; while (got < len) { int n = httpd_req_recv(req, buf + got, len - got); if (n <= 0) { free(buf); return NULL; } got += n; }
    buf[len] = 0; cJSON *j = cJSON_Parse(buf); free(buf); return j;
}

static esp_err_t h_info(httpd_req_t *req) { return send_json(req, "200 OK", info()); }
static esp_err_t h_config_get(httpd_req_t *req) { wn_config_lock(); cJSON *c = cJSON_Duplicate(wn_config(), true); wn_config_unlock(); return send_json(req, "200 OK", c); }
static esp_err_t h_config_put(httpd_req_t *req) {
    cJSON *b = read_body(req); if (!cJSON_IsObject(b)) { cJSON_Delete(b); return send_error(req, "400 Bad Request", "bad JSON"); }
    bool net = cJSON_GetObjectItem(b, "net") != NULL;
    wn_config_patch(b); cJSON_Delete(b);
    if (net) wn_reboot_soon();                                // a new address is taken with a restart, after this answer has gone out
    return h_config_get(req);                                 // what was really stored, after clamping
}
static esp_err_t h_status(httpd_req_t *req) {
    cJSON *s = cJSON_CreateObject(), *net = cJSON_CreateObject(), *alarms = cJSON_CreateArray();
    double temp = wn_board_temp(), vin = wn_board_vin(); char text[48];
    cJSON_AddStringToObject(s, "id", wn_id()); cJSON_AddNumberToObject(s, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(s, "tempC", ((int)(temp * 10)) / 10.0); cJSON_AddNumberToObject(s, "vin", ((int)(vin * 10)) / 10.0);
    cJSON_AddNumberToObject(s, "heap", esp_get_free_heap_size()); cJSON_AddBoolToObject(s, "rebooting", false);
    cJSON_AddStringToObject(net, "ip", wn_net_ip()); cJSON_AddStringToObject(net, "link", wn_net_link());
    int rssi = wn_net_rssi(); if (rssi) cJSON_AddNumberToObject(net, "rssi", rssi); else cJSON_AddNullToObject(net, "rssi");
    cJSON_AddItemToObject(s, "net", net); cJSON_AddBoolToObject(s, "identify", wn_cfg_bool("identify", false));
    if (temp > 75) { cJSON *a = cJSON_CreateObject(); snprintf(text, sizeof text, "Temperature %.0f °C", temp); cJSON_AddStringToObject(a, "code", "overtemp"); cJSON_AddStringToObject(a, "text", text); cJSON_AddItemToArray(alarms, a); }
    if (vin > 1 && vin < 20) { cJSON *a = cJSON_CreateObject(); snprintf(text, sizeof text, "Supply %.1f V", vin); cJSON_AddStringToObject(a, "code", "undervolt"); cJSON_AddStringToObject(a, "text", text); cJSON_AddItemToArray(alarms, a); }
    wn_device_status(s, alarms);
    cJSON_AddItemToObject(s, "alarms", alarms);
    return send_json(req, "200 OK", s);
}
static esp_err_t h_action(httpd_req_t *req) {
    cJSON *b = read_body(req); const cJSON *a = b ? cJSON_GetObjectItem(b, "action") : NULL;
    if (!cJSON_IsString(a)) { cJSON_Delete(b); return send_error(req, "400 Bad Request", "bad JSON"); }
    const char *name = a->valuestring, *error = NULL; bool ok = true;
    if (!strcmp(name, "identify")) { cJSON *p = cJSON_CreateObject(); cJSON_AddBoolToObject(p, "identify", !cJSON_IsFalse(cJSON_GetObjectItem(b, "on"))); wn_config_patch(p); cJSON_Delete(p); }
    else if (!strcmp(name, "reboot")) wn_reboot_soon();
    else if (!strcmp(name, "factory")) wn_config_factory(!cJSON_IsFalse(cJSON_GetObjectItem(b, "keepName")));
    else if (!wn_device_action(name, b, &error)) { ok = false; if (!error) error = "this action is not supported by a " WN_TYPE; }
    else if (error) ok = false;
    static char msg[96]; if (!ok) snprintf(msg, sizeof msg, "%s", error);
    cJSON_Delete(b);
    if (!ok) return send_error(req, "400 Bad Request", msg);
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", true); return send_json(req, "200 OK", j);
}
static esp_err_t h_options(httpd_req_t *req) { return send_json(req, "204 No Content", cJSON_CreateObject()); }

/* ── discovery ── */
static void hello(int s, const struct sockaddr_in *to) {
    cJSON *j = info(); cJSON_AddStringToObject(j, "t", "hello"); cJSON_AddNumberToObject(j, "v", 1); cJSON_AddStringToObject(j, "ip", wn_net_ip()); cJSON_AddNumberToObject(j, "port", 80);
    char *text = cJSON_PrintUnformatted(j); cJSON_Delete(j); if (!text) return;
    sendto(s, text, strlen(text), 0, (const struct sockaddr *)to, sizeof *to); cJSON_free(text);
}
static void discovery_task(void *arg) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), on = 1; setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(WN_DISCOVERY_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) }, from;
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) { ESP_LOGE(TAG, "discovery socket"); vTaskDelete(NULL); }
    struct timeval tv = { .tv_sec = 1 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[128]; bool announced = false;
    for (;;) {
        socklen_t fl = sizeof from; int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (!announced && strcmp(wn_net_ip(), "0.0.0.0")) {   // once, when the address is there: show up without a scan
            struct sockaddr_in all = { .sin_family = AF_INET, .sin_port = htons(WN_DISCOVERY_PORT), .sin_addr.s_addr = htonl(INADDR_BROADCAST) }; hello(s, &all); announced = true;
        }
        if (n <= 0) continue;
        buf[n] = 0;
        cJSON *m = cJSON_Parse(buf); const cJSON *t = m ? cJSON_GetObjectItem(m, "t") : NULL;
        if (cJSON_IsString(t) && !strcmp(t->valuestring, "discover")) { vTaskDelay(pdMS_TO_TICKS(esp_random() % 200)); hello(s, &from); }   // spread the answers of a large rig
        cJSON_Delete(m);
    }
}

void wn_api_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); cfg.stack_size = 8192; cfg.max_uri_handlers = 12; cfg.lru_purge_enable = true; cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t srv = NULL; if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "HTTP server"); return; }
    const httpd_uri_t uris[] = {
        { .uri = "/api/info", .method = HTTP_GET, .handler = h_info }, { .uri = "/api/config", .method = HTTP_GET, .handler = h_config_get }, { .uri = "/api/config", .method = HTTP_PUT, .handler = h_config_put },
        { .uri = "/api/status", .method = HTTP_GET, .handler = h_status }, { .uri = "/api/action", .method = HTTP_POST, .handler = h_action }, { .uri = "/api/*", .method = HTTP_OPTIONS, .handler = h_options },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(srv, &uris[i]);
    xTaskCreate(discovery_task, "wn_discovery", 4096, NULL, 4, NULL);
}
