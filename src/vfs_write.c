/*
 * Writing the SAI virtual file system.
 *
 * The writer builds a complete image in memory, laid out the way a freshly
 * created file from sai.exe is (FUN_004c0f80 / FUN_004be760 / FUN_004be830):
 *
 *   block 0        table for blocks 0..511
 *   block 1        header ("VFS-0001", free list head, root entry)
 *   block 2..      data, allocated sequentially, skipping table blocks
 *
 * Free table entries keep SAI's initial free list (entry i -> i+1, entry 511
 * -> next table's first data block) so that SAI can keep allocating from
 * the file if it ever edits it in place.
 */
#include "sai_internal.h"

typedef struct wnode {
    char           name[SAI_NAME_MAX + 1];
    int            is_dir;
    uint8_t       *data;
    size_t         size;
    uint64_t       time;
    struct wnode **kids;
    size_t         nkids, capkids;
    /* layout */
    uint32_t       first_block;
    uint32_t       nblocks;
} wnode;

struct sai_writer {
    wnode           root;
    const uint32_t *key;
    uint64_t        default_time;
};

static void node_free(wnode *n)
{
    for (size_t i = 0; i < n->nkids; i++) {
        node_free(n->kids[i]);
        free(n->kids[i]);
    }
    free(n->kids);
    free(n->data);
}

sai_writer *sai_writer_new(void)
{
    sai_writer *w = (sai_writer *)calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->root.is_dir = 1;
    w->key = sai_key_user;
    return w;
}

void sai_writer_free(sai_writer *w)
{
    if (!w)
        return;
    node_free(&w->root);
    free(w);
}

void sai_writer_set_key(sai_writer *w, const uint32_t *key)
{
    if (w)
        w->key = key ? key : sai_key_user;
}

void sai_writer_set_default_time(sai_writer *w, uint64_t t)
{
    if (w)
        w->default_time = t;
}

static uint64_t pick_time(sai_writer *w, uint64_t t)
{
    if (t)
        return t;
    if (!w->default_time)
        w->default_time = sai_now_filetime();
    return w->default_time;
}

static int ieq(const char *a, const char *b, size_t blen)
{
    size_t i;
    for (i = 0; i < blen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb || ca == '\0')
            return 0;
    }
    return a[blen] == '\0';
}

static wnode *child(wnode *dir, const char *name, size_t len)
{
    for (size_t i = 0; i < dir->nkids; i++)
        if (ieq(dir->kids[i]->name, name, len))
            return dir->kids[i];
    return NULL;
}

static int add_child(wnode *dir, const char *name, size_t len, int is_dir,
                     uint64_t t, wnode **out)
{
    if (len == 0 || len > SAI_NAME_MAX)
        return SAI_ERR_ARGUMENT;
    if (dir->nkids == dir->capkids) {
        size_t cap = dir->capkids ? dir->capkids * 2 : 8;
        wnode **k = (wnode **)realloc(dir->kids, cap * sizeof(*k));
        if (!k)
            return SAI_ERR_NOMEM;
        dir->kids = k;
        dir->capkids = cap;
    }
    wnode *n = (wnode *)calloc(1, sizeof(*n));
    if (!n)
        return SAI_ERR_NOMEM;
    memcpy(n->name, name, len);
    n->is_dir = is_dir;
    n->time = t;
    dir->kids[dir->nkids++] = n;
    *out = n;
    return SAI_OK;
}

/* Walk/create folders for `path`.  If `leaf` is non-NULL the last component
 * is returned in leaf/leaf_len and not created. */
static int resolve(sai_writer *w, const char *path, wnode **dir_out,
                   const char **leaf, size_t *leaf_len)
{
    wnode *cur = &w->root;
    const char *p = path;
    for (;;) {
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p) {
            if (leaf)
                return SAI_ERR_ARGUMENT;
            break;
        }
        const char *s = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        size_t len = (size_t)(p - s);
        const char *rest = p;
        while (*rest == '/' || *rest == '\\')
            rest++;
        if (leaf && !*rest) {
            *leaf = s;
            *leaf_len = len;
            break;
        }
        wnode *n = child(cur, s, len);
        if (!n) {
            int err = add_child(cur, s, len, 1, pick_time(w, 0), &n);
            if (err)
                return err;
        } else if (!n->is_dir) {
            return SAI_ERR_EXISTS;
        }
        cur = n;
    }
    *dir_out = cur;
    return SAI_OK;
}

int sai_writer_mkdir(sai_writer *w, const char *path)
{
    wnode *d;
    if (!w || !path)
        return SAI_ERR_ARGUMENT;
    return resolve(w, path, &d, NULL, NULL);
}

