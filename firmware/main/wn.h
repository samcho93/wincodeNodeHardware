// wn.h — Wincode node firmware: what the modules share.
//  The firmware implements the device API of the Node Manager (wincodeNodeManager/docs/DEVICE-API.md, version 1).
//  One source tree builds two devices, chosen in menuconfig (Wincode node → Device type):
//    winch   kinetic winch: step/dir motor on a 16-bit DMX position, with a pixel lamp        (model WN-WINCH)
//    pixel   pixel node: four WS281x / SK6812 outputs                                          (model WN-PIX4)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cJSON.h"
#include "sdkconfig.h"

#define WN_API_VERSION 1
#define WN_FW_VERSION  "0.1.0"
#define WN_HW_REV      "A"
#define WN_DISCOVERY_PORT 7788
#define WN_LOSS_MS     2500            // no data for this long = signal lost

#if CONFIG_WN_DEVICE_WINCH
#define WN_TYPE  "winch"
#define WN_MODEL "WN-WINCH"
#define WN_PORTS 1
#else
#define WN_TYPE  "pixel"
#define WN_MODEL "WN-PIX4"
#define WN_PORTS 4
#endif

/* ── net (wn_net.c): Ethernet first, Wi-Fi when no cable is there ── */
void wn_net_start(void);
const char *wn_net_ip(void);           // "0.0.0.0" until an address is there
const char *wn_net_link(void);         // "100M" · "10M" · "wifi" · "down"
int wn_net_rssi(void);                 // dBm on Wi-Fi, 0 on a cable
const char *wn_id(void);               // "WN" + the last three bytes of the MAC

/* ── configuration (wn_config.c): one JSON tree, kept in NVS ── */
void wn_config_load(void);
cJSON *wn_config(void);                // the live tree — read under wn_config_lock()
void wn_config_lock(void);
void wn_config_unlock(void);
void wn_config_patch(const cJSON *patch);      // deep merge, clamp, store, apply
void wn_config_factory(bool keep_name);
double wn_cfg_num(const char *path, double fallback);          // "travel.max", "ports.2.pixels"
bool wn_cfg_bool(const char *path, bool fallback);
const char *wn_cfg_str(const char *path, const char *fallback);  // valid until the next patch
uint32_t wn_config_generation(void);   // changes with every patch: modules re-read their settings when it moves

/* ── DMX from the network (wn_dmx.c): Art-Net and sACN ── */
typedef struct { uint16_t universe; uint8_t data[512]; uint32_t frames; uint32_t fps; uint8_t level; int64_t last_us; bool used; } wn_universe_t;
void wn_dmx_start(void);
void wn_dmx_listen(const uint16_t *universes, int n);          // the universes this device needs now
bool wn_dmx_copy(uint16_t universe, uint8_t out[512]);         // false when nothing arrived in the last WN_LOSS_MS
void wn_dmx_stat(uint16_t universe, uint32_t *fps, uint8_t *level);

/* ── pixels (wn_pixels.c): one WS281x / SK6812 output ── */
typedef struct wn_strip wn_strip_t;
typedef struct { int gpio; int pixels; const char *chip; const char *order; int group; bool reverse; int limit; double gamma; int universe; int start_channel; } wn_strip_cfg_t;
wn_strip_t *wn_strip_new(const wn_strip_cfg_t *cfg);           // NULL when the output cannot be made
void wn_strip_free(wn_strip_t *s);
void wn_strip_show_dmx(wn_strip_t *s);                          // from the universes it is set to; dark when the data is gone
void wn_strip_show_pattern(wn_strip_t *s, const char *pattern, double t);   // red green blue white chase rainbow
int wn_chip_channels(const char *chip);                         // 3, or 4 for RGBW
int wn_strip_universes(const wn_strip_cfg_t *cfg);

/* ── the device itself (wn_winch.c or wn_pixelnode.c) ── */
void wn_device_start(void);
cJSON *wn_device_defaults(void);                                // type-specific part of a factory configuration
void wn_device_clamp(cJSON *cfg);                               // put every value into its range
void wn_device_status(cJSON *status, cJSON *alarms);            // add the type's status fields and alarms
bool wn_device_action(const char *action, const cJSON *body, const char **error);   // false = not one of mine

/* ── board (wn_main.c) ── */
double wn_board_vin(void);             // supply voltage
double wn_board_temp(void);            // °C from the NTC next to the power stage
void wn_board_led(bool on);
void wn_reboot_soon(void);
void wn_api_start(void);
