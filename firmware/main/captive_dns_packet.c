#include "captive_dns.h"
#include <stdbool.h>
#include <string.h>

size_t rw_captive_dns_reply(uint8_t *p, size_t len, size_t cap, uint32_t address)
{
    /* Standard queries only, one question, no answer/authority records.
     * Extra EDNS data is discarded; response does not advertise EDNS support. */
    if (!p || len < 12 || len > 512 || len > cap || (p[2] & 0xfe) ||
        (p[3] & 0x4f) || p[4] || p[5] != 1 || p[6] || p[7] || p[8] || p[9])
        return 0;
    size_t pos = 12;
    for (;;) {
        if (pos >= len || pos - 12 >= 255)
            return 0;
        unsigned label = p[pos++];
        if (!label)
            break;
        /* Queries need no compression: reject pointers, loops and reserved labels. */
        if (label > 63 || label > len - pos)
            return 0;
        pos += label;
    }
    if (len - pos < 4)
        return 0;
    bool answer = p[pos] == 0 && p[pos + 1] == 1 &&
                  p[pos + 2] == 0 && p[pos + 3] == 1;
    pos += 4;
    if (answer && cap - pos < 16)
        return 0;
    p[2] = 0x84 | (p[2] & 1); /* QR, authoritative, preserve recursion requested */
    p[3] = 0;                 /* No recursion available, no error */
    p[7] = answer ? 1 : 0;
    p[10] = p[11] = 0;
    if (answer) {
        /* Name points to the validated question, IN/A, TTL 0 (setup is temporary). */
        static const uint8_t rr[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
        memcpy(p + pos, rr, sizeof rr);
        memcpy(p + pos + sizeof rr, &address, 4);
        pos += 16;
    }
    /* AAAA/HTTPS/etc. receive NOERROR with no records, not bogus IPv4 data. */
    return pos;
}