int sai_writer_add_file(sai_writer *w, const char *path, const void *data,
                        size_t size, uint64_t filetime)
{
    wnode *dir, *n;
    const char *leaf;
    size_t len;
    if (!w || !path || (size && !data) || size > 0xFFFFFFFFu)
        return SAI_ERR_ARGUMENT;
    int err = resolve(w, path, &dir, &leaf, &len);
    if (err)
        return err;
    uint8_t *copy = (uint8_t *)malloc(size ? size : 1);
    if (!copy)
        return SAI_ERR_NOMEM;
    if (size)
        memcpy(copy, data, size);
    n = child(dir, leaf, len);
    if (n) {
        if (n->is_dir) {
            free(copy);
            return SAI_ERR_EXISTS;
        }
        free(n->data);
    } else {
        err = add_child(dir, leaf, len, 0, 0, &n);
        if (err) {
            free(copy);
            return err;
        }
    }
    n->data = copy;
    n->size = size;
    n->time = pick_time(w, filetime);
    return SAI_OK;
}

/* ---- layout ----------------------------------------------------------------- */

typedef struct layout {
    uint32_t next;     /* next block to hand out */
    uint32_t used;     /* allocated data blocks (excludes header/tables) */
} layout;

static uint32_t alloc_block(layout *L)
{
    if (L->next % SAI_BLOCKS_PER_TABLE == 0)
        L->next++;
    L->used++;
    return L->next++;
}

static uint32_t node_bytes(const wnode *n)
{
    return n->is_dir ? (uint32_t)(n->nkids * SAI_ENTRY_SIZE) : (uint32_t)n->size;
}

static int assign(wnode *n, layout *L)
{
    uint32_t bytes = node_bytes(n);
    n->nblocks = (bytes + SAI_BLOCK_SIZE - 1) / SAI_BLOCK_SIZE;
    n->first_block = 0;
    if (n->nblocks) {
        n->first_block = L->next % SAI_BLOCKS_PER_TABLE == 0 ? L->next + 1
                                                             : L->next;
        /* Blocks of one file are handed out consecutively (skipping tables). */
        for (uint32_t i = 0; i < n->nblocks; i++) {
            alloc_block(L);
            if (L->next >= 0x7FFFFFF0u)
                return SAI_ERR_ARGUMENT;
        }
    }
    if (n->is_dir)
        for (size_t i = 0; i < n->nkids; i++) {
            int err = assign(n->kids[i], L);
            if (err)
                return err;
        }
    return SAI_OK;
}

static uint32_t step(uint32_t b)
{
    b++;
    if (b % SAI_BLOCKS_PER_TABLE == 0)
        b++;
    return b;
}

static void put_entry(uint8_t *p, uint32_t flags, const char *name,
                      uint8_t type, uint32_t first, uint32_t size, uint64_t t)
{
    memset(p, 0, SAI_ENTRY_SIZE);
    sai_wr32(p, flags);
    if (name)
        memcpy(p + 4, name, strnlen(name, SAI_NAME_MAX));
    p[0x26] = type;
    sai_wr32(p + 0x28, first);
    sai_wr32(p + 0x2C, size);
    sai_wr64(p + 0x30, t);
}

typedef struct image {
    uint8_t  *plain;     /* nblocks * 4096 plaintext */
    uint32_t *next;      /* chain links per block */
    uint8_t  *used;      /* 1 when the block holds data */
    uint32_t  nblocks;
} image;

static void emit(image *im, wnode *n)
{
    uint32_t bytes = node_bytes(n);
    const uint8_t *src = n->data;
    uint8_t *dirbuf = NULL;
    if (n->is_dir && bytes) {
        dirbuf = (uint8_t *)calloc(1, bytes);
        if (dirbuf)
            for (size_t i = 0; i < n->nkids; i++) {
                wnode *k = n->kids[i];
                put_entry(dirbuf + i * SAI_ENTRY_SIZE, SAI_ENTRY_USED, k->name,
                          k->is_dir ? SAI_ENTRY_FOLDER : SAI_ENTRY_FILE,
                          k->first_block, node_bytes(k), k->time);
            }
        src = dirbuf;
    }
    uint32_t b = n->first_block;
    for (uint32_t i = 0; i < n->nblocks && src; i++) {
        size_t off = (size_t)i * SAI_BLOCK_SIZE;
        size_t len = bytes - off < SAI_BLOCK_SIZE ? bytes - off : SAI_BLOCK_SIZE;
        memcpy(im->plain + (size_t)b * SAI_BLOCK_SIZE, src + off, len);
        im->used[b] = 1;
        uint32_t nb = i + 1 < n->nblocks ? step(b) : 0;
        im->next[b] = nb;
        b = nb;
    }
    free(dirbuf);
    if (n->is_dir)
        for (size_t i = 0; i < n->nkids; i++)
            emit(im, n->kids[i]);
}

