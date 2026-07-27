#include "sonos.h"
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "lwip/sockets.h"

#define NVS_NS "lpstreamer"

static const char *TAG = "sonos";

static sonos_zone_t g_zones[SONOS_MAX_ZONES];
static int g_zone_count = 0;
static int g_active = -1;

// ------------------------- helpers -------------------------
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

// In-place XML entity unescape (enough for Sonos ZoneGroupState).
static void xml_unesc(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '&') {
            if      (!strncmp(r, "&lt;", 4))   { *w++ = '<'; r += 4; continue; }
            else if (!strncmp(r, "&gt;", 4))   { *w++ = '>'; r += 4; continue; }
            else if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; continue; }
            else if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; continue; }
            else if (!strncmp(r, "&amp;", 5))  { *w++ = '&'; r += 5; continue; }
        }
        *w++ = *r++;
    }
    *w = 0;
}

// Extract attribute key="value" starting the search at p. Returns true on success.
static bool get_attr(const char *p, const char *key, char *out, int cap) {
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char *a = strstr(p, pat);
    if (!a) { out[0] = 0; return false; }
    a += strlen(pat);
    int i = 0;
    while (*a && *a != '"' && i < cap - 1) out[i++] = *a++;
    out[i] = 0;
    return true;
}

// Extract host from a "http://HOST:1400/..." URL.
static void host_from_url(const char *url, char *out, int cap) {
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    int i = 0;
    while (*h && *h != ':' && *h != '/' && i < cap - 1) out[i++] = *h++;
    out[i] = 0;
}

// ------------------------- SOAP -------------------------
typedef struct { char *buf; int cap; int len; } acc_t;

static esp_err_t on_data(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        acc_t *a = (acc_t *)e->user_data;
        int n = e->data_len;
        if (a->len + n > a->cap - 1) n = a->cap - 1 - a->len;
        if (n > 0) { memcpy(a->buf + a->len, e->data, n); a->len += n; }
    }
    return ESP_OK;
}

// POST a SOAP action. If resp != NULL, capture the response body (NUL-terminated).
// Returns response length (>=0) on HTTP 200, or -1 on failure.
static int soap(const char *ip, const char *ctrl_path, const char *svc_type,
                const char *action, const char *inner, char *resp, int resp_cap) {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:1400%s", ip, ctrl_path);
    char soapaction[192];
    snprintf(soapaction, sizeof(soapaction), "\"%s#%s\"", svc_type, action);

    char *env = malloc(3072);
    int n = snprintf(env, 3072,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
        action, svc_type, inner, action);

    const int MAX_TRY = 3;
    int result = -1;
    for (int attempt = 1; attempt <= MAX_TRY && result < 0; attempt++) {
        acc_t acc = { resp, resp_cap, 0 };
        esp_http_client_config_t c = {
            .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 8000,
            .keep_alive_enable = false,
            .event_handler = resp ? on_data : NULL,
            .user_data = resp ? &acc : NULL,
        };
        esp_http_client_handle_t h = esp_http_client_init(&c);
        esp_http_client_set_header(h, "Content-Type", "text/xml; charset=\"utf-8\"");
        esp_http_client_set_header(h, "SOAPACTION", soapaction);
        esp_http_client_set_post_field(h, env, n);
        esp_err_t err = esp_http_client_perform(h);
        int status = esp_http_client_get_status_code(h);
        if (err == ESP_OK && status == 200) {
            if (resp) { resp[acc.len] = 0; result = acc.len; }
            else result = 0;
        } else {
            ESP_LOGW(TAG, "SOAP %s@%s try %d -> err=%s status=%d",
                     action, ip, attempt, esp_err_to_name(err), status);
        }
        esp_http_client_cleanup(h);
    }
    free(env);
    return result;
}

// ------------------------- SSDP discovery -------------------------
static bool ssdp_find_one(char *ip_out) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t ttl = 2;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    const char *msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";

    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(1900) };
    dst.sin_addr.s_addr = inet_addr("239.255.255.250");
    sendto(s, msearch, strlen(msearch), 0, (struct sockaddr *)&dst, sizeof(dst));

    char buf[1024];
    bool found = false;
    for (int i = 0; i < 8 && !found; i++) {
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        char *loc = strstr(buf, "http://");
        if (loc) { host_from_url(loc, ip_out, SONOS_IP_LEN); found = true; }
    }
    close(s);
    return found;
}

// ------------------------- topology -------------------------
static void parse_topology(char *xml) {
    xml_unesc(xml);
    g_zone_count = 0;
    char *p = xml;
    while ((p = strstr(p, "<ZoneGroup ")) && g_zone_count < SONOS_MAX_ZONES) {
        char *gend = strstr(p, "</ZoneGroup>");
        if (!gend) gend = p + strlen(p);
        char coord[64];
        get_attr(p, "Coordinator", coord, sizeof(coord));

        char *m = p;
        while ((m = strstr(m, "<ZoneGroupMember")) && m < gend) {
            char uuid[64], loc[160], zn[SONOS_NAME_LEN];
            get_attr(m, "UUID", uuid, sizeof(uuid));
            get_attr(m, "Location", loc, sizeof(loc));
            get_attr(m, "ZoneName", zn, sizeof(zn));
            if (uuid[0] && strcmp(uuid, coord) == 0) {
                sonos_zone_t *z = &g_zones[g_zone_count++];
                strlcpy(z->name, zn[0] ? zn : "Sonos", sizeof(z->name));
                host_from_url(loc, z->ip, sizeof(z->ip));
                break;
            }
            m += 16;
        }
        p = gend + 1;
    }
}

