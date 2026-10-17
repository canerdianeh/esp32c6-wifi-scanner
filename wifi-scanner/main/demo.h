/*
    Demo builds (CONFIG_SCANNER_DEMO): names are redacted as they come in, so
    the screen, serial log and microSD log all show the same thing.
*/
#pragma once

#include <stddef.h>
#include <string.h>
#include <strings.h>
#include "sdkconfig.h"

/* Replace CONFIG_SCANNER_DEMO_REDACT (any case) with CONFIG_SCANNER_DEMO_REDACT_WITH, in place,
 * within a buffer of `size` bytes. No-op in normal builds or when nothing is set to redact. */
static inline void demo_sanitize(char *s, size_t size)
{
#if CONFIG_SCANNER_DEMO
    const char *from = CONFIG_SCANNER_DEMO_REDACT, *to = CONFIG_SCANNER_DEMO_REDACT_WITH;
    size_t fl = strlen(from), tl = strlen(to);
    if (!fl) {
        return;
    }
    for (char *p = s; *p; p++) {
        if (strncasecmp(p, from, fl) == 0) {
            size_t tail = strlen(p + fl) + 1;               /* rest of the string, with its terminator */
            size_t room = size - (size_t)(p - s);           /* bytes from p to the end of the buffer */
            if (tl + tail > room) {
                tail = room > tl ? room - tl : 0;           /* cut the end to fit */
            }
            memmove(p + tl, p + fl, tail);
            memcpy(p, to, tl < room ? tl : room);
            s[size - 1] = 0;
            p += tl ? tl - 1 : 0;
        }
    }
#else
    (void)s;
    (void)size;
#endif
}
