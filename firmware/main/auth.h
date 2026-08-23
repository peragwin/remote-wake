/* auth — remote-wake v1 command verification.
 *
 * Implements docs/protocol.md "Command envelope" / "Device-side verification"
 * verbatim. Deliberately free of any dependency on config.c, NVS or the
 * network stack: everything the verifier needs is supplied through
 * rw_auth_ops_t. That keeps this translation unit unit-testable against
 * docs/test-vectors.json (see firmware/test_apps/auth_test).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RW_PROTO_VERSION      1
#define RW_SIG_PREFIX         "remote-wake-v1"
#define RW_ED25519_PUBKEY_LEN 32
#define RW_ED25519_SIG_LEN    64
#define RW_DEVICE_ID_LEN      16   /* 8 bytes, lowercase hex */
#define RW_KID_MAXLEN         8
#define RW_ID_MAXLEN          64
#define RW_ACT_MAXLEN         24
#define RW_MAX_ENVELOPE_BYTES 4096 /* relay enforces the same cap */

/** Failure reasons. rw_auth_err_str() maps these to the protocol's `err`. */
typedef enum {
    RW_OK = 0,
    RW_ERR_BAD_JSON,     /* "bad_request" */
    RW_ERR_VERSION,      /* "version"     */
    RW_ERR_DEV,          /* "dev_mismatch"*/
    RW_ERR_UNKNOWN_KEY,  /* "unknown_key" */
    RW_ERR_SIG,          /* "sig"         */
    RW_ERR_STALE,        /* "stale"       */
    RW_ERR_REPLAY,       /* "replay"      */
    RW_ERR_UNKNOWN_ACT,  /* "unknown_act" */
    RW_ERR_INTERNAL,     /* "internal"    */
} rw_auth_err_t;

/** Protocol error token for @p e (never NULL). */
const char *rw_auth_err_str(rw_auth_err_t e);

/** A parsed, fully verified command. Release with rw_cmd_free(). */
typedef struct {
    char        id[RW_ID_MAXLEN];
    char        act[RW_ACT_MAXLEN];
    char        kid[RW_KID_MAXLEN];
    int64_t     ts;
    uint64_t    ctr;
    const cJSON *args;      /* borrowed from ->_root, never NULL */
    cJSON       *_root;     /* owned */
} rw_cmd_t;

void rw_cmd_free(rw_cmd_t *cmd);

/** Environment the verifier needs. All callbacks may be NULL-checked. */
typedef struct {
    /** Our own deviceId, 16 lowercase hex chars. */
    const char *device_id;
    /** Current wall-clock time, Unix seconds. */
    int64_t now;
    /** Accepted |ts - now|, seconds. Protocol v1 mandates 90. */
    int32_t ts_window;
    /** True once the clock has been SNTP-synced; when false the ts check is
     *  skipped and the caller is expected to log loudly. */
    bool clock_valid;

    void *ctx;
    /** Fetch the 32-byte Ed25519 public key for @p kid. False = no such slot. */
    bool (*get_key)(void *ctx, const char *kid, uint8_t out[RW_ED25519_PUBKEY_LEN]);
    /** Fetch the persisted counter high-water mark for @p kid (0 if none). */
    bool (*get_ctr)(void *ctx, const char *kid, uint64_t *out);
    /** Persist @p ctr as the new high-water mark. MUST be durable on return. */
    esp_err_t (*set_ctr)(void *ctx, const char *kid, uint64_t ctr);
    /** True when @p act is implemented and enabled on this build/profile. */
    bool (*act_enabled)(void *ctx, const char *act);
} rw_auth_ops_t;

/** One-time init (seeds libsodium). Safe to call repeatedly. */
esp_err_t rw_auth_init(void);

/**
 * Build the protocol signing string for the given fields.
 *
 *   remote-wake-v1\n<dev>\n<id>\n<ts>\n<ctr>\n<act>\n<canonical args>
 *
 * @param[out] out_len  byte length (excluding the NUL terminator)
 * @return malloc()ed NUL-terminated buffer (caller frees), or NULL on OOM.
 */
char *rw_auth_signing_string(const char *dev, const char *id, int64_t ts,
                             uint64_t ctr, const char *act, const cJSON *args,
                             size_t *out_len);

/**
 * Raw Ed25519 detached verification.
 * @return true iff @p sig is a valid signature over @p msg under @p pubkey.
 */
bool rw_auth_ed25519_verify(const uint8_t pubkey[RW_ED25519_PUBKEY_LEN],
                            const uint8_t *msg, size_t msg_len,
                            const uint8_t sig[RW_ED25519_SIG_LEN]);

/**
 * Parse and fully verify a command envelope, in the exact order mandated by
 * docs/protocol.md. On RW_OK the counter high-water mark has *already* been
 * persisted (write-then-execute), and @p out holds the parsed command.
 *
 * @param json  envelope text (need not be NUL-terminated)
 * @param len   length of @p json
 */
rw_auth_err_t rw_auth_verify(const rw_auth_ops_t *ops, const char *json,
                             size_t len, rw_cmd_t *out);

/**
 * Extract just the envelope's `id` without verifying anything, so that a
 * rejected command can still be answered with a correctly addressed error.
 * Writes "" when the field is missing or malformed.
 */
void rw_auth_peek_id(const char *json, size_t len, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
