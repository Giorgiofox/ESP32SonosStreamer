// LPStreamer - SPIKE #1 (ESP-IDF)
// Verifies that a Sonos speaker plays an infinite WAV/PCM stream served over HTTP
// by the ESP32, driven over UPnP (SetAVTransportURI + Play).
// PCM = 440 Hz sine tone generated in software. No ADC.
//
// Stream server = raw TCP socket (no chunked encoding: Sonos parses clean WAV).
// SOAP = esp_http_client.

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "config.h"

static const char *TAG = "lpstreamer";
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0
static char s_ip[16] = {0};

// ------------------------- WiFi -------------------------
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "IP = %s", s_ip);
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void) {
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    wifi_config_t wc = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi connecting to %s...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

// ------------------------- SOAP / UPnP -------------------------
static void xml_esc(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 6 < cap; i++) {
        switch (in[i]) {
            case '&': o += snprintf(out+o, cap-o, "&amp;");  break;
            case '<': o += snprintf(out+o, cap-o, "&lt;");   break;
            case '>': o += snprintf(out+o, cap-o, "&gt;");   break;
            case '"': o += snprintf(out+o, cap-o, "&quot;"); break;
            default:  out[o++] = in[i];
        }
    }
    out[o] = 0;
}

static bool soap_action(const char *action, const char *body_inner) {
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d/MediaRenderer/AVTransport/Control", SONOS_IP, SONOS_PORT);
    char soapaction[128];
    snprintf(soapaction, sizeof(soapaction),
             "\"urn:schemas-upnp-org:service:AVTransport:1#%s\"", action);

    char *env = malloc(2048);
    int n = snprintf(env, 2048,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<u:%s xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID>%s</u:%s></s:Body></s:Envelope>",
        action, body_inner, action);

    // Retry: the first cold request (ARP/TCP toward the Sonos) sometimes returns
    // ESP_ERR_HTTP_EAGAIN before the response is ready. Retry a few times.
    const int MAX_TRY = 4;
    bool ok = false;
    for (int attempt = 1; attempt <= MAX_TRY && !ok; attempt++) {
        esp_http_client_config_t c = {
            .url = url, .method = HTTP_METHOD_POST,
            .timeout_ms = 8000, .keep_alive_enable = false,
        };
        esp_http_client_handle_t h = esp_http_client_init(&c);
        esp_http_client_set_header(h, "Content-Type", "text/xml; charset=\"utf-8\"");
        esp_http_client_set_header(h, "SOAPACTION", soapaction);
        esp_http_client_set_post_field(h, env, n);
        esp_err_t err = esp_http_client_perform(h);
        int status = esp_http_client_get_status_code(h);
        ok = (err == ESP_OK && status == 200);
        ESP_LOGI(TAG, "SOAP %s try %d/%d -> err=%s status=%d%s",
                 action, attempt, MAX_TRY, esp_err_to_name(err), status, ok ? " OK" : "");
        esp_http_client_cleanup(h);
        if (!ok && attempt < MAX_TRY) vTaskDelay(pdMS_TO_TICKS(500));
    }
    free(env);
    return ok;
}

static void sonos_play(void) {
    char stream_url[80];
    snprintf(stream_url, sizeof(stream_url), "http://%s:%d%s", s_ip, STREAM_PORT, STREAM_PATH);
    ESP_LOGI(TAG, "Stream URL: %s", stream_url);

    char url_esc[256]; xml_esc(stream_url, url_esc, sizeof(url_esc));

    char didl[1024];
    snprintf(didl, sizeof(didl),
        "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
        "<item id=\"lp\" parentID=\"-1\" restricted=\"1\">"
        "<dc:title>LPStreamer Spike</dc:title>"
        "<upnp:class>object.item.audioItem.audioBroadcast</upnp:class>"
        "<res protocolInfo=\"http-get:*:audio/wav:*\">%s</res>"
        "</item></DIDL-Lite>", url_esc);

    char didl_esc[2048]; xml_esc(didl, didl_esc, sizeof(didl_esc));

    char body[2560];
    snprintf(body, sizeof(body),
        "<CurrentURI>%s</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>",
        url_esc, didl_esc);

    if (soap_action("SetAVTransportURI", body)) {
        soap_action("Play", "<Speed>1</Speed>");
    }
}

