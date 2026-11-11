/*
 * The .sai document model on top of the VFS.
 *
 * Recovered from sai.exe 1.2.6-Beta.3:
 *   0x465140  save canvas      0x465be0  load canvas
 *   0x463340  save one layer   0x463c50  load one layer
 *   0x462990  save raster tile (8 RLE planes)   0x462b70  load
 *   0x462ac0  save mask tile   (2 RLE planes)   0x462dd0  load
 *   0x462350  save thumbnail
 * Field tables (size, struct offset) at 0x51df60 (reso), 0x51df7c (lyid),
 * 0x51df88 (stid), 0x51df94 (name), 0x51dfa0 (layer header), 0x51e004
 * (lorg), 0x51e018 (texp), 0x51e02c (peff), 0x51e048 (vmrk).
 */
#include "sai_internal.h"

#include <stdio.h>

#define TAG(a, b, c, d) SAI_FOURCC(a, b, c, d)

#define TAG_RESO TAG('r', 'e', 's', 'o')
#define TAG_LYID TAG('l', 'y', 'i', 'd')
#define TAG_LAYR TAG('l', 'a', 'y', 'r')
#define TAG_WSRC TAG('w', 's', 'r', 'c')
#define TAG_STID TAG('s', 't', 'i', 'd')
#define TAG_LORG TAG('l', 'o', 'r', 'g')
#define TAG_NAME TAG('n', 'a', 'm', 'e')
#define TAG_PFID TAG('p', 'f', 'i', 'd')
#define TAG_PLID TAG('p', 'l', 'i', 'd')
#define TAG_LMFL TAG('l', 'm', 'f', 'l')
#define TAG_FOPN TAG('f', 'o', 'p', 'n')
#define TAG_TEXN TAG('t', 'e', 'x', 'n')
#define TAG_TEXP TAG('t', 'e', 'x', 'p')
#define TAG_PEFF TAG('p', 'e', 'f', 'f')
#define TAG_VMRK TAG('v', 'm', 'r', 'k')

#define CANVAS_VERSION   0x10u
#define LAYER_HEADER_LEN 33u
#define TILE_PIXELS      (SAI_TILE_SIZE * SAI_TILE_SIZE)  /* 1024 */
#define MAX_DIM          0x10000u

static int is_tiled(uint32_t type)
{
    return type == SAI_LAYER_RASTER || type == SAI_LAYER_TYPE4 ||
           type == SAI_LAYER_MASK || type == SAI_LAYER_TYPE7;
}

static size_t channels_of(uint32_t type)
{
    return type == SAI_LAYER_RASTER ? 4 : 1;
}

/* ---- 14-bit <-> 8-bit helpers ------------------------------------------------
 * SAI keeps 14 significant bits per channel and stores them shifted left by
 * two (opaque alpha = 0xFFFC; the two low bits are always zero). */
#define SAI_ONE14 0x3FFFu

static inline uint16_t from8(uint32_t v8)
{
    return (uint16_t)(((v8 * SAI_ONE14 + 127) / 255) << 2);
}

static inline uint8_t to8_14(uint32_t v14)
{
    uint32_t v = (v14 * 255 + SAI_ONE14 / 2) / SAI_ONE14;
    return (uint8_t)(v > 255 ? 255 : v);
}

/* ---- chunk helpers ------------------------------------------------------------ */

static int push_chunk(sai_chunk **arr, size_t *n, uint32_t tag,
                      const uint8_t *data, uint32_t size)
{
    sai_chunk *a = (sai_chunk *)realloc(*arr, (*n + 1) * sizeof(sai_chunk));
    if (!a)
        return SAI_ERR_NOMEM;
    *arr = a;
    a[*n].tag = tag;
    a[*n].size = size;
    a[*n].data = (uint8_t *)malloc(size ? size : 1);
    if (!a[*n].data)
        return SAI_ERR_NOMEM;
    if (size)
        memcpy(a[*n].data, data, size);
    (*n)++;
    return SAI_OK;
}

static void free_chunks(sai_chunk *c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(c[i].data);
    free(c);
}

static void layer_clear(sai_layer *l)
{
    free_chunks(l->extra_chunks, l->extra_chunk_count);
    free(l->pixels);
    free(l->tile_map);
    free(l->body);
    memset(l, 0, sizeof(*l));
}

/* Read up to `want` bytes of a chunk body into dst (zero filling), skipping
 * any remainder; mirrors SAI's tolerant field-table reader. */
static int chunk_fields(sai_reader *r, uint32_t size, uint8_t *dst,
                        size_t want)
{
    size_t take = size < want ? size : want;
    memset(dst, 0, want);
    int e = sai_rd_bytes(r, dst, take);
    if (e)
        return e;
    return sai_rd_skip(r, size - take);
}

/* ---- tiles -------------------------------------------------------------------- */

