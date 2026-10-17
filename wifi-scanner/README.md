# Wearable Wi-Fi Scanner (ESP32-C6)

Scans Wi-Fi continuously and shows signal strength as a color on WS2812 / WS2814
addressable pixels. On the Waveshare display board it adds a menu of modes:
Wi-Fi list, GPS, Bluetooth LE devices, 2.4 GHz band usage, Zigbee/Thread, and a
file browser for the microSD scan logs.

Based on the ESP-IDF Wi-Fi scan example. Built with ESP-IDF v6.1.

## LED

The LED shows either the strongest access point in range, or one specific
SSID, mapped onto a color scale (red→yellow→green by default; −90 dBm is the
bottom of the scale, −40 dBm the top). Colors fade smoothly and readings are
smoothed so the light doesn't flicker as you move. At startup the LEDs flash
**red, green, blue**; if the colors appear in a different order, change *Pixel
type / byte order*.

The LED goes dark while a mode that needs the radio is open (BLE devices,
2.4 GHz band, Zigbee, Wi-Fi sharing), since Wi-Fi scanning pauses.

## Button (BOOT, GPIO9)

| Where | Press | Action |
|---|---|---|
| Any mode | Long (0.8 s) | Open the mode menu |
| Menu | Short / Long | Next mode / open it (closes by itself after 15 s) |
| Wi-Fi / BLE list | Short / Double | Next page (dots show the page) / select, starting with the first item on the page |
| Wi-Fi / BLE details | Short / Double | Next network or device (the LED follows it) / back to the list |
| Files | Short / Double | Next file / share over Wi-Fi |
| Sharing | Short | Stop sharing |

Choosing **Wi-Fi scan** in the menu returns the LED to automatic and re-sorts
the list. After leaving Wi-Fi details the LED stays on the chosen network
(header `LED:<ssid>`, row highlighted). The RESET button restarts the chip and can't be read by software.
Without a display (DevKit): short = next network, long = automatic + re-sort.

For testing, the same presses can be sent over USB: `#BTN S`, `#BTN L` or
`#BTN D` on the serial port.

## Screen modes (Waveshare board)

Every screen has a status bar at the bottom: a **globe** when a GPS module is
connected (gray = no fix, red = 2D or poor, orange, yellow, green = good,
cyan = differential), with fix type, satellites and estimated accuracy; and a
green **SD card** icon while logging.

### Wi-Fi scan
SSID, channel and RSSI for up to 8 networks, with a bar that is empty at
−82 dBm and full at −35 dBm or stronger. Access points sharing an SSID share a
row: a dot after the name for each BSSID (white = in the latest scan, gray =
seen in the last 5 minutes but not now; each BSSID keeps its first-seen slot),
their bars are overlaid (each bar ends at a dark tick, in its own color), and
the channel and dBm are the strongest one's. `ch1+` means they span more than
one channel. The list is sorted when first built, then each network keeps its
row; a network that disappears stays dimmed (`--`) for 5 minutes. Hidden
networks and anything below −82 dBm aren't listed, but are logged
(`not shown: ...`) and still count for the LED.

**Details** (double press): approximate distance (large), RSSI, channel and
width, security, Wi-Fi standards (b/g/n/ax, WPS), vendor, FTM, country, and
every BSSID in first-seen order with its channel, RSSI and distance, or how
long ago it was last seen.

- **Vendor** comes from the strongest BSSID's MAC prefix (IEEE registry, about
  40,000 prefixes, from Wireshark's `manuf` file; table in `main/oui_db.c`).
  `(v)` means a *virtual* BSSID: a locally administered address, as access
  points use for extra SSIDs, looked up with that bit cleared. Randomized
  addresses (phone hotspots) usually show as unknown.
- **FTM** shows whether the access point advertises 802.11mc fine timing
  measurement (responder, initiator, or both) — green if it can answer
  ranging requests.
- **FTM ranging:** while details are open, BSSIDs that advertise FTM responder
  are ranged between scans (no connection needed). Measured distances show
  with one decimal: **green** = uncertainty under 3 m, **yellow** = less
  certain; the tag beside the RSSI says `FTM +-x m`. White `~x m` is the RSSI
  estimate. The distance is the median of the last 5 sessions, and the
  uncertainty their spread.
- For a known-position ranging beacon, flash a spare ESP32-C6 with the
  [FTM responder](../ftm-responder) project (SSID `OnePingOnly-XXXX`).
- **Network name colors** in the list: **blue** = the beacon carries a
  location, **green** = advertises FTM responder, yellow = the network the LED
  follows, white = neither.
- **ANQP location**: while a network's details are open, the scanner also
  sends ANQP (802.11u) queries to its BSSIDs between scans, asking for AP
  Geospatial Location (265) and AP Civic Location (266). Answers merge into
  the same location display, marked `ANQP` on the Position line. APs that
  don't answer three times are skipped for 5 minutes.
