// ChirpStack v4 payload codec for the t-higrow-lorawan firmware.
//
// The payload has the same layout as the Makerfabs firmware's
// (lsms092d-lorawan), so both boards can share a dashboard.
//
// Uplink payload, fPort 1, 8 bytes, big-endian:
//   0     status (bit 0: air sensor did not respond; bits 1-4: the board
//         restarted since the last uplink, by power-on, reset pin, brown-out
//         or watchdog respectively; bits 5-7: transmit power step, 1 being
//         the firmware's minimum power)
//   1-2   battery, mV (about 4800 with USB plugged in)
//   3-4   soil moisture, raw ADC count (0-4095, higher is drier)
//   5-6   air temperature, signed, 0.1 degC
//   7     air relative humidity, 0.5 % steps
//
// Soil calibration is per probe. Set two device variables in ChirpStack
// (Device > Configuration > Variables) to the raw readings you see with the
// probe in dry air and standing in water:
//   soil_air    default 3410
//   soil_water  default 1500

function decodeUplink(input) {
  var b = input.bytes;
  // The network's own traffic (answers to its commands) arrives with no
  // payload. That is not an error.
  if (b.length === 0) {
    return { data: {} };
  }
  if (input.fPort !== 1 || b.length !== 8) {
    return { errors: ["expected 8 bytes on fPort 1"] };
  }

  var vars = input.variables || {};
  var air = Number(vars.soil_air) || 3410;
  var water = Number(vars.soil_water) || 1500;

  var soilRaw = (b[3] << 8) | b[4];
  var moisture = ((air - soilRaw) / (air - water)) * 100;
  moisture = Math.max(0, Math.min(100, moisture));

  var data = {
    battery_v: ((b[1] << 8) | b[2]) / 1000,
    soil_raw: soilRaw,
    soil_moisture_pct: Math.round(moisture * 10) / 10,
    air_sensor_ok: (b[0] & 0x01) === 0
  };

  // Present only on the first uplink after the board restarts.
  var causes = [];
  if (b[0] & 0x02) causes.push("power-on");
  if (b[0] & 0x04) causes.push("reset pin");
  if (b[0] & 0x08) causes.push("brown-out");
  if (b[0] & 0x10) causes.push("watchdog");
  if (causes.length) {
    data.restarted = causes.join(", ");
  }

  // The power this uplink was sent at. The 2 and the 3 are MIN_TX_POWER_DBM
  // and TX_POWER_STEP_DB in the firmware's include/config.h; change them here
  // if you change them there.
  var powerStep = b[0] >> 5;
  if (powerStep) {
    data.tx_power_dbm = 2 + (powerStep - 1) * 3;
  }

  if (data.air_sensor_ok) {
    var temp = (b[5] << 8) | b[6];
    if (temp & 0x8000) {
      temp -= 0x10000;
    }
    data.air_temperature_c = temp / 10;
    data.air_humidity_pct = b[7] / 2;
  }

  return { data: data };
}
