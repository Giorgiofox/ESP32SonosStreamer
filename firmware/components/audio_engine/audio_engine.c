#include "audio_engine.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

static const char *TAG = "audio";

#define SAMPLE_RATE 44100
#define CHANNELS    2
#define BITS        16
#define STREAM_PATH "/stream.wav"

static uint16_t       s_port = 8080;
static volatile audio_source_t s_source = AUDIO_SRC_SILENCE;
static audio_stats_t  s_stats;

// ------------------------- WAV header -------------------------
typedef struct __attribute__((packed)) {
    char riff[4]; uint32_t chunkSize; char wave[4]; char fmt[4]; uint32_t fmtSize;
    uint16_t audioFmt; uint16_t channels; uint32_t sampleRate; uint32_t byteRate;
    uint16_t blockAlign; uint16_t bits; char data[4]; uint32_t dataSize;
} wav_hdr_t;

static void fill_wav(wav_hdr_t *h) {
    memcpy(h->riff, "RIFF", 4); h->chunkSize = 0xFFFFFFFF;
    memcpy(h->wave, "WAVE", 4); memcpy(h->fmt, "fmt ", 4); h->fmtSize = 16;
    h->audioFmt = 1; h->channels = CHANNELS; h->sampleRate = SAMPLE_RATE;
    h->byteRate = SAMPLE_RATE * CHANNELS * (BITS/8);
    h->blockAlign = CHANNELS * (BITS/8); h->bits = BITS;
    memcpy(h->data, "data", 4); h->dataSize = 0xFFFFFFFF;
}

// ------------------------- chime -------------------------
static bool send_all(int cs, const void *p, int n) {
    int sent = 0;
    while (sent < n) {
        int w = send(cs, (const uint8_t *)p + sent, n - sent, 0);
        if (w <= 0) return false;
        sent += w;
    }
    return true;
}

static bool send_tone(int cs, double freq, int ms, int16_t amp) {
    const int total = SAMPLE_RATE * ms / 1000;
    const int fade = SAMPLE_RATE / 100;
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
    bool ok = send_all(cs, buf, total * CHANNELS * sizeof(int16_t));
    free(buf);
    return ok;
}

static bool play_chime(int cs) {
    return send_tone(cs, 523.25, 120, 14000) &&
           send_tone(cs, 659.25, 120, 14000) &&
           send_tone(cs, 783.99, 220, 16000);
}

// ------------------------- pink noise -------------------------
typedef struct { float b0,b1,b2,b3,b4,b5,b6; } pink_state_t;
static uint32_t s_rng = 0x1234567u;

static inline float white_noise(void) {
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
    return pink * 0.11f;
}

// ------------------------- stream server -------------------------
static void serve_client(int cs, const char *ip) {
    strlcpy(s_stats.client_ip, ip, sizeof(s_stats.client_ip));
    s_stats.streaming = true;
    s_stats.total_bytes = 0;
    s_stats.connections++;

    char req[512]; recv(cs, req, sizeof(req)-1, 0);   // discard request headers

    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/wav\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n\r\n";
    if (!send_all(cs, hdr, strlen(hdr))) goto done;

    wav_hdr_t wh; fill_wav(&wh);
    if (!send_all(cs, &wh, sizeof(wh))) goto done;

    if (!play_chime(cs)) goto done;

    const int FRAMES = 441;   // 10 ms
    int16_t *buf = malloc(FRAMES * CHANNELS * sizeof(int16_t));
    if (!buf) goto done;
    pink_state_t pl = {0}, pr = {0};

    int64_t tlog = esp_timer_get_time();
    int64_t bytes_win = 0, max_batch = 0;

    for (;;) {
        if (s_source == AUDIO_SRC_PINK) {
            for (int i = 0; i < FRAMES; i++) {
                int l = (int)(pink_noise(&pl) * 9000);
                int r = (int)(pink_noise(&pr) * 9000);
                if (l >  32767) { l =  32767; }
                if (l < -32768) { l = -32768; }
                if (r >  32767) { r =  32767; }
                if (r < -32768) { r = -32768; }
                buf[i*2] = l; buf[i*2+1] = r;
            }
        } else {
            memset(buf, 0, FRAMES * CHANNELS * sizeof(int16_t));
        }
        int total = FRAMES * CHANNELS * sizeof(int16_t);
        int64_t ba = esp_timer_get_time();
        if (!send_all(cs, buf, total)) break;
        int64_t bd = esp_timer_get_time() - ba;
        if (bd > max_batch) max_batch = bd;
        bytes_win += total;
        s_stats.total_bytes += total;

        int64_t now = esp_timer_get_time();
        if (now - tlog >= 1000000) {
            s_stats.kbps = (bytes_win / 1024.0f) / ((now - tlog) / 1e6f);
            s_stats.max_batch_us = max_batch;
            tlog = now; bytes_win = 0; max_batch = 0;
        }
    }
    free(buf);
done:
    s_stats.streaming = false;
    s_stats.kbps = 0;
    close(cs);
    ESP_LOGI(TAG, "client %s closed", ip);
}

static void stream_task(void *arg) {
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY),
                              .sin_port = htons(s_port) };
    bind(ls, (struct sockaddr*)&sa, sizeof(sa));
    listen(ls, 1);
    ESP_LOGI(TAG, "stream server on :%d%s", s_port, STREAM_PATH);

    for (;;) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int cs = accept(ls, (struct sockaddr*)&ca, &cl);
        if (cs < 0) continue;
        char ip[16]; strlcpy(ip, inet_ntoa(ca.sin_addr), sizeof(ip));
        ESP_LOGI(TAG, "client %s connected", ip);
        serve_client(cs, ip);
    }
}

void audio_engine_start(uint16_t port) {
    s_port = port;
    memset(&s_stats, 0, sizeof(s_stats));
    xTaskCreate(stream_task, "audio_stream", 6144, NULL, 6, NULL);
}

const char *audio_engine_path(void) { return STREAM_PATH; }
uint16_t    audio_engine_port(void) { return s_port; }

void audio_engine_set_source(audio_source_t src) { s_source = src; }
audio_source_t audio_engine_get_source(void) { return s_source; }

audio_stats_t audio_engine_stats(void) { return s_stats; }

uint32_t audio_engine_sample_rate(void) { return SAMPLE_RATE; }
uint8_t  audio_engine_bits(void)        { return BITS; }
uint8_t  audio_engine_channels(void)    { return CHANNELS; }