static int decode_tiles(sai_reader *r, sai_layer *l)
{
    uint32_t tx = l->width / SAI_TILE_SIZE, ty = l->height / SAI_TILE_SIZE;
    size_t ntiles = (size_t)tx * ty;
    size_t ch = channels_of(l->type);
    l->tile_map = (uint8_t *)malloc(ntiles ? ntiles : 1);
    if (!l->tile_map)
        return SAI_ERR_NOMEM;
    int e = sai_rd_bytes(r, l->tile_map, ntiles);
    if (e)
        return e;
    e = sai_layer_alloc_pixels(l);
    if (e)
        return e;

    uint8_t tile[2 * SAI_BLOCK_SIZE]; /* 8192: two 32x32x4 planes */
    for (uint32_t y = 0; y < ty; y++)
        for (uint32_t x = 0; x < tx; x++) {
            if (!l->tile_map[(size_t)y * tx + x])
                continue;
            memset(tile, 0, sizeof(tile));
            unsigned streams = l->type == SAI_LAYER_RASTER ? 8 : 2;
            for (unsigned s = 0; s < streams; s++) {
                uint16_t len;
                if ((e = sai_rd_u16(r, &len)) != SAI_OK)
                    return e;
                if (len > 0x800 || len > r->size - r->pos)
                    return SAI_ERR_FORMAT;
                uint8_t *dst;
                size_t stride;
                if (l->type == SAI_LAYER_RASTER) {
                    dst = tile + (s < 4 ? 0 : SAI_BLOCK_SIZE) + (s & 3);
                    stride = 4;
                } else {
                    dst = tile + s;
                    stride = 2;
                }
                int used = sai_rle_decode(r->p + r->pos, len, dst, TILE_PIXELS,
                                          stride);
                if (used < 0)
                    return used;
                r->pos += len;
            }
            /* scatter into the layer */
            for (uint32_t py = 0; py < SAI_TILE_SIZE; py++) {
                size_t row = ((size_t)y * SAI_TILE_SIZE + py) * l->width +
                             (size_t)x * SAI_TILE_SIZE;
                for (uint32_t px = 0; px < SAI_TILE_SIZE; px++) {
                    size_t ti = (size_t)py * SAI_TILE_SIZE + px;
                    if (ch == 4) {
                        uint16_t *d = l->pixels + (row + px) * 4;
                        for (unsigned c = 0; c < 4; c++)
                            d[c] = (uint16_t)((tile[ti * 4 + c] << 8) |
                                              tile[SAI_BLOCK_SIZE + ti * 4 + c]);
                    } else {
                        l->pixels[row + px] =
                            (uint16_t)(tile[ti * 2] | (tile[ti * 2 + 1] << 8));
                    }
                }
            }
        }
    return SAI_OK;
}

static int tile_nonzero(const sai_layer *l, uint32_t x, uint32_t y)
{
    size_t ch = channels_of(l->type);
    for (uint32_t py = 0; py < SAI_TILE_SIZE; py++) {
        const uint16_t *p = l->pixels +
            (((size_t)y * SAI_TILE_SIZE + py) * l->width + (size_t)x * SAI_TILE_SIZE) * ch;
        for (size_t i = 0; i < SAI_TILE_SIZE * ch; i++)
            if (p[i])
                return 1;
    }
    return 0;
}

static void encode_tiles(sai_buf *b, const sai_layer *l)
{
    uint32_t tx = l->width / SAI_TILE_SIZE, ty = l->height / SAI_TILE_SIZE;
    size_t ntiles = (size_t)tx * ty;
    size_t ch = channels_of(l->type);
    uint8_t *map = (uint8_t *)calloc(ntiles ? ntiles : 1, 1);
    if (!map) {
        b->failed = 1;
        return;
    }
    for (uint32_t y = 0; y < ty; y++)
        for (uint32_t x = 0; x < tx; x++) {
            size_t i = (size_t)y * tx + x;
            map[i] = (l->tile_map && l->tile_map[i]) ||
                     (l->pixels && tile_nonzero(l, x, y));
        }
    sai_buf_put(b, map, ntiles);

    uint8_t tile[2 * SAI_BLOCK_SIZE];
    uint8_t rle[2 * TILE_PIXELS + 16];
    for (uint32_t y = 0; y < ty; y++)
        for (uint32_t x = 0; x < tx; x++) {
            if (!map[(size_t)y * tx + x])
                continue;
            memset(tile, 0, sizeof(tile));
            if (l->pixels)
                for (uint32_t py = 0; py < SAI_TILE_SIZE; py++) {
                    size_t row = ((size_t)y * SAI_TILE_SIZE + py) * l->width +
                                 (size_t)x * SAI_TILE_SIZE;
                    for (uint32_t px = 0; px < SAI_TILE_SIZE; px++) {
                        size_t ti = (size_t)py * SAI_TILE_SIZE + px;
                        if (ch == 4) {
                            const uint16_t *s = l->pixels + (row + px) * 4;
                            for (unsigned c = 0; c < 4; c++) {
                                tile[ti * 4 + c] = (uint8_t)(s[c] >> 8);
                                tile[SAI_BLOCK_SIZE + ti * 4 + c] = (uint8_t)s[c];
                            }
                        } else {
                            uint16_t v = l->pixels[row + px];
                            tile[ti * 2] = (uint8_t)v;
                            tile[ti * 2 + 1] = (uint8_t)(v >> 8);
                        }
                    }
                }
            unsigned streams = l->type == SAI_LAYER_RASTER ? 8 : 2;
            for (unsigned s = 0; s < streams; s++) {
                const uint8_t *src;
                size_t stride;
                if (l->type == SAI_LAYER_RASTER) {
                    src = tile + (s < 4 ? 0 : SAI_BLOCK_SIZE) + (s & 3);
                    stride = 4;
                } else {
                    src = tile + s;
                    stride = 2;
                }
                int n = sai_rle_encode(src, TILE_PIXELS, stride, rle);
                sai_buf_u16(b, (uint16_t)n);
                sai_buf_put(b, rle, (size_t)n);
            }
        }
    free(map);
}

/* ---- layers ------------------------------------------------------------------- */

