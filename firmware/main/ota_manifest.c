#include "ota_manifest.h"
#include "auth.h"
#include "b64url.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(rw_ota_image_identity_t) == 236, "OTA identity wire size");
static bool bounded_text(const char *s, size_t capacity, bool identifier)
{
    size_t n = 0;
    if (!s || !*s) return false;
    for (; n < capacity && s[n]; ++n) {
        unsigned char c = s[n];
        if (identifier ? !(isalnum(c) || c == '-' || c == '_' || c == '.') : (c < 33 || c > 126)) return false;
    }
    return n < capacity;
}
bool rw_ota_identity_valid(const rw_ota_image_identity_t *i)
{
    return i && !memcmp(i->magic, RW_OTA_IDENTITY_MAGIC, 16) && i->format_version == 1 &&
        i->updater_api == RW_OTA_API_VERSION && i->release &&
        bounded_text(i->product, sizeof(i->product), true) && bounded_text(i->board, sizeof(i->board), true) &&
        bounded_text(i->profile, sizeof(i->profile), true) && bounded_text(i->channel, sizeof(i->channel), true) &&
        bounded_text(i->layout, sizeof(i->layout), true) && bounded_text(i->chip, sizeof(i->chip), true) &&
        bounded_text(i->version, sizeof(i->version), false);
}
bool rw_ota_identity_tuple_equal(const rw_ota_image_identity_t *a, const rw_ota_image_identity_t *b)
{
    return !strcmp(a->product, b->product) && !strcmp(a->board, b->board) &&
        !strcmp(a->profile, b->profile) && !strcmp(a->channel, b->channel) &&
        !strcmp(a->layout, b->layout) && !strcmp(a->chip, b->chip);
}
bool rw_ota_https_host(const char *url, char *host, size_t capacity)
{
    if (!url || strncmp(url, "https://", 8)) return false;
    const char *p = url + 8, *end = strchr(p, '/');
    if (!end || end == p || (size_t)(end - p) >= capacity) return false;
    for (const char *s = p; s < end; ++s) if (!(isalnum((unsigned char)*s) || *s == '.' || *s == '-')) return false;
    for (const char *s = end; *s; ++s) if ((unsigned char)*s < 33 || (unsigned char)*s > 126 || *s == '\\' || *s == '#') return false;
    memcpy(host, p, end - p); host[end - p] = 0;
    return true;
}
bool rw_ota_url_in_source(const char *url, const char *prefix)
{
    char host[128];
    if (!prefix || !url || strlen(url) > RW_OTA_URL_MAX || strlen(prefix) > 383 ||
        !rw_ota_https_host(url, host, sizeof(host)) || !rw_ota_https_host(prefix, host, sizeof(host))) return false;
    size_t n = strlen(prefix);
    /* Release-source URLs are deliberately simple immutable asset paths.
     * Disallow encoded traversal, queries and fragments in initial requests. */
    return n && prefix[n - 1] == '/' && !strncmp(url, prefix, n) &&
        !strchr(url, '%') && !strchr(url, '?') && !strstr(url + 8, "/../") &&
        !strstr(url + 8, "/./") && strcmp(url + strlen(url) - (strlen(url) >= 3 ? 3 : 0), "/..");
}
/* Reject nested JSON before invoking cJSON, so depth/allocation are bounded.
 * Unicode escapes are unnecessary for this ASCII wire contract and can hide
 * embedded NULs in cJSON strings/keys. */
static cJSON *flat_object(const char *json, size_t length, size_t expected)
{
    if (!json || !length || memchr(json, 0, length)) return NULL;
    bool string = false, escape = false; unsigned depth = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = json[i];
        if (c > 126 || (!string && c < 32 && !isspace(c))) return NULL;
        if (string) {
            if (escape) { if (c == 'u') return NULL; escape = false; }
            else if (c == '\\') escape = true;
            else if (c == '"') string = false;
        } else if (c == '"') string = true;
        else if (c == '{') { if (++depth != 1) return NULL; }
        else if (c == '}') { if (depth-- != 1) return NULL; }
        else if (c == '[' || c == ']') return NULL;
    }
    if (string || depth) return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    while (end < json + length && isspace((unsigned char)*end)) ++end;
    if (end != json + length) { cJSON_Delete(root); return NULL; }
    size_t count = 0;
    for (cJSON *a = root->child; a; a = a->next) {
        if (!a->string || (!cJSON_IsString(a) && !cJSON_IsNumber(a))) goto bad;
        ++count;
        for (cJSON *b = a->next; b; b = b->next) if (!strcmp(a->string, b->string)) goto bad;
    }
    if (count == expected) return root;
