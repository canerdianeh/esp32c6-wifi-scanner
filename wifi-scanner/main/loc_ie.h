/*
    Location in beacons: vendor-specific elements (ID 221) that the FTM
    responders add to beacons / probe responses and the wearable scanner reads.

    KEEP IDENTICAL in ftm-responder/main/loc_ie.h and wifi-scanner/main/loc_ie.h.

    1. Geo position: the HPE Aruba vendor element, as Aruba APs send with
       "advertise-location" (layout as decoded by Wireshark and lswifi):
           DD <len> 00 0B 86 01 <subtype> 00 <data>
       subtype 8 (LCI): data = an 802.11 LCI report, i.e. subelements
           (ID, length, data): 0 = LCI (16 octets, the 802.11 LCI field below,
           derived from RFC 3825 / RFC 6225), 4 = Z (floor). WiFi Explorer
           parses subelements 0 and 4.
       subtype 9 (GPS ellipse, received only): len, subversion, hop, then
           big-endian doubles lat, lon, major axis, minor axis, orientation, distance.

    2. Civic address and FTM calibration: an element under Espressif's OUI
       (ESP-IDF allows only two vendor elements per frame, and Aruba has no
       documented civic subtype):
           DD <len> 18 FE 34 F0 <version 2> <TLVs>
       TLV 2: civic address in RFC 4776 format (as carried by RFC 5580):
              what(1) country(2) then CAtype(1) length(1) value...
       TLV 3: FTM distance correction, int16 cm, big-endian (subtract from measured distance)
       TLV 4: position source, 1 octet (0 fixed, 1 GPS)
*/
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Aruba element */
#define ARUBA_OUI_0         0x00
#define ARUBA_OUI_1         0x0B
#define ARUBA_OUI_2         0x86
#define ARUBA_OUI_TYPE      0x01
#define ARUBA_SUB_LCI       8
#define ARUBA_SUB_GPS       9

/* Espressif-OUI element */
#define LOC_IE_OUI_0        0x18
#define LOC_IE_OUI_1        0xFE
#define LOC_IE_OUI_2        0x34
#define LOC_IE_OUI_TYPE     0xF0
#define LOC_IE_VERSION      2
#define LOC_TLV_CIVIC       2
#define LOC_TLV_FTM_CAL     3
#define LOC_TLV_SOURCE      4

#define LOC_SOURCE_FIXED    0
#define LOC_SOURCE_GPS      1

/* RFC 4776 CAtypes used here */
#define CA_A1       1       /* state / province */
#define CA_A3       3       /* city */
#define CA_HNO      19      /* house number */
#define CA_LOC      22      /* additional location information */
#define CA_NAM      23      /* name (business, resident) */
#define CA_PC       24      /* postal code */
#define CA_BLD      25      /* building */
#define CA_FLR      27      /* floor */
#define CA_ROOM     28      /* room */
#define CA_RD       34      /* primary road / street (RFC 5139) */

#define LOC_CIVIC_MAX       8
#define LOC_CIVIC_VAL_MAX   40

typedef struct {
    uint8_t type;
    char value[LOC_CIVIC_VAL_MAX + 1];
} loc_civic_t;

typedef struct {
    bool has_position;
    double lat, lon;                /* degrees, WGS84 */
    bool has_alt;
    float alt_m;
    float accuracy_m;               /* horizontal, 0 = unknown */
    bool has_source;
    uint8_t source;                 /* LOC_SOURCE_* */
    char country[3];                /* "" = no civic address */
    loc_civic_t civic[LOC_CIVIC_MAX];
    int civic_n;
    bool has_ftm_cal;
    int16_t ftm_cal_cm;             /* subtract from measured FTM distance */
    bool via_anqp;                  /* receiver: location came from an ANQP query */
} loc_info_t;

static inline bool loc_has_location(const loc_info_t *l)
{
    return l->has_position || l->country[0];
}

