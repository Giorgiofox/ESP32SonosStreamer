idf_component_register(SRCS "sonos.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client lwip esp_netif nvs_flash)
