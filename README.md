# OnePingOnly: ESP32-C6 Wi-Fi scanner and FTM responders

Two ESP-IDF (v6.1) projects for the ESP32-C6:

| Project | What it is |
|---|---|
| [wifi-scanner](wifi-scanner) | Wearable Wi-Fi scanner on the Waveshare ESP32-C6-LCD-1.47. Features listed below. |
| [ftm-responder](ftm-responder) | Headless FTM responder access points (Seeed XIAO ESP32-C6), `OnePingOnly-XXXX`. They answer FTM ranging requests and broadcast their location and an FTM calibration correction, in beacon vendor elements and over ANQP. |

The wifi-scanner offers:
- **Signal display:** an RSSI color on an addressable LED, and a menu of modes:
  Wi-Fi, GPS with a sky plot, BLE devices, 2.4 GHz band usage, Zigbee and
  files.
- **Logging:** a microSD CSV log.
- **Ranging and location:** FTM ranging, vendor lookup, and decoding of the
  location that access points broadcast (beacon vendor elements and ANQP).
- **GPS sources:** a GPS module, a computer over USB, or a phone over Bluetooth
  LE; it can also share its position to a phone.

The location element format (for decoder authors) is in
[ftm-responder/docs/beacon-location-elements.md](ftm-responder/docs/beacon-location-elements.md).
`loc_ie.h` is shared and kept identical in both projects.

Each project's README has the build and flash commands.
