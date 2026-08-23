/* Unity tests for auth.c against docs/test-vectors.json.
 *
 * The vectors below are copied verbatim from docs/test-vectors.json (test key
 * only — never deployed). If that file changes, change this one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "b64url.h"
#include "canon_json.h"
#include "cJSON.h"
#include "unity.h"

/* pubkey_hex from docs/test-vectors.json */
static const uint8_t k_pub[32] = {
    0x7b, 0x4c, 0xa4, 0xf6, 0xe3, 0xc7, 0xcc, 0xff,
    0x77, 0x96, 0x79, 0x46, 0x4b, 0xda, 0x13, 0x0a,
    0x47, 0xfd, 0x28, 0x20, 0x33, 0x03, 0xd4, 0x40,
    0x7d, 0x93, 0x75, 0xbe, 0x7d, 0x55, 0x10, 0xad,
};

#define DEV "a1b2c3d4e5f60718"

typedef struct {
    const char *envelope;        /* exact JSON text to feed the verifier */
    const char *signing_string;  /* expected canonical signing string */
    int64_t     ts;
} vector_t;

static const vector_t k_vectors[] = {
    {
        .envelope =
            "{\"v\":1,\"dev\":\"" DEV "\","
            "\"id\":\"0f7c1c2e-9a4b-4c1d-8e2f-3a4b5c6d7e8f\","
            "\"ts\":1724457600,\"ctr\":1,\"act\":\"wake\",\"args\":{},"
            "\"kid\":\"p1\",\"sig\":\"uLroAu7moW368a_78D0IJubWbLN15TjgiVbowAeA"
            "VN4Roui7r2jRNpGuM8U_62Qvs1V_Wl1y4d6PGYD8PdDKAw\"}",
        .signing_string =
            "remote-wake-v1\n" DEV "\n0f7c1c2e-9a4b-4c1d-8e2f-3a4b5c6d7e8f\n"
            "1724457600\n1\nwake\n{}",
        .ts = 1724457600,
    },
    {
        /* Note the args keys are deliberately given in NON-canonical order
         * here: the device must re-serialize them sorted before verifying. */
        .envelope =
            "{\"v\":1,\"dev\":\"" DEV "\","
            "\"id\":\"1b9d6bcd-bbfd-4b2d-9b5d-ab8dfbbd4bed\","
            "\"ts\":1724457660,\"ctr\":2,\"act\":\"type\","
            "\"args\":{\"text\":\"hunter2!\",\"enter\":true},"
            "\"kid\":\"p1\",\"sig\":\"6TTF7R7qdcrIz5c64ow9M7YOwvmch-kxvDKTslID"
            "oySGzvOikTmgre-KgtZzUhhg_k2RJqE3mzsB0gQ2Dyi3DQ\"}",
        .signing_string =
            "remote-wake-v1\n" DEV "\n1b9d6bcd-bbfd-4b2d-9b5d-ab8dfbbd4bed\n"
            "1724457660\n2\ntype\n{\"enter\":true,\"text\":\"hunter2!\"}",
        .ts = 1724457660,
    },
    {
        .envelope =
            "{\"v\":1,\"dev\":\"" DEV "\","
            "\"id\":\"2c3e4f50-6172-4839-a0b1-c2d3e4f50617\","
            "\"ts\":1724457720,\"ctr\":3,\"act\":\"keys\","
            "\"args\":{\"seq\":[[\"LGUI\",\"L\"],[\"ENTER\"]]},"
            "\"kid\":\"p1\",\"sig\":\"BAUFe9GDMSUxPZeWtkgKZ8psk_ZScNGCJSq7tWgh"
            "ffStaM9eRnWKWZnwBA6J7ILDu93bV0trurgX7ZlI25RRDQ\"}",
        .signing_string =
            "remote-wake-v1\n" DEV "\n2c3e4f50-6172-4839-a0b1-c2d3e4f50617\n"
            "1724457720\n3\nkeys\n{\"seq\":[[\"LGUI\",\"L\"],[\"ENTER\"]]}",
        .ts = 1724457720,
    },
    {
        .envelope =
            "{\"v\":1,\"dev\":\"" DEV "\","
            "\"id\":\"3d4e5f60-7182-4930-b1c2-d3e4f5061728\","
            "\"ts\":1724457780,\"ctr\":4,\"act\":\"power_tap\","
            "\"args\":{\"ms\":200},"
            "\"kid\":\"p1\",\"sig\":\"EYPpGYY_z145tl8RgKKpbYb62SXfX3OkyOcSC89E"
            "y3OCLefMtkxgm_v57MdTM1bGw2PWFv2QcoHTpp9TMXLJBA\"}",
        .signing_string =
            "remote-wake-v1\n" DEV "\n3d4e5f60-7182-4930-b1c2-d3e4f5061728\n"
            "1724457780\n4\npower_tap\n{\"ms\":200}",
        .ts = 1724457780,
    },
    {
        .envelope =
            "{\"v\":1,\"dev\":\"" DEV "\","
            "\"id\":\"4e5f6071-8293-4a41-c2d3-e4f506172839\","
            "\"ts\":1724457840,\"ctr\":5,\"act\":\"power_hold\","
            "\"args\":{\"ms\":6000},"
            "\"kid\":\"p1\",\"sig\":\"yeNmEXPx1ZMrUkmZTMSXcrWX3ME4yatULohbPniX"
            "UU3tYEmisd2KCzTij1rdSM-AvyMQwbw7wlkkQxwsk4EGAA\"}",
        .signing_string =
            "remote-wake-v1\n" DEV "\n4e5f6071-8293-4a41-c2d3-e4f506172839\n"
            "1724457840\n5\npower_hold\n{\"ms\":6000}",
        .ts = 1724457840,
    },
};