// ------------------------- WAV header -------------------------
typedef struct __attribute__((packed)) {
    char     riff[4];    uint32_t chunkSize;
    char     wave[4];    char     fmt[4];   uint32_t fmtSize;
    uint16_t audioFmt;   uint16_t channels; uint32_t sampleRate;
    uint32_t byteRate;   uint16_t blockAlign; uint16_t bits;
    char     data[4];    uint32_t dataSize;
} wav_hdr_t;

__attribute__((unused))
static void fill_wav(wav_hdr_t *h) {
    memcpy(h->riff, "RIFF", 4); h->chunkSize = 0xFFFFFFFF;
    memcpy(h->wave, "WAVE", 4); memcpy(h->fmt, "fmt ", 4); h->fmtSize = 16;
    h->audioFmt = 1; h->channels = CHANNELS; h->sampleRate = SAMPLE_RATE;
    h->byteRate = SAMPLE_RATE * CHANNELS * (BITS/8);
    h->blockAlign = CHANNELS * (BITS/8); h->bits = BITS;
    memcpy(h->data, "data", 4); h->dataSize = 0xFFFFFFFF;
}

// ------------------------- chime ("speaker on") -------------------------
// Plays a note of freq Hz for ms milliseconds with an attack/release envelope
// (no click). Returns false if the client disconnected.
static bool send_tone(int cs, double freq, int ms, int16_t amp) {
    const int total = SAMPLE_RATE * ms / 1000;      // frames
    const int fade = SAMPLE_RATE / 100;             // 10 ms fade
    int16_t *buf = malloc(total * CHANNELS * sizeof(int16_t));
    if (!buf) return false;
    double phase = 0.0, step = 2.0 * M_PI * freq / SAMPLE_RATE;
    for (int i = 0; i < total; i++) {
        double env = 1.0;
        if (i < fade)              env = (double)i / fade;
        else if (i > total - fade) env = (double)(total - i) / fade;
        int16_t s = (int16_t)(sin(phase) * amp * env);
        phase += step; if (phase > 2*M_PI) phase -= 2*M_PI;
        buf[i*2] = buf[i*2+1] = s;
    }
    int n = total * CHANNELS * sizeof(int16_t), sent = 0;
    bool ok = true;
    while (sent < n) {
        int w = send(cs, ((uint8_t*)buf)+sent, n-sent, 0);
        if (w <= 0) { ok = false; break; }
        sent += w;
    }
    free(buf);
    return ok;
}

// Ascending "bluetooth speaker on" style chime: C5 - E5 - G5.
__attribute__((unused))
static bool play_chime(int cs) {
    return send_tone(cs, 523.25, 120, 14000) &&   // C5
           send_tone(cs, 659.25, 120, 14000) &&   // E5
           send_tone(cs, 783.99, 220, 16000);     // G5
}

// ------------------------- pink noise (Kellet refined, per channel) -------------------------
typedef struct { float b0,b1,b2,b3,b4,b5,b6; } pink_state_t;
static uint32_t s_rng = 0x1234567u;

static inline float white_noise(void) {
    // xorshift32 -> [-1, 1)
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return ((int32_t)s_rng) / 2147483648.0f;
}

static inline float pink_noise(pink_state_t *p) {
    float w = white_noise();
    p->b0 = 0.99886f*p->b0 + w*0.0555179f;
    p->b1 = 0.99332f*p->b1 + w*0.0750759f;
    p->b2 = 0.96900f*p->b2 + w*0.1538520f;
    p->b3 = 0.86650f*p->b3 + w*0.3104856f;
    p->b4 = 0.55000f*p->b4 + w*0.5329522f;
    p->b5 = -0.7616f*p->b5 - w*0.0168980f;
    float pink = p->b0+p->b1+p->b2+p->b3+p->b4+p->b5+p->b6 + w*0.5362f;
    p->b6 = w*0.115926f;
    return pink * 0.11f;   // roughly normalized to [-1, 1]
}

