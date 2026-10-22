# DMX Node (ESP32-S3 / ESP32 / ESP32-C6)

A small theatrical lighting node. It receives DMX512 over Wi-Fi (sACN or
Art-Net) or from a wired RS485 DMX input, and drives:

- **Port A** (UART + RS485): wired DMX **input**, or a second DMX **output**
- **Port B** (UART + RS485): DMX **output**
- **Pixels** (SPI): WS2812 / SK6812 (data only) or APA102 / SK9822 (data + clock), up to 4 universes
- **PWM** (LEDC): up to 8 LED dimmer channels (8- or 16-bit) or RC servos

Setup is over a lean **web page** or **Bluetooth LE**, with the same JSON for both.

> **Status: scaffold.** It builds for all three chips with ESP-IDF v6.1 but
> hasn't been run on hardware yet. TODOs in the code mark the known gaps; see
> [Next steps](#next-steps).

## Chip choice

The node needs two UARTs that aren't the log console, plus Wi-Fi, BLE, SPI and PWM.

| Chip | DMX UARTs | Notes |
|---|---|---|
| **ESP32-S3** (default) | UART1, UART2 | Log over native USB. Dual core: Wi-Fi on core 0, DMX output timing on core 1. 8 PWM channels. |
| ESP32 (classic) | UART1, UART2 | Log on UART0 via the USB-serial chip. Dual core. |
| ESP32-C6 | UART1, UART0 | Only two UARTs, so port B takes UART0 (log over native USB). Single core. 6 PWM channels. |

## Wiring

Each DMX port needs an RS485 transceiver (MAX485, SN75176, or for isolation
an ADM2587E / ISO3082) and a 5-pin XLR (pin 1 common, 2 data−, 3 data+).
Tie DE and /RE together to the direction GPIO; the firmware drives it high
for output and low for input. Terminate the input with 120 Ω (or provide a
switch for it).

Default pins (set in `idf.py menuconfig` → *DMX Node*):

| Function | ESP32-S3-DevKitC-1 | ESP32-C6-DevKitC-1 | ESP32-DevKitC |
|---|---|---|---|
| Port A TX / RX / DIR | 17 / 18 / 8 | 0 / 1 / 2 | 17 / 16 / 4 |
| Port B TX / DIR | 15 / 16 | 3 / 10 | 25 / 26 |
| Pixel data / clock | 11 / 12 | 7 / 6 | 23 / 18 |
| PWM 1-8 | 4,5,6,7,1,2,42,41 | 18-23 | 32,33,27,14,13,21,22,19 |

Use a 74AHCT125 or similar to bring pixel data and clock up to 5 V. For PWM
LEDs, drive logic-level MOSFETs. For servos, power them separately and share
ground.

## Build and flash

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
cd "D:\ESP32 Projects\dmx-node"
idf.py set-target esp32s3
idf.py -p COMx flash monitor
```

On the first build the component manager fetches cJSON (ESP-IDF 6 no
longer ships it).

## First setup

1. With no Wi-Fi network configured, the node starts an access point named
   `DMX-Node-XXXX` (password `dmxsetup`). Join it and open
   http://192.168.4.1/.
2. Enter the show network's name and password, choose the input and outputs,
   and press **Save and restart**.
3. If the node can't join its network, it brings the setup access point back
   up after about 5 tries. It keeps trying to join every 30 s.

The page shows the node's address and, for each universe, the source and
the time since the last frame.

## How the settings fit together

- **Input** is one of `sacn`, `artnet` or `dmx`. With `dmx`, port A is the
  input and its universe number is the one the wired data goes into.
- Every output names a **universe number**. With Wi-Fi input, outputs on the
  same universe show the same data. With wired input, set the outputs to port
  A's universe number to repeat it.
- **Universe numbers** are used as they arrive: sACN universes 1-63999, and
  Art-Net 15-bit port addresses (net:subnet:universe, so 0:0:0 is 0 and
  0:1:0 is 16).
- **Pixels** start at the start address in their universe and continue at
  channel 1 of the following universes (170 RGB or 128 RGBW per universe).
- **PWM** channels are consecutive from the start address, two channels each
  (coarse, fine) in 16-bit mode. Servo pulses run from `min_us` at 0 to
  `max_us` at full.
- **On data loss**, `hold` keeps the last look and `blackout` goes to zero
  2.5 s after the last frame. Servos never move until data arrives.
- **sACN priority** is respected: lower priority sources are ignored while a
  higher one is sending, and a source that sends "stream terminated" is
  released at once.
- Settings changes take effect on restart. The node restarts itself after
  saving.

## Settings JSON

`GET /api/config` returns this; `POST /api/config` accepts any subset of it
(missing fields keep their values):

```json
{
  "name": "DMX-Node-8641",
  "wifi":   {"ssid": "ShowNet", "pass": "secret"},
  "source": "sacn",
  "loss":   "hold",
  "fps":    40,
  "a":   {"mode": "out", "univ": 2},
  "b":   {"mode": "out", "univ": 1},
  "pix": {"type": "ws2812", "order": "grb", "univ": 3, "addr": 1, "count": 50},
  "pwm": {"mode": "led", "fine": false, "univ": 1, "addr": 101, "count": 4,
          "min_us": 1000, "max_us": 2000}
}
```

| Field | Values |
|---|---|
| `source` | `sacn`, `artnet`, `dmx` |
| `loss` | `hold`, `blackout` |
| `a.mode` | `off`, `in` (only with `source: dmx`), `out` |
| `b.mode` | `off`, `out` |
| `pix.type` | `off`, `ws2812`, `apa102` |
| `pix.order` | `rgb`, `grb`, `bgr`, `rgbw`, `grbw` |
| `pwm.mode` | `off`, `led`, `servo` |

Reads never include the Wi-Fi password; they show `"pass_set": true`
instead. Combinations that can't work are corrected rather than refused (for
example `a.mode: in` without `source: dmx` turns port A off).

`GET /api/status` returns the address, uptime, firmware version and, for each
universe, its source, frame count and time since the last frame.

## Bluetooth LE

The node advertises its name and this service:

| | UUID | |
|---|---|---|
| Service | `6e0d0001-0000-4c1e-9d3a-a5c0ffee0d12` | |
| Settings | `6e0d0002-0000-4c1e-9d3a-a5c0ffee0d12` | read / write, the JSON above |
| Status | `6e0d0003-0000-4c1e-9d3a-a5c0ffee0d12` | read, `{"ip":"…","station":true}` |

A generic app such as nRF Connect can read and write it. Writes over the MTU
are sent as long (prepared) writes, up to 512 bytes. To set up Wi-Fi from a
phone, write `{"wifi":{"ssid":"ShowNet","pass":"secret"}}`.

While Bluetooth is on, Wi-Fi has to share the radio and keep power saving on,
which adds latency and drops multicast frames. So Bluetooth turns off 5
minutes after boot (configurable, and it waits until no phone is connected),
then Wi-Fi power saving is turned off. Bluetooth stays on while no Wi-Fi
network is configured.

## Testing without a console

`tools/send_test.py` sends a fade or a chase:

```
python tools/send_test.py sacn 1
python tools/send_test.py artnet 0 --ip 192.168.4.1
python tools/send_test.py sacn 3 --chase
```

Software consoles that send sACN or Art-Net, such as QLC+, also work.

## Code layout

| File | Role |
|---|---|
| `app_main.c` | Startup: settings, outputs, network, input, setup interfaces |
| `node_config.c` | Settings struct, JSON in and out, NVS storage |
| `universe.c` | Universe table between inputs and outputs: priority, loss timing, notifications |
| `net.c` | Wi-Fi station with fallback setup access point |
| `sacn.c` | E1.31 receiver (multicast join per universe, sequence and priority) |
| `artnet.c` | Art-Net 4 ArtDmx receiver and ArtPollReply |
| `dmx_port.c` | DMX512 UART output (break by inverting TX) and input (frames cut at UART break events) |
| `pixels.c` | SPI pixel output (WS2812 encoded at 3.2 MHz, or APA102) |
| `pwm_out.c` | LEDC dimmer and servo output |
| `web_config.c`, `web/index.html` | Setup page and JSON API |
| `ble_config.c` | NimBLE GATT setup service |

## Next steps

- Hardware bring-up: check the break and mark-after-break timing on a scope,
  and test the input against a real console and a cheap USB DMX dongle.
- sACN/Art-Net **merging** (HTP across sources at the same priority), and
  per-source sequence tracking.
- A **dimming curve** for PWM LEDs, and higher LED PWM resolution on chips
  that allow it.
- Change settings **without restarting**.
- **Security**: a setup password for the web page, and BLE pairing (bonding
  with a passkey) before settings can be written.
- **Firmware update** from the web page (the partition table already has two
  app slots).
- **Ethernet** (W5500 over SPI, or the ESP32's RMII MAC) for rigs where
  Wi-Fi isn't trusted.
- **RDM** on port A, and a status LED.
- Art-Net **ArtAddress** / sACN universe discovery, so consoles can set the
  node's addresses.