static int parse_layer(const uint8_t *data, size_t size, sai_layer *l)
{
    sai_reader r = {data, size, 0};
    uint8_t h[LAYER_HEADER_LEN];
    int e;
    memset(l, 0, sizeof(*l));
    if ((e = sai_rd_bytes(&r, h, sizeof(h))) != SAI_OK)
        return e;
    l->type = sai_rd32(h + 0);
    l->id = sai_rd32(h + 4);
    l->x = (int32_t)sai_rd32(h + 8);
    l->y = (int32_t)sai_rd32(h + 12);
    l->width = sai_rd32(h + 16);
    l->height = sai_rd32(h + 20);
    l->unknown76 = sai_rd32(h + 24);
    l->opacity = h[28];
    l->visible = h[29];
    l->preserve_opacity = h[30];
    l->clipping = h[31];
    l->unknown86 = h[32];
    if ((e = sai_rd_u32(&r, &l->blend)) != SAI_OK)
        return e;

    for (;;) {
        uint32_t tag, csize;
        uint8_t f[256];
        if ((e = sai_rd_u32(&r, &tag)) != SAI_OK)
            return e;
        if (tag == 0)
            break;
        if ((e = sai_rd_u32(&r, &csize)) != SAI_OK)
            return e;
        if (csize > r.size - r.pos)
            return SAI_ERR_FORMAT;
        switch (tag) {
        case TAG_LORG:
            e = chunk_fields(&r, csize, f, 8);
            l->has_origin = 1;
            l->origin_x = (int32_t)sai_rd32(f);
            l->origin_y = (int32_t)sai_rd32(f + 4);
            break;
        case TAG_NAME:
            e = chunk_fields(&r, csize, f, 256);
            memcpy(l->name, f, 256);
            l->name[256] = '\0';
            break;
        case TAG_PFID:
            e = chunk_fields(&r, csize, f, 4);
            l->has_parent_set = 1;
            l->parent_set_id = sai_rd32(f);
            break;
        case TAG_PLID:
            e = chunk_fields(&r, csize, f, 4);
            l->has_parent_layer = 1;
            l->parent_layer_id = sai_rd32(f);
            break;
        case TAG_LMFL:
            e = chunk_fields(&r, csize, f, 4);
            l->has_mask_flags = 1;
            l->mask_flags = sai_rd32(f);
            break;
        case TAG_FOPN:
            e = chunk_fields(&r, csize, f, 1);
            l->has_set_open = 1;
            l->set_open = f[0];
            break;
        case TAG_TEXN:
            e = chunk_fields(&r, csize, f, 64);
            l->has_texture_name = 1;
            memcpy(l->texture_name, f, 64);
            l->texture_name[64] = '\0';
            break;
        case TAG_TEXP: /* {2 bytes @92, 1 byte @91} */
            e = chunk_fields(&r, csize, f, 3);
            l->has_texture = 1;
            l->texture_scale = sai_rd16(f);
            l->texture_opacity = f[2];
            break;
        case TAG_PEFF: /* {1 @94, 1 @95, 1 @96} */
            e = chunk_fields(&r, csize, f, 3);
            l->has_effect = 1;
            l->effect_enabled = f[0];
            l->effect_opacity = f[1];
            l->effect_width = f[2];
            break;
        case TAG_VMRK:
            e = chunk_fields(&r, csize, f, 1);
            l->has_vmrk = 1;
            l->vmrk = f[0];
            break;
        default:
            e = push_chunk(&l->extra_chunks, &l->extra_chunk_count, tag,
                           r.p + r.pos, csize);
            if (!e)
                r.pos += csize;
            break;
        }
        if (e)
            return e;
    }

    if (is_tiled(l->type)) {
        if (l->width % SAI_TILE_SIZE || l->height % SAI_TILE_SIZE ||
            l->width > MAX_DIM || l->height > MAX_DIM)
            return SAI_ERR_FORMAT;
        if ((e = decode_tiles(&r, l)) != SAI_OK)
            return e;
    }
    size_t rest = r.size - r.pos;
    if (rest) {
        l->body = (uint8_t *)malloc(rest);
        if (!l->body)
            return SAI_ERR_NOMEM;
        memcpy(l->body, r.p + r.pos, rest);
        l->body_size = rest;
    }
    return SAI_OK;
}

static void put_chunk(sai_buf *b, uint32_t tag, const void *data, uint32_t n)
{
    sai_buf_u32(b, tag);
    sai_buf_u32(b, n);
    sai_buf_put(b, data, n);
}

static int serialize_layer(const sai_layer *l, sai_buf *b)
{
    uint8_t h[LAYER_HEADER_LEN], f[256];
    sai_wr32(h + 0, l->type);
    sai_wr32(h + 4, l->id);
    sai_wr32(h + 8, (uint32_t)l->x);
    sai_wr32(h + 12, (uint32_t)l->y);
    sai_wr32(h + 16, l->width);
    sai_wr32(h + 20, l->height);
    sai_wr32(h + 24, l->unknown76);
    h[28] = l->opacity;
    h[29] = l->visible;
    h[30] = l->preserve_opacity;
    h[31] = l->clipping;
    h[32] = l->unknown86;
    sai_buf_put(b, h, sizeof(h));
    sai_buf_u32(b, l->blend ? l->blend : SAI_BLEND_NORMAL);

    /* Chunk order follows FUN_00463340. */
    sai_wr32(f, (uint32_t)l->origin_x);
    sai_wr32(f + 4, (uint32_t)l->origin_y);
    put_chunk(b, TAG_LORG, f, 8);
    memset(f, 0, 256);
    memcpy(f, l->name, strnlen(l->name, 256));
    put_chunk(b, TAG_NAME, f, 256);
    if (l->has_parent_layer) {
        sai_wr32(f, l->parent_layer_id);
        put_chunk(b, TAG_PLID, f, 4);
        if (l->type == SAI_LAYER_MASK) {
            sai_wr32(f, l->mask_flags & 3u);
            put_chunk(b, TAG_LMFL, f, 4);
        }
    } else if (l->has_parent_set) {
        sai_wr32(f, l->parent_set_id);
        put_chunk(b, TAG_PFID, f, 4);
    }
    if (l->type == SAI_LAYER_SET) {
        f[0] = l->set_open ? 1 : 0;
        put_chunk(b, TAG_FOPN, f, 1);
    }
    if (l->has_texture_name && l->texture_name[0]) {
        memset(f, 0, 64);
        memcpy(f, l->texture_name, strnlen(l->texture_name, 63));
        put_chunk(b, TAG_TEXN, f, 64);
    }
    sai_wr16(f, l->has_texture ? l->texture_scale : 100);
    f[2] = l->has_texture ? l->texture_opacity : 20;
    put_chunk(b, TAG_TEXP, f, 3);
    f[0] = l->has_effect ? l->effect_enabled : 0;
    f[1] = l->has_effect ? l->effect_opacity : 100;
    f[2] = l->has_effect ? l->effect_width : 1;
    put_chunk(b, TAG_PEFF, f, 3);
    f[0] = l->vmrk;
    put_chunk(b, TAG_VMRK, f, 1);
    for (size_t i = 0; i < l->extra_chunk_count; i++)
        put_chunk(b, l->extra_chunks[i].tag, l->extra_chunks[i].data,
                  l->extra_chunks[i].size);
    sai_buf_u32(b, 0);

    if (is_tiled(l->type)) {
        if (l->width % SAI_TILE_SIZE || l->height % SAI_TILE_SIZE)
            return SAI_ERR_ARGUMENT;
        encode_tiles(b, l);
    }
    sai_buf_put(b, l->body, l->body_size);
    return b->failed ? SAI_ERR_NOMEM : SAI_OK;
}

