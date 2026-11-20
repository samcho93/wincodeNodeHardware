// wn_main.c — start-up and the board itself: status LED, button, supply and temperature measurement.
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "wn.h"

static const char *TAG = "wn";
static adc_oneshot_unit_handle_t s_adc;
static double s_vin, s_temp = 25;

double wn_board_vin(void) { return s_vin; }
double wn_board_temp(void) { return s_temp; }
void wn_board_led(bool on) { if (CONFIG_WN_LED_GPIO >= 0) gpio_set_level(CONFIG_WN_LED_GPIO, on); }
static void reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
void wn_reboot_soon(void) { xTaskCreate(reboot_task, "wn_reboot", 2048, NULL, 5, NULL); }   // after the answer has gone out

/** mean of a few readings of an ADC1 channel, in volts at the pin (11 dB attenuation: about 0–3.1 V) */
static double read_volts(int channel) {
    int sum = 0, raw; for (int i = 0; i < 8; i++) { if (adc_oneshot_read(s_adc, channel, &raw) != ESP_OK) return 0; sum += raw; }
    return sum / 8.0 / 4095.0 * 3.1;
}
static void board_task(void *arg) {
    for (int tick = 0;; tick++) {
        if (CONFIG_WN_VIN_ADC_CHANNEL >= 0) s_vin += (read_volts(CONFIG_WN_VIN_ADC_CHANNEL) * 11.0 - s_vin) * 0.2;        // 100 k over 10 k
        if (CONFIG_WN_NTC_ADC_CHANNEL >= 0) { double v = read_volts(CONFIG_WN_NTC_ADC_CHANNEL);
            if (v > 0.05 && v < 3.2) { double r = 10000.0 * v / (3.3 - v), t = 1.0 / (1.0 / 298.15 + log(r / 10000.0) / 3950.0) - 273.15; s_temp += (t - s_temp) * 0.2; } }
        // status LED: identify = fast blink · no network = slow blink · running = on with a short gap every two seconds
        bool id = wn_cfg_bool("identify", false), net = strcmp(wn_net_ip(), "0.0.0.0") != 0;
        wn_board_led(id ? tick % 2 : !net ? (tick / 5) % 2 : tick % 20 != 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    if (CONFIG_WN_LED_GPIO >= 0) { gpio_reset_pin(CONFIG_WN_LED_GPIO); gpio_set_direction(CONFIG_WN_LED_GPIO, GPIO_MODE_OUTPUT); }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 }; adc_oneshot_new_unit(&ucfg, &s_adc);
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    if (CONFIG_WN_VIN_ADC_CHANNEL >= 0) adc_oneshot_config_channel(s_adc, CONFIG_WN_VIN_ADC_CHANNEL, &ccfg);
    if (CONFIG_WN_NTC_ADC_CHANNEL >= 0) adc_oneshot_config_channel(s_adc, CONFIG_WN_NTC_ADC_CHANNEL, &ccfg);

    wn_config_load();
    if (CONFIG_WN_BUTTON_GPIO >= 0) {                         // button held for five seconds at power-up: factory settings
        gpio_reset_pin(CONFIG_WN_BUTTON_GPIO); gpio_set_direction(CONFIG_WN_BUTTON_GPIO, GPIO_MODE_INPUT); gpio_set_pull_mode(CONFIG_WN_BUTTON_GPIO, GPIO_PULLUP_ONLY);
        int held = 0; while (!gpio_get_level(CONFIG_WN_BUTTON_GPIO) && held < 50) { wn_board_led(held % 2); vTaskDelay(pdMS_TO_TICKS(100)); held++; }
        if (held >= 50) { ESP_LOGW(TAG, "button held: factory settings"); wn_config_factory(false); cJSON *p = cJSON_Parse("{\"net\":{\"dhcp\":true}}"); wn_config_patch(p); cJSON_Delete(p); }   // a lost device comes back on DHCP
    }
    ESP_LOGI(TAG, "Wincode %s %s, firmware %s", WN_MODEL, wn_id(), WN_FW_VERSION);
    wn_net_start();
    wn_dmx_start();
    wn_device_start();                                        // motion and outputs work without the network
    wn_api_start();
    xTaskCreate(board_task, "wn_board", 3072, NULL, 2, NULL);
}
