// LoRaWAN firmware for the LilyGO T-Higrow with the LoRa shield.
//
// Each wake-up: read the sensors, restore the LoRaWAN session (or join), send
// one uplink, listen for a downlink, and go back into deep sleep. The board
// restarts from setup() on every wake-up, so nothing lives in loop().
//
// The payload is the same 8 bytes the Makerfabs firmware (lsms092d-lorawan)
// sends, so one decoder and one dashboard serve both boards.

#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <DHT.h>
#include <RadioLib.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "config.h"
#include "devices.h"

#if DEBUG_SERIAL
  #define LOG(...) Serial.printf(__VA_ARGS__)
#else
  #define LOG(...)
#endif

// Time between uplinks. A bench or range-test build shortens it with
// -D LINK_TEST_SECONDS=n: an uplink every n seconds. Nothing else changes.
#ifdef LINK_TEST_SECONDS
  #define UPLINK_INTERVAL_SECONDS (LINK_TEST_SECONDS)
#else
  #define UPLINK_INTERVAL_SECONDS (60UL * UPLINK_INTERVAL_MINUTES)
#endif

// A bench build can start higher with -D START_TX_POWER_DBM=n, to watch the
// power step down.
#ifndef START_TX_POWER_DBM
  #define START_TX_POWER_DBM MIN_TX_POWER_DBM
#endif

#if MIN_TX_POWER_DBM < 2 || MAX_TX_POWER_DBM > 17 || MAX_TX_POWER_DBM < MIN_TX_POWER_DBM
  #error "Transmit power limits must satisfy 2 <= MIN_TX_POWER_DBM <= MAX_TX_POWER_DBM <= 17."
#endif
#if START_TX_POWER_DBM < MIN_TX_POWER_DBM || START_TX_POWER_DBM > MAX_TX_POWER_DBM
  #error "START_TX_POWER_DBM must lie between MIN_TX_POWER_DBM and MAX_TX_POWER_DBM."
#endif
#if TX_POWER_STEP_DB < 1 || LINK_MARGIN_HIGH_DB < LINK_MARGIN_LOW_DB + 2 * TX_POWER_STEP_DB
  #error "LINK_MARGIN_HIGH_DB must be at least two power steps above LINK_MARGIN_LOW_DB, or the power will hunt."
#endif

// ---------------------------------------------------------------------------
// State kept through deep sleep
// ---------------------------------------------------------------------------
// RTC memory survives deep sleep. Anything else resets it: power-on, the reset
// button, a brown-out. After one of those the sensor joins again.

