/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VISUALPTT_SHA256_H
#define VISUALPTT_SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
} VisualPttSha256;

void visualptt_sha256_init(VisualPttSha256 *ctx);
/* Returns -1 if the message would exceed SHA-256's 64-bit bit-length limit. */
int visualptt_sha256_update(VisualPttSha256 *ctx, const void *data, size_t size);
/* Finalizes the context, producing the standard 32-byte SHA-256 digest. */
void visualptt_sha256_final(VisualPttSha256 *ctx, unsigned char digest[32]);
#endif
