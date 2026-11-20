// wn_winch.c — kinetic winch: a step / dir motor driver follows a 16-bit DMX position; a pixel lamp hangs on the cable.
//  Position is kept in motor steps and converted with CONFIG_WN_STEPS_PER_MM_X100. Top of travel = 0 mm.
//  A hardware timer makes the step pulses; a 1 kHz task runs the trapezoid ramp (speedMax, accel) and decides the target:
//  manual control (home, jog, goto, stop) always wins over DMX until "release".
//  Safety that works with the network unplugged: limit switches and the emergency-stop chain are checked in the step
//  interrupt itself, the travel limits clamp every target, a jog ends one second after its last command, and the brake
//  closes whenever the motor is not turning. The emergency-stop chain must ALSO cut the motor supply in hardware
//  (see hardware/winch.md): firmware is never the only barrier.
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "wn.h"

static const char *TAG = "wn_winch";
#define SPM (CONFIG_WN_STEPS_PER_MM_X100 / 100.0)
#define IDLE_US 1000                    // the step timer ticks at this pace while the motor stands

static gptimer_handle_t s_timer;
static volatile int32_t s_steps;        // position, in steps from the top reference (down = positive)
static volatile uint32_t s_period_us;   // time between steps, 0 = stand still
static volatile int s_dir;              // +1 down, -1 up
static volatile bool s_ignore_soft_top; // homing runs past "0" until the switch
static double s_target, s_vel;          // mm, mm/s (down = positive)
static bool s_homed, s_homing, s_manual;
static int s_jog; static int64_t s_jog_at, s_moved_at;
static wn_strip_t *s_lamp; static uint32_t s_lamp_gen;

static inline bool low(int gpio) { return gpio >= 0 && gpio_get_level(gpio) == 0; }
static inline bool estop_open(void) { return low(CONFIG_WN_ESTOP_GPIO); }
static inline bool driver_fault(void) { return low(CONFIG_WN_ALARM_GPIO); }

static bool IRAM_ATTR on_step(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *arg) {
    uint32_t p = s_period_us; int dir = s_dir;
    bool blocked = estop_open() || (dir < 0 && low(CONFIG_WN_LIMIT_TOP_GPIO)) || (dir > 0 && low(CONFIG_WN_LIMIT_BOTTOM_GPIO)) || (dir < 0 && s_steps <= 0 && !s_ignore_soft_top);
    if (p && !blocked) { gpio_set_level(CONFIG_WN_STEP_GPIO, 1); esp_rom_delay_us(4); gpio_set_level(CONFIG_WN_STEP_GPIO, 0); s_steps += dir; }
    gptimer_alarm_config_t a = { .alarm_count = e->alarm_value + (p ? p : IDLE_US) };
    gptimer_set_alarm_action(t, &a);
    return false;
}
static double pos_mm(void) { return s_steps / SPM; }
static void brake(bool release) { if (CONFIG_WN_BRAKE_GPIO >= 0) gpio_set_level(CONFIG_WN_BRAKE_GPIO, release); }

