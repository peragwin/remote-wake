#include "canon_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ sbuf -- */

static bool sbuf_reserve(rw_sbuf_t *sb, size_t extra)
{
    if (sb->err) {
        return false;
    }
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) {
        return true;
    }
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    char *nb = realloc(sb->buf, cap);
    if (!nb) {
        sb->err = true;
        return false;
    }
    sb->buf = nb;
    sb->cap = cap;
    return true;
}

void rw_sbuf_append(rw_sbuf_t *sb, const char *data, size_t n)
{
    if (!sbuf_reserve(sb, n)) {
        return;
    }
    memcpy(sb->buf + sb->len, data, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void rw_sbuf_puts(rw_sbuf_t *sb, const char *s)
{
    if (s) {
        rw_sbuf_append(sb, s, strlen(s));
    }
}

void rw_sbuf_puti(rw_sbuf_t *sb, long long v)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lld", v);
    if (n > 0) {
        rw_sbuf_append(sb, tmp, (size_t)n);
    }
}

void rw_sbuf_putu(rw_sbuf_t *sb, unsigned long long v)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%llu", v);
    if (n > 0) {
        rw_sbuf_append(sb, tmp, (size_t)n);
    }
}

void rw_sbuf_free(rw_sbuf_t *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = sb->cap = 0;
    sb->err = false;
}

/* ------------------------------------------------------------ primitives -- */

static void emit_string(rw_sbuf_t *sb, const char *s)
{
    rw_sbuf_append(sb, "\"", 1);
    if (!s) {
        rw_sbuf_append(sb, "\"", 1);
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  rw_sbuf_append(sb, "\\\"", 2); break;
        case '\\': rw_sbuf_append(sb, "\\\\", 2); break;
        case '\b': rw_sbuf_append(sb, "\\b", 2);  break;
        case '\f': rw_sbuf_append(sb, "\\f", 2);  break;
        case '\n': rw_sbuf_append(sb, "\\n", 2);  break;
        case '\r': rw_sbuf_append(sb, "\\r", 2);  break;
        case '\t': rw_sbuf_append(sb, "\\t", 2);  break;
        default:
            if (c < 0x20) {
                char esc[7];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                rw_sbuf_append(sb, esc, 6);
            } else {
                /* Includes all UTF-8 continuation bytes: pass through. */
                rw_sbuf_append(sb, (const char *)&c, 1);
            }
            break;
        }
    }
    rw_sbuf_append(sb, "\"", 1);
}

static void emit_number(rw_sbuf_t *sb, double d)
{
    if (isnan(d) || isinf(d)) {
        /* JSON has no representation; JSON.stringify() emits null. */
        rw_sbuf_puts(sb, "null");
        return;
    }
    /* Integral values in the exactly-representable range print as integers,
     * which is what JSON.stringify() does and what the vectors expect
     * ({"ms":200}, not {"ms":200.0}). */
    if (d == floor(d) && fabs(d) < 9007199254740992.0) {
        rw_sbuf_puti(sb, (long long)d);
        return;
    }
    /* Shortest representation that round-trips, mirroring JS number->string
     * closely enough for the arguments this protocol carries. */
    char tmp[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(tmp, sizeof(tmp), "%.*g", prec, d);
        if (strtod(tmp, NULL) == d) {
            break;
        }
    }
    rw_sbuf_puts(sb, tmp);
}

/* ------------------------------------------------------------- key sorting -- */

static int key_cmp(const void *a, const void *b)
{
    const cJSON *const *pa = (const cJSON *const *)a;
    const cJSON *const *pb = (const cJSON *const *)b;
    const char *ka = (*pa)->string ? (*pa)->string : "";
    const char *kb = (*pb)->string ? (*pb)->string : "";
    /* Byte-wise (unsigned) lexicographic order over the UTF-8 key bytes. */
    int r = strcmp(ka, kb);
    if (r != 0) {
        return r;
    }
    /* Deterministic tie-break for (invalid but possible) duplicate keys. */
    return (*pa < *pb) ? -1 : ((*pa > *pb) ? 1 : 0);
}

static void emit_object(rw_sbuf_t *sb, const cJSON *obj)
{
    size_t n = 0;
    for (const cJSON *c = obj->child; c; c = c->next) {
        n++;
    }

    rw_sbuf_append(sb, "{", 1);
    if (n == 0) {
        rw_sbuf_append(sb, "}", 1);
        return;
    }

    const cJSON **items = calloc(n, sizeof(*items));
    if (!items) {
        sb->err = true;
        rw_sbuf_append(sb, "}", 1);
        return;
    }
    size_t i = 0;
    for (const cJSON *c = obj->child; c; c = c->next) {
        items[i++] = c;
    }
    qsort(items, n, sizeof(*items), key_cmp);

    for (i = 0; i < n; i++) {
        if (i) {
            rw_sbuf_append(sb, ",", 1);
        }
        emit_string(sb, items[i]->string);
        rw_sbuf_append(sb, ":", 1);
        rw_canon_json_append(sb, items[i]);
    }
    free(items);
    rw_sbuf_append(sb, "}", 1);
}

/* ------------------------------------------------------------------- api -- */

void rw_canon_json_append(rw_sbuf_t *sb, const cJSON *item)
{
    if (!item) {
        rw_sbuf_puts(sb, "null");
        return;
    }
    switch (item->type & 0xFF) {
    case cJSON_NULL:
    case cJSON_Invalid:
        rw_sbuf_puts(sb, "null");
        break;
    case cJSON_False:
        rw_sbuf_puts(sb, "false");
        break;
    case cJSON_True:
        rw_sbuf_puts(sb, "true");
        break;
    case cJSON_Number:
        emit_number(sb, item->valuedouble);
        break;
    case cJSON_String:
        emit_string(sb, item->valuestring);
        break;
    case cJSON_Raw:
        /* We never produce these ourselves; emit verbatim. */
        rw_sbuf_puts(sb, item->valuestring ? item->valuestring : "null");
        break;
    case cJSON_Array: {
        rw_sbuf_append(sb, "[", 1);
        bool first = true;
        for (const cJSON *c = item->child; c; c = c->next) {
            if (!first) {
                rw_sbuf_append(sb, ",", 1);
            }
            first = false;
            rw_canon_json_append(sb, c);
        }
        rw_sbuf_append(sb, "]", 1);
        break;
    }
    case cJSON_Object:
        emit_object(sb, item);
        break;
    default:
        rw_sbuf_puts(sb, "null");
        break;
    }
}

char *rw_canon_json(const cJSON *item)
{
    rw_sbuf_t sb = {0};
    rw_canon_json_append(&sb, item);
    if (sb.err || !sb.buf) {
        rw_sbuf_free(&sb);
        return NULL;
    }
    return sb.buf; /* ownership transfers to the caller */
}
