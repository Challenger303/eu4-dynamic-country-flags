#ifndef CPBM_SHA256_H
#define CPBM_SHA256_H

#include <stddef.h>

typedef struct {
    unsigned int   state[8];
    unsigned long long bits;
    unsigned char  buf[64];
    size_t         len;
} cpbm_sha256;

void cpbm_sha256_init(cpbm_sha256 *c);
void cpbm_sha256_update(cpbm_sha256 *c, const void *data, size_t n);
void cpbm_sha256_final(cpbm_sha256 *c, unsigned char out[32]);

/* Hash a file. Returns 1 on success, 0 if it could not be read. */
int  cpbm_sha256_file(const wchar_t *path, unsigned char out[32]);

/* 64 lowercase hex chars + NUL into `out`. */
void cpbm_sha256_hex(const unsigned char digest[32], char out[65]);

#endif
