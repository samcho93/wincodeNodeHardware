set(srcs "wn_main.c" "wn_net.c" "wn_config.c" "wn_api.c" "wn_dmx.c" "wn_pixels.c")
if(CONFIG_WN_DEVICE_WINCH)
    list(APPEND srcs "wn_winch.c")
else()
    list(APPEND srcs "wn_pixelnode.c")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "."
    REQUIRES esp_eth esp_wifi esp_netif esp_event nvs_flash esp_http_server json esp_driver_gpio esp_driver_gptimer esp_adc esp_timer lwip)