// Stream N seconds of pink noise, logging throughput about once per second.
// Returns false on disconnect.
__attribute__((unused))
static bool stream_pink(int cs, int16_t *buf, int frames, int seconds) {
    pink_state_t pl = {0}, pr = {0};
    const int gain = 9000;
    const int64_t target_frames = (int64_t)SAMPLE_RATE * seconds;
    int64_t done = 0;

    int64_t t0 = esp_timer_get_time(), tlog = t0;
    int64_t bytes_win = 0;
    int64_t max_batch_us = 0;

    while (done < target_frames) {
        for (int i = 0; i < frames; i++) {
            int l = (int)(pink_noise(&pl) * gain);
            int r = (int)(pink_noise(&pr) * gain);
            if (l >  32767) { l =  32767; }
            if (l < -32768) { l = -32768; }
            if (r >  32767) { r =  32767; }
            if (r < -32768) { r = -32768; }
            buf[i*2] = l;
            buf[i*2+1] = r;
        }
        int total = frames * CHANNELS * sizeof(int16_t), sent = 0;
        int64_t ba = esp_timer_get_time();
        while (sent < total) {
            int w = send(cs, ((uint8_t*)buf)+sent, total-sent, 0);
            if (w <= 0) return false;
            sent += w;
        }
        int64_t bd = esp_timer_get_time() - ba;
        if (bd > max_batch_us) max_batch_us = bd;
        bytes_win += total;
        done += frames;

        int64_t now = esp_timer_get_time();
        if (now - tlog >= 1000000) {
            float kbs = (bytes_win / 1024.0f) / ((now - tlog) / 1e6f);
            // target 176.4 kB/s. max_batch = worst-case stall on a single 10 ms audio block.
            ESP_LOGI(TAG, "throughput=%.1f kB/s (target 176.4)  max_batch=%lld us  t=%llds",
                     kbs, max_batch_us, (now - t0)/1000000);
            tlog = now; bytes_win = 0; max_batch_us = 0;
        }
    }
    return true;
}

// ------------------------- proxy: host WAV -> Sonos -------------------------
// Opens TCP to the host, GETs the file, skips the host's HTTP headers, and
// forwards the body (WAV incl. header) to the Sonos socket. Logs throughput.
static bool stream_proxy(int cs) {
    int ps = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ps < 0) return false;
    struct sockaddr_in pa = { .sin_family = AF_INET,
                              .sin_port = htons(PROXY_PORT),
                              .sin_addr.s_addr = inet_addr(PROXY_HOST) };
    if (connect(ps, (struct sockaddr*)&pa, sizeof(pa)) != 0) {
        ESP_LOGE(TAG, "proxy: connect to %s:%d failed", PROXY_HOST, PROXY_PORT);
        close(ps); return false;
    }
    char get[192];
    int gl = snprintf(get, sizeof(get),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        PROXY_PATH, PROXY_HOST, PROXY_PORT);
    send(ps, get, gl, 0);

    // skip the host response headers up to \r\n\r\n
    int match = 0;
    while (match < 4) {
        char c; int n = recv(ps, &c, 1, 0);
        if (n <= 0) { ESP_LOGE(TAG, "proxy: no header"); close(ps); return false; }
        char want = (match == 1) ? '\n' : (match == 3) ? '\n' : '\r';
        if (c == want) match++;
        else match = (c == '\r') ? 1 : 0;
    }
    ESP_LOGI(TAG, "proxy: host header skipped, forwarding body...");

    uint8_t fb[1460];
    int64_t t0 = esp_timer_get_time(), tlog = t0, bytes_win = 0, total_bytes = 0, max_batch = 0;
    while (1) {
        int n = recv(ps, fb, sizeof(fb), 0);
        if (n == 0) break;                 // file EOF
        if (n < 0) { close(ps); return false; }
        int sent = 0;
        int64_t ba = esp_timer_get_time();
        while (sent < n) {
            int w = send(cs, fb + sent, n - sent, 0);
            if (w <= 0) { close(ps); return false; }   // Sonos disconnected
            sent += w;
        }
        int64_t bd = esp_timer_get_time() - ba;
        if (bd > max_batch) max_batch = bd;
        bytes_win += n; total_bytes += n;
        int64_t now = esp_timer_get_time();
        if (now - tlog >= 1000000) {
            float kbs = (bytes_win / 1024.0f) / ((now - tlog) / 1e6f);
            ESP_LOGI(TAG, "proxy throughput=%.1f kB/s (target 176.4) max_batch=%lld us t=%llds",
                     kbs, max_batch, (now - t0)/1000000);
            tlog = now; bytes_win = 0; max_batch = 0;
        }
    }
    close(ps);
    ESP_LOGI(TAG, "proxy: file done, %lld bytes forwarded", total_bytes);
    return true;
}

