#include "sai_internal.h"

#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

const char *sai_strerror(int err)
{
    switch (err) {
    case SAI_OK:              return "success";
    case SAI_ERR_IO:          return "I/O error";
    case SAI_ERR_NOMEM:       return "out of memory";
    case SAI_ERR_FORMAT:      return "invalid or corrupt data";
    case SAI_ERR_CHECKSUM:    return "block checksum mismatch";
    case SAI_ERR_NOT_FOUND:   return "path not found";
    case SAI_ERR_UNSUPPORTED: return "unsupported format variant";
    case SAI_ERR_ARGUMENT:    return "invalid argument";
    case SAI_ERR_EXISTS:      return "already exists";
    default:                  return "unknown error";
    }
}

void sai_free(void *ptr) { free(ptr); }

const char *sai_blend_name(uint32_t f)
{
    switch (f) {
    case SAI_BLEND_PASS_THROUGH: return "Pass Through";
    case SAI_BLEND_NORMAL:       return "Normal";
    case SAI_BLEND_MULTIPLY:     return "Multiply";
    case SAI_BLEND_SCREEN:       return "Screen";
    case SAI_BLEND_OVERLAY:      return "Overlay";
    case SAI_BLEND_LUMINOSITY:   return "Luminosity";
    case SAI_BLEND_SHADE:        return "Shade";
    case SAI_BLEND_LUMI_SHADE:   return "Lumi & Shade";
    case SAI_BLEND_BINARY_COLOR: return "Binary Color";
    default:                     return "Unknown";
    }
}

/* ---- sai_buf -------------------------------------------------------------- */

int sai_buf_reserve(sai_buf *b, size_t extra)
{
    if (b->failed)
        return SAI_ERR_NOMEM;
    if (extra > SIZE_MAX - b->size) {
        b->failed = 1;
        return SAI_ERR_NOMEM;
    }
    size_t need = b->size + extra;
    if (need <= b->cap)
        return SAI_OK;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap = cap > SIZE_MAX / 2 ? need : cap * 2;
    uint8_t *p = (uint8_t *)realloc(b->data, cap);
    if (!p) {
        b->failed = 1;
        return SAI_ERR_NOMEM;
    }
    b->data = p;
    b->cap = cap;
    return SAI_OK;
}

void sai_buf_put(sai_buf *b, const void *data, size_t n)
{
    if (n == 0 || sai_buf_reserve(b, n) != SAI_OK)
        return;
    memcpy(b->data + b->size, data, n);
    b->size += n;
}

void sai_buf_u8(sai_buf *b, uint8_t v) { sai_buf_put(b, &v, 1); }

void sai_buf_u16(sai_buf *b, uint16_t v)
{
    uint8_t t[2];
    sai_wr16(t, v);
    sai_buf_put(b, t, 2);
}

void sai_buf_u32(sai_buf *b, uint32_t v)
{
    uint8_t t[4];
    sai_wr32(t, v);
    sai_buf_put(b, t, 4);
}

void sai_buf_free(sai_buf *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

/* ---- sai_reader ----------------------------------------------------------- */

int sai_rd_bytes(sai_reader *r, void *dst, size_t n)
{
    if (n > r->size - r->pos)
        return SAI_ERR_FORMAT;
    if (dst)
        memcpy(dst, r->p + r->pos, n);
    r->pos += n;
    return SAI_OK;
}

int sai_rd_skip(sai_reader *r, size_t n) { return sai_rd_bytes(r, NULL, n); }

int sai_rd_u8(sai_reader *r, uint8_t *v) { return sai_rd_bytes(r, v, 1); }

int sai_rd_u16(sai_reader *r, uint16_t *v)
{
    uint8_t t[2];
    int e = sai_rd_bytes(r, t, 2);
    if (e == SAI_OK)
        *v = sai_rd16(t);
    return e;
}

int sai_rd_u32(sai_reader *r, uint32_t *v)
{
    uint8_t t[4];
    int e = sai_rd_bytes(r, t, 4);
    if (e == SAI_OK)
        *v = sai_rd32(t);
    return e;
}

/* ---- time / files ----------------------------------------------------------- */

uint64_t sai_now_filetime(void)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
#else
    /* 11644473600 seconds between 1601-01-01 and 1970-01-01 */
    return ((uint64_t)time(NULL) + 11644473600ull) * 10000000ull;
#endif
}

static FILE *sai_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    /* Accept UTF-8 paths on Windows. */
    wchar_t wpath[1024], wmode[8];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) > 0 &&
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8) > 0) {
        FILE *f = _wfopen(wpath, wmode);
        if (f)
            return f;
    }
#endif
    return fopen(path, mode);
}

int sai_read_whole_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *f = sai_fopen(path, "rb");
    if (!f)
        return SAI_ERR_IO;
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            size_t ncap = cap ? cap * 2 : 1u << 20;
            uint8_t *p = (uint8_t *)realloc(buf, ncap);
            if (!p) {
                free(buf);
                fclose(f);
                return SAI_ERR_NOMEM;
            }
            buf = p;
            cap = ncap;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) {
            if (ferror(f)) {
                free(buf);
                fclose(f);
                return SAI_ERR_IO;
            }
            break;
        }
    }
    fclose(f);
    *data = buf;
    *size = len;
    return SAI_OK;
}

int sai_write_whole_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = sai_fopen(path, "wb");
    if (!f)
        return SAI_ERR_IO;
    size_t n = size ? fwrite(data, 1, size, f) : 0;
    int bad = (n != size);
    if (fclose(f) != 0)
        bad = 1;
    return bad ? SAI_ERR_IO : SAI_OK;
}