- **Beacon location**: the HPE Aruba location element (LCI subtype 8 carrying
  an 802.11 LCI report, or GPS-ellipse subtype 9, as Aruba APs send with
  *advertise-location*) and the responder's civic address (RFC 4776 / RFC
  5580) show in blue on the details page: coordinates, altitude, accuracy,
  source (GPS/fixed) and the address. A responder can also broadcast its FTM
  distance correction, which is subtracted from measured distances. Beacons
  are read with the Wi-Fi sniffer during scans (ESP-IDF's vendor-element
  callback doesn't report scan results); the sniffer pauses whenever Wi-Fi
  scanning does.

### Approximate distance
Estimated from RSSI with a log-distance path-loss model (Wi-Fi: −32 dBm at
1 m, exponent 3.0; BLE: the advertised TX power − 41 dB at 1 m, or −59 dBm if
not advertised, exponent 2.5). Walls, bodies and antenna orientation easily
shift RSSI by 10 dB, so read it as near / a room away / far. Constants are in
`main/distance.h`.

### GPS
Latitude/longitude in large digits. Below them, a **sky plot** sits beside the
fix summary:
- **Sky plot:** north at the top. The outer ring is the horizon, the inner ring
  45° up, and the center straight overhead. Satellites in the fix are colored
  by signal strength (green ≥ 40 dB-Hz, yellow, orange, red); the rest are gray.
- **Fix summary:** satellites used/seen, fix type with estimated accuracy, HDOP
  and update rate.

Then altitude, speed, heading and UTC time. Wi-Fi scanning continues.

**Short press: phone GPS on/off** (see below). The header says "GPS phone on"
while it's enabled.

### Phone GPS link over Bluetooth
The scanner can act as a **Bluetooth GNSS receiver for a phone**, and a phone
can also supply the position when the scanner has none.

1. On the GPS screen, press the button to turn the link on. The setting is
   saved and is off by default. The scanner advertises as
   **WiFiScan-XXXX** (Bluetooth LE, Nordic UART Service).
2. Connect from the phone. The first time, the scanner shows a **6-digit
   passkey**; type it on the phone. The phones stay paired (bonded) across
   restarts.

**Scanner → phone:**
- **What is sent:** the scanner sends every NMEA sentence from its own
  position source (GPS module, USB or the demo simulation) as notifications on
  the TX characteristic (`6E400003-B5A3-F393-E0A9-E50E24DCCA9E`). This is the
  "BLE serial" / Nordic UART form many GNSS receivers use, so phone GNSS apps
  that accept a BLE receiver of that kind can read it.
- **Pairing:** subscribing makes an unpaired phone pair first. Nothing is sent
  until the link is encrypted.
- **No echo:** a position that came from the phone is never sent back to it.

**Phone → scanner:**
- **The page:** open [tools/phone-gps.html](tools/phone-gps.html) on the phone
  and tap **Connect to scanner**. This needs Chrome on Android, or the Bluefy
  browser on iPhone, since Safari has no Web Bluetooth.
- **What is sent:** the page sends GGA/RMC/GSA built from the phone's location.
  The footer shows "phone ~N m", and the header shows "phone 3D fix".
- **Access:** only a paired phone can write. The RX characteristic
  (`6E400002-…`) needs an encrypted, authenticated link, so an unpaired
  device's writes are refused.
- **Other apps:** any app that writes NMEA there works, after pairing.
- **Precedence:** a GPS module or USB source takes precedence over the phone.
- **Hosting the page:** Web Bluetooth only works on HTTPS pages, so host
  `phone-gps.html` somewhere with HTTPS (for example GitHub Pages). Opened
  from the scanner's own share access point (plain HTTP), it won't work.
- **Scan speed:** while phone GPS is on (advertising or connected), Wi-Fi scans
  take about 4.9 s instead of 2 s, because Wi-Fi and Bluetooth share the
  radio. That's why it is a switch.

### GPS over USB
A computer can also send NMEA on the USB port: each line starting with `$` is
used like a module's. A module wins over USB, and USB wins over a phone.
[tools/nmea_usb.py](tools/nmea_usb.py) does the sending, and prints the
scanner's log while it runs:

```powershell
python tools/nmea_usb.py COM5 --gps COM7                       # forward a GPS on the computer
python tools/nmea_usb.py COM5 --fixed 41.948437 -87.655334 --alt 181
python tools/nmea_usb.py COM5 --file track.nmea                # replay a log, one fix per second
```

It shares the port with `sdlog.py`, so close one before using the other.

### BLE devices
Bluetooth LE devices heard in the last 30 s, strongest first, 7 per page:
name (or maker + end of address), maker, RSSI bar. **Details** (double
press): approximate distance, smoothed RSSI with a bar, maker, address and
type, vendor (public addresses only), advertised TX power, connectable,
advert count, last seen. While a
device is selected **the LED follows its signal**, which helps track it down. `rnd` marks a random, often rotating,
address. The C6 has no Bluetooth Classic, so classic-only devices don't appear.

### 2.4GHz band
RF energy across 2400–2484 MHz: average per point (bar) with a peak-hold tick,
and a waterfall below (newest at the top, ~12 sweeps per second). Cyan markers
are the BLE advertising channels (37/38/39), orange markers Wi-Fi 1/6/11. It
uses the 802.15.4 radio's energy detector at 16 points, 5 MHz apart, because
the Bluetooth controller doesn't report per-channel activity; it measures all
RF energy, not only Bluetooth.

### Zigbee
Energy per 802.15.4 channel (11–26), and networks that answer beacon requests:
Zigbee (PAN ID, `ZB PRO`), Thread (network name if broadcast), or other
802.15.4. A green `J` means permit-join is open; dots on the bar are the
devices that answered. A white cap on a channel's bar marks a network there.

### Files
The microSD log files, newest first (red dot = being recorded). Double press
**shares over Wi-Fi**: the scanner becomes an access point
(`WiFiScanner-XXXX`, random password shown on screen); join it from a phone or
laptop and open `http://192.168.4.1` to download. (AirDrop is Apple-only and
Bluetooth file transfer needs Bluetooth Classic, so neither is possible.)

## Boards

| Board | Config | Build folder | Notes |
|---|---|---|---|
| ESP32-C6 DevKit (LED only) | `sdkconfig` | `build` | Built-in RGB LED on GPIO8 mirrors the pixel color |
| Waveshare ESP32-C6-LCD-1.47 | `sdkconfig.waveshare-lcd` | `build-lcd` | ST7789 172×320, RGB LED on GPIO8, microSD, 8 MB flash (4 MB app partition) |

### Wiring

| Part | Connection |
|---|---|
| Pixels DIN (data in) | **GPIO2** (configurable) |
| GPS module TX | **GPIO17** (ESP RX; labeled **RXD** on the Waveshare board) |
| GPS module RX | **GPIO16** (ESP TX, optional; labeled **TXD** on the Waveshare board) |
| GPS module VCC / GND | 3V3 / GND (check your module's supply range) |

- Any NMEA GPS works (e.g. u-blox M8/M10 breakouts). The baud rate is
  detected automatically; the GPS also sets the clock, so files on the card get
  real timestamps.
- Pixels: the C6 outputs 3.3 V data. 5 V WS2812s usually accept it, but a
  74AHCT125 level shifter makes it reliable. Put a 330–470 Ω resistor in series
  with the data line near the first pixel, and a 100–470 µF capacitor across the
  pixel supply. WS2814 strips are usually 12 V or 24 V: give them their own
  supply and tie its ground to the board's GND.

## Build and flash

Activate ESP-IDF first. On this machine (installed with EIM):

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
```

**DevKit:**

```powershell
idf.py -p COM4 flash monitor
```

**Waveshare ESP32-C6-LCD-1.47:**

```powershell
idf.py -B build-lcd -D SDKCONFIG=sdkconfig.waveshare-lcd -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.waveshare-lcd -p COM5 flash monitor
```

Exit the monitor with `Ctrl+]`. Only one program can use a COM port at a time,
so close any open monitor before flashing or downloading logs.

## Demo mode (for video)

A separate build that tours the modes by itself:
1. Wi-Fi list, paging through it.
2. Details of the first two networks with FTM or a location (the
   OnePingOnly responders).
3. GPS: a simulated multi-GNSS receiver (GPS, GLONASS, Galileo, BeiDou)
   walking a loop beside Wrigley Field, with the sky plot. It's used only when
   no module or phone is sending. The clock starts from the build's UTC time
   (or the real time, if already set).
4. BLE list and one device's details.
5. 2.4 GHz band.
6. Zigbee.
7. Files, including the share screen.
8. Back to the start, repeating.

Mode changes go through the menu, as a real user's would. A loop takes about
two minutes. Press the button to stop it (that press is otherwise ignored),
and reset to start it again. Settings under **Demo**:
- **Redact:** a word to replace in SSIDs and BLE names, any case (for example,
  a family name in home network names). It's replaced with "Test" as names
  arrive, so the screen, serial log and microSD log all match. Set it in your
  local `sdkconfig.waveshare-demo`, which isn't committed.
- **Pace:** percent of the default time on each screen.

```powershell
idf.py -B build-demo -D SDKCONFIG=sdkconfig.waveshare-demo -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.waveshare-lcd;sdkconfig.defaults.demo" -p COM5 flash
```

Flash the normal `build-lcd` build to go back.

## microSD log (Waveshare board)

With a **FAT32**-formatted card in the slot, each boot writes a new
`SCANnnnn.CSV` with one row per access point per scan:

```
uptime_s,scan,ssid,bssid,channel,rssi,auth,listed,lat,lon,alt_m,speed_kmh,course,fix,sats,hdop,utc
12.4,3,Example-IoT,02:00:5e:10:00:01,1,-51,WPA2/WPA3,1,47.6062100,-122.3320700,56.2,3.1,270,3,9,0.9,2026-10-16T18:04:11Z
```

`listed` is 1 if the access point is shown on screen (not hidden, ≥ −82 dBm).
The GPS columns are empty without a module; with a module but no fix, only
`fix` (0), `sats` and `utc` are filled. The file is synced after every scan, so
pulling the power loses at most one scan. Without a card the scanner runs
normally and checks for one every minute. Logging pauses with Wi-Fi scanning.

exFAT is not supported (ESP-IDF's FatFS is built without it). Cards of 64 GB
and up usually come exFAT-formatted and must be reformatted as FAT32.

### Downloading logs over USB

The ESP32-C6 has no USB OTG, so the card can't appear as a USB drive. Instead
the firmware answers commands on the USB serial port. Close any monitor, then:

```powershell
python tools\sdlog.py COM5 list
python tools\sdlog.py COM5 get all logs      # new/changed files into .\logs
python tools\sdlog.py COM5 get SCAN0003.CSV
python tools\sdlog.py COM5 del SCAN0001.CSV
```

Downloads are checked with a CRC-32. Use the ESP-IDF Python (activate ESP-IDF
first) or any Python with `pyserial`. Or use **Files → double press** to
download over Wi-Fi.

## Configuration

All options are build-time settings under **Wearable Wi-Fi Scanner** in
menuconfig. Use the same `-B`/`-D` arguments as above for the Waveshare board,
for example `idf.py -B build-lcd -D SDKCONFIG=sdkconfig.waveshare-lcd menuconfig`.

| Menu | Settings |
|---|---|
| (top level) | Signal source (strongest AP or a specific SSID), not-found color, missed scans, how long networks stay listed (5 min) |
| Button | Enable, GPIO, long-press time |
| Scanning | Active/passive, time per channel, pause between scans, channel list (e.g. `1,6,11`), hidden networks, RSSI smoothing, per-AP logging |
| Color scale | RSSI range; red→yellow→green, thermal, single color, or a custom three-color gradient; gamma correction |
| LED output | Data GPIO, pixel count, byte order (WS2812 GRB, RGB, WS2814 WRGB, RGBW, SK6812 GRBW), white channel use, brightness, fade time, built-in LED mirror and its byte order |
| Display | Enable, bar range (−82 to −35 dBm), backlight, panel size/offsets, inversion, BGR, SPI clock, pins |
| GPS module | Enable, UART pins, baud rate (0 = automatic) |
| Extra modes | BLE devices, 2.4 GHz band, Zigbee, Files (each can be left out) |
| microSD log | Enable, CS pin, SPI clock, USB download commands |
| SPI bus | MOSI / SCLK / MISO shared by the display and the card |

A full Wi-Fi scan of all 13 channels takes about 2 seconds. Lower *time per
channel* or scan fewer channels for faster updates.

## Files

| File | Purpose |
|---|---|
| `main/scan.c` | Wi-Fi setup, scan loop, LED fading, pause/resume |
| `main/ap_rows.c` | The stable network list (rows, BSSID dots, aging) |
| `main/led_pixels.c` | WS2812/WS2814/SK6812 driver on the RMT peripheral |
| `main/color_scale.c` | RSSI → color mapping, gamma, brightness |
| `main/gfx.c` | ST7789 drawing (bands, text, icons) |
| `main/ui.c` | Modes, menu, status bar, button |
| `main/ui_*.c` | One file per screen |
| `main/gps.c` | NMEA parser (module on a UART with auto baud, phone, demo simulation), satellites |
| `main/ble_gps.c` | Phone GPS: BLE peripheral, Nordic UART Service, passkey pairing |
| `main/ble_host.c` | NimBLE host shared by the BLE scanner and phone GPS |
| `tools/phone-gps.html` | Phone page: Web Bluetooth + location → NMEA |
| `tools/nmea_usb.py` | Send NMEA to the scanner over USB (forward a GPS, fixed position, replay) |
| `main/ble_scan.c` | BLE scanner (NimBLE) |
| `main/zb_scan.c` | 802.15.4 energy and beacon scanner |
| `main/sdlog.c` | microSD CSV log and USB commands |
| `main/share.c` | Wi-Fi access point + download page |
| `main/font8x16.h`, `main/icons.c` | Generated font (Consolas) and icons |
| `tools/sdlog.py` | Download logs over USB |
| `partitions-8mb.csv` | Partition table for the Waveshare board |