static void motion_task(void *arg) {
    const double dt = 0.001; uint8_t d[512]; int was_dir = 0; int64_t start_at = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
        static uint32_t gen; static double min, max = 5000, vmax = 800, acc = 600; static int uni, addr = 1; static bool invert, loss_home;
        if (gen != wn_config_generation()) { gen = wn_config_generation(); min = wn_cfg_num("travel.min", 0); max = wn_cfg_num("travel.max", 5000); vmax = wn_cfg_num("speedMax", 800); acc = wn_cfg_num("accel", 600);
            uni = (int)wn_cfg_num("universe", 0); addr = (int)wn_cfg_num("address", 1); invert = wn_cfg_bool("invert", false); loss_home = !strcmp(wn_cfg_str("onLoss", "hold"), "home"); }   // settings are read when they change, not every millisecond
        double pos = pos_mm();
        int64_t now = esp_timer_get_time();
        bool stopped = estop_open() || driver_fault();

        if (s_homing) {                                        // up at the homing speed until the top switch closes
            if (low(CONFIG_WN_LIMIT_TOP_GPIO)) { s_steps = 0; s_homing = false; s_ignore_soft_top = false; s_homed = true; s_target = min; s_vel = 0; ESP_LOGI(TAG, "homed"); }
            else s_target = pos - 50;
        } else if (s_manual) {
            if (s_jog && now - s_jog_at > 1000000) s_jog = 0;  // the manager repeats nothing: a jog ends by itself
            if (s_jog) s_target = pos + s_jog * 30;
        } else if (CONFIG_WN_REQUIRE_HOME && !s_homed) s_target = pos;
        else if (wn_dmx_copy((uint16_t)uni, d)) {
            int a = addr; double k = ((d[a - 1] << 8) | d[a]) / 65535.0; if (invert) k = 1 - k;
            s_target = min + k * (max - min);
        } else if (loss_home) s_target = min;      // "hold": the last target stays

        if (!s_homing) s_target = fmin(max, fmax(min, s_target));
        double limit = s_homing ? CONFIG_WN_HOME_SPEED : s_manual && s_jog ? vmax * 0.4 : vmax;
        double dist = s_target - pos, stop = s_vel * s_vel / (2 * acc), want = fabs(dist) < 0.3 ? 0 : copysign(fabs(dist) <= stop ? fmax(5, fabs(s_vel) - acc * dt) : limit, dist);
        if (stopped) { want = 0; s_vel = 0; s_homing = false; s_ignore_soft_top = false; }
        double dv = want - s_vel; s_vel += fmax(-acc * dt, fmin(acc * dt, dv));
        if (fabs(dist) < 0.3 && fabs(s_vel) < 20) s_vel = 0;

        // brake and enable: release the brake first, then step; close it half a second after the motor stopped
        bool turning = fabs(s_vel) > 0.01;
        if (turning) { if (!start_at) { start_at = now; brake(true); } s_moved_at = now; } else if (now - s_moved_at > 500000) { brake(false); start_at = 0; }
        int dir = s_vel > 0 ? 1 : -1; double sps = fabs(s_vel) * SPM;
        if (!turning || now - start_at < 80000 || sps < 1) s_period_us = 0;
        else { if (dir != was_dir) { s_period_us = 0; gpio_set_level(CONFIG_WN_DIR_GPIO, (dir > 0) == CONFIG_WN_DIR_DOWN_HIGH); was_dir = dir; s_dir = dir; esp_rom_delay_us(20); }   // direction set-up time of the driver
            s_period_us = (uint32_t)fmax(40, 1e6 / sps); }
    }
}
static void lamp_task(void *arg) {
    for (;;) {
        if (s_lamp_gen != wn_config_generation()) {            // settings changed: build the output again
            s_lamp_gen = wn_config_generation(); wn_strip_free(s_lamp); s_lamp = NULL;
            uint16_t u[2] = { (uint16_t)wn_cfg_num("universe", 0), (uint16_t)wn_cfg_num("pixel.universe", 10) }; bool on = wn_cfg_bool("pixel.enabled", true);
            wn_dmx_listen(u, on ? 2 : 1);
            wn_strip_cfg_t c = { .gpio = CONFIG_WN_LAMP_GPIO, .pixels = (int)wn_cfg_num("pixel.pixels", 60), .chip = wn_cfg_str("pixel.chip", "WS2812B"), .order = "GRB", .group = 1, .limit = 100, .gamma = 2.2, .universe = u[1], .start_channel = 1 };
            if (on) s_lamp = wn_strip_new(&c);
        }
        if (wn_cfg_bool("identify", false)) wn_strip_show_pattern(s_lamp, (esp_timer_get_time() / 330000) % 2 ? "white" : "off", 0); else wn_strip_show_dmx(s_lamp);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void wn_device_start(void) {
    const int outs[] = { CONFIG_WN_STEP_GPIO, CONFIG_WN_DIR_GPIO, CONFIG_WN_ENABLE_GPIO, CONFIG_WN_BRAKE_GPIO }, ins[] = { CONFIG_WN_LIMIT_TOP_GPIO, CONFIG_WN_LIMIT_BOTTOM_GPIO, CONFIG_WN_ALARM_GPIO, CONFIG_WN_ESTOP_GPIO };
    for (size_t i = 0; i < 4; i++) if (outs[i] >= 0) { gpio_reset_pin(outs[i]); gpio_set_direction(outs[i], GPIO_MODE_OUTPUT); gpio_set_level(outs[i], 0); }   // brake closed, driver enabled (ENA low at the opto)
    for (size_t i = 0; i < 4; i++) if (ins[i] >= 0) { gpio_reset_pin(ins[i]); gpio_set_direction(ins[i], GPIO_MODE_INPUT); }                               // GPIO 34–39 have no pull-ups: the board has them
    s_dir = 1; s_target = 0;
    gptimer_config_t tc = { .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000 };
    ESP_ERROR_CHECK(gptimer_new_timer(&tc, &s_timer));
    gptimer_event_callbacks_t cb = { .on_alarm = on_step }; ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &cb, NULL));
    gptimer_alarm_config_t a = { .alarm_count = IDLE_US }; gptimer_set_alarm_action(s_timer, &a);
    ESP_ERROR_CHECK(gptimer_enable(s_timer)); ESP_ERROR_CHECK(gptimer_start(s_timer));
    xTaskCreatePinnedToCore(motion_task, "wn_motion", 4096, NULL, 15, NULL, 1);   // away from the network tasks
    xTaskCreatePinnedToCore(lamp_task, "wn_lamp", 4096, NULL, 5, NULL, 1);
}

