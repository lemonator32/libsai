/* Internal declarations shared by the libsai translation units. */
#ifndef LIBSAI_INTERNAL_H
#define LIBSAI_INTERNAL_H

#include "sai.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SAI_WORDS_PER_BLOCK (SAI_BLOCK_SIZE / 4u)

/* Directory-entry flag set on every used slot. */
#define SAI_ENTRY_USED 0x80000000u

/* Header block (block 1) layout. */
#define SAI_HDR_MAGIC_OFF   8u    /* "VFS-0001" / "VFS-0000"              */
#define SAI_HDR_FREE_HEAD   4u    /* word: head of the free block list     */
#define SAI_HDR_TABLE_SPAN  5u    /* word: blocks covered by tables        */
#define SAI_HDR_USED_COUNT  6u    /* word: allocated data blocks           */
#define SAI_HDR_ROOT_OFF    0x40u /* root directory entry (64 bytes)       */

/* Marker SAI stores in the "next" field of each table's own entry. */
#define SAI_TABLE_SELF_NEXT 0x1000u

extern const uint32_t sai_key_user[256];
extern const uint32_t sai_key_system[256];

/* --- crypto.c ------------------------------------------------------------ */
uint32_t sai_checksum_data(const uint32_t *words);   /* all 1024 words      */
uint32_t sai_checksum_table(const uint32_t *words);  /* words 1..1023        */
void sai_decrypt_table(const uint32_t *key, uint32_t index, uint32_t *w);
void sai_encrypt_table(const uint32_t *key, uint32_t index, uint32_t *w);
void sai_decrypt_data(const uint32_t *key, uint32_t seed, uint32_t *w);
void sai_encrypt_data(const uint32_t *key, uint32_t seed, uint32_t *w);

/* --- byte helpers ---------------------------------------------------------- */
static inline uint16_t sai_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t sai_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint64_t sai_rd64(const uint8_t *p)
{
    return (uint64_t)sai_rd32(p) | ((uint64_t)sai_rd32(p + 4) << 32);
}
static inline void sai_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void sai_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline void sai_wr64(uint8_t *p, uint64_t v)
{
    sai_wr32(p, (uint32_t)v);
    sai_wr32(p + 4, (uint32_t)(v >> 32));
}

/* Growable byte buffer used by the serialisers. */
typedef struct sai_buf {
    uint8_t *data;
    size_t   size;
    size_t   cap;
    int      failed;
} sai_buf;

int  sai_buf_reserve(sai_buf *b, size_t extra);
void sai_buf_put(sai_buf *b, const void *data, size_t n);
void sai_buf_u8(sai_buf *b, uint8_t v);
void sai_buf_u16(sai_buf *b, uint16_t v);
void sai_buf_u32(sai_buf *b, uint32_t v);
void sai_buf_free(sai_buf *b);

/* Bounded reader. */
typedef struct sai_reader {
    const uint8_t *p;
    size_t         size;
    size_t         pos;
} sai_reader;

int sai_rd_bytes(sai_reader *r, void *dst, size_t n);
int sai_rd_u8(sai_reader *r, uint8_t *v);
int sai_rd_u16(sai_reader *r, uint16_t *v);
int sai_rd_u32(sai_reader *r, uint32_t *v);
int sai_rd_skip(sai_reader *r, size_t n);

/* Current time as FILETIME. */
uint64_t sai_now_filetime(void);

/* File helpers. */
int sai_read_whole_file(const char *path, uint8_t **data, size_t *size);
int sai_write_whole_file(const char *path, const uint8_t *data, size_t size);

#endif
