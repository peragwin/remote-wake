#include "auth.h"

#include <stdlib.h>
#include <string.h>

#include "b64url.h"
#include "canon_json.h"
#include "esp_log.h"
#include "sodium.h"

static const char *TAG = "rw.auth";

const char *rw_auth_err_str(rw_auth_err_t e)
{
    switch (e) {
    case RW_OK:               return "ok";
    case RW_ERR_BAD_JSON:     return "bad_request";
    case RW_ERR_VERSION:      return "version";
    case RW_ERR_DEV:          return "dev_mismatch";
    case RW_ERR_UNKNOWN_KEY:  return "unknown_key";
    case RW_ERR_SIG:          return "sig";
    case RW_ERR_STALE:        return "stale";
    case RW_ERR_REPLAY:       return "replay";
    case RW_ERR_UNKNOWN_ACT:  return "unknown_act";
    case RW_ERR_INTERNAL:     return "internal";
    }
    return "internal";
}

void rw_cmd_free(rw_cmd_t *cmd)
{
    if (!cmd) {
        return;
    }
    if (cmd->_root) {
        cJSON_Delete(cmd->_root);
    }
    memset(cmd, 0, sizeof(*cmd));
}

esp_err_t rw_auth_init(void)
{
    /* sodium_init() is idempotent: 1 = already initialized. */
    int rc = sodium_init();
    if (rc < 0) {
        ESP_LOGE(TAG, "sodium_init failed (%d)", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

char *rw_auth_signing_string(const char *dev, const char *id, int64_t ts,
                             uint64_t ctr, const char *act, const cJSON *args,
                             size_t *out_len)
{
    rw_sbuf_t sb = {0};

    rw_sbuf_puts(&sb, RW_SIG_PREFIX);
    rw_sbuf_append(&sb, "\n", 1);
    rw_sbuf_puts(&sb, dev);
    rw_sbuf_append(&sb, "\n", 1);
    rw_sbuf_puts(&sb, id);
    rw_sbuf_append(&sb, "\n", 1);
    rw_sbuf_puti(&sb, (long long)ts);
    rw_sbuf_append(&sb, "\n", 1);
    rw_sbuf_putu(&sb, (unsigned long long)ctr);
    rw_sbuf_append(&sb, "\n", 1);
    rw_sbuf_puts(&sb, act);
    rw_sbuf_append(&sb, "\n", 1);
    /* args is an object per the protocol; a NULL/absent args is a protocol
     * violation caught by the caller, but serialize defensively as {}. */
    if (args) {
        rw_canon_json_append(&sb, args);
    } else {
        rw_sbuf_puts(&sb, "{}");
    }

    if (sb.err || !sb.buf) {
        rw_sbuf_free(&sb);
        return NULL;
    }
    if (out_len) {
        *out_len = sb.len;
    }
    return sb.buf;
}

bool rw_auth_ed25519_verify(const uint8_t pubkey[RW_ED25519_PUBKEY_LEN],
                            const uint8_t *msg, size_t msg_len,
                            const uint8_t sig[RW_ED25519_SIG_LEN])
{
    if (!pubkey || !msg || !sig) {
        return false;
    }
    /* libsodium's verify is constant time and rejects small-order / malleable
     * signatures. */
    return crypto_sign_verify_detached(sig, msg, (unsigned long long)msg_len,
                                       pubkey) == 0;
}

/* ------------------------------------------------------------- helpers -- */

static bool copy_str_field(const cJSON *root, const char *name, char *out,
                           size_t out_sz)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(it) || !it->valuestring) {
        return false;
    }
    size_t n = strlen(it->valuestring);
    if (n == 0 || n >= out_sz) {
        return false;
    }
    memcpy(out, it->valuestring, n + 1);
    return true;
}

/* cJSON stores every number as a double; require an exact integer. */
static bool num_field_i64(const cJSON *root, const char *name, int64_t *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(it)) {
        return false;
    }
    double d = it->valuedouble;
    if (d != (double)(int64_t)d || d > 9007199254740992.0 ||
        d < -9007199254740992.0) {
        return false;
    }
    *out = (int64_t)d;
    return true;
}

void rw_auth_peek_id(const char *json, size_t len, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (!json || len == 0 || len > RW_MAX_ENVELOPE_BYTES) {
        return;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return;
    }
    (void)copy_str_field(root, "id", out, out_sz);
    cJSON_Delete(root);
}

/* -------------------------------------------------------------- verify -- */

