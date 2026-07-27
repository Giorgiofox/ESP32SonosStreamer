// LPStreamer - main firmware
//
// Control plane over the validated streaming engine:
//   net_wifi      -> join the LAN
//   audio_engine  -> serve an infinite WAV/PCM stream over HTTP (test source for now,
//                    turntable ADC later) with live throughput stats
//   sonos         -> discover zones (SSDP + topology) and drive them over UPnP
//   webui         -> dashboard + JSON API to pick the target zone, volume, source, stats
//
// Flow: connect WiFi -> start stream server -> discover Sonos zones -> start web UI.
// The user then picks a target zone from the web UI (or, later, the rotary encoder),
// which points that Sonos coordinator at our stream URL and starts playback.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "net_wifi.h"
#include "audio_engine.h"
#include "sonos.h"
#include "webui.h"
#include "config.h"

static const char *TAG = "app";

void app_main(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    net_wifi_start(WIFI_SSID, WIFI_PASS);
    ESP_LOGI(TAG, "WiFi up, IP=%s", net_wifi_ip());

    audio_engine_start(STREAM_PORT);

    int n = sonos_discover();
    ESP_LOGI(TAG, "%d Sonos zones discovered", n);

    webui_start(WEB_PORT);

    // Auto-reconnect to the last used zone so turning on the system needs no web action.
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%u%s",
             net_wifi_ip(), audio_engine_port(), audio_engine_path());
    sonos_autoconnect(url);

    ESP_LOGI(TAG, "ready - open http://%s/ to pick a zone", net_wifi_ip());
}