/* ---- canvas -------------------------------------------------------------------- */

static int parse_canvas(const uint8_t *data, size_t size, sai_document *d)
{
    sai_reader r = {data, size, 0};
    uint32_t ver;
    int e;
    if ((e = sai_rd_u32(&r, &ver)) != SAI_OK)
        return e;
    if (ver < CANVAS_VERSION)
        return SAI_ERR_UNSUPPORTED; /* legacy fixed-struct canvas (0,1,2) */
    if ((e = sai_rd_u32(&r, &d->width)) || (e = sai_rd_u32(&r, &d->height)))
        return e;
    if (d->width == 0 || d->height == 0 || d->width > MAX_DIM ||
        d->height > MAX_DIM)
        return SAI_ERR_FORMAT;
    for (;;) {
        uint32_t tag, csize;
        uint8_t f[8];
        if ((e = sai_rd_u32(&r, &tag)) != SAI_OK)
            return e;
        if (tag == 0)
            break;
        if ((e = sai_rd_u32(&r, &csize)) != SAI_OK)
            return e;
        if (csize > r.size - r.pos)
            return SAI_ERR_FORMAT;
        switch (tag) {
        case TAG_RESO: /* {4 @332, 2 @336, 2 @338} */
            e = chunk_fields(&r, csize, f, 8);
            d->has_resolution = 1;
            d->dpi = sai_rd32(f);
            d->resolution_unit = sai_rd16(f + 4);
            d->size_unit = sai_rd16(f + 6);
            break;
        case TAG_LYID:
            e = chunk_fields(&r, csize, f, 4);
            d->has_next_layer_id = 1;
            d->next_layer_id = sai_rd32(f);
            break;
        case TAG_LAYR:
            e = chunk_fields(&r, csize, f, 4);
            d->has_selected_layer = 1;
            d->selected_layer_id = sai_rd32(f);
            break;
        case TAG_WSRC:
            e = chunk_fields(&r, csize, f, 4);
            d->has_wsrc = 1;
            d->wsrc = sai_rd32(f);
            break;
        case TAG_STID:
            e = chunk_fields(&r, csize, f, 4);
            d->has_stid = 1;
            d->stid = sai_rd32(f);
            break;
        default:
            e = push_chunk(&d->extra_chunks, &d->extra_chunk_count, tag,
                           r.p + r.pos, csize);
            if (!e)
                r.pos += csize;
        }
        if (e)
            return e;
    }
    return SAI_OK;
}

static void serialize_canvas(const sai_document *d, sai_buf *b)
{
    uint8_t f[8];
    sai_buf_u32(b, CANVAS_VERSION);
    sai_buf_u32(b, d->width);
    sai_buf_u32(b, d->height);
    sai_wr32(f, d->has_resolution ? d->dpi : 0x00480000u);
    sai_wr16(f + 4, d->resolution_unit);
    sai_wr16(f + 6, d->size_unit);
    put_chunk(b, TAG_RESO, f, 8);
    sai_wr32(f, d->next_layer_id);
    put_chunk(b, TAG_LYID, f, 4);
    sai_wr32(f, d->selected_layer_id);
    put_chunk(b, TAG_LAYR, f, 4);
    if (d->has_wsrc && d->wsrc) {
        sai_wr32(f, d->wsrc);
        put_chunk(b, TAG_WSRC, f, 4);
    }
    if (d->has_stid) {
        sai_wr32(f, d->stid);
        put_chunk(b, TAG_STID, f, 4);
    }
    for (size_t i = 0; i < d->extra_chunk_count; i++)
        put_chunk(b, d->extra_chunks[i].tag, d->extra_chunks[i].data,
                  d->extra_chunks[i].size);
    sai_buf_u32(b, 0);
}

/* ---- loading ------------------------------------------------------------------- */

typedef struct tbl_entry {
    uint32_t id;
    uint16_t type;
    uint16_t count;
} tbl_entry;

static int read_table(sai_vfs *v, const char *path, tbl_entry **out,
                      size_t *n)
{
    uint8_t *data;
    size_t size;
    *out = NULL;
    *n = 0;
    int e = sai_vfs_read_file(v, path, &data, &size);
    if (e)
        return e;
    if (size < 4) {
        free(data);
        return SAI_ERR_FORMAT;
    }
    uint32_t count = sai_rd32(data);
    if (count > (size - 4) / 8 || count > 0x10000) {
        free(data);
        return SAI_ERR_FORMAT;
    }
    tbl_entry *t = (tbl_entry *)calloc(count ? count : 1, sizeof(*t));
    if (!t) {
        free(data);
        return SAI_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < count; i++) {
        t[i].id = sai_rd32(data + 4 + i * 8);
        t[i].type = sai_rd16(data + 8 + i * 8);
        t[i].count = sai_rd16(data + 10 + i * 8);
    }
    free(data);
    *out = t;
    *n = count;
    return SAI_OK;
}

