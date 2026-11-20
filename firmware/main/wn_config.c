// wn_config.c — the configuration: one JSON tree, kept as text in NVS.
//  PUT /api/config sends a patch that is deep-merged into it exactly as merge() in the Node Manager's vdevice.js does:
//  objects key by key, arrays of objects element by element, arrays of plain values replaced.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "wn.h"

static const char *TAG = "wn_config";
static cJSON *s_cfg;
static SemaphoreHandle_t s_lock;
static uint32_t s_gen = 1;

void wn_config_lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void wn_config_unlock(void) { xSemaphoreGiveRecursive(s_lock); }
cJSON *wn_config(void) { return s_cfg; }
uint32_t wn_config_generation(void) { return s_gen; }

static cJSON *defaults(void) {
    cJSON *c = wn_device_defaults(), *net = cJSON_CreateObject();
    char name[40]; snprintf(name, sizeof name, "%s %s", WN_MODEL, wn_id() + 2);
    cJSON_AddStringToObject(c, "name", name);
    cJSON_AddBoolToObject(net, "dhcp", true);
    cJSON_AddStringToObject(net, "ip", "192.168.1.50");
    cJSON_AddStringToObject(net, "mask", "255.255.255.0");
    cJSON_AddStringToObject(net, "gateway", "192.168.1.1");
    cJSON_AddItemToObject(c, "net", net);
    cJSON_AddBoolToObject(c, "identify", false);
    return c;
}

/** deep merge of `src` into `dst` (both objects) */
static void merge(cJSON *dst, const cJSON *src) {
    for (const cJSON *s = src->child; s; s = s->next) {
        if (!s->string) continue;
        cJSON *d = cJSON_GetObjectItemCaseSensitive(dst, s->string);
        if (cJSON_IsObject(s) && cJSON_IsObject(d)) { merge(d, s); continue; }
        if (cJSON_IsArray(s) && cJSON_IsArray(d) && cJSON_GetArraySize(s) && cJSON_IsObject(cJSON_GetArrayItem(s, 0))) {   // array of objects: element by element
            int i = 0;
            for (const cJSON *e = s->child; e; e = e->next, i++) {
                cJSON *de = cJSON_GetArrayItem(d, i);
                if (cJSON_IsObject(e) && cJSON_IsObject(de)) merge(de, e);
                else if (cJSON_IsObject(e) && !de) cJSON_AddItemToArray(d, cJSON_Duplicate(e, true));
            }
            continue;
        }
        cJSON *copy = cJSON_Duplicate(s, true);
        if (d) cJSON_ReplaceItemInObjectCaseSensitive(dst, s->string, copy); else cJSON_AddItemToObject(dst, s->string, copy);
    }
}

static void store(void) {
    char *text = cJSON_PrintUnformatted(s_cfg); if (!text) return;
    nvs_handle_t h;
    if (nvs_open("wn", NVS_READWRITE, &h) == ESP_OK) { if (nvs_set_str(h, "config", text) != ESP_OK || nvs_commit(h) != ESP_OK) ESP_LOGW(TAG, "could not store the configuration"); nvs_close(h); }
    cJSON_free(text);
}
static void clamp_common(cJSON *c) {
    cJSON *n = cJSON_GetObjectItem(c, "name");
    if (!cJSON_IsString(n) || !n->valuestring[0]) cJSON_ReplaceItemInObject(c, "name", cJSON_CreateString(WN_MODEL));
    else if (strlen(n->valuestring) > 32) n->valuestring[32] = 0;
    wn_device_clamp(c);
}

void wn_config_load(void) {
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_cfg = defaults();
    nvs_handle_t h; size_t len = 0;
    if (nvs_open("wn", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "config", NULL, &len) == ESP_OK && len > 1) {
            char *text = malloc(len);
            if (text && nvs_get_str(h, "config", text, &len) == ESP_OK) { cJSON *saved = cJSON_Parse(text); if (cJSON_IsObject(saved)) merge(s_cfg, saved); cJSON_Delete(saved); }   // onto the defaults: fields added by newer firmware stay
            free(text);
        }
        nvs_close(h);
    }
    clamp_common(s_cfg);
}
void wn_config_patch(const cJSON *patch) {
    if (!cJSON_IsObject(patch)) return;
    wn_config_lock(); merge(s_cfg, patch); clamp_common(s_cfg); store(); s_gen++; wn_config_unlock();
}
void wn_config_factory(bool keep_name) {
    wn_config_lock();
    cJSON *fresh = defaults(), *net = cJSON_DetachItemFromObject(s_cfg, "net"), *name = cJSON_DetachItemFromObject(s_cfg, "name");
    if (net) cJSON_ReplaceItemInObject(fresh, "net", net);                       // a reset must not cut the device off the network
    if (name && keep_name) cJSON_ReplaceItemInObject(fresh, "name", name); else cJSON_Delete(name);
    cJSON_Delete(s_cfg); s_cfg = fresh; clamp_common(s_cfg); store(); s_gen++;
    wn_config_unlock();
}

/** "a.b.2.c" → the item, or NULL */
static cJSON *at(const char *path) {
    cJSON *c = s_cfg; char key[32];
    while (c && *path) {
        const char *dot = strchr(path, '.'); size_t n = dot ? (size_t)(dot - path) : strlen(path); if (n >= sizeof key) return NULL;
        memcpy(key, path, n); key[n] = 0;
        c = cJSON_IsArray(c) ? cJSON_GetArrayItem(c, atoi(key)) : cJSON_GetObjectItemCaseSensitive(c, key);
        path += n + (dot ? 1 : 0);
    }
    return c;
}
double wn_cfg_num(const char *path, double fallback) { wn_config_lock(); cJSON *c = at(path); double v = cJSON_IsNumber(c) ? c->valuedouble : fallback; wn_config_unlock(); return v; }
bool wn_cfg_bool(const char *path, bool fallback) { wn_config_lock(); cJSON *c = at(path); bool v = cJSON_IsBool(c) ? cJSON_IsTrue(c) : fallback; wn_config_unlock(); return v; }
const char *wn_cfg_str(const char *path, const char *fallback) { wn_config_lock(); cJSON *c = at(path); const char *v = cJSON_IsString(c) ? c->valuestring : fallback; wn_config_unlock(); return v; }