RTC_DATA_ATTR static uint8_t savedSession[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
RTC_DATA_ATTR static uint16_t silentUplinks = 0;   // uplinks in a row with no downlink
RTC_DATA_ATTR static uint8_t failedJoins = 0;      // join requests in a row with no answer
RTC_DATA_ATTR static uint8_t settling = 0;         // quick readings left after a join, until a downlink is heard
RTC_DATA_ATTR static uint8_t txPower = START_TX_POWER_DBM;   // what the link asks for, in dBm
RTC_DATA_ATTR static uint8_t strongAnswers = 0;    // LinkCheck answers in a row with margin to spare
RTC_DATA_ATTR static uint8_t restartFlags = 0;     // why the board last restarted, until an uplink reports it

// How many times in a row the board has browned out without managing an
// uplink in between. This one is kept in memory that a reset does not clear,
// though a loss of power does; the second byte is its complement, so the
// random contents after a power-up are not mistaken for a count.
RTC_NOINIT_ATTR static uint8_t brownOuts;
RTC_NOINIT_ATTR static uint8_t brownOutsCheck;

// ---------------------------------------------------------------------------
// Transmit power
// ---------------------------------------------------------------------------
// The sensor chooses its own transmit power, as the Makerfabs firmware does.
// It starts at MIN_TX_POWER_DBM and asks the network for a LinkCheck answer
// with every uplink: a short downlink saying how much signal the best gateway
// had to spare. Power goes up a step when that margin is thin, when no
// downlink at all has been heard for RAISE_POWER_AFTER_SILENT_UPLINKS uplinks
// in a row, or when a join request gets no answer. It comes down a step after
// two answers in a row with margin to spare.

static void raisePower() {
  strongAnswers = 0;
  txPower = (uint8_t)min(MAX_TX_POWER_DBM, txPower + TX_POWER_STEP_DB);
}

static void lowerPower() {
  txPower = (uint8_t)max(MIN_TX_POWER_DBM, txPower - TX_POWER_STEP_DB);
}

static void linkCheckDone(uint8_t margin) {
  if (margin < LINK_MARGIN_LOW_DB) {
    raisePower();
  } else if (margin < LINK_MARGIN_HIGH_DB) {
    strongAnswers = 0;
  } else if (++strongAnswers >= 2) {
    strongAnswers = 0;
    lowerPower();
  }
}

// The radio, with one change: whatever power the LoRaWAN layer asks for, it
// transmits at the power this sensor has chosen. RadioLib sets the power
// through this one call for every frame it sends, join requests and its own
// answers to network commands included.
//
// The network's ADR still sets the data rate, and its power commands are
// acknowledged, but they stop here. A server starts a new device at the band's
// maximum power and walks it down one small step per answered uplink, which at
// one reading an hour takes days.
class LadderRadio : public SX1276 {
 public:
  using SX1276::SX1276;
  int16_t setOutputPower(int8_t) override {
    return SX1276::setOutputPower((int8_t)txPower);
  }
};

static LadderRadio radio(new Module(PIN_RADIO_CS, PIN_RADIO_DIO0, PIN_RADIO_RESET, PIN_RADIO_DIO1));
static LoRaWANNode node(&radio, &LORAWAN_REGION, LORAWAN_SUBBAND);
static DHT dht(PIN_DHT, DHT_TYPE);

static const uint64_t joinEui = JOIN_EUI;
static const uint64_t devEui = DEV_EUI;
static const uint8_t appKey[] = { APP_KEY };

// ---------------------------------------------------------------------------
// What RadioLib asks the application to keep
// ---------------------------------------------------------------------------
// Two buffers. The session goes in RTC memory. The join counters (DevNonce and
// JoinNonce) must survive a power cut as well, or the network refuses the next
// join request as a replay, so they go in flash.

static const char STORE_NAMESPACE[] = "lorawan";
static const char STORE_KEY[] = "persist";

static void storeSession(uint8_t* buf, size_t len) {
  memcpy(savedSession, buf, min(len, sizeof(savedSession)));
}

static void restoreSession(uint8_t* buf, size_t len) {
  memcpy(buf, savedSession, min(len, sizeof(savedSession)));
}

static void storeJoinCounters(uint8_t* buf, size_t len) {
  Preferences store;
  store.begin(STORE_NAMESPACE, false);
  store.putBytes(STORE_KEY, buf, len);
  store.end();
}

static void restoreJoinCounters(uint8_t* buf, size_t len) {
  Preferences store;
  store.begin(STORE_NAMESPACE, false);
  // Nothing is saved before the first join. RadioLib rejects a buffer of zeros
  // and starts its counters afresh.
  if (!store.isKey(STORE_KEY) || store.getBytes(STORE_KEY, buf, len) != len) {
    memset(buf, 0, len);
  }
  store.end();
}

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

// Waits with the processor in light sleep, which leaves the pins as they are.
// For waits where the exact length does not matter.
static void nap(uint32_t milliseconds) {
#if DEBUG_SERIAL
  Serial.flush();
#endif
  esp_sleep_enable_timer_wakeup((uint64_t)milliseconds * 1000ULL);
  esp_light_sleep_start();
}

[[noreturn]] static void sleepFor(uint32_t seconds) {
  radio.sleep();
  // The ESP32's pins float in deep sleep. Hold the radio deselected and the
  // sensors switched off.
  digitalWrite(PIN_RADIO_CS, HIGH);
  digitalWrite(PIN_SENSOR_POWER, LOW);
  gpio_hold_en((gpio_num_t)PIN_RADIO_CS);
  gpio_hold_en((gpio_num_t)PIN_SENSOR_POWER);
  gpio_deep_sleep_hold_en();

  LOG("Sleeping s %lu\n", (unsigned long)seconds);
#if DEBUG_SERIAL
  Serial.flush();
#endif
  esp_deep_sleep((uint64_t)seconds * 1000000ULL);
}

// Sleeps until the next reading is due, allowing for the time spent awake.
[[noreturn]] static void sleepUntilNextReading() {
  const uint32_t interval = settling ? SETTLE_INTERVAL_SECONDS : UPLINK_INTERVAL_SECONDS;
  const uint32_t awake = millis() / 1000;
  sleepFor(awake + 10 < interval ? interval - awake : 10);
}

// Rests after a failure that may repeat: 1 minute after the first in a row,
// then 2, 4 and so on up to an hour.
[[noreturn]] static void restAfter(uint8_t failuresInARow) {
  const uint32_t minutes = min(60, 1 << (failuresInARow - 1));
  LOG("Resting minutes: %lu\n", (unsigned long)minutes);
  sleepFor(minutes * 60);
}

// ---------------------------------------------------------------------------
// Restart cause
// ---------------------------------------------------------------------------

// Notes why the board restarted, for the status byte of the next uplink: bit
// values as in the Makerfabs firmware, 1 power-on, 4 brown-out, 8 watchdog or
// crash. (2, reset pin, is never set here: the ESP32 reports its reset button
// as power-on.) A wake from deep sleep is not a restart. Returns true when the
// board has just browned out.
static bool noteRestart() {
  const esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    return false;
  }
  if (brownOutsCheck != (uint8_t)~brownOuts) {
    brownOuts = 0;
  }
  const char* name = "other";
  restartFlags = 0x00;
  switch (reason) {
    case ESP_RST_POWERON:  restartFlags = 0x01; name = "power-on"; break;
    case ESP_RST_BROWNOUT: restartFlags = 0x04; name = "brown-out"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_PANIC:    restartFlags = 0x08; name = "watchdog or crash"; break;
    default: break;
  }
  LOG("Reset cause: %s\n", name);
  brownOuts = reason == ESP_RST_BROWNOUT ? (uint8_t)min(7, brownOuts + 1) : 0;
  brownOutsCheck = (uint8_t)~brownOuts;
  return reason == ESP_RST_BROWNOUT;
}

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------