static inline const char *loc_civic_get(const loc_info_t *l, uint8_t type)
{
    for (int i = 0; i < l->civic_n; i++) {
        if (l->civic[i].type == type) {
            return l->civic[i].value;
        }
    }
    return NULL;
}

static inline void loc_civic_add(loc_info_t *l, uint8_t type, const char *value)
{
    if (value && value[0] && l->civic_n < LOC_CIVIC_MAX) {
        l->civic[l->civic_n].type = type;
        snprintf(l->civic[l->civic_n].value, sizeof(l->civic[0].value), "%s", value);
        l->civic_n++;
    }
}

/* ---- building the elements (complete, starting with DD) ---- */


/* Civic / calibration element; returns total length, 0 if there's nothing to send. */
static inline int loc_build_esp(const loc_info_t *l, uint8_t *out, int max)
{
    int n = 6;
    out[n++] = LOC_IE_VERSION;
    if (l->country[0]) {
        int start = n;
        out[n++] = LOC_TLV_CIVIC;
        out[n++] = 0;                       /* length, filled in below */
        out[n++] = 1;                       /* what: location of the network element (the AP) */
        out[n++] = l->country[0];
        out[n++] = l->country[1];
        for (int i = 0; i < l->civic_n; i++) {
            int vl = (int)strlen(l->civic[i].value);
            if (n + 2 + vl > max - 8) {
                break;
            }
            out[n++] = l->civic[i].type;
            out[n++] = vl;
            memcpy(out + n, l->civic[i].value, vl);
            n += vl;
        }
        out[start + 1] = n - start - 2;
    }
    if (l->has_ftm_cal) {
        out[n++] = LOC_TLV_FTM_CAL;
        out[n++] = 2;
        out[n++] = (uint8_t)((uint16_t)l->ftm_cal_cm >> 8);     /* network byte order */
        out[n++] = (uint8_t)l->ftm_cal_cm;
    }
    if (l->has_position && l->has_source) {
        out[n++] = LOC_TLV_SOURCE;
        out[n++] = 1;
        out[n++] = l->source;
    }
    if (n == 7) {
        return 0;
    }
    out[0] = 0xDD;
    out[1] = n - 2;
    out[2] = LOC_IE_OUI_0;
    out[3] = LOC_IE_OUI_1;
    out[4] = LOC_IE_OUI_2;
    out[5] = LOC_IE_OUI_TYPE;
    return n;
}

static inline bool loc_anqp_geo_parse(const uint8_t *p, int len, loc_info_t *l);

/* ---- parsing (ie points at the OUI, len = element length) ---- */

static inline double loc_be_double(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    double d;
    memcpy(&d, &v, 8);
    return d;
}

