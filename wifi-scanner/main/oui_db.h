/*
    MAC address vendor lookup (IEEE MA-L / OUI registry, short names).
    Data in oui_db.c, generated from Wireshark's manuf file.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Vendor for the first three bytes of a MAC address, or NULL if unknown. */
const char *oui_vendor(const uint8_t mac[3]);

/* Vendor for a full MAC. Locally administered addresses (bit 0x02 of the first byte,
 * used for an AP's extra SSIDs and for randomized MACs) are looked up with that bit
 * cleared; *virtual_mac is set when that happened. */
static inline const char *oui_vendor_mac(const uint8_t mac[6], bool *virtual_mac)
{
    *virtual_mac = false;
    const char *v = oui_vendor(mac);
    if (!v && (mac[0] & 0x02)) {
        uint8_t hw[3] = { (uint8_t)(mac[0] & ~0x02), mac[1], mac[2] };
        v = oui_vendor(hw);
        *virtual_mac = v != NULL;
    }
    return v;
}
