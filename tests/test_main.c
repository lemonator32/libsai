/*
 * libsai test suite.
 *
 *   test_libsai [SAMPLE.sai ...]
 *
 * Without arguments only the self-contained tests run.  Each sample file
 * given on the command line is additionally loaded, re-saved and compared
 * field by field and pixel by pixel.
 */
#include "sai.h"
#include "../src/sai_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)
#define CHECK_OK(expr)                                                       \
    do {                                                                     \
        int rc_ = (expr);                                                    \
        if (rc_ != SAI_OK) {                                                 \
            fprintf(stderr, "%s:%d: %s -> %s\n", __FILE__, __LINE__, #expr,  \
                    sai_strerror(rc_));                                      \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void test_crypto(void)
{
    uint32_t a[1024], b[1024];
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < 1024; i++)
            a[i] = b[i] = rnd();
        uint32_t seed = rnd();
        sai_encrypt_data(sai_key_document(), seed, b);
        CHECK(memcmp(a, b, sizeof a) != 0);
        sai_decrypt_data(sai_key_document(), seed, b);
        CHECK(memcmp(a, b, sizeof a) == 0);
        uint32_t idx = (rnd() & 0xFF) * 512;
        sai_encrypt_table(sai_key_document(), idx, b);
        sai_decrypt_table(sai_key_document(), idx, b);
        CHECK(memcmp(a, b, sizeof a) == 0);
    }
    /* first entry of the document key, from sai.exe VA 0x518cc0 */
    CHECK(sai_key_document()[0] == 0x9913D29Eu);
}

static void test_rle(void)
{
    uint8_t plane[4096], back[4096], enc[4096];
    for (int round = 0; round < 64; round++) {
        size_t stride = (round & 1) ? 4 : 2;
        int mode = round % 4;
        for (size_t i = 0; i < 1024; i++) {
            uint8_t v;
            if (mode == 0) v = 0;
            else if (mode == 1) v = (uint8_t)rnd();
            else if (mode == 2) v = (uint8_t)((i / 7) & 3);
            else v = (uint8_t)((rnd() & 7) == 0 ? rnd() : 0x55);
            plane[i * stride] = v;
        }
        int n = sai_rle_encode(plane, 1024, stride, enc);
        CHECK(n > 0 && n <= 2048);
        memset(back, 0xEE, sizeof back);
        int used = sai_rle_decode(enc, (size_t)n, back, 1024, stride);
        CHECK(used == n);
        for (size_t i = 0; i < 1024; i++)
            if (back[i * stride] != plane[i * stride]) {
                CHECK(!"rle mismatch");
                break;
            }
    }
    /* An all-zero channel compresses to 8 runs of 128 = 16 bytes. */
    memset(plane, 0, sizeof plane);
    CHECK(sai_rle_encode(plane, 1024, 4, enc) == 16);
    /* truncated input is rejected */
    CHECK(sai_rle_decode(enc, 3, back, 1024, 4) < 0);
}

