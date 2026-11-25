# LoRaWAN firmware for the LilyGO T-Higrow with the LoRa shield

Firmware that turns a LilyGO **T-Higrow** (ESP32) fitted with LilyGO's **LoRa shield** into a LoRaWAN 1.0.4 Class A device. It reports soil moisture, air temperature, air humidity and battery voltage, and deep-sleeps between readings.

It is the companion to [lsms092d-lorawan](https://github.com/jthiller/lsms092d-lorawan), the firmware for the Makerfabs soil sensor, and sends the same 8-byte payload, so one decoder and one dashboard serve both boards. The two differ underneath: this one is built on RadioLib, which the ESP32 has room for.

**Tested on:** one board (ESP32-D0WDQ6, DHT11, SX1276-family shield), US915, on the Helium network through a ChirpStack v4 console. Other regions and network servers should work with the changes described below but have not been tried.

## Contents

- [What you need](#what-you-need)
- [1. Connect the board](#1-connect-the-board)
- [2. Install the toolchain](#2-install-the-toolchain)
- [3. Register the device](#3-register-the-device)
- [4. Add your keys](#4-add-your-keys)
- [5. Build and flash](#5-build-and-flash)
- [6. Calibrate the soil probe](#6-calibrate-the-soil-probe)
- [Configuration](#configuration)
- [Payload format](#payload-format)
- [Power](#power)
- [Link test mode](#link-test-mode)
- [Probe sketch](#probe-sketch)
- [Why RadioLib here](#why-radiolib-here)
- [Troubleshooting](#troubleshooting)
- [Board notes](#board-notes)

## What you need

- A LilyGO T-Higrow with the LoRa shield for your region's band (868 or 915 MHz), and its antenna.
- A USB cable. The board has its own USB-to-serial chip (CP2104), so no programmer is needed.
- A LoRaWAN network server and a gateway in range.
- [PlatformIO Core](https://platformio.org/install/cli).

## 1. Connect the board

Fit the antenna before powering the board: transmitting without one can damage the radio. Then plug in USB. The board appears as a serial port, `/dev/cu.usbserial-…` on macOS.

## 2. Install the toolchain

```bash
pip install platformio
```

The first build downloads the ESP32 toolchain and the libraries (RadioLib 8.0.0 and Adafruit's DHT library).

## 3. Register the device

These are the ChirpStack v4 names; other servers have the same settings under other labels.

### Device profile

| Setting | Value |
|---|---|
| Region | US915 (or your region) |
| MAC version | LoRaWAN 1.0.4 |
| Regional parameters revision | RP002-1.0.4 |
| ADR algorithm | Default |
| Supports OTAA | Yes |
| Class B, Class C | No |
| Expected uplink interval | 3600 seconds |
| Codec | JavaScript functions: paste [codec/chirpstack.js](codec/chirpstack.js) |

On Helium, set the profile's RX1 delay to 3 seconds as well.

### Device

Create a device with that profile and generate a DevEUI and an AppKey. Leave the JoinEUI at all zeros unless your server issued one.

## 4. Add your keys

```bash
cp include/devices.example.h include/devices.h
```

Edit `include/devices.h` and enter the DevEUI and AppKey exactly as the console shows them, most significant byte first. The file is listed in `.gitignore`, so the keys stay out of version control.

## 5. Build and flash

```bash
pio run -t upload
```

```bash
pio device monitor
```

A first boot looks like this:

```text
Soil sensor LoRaWAN (T-Higrow)
Reset cause: power-on
Battery mV 4814  soil ADC 3463  temp x10 247  RH x2 88
Joining at dBm 2
Joined, DevAddr 78000005
Sent #0 DR0 at dBm 2, downlink in RX1
  link margin dB 27, gateways 1
Sleeping s 3592
```

- The board then sleeps until the next reading and starts again from the top, without the `Reset cause` and `Joining` lines: the session is kept through sleep.
- `at dBm` is the transmit power of that uplink, and `link margin` is the network's answer to it. See [Transmit power](#transmit-power).
- `No join accept` now and then is normal. The board rests for a minute and tries again a step stronger. The rest doubles with each miss in a row, up to an hour, so a network that is not answering does not drain the battery.
- Until the first downlink after a join, readings come 30 seconds apart rather than an hour, so the network can hand over its settings.
- `Answering network commands first` means the network's commands have queued more answers than fit beside a reading at the current data rate. The board sends them on their own and then the reading.

## 6. Calibrate the soil probe

The probe reports a raw number that is higher when drier. Note the `soil ADC` value with the probe in dry air and again standing in a glass of water up to the line on the board. On the test board the air reading is about 3460; the water reading has not been taken yet.

Put the two numbers wherever the percentage is worked out. With the ChirpStack codec that is two device variables, `soil_air` and `soil_water` (Device > Configuration > Variables).

## Configuration

Settings are in [include/config.h](include/config.h).

| Setting | Default | Meaning |
|---|---|---|
| `DEBUG_SERIAL` | 1 | Print status and readings on the serial port at 115200 baud. Set to 0 for deployment |
| `DHT_TYPE` | `DHT11` | The air sensor fitted to the board |
| `LORAWAN_REGION` | `US915` | RadioLib band name |
| `LORAWAN_SUBBAND` | 2 | Sub-band for US915 and AU915. Helium uses 2. Use 0 for other regions |
| `UPLINK_INTERVAL_MINUTES` | 60 | Time between readings |
| `MIN_TX_POWER_DBM` | 2 | Transmit power a sensor starts at and comes back down to |
| `MAX_TX_POWER_DBM` | 14 | Ceiling on transmit power, at most 17 |
| `TX_POWER_STEP_DB` | 3 | Size of one power step |
| `RAISE_POWER_AFTER_SILENT_UPLINKS` | 10 | Readings in a row without a downlink before the power goes up a step |
| `LINK_MARGIN_LOW_DB`, `LINK_MARGIN_HIGH_DB` | 7, 13 | Below the low mark the power goes up; at or above the high mark, twice running, it comes down |
| `SETTLE_UPLINKS`, `SETTLE_INTERVAL_SECONDS` | 8, 30 | After a join, up to this many readings this far apart until a downlink is heard |
| `RX_WINDOW_EXTRA_MS` | 20 | Extra time each receive window stays open. See [Receive windows](#receive-windows) |
| `SENSOR_WARMUP_MS` | 1500 | Time the sensors are powered before they are read |
| `REJOIN_AFTER_SILENT_UPLINKS` | 168 | Readings in a row without any downlink before joining again |

The pin numbers at the top of the file are from LilyGO's own example and should not need changing.

## Payload format

One uplink per reading, fPort 1, 8 bytes, big-endian. It is the same layout the Makerfabs firmware sends.

| Byte | Field | Notes |
|---|---|---|
| 0 | Status | Bit 0: the air sensor did not respond. Bits 1–4: the board restarted since the last uplink, by power-on, reset pin, brown-out or watchdog respectively. Bits 5–7: transmit power step, where step 1 is 2 dBm and each step above it adds 3 dB |
| 1–2 | Battery | mV. About 4800 with USB plugged in |
| 3–4 | Soil moisture | Raw ADC count, 0–4095, higher is drier |
| 5–6 | Air temperature | Signed, 0.1 °C |
| 7 | Air humidity | 0.5 % steps |

The ESP32 does not tell a press of the reset button apart from power being applied; both report as power-on.

## Power

The board wakes, powers its sensors for about two seconds, sends one uplink, listens for the reply, and goes into deep sleep until the next reading. A wake-up lasts about 5 seconds when the reply arrives, and about 7 when none does. To draw less while awake, the processor runs at 80 MHz and dozes while the sensors warm up. The sleep current of this board has not been measured.

### Transmit power

The sensor works out the lowest transmit power that serves its own link, the same way the Makerfabs firmware does:

- **It starts at `MIN_TX_POWER_DBM`** (2 dBm), for the join request as well.
- **Every uplink asks the network for a LinkCheck answer**, a short downlink that says how far above the weakest receivable signal the best gateway heard it. That is the link margin.
- **Power goes up one step** (`TX_POWER_STEP_DB`, 3 dB) when the margin is below `LINK_MARGIN_LOW_DB`, when `RAISE_POWER_AFTER_SILENT_UPLINKS` readings in a row bring no downlink at all, or when a join request gets no answer.
- **Power comes down one step** after two answers in a row with a margin of `LINK_MARGIN_HIGH_DB` or more.
- **It never exceeds `MAX_TX_POWER_DBM`** (14 dBm).

The network's ADR still chooses the data rate, but its power commands are acknowledged and not applied. The firmware wraps the radio so that every frame, join requests and the library's own answers to network commands included, goes out at the sensor's own power. A network server starts a new device at the band's maximum power and walks it down one small step per answered uplink. On the test bench the server's first command was for maximum power, and at one reading an hour it would have taken days to come down.

On the test bench, in range of one Helium hotspot, a board started at 8 dBm stepped down to 5 and then 2 dBm within five uplinks, and at 2 dBm the network still reported about 15 dB of margin.

**After a brown-out.** A supply that sags under the radio restarts the board. It then rests before touching the radio again, a minute after the first brown-out and longer after each one in a row, so a tired battery does not turn into a stream of join requests. The serial port prints `Supply sagged under load` and the next uplink reports the brown-out. Unlike the Makerfabs firmware, this one does not also lower its power ceiling: after a restart it is already back at the minimum. Only the one-minute rest has been tried, with a simulated brown-out; the longer rests have not.

### Receive windows

Left to itself, RadioLib opens each receive window about 2 ms before a reply is due. That is roughly how long this radio takes to start listening, and at the fastest data rate the part of a reply it can lock on to lasts only 2 ms. On the test board, with the network's 3-second RX1 delay, that heard one reply in six.

The replies themselves were on time: measured with RadioLib's protocol debugging, each started within a millisecond or two of when it was due. So the window only needs to open earlier. `RX_WINDOW_EXTRA_MS` widens it, half before and half after the due moment. At 20 the window opens 12 ms early and the board heard 13 replies in 14; at 40 it did no better. The cost is about 10 ms more listening per reply.

## Link test mode

For bench and range testing, build with an uplink every few seconds:

```bash
PLATFORMIO_BUILD_FLAGS="-D LINK_TEST_SECONDS=20" pio run -t upload
```

The serial output shows the power used, the link margin, how many gateways heard the uplink, and which receive window the answer arrived in. Add `-D START_TX_POWER_DBM=8` to start higher and watch the power step down. Flash the normal build again afterwards.

## Probe sketch

`pio run -e probe -t upload` flashes a bring-up sketch instead of the firmware. It sends nothing over the air. It prints which I2C devices are present, whether the radio answers, what the DHT sensor reads as a DHT11 and as a DHT22, and the raw soil, salt and battery readings. Use it first on a board that differs from the tested one.

## Why RadioLib here

The Makerfabs firmware uses MCCI LMIC because RadioLib's LoRaWAN stack needs about twice the flash and RAM an ATmega328P has. The ESP32 has room to spare: this firmware uses 28% of the default 1.3 MB application space.

RadioLib 8 hands the application two buffers to keep. This firmware puts them here:

- **The session** goes in RTC memory, which survives deep sleep. A power cut or a reset clears it, and the board joins again.
- **The join counters** go in flash (NVS), so they survive a power cut too. LoRaWAN 1.0.4 requires the DevNonce to keep counting up, and a server refuses a join request that repeats an old one.

## Troubleshooting

**The console shows "DevNonce has already been used" and the board never joins.** The join counters in flash were erased, for example by `pio run -t erase`, and the board is counting up from zero again. Flush the device's nonces on the server (in ChirpStack: Device > Configuration > Flush OTAA device nonces), or wait: each attempt uses the next number.

**It joins but `no downlink` on almost every uplink.** Check that the device profile's RX1 delay matches what your network uses, then try a larger `RX_WINDOW_EXTRA_MS`. A build with `-D RADIOLIB_DEBUG_PROTOCOL=1` prints `Rx window timing` for every reply it hears, which shows how early the window is opening. RadioLib's authors say a need for more than 50 points to a fault somewhere else.

**It prints `Supply sagged under load`.** The battery voltage is dropping when the radio transmits. Charge or replace the battery. The board rests and tries again by itself.

**It prints `air sensor did not respond`.** The DHT11 needs about a second after power is switched on. Raise `SENSOR_WARMUP_MS`. If the board has a different air sensor, run the [probe sketch](#probe-sketch) to see what is fitted.

**The temperature reads high on the bench.** The DHT11 sits on the circuit board and reads a few degrees warm while USB is plugged in. It is also only accurate to about ±2 °C.

**It prints `Radio did not start`.** The shield is not seated, or it is a different shield from the one this was written for. Run the probe sketch: an SX1276-family radio answers 0x12 on its version register.

## Board notes

- **Sensors not used.** The board also has a BH1750 light sensor and a fertiliser ("salt") probe. Neither fits in the shared 8-byte payload, so this firmware leaves them alone.
- **Battery reading.** The battery is read through a 1:2 divider. With USB plugged in and no battery it reads about 4.8 V, and the firmware tells the network the board is on external power.
- **The user button** on GPIO 35 is not used.
- **Pins** are from LilyGO's example: soil probe on GPIO 32, battery on 33, DHT11 on 16, sensor power on 4, and the radio on 5 (SCLK), 19 (MISO), 27 (MOSI), 18 (CS), 23 (reset), 14 (DIO0) and 13 (DIO1).

## References

- [LilyGO's T-Higrow example and pin definitions](https://github.com/Xinyuan-LilyGO/LilyGo-HiGrow)
- [RadioLib](https://github.com/jgromes/RadioLib) and its [LoRaWAN notes](https://github.com/jgromes/RadioLib/wiki/LoRaWAN)
- [lsms092d-lorawan](https://github.com/jthiller/lsms092d-lorawan), the Makerfabs firmware this one follows

## Licence

MIT. See [LICENSE](LICENSE).
