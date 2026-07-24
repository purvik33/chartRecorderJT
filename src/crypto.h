/* crypto.h - SHA-256 (public-domain style), used for the tamper-evident
 * audit-trail hash chain (21 CFR Part 11). */
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>

void sha256(const void *data, size_t len, unsigned char out[32]);
/* SHA-256 of a NUL-terminated string, written as 64 lowercase hex chars. */
void sha256_hex(const char *str, char hex[65]);

#endif