static void test_vfs_roundtrip(void)
{
    sai_writer *w = sai_writer_new();
    CHECK(w != NULL);
    if (!w)
        return;
    sai_writer_set_default_time(w, 0x01D0000000000000ull);
    /* enough data to cross the 512-block table boundary */
    size_t big = 700 * 4096 + 123;
    uint8_t *data = (uint8_t *)malloc(big);
    for (size_t i = 0; i < big; i++)
        data[i] = (uint8_t)(i * 31 + (i >> 12));
    CHECK_OK(sai_writer_add_file(w, "/a", "hello", 5, 0));
    CHECK_OK(sai_writer_add_file(w, "/dir/sub/big", data, big, 0));
    CHECK_OK(sai_writer_add_file(w, "/dir/empty", NULL, 0, 0));
    CHECK_OK(sai_writer_mkdir(w, "/emptydir"));
    for (int i = 0; i < 100; i++) { /* folder bigger than one block */
        char p[64], c[16];
        snprintf(p, sizeof p, "/many/%08x", i);
        snprintf(c, sizeof c, "%d", i);
        CHECK_OK(sai_writer_add_file(w, p, c, strlen(c), 0));
    }
    CHECK(sai_writer_add_file(w, "/a/x", "x", 1, 0) == SAI_ERR_EXISTS);
    CHECK(sai_writer_add_file(w, "/123456789012345678901234567890123", "",
                              0, 0) == SAI_ERR_ARGUMENT);

    uint8_t *img;
    size_t size;
    CHECK_OK(sai_writer_save_memory(w, &img, &size));
    sai_writer_free(w);
    CHECK(size % 4096 == 0 && size > 700 * 4096);

    sai_vfs *v;
    CHECK_OK(sai_vfs_open_memory(img, size, NULL, &v));
    if (v) {
        uint8_t *out;
        size_t n;
        CHECK(sai_vfs_version(v) == 1);
        CHECK_OK(sai_vfs_read_file(v, "/a", &out, &n));
        CHECK(n == 5 && memcmp(out, "hello", 5) == 0);
        sai_free(out);
        CHECK_OK(sai_vfs_read_file(v, "/DIR/Sub/big", &out, &n));
        CHECK(n == big && memcmp(out, data, big) == 0);
        sai_free(out);
        CHECK_OK(sai_vfs_read_file(v, "/dir/empty", &out, &n));
        CHECK(n == 0);
        sai_free(out);
        sai_entry e;
        CHECK_OK(sai_vfs_stat(v, "/emptydir", &e));
        CHECK(e.type == SAI_ENTRY_FOLDER && e.size == 0);
        CHECK(sai_vfs_stat(v, "/nope", &e) == SAI_ERR_NOT_FOUND);
        sai_entry *list;
        size_t cnt;
        CHECK_OK(sai_vfs_list(v, "/many", &list, &cnt));
        CHECK(cnt == 100);
        if (cnt == 100)
            CHECK(strcmp(list[99].name, "00000063") == 0);
        sai_free(list);
        CHECK_OK(sai_vfs_read_file(v, "/many/00000042", &out, &n));
        CHECK(n == 2 && memcmp(out, "66", 2) == 0);
        sai_free(out);
        sai_vfs_close(v);
    }

    /* Corrupt one data byte: the checksum must catch it. */
    img[2 * 4096 + 100] ^= 0x40;
    CHECK_OK(sai_vfs_open_memory(img, size, NULL, &v));
    if (v) {
        uint8_t *out;
        size_t n;
        CHECK(sai_vfs_read_file(v, "/a", &out, &n) == SAI_ERR_CHECKSUM);
        sai_vfs_close(v);
    }
    sai_open_options opt = {NULL, 1};
    CHECK_OK(sai_vfs_open_memory(img, size, &opt, &v));
    sai_vfs_close(v);

    /* Wrong key: table 0 checksum fails. */
    sai_open_options wrong = {sai_key_system_vfs(), 0};
    CHECK(sai_vfs_open_memory(img, size, &wrong, &v) == SAI_ERR_FORMAT);
    free(img);
    free(data);
}

static void test_document_roundtrip(void)
{
    const uint32_t W = 300, H = 200;
    sai_document *d = sai_document_new(W, H);
    CHECK(d != NULL);
    if (!d)
        return;
    sai_layer *set = sai_document_add_set(d, "Folder");
    CHECK(set != NULL);
    uint32_t set_id = set ? set->id : 0;
    sai_layer *l = sai_document_add_layer(d, "Paint");
    CHECK(l != NULL);
    if (!l) {
        sai_document_free(d);
        return;
    }
    CHECK(l->width == 320 && l->height == 224); /* roundup32(w + 16) */
    l->has_parent_set = 1;
    l->parent_set_id = set_id;
    l->opacity = 80;
    l->blend = SAI_BLEND_MULTIPLY;
    uint8_t *rgba = (uint8_t *)malloc((size_t)W * H * 4);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = rgba + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)x;
            p[1] = (uint8_t)y;
            p[2] = (uint8_t)(x ^ y);
            p[3] = (x > 50 && x < 250) ? 255 : (x < 20 ? 0 : 128);
        }
    CHECK_OK(sai_layer_set_rgba8(l, W, H, rgba));
    uint32_t lid = l->id;

    uint8_t *img;
    size_t size;
    CHECK_OK(sai_document_save_memory(d, &img, &size));
    sai_document *r = NULL;
    CHECK_OK(sai_document_load_memory(img, size, &r));
    free(img);
    if (r) {
        CHECK(r->width == W && r->height == H);
        CHECK(r->layer_count == 2);
        CHECK(r->thumb_width == 150 && r->thumb_height == 100);
        sai_layer *rl = sai_document_find_layer(r, lid);
        CHECK(rl != NULL);
        if (rl) {
            CHECK(strcmp(rl->name, "Paint") == 0);
            CHECK(rl->opacity == 80 && rl->blend == SAI_BLEND_MULTIPLY);
            CHECK(rl->has_parent_set && rl->parent_set_id == set_id);
            uint8_t *back = (uint8_t *)malloc((size_t)W * H * 4);
            CHECK_OK(sai_layer_get_rgba8(rl, W, H, back));
            int bad = 0;
            for (size_t i = 0; i < (size_t)W * H; i++) {
                const uint8_t *a = rgba + i * 4, *b = back + i * 4;
                if (a[3] != b[3]) bad++;
                else if (a[3] == 255 && memcmp(a, b, 3) != 0) bad++;
                else if (a[3] == 128)
                    for (int c = 0; c < 3; c++)
                        if (abs((int)a[c] - (int)b[c]) > 2) { bad++; break; }
            }
            CHECK(bad == 0);
            free(back);
        }
        sai_sysinfo si;
        CHECK_OK(sai_document_get_sysinfo(r, &si));
        CHECK(si.version == 37);
        sai_document_free(r);
    }
    free(rgba);
    sai_document_free(d);
}