rw_auth_err_t rw_auth_verify(const rw_auth_ops_t *ops, const char *json,
                             size_t len, rw_cmd_t *out)
{
    if (!ops || !ops->device_id || !json || !out) {
        return RW_ERR_INTERNAL;
    }
    memset(out, 0, sizeof(*out));

    if (len == 0 || len > RW_MAX_ENVELOPE_BYTES) {
        return RW_ERR_BAD_JSON;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return RW_ERR_BAD_JSON;
    }

    rw_auth_err_t rc = RW_ERR_BAD_JSON;
    char *msg = NULL;
    char dev[RW_DEVICE_ID_LEN + 1] = {0};
    char sig_b64[128] = {0};
    uint8_t sig[RW_ED25519_SIG_LEN];
    uint8_t pub[RW_ED25519_PUBKEY_LEN];
    const cJSON *args = NULL;
    int64_t v = 0, ts = 0, ctr_s = 0;
    size_t msg_len = 0;
    uint64_t last = 0;

    /* ---- structural parse (everything the signing string needs) ---- */
    if (!num_field_i64(root, "v", &v) ||
        !num_field_i64(root, "ts", &ts) ||
        !num_field_i64(root, "ctr", &ctr_s) || ctr_s < 1 ||
        !copy_str_field(root, "dev", dev, sizeof(dev)) ||
        !copy_str_field(root, "id", out->id, sizeof(out->id)) ||
        !copy_str_field(root, "act", out->act, sizeof(out->act)) ||
        !copy_str_field(root, "kid", out->kid, sizeof(out->kid)) ||
        !copy_str_field(root, "sig", sig_b64, sizeof(sig_b64))) {
        goto done;
    }
    args = cJSON_GetObjectItemCaseSensitive(root, "args");
    if (!cJSON_IsObject(args)) {  /* "may be {}; never absent" */
        goto done;
    }
    out->ts   = ts;
    out->ctr  = (uint64_t)ctr_s;
    out->args = args;

    /* ---- 1. v == 1, dev matches own id ---- */
    if (v != RW_PROTO_VERSION) {
        rc = RW_ERR_VERSION;
        goto done;
    }
    if (strlen(dev) != RW_DEVICE_ID_LEN || strlen(ops->device_id) != RW_DEVICE_ID_LEN ||
        !rw_ct_equal(dev, ops->device_id, RW_DEVICE_ID_LEN)) {
        rc = RW_ERR_DEV;
        goto done;
    }

    /* ---- 2. kid refers to a registered operator key ---- */
    if (!ops->get_key || !ops->get_key(ops->ctx, out->kid, pub)) {
        rc = RW_ERR_UNKNOWN_KEY;
        goto done;
    }

    /* ---- 3. sig verifies over the signing string ---- */
    if (rw_b64url_decode(sig_b64, strlen(sig_b64), sig, sizeof(sig)) !=
        RW_ED25519_SIG_LEN) {
        rc = RW_ERR_SIG;
        goto done;
    }
    msg = rw_auth_signing_string(dev, out->id, out->ts, out->ctr, out->act,
                                 args, &msg_len);
    if (!msg) {
        rc = RW_ERR_INTERNAL;
        goto done;
    }
    if (!rw_auth_ed25519_verify(pub, (const uint8_t *)msg, msg_len, sig)) {
        rc = RW_ERR_SIG;
        goto done;
    }

    /* ---- 4. |ts - now| <= window ---- */
    if (ops->clock_valid) {
        int64_t delta = ops->now - out->ts;
        if (delta < 0) {
            delta = -delta;
        }
        if (delta > (int64_t)ops->ts_window) {
            ESP_LOGW(TAG, "stale: ts=%lld now=%lld delta=%llds",
                     (long long)out->ts, (long long)ops->now, (long long)delta);
            rc = RW_ERR_STALE;
            goto done;
        }
    } else {
        ESP_LOGW(TAG, "clock not synced — ts window NOT enforced for id=%s",
                 out->id);
    }

    /* ---- 5. ctr > lastCtr[kid]; persist BEFORE executing ---- */
    if (ops->get_ctr) {
        (void)ops->get_ctr(ops->ctx, out->kid, &last);
    }
    if (out->ctr <= last) {
        ESP_LOGW(TAG, "replay: kid=%s ctr=%llu <= hwm=%llu", out->kid,
                 (unsigned long long)out->ctr, (unsigned long long)last);
        rc = RW_ERR_REPLAY;
        goto done;
    }
    if (!ops->set_ctr || ops->set_ctr(ops->ctx, out->kid, out->ctr) != ESP_OK) {
        /* Refuse to execute if we cannot make the counter durable first —
         * otherwise a power loss would re-open the replay window. */
        ESP_LOGE(TAG, "counter persist failed for kid=%s", out->kid);
        rc = RW_ERR_INTERNAL;
        goto done;
    }

    /* ---- 6. act is known and enabled ---- */
    if (!ops->act_enabled || !ops->act_enabled(ops->ctx, out->act)) {
        rc = RW_ERR_UNKNOWN_ACT;
        goto done;
    }

    rc = RW_OK;

done:
    free(msg);
    if (rc == RW_OK) {
        out->_root = root;   /* keeps out->args alive */
    } else {
        cJSON_Delete(root);
        out->args  = NULL;
        out->_root = NULL;
    }
    return rc;
}
