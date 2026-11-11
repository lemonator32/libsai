/*
 * Tile channel RLE (PackBits variant).
 *
 * Encoder recovered from sai.exe 0x4aefc0 (stride 4, BGRA planes) and
 * 0x4af110 (stride 2, 16-bit single channel planes).  Runs are limited to
 * 128 samples.  Control byte c:
 *   0..127   : copy the next c+1 bytes literally
 *   129..255 : repeat the next byte 257-c times
 *   128      : never produced; treated as a no-op when decoding
 */
#include "sai_internal.h"

int sai_rle_decode(const uint8_t *src, size_t src_len, uint8_t *plane,
                   size_t count, size_t stride)
{
    size_t i = 0, k = 0;
    if (!src || !plane || stride == 0)
        return SAI_ERR_ARGUMENT;
    while (k < count) {
        if (i >= src_len)
            return SAI_ERR_FORMAT;
        uint8_t c = src[i++];
        if (c < 128) {
            size_t n = (size_t)c + 1;
            if (i + n > src_len || k + n > count)
                return SAI_ERR_FORMAT;
            for (size_t j = 0; j < n; j++)
                plane[(k++) * stride] = src[i++];
        } else if (c > 128) {
            size_t n = 257u - c;
            if (i >= src_len || k + n > count)
                return SAI_ERR_FORMAT;
            uint8_t v = src[i++];
            for (size_t j = 0; j < n; j++)
                plane[(k++) * stride] = v;
        }
    }
    return (int)i;
}

/* Byte-for-byte port of the SAI encoder. */
int sai_rle_encode(const uint8_t *plane, size_t count, size_t stride,
                   uint8_t *dst)
{
    size_t pos = 0, out = 0;
    if (!plane || !dst || stride == 0)
        return SAI_ERR_ARGUMENT;
    while (pos < count) {
        uint8_t first = plane[pos * stride];
        size_t limit = pos + 128 < count ? pos + 128 : count;
        size_t run = pos + 1;
        while (run < limit && plane[run * stride] == first)
            run++;
        if (run == pos + 1) {
            /* literal: extend while neighbouring samples differ */
            size_t end = pos + 1;
            uint8_t last = first;
            while (end < limit && plane[end * stride] != last) {
                last = plane[end * stride];
                end++;
            }
            dst[out++] = (uint8_t)(end - pos - 1);
            for (size_t j = pos; j < end; j++)
                dst[out++] = plane[j * stride];
            pos = end;
        } else {
            dst[out++] = (uint8_t)(1 + 256 - (run - pos)); /* 257 - len */
            dst[out++] = first;
            pos = run;
        }
    }
    return (int)out;
}