static int layers_equal(const sai_layer *a, const sai_layer *b)
{
    if (a->type != b->type || a->id != b->id || a->x != b->x || a->y != b->y ||
        a->width != b->width || a->height != b->height ||
        a->opacity != b->opacity || a->visible != b->visible ||
        a->blend != b->blend || strcmp(a->name, b->name) != 0 ||
        a->body_size != b->body_size)
        return 0;
    if (a->body_size && memcmp(a->body, b->body, a->body_size) != 0)
        return 0;
    if ((a->pixels == NULL) != (b->pixels == NULL))
        return 0;
    if (a->pixels) {
        size_t n = (size_t)a->width * a->height *
                   (a->type == SAI_LAYER_RASTER ? 4 : 1);
        if (memcmp(a->pixels, b->pixels, n * 2) != 0)
            return 0;
    }
    return 1;
}

static void test_sample(const char *path)
{
    sai_document *d = NULL, *r = NULL;
    printf("sample %s\n", path);
    CHECK_OK(sai_document_load(path, &d));
    if (!d)
        return;
    printf("  %ux%u, %zu layers, %zu sublayers\n", d->width, d->height,
           d->layer_count, d->sublayer_count);
    uint8_t *img;
    size_t size;
    CHECK_OK(sai_document_save_memory(d, &img, &size));
    CHECK_OK(sai_document_load_memory(img, size, &r));
    if (r) {
        CHECK(r->width == d->width && r->height == d->height);
        CHECK(r->layer_count == d->layer_count);
        CHECK(r->extra_file_count == d->extra_file_count);
        CHECK(r->thumb_width == d->thumb_width);
        if (d->thumb_bgra && r->thumb_bgra)
            CHECK(memcmp(d->thumb_bgra, r->thumb_bgra,
                         (size_t)d->thumb_width * d->thumb_height * 4) == 0);
        for (size_t i = 0; i < d->layer_count && i < r->layer_count; i++)
            CHECK(layers_equal(&d->layers[i], &r->layers[i]));
        for (size_t i = 0; i < d->sublayer_count && i < r->sublayer_count; i++)
            CHECK(layers_equal(&d->sublayers[i], &r->sublayers[i]));
        sai_document_free(r);
    }

    /* The raw layer streams must be byte identical after a resave:
     * libsai's RLE encoder is a port of SAI's. */
    sai_vfs *a, *b;
    if (sai_vfs_open(path, &a) == SAI_OK) {
        if (sai_vfs_open_memory(img, size, NULL, &b) == SAI_OK) {
            const char *files[] = {"/canvas", "/laytbl", "/thumbnail"};
            for (size_t i = 0; i < 3; i++) {
                uint8_t *x, *y;
                size_t nx, ny;
                if (sai_vfs_read_file(a, files[i], &x, &nx) == SAI_OK) {
                    CHECK_OK(sai_vfs_read_file(b, files[i], &y, &ny));
                    CHECK(nx == ny && memcmp(x, y, nx) == 0);
                    sai_free(y);
                    sai_free(x);
                }
            }
            for (size_t i = 0; i < d->layer_count; i++) {
                char p[64];
                uint8_t *x, *y;
                size_t nx, ny;
                snprintf(p, sizeof p, "/layers/%08x", d->layers[i].id);
                CHECK_OK(sai_vfs_read_file(a, p, &x, &nx));
                CHECK_OK(sai_vfs_read_file(b, p, &y, &ny));
                if (nx != ny || memcmp(x, y, nx) != 0) {
                    fprintf(stderr, "  %s differs (%zu vs %zu bytes)\n", p, nx,
                            ny);
                    failures++;
                }
                sai_free(x);
                sai_free(y);
            }
            sai_vfs_close(b);
        }
        sai_vfs_close(a);
    }
    free(img);
    sai_document_free(d);
}