static int load_layers(sai_vfs *v, const char *table, const char *dir,
                       sai_layer **out, size_t *count)
{
    tbl_entry *t;
    size_t n;
    *out = NULL;
    *count = 0;
    int e = read_table(v, table, &t, &n);
    if (e)
        return e;
    sai_layer *layers = (sai_layer *)calloc(n ? n : 1, sizeof(sai_layer));
    if (!layers) {
        free(t);
        return SAI_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        char path[64];
        uint8_t *data;
        size_t size;
        snprintf(path, sizeof(path), "%s/%08x", dir, (unsigned)t[i].id);
        e = sai_vfs_read_file(v, path, &data, &size);
        if (!e) {
            e = parse_layer(data, size, &layers[i]);
            free(data);
        }
        if (e) {
            for (size_t j = 0; j <= i; j++)
                layer_clear(&layers[j]);
            free(layers);
            free(t);
            return e;
        }
    }
    free(t);
    *out = layers;
    *count = n;
    return SAI_OK;
}

static int is_owned_path(const char *p)
{
    static const char *const known[] = {"/canvas", "/laytbl", "/subtbl",
                                        "/thumbnail", "/layers", "/sublayers"};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        size_t n = strlen(known[i]);
        if (strncmp(p, known[i], n) == 0 && (p[n] == '\0' || p[n] == '/'))
            return 1;
    }
    return 0;
}

typedef struct collect_ctx {
    sai_vfs      *vfs;
    sai_document *doc;
    int           err;
} collect_ctx;

static int collect_extra(void *user, const char *path, const sai_entry *e,
                         int depth)
{
    collect_ctx *c = (collect_ctx *)user;
    (void)depth;
    if (e->type != SAI_ENTRY_FILE || is_owned_path(path))
        return 0;
    sai_document *d = c->doc;
    sai_extra_file *a = (sai_extra_file *)realloc(
        d->extra_files, (d->extra_file_count + 1) * sizeof(*a));
    if (!a)
        return c->err = SAI_ERR_NOMEM;
    d->extra_files = a;
    sai_extra_file *x = &a[d->extra_file_count];
    memset(x, 0, sizeof(*x));
    snprintf(x->path, sizeof(x->path), "%s", path);
    x->timestamp = e->timestamp;
    int err = sai_vfs_read_entry(c->vfs, e, &x->data, &x->size);
    if (err)
        return c->err = err;
    d->extra_file_count++;
    return 0;
}

int sai_document_load_vfs(sai_vfs *v, sai_document **out)
{
    if (!v || !out)
        return SAI_ERR_ARGUMENT;
    *out = NULL;
    sai_document *d = (sai_document *)calloc(1, sizeof(*d));
    if (!d)
        return SAI_ERR_NOMEM;

    uint8_t *data;
    size_t size;
    int e = sai_vfs_read_file(v, "/canvas", &data, &size);
    if (!e) {
        e = parse_canvas(data, size, d);
        free(data);
    }
    if (!e)
        e = load_layers(v, "/laytbl", "/layers", &d->layers, &d->layer_count);
    if (!e) {
        sai_entry st;
        if (sai_vfs_stat(v, "/subtbl", &st) == SAI_OK)
            e = load_layers(v, "/subtbl", "/sublayers", &d->sublayers,
                            &d->sublayer_count);
    }
    if (!e && sai_vfs_read_file(v, "/thumbnail", &data, &size) == SAI_OK) {
        if (size >= 12 && memcmp(data + 8, "BM32", 4) == 0) {
            uint32_t tw = sai_rd32(data), th = sai_rd32(data + 4);
            if (tw && th && tw <= MAX_DIM && th <= MAX_DIM &&
                (size - 12) / 4 / tw >= th) {
                d->thumb_bgra = (uint8_t *)malloc((size_t)tw * th * 4);
                if (d->thumb_bgra) {
                    memcpy(d->thumb_bgra, data + 12, (size_t)tw * th * 4);
                    d->thumb_width = tw;
                    d->thumb_height = th;
                }
            }
        }
        free(data);
    }
    if (!e) {
        collect_ctx c = {v, d, 0};
        int rc = sai_vfs_walk(v, collect_extra, &c);
        e = c.err ? c.err : (rc < 0 ? rc : SAI_OK);
    }
    if (e) {
        sai_document_free(d);
        return e;
    }
    *out = d;
    return SAI_OK;
}

int sai_document_load(const char *path, sai_document **out)
{
    sai_vfs *v;
    int e = sai_vfs_open(path, &v);
    if (e)
        return e;
    e = sai_document_load_vfs(v, out);
    sai_vfs_close(v);
    return e;
}

int sai_document_load_memory(const void *data, size_t size,
                             sai_document **out)
{
    sai_vfs *v;
    int e = sai_vfs_open_memory(data, size, NULL, &v);
    if (e)
        return e;
    e = sai_document_load_vfs(v, out);
    sai_vfs_close(v);
    return e;
}

void sai_document_free(sai_document *d)
{
    if (!d)
        return;
    free_chunks(d->extra_chunks, d->extra_chunk_count);
    for (size_t i = 0; i < d->layer_count; i++)
        layer_clear(&d->layers[i]);
    free(d->layers);
    for (size_t i = 0; i < d->sublayer_count; i++)
        layer_clear(&d->sublayers[i]);
    free(d->sublayers);
    free(d->thumb_bgra);
    for (size_t i = 0; i < d->extra_file_count; i++)
        free(d->extra_files[i].data);
    free(d->extra_files);
    free(d);
}

/* ---- creation ------------------------------------------------------------------ */