/* Merge what a vendor element says into *l. Returns true if it was one of ours / Aruba's location data. */
static inline bool loc_parse_vendor_ie(const uint8_t *ie, int len, loc_info_t *l)
{
    if (len >= 6 + 18 && ie[0] == ARUBA_OUI_0 && ie[1] == ARUBA_OUI_1 && ie[2] == ARUBA_OUI_2 &&
        ie[3] == ARUBA_OUI_TYPE && ie[4] == ARUBA_SUB_LCI) {
        return loc_anqp_geo_parse(ie + 6, len - 6, l);     /* LCI report subelements */
    }
    if (len >= 6 + 51 && ie[0] == ARUBA_OUI_0 && ie[1] == ARUBA_OUI_1 && ie[2] == ARUBA_OUI_2 &&
        ie[3] == ARUBA_OUI_TYPE && ie[4] == ARUBA_SUB_GPS) {
        double lat = loc_be_double(ie + 9), lon = loc_be_double(ie + 17), major = loc_be_double(ie + 25);
        if (fabs(lat) <= 90 && fabs(lon) <= 180 && major >= 0) {
            l->has_position = true;
            l->lat = lat;
            l->lon = lon;
            l->accuracy_m = (float)major;
            return true;
        }
        return false;
    }
    if (len >= 5 && ie[0] == LOC_IE_OUI_0 && ie[1] == LOC_IE_OUI_1 && ie[2] == LOC_IE_OUI_2 &&
        ie[3] == LOC_IE_OUI_TYPE && ie[4] == LOC_IE_VERSION) {
        int i = 5;
        while (i + 2 <= len) {
            int t = ie[i], tl = ie[i + 1];
            const uint8_t *v = ie + i + 2;
            if (i + 2 + tl > len) {
                break;
            }
            if (t == LOC_TLV_CIVIC && tl >= 3) {
                l->country[0] = v[1];
                l->country[1] = v[2];
                l->country[2] = 0;
                l->civic_n = 0;
                int j = 3;
                while (j + 2 <= tl && l->civic_n < LOC_CIVIC_MAX) {
                    int ct = v[j], cl = v[j + 1];
                    if (j + 2 + cl > tl) {
                        break;
                    }
                    int c = cl < LOC_CIVIC_VAL_MAX ? cl : LOC_CIVIC_VAL_MAX;
                    l->civic[l->civic_n].type = ct;
                    memcpy(l->civic[l->civic_n].value, v + j + 2, c);
                    l->civic[l->civic_n].value[c] = 0;
                    l->civic_n++;
                    j += 2 + cl;
                }
            } else if (t == LOC_TLV_FTM_CAL && tl >= 2) {
                l->has_ftm_cal = true;
                l->ftm_cal_cm = (int16_t)((v[0] << 8) | v[1]);
            } else if (t == LOC_TLV_SOURCE && tl >= 1) {
                l->has_source = true;
                l->source = v[0];
            }
            i += 2 + tl;
        }
        return true;
    }
    return false;
}

/* ---- ANQP (IEEE 802.11u / 802.11-2016 9.4.5): AP Geospatial Location and AP Civic Location ----

   GAS Initial Request  (public action 10): dialog token, Advertisement Protocol
       element (6C 02 <query response info> 00=ANQP), query length (2, LE),
       ANQP Query List element (Info ID 256) listing the wanted Info IDs.
   GAS Initial Response (public action 11): dialog token, status (2), comeback
       delay (2), Advertisement Protocol element, query length (2, LE), ANQP elements.
   ANQP element: Info ID (2, LE), Length (2, LE), payload.

   265 AP Geospatial Location: an LCI report = subelements; LCI subelement
       (ID 0, 16 octets) and Usage Rules/Policy (ID 6).
   266 AP Civic Location: civic location type (0 = RFC 4776), then the Location
       Civic subelement (ID 0): country code (2) + CAtype/length/value entries.

   The 16-octet LCI field (used here and in the Aruba element) carries RFC 6225
   fields in 802.11 bit order (B0 = LSB of octet 0, each field least significant
   bit first), not RFC 3825's network bit order:
       LatUnc(6) Lat(34) LonUnc(6) Lon(34) AT(4) AltUnc(6) Alt(30) Datum(3)
       RegLocAgreement(1) RegLocDSE(1) DependentSTA(1) Version(2)
   Uncertainty x means 2^(8-x) degrees (lat/lon) or 2^(21-x) m (altitude); 0 = unknown. */

#define ANQP_QUERY_LIST     256
#define ANQP_CAPAB_LIST     257
#define ANQP_AP_GEO_LOC     265
#define ANQP_AP_CIVIC_LOC   266

static inline int64_t loc_sign_extend(uint64_t v, int bits)
{
    uint64_t m = 1ULL << (bits - 1);
    return (int64_t)((v ^ m) - m);
}

static inline void lci80211_put(uint8_t *buf, int pos, int n, uint64_t v)
{
    for (int i = 0; i < n; i++) {
        int p = pos + i;
        if ((v >> i) & 1) {
            buf[p / 8] |= 1 << (p % 8);
        } else {
            buf[p / 8] &= ~(1 << (p % 8));
        }
    }
}

