/* b64url — base64url (RFC 4648 §5) without padding.
 *
 * Self-contained so that auth.c can be compiled into a host/unit-test target
 * without dragging in mbedtls.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Encoded length (no padding, no NUL) for @p n input bytes. */
#define RW_B64URL_ENC_LEN(n) (((n) * 4 + 2) / 3)

/**
 * Encode @p len bytes into @p out as unpadded base64url and NUL-terminate.
 * @p out_sz must be at least RW_B64URL_ENC_LEN(len) + 1.
 * @return number of characters written (excluding NUL), or -1 on overflow.
 */
int rw_b64url_encode(const uint8_t *in, size_t len, char *out, size_t out_sz);

/**
 * Decode an unpadded (or padded — '=' is tolerated and ignored) base64url
 * string of @p len characters into @p out.
 * @return number of bytes written, or -1 on invalid input / overflow.
 */
int rw_b64url_decode(const char *in, size_t len, uint8_t *out, size_t out_sz);

/** Constant-time buffer comparison. Returns true when equal. */
bool rw_ct_equal(const void *a, const void *b, size_t len);

#ifdef __cplusplus
}
#endif