bad:
    cJSON_Delete(root); return NULL;
}
static bool text(const cJSON *root, const char *name, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(v) || !bounded_text(v->valuestring, size, false)) return false;
    strcpy(out, v->valuestring); return true;
}
static bool number(const cJSON *root, const char *name, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > UINT32_MAX || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (uint32_t)v->valuedouble; return true;
}
static int decode(const char *text, uint8_t *out, size_t capacity)
{
    size_t n = strlen(text);
    if (!n || n > RW_B64URL_ENC_LEN(capacity)) return -1;
    for (size_t i = 0; i < n; ++i) if (!(isalnum((unsigned char)text[i]) || text[i] == '-' || text[i] == '_')) return -1;
    int count = rw_b64url_decode(text, n, out, capacity);
    if (count < 0) return -1;
    char *canonical = malloc(n + 1);
    if (!canonical) return -1;
    int chars = rw_b64url_encode(out, count, canonical, n + 1);
    bool same = chars == (int)n && !memcmp(canonical, text, n);
    free(canonical); return same ? count : -1;
}
esp_err_t rw_ota_manifest_verify(const rw_ota_config_t *config, const char *json, size_t length,
    bool clock_valid, int64_t now, uint32_t flash_size, uint32_t psram_size,
    uint32_t release_floor, rw_ota_manifest_t *out)
{
    if (!config || !out || !rw_ota_identity_valid(config->image_identity) || !length || length > RW_OTA_MANIFEST_MAX) return ESP_ERR_INVALID_ARG;
    if (!clock_valid || now < 1700000000) return ESP_ERR_INVALID_STATE;
    memset(out, 0, sizeof(*out));
    cJSON *envelope = flat_object(json, length, 4), *payload = NULL;
    char *raw = NULL, *message = NULL;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!envelope) return err;
    uint32_t v;
    char kid[16]; uint8_t sig[64];
    if (!number(envelope, "v", &v) || v != 1 || !text(envelope, "kid", kid, sizeof(kid))) goto done;
    const cJSON *encoded = cJSON_GetObjectItemCaseSensitive(envelope, "payload");
    const cJSON *signature = cJSON_GetObjectItemCaseSensitive(envelope, "sig");
    if (!cJSON_IsString(encoded) || !cJSON_IsString(signature) || decode(signature->valuestring, sig, sizeof(sig)) != 64) goto done;
    raw = malloc(RW_OTA_PAYLOAD_MAX + 1);
    if (!raw) { err = ESP_ERR_NO_MEM; goto done; }
    int bytes = decode(encoded->valuestring, (uint8_t *)raw, RW_OTA_PAYLOAD_MAX);
    if (bytes <= 0) goto done;
    raw[bytes] = 0;
    const rw_ota_trusted_key_t *key = NULL;
    if (config->trusted_key_count > RW_OTA_MAX_KEYS) goto done;
    for (size_t i = 0; i < config->trusted_key_count; ++i) {
        if (!strcmp(config->trusted_keys[i].kid, kid)) { key = &config->trusted_keys[i]; break; }
    }
    if (!key) { err = ESP_ERR_NOT_SUPPORTED; goto done; }
    const size_t prefix = sizeof(RW_OTA_SIGNATURE_DOMAIN) - 1;
    message = malloc(prefix + bytes);
    if (!message) { err = ESP_ERR_NO_MEM; goto done; }
    memcpy(message, RW_OTA_SIGNATURE_DOMAIN, prefix); memcpy(message + prefix, raw, bytes);
    if (!rw_auth_ed25519_verify(key->public_key, (uint8_t *)message, prefix + bytes, sig)) { err = ESP_ERR_INVALID_CRC; goto done; }
    payload = flat_object(raw, bytes, 18);
    if (!payload) goto done;
    rw_ota_image_identity_t *i = &out->identity;
    memcpy(i->magic, RW_OTA_IDENTITY_MAGIC, 16); i->format_version = 1;
    char hash[65];
    if (!number(payload, "v", &v) || v != 1 || !number(payload, "min_updater", &i->updater_api) ||
        !number(payload, "release", &i->release) || !text(payload, "product", i->product, sizeof(i->product)) ||
        !text(payload, "board", i->board, sizeof(i->board)) || !text(payload, "profile", i->profile, sizeof(i->profile)) ||
        !text(payload, "channel", i->channel, sizeof(i->channel)) || !text(payload, "layout", i->layout, sizeof(i->layout)) ||
        !text(payload, "version", i->version, sizeof(i->version)) || !text(payload, "chip", i->chip, sizeof(i->chip)) ||
        !text(payload, "url", out->url, sizeof(out->url)) || !text(payload, "sha256", hash, sizeof(hash)) || strlen(hash) != 64 ||
        !number(payload, "size", &out->size) || !number(payload, "min_flash", &out->min_flash) ||
        !number(payload, "min_psram", &out->min_psram) || !number(payload, "published", &out->published) ||
        !number(payload, "expires", &out->expires) || !number(payload, "key_generation", &out->key_generation)) goto done;
    for (size_t n = 0; n < 32; ++n) {
        char a = hash[n * 2], b = hash[n * 2 + 1];
        if (!((a >= '0' && a <= '9') || (a >= 'a' && a <= 'f')) || !((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f'))) goto done;
        out->sha256[n] = (uint8_t)(((a <= '9' ? a - '0' : a - 'a' + 10) << 4) | (b <= '9' ? b - '0' : b - 'a' + 10));
    }
    err = ESP_ERR_NOT_SUPPORTED;
    if (!rw_ota_identity_valid(i) || !rw_ota_identity_tuple_equal(i, config->image_identity) ||
        out->key_generation != key->generation || out->min_flash > flash_size || out->min_psram > psram_size ||
        !out->size || !rw_ota_url_in_source(out->url, config->source_prefix)) goto done;
    err = ESP_ERR_INVALID_STATE;
    uint32_t effective_floor = release_floor > config->image_identity->release ? release_floor : config->image_identity->release;
    if (i->release <= effective_floor || out->published > (uint64_t)now + 300 || out->expires <= (uint64_t)now || out->expires <= out->published) goto done;
    err = ESP_OK;
done:
    free(raw); free(message); cJSON_Delete(payload); cJSON_Delete(envelope); return err;
}