#define N_VECTORS (sizeof(k_vectors) / sizeof(k_vectors[0]))

/* ------------------------------------------------------------ fake ops -- */

static uint64_t g_ctr[4];
static int      g_persist_calls;

static bool fake_get_key(void *ctx, const char *kid, uint8_t out[32])
{
    (void)ctx;
    if (strcmp(kid, "p1") != 0) {
        return false;
    }
    memcpy(out, k_pub, 32);
    return true;
}

static bool fake_get_ctr(void *ctx, const char *kid, uint64_t *out)
{
    (void)ctx;
    if (kid[0] != 'p' || kid[1] < '1' || kid[1] > '4') {
        return false;
    }
    *out = g_ctr[kid[1] - '1'];
    return true;
}

static esp_err_t fake_set_ctr(void *ctx, const char *kid, uint64_t ctr)
{
    (void)ctx;
    g_persist_calls++;
    g_ctr[kid[1] - '1'] = ctr;
    return ESP_OK;
}

static bool g_disable_acts;

static bool fake_act_enabled(void *ctx, const char *act)
{
    (void)ctx;
    if (g_disable_acts) {
        return false;
    }
    static const char *known[] = {"ping",      "status",    "wake",
                                  "type",      "keys",      "power_tap",
                                  "power_hold"};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(act, known[i]) == 0) {
            return true;
        }
    }
    return false;
}

static rw_auth_ops_t make_ops(int64_t now)
{
    rw_auth_ops_t ops = {
        .device_id   = DEV,
        .now         = now,
        .ts_window   = 90,
        .clock_valid = true,
        .ctx         = NULL,
        .get_key     = fake_get_key,
        .get_ctr     = fake_get_ctr,
        .set_ctr     = fake_set_ctr,
        .act_enabled = fake_act_enabled,
    };
    return ops;
}

static void reset_state(void)
{
    memset(g_ctr, 0, sizeof(g_ctr));
    g_persist_calls = 0;
    g_disable_acts = false;
}

