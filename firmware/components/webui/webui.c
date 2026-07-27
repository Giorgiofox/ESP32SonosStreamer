#include "webui.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "sonos.h"
#include "audio_engine.h"
#include "net_wifi.h"

static const char *TAG = "webui";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

// Build the stream URL this ESP32 serves (points Sonos back at us).
static void stream_url(char *out, int cap) {
    snprintf(out, cap, "http://%s:%u%s",
             net_wifi_ip(), audio_engine_port(), audio_engine_path());
}

// ------------------------- handlers -------------------------
static esp_err_t h_root(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_status(httpd_req_t *r) {
    audio_stats_t st = audio_engine_stats();
    int a = sonos_active();
    const sonos_zone_t *z = (a >= 0) ? sonos_zone(a) : NULL;

    char json[512];
    int n = snprintf(json, sizeof(json),
        "{\"ip\":\"%s\",\"zone\":\"%s\",\"streaming\":%s,\"client\":\"%s\","
        "\"kbps\":%.1f,\"sent\":%lld,\"batch_us\":%lld,\"rssi\":%d,\"source\":\"%s\"}",
        net_wifi_ip(),
        z ? z->name : "",
        st.streaming ? "true" : "false",
        st.client_ip,
        st.kbps, (long long)st.total_bytes, (long long)st.max_batch_us,
        net_wifi_rssi(),
        audio_engine_get_source() == AUDIO_SRC_PINK ? "pink" : "silence");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, json, n);
}

static esp_err_t h_zones(httpd_req_t *r) {
    char *json = malloc(2048);
    int n = snprintf(json, 2048, "{\"active\":%d,\"zones\":[", sonos_active());
    for (int i = 0; i < sonos_zone_count(); i++) {
        const sonos_zone_t *z = sonos_zone(i);
        n += snprintf(json + n, 2048 - n, "%s{\"name\":\"%s\",\"ip\":\"%s\"}",
                      i ? "," : "", z->name, z->ip);
    }
    n += snprintf(json + n, 2048 - n, "]}");
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_send(r, json, n);
    free(json);
    return e;
}

// Read an integer query parameter (e.g. ?i=3). Returns def if absent.
static int query_int(httpd_req_t *r, const char *key, int def) {
    char q[64], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK) return def;
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return def;
    return atoi(v);
}

static esp_err_t h_select(httpd_req_t *r) {
    int i = query_int(r, "i", -1);
    char url[96]; stream_url(url, sizeof(url));
    bool ok = sonos_select(i, url);
    httpd_resp_sendstr(r, ok ? "ok" : "err");
    return ESP_OK;
}

static esp_err_t h_volume(httpd_req_t *r) {
    int v = query_int(r, "v", -1);
    bool ok = (v >= 0) && sonos_set_volume(v);
    httpd_resp_sendstr(r, ok ? "ok" : "err");
    return ESP_OK;
}

static esp_err_t h_source(httpd_req_t *r) {
    char q[64], v[16] = {0};
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "s", v, sizeof(v));
    audio_engine_set_source(strcmp(v, "pink") == 0 ? AUDIO_SRC_PINK : AUDIO_SRC_SILENCE);
    httpd_resp_sendstr(r, "ok");
    return ESP_OK;
}

static esp_err_t h_rescan(httpd_req_t *r) {
    int n = sonos_discover();
    char msg[32]; snprintf(msg, sizeof(msg), "%d", n);
    httpd_resp_sendstr(r, msg);
    return ESP_OK;
}

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t *)) {
    httpd_uri_t u = { .uri = uri, .method = m, .handler = fn };
    httpd_register_uri_handler(s, &u);
}

void webui_start(uint16_t port) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    reg(s, "/",            HTTP_GET,  h_root);
    reg(s, "/api/status",  HTTP_GET,  h_status);
    reg(s, "/api/zones",   HTTP_GET,  h_zones);
    reg(s, "/api/select",  HTTP_POST, h_select);
    reg(s, "/api/volume",  HTTP_POST, h_volume);
    reg(s, "/api/source",  HTTP_POST, h_source);
    reg(s, "/api/rescan",  HTTP_POST, h_rescan);
    ESP_LOGI(TAG, "web UI on :%u", port);
}