struct Reading {
  uint16_t batteryMillivolts;
  uint16_t soilRaw;         // 12-bit ADC count, higher is drier
  int16_t tempDeciC;        // 0.1 degC
  uint8_t humidityHalfPct;  // 0.5 % steps
  bool airOk;
};

static uint16_t averageOf(uint8_t pin, bool millivolts) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 32; i++) {
    sum += millivolts ? analogReadMilliVolts(pin) : analogRead(pin);
    delay(2);
  }
  return (uint16_t)(sum / 32);
}

static void takeReading(Reading& r) {
  digitalWrite(PIN_SENSOR_POWER, HIGH);
  dht.begin();
  nap(SENSOR_WARMUP_MS);

  r.soilRaw = averageOf(PIN_SOIL_ADC, false);
  r.batteryMillivolts = averageOf(PIN_BATTERY_ADC, true) * BATTERY_DIVIDER;

  r.airOk = dht.read();
  if (!r.airOk) {
    nap(2100);   // a DHT11 answers at most once every two seconds
    r.airOk = dht.read();
  }
  // These two return the reading just taken.
  const float t = dht.readTemperature();
  const float h = dht.readHumidity();
  r.tempDeciC = r.airOk ? (int16_t)lroundf(t * 10) : 0;
  r.humidityHalfPct = r.airOk ? (uint8_t)constrain(lroundf(h * 2), 0L, 200L) : 0;

  digitalWrite(PIN_SENSOR_POWER, LOW);
}

