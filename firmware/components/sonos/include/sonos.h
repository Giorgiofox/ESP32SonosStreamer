#pragma once
#include <stdbool.h>

#define SONOS_MAX_ZONES 16
#define SONOS_NAME_LEN  48
#define SONOS_IP_LEN    16

typedef struct {
    char name[SONOS_NAME_LEN];   // zone / group display name (e.g. "Living Room")
    char ip[SONOS_IP_LEN];       // coordinator IP
} sonos_zone_t;

// Discover Sonos zone groups: SSDP M-SEARCH + GetZoneGroupState topology parse.
// Fills the internal zone list (coordinators only). Returns the number of zones.
int sonos_discover(void);

int sonos_zone_count(void);
const sonos_zone_t *sonos_zone(int idx);          // NULL if out of range

// Currently selected zone index, or -1 if none.
int sonos_active(void);

// Select zone idx as the streaming target: stop the previous coordinator, then
// SetAVTransportURI(stream_url) + Play on the new one. Returns true on success.
bool sonos_select(int idx, const char *stream_url);

// Set volume (0..100) on the active coordinator.
bool sonos_set_volume(int vol);

// Stop playback on the active coordinator.
bool sonos_stop_active(void);
