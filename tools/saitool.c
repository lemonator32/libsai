/*
 * saitool - command line front end for libsai.
 *
 *   saitool info   FILE.sai                 canvas, layers and sysinfo
 *   saitool ls     FILE.sai                 list the virtual file system
 *   saitool cat    FILE.sai /path OUT       extract one raw VFS file
 *   saitool export FILE.sai PREFIX          write PREFIX-flat.tga,
 *                                           PREFIX-thumb.tga and one TGA per
 *                                           raster layer
 *   saitool resave IN.sai OUT.sai           load and save again
 *   saitool new    OUT.sai W H [IMAGE.tga]  create a document (optionally
 *                                           with one layer from a TGA)
 */
#include "sai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fail(const char *what, int err)
{
    fprintf(stderr, "saitool: %s: %s\n", what, sai_strerror(err));
    return 1;
}

/* ---- tiny TGA (uncompressed 32-bit, top-left origin) ---------------------- */

static int write_tga(const char *path, uint32_t w, uint32_t h,
                     const uint8_t *px, int is_bgra)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return SAI_ERR_IO;
    uint8_t hdr[18] = {0};
    hdr[2] = 2;
    hdr[12] = (uint8_t)w;
    hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)h;
    hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32;
    hdr[17] = 0x28; /* 8 alpha bits, top-left origin */
    fwrite(hdr, 1, 18, f);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t *p = px + i * 4;
        uint8_t q[4];
        if (is_bgra) {
            memcpy(q, p, 4);
        } else {
            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
            q[3] = p[3];
        }
        fwrite(q, 1, 4, f);
    }
    return fclose(f) == 0 ? SAI_OK : SAI_ERR_IO;
}

static uint8_t *read_tga(const char *path, uint32_t *w, uint32_t *h)
{
    FILE *f = fopen(path, "rb");
    uint8_t hdr[18];
    if (!f)
        return NULL;
    if (fread(hdr, 1, 18, f) != 18 || hdr[2] != 2 ||
        (hdr[16] != 32 && hdr[16] != 24)) {
        fclose(f);
        return NULL;
    }
    fseek(f, hdr[0], SEEK_CUR);
    *w = hdr[12] | (hdr[13] << 8);
    *h = hdr[14] | (hdr[15] << 8);
    int bpp = hdr[16] / 8, top = (hdr[17] & 0x20) != 0;
    uint8_t *rgba = (uint8_t *)malloc((size_t)*w * *h * 4);
    if (!rgba) {
        fclose(f);
        return NULL;
    }
    for (uint32_t y = 0; y < *h; y++) {
        uint32_t row = top ? y : *h - 1 - y;
        for (uint32_t x = 0; x < *w; x++) {
            uint8_t q[4] = {0, 0, 0, 255};
            if (fread(q, 1, (size_t)bpp, f) != (size_t)bpp) {
                free(rgba);
                fclose(f);
                return NULL;
            }
            uint8_t *o = rgba + ((size_t)row * *w + x) * 4;
            o[0] = q[2];
            o[1] = q[1];
            o[2] = q[0];
            o[3] = bpp == 4 ? q[3] : 255;
        }
    }
    fclose(f);
    return rgba;
}

/* ---- commands ------------------------------------------------------------- */

static const char *type_name(uint32_t t)
{
    switch (t) {
    case SAI_LAYER_RASTER:   return "raster";
    case SAI_LAYER_LINEWORK: return "linework";
    case SAI_LAYER_MASK:     return "mask";
    case SAI_LAYER_SET:      return "set";
    case SAI_LAYER_TYPE4:    return "type4";
    case SAI_LAYER_TYPE7:    return "type7";
    default:                 return "other";
    }
}

