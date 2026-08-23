/* canon_json — canonical JSON serialization for the remote-wake signing
 * string.
 *
 * Rules (docs/protocol.md, "Signing string"):
 *   - object keys sorted lexicographically by their raw UTF-8 bytes
 *   - no insignificant whitespace at all
 *   - UTF-8 output; only the JSON-mandatory escapes are emitted, so non-ASCII
 *     characters are passed through verbatim (this matches what
 *     JSON.stringify() produces on the phone side)
 *   - integral numbers are printed without a decimal point or exponent
 *
 * The device MUST re-serialize the received `args` with exactly these rules
 * before verifying, rather than trusting the bytes on the wire.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Growable string buffer. Zero-initialize before first use. */
typedef struct {
    char  *buf;   /**< NUL-terminated once anything has been appended. */
    size_t len;   /**< bytes used, excluding the NUL */
    size_t cap;   /**< bytes allocated */
    bool   err;   /**< sticky: set on any allocation failure */
} rw_sbuf_t;

/** Append @p n raw bytes. Failures are recorded in ->err, never fatal. */
void rw_sbuf_append(rw_sbuf_t *sb, const char *data, size_t n);
/** Append a NUL-terminated string. */
void rw_sbuf_puts(rw_sbuf_t *sb, const char *s);
/** Append a signed decimal integer. */
void rw_sbuf_puti(rw_sbuf_t *sb, long long v);
/** Append an unsigned decimal integer. */
void rw_sbuf_putu(rw_sbuf_t *sb, unsigned long long v);
/** Release the buffer and reset the struct. */
void rw_sbuf_free(rw_sbuf_t *sb);

/**
 * Append the canonical serialization of @p item to @p sb.
 * A NULL item serializes as `null`.
 */
void rw_canon_json_append(rw_sbuf_t *sb, const cJSON *item);

/**
 * Canonically serialize @p item into a freshly malloc()ed NUL-terminated
 * string. Caller frees. Returns NULL on allocation failure.
 */
char *rw_canon_json(const cJSON *item);

#ifdef __cplusplus
}
#endif
