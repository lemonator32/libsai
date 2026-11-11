/*
 * Reading the SAI virtual file system.
 *
 * Layout (see docs/file-format.md):
 *   - The file is a sequence of 4096 byte blocks.
 *   - Block N*512 is a table block for blocks [N*512, N*512+512): 512
 *     entries of {u32 checksum, u32 next}.  Encrypted with seed = index.
 *   - Every other block is a data block, encrypted with seed = its checksum.
 *     A checksum of 0 marks a free block.
 *   - Files are chains of data blocks linked through table "next" fields.
 *   - Block 1 is the header: "VFS-0001" at +8, root directory entry at +0x40.
 *   - Folders are files containing 64 byte entries.
 */
#include "sai_internal.h"

#include <stdio.h>

struct sai_vfs {
    uint8_t        *data;       /* raw (encrypted) image */
    size_t          size;
    uint32_t        nblocks;    /* blocks present in the image */
    const uint32_t *key;
    int             ignore_checksums;
    int             version;    /* 1, 0 or -1 (no header) */
    sai_entry       root;
    /* decrypted table cache, indexed by table number */
    uint32_t      **tables;
    uint32_t        ntables;
};

/* Raw block words; blocks beyond EOF read as zeros (SAI does the same). */
static void raw_block(const sai_vfs *v, uint32_t index, uint32_t *w)
{
    size_t off = (size_t)index * SAI_BLOCK_SIZE;
    if (index < v->nblocks && off + SAI_BLOCK_SIZE <= v->size) {
        const uint8_t *p = v->data + off;
        for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++)
            w[i] = sai_rd32(p + i * 4);
    } else {
        memset(w, 0, SAI_BLOCK_SIZE);
    }
}

static const uint32_t *get_table(sai_vfs *v, uint32_t index, int *err)
{
    uint32_t t = index / SAI_BLOCKS_PER_TABLE;
    if (t >= v->ntables) {
        *err = SAI_ERR_FORMAT;
        return NULL;
    }
    if (!v->tables[t]) {
        uint32_t *w = (uint32_t *)malloc(SAI_BLOCK_SIZE);
        if (!w) {
            *err = SAI_ERR_NOMEM;
            return NULL;
        }
        uint32_t tindex = t * SAI_BLOCKS_PER_TABLE;
        raw_block(v, tindex, w);
        /* Legacy "VFS-0000" containers only encrypted tables whose index is
         * a multiple of 4096 blocks (sai.exe 0x4bdb00). */
        if (v->version != 0 || (tindex & 0xFFFu) == 0)
            sai_decrypt_table(v->key, tindex, w);
        v->tables[t] = w;
    }
    return v->tables[t];
}

static int read_data_block(sai_vfs *v, uint32_t index, uint32_t *w,
                           uint32_t *next)
{
    int err = SAI_OK;
    if (index % SAI_BLOCKS_PER_TABLE == 0)
        return SAI_ERR_FORMAT; /* a table block can never hold data */
    const uint32_t *t = get_table(v, index, &err);
    if (!t)
        return err;
    uint32_t e = index % SAI_BLOCKS_PER_TABLE;
    uint32_t checksum = t[e * 2];
    if (next)
        *next = t[e * 2 + 1];
    if (checksum == 0)
        return SAI_ERR_FORMAT; /* free block */
    raw_block(v, index, w);
    sai_decrypt_data(v->key, checksum, w);
    if (!v->ignore_checksums && sai_checksum_data(w) != checksum)
        return SAI_ERR_CHECKSUM;
    return SAI_OK;
}

int sai_vfs_read_block(sai_vfs *v, uint32_t index, uint8_t *buf)
{
    uint32_t w[SAI_WORDS_PER_BLOCK];
    int err = SAI_OK;
    if (!v || !buf)
        return SAI_ERR_ARGUMENT;
    if (index % SAI_BLOCKS_PER_TABLE == 0) {
        const uint32_t *t = get_table(v, index, &err);
        if (!t)
            return err;
        memcpy(w, t, SAI_BLOCK_SIZE);
    } else {
        err = read_data_block(v, index, w, NULL);
        if (err)
            return err;
    }
    for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++)
        sai_wr32(buf + i * 4, w[i]);
    return SAI_OK;
}

static void parse_entry(const uint8_t *p, sai_entry *e)
{
    memset(e, 0, sizeof(*e));
    e->flags = sai_rd32(p);
    memcpy(e->name, p + 4, SAI_NAME_MAX);
    e->name[SAI_NAME_MAX] = '\0';
    e->type = p[0x26];
    e->first_block = sai_rd32(p + 0x28);
    e->size = sai_rd32(p + 0x2C);
    e->timestamp = sai_rd64(p + 0x30);
    e->reserved = sai_rd64(p + 0x38);
}

