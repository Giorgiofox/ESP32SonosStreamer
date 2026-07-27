# LPStreamer - Spike #1 (Arduino, reference)

Earlier Arduino version of Spike #1, kept for reference. The active project is
[`../spike_idf/`](../spike_idf/) (ESP-IDF).

Verifies that a Sonos speaker plays an **infinite WAV/PCM stream** served over HTTP by the
ESP32, driven over **UPnP** (`SetAVTransportURI` + `Play`). No ADC involved: the PCM is a
440 Hz sine tone generated in software.

## Usage

1. Arduino IDE with **esp32 by Espressif** installed (board manager).
2. Copy `config.h.example` to `config.h` and edit:
   - `WIFI_SSID` / `WIFI_PASS`
   - `SONOS_IP` -> the Sonos coordinator IP (Sonos app > Settings > System > About, or
     `http://<ip>:1400/xml/device_description.xml`)
3. Select the board (ESP32), flash.
4. Open the Serial Monitor at 115200. The ESP32:
   - connects to WiFi
   - starts the stream server on `:8080/stream.wav`
   - tells Sonos to play
5. Expected: Sonos plays a continuous 440 Hz tone.

## If it does not play - fallbacks in order

1. **Content-Type**: try `audio/L16;rate=44100;channels=2` both in the HTTP header and in the
   DIDL `protocolInfo` (`audio/L16`).
2. **Wrong coordinator**: if the zone is grouped, command the coordinator IP.
3. **Firewall/network**: ESP32 and Sonos on the same subnet/VLAN. Verify Sonos can reach
   `http://<esp-ip>:8080/stream.wav` (open it from the PC browser).
4. **SOAP error**: read the body printed on Serial (UPnP fault).