static void print_layer(const sai_layer *l, const char *indent)
{
    printf("%s[%08x] %-8s \"%s\" bounds=(%d,%d %ux%u) opacity=%u%% %s%s%s "
           "blend=%s",
           indent, l->id, type_name(l->type), l->name, l->x, l->y, l->width,
           l->height, l->opacity, l->visible ? "visible" : "hidden",
           l->preserve_opacity ? " preserve-opacity" : "",
           l->clipping ? " clipping" : "", sai_blend_name(l->blend));
    if (l->has_parent_set)
        printf(" parent-set=%08x", l->parent_set_id);
    if (l->has_parent_layer)
        printf(" parent-layer=%08x", l->parent_layer_id);
    if (l->has_texture_name && l->texture_name[0])
        printf(" texture=\"%s\"", l->texture_name);
    if (l->body_size)
        printf(" body=%zu bytes", l->body_size);
    if (l->extra_chunk_count)
        printf(" unknown-chunks=%zu", l->extra_chunk_count);
    printf("\n");
}

static int cmd_info(const char *path)
{
    sai_document *d;
    int e = sai_document_load(path, &d);
    if (e)
        return fail(path, e);
    printf("canvas      %u x %u\n", d->width, d->height);
    if (d->has_resolution)
        printf("resolution  %.2f dpi (unit %u, size unit %u)\n",
               d->dpi / 65536.0, d->resolution_unit, d->size_unit);
    printf("next id     %08x   selected %08x\n", d->next_layer_id,
           d->selected_layer_id);
    printf("thumbnail   %u x %u\n", d->thumb_width, d->thumb_height);
    sai_sysinfo si;
    if (sai_document_get_sysinfo(d, &si) == SAI_OK) {
        /* seconds since 1601 -> unix */
        time_t a = (time_t)(si.first_time - 11644473600ull);
        time_t b = (time_t)(si.last_time - 11644473600ull);
        char sa[32], sb[32];
        strftime(sa, sizeof sa, "%Y-%m-%d %H:%M:%S", gmtime(&a));
        strftime(sb, sizeof sb, "%Y-%m-%d %H:%M:%S", gmtime(&b));
        printf("sysinfo     v%u sessions=%u first=%s last=%s (UTC)\n",
               si.version, si.sessions, sa, sb);
    }
    printf("layers      %zu\n", d->layer_count);
    for (size_t i = 0; i < d->layer_count; i++)
        print_layer(&d->layers[i], "  ");
    if (d->sublayer_count) {
        printf("sublayers   %zu\n", d->sublayer_count);
        for (size_t i = 0; i < d->sublayer_count; i++)
            print_layer(&d->sublayers[i], "  ");
    }
    for (size_t i = 0; i < d->extra_file_count; i++)
        printf("extra file  %s (%zu bytes)\n", d->extra_files[i].path,
               d->extra_files[i].size);
    sai_document_free(d);
    return 0;
}

static int ls_cb(void *u, const char *path, const sai_entry *e, int depth)
{
    (void)u;
    (void)depth;
    printf("%c %10u  blk=%-6u %s\n", e->type == SAI_ENTRY_FOLDER ? 'd' : '-',
           e->size, e->first_block, path);
    return 0;
}

static int cmd_ls(const char *path)
{
    sai_vfs *v;
    int e = sai_vfs_open(path, &v);
    if (e)
        return fail(path, e);
    printf("VFS version %d, %u blocks\n", sai_vfs_version(v),
           sai_vfs_block_count(v));
    e = sai_vfs_walk(v, ls_cb, NULL);
    sai_vfs_close(v);
    return e < 0 ? fail("walk", e) : 0;
}

static int cmd_cat(const char *path, const char *inner, const char *out)
{
    sai_vfs *v;
    uint8_t *data;
    size_t size;
    int e = sai_vfs_open(path, &v);
    if (e)
        return fail(path, e);
    e = sai_vfs_read_file(v, inner, &data, &size);
    sai_vfs_close(v);
    if (e)
        return fail(inner, e);
    FILE *f = fopen(out, "wb");
    if (!f) {
        sai_free(data);
        return fail(out, SAI_ERR_IO);
    }
    fwrite(data, 1, size, f);
    fclose(f);
    sai_free(data);
    return 0;
}

