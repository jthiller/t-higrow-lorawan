#pragma once

// 1 prints status and readings on the serial port at 115200 baud. Set to 0 for
// deployment to save a little power.
#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 1
#endif

// ---------------------------------------------------------------------------
// Board: LilyGO T-Higrow with the LoRa shield
// ---------------------------------------------------------------------------
// Pin numbers are from LilyGO's own example
// (github.com/Xinyuan-LilyGO/LilyGo-HiGrow, src/configuration.h).

#define PIN_I2C_SDA      25   // on-board BH1750 light sensor (0x23), not used here
#define PIN_I2C_SCL      26
#define PIN_DHT          16   // DHT11 air temperature and humidity
#define PIN_BATTERY_ADC  33   // battery, through a divider
#define BATTERY_DIVIDER   2   // the battery voltage is this many times the pin's
#define PIN_SALT_ADC     34   // fertiliser ("salt") probe, not used here
#define PIN_SOIL_ADC     32   // capacitive soil probe: a higher reading is drier
#define PIN_SENSOR_POWER  4   // HIGH powers the sensors
#define PIN_USER_BUTTON  35   // not used here

// LoRa shield: an SX1276-family radio
#define PIN_RADIO_SCLK    5
#define PIN_RADIO_MISO   19
#define PIN_RADIO_MOSI   27
#define PIN_RADIO_CS     18
#define PIN_RADIO_RESET  23
#define PIN_RADIO_DIO0   14
#define PIN_RADIO_DIO1   13

// The air sensor fitted to the board: DHT11 on most T-Higrows.
#define DHT_TYPE DHT11

// Time for the DHT11 to start after sensor power is switched on. It answers
// nothing for about a second.
#define SENSOR_WARMUP_MS 1500

// ---------------------------------------------------------------------------
// LoRaWAN
// ---------------------------------------------------------------------------

// RadioLib band name: US915, EU868, AU915, AS923, IN865, KR920 or CN470.
#define LORAWAN_REGION   US915
#define LORAWAN_SUBBAND  2    // Helium uses US915 sub-band 2. Use 0 for regions without sub-bands
#define LORAWAN_FPORT    1

// Time between uplinks. The board sleeps in between.
#ifndef UPLINK_INTERVAL_MINUTES
#define UPLINK_INTERVAL_MINUTES 60
#endif

// Transmit power, in dBm. The sensor manages its own power between these two
// limits, the same way the Makerfabs firmware does. It starts at the minimum,
// asks the network with every uplink how much signal the gateway had to spare,
// and moves a step at a time. The radio's amplifier covers 2 to 17 dBm, and it
// draws most of the board's peak current, so raise the ceiling only for a
// sensor far from any gateway.
#ifndef MIN_TX_POWER_DBM
#define MIN_TX_POWER_DBM 2
#endif
#ifndef MAX_TX_POWER_DBM
#define MAX_TX_POWER_DBM 14
#endif
#define TX_POWER_STEP_DB 3

// Raise the power a step each time this many readings in a row bring no
// downlink. Silence is weak evidence on Helium, where downlinks go missing
// while every uplink arrives, so a short run of it says little.
#ifndef RAISE_POWER_AFTER_SILENT_UPLINKS
#define RAISE_POWER_AFTER_SILENT_UPLINKS 10
#endif

// Link margin is how far above the weakest receivable signal the best gateway
// heard an uplink, in dB, as the network reports it. Below the low mark the
// power goes up a step; at or above the high mark, twice running, it comes
// down a step. Keep the two at least two power steps apart.
#ifndef LINK_MARGIN_LOW_DB
#define LINK_MARGIN_LOW_DB  7
#endif
#ifndef LINK_MARGIN_HIGH_DB
#define LINK_MARGIN_HIGH_DB 13
#endif

// Straight after joining, the network has settings to hand the sensor (a
// faster data rate, a longer receive delay) and can only do so in reply to an
// uplink. Until the first downlink is heard, send up to this many readings
// this far apart instead of waiting a full interval between tries.
#define SETTLE_UPLINKS 8
#define SETTLE_INTERVAL_SECONDS 30

// Extra time, in milliseconds, that each receive window stays open, half of it
// before the moment a reply is due. Left to itself RadioLib opens the window
// about 2 ms early, which is roughly how long this radio takes to start
// listening, and at the fastest data rate the part of a reply it can lock on
// to lasts only 2 ms. On the test board that heard one reply in six, although
// the replies themselves started within a millisecond or two of when they
// were due. With 20 the window opens 12 ms early.
#ifndef RX_WINDOW_EXTRA_MS
#define RX_WINDOW_EXTRA_MS 20
#endif

// If this many uplinks in a row get no downlink at all, assume the network has
// dropped the session and join again. Uplinks still reach the network when
// only the downlinks are failing, and a rejoin needs a downlink to succeed, so
// keep this long: 168 is a week of hourly readings.
#define REJOIN_AFTER_SILENT_UPLINKS 168