static uint64_t rand64(void)
{
    static uint64_t s = 0;
    if (!s)
        s = sai_now_filetime() ^ (uint64_t)(uintptr_t)&s ^ 0x9E3779B97F4A7C15ull;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

sai_document *sai_document_new(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || width > MAX_DIM || height > MAX_DIM)
        return NULL;
    sai_document *d = (sai_document *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->width = width;
    d->height = height;
    d->has_resolution = 1;
    d->dpi = 0x00480000u; /* 72.0 */
    d->has_next_layer_id = 1;
    d->next_layer_id = 2; /* first id handed out by SAI */
    d->has_selected_layer = 1;

    /* Work-time record "/#01.<id>" (see sai_sysinfo). */
    uint64_t id = rand64();
    uint64_t now = sai_now_filetime() / 10000000ull;
    sai_extra_file *x = (sai_extra_file *)calloc(1, sizeof(*x));
    uint8_t *rec = (uint8_t *)calloc(1, 32);
    if (!x || !rec) {
        free(x);
        free(rec);
        sai_document_free(d);
        return NULL;
    }
    sai_wr16(rec + 0, 37);
    sai_wr16(rec + 2, 0x8200);
    sai_wr32(rec + 4, 1);
    sai_wr64(rec + 8, now);
    sai_wr64(rec + 16, now);
    sai_wr64(rec + 24, id);
    snprintf(x->path, sizeof(x->path), "/#01.%016llx", (unsigned long long)id);
    x->data = rec;
    x->size = 32;
    d->extra_files = x;
    d->extra_file_count = 1;
    return d;
}

int sai_layer_alloc_pixels(sai_layer *l)
{
    if (!l || !is_tiled(l->type))
        return SAI_ERR_ARGUMENT;
    if (l->pixels)
        return SAI_OK;
    if (l->width > MAX_DIM || l->height > MAX_DIM)
        return SAI_ERR_ARGUMENT;
    size_t n = (size_t)l->width * l->height * channels_of(l->type);
    l->pixels = (uint16_t *)calloc(n ? n : 1, sizeof(uint16_t));
    return l->pixels ? SAI_OK : SAI_ERR_NOMEM;
}

static sai_layer *append_layer(sai_document *d, uint32_t type,
                               const char *name)
{
    sai_layer *a = (sai_layer *)realloc(d->layers,
                                        (d->layer_count + 1) * sizeof(*a));
    if (!a)
        return NULL;
    d->layers = a;
    sai_layer *l = &a[d->layer_count];
    memset(l, 0, sizeof(*l));
    uint32_t maxid = 1;
    for (size_t i = 0; i < d->layer_count; i++)
        if (a[i].id > maxid)
            maxid = a[i].id;
    for (size_t i = 0; i < d->sublayer_count; i++)
        if (d->sublayers[i].id > maxid)
            maxid = d->sublayers[i].id;
    l->id = d->next_layer_id > maxid ? d->next_layer_id : maxid + 1;
    d->next_layer_id = l->id + 1;
    d->has_next_layer_id = 1;
    l->type = type;
    l->opacity = 100;
    l->visible = 1;
    l->blend = type == SAI_LAYER_SET ? SAI_BLEND_PASS_THROUGH : SAI_BLEND_NORMAL;
    if (type == SAI_LAYER_RASTER) {
        uint32_t b = 2 * SAI_LAYER_BORDER + SAI_TILE_SIZE - 1;
        l->width = (d->width + b) & ~(SAI_TILE_SIZE - 1);
        l->height = (d->height + b) & ~(SAI_TILE_SIZE - 1);
    } else {
        l->width = d->width;
        l->height = d->height;
    }
    l->has_texture = 1;
    l->texture_scale = 100;
    l->texture_opacity = 20;
    l->has_effect = 1;
    l->effect_opacity = 100;
    l->effect_width = 1;
    if (name)
        snprintf(l->name, sizeof(l->name), "%s", name);
    if (!d->selected_layer_id)
        d->selected_layer_id = l->id;
    d->layer_count++;
    return l;
}

sai_layer *sai_document_add_layer(sai_document *d, const char *name)
{
    if (!d)
        return NULL;
    sai_layer *l = append_layer(d, SAI_LAYER_RASTER, name);
    if (l && sai_layer_alloc_pixels(l) != SAI_OK) {
        d->layer_count--;
        return NULL;
    }
    return l;
}

sai_layer *sai_document_add_set(sai_document *d, const char *name)
{
    if (!d)
        return NULL;
    sai_layer *l = append_layer(d, SAI_LAYER_SET, name);
    if (l) {
        l->has_set_open = 1;
        l->set_open = 1;
    }
    return l;
}

sai_layer *sai_document_find_layer(sai_document *d, uint32_t id)
{
    if (!d)
        return NULL;
    for (size_t i = 0; i < d->layer_count; i++)
        if (d->layers[i].id == id)
            return &d->layers[i];
    for (size_t i = 0; i < d->sublayer_count; i++)
        if (d->sublayers[i].id == id)
            return &d->sublayers[i];
    return NULL;
}

/* ---- pixels ---------------------------------------------------------------------- */

/* Canvas pixel (cx, cy) lives at layer pixel (cx + 8 - x, cy + 8 - y). */
static int layer_xy(const sai_layer *l, uint32_t cx, uint32_t cy, size_t *idx)
{
    int64_t lx = (int64_t)cx + SAI_LAYER_BORDER - l->x;
    int64_t ly = (int64_t)cy + SAI_LAYER_BORDER - l->y;
    if (lx < 0 || ly < 0 || lx >= (int64_t)l->width || ly >= (int64_t)l->height)
        return 0;
    *idx = (size_t)ly * l->width + (size_t)lx;
    return 1;
}

int sai_layer_get_rgba8(const sai_layer *l, uint32_t cw, uint32_t ch,
                        uint8_t *out)
{
    if (!l || !out || l->type != SAI_LAYER_RASTER)
        return SAI_ERR_ARGUMENT;
    memset(out, 0, (size_t)cw * ch * 4);
    if (!l->pixels)
        return SAI_OK;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++) {
            size_t i;
            if (!layer_xy(l, x, y, &i))
                continue;
            const uint16_t *p = l->pixels + i * 4; /* B G R A, premultiplied */
            uint32_t a = p[3] >> 2;
            uint8_t *o = out + ((size_t)y * cw + x) * 4;
            if (a == 0)
                continue;
            for (int c = 0; c < 3; c++) {
                uint32_t v = ((uint32_t)(p[2 - c] >> 2) * 255 + a / 2) / a;
                o[c] = (uint8_t)(v > 255 ? 255 : v);
            }
            o[3] = to8_14(a);
        }
    return SAI_OK;
}