static inline uint64_t lci80211_get(const uint8_t *buf, int pos, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        int p = pos + i;
        v |= (uint64_t)((buf[p / 8] >> (p % 8)) & 1) << i;
    }
    return v;
}

static inline void loc_lci80211_encode(const loc_info_t *l, uint8_t out[16])
{
    memset(out, 0, 16);
    int unc = 0;
    if (l->accuracy_m > 0) {
        unc = (int)floor(8.0 - log2(l->accuracy_m / 111320.0));
        unc = unc < 1 ? 1 : (unc > 34 ? 34 : unc);
    }
    lci80211_put(out, 0, 6, unc);
    lci80211_put(out, 6, 34, (uint64_t)(int64_t)llround(l->lat * 33554432.0) & 0x3FFFFFFFFULL);
    lci80211_put(out, 40, 6, unc);
    lci80211_put(out, 46, 34, (uint64_t)(int64_t)llround(l->lon * 33554432.0) & 0x3FFFFFFFFULL);
    if (l->has_alt) {
        lci80211_put(out, 80, 4, 1);                /* meters */
        lci80211_put(out, 90, 30, (uint64_t)(int64_t)llround(l->alt_m * 256.0) & 0x3FFFFFFFULL);
    }
    lci80211_put(out, 120, 3, 1);                   /* WGS84 */
    lci80211_put(out, 126, 2, 1);                   /* version 1 */
}

static inline bool loc_lci80211_decode(const uint8_t in[16], loc_info_t *l)
{
    int unc = (int)lci80211_get(in, 0, 6);
    l->lat = loc_sign_extend(lci80211_get(in, 6, 34), 34) / 33554432.0;
    l->lon = loc_sign_extend(lci80211_get(in, 46, 34), 34) / 33554432.0;
    l->has_alt = lci80211_get(in, 80, 4) == 1;
    l->alt_m = l->has_alt ? loc_sign_extend(lci80211_get(in, 90, 30), 30) / 256.0f : 0;
    l->accuracy_m = unc ? (float)(ldexp(1.0, 8 - unc) * 111320.0) : 0;
    l->has_position = fabs(l->lat) <= 90 && fabs(l->lon) <= 180 && (l->lat != 0 || l->lon != 0);
    return l->has_position;
}

/* Aruba LCI element (subtype 8): an LCI report with the LCI subelement.
 * Returns the total element length, 0 if there's no position. */
static inline int loc_build_aruba_lci(const loc_info_t *l, uint8_t *out)
{
    if (!l->has_position) {
        return 0;
    }
    out[0] = 0xDD;
    out[1] = 4 + 2 + 18;
    out[2] = ARUBA_OUI_0;
    out[3] = ARUBA_OUI_1;
    out[4] = ARUBA_OUI_2;
    out[5] = ARUBA_OUI_TYPE;
    out[6] = ARUBA_SUB_LCI;
    out[7] = 0;
    out[8] = 0;                 /* LCI subelement */
    out[9] = 16;
    loc_lci80211_encode(l, out + 10);
    return 2 + out[1];
}

/* Payload of ANQP 265; returns its length, 0 without a position. */
static inline int loc_anqp_geo_build(const loc_info_t *l, uint8_t *out)
{
    if (!l->has_position) {
        return 0;
    }
    out[0] = 0;                 /* LCI subelement */
    out[1] = 16;
    loc_lci80211_encode(l, out + 2);
    out[18] = 6;                /* Usage Rules/Policy subelement */
    out[19] = 1;
    out[20] = 0x01;             /* retransmission allowed */
    return 21;
}

