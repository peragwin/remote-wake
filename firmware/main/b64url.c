#include "b64url.h"

#include <string.h>

static const char k_enc[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int dec_one(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    /* Be forgiving about standard-alphabet input from other implementations. */
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int rw_b64url_encode(const uint8_t *in, size_t len, char *out, size_t out_sz)
{
    size_t need = RW_B64URL_ENC_LEN(len);
    if (!in || !out || out_sz < need + 1) {
        return -1;
    }

    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = k_enc[(v >> 18) & 0x3f];
        out[o++] = k_enc[(v >> 12) & 0x3f];
        out[o++] = k_enc[(v >> 6) & 0x3f];
        out[o++] = k_enc[v & 0x3f];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = k_enc[(v >> 18) & 0x3f];
        out[o++] = k_enc[(v >> 12) & 0x3f];
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = k_enc[(v >> 18) & 0x3f];
        out[o++] = k_enc[(v >> 12) & 0x3f];
        out[o++] = k_enc[(v >> 6) & 0x3f];
    }
    out[o] = '\0';
    return (int)o;
}

int rw_b64url_decode(const char *in, size_t len, uint8_t *out, size_t out_sz)
{
    if (!in || !out) {
        return -1;
    }
    /* Ignore any trailing padding. */
    while (len > 0 && in[len - 1] == '=') {
        len--;
    }

    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;

    for (size_t i = 0; i < len; i++) {
        int d = dec_one(in[i]);
        if (d < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_sz) {
                return -1;
            }
            out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    /* Leftover bits must be zero padding, and can never be a full byte. */
    if (bits >= 6 || (acc & ((1u << bits) - 1u)) != 0) {
        return -1;
    }
    return (int)o;
}

bool rw_ct_equal(const void *a, const void *b, size_t len)
{
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(pa[i] ^ pb[i]);
    }
    return diff == 0;
}