static void printReading(const Reading& r) {
  LOG("Battery mV %u  soil ADC %u", r.batteryMillivolts, r.soilRaw);
  if (r.airOk) {
    LOG("  temp x10 %d  RH x2 %u\n", r.tempDeciC, r.humidityHalfPct);
  } else {
    LOG("  air sensor did not respond\n");
  }
}

// ---------------------------------------------------------------------------
// LoRaWAN
// ---------------------------------------------------------------------------

// Starts the radio. If it does not answer, sleeps until the next reading.
static void startRadio() {
  SPI.begin(PIN_RADIO_SCLK, PIN_RADIO_MISO, PIN_RADIO_MOSI, PIN_RADIO_CS);
  ConfigLoRa_t radioConfig;
  radioConfig.frequency = RADIOLIB_UNIT_MEGA(915);   // LoRaWAN sets the real frequency for every frame
  const int16_t state = radio.begin(radioConfig);
  if (state != RADIOLIB_ERR_NONE) {
    LOG("Radio did not start, code %d\n", state);
    sleepUntilNextReading();
  }
}

// Restores the session kept through deep sleep, or sends one join request.
// Returns true once the sensor has a session.
static bool activate() {
  node.beginOTAA(joinEui, devEui, nullptr, appKey);   // no NwkKey: LoRaWAN 1.0.x
  node.scanGuardA = RX_WINDOW_EXTRA_MS;
  node.setCallbackStorePersistence(storeJoinCounters);
  node.setCallbackRestorePersistence(restoreJoinCounters);
  node.setCallbackStoreSession(storeSession);
  node.setCallbackRestoreSession(restoreSession);
  // Loading fails harmlessly when nothing is saved yet, which leaves no session.
  if (node.loadBuffers() == RADIOLIB_ERR_NONE && node.activateOTAA() == RADIOLIB_LORAWAN_SESSION_RESTORED) {
    return true;
  }

  LOG("Joining at dBm %u\n", txPower);
  const int16_t state = node.activateOTAA();
  if (state != RADIOLIB_LORAWAN_NEW_SESSION) {
    // A join request that gets no answer is retried a step stronger.
    raisePower();
    LOG("No join accept, code %d, next try at dBm %u\n", state, txPower);
    return false;
  }
  LOG("Joined, DevAddr %lX\n", (unsigned long)node.getDevAddr());
  failedJoins = 0;
  silentUplinks = 0;
  settling = SETTLE_UPLINKS;
  return true;
}

// The status byte reports transmit power as a step, where step 1 is
// MIN_TX_POWER_DBM and each step above it adds TX_POWER_STEP_DB.
static uint8_t powerStep() {
  return (uint8_t)min(7, (txPower - MIN_TX_POWER_DBM) / TX_POWER_STEP_DB + 1);
}

// Sends one frame, asking for a LinkCheck answer, and acts on what comes back.
// Returns RadioLib's result: the receive window a downlink arrived in, 0 for
// none, or a negative error code.
static int16_t exchange(const uint8_t* data, size_t length) {
  node.sendMacCommandReq(RADIOLIB_LORAWAN_MAC_LINK_CHECK);
  LoRaWANEvent_t up;
  const int16_t state = node.sendReceive(data, length, LORAWAN_FPORT, false, &up);
  if (state == RADIOLIB_ERR_MAC_ANSWERS_PENDING) {
    return state;   // nothing was sent
  }

  if (state < RADIOLIB_ERR_NONE) {
    LOG("Uplink failed, code %d\n", state);
  } else {
    LOG("Sent #%lu DR%u at dBm %u%s\n", (unsigned long)node.getFCntUp(), up.datarate, txPower,
        state == 0 ? ", no downlink" : state == 1 ? ", downlink in RX1" : ", downlink in RX2");
  }

  if (state > 0) {
    silentUplinks = 0;
    settling = 0;
    uint8_t margin = 0;
    uint8_t gateways = 0;
    if (node.getMacLinkCheckAns(&margin, &gateways) == RADIOLIB_ERR_NONE) {
      LOG("  link margin dB %u, gateways %u\n", margin, gateways);
      linkCheckDone(margin);
    }
  } else {
    if (settling) {
      settling--;
    }
    if (++silentUplinks % RAISE_POWER_AFTER_SILENT_UPLINKS == 0) {
      raisePower();
    }
  }
  return state;
}