int sai_vfs_read_entry(sai_vfs *v, const sai_entry *entry, uint8_t **data,
                       size_t *size)
{
    if (!v || !entry || !data || !size)
        return SAI_ERR_ARGUMENT;
    *data = NULL;
    *size = 0;
    size_t total = entry->size;
    /* A file cannot be larger than the blocks backing it. */
    if (total > (size_t)v->ntables * SAI_BLOCKS_PER_TABLE * SAI_BLOCK_SIZE)
        return SAI_ERR_FORMAT;
    uint8_t *out = (uint8_t *)malloc(total ? total : 1);
    if (!out)
        return SAI_ERR_NOMEM;
    uint32_t w[SAI_WORDS_PER_BLOCK];
    uint8_t tmp[SAI_BLOCK_SIZE];
    uint32_t block = entry->first_block;
    size_t done = 0, hops = 0;
    size_t max_hops = (size_t)v->ntables * SAI_BLOCKS_PER_TABLE;
    while (done < total) {
        uint32_t next = 0;
        if (block == 0 || ++hops > max_hops) {
            free(out);
            return SAI_ERR_FORMAT;
        }
        int err = read_data_block(v, block, w, &next);
        if (err) {
            free(out);
            return err;
        }
        for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++)
            sai_wr32(tmp + i * 4, w[i]);
        size_t n = total - done < SAI_BLOCK_SIZE ? total - done : SAI_BLOCK_SIZE;
        memcpy(out + done, tmp, n);
        done += n;
        block = next;
    }
    *data = out;
    *size = total;
    return SAI_OK;
}

/* Find `name` (case-insensitive, 32 chars like SAI's _strnicmp) in a folder. */
static int name_eq(const char *a, const char *b, size_t blen)
{
    size_t i;
    for (i = 0; i < SAI_NAME_MAX; i++) {
        char ca = a[i];
        char cb = i < blen ? b[i] : '\0';
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        if (ca == '\0')
            return 1;
    }
    return blen <= SAI_NAME_MAX;
}

static int lookup_child(sai_vfs *v, const sai_entry *dir, const char *name,
                        size_t len, sai_entry *out)
{
    uint8_t *buf;
    size_t size;
    if (dir->type != SAI_ENTRY_FOLDER)
        return SAI_ERR_NOT_FOUND;
    int err = sai_vfs_read_entry(v, dir, &buf, &size);
    if (err)
        return err;
    err = SAI_ERR_NOT_FOUND;
    for (size_t o = 0; o + SAI_ENTRY_SIZE <= size; o += SAI_ENTRY_SIZE) {
        sai_entry e;
        parse_entry(buf + o, &e);
        if (e.flags == 0)
            continue;
        if (name_eq(e.name, name, len)) {
            *out = e;
            err = SAI_OK;
            break;
        }
    }
    free(buf);
    return err;
}

int sai_vfs_stat(sai_vfs *v, const char *path, sai_entry *out)
{
    if (!v || !path || !out)
        return SAI_ERR_ARGUMENT;
    sai_entry cur = v->root;
    const char *p = path;
    for (;;) {
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        sai_entry next;
        int err = lookup_child(v, &cur, s, (size_t)(p - s), &next);
        if (err)
            return err;
        cur = next;
    }
    *out = cur;
    return SAI_OK;
}

int sai_vfs_read_file(sai_vfs *v, const char *path, uint8_t **data,
                      size_t *size)
{
    sai_entry e;
    int err = sai_vfs_stat(v, path, &e);
    if (err)
        return err;
    return sai_vfs_read_entry(v, &e, data, size);
}

static int list_entry(sai_vfs *v, const sai_entry *dir, sai_entry **entries,
                      size_t *count)
{
    uint8_t *buf;
    size_t size;
    *entries = NULL;
    *count = 0;
    int err = sai_vfs_read_entry(v, dir, &buf, &size);
    if (err)
        return err;
    size_t n = 0;
    sai_entry *list =
        (sai_entry *)malloc((size / SAI_ENTRY_SIZE + 1) * sizeof(sai_entry));
    if (!list) {
        free(buf);
        return SAI_ERR_NOMEM;
    }
    for (size_t o = 0; o + SAI_ENTRY_SIZE <= size; o += SAI_ENTRY_SIZE) {
        parse_entry(buf + o, &list[n]);
        if (list[n].flags != 0)
            n++;
    }
    free(buf);
    *entries = list;
    *count = n;
    return SAI_OK;
}

int sai_vfs_list(sai_vfs *v, const char *path, sai_entry **entries,
                 size_t *count)
{
    sai_entry dir;
    if (!entries || !count)
        return SAI_ERR_ARGUMENT;
    int err = sai_vfs_stat(v, path, &dir);
    if (err)
        return err;
    if (dir.type != SAI_ENTRY_FOLDER)
        return SAI_ERR_ARGUMENT;
    return list_entry(v, &dir, entries, count);
}

static int walk_rec(sai_vfs *v, const sai_entry *dir, char *path, size_t plen,
                    int depth, sai_walk_fn fn, void *user)
{
    sai_entry *list;
    size_t n;
    if (depth > 64)
        return SAI_ERR_FORMAT;
    int err = list_entry(v, dir, &list, &n);
    if (err)
        return err;
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        size_t nl = strlen(list[i].name);
        if (plen + 1 + nl + 1 > 1024) {
            rc = SAI_ERR_FORMAT;
            break;
        }
        path[plen] = '/';
        memcpy(path + plen + 1, list[i].name, nl + 1);
        rc = fn(user, path, &list[i], depth);
        if (rc == 0 && list[i].type == SAI_ENTRY_FOLDER)
            rc = walk_rec(v, &list[i], path, plen + 1 + nl, depth + 1, fn,
                          user);
        path[plen] = '\0';
    }
    free(list);
    return rc;
}

