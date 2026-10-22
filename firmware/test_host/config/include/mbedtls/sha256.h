#pragma once
/* Only the SoftAP password uses SHA-256; its value is irrelevant here. */
#include <stddef.h>
#include <stdint.h>
typedef struct { int unused; } mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context *c) { (void)c; }
static inline void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }
static inline int mbedtls_sha256_starts(mbedtls_sha256_context *c, int is224)
{ (void)c; (void)is224; return 0; }
static inline int mbedtls_sha256_update(mbedtls_sha256_context *c, const uint8_t *d, size_t n)
{ (void)c; (void)d; (void)n; return 0; }
static inline int mbedtls_sha256_finish(mbedtls_sha256_context *c, uint8_t out[32])
{ (void)c; for (int i = 0; i < 32; i++) out[i] = (uint8_t)i; return 0; }
