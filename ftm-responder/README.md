# FTM Responder (ESP32-C6)

A standalone Wi-Fi access point that answers **FTM (802.11mc fine timing
measurement)** ranging requests and can broadcast **its location** in its
beacons. Place it at a known spot and the [wearable Wi-Fi
scanner](../wifi-scanner) shows its measured distance (green/yellow) and
location (blue).

- **SSID:** `OnePingOnly-XXXX` (last 4 hex digits of the AP MAC).
- **20 MHz channel only**, as is best practice on 2.4 GHz.
- **Ranging needs no connection.** The password (`onepingonly`) just keeps
  devices off the network.

## Boards and builds

Each board has its own config file and build folder:

| Board | Config | Build | Notes |
|---|---|---|---|
| Seeed XIAO ESP32-C6, built-in antenna | `sdkconfig.xiao-int` | `build-xiao-int` | e.g. OnePingOnly-4BB1 |
| Seeed XIAO ESP32-C6, external antenna | `sdkconfig.xiao-ext` | `build-xiao-ext` | e.g. OnePingOnly-4B0D |
| Other C6 boards (WS2812 on GPIO8) | `sdkconfig` | `build` | |

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
cd "D:\ESP32 Projects\ftm-responder"
idf.py -B build-xiao-ext -D SDKCONFIG=sdkconfig.xiao-ext -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.xiao-ext" -p COMx flash monitor
```

On the XIAO the firmware drives the RF switch (GPIO3 low = powered, GPIO14
high = external u.FL antenna, low = built-in). Without that the radio runs
~20 dB weak.

## Status LED

| | WS2812 RGB | Single LED (XIAO, GPIO15) |
|---|---|---|
| Running | green blink every 2 s | short blink every 2 s |
| Ranging | dim cyan, bright flash per request | on, flicker per request |

Ranging is detected by watching for FTM Request frames (ESP-IDF has no
responder-side event); the log shows "Ranging started by …" and a count per
minute.

## Location in the beacon

Two vendor-specific elements (ESP-IDF allows two per frame); format in
`main/loc_ie.h`, shared with the scanner:

1. **Position — HPE Aruba element** (`00:0B:86`, type 1, **subtype 8 LCI**,
   as Aruba APs send with *advertise-location*), carrying an **RFC 3825**
   binary geo LCI: latitude/longitude (9.25 fixed point with resolution bits
   from the accuracy), altitude (22.8 fixed, meters), datum WGS84.
2. **Civic address and FTM calibration — Espressif-OUI element**: the civic
   address in **RFC 4776** format (the form RFC 5580 carries: country code +
   CAtype fields), the FTM distance correction, and the position source.

The position comes from a **GPS module** if one is attached and has a fix
(updated every 5 s), otherwise from the **fixed position** in menuconfig.

The responder also answers **ANQP** (802.11u) queries: GAS Initial Requests
for ANQP Capability List (257), **AP Geospatial Location (265)** and **AP
Civic Location (266)** get a GAS Initial Response with the same position and
address. It can't advertise an Interworking element (ESP-IDF allows only the
two added beacon elements above), so clients that only query APs advertising
Interworking won't ask. Each query is logged with the response in hex, and
flashes the LED.

Full byte-level format, with a decoded capture:
[docs/beacon-location-elements.md](docs/beacon-location-elements.md).

## Settings (`menuconfig` → FTM Responder)

| Setting | Notes |
|---|---|
| SSID prefix, password, channel, TX power | |
| FTM distance correction (cm) | Broadcast in the beacon; the scanner subtracts it. See calibration. |
| Status LED | WS2812 / single LED / none, GPIO, active-low, byte order, brightness |
| Antenna | Board default, XIAO built-in, XIAO external |
| Location in beacon → Fixed position | Latitude, longitude (decimal degrees), altitude, accuracy |
| Location in beacon → Civic address | Country, state (CAtype 1), city (3), street (34), house number (19), postal code (24), building (25), floor (27), room (28), name (23), extra (22) |
| Location in beacon → GPS module | UART pins (XIAO: D7 = GPIO17 RX, D6 = GPIO16 TX), baud (0 = auto) |

## Calibration

The antenna and its cable add delay (coax ≈ 1.5 cm of apparent distance per
cm of cable), so each antenna setup needs its own correction:

1. Put the scanner exactly 1 m from the responder (line of sight).
2. Open the responder's Wi-Fi details on the scanner; the log shows
   `ftm: … X m (raw R m, correction C m)`.
3. Set **FTM distance correction** to `raw − 100` cm and reflash.

The scanner also has a **per-boot ranging offset** that is the same for every
responder and shifts by up to about ±0.9 m each time it restarts, so measure
over several scanner restarts and use the average. Measured 2026-10-16 at 1 m
over four restarts: built-in antenna ≈ 2.93 m raw (correction 195), external
antenna ≈ 2.46 m raw (correction 145).

The element format is documented in [docs/beacon-location-elements.md](docs/beacon-location-elements.md).

ESP-IDF's own responder offset (`esp_wifi_ftm_resp_set_offset`) made no
consistent difference in testing, so calibration travels in the beacon
instead.