static int cmd_export(const char *path, const char *prefix)
{
    sai_document *d;
    char out[1024];
    int e = sai_document_load(path, &d);
    if (e)
        return fail(path, e);
    uint8_t *buf = (uint8_t *)malloc((size_t)d->width * d->height * 4);
    if (!buf) {
        sai_document_free(d);
        return fail("export", SAI_ERR_NOMEM);
    }
    sai_document_flatten_rgba8(d, buf);
    snprintf(out, sizeof out, "%s-flat.tga", prefix);
    write_tga(out, d->width, d->height, buf, 0);
    printf("wrote %s\n", out);
    for (size_t i = 0; i < d->layer_count; i++) {
        if (d->layers[i].type != SAI_LAYER_RASTER)
            continue;
        sai_layer_get_rgba8(&d->layers[i], d->width, d->height, buf);
        snprintf(out, sizeof out, "%s-layer-%08x.tga", prefix,
                 d->layers[i].id);
        write_tga(out, d->width, d->height, buf, 0);
        printf("wrote %s\n", out);
    }
    if (d->thumb_bgra) {
        snprintf(out, sizeof out, "%s-thumb.tga", prefix);
        write_tga(out, d->thumb_width, d->thumb_height, d->thumb_bgra, 1);
        printf("wrote %s\n", out);
    }
    free(buf);
    sai_document_free(d);
    return 0;
}

static int cmd_resave(const char *in, const char *out)
{
    sai_document *d;
    int e = sai_document_load(in, &d);
    if (e)
        return fail(in, e);
    e = sai_document_save(d, out);
    sai_document_free(d);
    return e ? fail(out, e) : 0;
}

static int cmd_new(const char *out, uint32_t w, uint32_t h, const char *tga)
{
    sai_document *d = sai_document_new(w, h);
    if (!d)
        return fail("new", SAI_ERR_ARGUMENT);
    sai_layer *l = sai_document_add_layer(d, "Layer1");
    if (!l) {
        sai_document_free(d);
        return fail("new", SAI_ERR_NOMEM);
    }
    if (tga) {
        uint32_t tw, th;
        uint8_t *img = read_tga(tga, &tw, &th);
        if (!img) {
            sai_document_free(d);
            return fail(tga, SAI_ERR_FORMAT);
        }
        /* place the image at the top-left of the canvas */
        uint8_t *canvas = (uint8_t *)calloc((size_t)w * h, 4);
        if (canvas) {
            for (uint32_t y = 0; y < h && y < th; y++)
                memcpy(canvas + (size_t)y * w * 4, img + (size_t)y * tw * 4,
                       (size_t)(tw < w ? tw : w) * 4);
            sai_layer_set_rgba8(l, w, h, canvas);
            free(canvas);
        }
        free(img);
    }
    int e = sai_document_save(d, out);
    sai_document_free(d);
    return e ? fail(out, e) : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "info") == 0)
        return cmd_info(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "ls") == 0)
        return cmd_ls(argv[2]);
    if (argc >= 5 && strcmp(argv[1], "cat") == 0)
        return cmd_cat(argv[2], argv[3], argv[4]);
    if (argc >= 4 && strcmp(argv[1], "export") == 0)
        return cmd_export(argv[2], argv[3]);
    if (argc >= 4 && strcmp(argv[1], "resave") == 0)
        return cmd_resave(argv[2], argv[3]);
    if (argc >= 5 && strcmp(argv[1], "new") == 0)
        return cmd_new(argv[2], (uint32_t)strtoul(argv[3], NULL, 10),
                       (uint32_t)strtoul(argv[4], NULL, 10),
                       argc >= 6 ? argv[5] : NULL);
    fprintf(stderr,
            "usage:\n"
            "  saitool info   FILE.sai\n"
            "  saitool ls     FILE.sai\n"
            "  saitool cat    FILE.sai /path OUT\n"
            "  saitool export FILE.sai PREFIX\n"
            "  saitool resave IN.sai OUT.sai\n"
            "  saitool new    OUT.sai WIDTH HEIGHT [IMAGE.tga]\n");
    return 2;
}