int sai_vfs_walk(sai_vfs *v, sai_walk_fn fn, void *user)
{
    char path[1024];
    if (!v || !fn)
        return SAI_ERR_ARGUMENT;
    path[0] = '\0';
    return walk_rec(v, &v->root, path, 0, 0, fn, user);
}

int sai_vfs_version(const sai_vfs *v) { return v ? v->version : -1; }
uint32_t sai_vfs_block_count(const sai_vfs *v) { return v ? v->nblocks : 0; }

void sai_vfs_close(sai_vfs *v)
{
    if (!v)
        return;
    if (v->tables) {
        for (uint32_t i = 0; i < v->ntables; i++)
            free(v->tables[i]);
        free(v->tables);
    }
    free(v->data);
    free(v);
}

/* Takes ownership of `data`. */
static int open_owned(uint8_t *data, size_t size, const sai_open_options *opt,
                      sai_vfs **out)
{
    *out = NULL;
    if (size < 2 * SAI_BLOCK_SIZE || size % SAI_BLOCK_SIZE != 0) {
        free(data);
        return SAI_ERR_FORMAT;
    }
    sai_vfs *v = (sai_vfs *)calloc(1, sizeof(*v));
    if (!v) {
        free(data);
        return SAI_ERR_NOMEM;
    }
    v->data = data;
    v->size = size;
    v->nblocks = (uint32_t)(size / SAI_BLOCK_SIZE);
    v->key = (opt && opt->key) ? opt->key : sai_key_user;
    v->ignore_checksums = opt ? opt->ignore_checksums : 0;
    v->version = 1;
    v->ntables = (v->nblocks + SAI_BLOCKS_PER_TABLE - 1) / SAI_BLOCKS_PER_TABLE;
    v->tables = (uint32_t **)calloc(v->ntables, sizeof(uint32_t *));
    if (!v->tables) {
        sai_vfs_close(v);
        return SAI_ERR_NOMEM;
    }

    /* Validate table 0. */
    int err = SAI_OK;
    const uint32_t *t0 = get_table(v, 0, &err);
    if (!t0) {
        sai_vfs_close(v);
        return err;
    }
    if (t0[0] != sai_checksum_table(t0)) {
        sai_vfs_close(v);
        return SAI_ERR_FORMAT; /* wrong key or not a SAI file */
    }

    uint32_t w[SAI_WORDS_PER_BLOCK];
    uint8_t hdr[SAI_BLOCK_SIZE];
    err = read_data_block(v, 1, w, NULL);
    if (err == SAI_OK) {
        for (unsigned i = 0; i < SAI_WORDS_PER_BLOCK; i++)
            sai_wr32(hdr + i * 4, w[i]);
        if (memcmp(hdr + SAI_HDR_MAGIC_OFF, "VFS-0001", 8) == 0)
            v->version = 1;
        else if (memcmp(hdr + SAI_HDR_MAGIC_OFF, "VFS-0000", 8) == 0)
            v->version = 0;
        else
            v->version = -1;
    } else {
        v->version = -1;
    }

    if (v->version >= 0) {
        parse_entry(hdr + SAI_HDR_ROOT_OFF, &v->root);
        if (v->version == 0) {
            /* Tables other than multiples of 4096 were stored in the clear;
             * drop any cached decryptions done before the version was known. */
            for (uint32_t i = 1; i < v->ntables; i++) {
                free(v->tables[i]);
                v->tables[i] = NULL;
            }
        }
    } else {
        /* Heuristic for containers without a header block: treat block 2 as
         * a one-block root folder (this is where SAI allocates it first). */
        memset(&v->root, 0, sizeof(v->root));
        v->root.first_block = 2;
        v->root.size = SAI_BLOCK_SIZE;
    }
    v->root.type = SAI_ENTRY_FOLDER;
    v->root.name[0] = '\0';
    *out = v;
    return SAI_OK;
}

int sai_vfs_open_memory(const void *data, size_t size,
                        const sai_open_options *opt, sai_vfs **out)
{
    if (!data || !out)
        return SAI_ERR_ARGUMENT;
    uint8_t *copy = (uint8_t *)malloc(size ? size : 1);
    if (!copy)
        return SAI_ERR_NOMEM;
    memcpy(copy, data, size);
    return open_owned(copy, size, opt, out);
}

int sai_vfs_open_ex(const char *path, const sai_open_options *opt,
                    sai_vfs **out)
{
    uint8_t *data;
    size_t size;
    if (!path || !out)
        return SAI_ERR_ARGUMENT;
    int err = sai_read_whole_file(path, &data, &size);
    if (err)
        return err;
    return open_owned(data, size, opt, out);
}

int sai_vfs_open(const char *path, sai_vfs **out)
{
    return sai_vfs_open_ex(path, NULL, out);
}
