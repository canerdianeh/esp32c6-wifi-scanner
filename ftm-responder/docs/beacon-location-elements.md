# OnePingOnly location information

How OnePingOnly FTM responders (ESP32-C6) publish their location: two
vendor-specific elements in beacons and probe responses, and ANQP answers.
The reference implementation is `main/loc_ie.h` (encoders and decoders).

| | Where | Carries |
|---|---|---|
| A | Beacon: HPE Aruba vendor element (OUI `00:0B:86`), type 1, subtype 8 | Position as an 802.11 LCI report |
| B | Beacon: Espressif-OUI vendor element (`18:FE:34`, type `0xF0`) | Civic address (RFC 4776), FTM distance correction, position source |
| C | ANQP: AP Geospatial Location (265), AP Civic Location (266) | Position and civic address, standard 802.11u query |

## The 802.11 LCI field

Used in Element A and ANQP 265. Sixteen octets carrying the RFC 6225 fields in
**802.11 bit order**: bit B0 is the least significant bit of octet 0, and each
field is least significant bit first.

| Bits | Field | Encoding |
|---|---|---|
| 6 | Latitude Uncertainty | x means 2^(8−x) degrees; 0 = unknown |
| 34 | Latitude | Two's complement, 9.25 fixed point, degrees (+ = north) |
| 6 | Longitude Uncertainty | As above |
| 34 | Longitude | Two's complement, 9.25 fixed point, degrees (+ = east) |
| 4 | Altitude Type | 1 = meters, 0 = none |
| 6 | Altitude Uncertainty | x means 2^(21−x) m; 0 = unknown |
| 30 | Altitude | Two's complement, 22.8 fixed point, meters |
| 3 | Datum | 1 = WGS 84 |
| 1 | RegLoc Agreement | 0 |
| 1 | RegLoc DSE | 0 |
| 1 | Dependent STA | 0 |
| 2 | Version | 1 |

The responder sets the uncertainty to floor(8 − log2(accuracy in degrees)), so
the advertised uncertainty always covers the configured accuracy.

## Element A: position (Aruba LCI)

The HPE Aruba vendor element as Aruba APs send it with *advertise-location*.
After the reserved octet comes an 802.11 LCI report: subelements of
ID (1) / Length (1) / data.

```
DD <len> 00 0B 86 01 08 00 | 00 10 <16-octet LCI>
```

| Field | Size | Value |
|---|---|---|
| Element ID, Length | 2 | `DD`, `0x18` (24) |
| OUI, OUI type | 4 | `00 0B 86`, `01` |
| Subtype | 1 | `08` (LCI) |
| Reserved | 1 | `00` |
| LCI subelement | 18 | ID `00`, length `0x10`, the 16-octet LCI |

Subelement 4 (Z, floor) is not sent yet. The OnePingOnly scanner also reads
Aruba subtype 9 (GPS ellipse: length, subversion, hop, then big-endian IEEE
doubles latitude, longitude, major axis, minor axis, orientation, distance).

## Element B: civic address, FTM correction, position source

```
DD <len> 18 FE 34 F0 <version 02> <TLVs>
```

| Field | Size | Value |
|---|---|---|
| Element ID | 1 | `0xDD` (221, Vendor Specific) |
| Length | 1 | Octets after this field |
| OUI | 3 | `18 FE 34` (Espressif) |
| OUI Type | 1 | `0xF0` |
| Version | 1 | `0x02`. Ignore the element if the version is not 2. |
| TLVs | rest | Type (1) / Length (1) / Value, repeated. Skip unknown types. |

| TLV | Value |
|---|---|
| 2, Civic Address | RFC 4776 civic address (the format RFC 5580 uses): what (1, `1` = the AP), country (2, ISO 3166-1 alpha-2), then CAtype (1) / CAlength (1) / CAvalue (UTF-8), repeated |
| 3, FTM Distance Correction | Signed 16-bit, **big-endian**, centimeters. `corrected = measured − correction` for FTM distances to this BSSID. |
| 4, Position Source | 1 octet: `0` = fixed (configured), `1` = live GPS fix. Describes Element A / ANQP 265 from the same BSSID. |

CAtypes the responder sends (RFC 4776; 34 from RFC 5139): 1 A1 state,
3 A3 city, 19 HNO house number, 22 LOC additional info, 23 NAM name,
24 PC postal code, 25 BLD building, 27 FLR floor, 28 ROOM room, 34 RD street.
Decoders should accept any CAtype.

## C: ANQP (802.11u)

The responder answers **GAS Initial Request** public action frames (category
4, action 10) addressed to its BSSID, with an Advertisement Protocol element
for ANQP and an ANQP Query List. It replies with a **GAS Initial Response**
(action 11), status 0, comeback delay 0, no fragmentation.