void setup() {
#if DEBUG_SERIAL
  Serial.begin(115200);
  LOG("\nSoil sensor LoRaWAN (T-Higrow)\n");
#endif
  // Release the pins held through deep sleep.
  gpio_hold_dis((gpio_num_t)PIN_RADIO_CS);
  gpio_hold_dis((gpio_num_t)PIN_SENSOR_POWER);
  gpio_deep_sleep_hold_dis();
  pinMode(PIN_SENSOR_POWER, OUTPUT);
  digitalWrite(PIN_SENSOR_POWER, LOW);

  // A supply that sags under the radio's load resets the board, and without a
  // pause it would restart and try again every few seconds, draining the
  // battery further and flooding the network with join requests. Rest first.
  if (noteRestart()) {
    startRadio();   // the reset did not reach the radio, which may still be awake
    LOG("Supply sagged under load. ");
    restAfter(brownOuts);
  }

  // Read the sensors first, with the radio idle, then switch them off.
  Reading r;
  takeReading(r);
  printReading(r);

  startRadio();
  if (!activate()) {
    if (failedJoins < 7) {
      failedJoins++;
    }
    restAfter(failedJoins);
  }

  // Payload, 8 bytes, big-endian:
  //   0     status flags (bit 0: air sensor did not respond; bits 1-4: the
  //         board restarted since the last uplink, by power-on, reset pin,
  //         brown-out or watchdog respectively; bits 5-7: transmit power step)
  //   1-2   battery, mV
  //   3-4   soil moisture, raw ADC count
  //   5-6   air temperature, signed, 0.1 degC
  //   7     air relative humidity, 0.5 % steps
  uint8_t payload[8];
  payload[0] = (r.airOk ? 0x00 : 0x01) | (restartFlags << 1) | (powerStep() << 5);
  payload[1] = highByte(r.batteryMillivolts);
  payload[2] = lowByte(r.batteryMillivolts);
  payload[3] = highByte(r.soilRaw);
  payload[4] = lowByte(r.soilRaw);
  payload[5] = highByte((uint16_t)r.tempDeciC);
  payload[6] = lowByte((uint16_t)r.tempDeciC);
  payload[7] = r.humidityHalfPct;

  // Battery level for the network's DevStatus request: 0 means external power
  // (USB plugged in), otherwise 1 (empty) to 254 (full) across 3.3 V to 4.2 V.
  const long level = map(r.batteryMillivolts, 3300, 4200, 1, 254);
  node.setDeviceStatus(r.batteryMillivolts > 4300 ? 0 : (uint8_t)constrain(level, 1L, 254L));

  int16_t state = exchange(payload, sizeof(payload));
  if (state == RADIOLIB_ERR_MAC_ANSWERS_PENDING) {
    // The network's commands have queued more answers than fit beside a
    // reading at this data rate. Send them on their own first.
    LOG("Answering network commands first\n");
    exchange(nullptr, 0);
    state = exchange(payload, sizeof(payload));
  }
  if (state >= RADIOLIB_ERR_NONE) {
    restartFlags = 0;   // reported; later uplinks carry it only after another restart
    brownOuts = 0;
    brownOutsCheck = (uint8_t)~brownOuts;
  }

  if (silentUplinks >= REJOIN_AFTER_SILENT_UPLINKS) {
    LOG("No downlinks for too long, joining again next time\n");
    // With no saved session the next wake-up joins, which resets the counters.
    memset(savedSession, 0, sizeof(savedSession));
  }

  sleepUntilNextReading();
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
