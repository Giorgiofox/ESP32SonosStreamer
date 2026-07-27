// LPStreamer - SPIKE #1
// Goal: verify that Sonos plays an infinite WAV/PCM stream served over HTTP by
// the ESP32, driven over UPnP (SetAVTransportURI + Play).
// No ADC involved: the PCM is a sine tone generated in software.
//
// If Sonos plays the 440 Hz tone -> risk #1 (WAV ingest) is VALIDATED.
//
// Board: ESP32 / ESP32-S3  (Arduino-ESP32 framework)

#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include "config.h"

// ---------------------------------------------------------------------------
WiFiServer streamServer(STREAM_PORT);

// WAV header for an "infinite" stream: use huge sizes so the client never tries
// to close on end-of-file. Sonos reads it as a stream.
struct __attribute__((packed)) WavHeader {
  char     riff[4]   = {'R','I','F','F'};
  uint32_t chunkSize = 0xFFFFFFFF;              // "infinite"
  char     wave[4]   = {'W','A','V','E'};
  char     fmt[4]    = {'f','m','t',' '};
  uint32_t fmtSize   = 16;
  uint16_t audioFmt  = 1;                        // PCM
  uint16_t channels  = CHANNELS;
  uint32_t sampleRate= SAMPLE_RATE;
  uint32_t byteRate  = SAMPLE_RATE * CHANNELS * (BITS/8);
  uint16_t blockAlign= CHANNELS * (BITS/8);
  uint16_t bits      = BITS;
  char     data[4]   = {'d','a','t','a'};
  uint32_t dataSize  = 0xFFFFFFFF;               // "infinite"
};

// ---------------------------------------------------------------------------
static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi...");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.printf("\nOK  IP=%s\n", WiFi.localIP().toString().c_str());
}

// SOAP helper: send an AVTransport action to Sonos.
static bool soapAction(const char* action, const String& bodyInner) {
  HTTPClient http;
  String url = String("http://") + SONOS_IP + ":" + SONOS_PORT +
               "/MediaRenderer/AVTransport/Control";
  http.begin(url);
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION",
    String("\"urn:schemas-upnp-org:service:AVTransport:1#") + action + "\"");

  String env =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:" + action + " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>" + bodyInner +
    "</u:" + action + ">"
    "</s:Body></s:Envelope>";

  int code = http.POST(env);
  Serial.printf("SOAP %s -> HTTP %d\n", action, code);
  if (code > 0) Serial.println(http.getString());
  http.end();
  return code == 200;
}

// Minimal XML-escape to embed the URI inside the DIDL.
static String xmlEsc(const String& in) {
  String o; o.reserve(in.length()+16);
  for (char c : in) {
    switch (c) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '"':  o += "&quot;"; break;
      default:   o += c;
    }
  }
  return o;
}

static void sonosPlayStream() {
  String streamUrl = String("http://") + WiFi.localIP().toString() +
                     ":" + STREAM_PORT + STREAM_PATH;
  Serial.printf("Stream URL: %s\n", streamUrl.c_str());

  // DIDL-Lite metadata. protocolInfo declares PCM WAV.
  String didl =
    "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
    "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
    "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
    "<item id=\"lpstreamer\" parentID=\"-1\" restricted=\"1\">"
    "<dc:title>LPStreamer Spike</dc:title>"
    "<upnp:class>object.item.audioItem.audioBroadcast</upnp:class>"
    "<res protocolInfo=\"http-get:*:audio/wav:*\">" + xmlEsc(streamUrl) + "</res>"
    "</item></DIDL-Lite>";

  String setBody =
    "<CurrentURI>" + xmlEsc(streamUrl) + "</CurrentURI>"
    "<CurrentURIMetaData>" + xmlEsc(didl) + "</CurrentURIMetaData>";

  if (soapAction("SetAVTransportURI", setBody)) {
    soapAction("Play", "<Speed>1</Speed>");
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  connectWifi();
  streamServer.begin();
  streamServer.setNoDelay(true);
  Serial.println("Stream server up. Commanding Sonos in 2s...");
  delay(2000);
  sonosPlayStream();
}

// Generate N stereo sine frames and write them to the client. Returns false on disconnect.
static bool pumpSine(WiFiClient& client) {
  static double phase = 0.0;
  const double step = 2.0 * PI * TONE_HZ / SAMPLE_RATE;

  const int FRAMES = 512;                 // stereo frames per burst
  int16_t buf[FRAMES * CHANNELS];
  for (int i = 0; i < FRAMES; ++i) {
    int16_t s = (int16_t)(sin(phase) * 12000.0);   // ~-8 dBFS, headroom
    phase += step;
    if (phase > 2.0 * PI) phase -= 2.0 * PI;
    buf[i*2]   = s;                       // L
    buf[i*2+1] = s;                       // R
  }
  size_t bytes = sizeof(buf);
  size_t w = client.write((uint8_t*)buf, bytes);
  return w == bytes && client.connected();
}

void loop() {
  WiFiClient client = streamServer.available();
  if (!client) { delay(1); return; }

  Serial.printf("Client %s\n", client.remoteIP().toString().c_str());

  // Read (and discard) the request line until an empty line.
  String req = client.readStringUntil('\n');
  Serial.printf("REQ: %s\n", req.c_str());
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }

  // HTTP header + WAV header, then continuous pump.
  client.print(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: audio/wav\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-cache\r\n"
    "\r\n");

  WavHeader hdr;
  client.write((uint8_t*)&hdr, sizeof(hdr));

  Serial.println("Streaming sine...");
  while (client.connected()) {
    if (!pumpSine(client)) break;
  }
  client.stop();
  Serial.println("Client closed.");
}
