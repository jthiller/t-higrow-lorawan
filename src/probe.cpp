// Bring-up sketch: prints which sensors are on this T-Higrow, whether the
// radio answers, and the raw analog readings. It sends nothing over the air.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <DHT.h>
#include "config.h"

static DHT dht11(PIN_DHT, DHT11);
static DHT dht22(PIN_DHT, DHT22);

static uint8_t radioRead(uint8_t reg) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_RADIO_CS, LOW);
  SPI.transfer(reg & 0x7F);
  const uint8_t value = SPI.transfer(0);
  digitalWrite(PIN_RADIO_CS, HIGH);
  SPI.endTransaction();
  return value;
}

static void scan(TwoWire& bus, const char* name) {
  Serial.printf("I2C %s:", name);
  bool any = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() == 0) {
      Serial.printf(" 0x%02X", addr);
      any = true;
    }
  }
  Serial.println(any ? "" : " nothing");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nT-Higrow probe");

  pinMode(PIN_SENSOR_POWER, OUTPUT);
  digitalWrite(PIN_SENSOR_POWER, HIGH);
  delay(1000);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  scan(Wire, "on-board");
  Wire1.begin(21, 22);
  scan(Wire1, "socket (pins 21 and 22)");

  pinMode(PIN_RADIO_CS, OUTPUT);
  digitalWrite(PIN_RADIO_CS, HIGH);
  pinMode(PIN_RADIO_RESET, OUTPUT);
  digitalWrite(PIN_RADIO_RESET, LOW);
  delay(10);
  digitalWrite(PIN_RADIO_RESET, HIGH);
  delay(20);
  SPI.begin(PIN_RADIO_SCLK, PIN_RADIO_MISO, PIN_RADIO_MOSI, PIN_RADIO_CS);
  Serial.printf("Radio version register 0x42 = 0x%02X (SX1276/7/8/9 read 0x12)\n", radioRead(0x42));
  Serial.printf("Radio op-mode register 0x01 = 0x%02X\n", radioRead(0x01));

  dht11.begin();
  dht22.begin();
}

void loop() {
  delay(2500);
  const float t11 = dht11.readTemperature();
  const float h11 = dht11.readHumidity();
  delay(2500);
  const float t22 = dht22.readTemperature();
  const float h22 = dht22.readHumidity();
  Serial.printf("DHT as DHT11: %.1f C %.1f %%   as DHT22: %.1f C %.1f %%\n", t11, h11, t22, h22);
  Serial.printf("soil ADC %u (mV %u)   salt ADC %u   battery pin mV %u (battery mV %u)   button %d\n",
                analogRead(PIN_SOIL_ADC), analogReadMilliVolts(PIN_SOIL_ADC), analogRead(PIN_SALT_ADC),
                analogReadMilliVolts(PIN_BATTERY_ADC), analogReadMilliVolts(PIN_BATTERY_ADC) * BATTERY_DIVIDER,
                digitalRead(PIN_USER_BUTTON));
}
