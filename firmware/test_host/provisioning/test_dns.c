#include "captive_dns.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t query[] = {
    0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    7, 'c', 'a', 'p', 't', 'i', 'v', 'e', 5, 'a', 'p', 'p', 'l', 'e',
    3, 'c', 'o', 'm', 0, 0, 1, 0, 1,
};
static uint32_t address;
static void reply_tests(void)
{
    uint8_t p[512];
    memcpy(p, query, sizeof query);
    size_t n = rw_captive_dns_reply(p, sizeof query, sizeof p, address);
    assert(n == sizeof query + 16);
    assert(p[0] == 0x12 && p[1] == 0x34 && p[2] == 0x85 && p[3] == 0);
    assert(p[5] == 1 && p[7] == 1 && p[9] == 0 && p[11] == 0);
    assert(!memcmp(p + 12, query + 12, sizeof query - 12));
    const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4,
                              192, 168, 4, 1};
    assert(!memcmp(p + sizeof query, answer, sizeof answer));
    for (unsigned type = 1; type < 256; type++) {
        memcpy(p, query, sizeof query);
        p[sizeof query - 3] = type;
        n = rw_captive_dns_reply(p, sizeof query, sizeof p, address);
        assert(n == sizeof query + (type == 1 ? 16 : 0));
        assert(p[7] == (type == 1));
    }
    /* A query with EDNS options produces a plain DNS response. */
    memcpy(p, query, sizeof query);
    p[11] = 1;
    memset(p + sizeof query, 0, 11);
    assert(rw_captive_dns_reply(p, sizeof query + 11, sizeof p, address) == sizeof query + 16);
    assert(!p[11]);
}
static void malformed_tests(void)
{
    uint8_t p[512];
    assert(!rw_captive_dns_reply(NULL, 12, 512, address));
    for (size_t cut = 0; cut < sizeof query; cut++) {
        memcpy(p, query, sizeof query);
        assert(!rw_captive_dns_reply(p, cut, sizeof p, address));
    }
    for (size_t cap = 0; cap < sizeof query + 16; cap++) {
        memcpy(p, query, sizeof query);
        assert(!rw_captive_dns_reply(p, sizeof query, cap, address));
    }
    const size_t offsets[] = {2, 3, 4, 5, 6, 7, 8, 9, 12};
    const uint8_t bad[] = {0x80, 1, 1, 2, 1, 1, 1, 1, 0xc0};
    for (size_t i = 0; i < sizeof bad; i++) {
        memcpy(p, query, sizeof query);
        p[offsets[i]] = bad[i];
        assert(!rw_captive_dns_reply(p, sizeof query, sizeof p, address));
    }
    memcpy(p, query, sizeof query);
    p[12] = 64;
    assert(!rw_captive_dns_reply(p, sizeof query, sizeof p, address));
    memset(p, 0, sizeof p);
    p[5] = 1;
    for (int i = 12; i < 268; i += 64) { p[i] = 63; memset(p+i+1, 'a', 63); }
    assert(!rw_captive_dns_reply(p, 273, sizeof p, address));
    /* Deterministic malformed-packet corpus under address/UB sanitizers. */
    uint32_t seed = 42;
    for (unsigned trial = 0; trial < 100000; trial++) {
        for (size_t j = 0; j < sizeof p; j++) {
            seed = seed * 1664525 + 1013904223;
            p[j] = seed >> 24;
        }
        if (trial % 2) { memset(p, 0, 12); p[5] = 1; }
        size_t len = trial % 513;
        size_t result = rw_captive_dns_reply(p, len, sizeof p, address);
        assert(result <= sizeof p);
    }
}
int main(void)
{
    const uint8_t ip[] = {192,168,4,1};
    memcpy(&address, ip, 4);
    reply_tests();
    malformed_tests();
    puts("Captive DNS: A/AAAA/other queries, EDNS, bounds and malformed corpus passed");
}