All ANQP Info IDs and lengths are **two octets, little-endian**:

| Info ID | Name | On the air | Payload |
|---|---|---|---|
| 256 | Query List | `00 01` | List of wanted Info IDs |
| 257 | ANQP Capability List | `01 01` | 257, plus 265 and 266 when configured |
| 265 | AP Geospatial Location | `09 01` | LCI report: LCI subelement (ID 0, 16 octets) + Usage Rules/Policy (ID 6, 1 octet: `01` = retransmission allowed) |
| 266 | AP Civic Location | `0A 01` | Civic location type (1, `0` = RFC 4776), then Location Civic subelement (ID 0, length): country (2) + CAtype / CAlength / CAvalue entries. No "what" octet. |

The responder can't advertise ANQP in its beacon: ESP-IDF builds the beacon
and allows only two added elements, which carry Elements A and B, so there is
no Interworking element. Clients that only query APs advertising Interworking
won't ask; the OnePingOnly scanner queries regardless.

## Example: OnePingOnly-4BB1 at Wrigley Field

Wrigley Field, 1060 W Addison St, Chicago, IL 60613, configured as a test
location: 41.948437, −87.655334, 181 m, ±10 m. Captured from the responder's
log.

**Element A** (26 octets)

```
dd 18 00 0b 86 01 08 00 00 10 55 62 66 f9 14 15 04 1e 2c d4 01 00 d4 02 00 41
```

| Octets | Meaning |
|---|---|
| `00 10` | LCI subelement, 16 octets |
| `55` … | Latitude uncertainty 21 (≈ 13.6 m) in the low 6 bits |
| `55 62 66 f9 14` | Latitude 0x53E59989 / 2^25 = 41.948437 |
| `15 04 1e 2c d4` | Longitude uncertainty 21; longitude (two's complement) = −87.655334 |
| `01 00 d4 02 00` | Altitude type 1 (meters), uncertainty 0, altitude 0xB500 / 256 = 181.0 m |
| `41` | Datum 1 (WGS 84), version 1 |

**Element B** (74 octets)

```
dd 48 18 fe 34 f0 02 02 3a 01 55 53 01 02 49 4c 03 07 43 68 69 63 61 67 6f
22 0c 57 20 41 64 64 69 73 6f 6e 20 53 74 13 04 31 30 36 30 18 05 36 30 36
31 33 17 0d 57 72 69 67 6c 65 79 20 46 69 65 6c 64 03 02 00 c3 04 01 00
```

| Octets | Meaning |
|---|---|
| `02 3a` | TLV 2 Civic Address, 58 octets |
| `01 55 53` | what = 1 (the AP), country "US" |
| `01 02 49 4c` | A1 = "IL" |
| `03 07 …` | A3 = "Chicago" |
| `22 0c …` | RD (34) = "W Addison St" |
| `13 04 …` | HNO (19) = "1060" |
| `18 05 …` | PC (24) = "60613" |
| `17 0d …` | NAM (23) = "Wrigley Field" |
| `03 02 00 c3` | TLV 3 FTM correction = 195 cm |
| `04 01 00` | TLV 4 position source = fixed |

**ANQP: GAS Initial Response body** (from the public action category)

```
04 0b 0d 00 00 00 00 6c 02 7f 00 63 00
01 01 06 00 01 01 09 01 0a 01
09 01 15 00 00 10 55 62 66 f9 14 15 04 1e 2c d4 01 00 d4 02 00 41 06 01 01
0a 01 3c 00 00 00 39 55 53 01 02 49 4c 03 07 43 68 69 63 61 67 6f 22 0c 57
20 41 64 64 69 73 6f 6e 20 53 74 13 04 31 30 36 30 18 05 36 30 36 31 33 17
0d 57 72 69 67 6c 65 79 20 46 69 65 6c 64
```

| Octets | Meaning |
|---|---|
| `04 0b 0d` | Public action, GAS Initial Response, dialog token 13 |
| `00 00 00 00` | Status success, comeback delay 0 |
| `6c 02 7f 00` | Advertisement Protocol element: ANQP |
| `63 00` | Query response length 99 |
| `01 01 06 00` + 6 | 257 Capability List: 257, 265, 266 |
| `09 01 15 00` + 21 | 265 AP Geospatial Location: LCI subelement (same LCI as Element A) + Usage Rules `06 01 01` |
| `0a 01 3c 00` + 60 | 266 AP Civic Location: type 0 (RFC 4776), Location Civic subelement `00 39`, "US" + CAtypes |

## Identifier

`18:FE:34` is Espressif's OUI. OUI type `0xF0` was chosen by this project and
is not registered with Espressif, so another vendor could reuse it. If a
registered identifier is needed for wider support, Element B can move to a
different OUI or CID with the same contents.