static uint32_t rnd32(uint32_t *state)
{
    /* xorshift32; the header's first two words are random in SAI too. */
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int sai_writer_save_memory(sai_writer *w, uint8_t **out, size_t *out_size)
{
    if (!w || !out || !out_size)
        return SAI_ERR_ARGUMENT;
    *out = NULL;
    *out_size = 0;

    layout L = {2, 0};
    int err = assign(&w->root, &L);
    if (err)
        return err;
    uint32_t last = L.next - 1;            /* highest allocated block */
    if (last < 1)
        last = 1;
    uint32_t nblocks = last + 1;
    uint32_t ntables = (nblocks + SAI_BLOCKS_PER_TABLE - 1) / SAI_BLOCKS_PER_TABLE;

    image im;
    im.nblocks = nblocks;
    im.plain = (uint8_t *)calloc(nblocks, SAI_BLOCK_SIZE);
    im.next = (uint32_t *)calloc(nblocks, sizeof(uint32_t));
    im.used = (uint8_t *)calloc(nblocks, 1);
    if (!im.plain || !im.next || !im.used) {
        free(im.plain);
        free(im.next);
        free(im.used);
        return SAI_ERR_NOMEM;
    }
    emit(&im, &w->root);

    /* Header block (block 1). */
    uint64_t t = pick_time(w, 0);
    uint32_t seed = (uint32_t)(t ^ (t >> 32)) | 1u;
    uint8_t *hdr = im.plain + SAI_BLOCK_SIZE;
    sai_wr32(hdr + 0, rnd32(&seed));
    sai_wr32(hdr + 4, rnd32(&seed));
    memcpy(hdr + SAI_HDR_MAGIC_OFF, "VFS-0001", 8);
    sai_wr32(hdr + SAI_HDR_FREE_HEAD * 4, step(last));
    sai_wr32(hdr + SAI_HDR_TABLE_SPAN * 4, ntables * SAI_BLOCKS_PER_TABLE);
    sai_wr32(hdr + SAI_HDR_USED_COUNT * 4, L.used);
    put_entry(hdr + SAI_HDR_ROOT_OFF, 0, NULL, 0, w->root.first_block,
              node_bytes(&w->root), w->root.time ? w->root.time : t);
    im.used[1] = 1;

    uint8_t *img = (uint8_t *)malloc((size_t)nblocks * SAI_BLOCK_SIZE);
    uint32_t *words = (uint32_t *)malloc(SAI_BLOCK_SIZE);
    uint32_t *table = (uint32_t *)malloc(SAI_BLOCK_SIZE);
    if (!img || !words || !table) {
        free(img);
        free(words);
        free(table);
        free(im.plain);
        free(im.next);
        free(im.used);
        return SAI_ERR_NOMEM;
    }

    for (uint32_t ti = 0; ti < ntables; ti++) {
        uint32_t base = ti * SAI_BLOCKS_PER_TABLE;
        /* Initial free list exactly as FUN_004be760 builds it. */
        for (uint32_t e = 0; e < SAI_BLOCKS_PER_TABLE; e++) {
            table[e * 2] = 0;
            table[e * 2 + 1] = base + 1 + e;
        }
        table[1] = SAI_TABLE_SELF_NEXT;
        table[(SAI_BLOCKS_PER_TABLE - 1) * 2 + 1] = base + SAI_BLOCKS_PER_TABLE + 1;

        for (uint32_t e = 1; e < SAI_BLOCKS_PER_TABLE; e++) {
            uint32_t b = base + e;
            if (b >= nblocks || !im.used[b])
                continue;
            uint8_t *p = im.plain + (size_t)b * SAI_BLOCK_SIZE;
            for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++)
                words[i] = sai_rd32(p + i * 4);
            uint32_t sum = sai_checksum_data(words);
            table[e * 2] = sum;
            if (b != 1) /* header keeps its initial free-list link, like SAI */
                table[e * 2 + 1] = im.next[b];
            sai_encrypt_data(w->key, sum, words);
            for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++)
                sai_wr32(img + (size_t)b * SAI_BLOCK_SIZE + i * 4, words[i]);
        }
        /* Free blocks inside the image: random-looking bytes are not needed,
         * SAI never reads a block whose checksum is 0. */
        for (uint32_t e = 1; e < SAI_BLOCKS_PER_TABLE; e++) {
            uint32_t b = base + e;
            if (b < nblocks && !im.used[b])
                memset(img + (size_t)b * SAI_BLOCK_SIZE, 0, SAI_BLOCK_SIZE);
        }
        table[0] = sai_checksum_table(table);
        sai_encrypt_table(w->key, base, table);
        for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++)
            sai_wr32(img + (size_t)base * SAI_BLOCK_SIZE + i * 4, table[i]);
    }

    free(words);
    free(table);
    free(im.plain);
    free(im.next);
    free(im.used);
    *out = img;
    *out_size = (size_t)nblocks * SAI_BLOCK_SIZE;
    return SAI_OK;
}

int sai_writer_save(sai_writer *w, const char *path)
{
    uint8_t *data;
    size_t size;
    if (!path)
        return SAI_ERR_ARGUMENT;
    int err = sai_writer_save_memory(w, &data, &size);
    if (err)
        return err;
    err = sai_write_whole_file(path, data, size);
    free(data);
    return err;
}
