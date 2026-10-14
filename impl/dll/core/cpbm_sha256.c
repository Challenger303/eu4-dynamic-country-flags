/* cpbm_sha256.c - FIPS 180-4 SHA-256, used only by the build gate.
 *
 * Written out rather than calling bcrypt so the gate has no dependency that
 * could itself fail to load; the gate has to be the one thing that always runs.
 */

#include "cpbm_sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROR(x,2)  ^ ROR(x,13) ^ ROR(x,22))
#define BSIG1(x) (ROR(x,6)  ^ ROR(x,11) ^ ROR(x,25))
#define SSIG0(x) (ROR(x,7)  ^ ROR(x,18) ^ ((x) >> 3))
#define SSIG1(x) (ROR(x,17) ^ ROR(x,19) ^ ((x) >> 10))

static const unsigned int K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void block(cpbm_sha256 *c, const unsigned char *p)
{
    unsigned int w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) |
               ((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
    for (i = 16; i < 64; i++)
        w[i] = SSIG1(w[i-2]) + w[i-7] + SSIG0(w[i-15]) + w[i-16];

    a=c->state[0]; b=c->state[1]; cc=c->state[2]; d=c->state[3];
    e=c->state[4]; f=c->state[5]; g =c->state[6]; h=c->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + BSIG1(e) + CH(e,f,g) + K[i] + w[i];
        t2 = BSIG0(a) + MAJ(a,b,cc);
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g;  c->state[7]+=h;
}

void cpbm_sha256_init(cpbm_sha256 *c)
{
    c->state[0]=0x6a09e667u; c->state[1]=0xbb67ae85u;
    c->state[2]=0x3c6ef372u; c->state[3]=0xa54ff53au;
    c->state[4]=0x510e527fu; c->state[5]=0x9b05688cu;
    c->state[6]=0x1f83d9abu; c->state[7]=0x5be0cd19u;
    c->bits = 0;
    c->len  = 0;
}

void cpbm_sha256_update(cpbm_sha256 *c, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    c->bits += (unsigned long long)n * 8;
    while (n) {
        size_t take = 64 - c->len;
        if (take > n) take = n;
        memcpy(c->buf + c->len, p, take);
        c->len += take; p += take; n -= take;
        if (c->len == 64) { block(c, c->buf); c->len = 0; }
    }
}

void cpbm_sha256_final(cpbm_sha256 *c, unsigned char out[32])
{
    unsigned long long bits = c->bits;
    int i;

    c->buf[c->len++] = 0x80;
    if (c->len > 56) {
        while (c->len < 64) c->buf[c->len++] = 0;
        block(c, c->buf);
        c->len = 0;
    }
    while (c->len < 56) c->buf[c->len++] = 0;
    for (i = 7; i >= 0; i--) c->buf[c->len++] = (unsigned char)((bits >> (i * 8)) & 0xFF);
    block(c, c->buf);

    for (i = 0; i < 8; i++) {
        out[i*4+0] = (unsigned char)((c->state[i] >> 24) & 0xFF);
        out[i*4+1] = (unsigned char)((c->state[i] >> 16) & 0xFF);
        out[i*4+2] = (unsigned char)((c->state[i] >>  8) & 0xFF);
        out[i*4+3] = (unsigned char)( c->state[i]        & 0xFF);
    }
}

int cpbm_sha256_file(const wchar_t *path, unsigned char out[32])
{
    HANDLE h;
    cpbm_sha256 c;
    unsigned char *buf;
    DWORD got;
    int ok = 1;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, 1 << 20);
    if (!buf) { CloseHandle(h); return 0; }

    cpbm_sha256_init(&c);
    for (;;) {
        if (!ReadFile(h, buf, 1 << 20, &got, NULL)) { ok = 0; break; }
        if (got == 0) break;
        cpbm_sha256_update(&c, buf, got);
    }
    if (ok) cpbm_sha256_final(&c, out);

    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(h);
    return ok;
}

void cpbm_sha256_hex(const unsigned char digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i*2+0] = hex[(digest[i] >> 4) & 0xF];
        out[i*2+1] = hex[digest[i] & 0xF];
    }
    out[64] = 0;
}