/* Payload of ANQP 266; returns its length, 0 without a civic address. */
static inline int loc_anqp_civic_build(const loc_info_t *l, uint8_t *out, int max)
{
    if (!l->country[0] || max < 5) {
        return 0;
    }
    int n = 0;
    out[n++] = 0;               /* civic location type: RFC 4776 */
    out[n++] = 0;               /* Location Civic subelement */
    int len_at = n++;
    out[n++] = l->country[0];
    out[n++] = l->country[1];
    for (int i = 0; i < l->civic_n; i++) {
        int vl = (int)strlen(l->civic[i].value);
        if (n + 2 + vl > max || n + 2 + vl - len_at - 1 > 255) {
            break;
        }
        out[n++] = l->civic[i].type;
        out[n++] = vl;
        memcpy(out + n, l->civic[i].value, vl);
        n += vl;
    }
    out[len_at] = n - len_at - 1;
    return n;
}

static inline bool loc_anqp_geo_parse(const uint8_t *p, int len, loc_info_t *l)
{
    for (int i = 0; i + 2 <= len;) {
        int id = p[i], sl = p[i + 1];
        if (i + 2 + sl > len) {
            break;
        }
        if (id == 0 && sl >= 16) {
            return loc_lci80211_decode(p + i + 2, l);
        }
        i += 2 + sl;
    }
    return false;
}

static inline bool loc_anqp_civic_parse(const uint8_t *p, int len, loc_info_t *l)
{
    if (len < 1 || p[0] != 0) {
        return false;
    }
    for (int i = 1; i + 2 <= len;) {
        int id = p[i], sl = p[i + 1];
        const uint8_t *v = p + i + 2;
        if (i + 2 + sl > len) {
            break;
        }
        if (id == 0 && sl >= 2) {
            l->country[0] = v[0];
            l->country[1] = v[1];
            l->country[2] = 0;
            l->civic_n = 0;
            for (int j = 2; j + 2 <= sl && l->civic_n < LOC_CIVIC_MAX;) {
                int ct = v[j], cl = v[j + 1];
                if (j + 2 + cl > sl) {
                    break;
                }
                int c = cl < LOC_CIVIC_VAL_MAX ? cl : LOC_CIVIC_VAL_MAX;
                l->civic[l->civic_n].type = ct;
                memcpy(l->civic[l->civic_n].value, v + j + 2, c);
                l->civic[l->civic_n].value[c] = 0;
                l->civic_n++;
                j += 2 + cl;
            }
            return true;
        }
        i += 2 + sl;
    }
    return false;
}

/* One-line civic address, e.g. "123 Main St, Bldg A, Fl 2, Rm 210, Springfield". */
static inline void loc_civic_format(const loc_info_t *l, char *buf, size_t len)
{
    const char *nam = loc_civic_get(l, CA_NAM), *hno = loc_civic_get(l, CA_HNO), *rd = loc_civic_get(l, CA_RD);
    const char *bld = loc_civic_get(l, CA_BLD), *flr = loc_civic_get(l, CA_FLR), *room = loc_civic_get(l, CA_ROOM);
    const char *city = loc_civic_get(l, CA_A3), *st = loc_civic_get(l, CA_A1), *add = loc_civic_get(l, CA_LOC);
    size_t n = 0;
    buf[0] = 0;
#define LOC_APPEND(fmt, v) \
    if (v && n < len) { n += snprintf(buf + n, len - n, "%s" fmt, n ? ", " : "", v); }
    LOC_APPEND("%s", nam);
    if (hno && rd && n < len) {
        n += snprintf(buf + n, len - n, "%s%s %s", n ? ", " : "", hno, rd);
    } else {
        LOC_APPEND("%s", rd);
    }
    LOC_APPEND("Bldg %s", bld);
    LOC_APPEND("Fl %s", flr);
    LOC_APPEND("Rm %s", room);
    LOC_APPEND("%s", add);
    LOC_APPEND("%s", city);
    const char *pc = loc_civic_get(l, CA_PC);
    if (st && pc && n < len) {
        n += snprintf(buf + n, len - n, "%s%s %s", n ? ", " : "", st, pc);
    } else {
        LOC_APPEND("%s", st);
        LOC_APPEND("%s", pc);
    }
#undef LOC_APPEND
    if (!buf[0] && l->country[0]) {
        snprintf(buf, len, "%s", l->country);
    }
}