/* Replace the signature of @p envelope with @p sig (same length). */
static char *with_broken_sig(const char *envelope)
{
    char *copy = strdup(envelope);
    TEST_ASSERT_NOT_NULL(copy);
    char *p = strstr(copy, "\"sig\":\"");
    TEST_ASSERT_NOT_NULL(p);
    /* Flip one base64 character — still decodes, no longer verifies. */
    p += 7;
    *p = (*p == 'A') ? 'B' : 'A';
    return copy;
}

/* --------------------------------------------------------------- tests -- */

TEST_CASE("signing string matches the shared vectors", "[auth]")
{
    for (size_t i = 0; i < N_VECTORS; i++) {
        cJSON *root = cJSON_Parse(k_vectors[i].envelope);
        TEST_ASSERT_NOT_NULL_MESSAGE(root, "envelope must parse");

        const cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
        const cJSON *id   = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON *act  = cJSON_GetObjectItemCaseSensitive(root, "act");
        const cJSON *ts   = cJSON_GetObjectItemCaseSensitive(root, "ts");
        const cJSON *ctr  = cJSON_GetObjectItemCaseSensitive(root, "ctr");

        size_t len = 0;
        char *s = rw_auth_signing_string(DEV, id->valuestring,
                                         (int64_t)ts->valuedouble,
                                         (uint64_t)ctr->valuedouble,
                                         act->valuestring, args, &len);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQUAL_STRING(k_vectors[i].signing_string, s);
        TEST_ASSERT_EQUAL_UINT32(strlen(k_vectors[i].signing_string), len);
        free(s);
        cJSON_Delete(root);
    }
}

TEST_CASE("canonical json sorts keys and strips whitespace", "[auth]")
{
    cJSON *o = cJSON_Parse("{ \"z\" : 1 , \"a\" : { \"y\" : [1,2] , "
                           "\"b\" : \"x\" } , \"m\" : false }");
    TEST_ASSERT_NOT_NULL(o);
    char *s = rw_canon_json(o);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("{\"a\":{\"b\":\"x\",\"y\":[1,2]},\"m\":false,"
                             "\"z\":1}", s);
    free(s);
    cJSON_Delete(o);

    cJSON *e = cJSON_Parse("{}");
    char *es = rw_canon_json(e);
    TEST_ASSERT_EQUAL_STRING("{}", es);
    free(es);
    cJSON_Delete(e);
}