/* Mutate a valid document's internal files and make sure loading never
 * crashes (errors are fine). */
static void test_fuzz(void)
{
    sai_document *d = sai_document_new(96, 64);
    sai_layer *l = d ? sai_document_add_layer(d, "x") : NULL;
    if (!l) {
        CHECK(!"setup");
        sai_document_free(d);
        return;
    }
    for (size_t i = 0; i < (size_t)l->width * l->height * 4; i++)
        l->pixels[i] = (uint16_t)((i % 7) ? 0 : (rnd() & 0xFFFC));
    sai_writer *w0 = sai_writer_new();
    CHECK_OK(sai_document_write(d, w0));
    sai_writer_free(w0);

    uint8_t *img;
    size_t size;
    CHECK_OK(sai_document_save_memory(d, &img, &size));
    sai_vfs *v;
    CHECK_OK(sai_vfs_open_memory(img, size, NULL, &v));
    free(img);
    uint8_t *canvas = NULL, *laytbl = NULL, *layer = NULL;
    size_t nc = 0, nt = 0, nl = 0;
    char lp[64];
    snprintf(lp, sizeof lp, "/layers/%08x", l->id);
    CHECK_OK(sai_vfs_read_file(v, "/canvas", &canvas, &nc));
    CHECK_OK(sai_vfs_read_file(v, "/laytbl", &laytbl, &nt));
    CHECK_OK(sai_vfs_read_file(v, lp, &layer, &nl));
    sai_vfs_close(v);

    int loaded = 0;
    for (int iter = 0; iter < 400; iter++) {
        uint8_t *c2 = (uint8_t *)malloc(nc), *t2 = (uint8_t *)malloc(nt),
                *l2 = (uint8_t *)malloc(nl);
        memcpy(c2, canvas, nc);
        memcpy(t2, laytbl, nt);
        memcpy(l2, layer, nl);
        size_t lc = nc, lt = nt, ll = nl;
        int flips = 1 + (int)(rnd() % 8);
        for (int f = 0; f < flips; f++) {
            switch (rnd() % 4) {
            case 0: c2[rnd() % nc] = (uint8_t)rnd(); break;
            case 1: t2[rnd() % nt] = (uint8_t)rnd(); break;
            case 2: l2[rnd() % nl] = (uint8_t)rnd(); break;
            default: ll = rnd() % nl; break; /* truncate */
            }
        }
        if (iter % 13 == 0) lc = rnd() % nc;
        if (iter % 17 == 0) lt = rnd() % nt;
        sai_writer *w = sai_writer_new();
        sai_writer_add_file(w, "/canvas", c2, lc, 1);
        sai_writer_add_file(w, "/laytbl", t2, lt, 1);
        sai_writer_add_file(w, lp, l2, ll, 1);
        uint8_t *m;
        size_t ms;
        if (sai_writer_save_memory(w, &m, &ms) == SAI_OK) {
            sai_document *r = NULL;
            if (sai_document_load_memory(m, ms, &r) == SAI_OK) {
                loaded++;
                sai_document_free(r);
            }
            free(m);
        }
        sai_writer_free(w);
        free(c2);
        free(t2);
        free(l2);
    }
    printf("fuzz: %d/400 mutated documents still loaded\n", loaded);
    sai_free(canvas);
    sai_free(laytbl);
    sai_free(layer);
    sai_document_free(d);
}

int main(int argc, char **argv)
{
    test_crypto();
    test_rle();
    test_vfs_roundtrip();
    test_document_roundtrip();
    test_fuzz();
    for (int i = 1; i < argc; i++)
        test_sample(argv[i]);
    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
