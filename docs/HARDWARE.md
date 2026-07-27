# Hardware - planned wiring (Spike #2)

> Planned **WM8782 I2S ADC board <-> ESP32** wiring. To confirm once the board arrives:
> pin labels vary slightly between modules.

## WM8782 I2S ADC board (chosen)

Audiophile 24-bit ADC (Wolfson/Cirrus WM8782) on a breakout with **RCA L/R line inputs**
(plus a 3.5 mm jack), an **onboard 24.576 MHz oscillator** (so MCLK is handled by the board),
and a **Master/Slave** switch.

Rate support per the board:
- **Master mode** (board drives the clocks): 192 k or 96 k only.
- **Slave mode** (the ESP32 drives BICK/LRCK): also 48 k / 16-bit.

We target **48 kHz / 16-bit** (the Sonos internal rate), so use **Slave mode**: the ESP32 is
the I2S master and generates BICK (bit clock) and LRCK (word select); the board provides MCLK
from its own oscillator plus the DATA line. This matches the stream format directly, with no
resampling on the ESP32.

(Alternative: Master mode at 96 k / 24-bit, then downsample to 48/16 on the ESP32 - more CPU,
not needed.)

## Pinout (indicative)

| WM8782 board | ESP32 (example) | Notes |
|-------------:|:----------------|-------|
| VCC (5V)     | 5V              | board runs at 5V |
| GND / DGND   | GND             | common ground |
| RCA L/R in   | <- phono preamp | **line-level** input |
| DATA         | GPIO25 (I2S DIN)| I2S data to the ESP32 |
| BICK         | GPIO26 (I2S BCK)| bit clock (ESP32 drives it in slave mode) |
| LRCK         | GPIO22 (I2S WS) | word/frame select (ESP32 drives it) |
| MCLK         | (from board osc)| provided by the onboard 24.576 MHz clock |
| Master/Slave | switch -> Slave | so the ESP32 clocks 48 k / 16-bit |

Note: GPIO choices to be confirmed (avoid boot strapping pins). Update after testing.

## Power

- Analog side of the ADC on a **dedicated low-noise LDO**, with the analog ground separated
  from the digital ground and joined at a single point.
- Supply noise is the number one factor that degrades audio: take care with decoupling.

## Buffering (from the stability test)

Measured WiFi jitter up to ~120 ms on a 10 ms block. With the ADC the stream is exactly
realtime (you cannot "run ahead" like the proxy does), so a **ring buffer** in DRAM is needed to
absorb the stalls:

- target: **~150-200 ms**, about 35 KB at 16-bit/44.1k stereo
- the classic ESP32 has ~150 KB of free DRAM, more than enough
- I2S DMA fills the ring, the HTTP task drains it toward Sonos

## Turntable control (idea, not yet implemented)

Optional future feature: drive the turntable from the web UI. Scope kept minimal:

- **Power on/off** and **play/stop** by wiring **in parallel to the turntable's existing
  buttons/switches** - a relay or optocoupler across each existing switch, so the ESP32
  "presses" them electrically. No mechanical actuators, no arm cueing.
- **Auto play/stop of Sonos via signal detection**: the audio engine measures the PCM RMS
  from the ADC; when signal appears (needle down) it triggers `Play`, when it stays silent
  for a few seconds (end of side / arm lifted) it triggers `Stop`. Software only.

Safety: only switch the **low-voltage** side (the turntable's external PSU). Never switch
mains directly with hobby wiring. Verify the PSU output first (voltage, AC vs DC): DC is a
simple MOSFET, AC needs a relay.

Planned interface: a `turntable` component with GPIO relay/opto control and an
`/api/turntable?power=on|off` / `?speed=33|45` endpoint, mirrored by a web button.

## Phono preamp

A turntable cartridge produces a **phono** signal (millivolts, with an RIAA curve to correct).
The ADC expects **line level**. A phono preamp is therefore required between the two (external,
or built into the turntable). Never wire the cartridge directly to the ADC input.