TEST_CASE("base64url round trip", "[auth]")
{
    const uint8_t in[32] = {0x7b, 0x4c, 0xa4, 0xf6};
    char enc[64];
    TEST_ASSERT_EQUAL_INT(43, rw_b64url_encode(in, 32, enc, sizeof(enc)));
    uint8_t out[32];
    TEST_ASSERT_EQUAL_INT(32, rw_b64url_decode(enc, strlen(enc), out,
                                               sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(in, out, 32);
    /* Wrong length must not silently truncate. */
    TEST_ASSERT_EQUAL_INT(-1, rw_b64url_decode(enc, strlen(enc), out, 16));
    TEST_ASSERT_EQUAL_INT(-1, rw_b64url_decode("!!!!", 4, out, sizeof(out)));
}

TEST_CASE("all five vectors verify in order", "[auth]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rw_auth_init());
    reset_state();

    for (size_t i = 0; i < N_VECTORS; i++) {
        rw_auth_ops_t ops = make_ops(k_vectors[i].ts);
        rw_cmd_t cmd;
        rw_auth_err_t rc = rw_auth_verify(&ops, k_vectors[i].envelope,
                                          strlen(k_vectors[i].envelope), &cmd);
        TEST_ASSERT_EQUAL_MESSAGE(RW_OK, rc, rw_auth_err_str(rc));
        TEST_ASSERT_EQUAL_UINT64(i + 1, cmd.ctr);
        TEST_ASSERT_EQUAL_STRING("p1", cmd.kid);
        TEST_ASSERT_NOT_NULL(cmd.args);
        rw_cmd_free(&cmd);
    }
    /* The high-water mark was persisted once per accepted command, before
     * the caller ever got to execute it. */
    TEST_ASSERT_EQUAL_INT((int)N_VECTORS, g_persist_calls);
    TEST_ASSERT_EQUAL_UINT64(N_VECTORS, g_ctr[0]);
}

TEST_CASE("replayed counter is rejected", "[auth]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rw_auth_init());
    reset_state();

    rw_auth_ops_t ops = make_ops(k_vectors[0].ts);
    rw_cmd_t cmd;
    TEST_ASSERT_EQUAL(RW_OK, rw_auth_verify(&ops, k_vectors[0].envelope,
                                            strlen(k_vectors[0].envelope),
                                            &cmd));
    rw_cmd_free(&cmd);

    int persisted = g_persist_calls;
    rw_auth_err_t rc = rw_auth_verify(&ops, k_vectors[0].envelope,
                                      strlen(k_vectors[0].envelope), &cmd);
    TEST_ASSERT_EQUAL(RW_ERR_REPLAY, rc);
    TEST_ASSERT_EQUAL_STRING("replay", rw_auth_err_str(rc));
    TEST_ASSERT_EQUAL_INT(persisted, g_persist_calls);
    rw_cmd_free(&cmd);
}

TEST_CASE("stale timestamp is rejected outside the 90 s window", "[auth]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rw_auth_init());
    reset_state();

    rw_cmd_t cmd;
    /* Exactly on the boundary: accepted. */
    rw_auth_ops_t edge = make_ops(k_vectors[0].ts + 90);
    TEST_ASSERT_EQUAL(RW_OK, rw_auth_verify(&edge, k_vectors[0].envelope,
                                            strlen(k_vectors[0].envelope),
                                            &cmd));
    rw_cmd_free(&cmd);

    reset_state();
    rw_auth_ops_t late = make_ops(k_vectors[0].ts + 91);
    TEST_ASSERT_EQUAL(RW_ERR_STALE,
                      rw_auth_verify(&late, k_vectors[0].envelope,
                                     strlen(k_vectors[0].envelope), &cmd));
    rw_cmd_free(&cmd);

    reset_state();
    rw_auth_ops_t early = make_ops(k_vectors[0].ts - 91);
    TEST_ASSERT_EQUAL(RW_ERR_STALE,
                      rw_auth_verify(&early, k_vectors[0].envelope,
                                     strlen(k_vectors[0].envelope), &cmd));
    /* A stale command must never move the counter. */
    TEST_ASSERT_EQUAL_INT(0, g_persist_calls);
    rw_cmd_free(&cmd);
}

TEST_CASE("tampered signature is rejected", "[auth]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rw_auth_init());
    reset_state();

    char *bad = with_broken_sig(k_vectors[0].envelope);
    rw_auth_ops_t ops = make_ops(k_vectors[0].ts);
    rw_cmd_t cmd;
    TEST_ASSERT_EQUAL(RW_ERR_SIG,
                      rw_auth_verify(&ops, bad, strlen(bad), &cmd));
    TEST_ASSERT_EQUAL_INT(0, g_persist_calls);
    rw_cmd_free(&cmd);
    free(bad);
}

TEST_CASE("tampered args are rejected", "[auth]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rw_auth_init());
    reset_state();

    /* Same signature, different power_hold duration. */
    const char *tampered =
        "{\"v\":1,\"dev\":\"" DEV "\","
        "\"id\":\"4e5f6071-8293-4a41-c2d3-e4f506172839\","
        "\"ts\":1724457840,\"ctr\":5,\"act\":\"power_hold\","
        "\"args\":{\"ms\":12000},"
        "\"kid\":\"p1\",\"sig\":\"yeNmEXPx1ZMrUkmZTMSXcrWX3ME4yatULohbPniX"
        "UU3tYEmisd2KCzTij1rdSM-AvyMQwbw7wlkkQxwsk4EGAA\"}";

    rw_auth_ops_t ops = make_ops(1724457840);
    rw_cmd_t cmd;
    TEST_ASSERT_EQUAL(RW_ERR_SIG,
                      rw_auth_verify(&ops, tampered, strlen(tampered), &cmd));
    rw_cmd_free(&cmd);
}

TEST_CASE("wrong device, version, key slot and action are rejected", "[auth]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rw_auth_init());
    reset_state();

    rw_cmd_t cmd;

    /* dev mismatch: our id is something else entirely. */
    rw_auth_ops_t other = make_ops(k_vectors[0].ts);
    other.device_id = "0000000000000000";
    TEST_ASSERT_EQUAL(RW_ERR_DEV,
                      rw_auth_verify(&other, k_vectors[0].envelope,
                                     strlen(k_vectors[0].envelope), &cmd));
    rw_cmd_free(&cmd);

    /* v != 1 */
    const char *v2 =
        "{\"v\":2,\"dev\":\"" DEV "\",\"id\":\"x\",\"ts\":1724457600,"
        "\"ctr\":1,\"act\":\"wake\",\"args\":{},\"kid\":\"p1\",\"sig\":\"AA\"}";
    rw_auth_ops_t ops = make_ops(k_vectors[0].ts);
    TEST_ASSERT_EQUAL(RW_ERR_VERSION,
                      rw_auth_verify(&ops, v2, strlen(v2), &cmd));
    rw_cmd_free(&cmd);

    /* unknown kid */
    const char *p9 =
        "{\"v\":1,\"dev\":\"" DEV "\",\"id\":\"x\",\"ts\":1724457600,"
        "\"ctr\":1,\"act\":\"wake\",\"args\":{},\"kid\":\"p3\",\"sig\":\"AA\"}";
    TEST_ASSERT_EQUAL(RW_ERR_UNKNOWN_KEY,
                      rw_auth_verify(&ops, p9, strlen(p9), &cmd));
    rw_cmd_free(&cmd);

    /* missing args object */
    const char *noargs =
        "{\"v\":1,\"dev\":\"" DEV "\",\"id\":\"x\",\"ts\":1724457600,"
        "\"ctr\":1,\"act\":\"wake\",\"kid\":\"p1\",\"sig\":\"AA\"}";
    TEST_ASSERT_EQUAL(RW_ERR_BAD_JSON,
                      rw_auth_verify(&ops, noargs, strlen(noargs), &cmd));
    rw_cmd_free(&cmd);

    /* garbage */
    const char *junk = "not json at all";
    TEST_ASSERT_EQUAL(RW_ERR_BAD_JSON,
                      rw_auth_verify(&ops, junk, strlen(junk), &cmd));
    rw_cmd_free(&cmd);

    TEST_ASSERT_EQUAL_INT(0, g_persist_calls);
}

TEST_CASE("disabled action fails last, after the counter is persisted", "[auth]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rw_auth_init());
    reset_state();
    g_disable_acts = true;

    rw_auth_ops_t ops = make_ops(k_vectors[0].ts);
    rw_cmd_t cmd;
    TEST_ASSERT_EQUAL(RW_ERR_UNKNOWN_ACT,
                      rw_auth_verify(&ops, k_vectors[0].envelope,
                                     strlen(k_vectors[0].envelope), &cmd));
    /* Protocol step 5 runs before step 6, so the counter has already moved —
     * the command can never be replayed even though it was refused. */
    TEST_ASSERT_EQUAL_INT(1, g_persist_calls);
    TEST_ASSERT_EQUAL_UINT64(1, g_ctr[0]);
    rw_cmd_free(&cmd);
}

TEST_CASE("peek_id recovers the id from an unverifiable envelope", "[auth]")
{
    char id[64];
    rw_auth_peek_id(k_vectors[0].envelope, strlen(k_vectors[0].envelope), id,
                    sizeof(id));
    TEST_ASSERT_EQUAL_STRING("0f7c1c2e-9a4b-4c1d-8e2f-3a4b5c6d7e8f", id);

    rw_auth_peek_id("garbage", 7, id, sizeof(id));
    TEST_ASSERT_EQUAL_STRING("", id);
}

/* ------------------------------------------------------------- runner --- */

void app_main(void)
{
    printf("\nremote-wake auth tests\n");
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
