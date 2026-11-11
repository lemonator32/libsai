/*
 * Block ciphers and checksums of the SAI virtual file system.
 *
 * Recovered from sai.exe 1.2.6-Beta.3:
 *   0x4bd720  table block encrypt     0x4bd860  table block decrypt
 *   0x4be2a0  data block encrypt      0x4be170  data block decrypt
 *   0x4be3e0  data block checksum     0x4be3b0  table block checksum
 *
 * Both ciphers are chained over the 1024 little-endian words of a block and
 * use a whitening function of the previous ciphertext word:
 *
 *   K(x) = key[x >> 24] + key[(x >> 16) & 255] + key[(x >> 8) & 255]
 *        + key[x & 255]                                  (mod 2^32)
 */
#include "sai_internal.h"

static inline uint32_t rotl1(uint32_t x) { return (x << 1) | (x >> 31); }
static inline uint32_t rot16(uint32_t x) { return (x << 16) | (x >> 16); }

static inline uint32_t whiten(const uint32_t *key, uint32_t x)
{
    return key[x >> 24] + key[(x >> 16) & 0xFF] + key[(x >> 8) & 0xFF] +
           key[x & 0xFF];
}

uint32_t sai_checksum_data(const uint32_t *w)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++)
        s = rotl1(s) ^ w[i];
    return s | 1u;
}

uint32_t sai_checksum_table(const uint32_t *w)
{
    uint32_t s = 0;
    for (unsigned i = 1; i < SAI_WORDS_PER_BLOCK; i++)
        s = rotl1(s) ^ w[i];
    return s | 1u;
}

/* Table blocks: seed = block index (a multiple of 512).
 *   c[i] = c[i-1] ^ rot16(p[i]) ^ K(c[i-1]),  c[-1] = index */
void sai_encrypt_table(const uint32_t *key, uint32_t index, uint32_t *w)
{
    uint32_t prev = index;
    for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++) {
        prev = prev ^ rot16(w[i]) ^ whiten(key, prev);
        w[i] = prev;
    }
}

void sai_decrypt_table(const uint32_t *key, uint32_t index, uint32_t *w)
{
    uint32_t prev = index;
    for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++) {
        uint32_t c = w[i];
        w[i] = rot16(whiten(key, prev) ^ c ^ prev);
        prev = c;
    }
}

/* Data blocks: seed = the block's checksum stored in its table entry.
 *   c[i] = p[i] + (K(c[i-1]) ^ c[i-1]),  c[-1] = seed */
void sai_encrypt_data(const uint32_t *key, uint32_t seed, uint32_t *w)
{
    uint32_t prev = seed;
    for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++) {
        w[i] = w[i] + (whiten(key, prev) ^ prev);
        prev = w[i];
    }
}

void sai_decrypt_data(const uint32_t *key, uint32_t seed, uint32_t *w)
{
    uint32_t prev = seed;
    for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++) {
        uint32_t c = w[i];
        w[i] = c - (whiten(key, prev) ^ prev);
        prev = c;
    }
}

const uint32_t *sai_key_document(void) { return sai_key_user; }
const uint32_t *sai_key_system_vfs(void) { return sai_key_system; }