cJSON *wn_device_defaults(void) {
    cJSON *c = cJSON_CreateObject(), *t = cJSON_CreateObject(), *p = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "protocol", "artnet"); cJSON_AddNumberToObject(c, "universe", 0); cJSON_AddNumberToObject(c, "address", 1);
    cJSON_AddNumberToObject(t, "min", 0); cJSON_AddNumberToObject(t, "max", 5000); cJSON_AddItemToObject(c, "travel", t);
    cJSON_AddNumberToObject(c, "speedMax", 800); cJSON_AddNumberToObject(c, "accel", 600); cJSON_AddBoolToObject(c, "invert", false); cJSON_AddStringToObject(c, "onLoss", "hold");
    cJSON_AddBoolToObject(p, "enabled", true); cJSON_AddNumberToObject(p, "universe", 10); cJSON_AddNumberToObject(p, "pixels", 60); cJSON_AddStringToObject(p, "chip", "WS2812B"); cJSON_AddItemToObject(c, "pixel", p);
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
    range(c, "universe", 0, 63999, 0); range(c, "address", 1, 511, 1); range(c, "speedMax", 10, 2000, 800); range(c, "accel", 10, 5000, 600);
    one_of(c, "protocol", "artnet", "sacn"); one_of(c, "onLoss", "hold", "home");
    cJSON *t = cJSON_GetObjectItem(c, "travel"), *p = cJSON_GetObjectItem(c, "pixel");
    if (cJSON_IsObject(t)) { range(t, "min", 0, 50000, 0); range(t, "max", cJSON_GetObjectItem(t, "min")->valuedouble + 10, 50000, 5000); }
    if (cJSON_IsObject(p)) { range(p, "universe", 0, 63999, 10); range(p, "pixels", 0, 1024, 60); }
}
void wn_device_status(cJSON *s, cJSON *alarms) {
    uint32_t fps; uint8_t level; double vmax = wn_cfg_num("speedMax", 800);
    cJSON_AddNumberToObject(s, "position", lround(pos_mm())); cJSON_AddNumberToObject(s, "target", lround(s_target)); cJSON_AddNumberToObject(s, "speed", lround(s_vel));
    cJSON_AddBoolToObject(s, "moving", fabs(s_vel) > 1); cJSON_AddBoolToObject(s, "homed", s_homed); cJSON_AddNumberToObject(s, "load", lround(25 + fabs(s_vel) / vmax * 60));
    cJSON *d = cJSON_CreateObject(); wn_dmx_stat((uint16_t)wn_cfg_num("universe", 0), &fps, &level); cJSON_AddNumberToObject(d, "fps", fps); cJSON_AddNumberToObject(d, "level", level); cJSON_AddItemToObject(s, "dmx", d);
    if (!fps) { cJSON *a = cJSON_CreateObject(); char t[48]; snprintf(t, sizeof t, "No data on universe %d", (int)wn_cfg_num("universe", 0)); cJSON_AddStringToObject(a, "code", "nodata"); cJSON_AddStringToObject(a, "text", t); cJSON_AddItemToArray(alarms, a); }
    if (wn_cfg_bool("pixel.enabled", true)) { cJSON *p = cJSON_CreateObject(); wn_dmx_stat((uint16_t)wn_cfg_num("pixel.universe", 10), &fps, &level); cJSON_AddNumberToObject(p, "fps", fps); cJSON_AddNumberToObject(p, "level", level); cJSON_AddItemToObject(s, "pixel", p); }
    else cJSON_AddNullToObject(s, "pixel");
    const struct { bool on; const char *code, *text; } more[] = { { driver_fault(), "stall", "Motor stalled" }, { estop_open(), "estop", "Emergency stop" }, { CONFIG_WN_REQUIRE_HOME && !s_homed, "nothomed", "Not homed: press Home" } };
    for (size_t i = 0; i < 3; i++) if (more[i].on) { cJSON *a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "code", more[i].code); cJSON_AddStringToObject(a, "text", more[i].text); cJSON_AddItemToArray(alarms, a); }
}
bool wn_device_action(const char *action, const cJSON *b, const char **error) {
    double min = wn_cfg_num("travel.min", 0), max = wn_cfg_num("travel.max", 5000);
    if (!strcmp(action, "home")) { if (estop_open() || driver_fault()) { *error = "cannot home: emergency stop or driver fault"; return true; } s_manual = true; s_jog = 0; s_ignore_soft_top = true; s_homing = true; }
    else if (!strcmp(action, "jog")) { const cJSON *d = cJSON_GetObjectItem(b, "dir"); int v = cJSON_IsNumber(d) ? (d->valuedouble > 0) - (d->valuedouble < 0) : 0; s_homing = false; s_ignore_soft_top = false;
        if (v) { s_manual = true; s_jog = v; s_jog_at = esp_timer_get_time(); } else { s_jog = 0; s_target = pos_mm(); } }
    else if (!strcmp(action, "goto")) { const cJSON *p = cJSON_GetObjectItem(b, "position"); s_homing = false; s_ignore_soft_top = false; s_manual = true; s_jog = 0; s_target = fmin(max, fmax(min, cJSON_IsNumber(p) ? p->valuedouble : 0)); }
    else if (!strcmp(action, "stop")) { s_homing = false; s_ignore_soft_top = false; s_manual = true; s_jog = 0; s_target = pos_mm(); }
    else if (!strcmp(action, "release")) { s_homing = false; s_ignore_soft_top = false; s_jog = 0; s_manual = false; }
    else return false;
    return true;
}