int sai_layer_set_rgba8(sai_layer *l, uint32_t cw, uint32_t ch,
                        const uint8_t *rgba)
{
    if (!l || !rgba || l->type != SAI_LAYER_RASTER)
        return SAI_ERR_ARGUMENT;
    int e = sai_layer_alloc_pixels(l);
    if (e)
        return e;
    memset(l->pixels, 0, (size_t)l->width * l->height * 4 * sizeof(uint16_t));
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++) {
            size_t i;
            if (!layer_xy(l, x, y, &i))
                continue;
            const uint8_t *s = rgba + ((size_t)y * cw + x) * 4;
            uint16_t *p = l->pixels + i * 4;
            uint32_t a14 = (from8(s[3]) >> 2);
            for (int c = 0; c < 3; c++)
                p[2 - c] = (uint16_t)((((uint32_t)s[c] * a14 + 127) / 255) << 2);
            p[3] = (uint16_t)(a14 << 2);
        }
    if (l->tile_map) {
        /* recomputed from content on save */
        free(l->tile_map);
        l->tile_map = NULL;
    }
    return SAI_OK;
}

/* Effective visibility/opacity through parent sets. */
static int effective(const sai_document *d, const sai_layer *l, float *op)
{
    float o = l->opacity / 100.0f;
    const sai_layer *cur = l;
    for (int guard = 0; guard < 64 && cur->has_parent_set; guard++) {
        const sai_layer *p = NULL;
        for (size_t i = 0; i < d->layer_count; i++)
            if (d->layers[i].id == cur->parent_set_id)
                p = &d->layers[i];
        if (!p)
            break;
        if (!p->visible)
            return 0;
        o *= p->opacity / 100.0f;
        cur = p;
    }
    *op = o;
    return l->visible != 0;
}

