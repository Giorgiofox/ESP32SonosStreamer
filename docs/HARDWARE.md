# Hardware - planned wiring (Spike #2)

> Planned **PCM1802 <-> ESP32** wiring. To confirm once the ADC arrives: pin names vary
> slightly between modules.

## PCM1802 (24-bit I2S ADC)

The PCM1802 typically runs in **master mode**: it generates `BCK` and `LRCK` from a system
clock (`SCKI`). Many modules have the oscillator or a divider on board, or require `SCKI` from
the MCU. As a result:

- **ESP32 = I2S RX in slave mode** (receives BCK/LRCK from the PCM1802), or
- **ESP32 = master** and provides `MCLK`/`SCKI` to the PCM1802 (256 x fs = 11.2896 MHz at 44.1k).

To be decided based on the module (`MODE`/`FMT` jumpers). Preferably ESP32 as master providing
MCLK, so the sample rate is driven by the MCU.

## Pinout (indicative)

| PCM1802 | ESP32 (example) | Notes |
|--------:|:----------------|-------|
| VCC     | 3V3             | digital side |
| GND     | GND             | common ground |
| LIN/RIN | <- phono preamp | **line-level** L/R input |
| DOUT    | GPIO25 (I2S DIN)| I2S data to the ESP32 |
| BCK     | GPIO26 (I2S BCK)| bit clock |
| LRCK    | GPIO22 (I2S WS) | word/frame select |
| SCKI    | GPIO0 (I2S MCLK)| system clock (if ESP32 is master) |
| FMT/MODE| jumper          | I2S format, master/slave |

Note: GPIO choices to be confirmed (avoid boot strapping pins). Update after testing.

## Power

- Analog side of the PCM1802 on a **dedicated low-noise LDO**, with the analog ground separated
  from the digital ground and joined at a single point.
- Supply noise is the number one factor that degrades audio: take care with decoupling.

## Buffering (from the stability test)

Measured WiFi jitter up to ~120 ms on a 10 ms block. With the ADC the stream is exactly
realtime (you cannot "run ahead" like the proxy does), so a **ring buffer** in DRAM is needed to
absorb the stalls:

- target: **~150-200 ms**, about 35 KB at 16-bit/44.1k stereo
- the classic ESP32 has ~150 KB of free DRAM, more than enough
- I2S DMA fills the ring, the HTTP task drains it toward Sonos

## Phono preamp

A turntable cartridge produces a **phono** signal (millivolts, with an RIAA curve to correct).
The ADC expects **line level**. A phono preamp is therefore required between the two (external,
or built into the turntable). Never wire the cartridge directly to the ADC input.
