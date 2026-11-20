// wn_net.c — the network: Ethernet (LAN8720 on the ESP32's RMII MAC), Wi-Fi station as the fallback.
//  Address: DHCP, or the static address of the configuration; with DHCP on, the static address is used after 10 s
//  without a lease, so a node on a bench without a router can still be reached.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "lwip/ip4_addr.h"
#include "wn.h"

static const char *TAG = "wn_net";
static char s_id[12] = "WN", s_ip[16] = "0.0.0.0";
static const char *s_link = "down";
static esp_netif_t *s_eth_netif, *s_sta_netif, *s_active;
static bool s_wifi;

const char *wn_id(void) { if (!s_id[2]) { uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_ETH); snprintf(s_id, sizeof s_id, "WN%02X%02X%02X", mac[3], mac[4], mac[5]); } return s_id; }
const char *wn_net_ip(void) { return s_ip; }
const char *wn_net_link(void) { return s_link; }
int wn_net_rssi(void) { wifi_ap_record_t ap; return s_wifi && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0; }

static void use_static(esp_netif_t *n) {
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = ipaddr_addr(wn_cfg_str("net.ip", "192.168.1.50")); ip.netmask.addr = ipaddr_addr(wn_cfg_str("net.mask", "255.255.255.0")); ip.gw.addr = ipaddr_addr(wn_cfg_str("net.gateway", "192.168.1.1"));
    esp_netif_dhcpc_stop(n);
    if (esp_netif_set_ip_info(n, &ip) == ESP_OK) { snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&ip.ip)); ESP_LOGI(TAG, "static address %s", s_ip); }
}
static void lease_watch(void *arg) {                          // DHCP on, but nobody answers: fall back to the static address
    vTaskDelay(pdMS_TO_TICKS(10000));
    if (!strcmp(s_ip, "0.0.0.0") && s_active) use_static(s_active);
    vTaskDelete(NULL);
}
static void link_up(esp_netif_t *n, const char *link) {
    s_active = n; s_link = link;
    if (!wn_cfg_bool("net.dhcp", true)) use_static(n); else xTaskCreate(lease_watch, "wn_lease", 3072, NULL, 3, NULL);
}
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        esp_eth_handle_t h = *(esp_eth_handle_t *)data; eth_speed_t sp = ETH_SPEED_100M; esp_eth_ioctl(h, ETH_CMD_G_SPEED, &sp);
        s_wifi = false; link_up(s_eth_netif, sp == ETH_SPEED_100M ? "100M" : "10M");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) { if (!s_wifi) { s_link = "down"; strcpy(s_ip, "0.0.0.0"); } }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) { if (!strcmp(s_link, "down")) { s_wifi = true; link_up(s_sta_netif, "wifi"); } }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) { if (s_wifi) { s_link = "down"; strcpy(s_ip, "0.0.0.0"); s_wifi = false; } esp_wifi_connect(); }
    else if (base == IP_EVENT && (id == IP_EVENT_ETH_GOT_IP || id == IP_EVENT_STA_GOT_IP)) {
        ip_event_got_ip_t *e = data; snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip)); ESP_LOGI(TAG, "address %s (%s)", s_ip, s_link);
    }
}

void wn_net_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);

#if CONFIG_WN_ETH_ENABLE
    if (CONFIG_WN_ETH_POWER_GPIO >= 0) { gpio_reset_pin(CONFIG_WN_ETH_POWER_GPIO); gpio_set_direction(CONFIG_WN_ETH_POWER_GPIO, GPIO_MODE_OUTPUT); gpio_set_level(CONFIG_WN_ETH_POWER_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(20)); }
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH(); s_eth_netif = esp_netif_new(&ncfg);
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG(); eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = CONFIG_WN_ETH_MDC_GPIO; emac.smi_gpio.mdio_num = CONFIG_WN_ETH_MDIO_GPIO;
    phy_cfg.phy_addr = CONFIG_WN_ETH_PHY_ADDR; phy_cfg.reset_gpio_num = -1;
    esp_eth_mac_t *m = esp_eth_mac_new_esp32(&emac, &mac_cfg); esp_eth_phy_t *p = esp_eth_phy_new_lan87xx(&phy_cfg);
    esp_eth_config_t ecfg = ETH_DEFAULT_CONFIG(m, p); esp_eth_handle_t eth = NULL;
    if (esp_eth_driver_install(&ecfg, &eth) == ESP_OK) { esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(eth)); esp_eth_start(eth); }
    else ESP_LOGW(TAG, "no Ethernet PHY answered");
#endif
    if (strlen(CONFIG_WN_WIFI_SSID)) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
        wifi_config_t sta = { 0 }; strlcpy((char *)sta.sta.ssid, CONFIG_WN_WIFI_SSID, sizeof sta.sta.ssid); strlcpy((char *)sta.sta.password, CONFIG_WN_WIFI_PASSWORD, sizeof sta.sta.password);
        esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &sta); esp_wifi_set_ps(WIFI_PS_NONE); esp_wifi_start();
    }
}