int sai_document_flatten_rgba8(const sai_document *d, uint8_t *out)
{
    if (!d || !out)
        return SAI_ERR_ARGUMENT;
    size_t n = (size_t)d->width * d->height;
    float *acc = (float *)calloc(n * 4, sizeof(float)); /* premultiplied RGBA */
    if (!acc)
        return SAI_ERR_NOMEM;
    for (size_t li = 0; li < d->layer_count; li++) {
        const sai_layer *l = &d->layers[li];
        float op;
        if (l->type != SAI_LAYER_RASTER || !l->pixels || !effective(d, l, &op))
            continue;
        for (uint32_t y = 0; y < d->height; y++)
            for (uint32_t x = 0; x < d->width; x++) {
                size_t i;
                if (!layer_xy(l, x, y, &i))
                    continue;
                const uint16_t *p = l->pixels + i * 4;
                float a = (p[3] >> 2) / (float)SAI_ONE14 * op;
                if (a <= 0.0f)
                    continue;
                float *q = acc + ((size_t)y * d->width + x) * 4;
                for (int c = 0; c < 3; c++) {
                    float s = (p[2 - c] >> 2) / (float)SAI_ONE14 * op;
                    q[c] = s + q[c] * (1.0f - a);
                }
                q[3] = a + q[3] * (1.0f - a);
            }
    }
    for (size_t i = 0; i < n; i++) {
        float a = acc[i * 4 + 3];
        for (int c = 0; c < 3; c++) {
            float v = a > 0.0f ? acc[i * 4 + c] / a : 0.0f;
            int iv = (int)(v * 255.0f + 0.5f);
            out[i * 4 + c] = (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
        }
        int ia = (int)(a * 255.0f + 0.5f);
        out[i * 4 + 3] = (uint8_t)(ia < 0 ? 0 : ia > 255 ? 255 : ia);
    }
    free(acc);
    return SAI_OK;
}

/* Thumbnail scale rule from FUN_00462350: the smallest power of two f in
 * {1,2,4,8,16} such that max(w,h)/f < 97, capped at 16. */
static uint32_t thumb_factor(uint32_t w, uint32_t h)
{
    uint32_t m = w > h ? w : h;
    if (m / 16 >= 97) return 16;
    if (m / 8 >= 97) return 8;
    if (m / 4 >= 97) return 4;
    if (m / 2 >= 97) return 2;
    return 1;
}

int sai_document_update_thumbnail(sai_document *d)
{
    if (!d)
        return SAI_ERR_ARGUMENT;
    uint32_t f = thumb_factor(d->width, d->height);
    uint32_t tw = d->width / f, th = d->height / f;
    if (!tw) tw = 1;
    if (!th) th = 1;
    uint8_t *flat = (uint8_t *)malloc((size_t)d->width * d->height * 4);
    uint8_t *t = (uint8_t *)malloc((size_t)tw * th * 4);
    if (!flat || !t) {
        free(flat);
        free(t);
        return SAI_ERR_NOMEM;
    }
    int e = sai_document_flatten_rgba8(d, flat);
    if (e) {
        free(flat);
        free(t);
        return e;
    }
    for (uint32_t y = 0; y < th; y++)
        for (uint32_t x = 0; x < tw; x++) {
            uint32_t sum[3] = {0, 0, 0}, cnt = 0;
            for (uint32_t dy = 0; dy < f; dy++)
                for (uint32_t dx = 0; dx < f; dx++) {
                    uint32_t sx = x * f + dx, sy = y * f + dy;
                    if (sx >= d->width || sy >= d->height)
                        continue;
                    const uint8_t *p = flat + ((size_t)sy * d->width + sx) * 4;
                    for (int c = 0; c < 3; c++) /* over white */
                        sum[c] += (p[c] * p[3] + 255u * (255u - p[3]) + 127) / 255;
                    cnt++;
                }
            uint8_t *o = t + ((size_t)y * tw + x) * 4;
            for (int c = 0; c < 3; c++)
                o[2 - c] = (uint8_t)(cnt ? sum[c] / cnt : 255); /* BGR */
            o[3] = 0xFF;
        }
    free(flat);
    free(d->thumb_bgra);
    d->thumb_bgra = t;
    d->thumb_width = tw;
    d->thumb_height = th;
    return SAI_OK;
}

int sai_document_get_sysinfo(const sai_document *d, sai_sysinfo *out)
{
    if (!d || !out)
        return SAI_ERR_ARGUMENT;
    for (size_t i = 0; i < d->extra_file_count; i++) {
        const sai_extra_file *x = &d->extra_files[i];
        if (strncmp(x->path, "/#", 2) == 0 && x->size >= 32) {
            out->version = sai_rd16(x->data);
            out->flags = sai_rd16(x->data + 2);
            out->sessions = sai_rd32(x->data + 4);
            out->first_time = sai_rd64(x->data + 8);
            out->last_time = sai_rd64(x->data + 16);
            out->id = sai_rd64(x->data + 24);
            return SAI_OK;
        }
    }
    return SAI_ERR_NOT_FOUND;
}

/* ---- saving --------------------------------------------------------------------- */

static int add_buf(sai_writer *w, const char *path, sai_buf *b)
{
    int e = b->failed ? SAI_ERR_NOMEM
                      : sai_writer_add_file(w, path, b->data, b->size, 0);
    sai_buf_free(b);
    return e;
}

static int write_table(sai_writer *w, const char *path, const sai_layer *ls,
                       size_t n, int sub)
{
    sai_buf b = {0};
    sai_buf_u32(&b, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        sai_buf_u32(&b, ls[i].id);
        sai_buf_u16(&b, (uint16_t)ls[i].type);
        /* Progress weight (FUN_00465140): /laytbl stores the tile-row count
         * for raster layers and 1 otherwise; /subtbl always stores rows. */
        uint16_t c = (sub || ls[i].type == SAI_LAYER_RASTER)
                         ? (uint16_t)(ls[i].height / SAI_TILE_SIZE)
                         : 1;
        sai_buf_u16(&b, c);
    }
    return add_buf(w, path, &b);
}

int sai_document_write(const sai_document *d, sai_writer *w)
{
    int e = SAI_OK;
    if (!d || !w)
        return SAI_ERR_ARGUMENT;

    /* Order follows FUN_00465140: sysinfo first, then canvas, tables,
     * folders, layer files and finally the thumbnail. */
    for (size_t i = 0; i < d->extra_file_count && !e; i++)
        e = sai_writer_add_file(w, d->extra_files[i].path,
                                d->extra_files[i].data, d->extra_files[i].size,
                                d->extra_files[i].timestamp);
    if (e)
        return e;

    sai_buf b = {0};
    serialize_canvas(d, &b);
    if ((e = add_buf(w, "/canvas", &b)) != SAI_OK)
        return e;
    if ((e = write_table(w, "/laytbl", d->layers, d->layer_count, 0)) != SAI_OK)
        return e;
    if ((e = sai_writer_mkdir(w, "/layers")) != SAI_OK)
        return e;
    if (d->sublayer_count) {
        if ((e = write_table(w, "/subtbl", d->sublayers, d->sublayer_count, 1)))
            return e;
        if ((e = sai_writer_mkdir(w, "/sublayers")) != SAI_OK)
            return e;
    }
    for (int pass = 0; pass < 2; pass++) {
        const sai_layer *ls = pass ? d->sublayers : d->layers;
        size_t n = pass ? d->sublayer_count : d->layer_count;
        for (size_t i = 0; i < n; i++) {
            char path[64];
            snprintf(path, sizeof(path), "%s/%08x",
                     pass ? "/sublayers" : "/layers", (unsigned)ls[i].id);
            sai_buf lb = {0};
            e = serialize_layer(&ls[i], &lb);
            if (e) {
                sai_buf_free(&lb);
                return e;
            }
            if ((e = add_buf(w, path, &lb)) != SAI_OK)
                return e;
        }
    }

    /* Thumbnail: width, height, "BM32", BGRA rows. */
    sai_document *tmp = NULL;
    const sai_document *src = d;
    if (!d->thumb_bgra) {
        /* generate on a shallow copy so the caller's document is untouched */
        tmp = (sai_document *)malloc(sizeof(*tmp));
        if (!tmp)
            return SAI_ERR_NOMEM;
        *tmp = *d;
        tmp->thumb_bgra = NULL;
        if ((e = sai_document_update_thumbnail(tmp)) != SAI_OK) {
            free(tmp);
            return e;
        }
        src = tmp;
    }
    sai_buf tb = {0};
    sai_buf_u32(&tb, src->thumb_width);
    sai_buf_u32(&tb, src->thumb_height);
    sai_buf_put(&tb, "BM32", 4);
    sai_buf_put(&tb, src->thumb_bgra,
                (size_t)src->thumb_width * src->thumb_height * 4);
    if (tmp) {
        free(tmp->thumb_bgra);
        free(tmp);
    }
    return add_buf(w, "/thumbnail", &tb);
}

int sai_document_save_memory(const sai_document *d, uint8_t **out,
                             size_t *size)
{
    sai_writer *w = sai_writer_new();
    if (!w)
        return SAI_ERR_NOMEM;
    int e = sai_document_write(d, w);
    if (!e)
        e = sai_writer_save_memory(w, out, size);
    sai_writer_free(w);
    return e;
}

int sai_document_save(const sai_document *d, const char *path)
{
    uint8_t *data;
    size_t size;
    if (!path)
        return SAI_ERR_ARGUMENT;
    int e = sai_document_save_memory(d, &data, &size);
    if (e)
        return e;
    e = sai_write_whole_file(path, data, size);
    free(data);
    return e;
}
