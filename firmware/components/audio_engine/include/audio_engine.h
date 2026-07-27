#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     streaming;        // a client (Sonos) is currently pulling the stream
    char     client_ip[16];    // its IP
    float    kbps;             // throughput over the last window
    int64_t  total_bytes;      // bytes sent on the current connection
    int64_t  max_batch_us;     // worst-case send latency for a 10 ms block (window)
    float    buffer_sec;       // estimated seconds of audio buffered ahead of realtime
    uint32_t connections;      // total client connections since boot
} audio_stats_t;

// Source of the audio the engine streams.
typedef enum {
    AUDIO_SRC_SILENCE = 0,     // silent keep-alive
    AUDIO_SRC_PINK    = 1,     // pink noise (test)
} audio_source_t;

// Start the HTTP WAV stream server on the given port.
void audio_engine_start(uint16_t port);

// URL path served (e.g. "/stream.wav").
const char *audio_engine_path(void);
uint16_t    audio_engine_port(void);

void          audio_engine_set_source(audio_source_t src);
audio_source_t audio_engine_get_source(void);

audio_stats_t audio_engine_stats(void);

// Stream PCM format.
uint32_t audio_engine_sample_rate(void);
uint8_t  audio_engine_bits(void);
uint8_t  audio_engine_channels(void);