// Sort zones by name so the index -> zone mapping is stable across reboots
// (SSDP/topology order is otherwise non-deterministic).
static int zone_cmp(const void *a, const void *b) {
    return strcasecmp(((const sonos_zone_t *)a)->name, ((const sonos_zone_t *)b)->name);
}

int sonos_discover(void) {
    char ip[SONOS_IP_LEN];
    if (!ssdp_find_one(ip)) { ESP_LOGW(TAG, "no Sonos found via SSDP"); return 0; }
    ESP_LOGI(TAG, "SSDP responder: %s", ip);

    char *xml = malloc(16384);
    if (!xml) return 0;
    int len = soap(ip, "/ZoneGroupTopology/Control",
                   "urn:schemas-upnp-org:service:ZoneGroupTopology:1",
                   "GetZoneGroupState", "", xml, 16384);
    if (len <= 0) { free(xml); ESP_LOGW(TAG, "GetZoneGroupState failed"); return 0; }
    parse_topology(xml);
    free(xml);
    qsort(g_zones, g_zone_count, sizeof(g_zones[0]), zone_cmp);

    ESP_LOGI(TAG, "found %d zones:", g_zone_count);
    for (int i = 0; i < g_zone_count; i++)
        ESP_LOGI(TAG, "  [%d] %s @ %s", i, g_zones[i].name, g_zones[i].ip);
    return g_zone_count;
}

int sonos_zone_count(void) { return g_zone_count; }

const sonos_zone_t *sonos_zone(int idx) {
    if (idx < 0 || idx >= g_zone_count) return NULL;
    return &g_zones[idx];
}

int sonos_active(void) { return g_active; }

// ------------------------- control -------------------------
static bool av_play(const char *ip) {
    return soap(ip, "/MediaRenderer/AVTransport/Control",
                "urn:schemas-upnp-org:service:AVTransport:1",
                "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", NULL, 0) >= 0;
}

static bool av_stop(const char *ip) {
    return soap(ip, "/MediaRenderer/AVTransport/Control",
                "urn:schemas-upnp-org:service:AVTransport:1",
                "Stop", "<InstanceID>0</InstanceID>", NULL, 0) >= 0;
}

bool sonos_select(int idx, const char *stream_url) {
    if (idx < 0 || idx >= g_zone_count) return false;

    // stop the previously selected coordinator (best effort)
    if (g_active >= 0 && g_active != idx && g_active < g_zone_count)
        av_stop(g_zones[g_active].ip);

    const char *ip = g_zones[idx].ip;

    // Big buffers on the heap: this runs in the httpd task with a limited stack.
    char *url_esc  = malloc(256);
    char *didl     = malloc(1024);
    char *didl_esc = malloc(2048);
    char *inner    = malloc(2600);
    bool ok = url_esc && didl && didl_esc && inner;
    if (ok) {
        xml_esc(stream_url, url_esc, 256);
        snprintf(didl, 1024,
            "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
            "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
            "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
            "<item id=\"lp\" parentID=\"-1\" restricted=\"1\">"
            "<dc:title>LPStreamer</dc:title>"
            "<upnp:class>object.item.audioItem.audioBroadcast</upnp:class>"
            "<res protocolInfo=\"http-get:*:audio/wav:*\">%s</res>"
            "</item></DIDL-Lite>", url_esc);
        xml_esc(didl, didl_esc, 2048);
        snprintf(inner, 2600,
            "<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI>"
            "<CurrentURIMetaData>%s</CurrentURIMetaData>", url_esc, didl_esc);

        ok = soap(ip, "/MediaRenderer/AVTransport/Control",
                  "urn:schemas-upnp-org:service:AVTransport:1",
                  "SetAVTransportURI", inner, NULL, 0) >= 0
             && av_play(ip);
    }
    free(url_esc); free(didl); free(didl_esc); free(inner);
    if (!ok) return false;

    g_active = idx;
    ESP_LOGI(TAG, "selected zone [%d] %s @ %s", idx, g_zones[idx].name, ip);

    // persist as last-used zone (matched by name on next boot)
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "last_zone", g_zones[idx].name);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

bool sonos_set_volume(int vol) {
    if (g_active < 0) return false;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    char inner[128];
    snprintf(inner, sizeof(inner),
        "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>%d</DesiredVolume>", vol);
    return soap(g_zones[g_active].ip, "/MediaRenderer/RenderingControl/Control",
                "urn:schemas-upnp-org:service:RenderingControl:1",
                "SetVolume", inner, NULL, 0) >= 0;
}

bool sonos_stop_active(void) {
    if (g_active < 0) return false;
    return av_stop(g_zones[g_active].ip);
}

bool sonos_autoconnect(const char *stream_url) {
    char name[SONOS_NAME_LEN];
    size_t len = sizeof(name);
    nvs_handle_t h;
    bool got = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        got = (nvs_get_str(h, "last_zone", name, &len) == ESP_OK);
        nvs_close(h);
    }
    if (!got) { ESP_LOGI(TAG, "no last zone stored"); return false; }
    for (int i = 0; i < g_zone_count; i++) {
        if (strcmp(g_zones[i].name, name) == 0) {
            ESP_LOGI(TAG, "auto-connecting to last zone '%s'", name);
            return sonos_select(i, stream_url);
        }
    }
    ESP_LOGW(TAG, "last zone '%s' not currently available", name);
    return false;
}

bool sonos_disconnect(void) {
    if (g_active < 0) return false;
    bool ok = av_stop(g_zones[g_active].ip);
    ESP_LOGI(TAG, "disconnected from zone [%d] %s", g_active, g_zones[g_active].name);
    g_active = -1;
    return ok;
}
