// Android stand-in for CommonCrypto's SHA-256 (plat_android.c carries a small portable implementation)
#pragma once
#include "dm_android.h"
typedef uint32_t CC_LONG;
#define CC_SHA256_DIGEST_LENGTH 32
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } CC_SHA256_CTX;
int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *d, CC_LONG n);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
unsigned char *CC_SHA256(const void *d, CC_LONG n, unsigned char *md);
