#pragma once

// LoRaWAN credentials for this sensor.
//
// Copy this file to include/devices.h and fill in the values from your network
// server. devices.h is listed in .gitignore so the keys stay out of version
// control.
//
// Enter values exactly as the ChirpStack console displays them (MSB first):
//   DEV_EUI  16 hex digits written as one number, e.g. 0x70B3D57ED0061234
//   APP_KEY  16 bytes, e.g. 0x2B, 0x7E, 0x15, 0x16, ...

// The JoinEUI (called AppEUI before LoRaWAN 1.0.4). It must match the value
// registered for the device on the network server. All zeros is the LoRa
// Alliance's recommendation for devices without an assigned JoinEUI.
#define JOIN_EUI 0x0000000000000000

#define DEV_EUI 0x0000000000000000
#define APP_KEY 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