// ------------------------- stream server (raw TCP) -------------------------
static void stream_task(void *arg) {
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY),
                              .sin_port = htons(STREAM_PORT) };
    bind(ls, (struct sockaddr*)&sa, sizeof(sa));
    listen(ls, 1);
    ESP_LOGI(TAG, "Stream server on :%d", STREAM_PORT);

    const int FRAMES = 441;                    // 10 ms @ 44.1k
    int16_t *buf = malloc(FRAMES * CHANNELS * sizeof(int16_t));
    (void)FRAMES; (void)buf;

    while (1) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int cs = accept(ls, (struct sockaddr*)&ca, &cl);
        if (cs < 0) continue;
        ESP_LOGI(TAG, "Client %s connected", inet_ntoa(ca.sin_addr));

        // discard request headers
        char req[512]; recv(cs, req, sizeof(req)-1, 0);

        const char *hdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: audio/wav\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-cache\r\n\r\n";
        send(cs, hdr, strlen(hdr), 0);

#if STREAM_MODE == MODE_PROXY
        // Proxy: forward the host WAV (already has its own RIFF header) to Sonos.
        ESP_LOGI(TAG, "PROXY %s:%d%s", PROXY_HOST, PROXY_PORT, PROXY_PATH);
        stream_proxy(cs);
        close(cs);
        ESP_LOGI(TAG, "Client closed");
#else
        wav_hdr_t wh; fill_wav(&wh);
        send(cs, &wh, sizeof(wh), 0);

        // "Speaker on" chime as soon as Sonos picks up the stream.
        ESP_LOGI(TAG, "Chime...");
        if (!play_chime(cs)) { close(cs); ESP_LOGI(TAG, "Client closed (chime)"); continue; }

        // Stability test: 20 s of pink noise with throughput logging.
        ESP_LOGI(TAG, "Streaming pink noise 20s (stability test)...");
        bool alive = stream_pink(cs, buf, FRAMES, 20);
        if (alive) ESP_LOGI(TAG, "Pink test OK. Switching to keep-alive silence.");

        // Then continuous silence (stream stays alive, quiet). Becomes ADC PCM in Spike #2.
        memset(buf, 0, FRAMES * CHANNELS * sizeof(int16_t));
        while (alive) {
            int total = FRAMES * CHANNELS * sizeof(int16_t), sent = 0;
            while (sent < total) {
                int w = send(cs, ((uint8_t*)buf)+sent, total-sent, 0);
                if (w <= 0) { alive = false; break; }
                sent += w;
            }
        }
        close(cs);
        ESP_LOGI(TAG, "Client closed");
#endif
    }
}

// ------------------------- main -------------------------
void app_main(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    wifi_start();
    xTaskCreate(stream_task, "stream", 6144, NULL, 5, NULL);
    vTaskDelay(pdMS_TO_TICKS(2000));   // give the server time to start
    sonos_play();
}
