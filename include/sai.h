/*
 * libsai - read and write PaintTool SAI (.sai) documents.
 *
 * The format was recovered by reverse engineering PaintTool SAI 1.2.6-Beta.3
 * (see docs/file-format.md and docs/reverse-engineering.md).
 *
 * The library has two layers:
 *
 *   1. The VFS layer (sai_vfs_*, sai_writer_*): the encrypted, block based
 *      virtual file system that every .sai file is.  It gives access to the
 *      raw files inside the container ("/canvas", "/laytbl", "/layers/...").
 *
 *   2. The document layer (sai_document_*, sai_layer_*): parses those files
 *      into a canvas description, a list of layers with decoded pixels, and
 *      the thumbnail.  Documents can be built from scratch and saved.
 *
 * All functions returning int return SAI_OK (0) on success or a negative
 * sai_error value.  Memory handed to the caller must be released with the
 * matching *_free function or sai_free().
 */
#ifndef LIBSAI_SAI_H
#define LIBSAI_SAI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(SAI_BUILD_SHARED)
#  define SAI_API __declspec(dllexport)
#elif defined(_WIN32) && defined(SAI_USE_SHARED)
#  define SAI_API __declspec(dllimport)
#else
#  define SAI_API
#endif

#define SAI_VERSION_MAJOR 1
#define SAI_VERSION_MINOR 0
#define SAI_VERSION_PATCH 0

/* ------------------------------------------------------------------------ */
/* Errors                                                                   */
/* ------------------------------------------------------------------------ */

typedef enum sai_error {
    SAI_OK              =  0,
    SAI_ERR_IO          = -1,  /* fopen/fread/fwrite failed                 */
    SAI_ERR_NOMEM       = -2,  /* allocation failed                         */
    SAI_ERR_FORMAT      = -3,  /* structurally invalid data                 */
    SAI_ERR_CHECKSUM    = -4,  /* a block failed its checksum               */
    SAI_ERR_NOT_FOUND   = -5,  /* path does not exist in the VFS            */
    SAI_ERR_UNSUPPORTED = -6,  /* valid but unsupported variant             */
    SAI_ERR_ARGUMENT    = -7,  /* invalid argument                          */
    SAI_ERR_EXISTS      = -8   /* path already exists / name conflict       */
} sai_error;

SAI_API const char *sai_strerror(int err);

/* Free memory returned by libsai (buffers from sai_vfs_read_file, etc.). */
SAI_API void sai_free(void *ptr);

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define SAI_BLOCK_SIZE        4096u   /* every VFS block                   */
#define SAI_BLOCKS_PER_TABLE  512u    /* one table block per 512 blocks    */
#define SAI_ENTRY_SIZE        64u     /* directory entry size              */
#define SAI_NAME_MAX          32u     /* entry name field (not always NUL) */
#define SAI_TILE_SIZE         32u     /* layer pixel tiles are 32x32       */
#define SAI_LAYER_BORDER      8u      /* layer pixels extend 8px past canvas */

/* Directory entry types (byte at entry offset 0x26). */
#define SAI_ENTRY_FOLDER 0x10u
#define SAI_ENTRY_FILE   0x80u

/* Build a little-endian FourCC the way SAI does ('norm' -> 0x6E6F726D). */
#define SAI_FOURCC(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

/* Layer types (u32 at layer header offset 0). */
typedef enum sai_layer_type {
    SAI_LAYER_ROOT     = 0,
    SAI_LAYER_RASTER   = 3,  /* normal painting layer: 16-bit BGRA tiles   */
    SAI_LAYER_TYPE4    = 4,  /* single channel 16-bit tiles (unnamed)      */
    SAI_LAYER_LINEWORK = 5,  /* vector linework layer: stroke list         */
    SAI_LAYER_MASK     = 6,  /* layer mask: single channel 16-bit tiles    */
    SAI_LAYER_TYPE7    = 7,  /* single channel 16-bit tiles (unnamed)      */
    SAI_LAYER_SET      = 8   /* folder / layer set                         */
} sai_layer_type;

/* Blend modes, stored in the file as FourCC u32 values. */
#define SAI_BLEND_PASS_THROUGH SAI_FOURCC('p','a','s','s')
#define SAI_BLEND_NORMAL       SAI_FOURCC('n','o','r','m')
#define SAI_BLEND_MULTIPLY     SAI_FOURCC('m','u','l',' ')
#define SAI_BLEND_SCREEN       SAI_FOURCC('s','c','r','n')
#define SAI_BLEND_OVERLAY      SAI_FOURCC('o','v','e','r')
#define SAI_BLEND_LUMINOSITY   SAI_FOURCC('a','d','d',' ')
#define SAI_BLEND_SHADE        SAI_FOURCC('s','u','b',' ')
#define SAI_BLEND_LUMI_SHADE   SAI_FOURCC('a','d','s','b')
#define SAI_BLEND_BINARY_COLOR SAI_FOURCC('c','b','i','n')

/* Returns a human readable name for a blend FourCC ("Normal", ...). */
SAI_API const char *sai_blend_name(uint32_t fourcc);

/* ------------------------------------------------------------------------ */
/* VFS: reading                                                             */
/* ------------------------------------------------------------------------ */

typedef struct sai_vfs sai_vfs;

typedef struct sai_entry {
    char     name[SAI_NAME_MAX + 1]; /* NUL terminated copy of the name   */
    uint32_t flags;        /* 0x80000000 when the slot is in use           */
    uint8_t  type;         /* SAI_ENTRY_FOLDER or SAI_ENTRY_FILE           */
    uint32_t first_block;  /* first data block, 0 when the file is empty   */
    uint32_t size;         /* size in bytes                                */
    uint64_t timestamp;    /* Windows FILETIME (100ns since 1601-01-01)    */
    uint64_t reserved;     /* trailing 8 bytes of the entry                */
} sai_entry;

/* Options for opening a container. */
typedef struct sai_open_options {
    /* Use this key table instead of the .sai document key (256 u32).      */
    const uint32_t *key;
    /* Ignore data block checksum mismatches (SAI itself can be told to). */
    int ignore_checksums;
} sai_open_options;

SAI_API int  sai_vfs_open(const char *path, sai_vfs **out);
SAI_API int  sai_vfs_open_ex(const char *path, const sai_open_options *opt,
                             sai_vfs **out);
/* The memory is copied; the caller keeps ownership of `data`. */
SAI_API int  sai_vfs_open_memory(const void *data, size_t size,
                                 const sai_open_options *opt, sai_vfs **out);
SAI_API void sai_vfs_close(sai_vfs *vfs);

/* "VFS-0001" header version: 1 (current), 0 (legacy) or -1 (no header). */
SAI_API int  sai_vfs_version(const sai_vfs *vfs);
SAI_API uint32_t sai_vfs_block_count(const sai_vfs *vfs);

/* Look up an absolute path ("/", "/canvas", "/layers/00000002"). */
SAI_API int  sai_vfs_stat(sai_vfs *vfs, const char *path, sai_entry *out);

/* List a folder.  *entries is allocated; release it with sai_free(). */
SAI_API int  sai_vfs_list(sai_vfs *vfs, const char *path,
                          sai_entry **entries, size_t *count);

/* Read a whole file.  *data is allocated; release it with sai_free(). */
SAI_API int  sai_vfs_read_file(sai_vfs *vfs, const char *path,
                               uint8_t **data, size_t *size);

/* Read a whole file given its entry. */
SAI_API int  sai_vfs_read_entry(sai_vfs *vfs, const sai_entry *entry,
                                uint8_t **data, size_t *size);

/* Depth first walk of the whole tree.  Return non-zero from the callback
 * to stop the walk (that value is returned). */
typedef int (*sai_walk_fn)(void *user, const char *path,
                           const sai_entry *entry, int depth);
SAI_API int  sai_vfs_walk(sai_vfs *vfs, sai_walk_fn fn, void *user);

/* Decrypt one raw block to plain text (for diagnostics/tools).
 * buf must hold SAI_BLOCK_SIZE bytes. */
SAI_API int  sai_vfs_read_block(sai_vfs *vfs, uint32_t index, uint8_t *buf);

/* ------------------------------------------------------------------------ */
/* VFS: writing                                                             */
/* ------------------------------------------------------------------------ */

typedef struct sai_writer sai_writer;

SAI_API sai_writer *sai_writer_new(void);
SAI_API void sai_writer_free(sai_writer *w);

/* Use a different key table (default: the .sai document key). */
SAI_API void sai_writer_set_key(sai_writer *w, const uint32_t *key);

/* Timestamp (FILETIME) used for entries added without one; 0 = now. */
SAI_API void sai_writer_set_default_time(sai_writer *w, uint64_t filetime);

/* Create a folder (parents are created as needed). */
SAI_API int  sai_writer_mkdir(sai_writer *w, const char *path);

/* Add/replace a file; data is copied.  Parent folders are created.
 * filetime 0 uses the default time. Names are limited to 31 bytes. */
SAI_API int  sai_writer_add_file(sai_writer *w, const char *path,
                                 const void *data, size_t size,
                                 uint64_t filetime);

SAI_API int  sai_writer_save(sai_writer *w, const char *path);
/* *out is allocated; release it with sai_free(). */
SAI_API int  sai_writer_save_memory(sai_writer *w, uint8_t **out,
                                    size_t *size);

/* ------------------------------------------------------------------------ */
/* Document model                                                           */
/* ------------------------------------------------------------------------ */

/* A tagged chunk that libsai does not interpret; preserved on save. */
typedef struct sai_chunk {
    uint32_t tag;
    uint32_t size;
    uint8_t *data;
} sai_chunk;

/* A file inside the container that the document layer does not own
 * (for example the "#01.<id>" work-time record). Preserved on save. */
typedef struct sai_extra_file {
    char     path[256];
    uint8_t *data;
    size_t   size;
    uint64_t timestamp;
} sai_extra_file;

/* Work-time record stored in "/#01.<16 hex digits>" (32 bytes). */
typedef struct sai_sysinfo {
    uint16_t version;     /* 37 in files written by 1.2.x                */
    uint16_t flags;
    uint32_t sessions;    /* observed 1, 2 ...: number of editing sessions */
    uint64_t first_time;  /* seconds since 1601-01-01 (FILETIME / 1e7)    */
    uint64_t last_time;   /* seconds since 1601-01-01                     */
    uint64_t id;          /* identical to the hex number in the file name */
} sai_sysinfo;

typedef struct sai_layer {
    /* --- fixed header ------------------------------------------------- */
    uint32_t type;            /* sai_layer_type                           */
    uint32_t id;              /* unique id, file is /layers/%08x          */
    int32_t  x, y;            /* bounds in layer space                    */
    uint32_t width, height;   /* bounds; multiples of 32 for pixel layers */
    uint32_t unknown76;       /* u32 at header offset 24 (struct +76)     */
    uint8_t  opacity;         /* 0..100                                   */
    uint8_t  visible;         /* 0/1                                      */
    uint8_t  preserve_opacity;/* 0/1                                      */
    uint8_t  clipping;        /* 0/1 clip to layer below                  */
    uint8_t  unknown86;       /* u8 at header offset 32                   */
    uint32_t blend;           /* SAI_BLEND_* FourCC                       */

    /* --- tagged chunks ------------------------------------------------ */
    char     name[257];       /* 'name' (256 bytes, ANSI code page, e.g. CP932) */
    int      has_origin;      /* 'lorg' */
    int32_t  origin_x, origin_y;
    int      has_parent_set;  /* 'pfid' : id of the containing layer set  */
    uint32_t parent_set_id;
    int      has_parent_layer;/* 'plid' : id of the owning layer (masks)   */
    uint32_t parent_layer_id;
    int      has_mask_flags;  /* 'lmfl' : bit0, bit1 (mask layers)         */
    uint32_t mask_flags;
    int      has_set_open;    /* 'fopn' : folder expanded in layer panel   */
    uint8_t  set_open;
    int      has_texture_name;/* 'texn' : 64 byte paper texture name       */
    char     texture_name[65];
    int      has_texture;     /* 'texp' : u16 scale, u8 opacity            */
    uint16_t texture_scale;
    uint8_t  texture_opacity;
    int      has_effect;      /* 'peff' : u8 enabled, u8 opacity, u8 width */
    uint8_t  effect_enabled, effect_opacity, effect_width;
    int      has_vmrk;        /* 'vmrk' : u8                               */
    uint8_t  vmrk;
    sai_chunk *extra_chunks;  /* unknown chunks, preserved                 */
    size_t   extra_chunk_count;

    /* --- payload -------------------------------------------------------
     * Raster layers (type 3): `pixels` holds width*height*4 uint16 values,
     *   premultiplied B,G,R,A order, 16-bit (SAI stores the high byte and
     *   low byte of each channel in separate RLE planes).
     * Types 4/6/7: `pixels` holds width*height uint16 values (one channel).
     * Other types: `pixels` is NULL and `body` holds the raw remainder of
     *   the layer stream (e.g. the linework stroke list), preserved verbatim.
     * tile_map: (width/32)*(height/32) bytes, 1 where a tile is stored.    */
    uint16_t *pixels;
    uint8_t  *tile_map;
    uint8_t  *body;
    size_t    body_size;
} sai_layer;

typedef struct sai_document {
    uint32_t width, height;

    int      has_resolution;  /* 'reso' */
    uint32_t dpi;             /* 16.16 fixed point (72 dpi = 0x00480000)   */
    uint16_t resolution_unit;
    uint16_t size_unit;
    int      has_next_layer_id; /* 'lyid' */
    uint32_t next_layer_id;
    int      has_selected_layer;/* 'layr' */
    uint32_t selected_layer_id;
    int      has_wsrc;        /* 'wsrc' : u32, written when non-zero        */
    uint32_t wsrc;
    int      has_stid;        /* 'stid' : u32, read-only in SAI 1.2.6      */
    uint32_t stid;
    sai_chunk *extra_chunks;
    size_t   extra_chunk_count;

    /* Layers in /laytbl order and sub-layers in /subtbl order. */
    sai_layer *layers;
    size_t     layer_count;
    sai_layer *sublayers;
    size_t     sublayer_count;

    /* Thumbnail: BGRA 8-bit, alpha always 255 in files written by SAI. */
    uint32_t thumb_width, thumb_height;
    uint8_t *thumb_bgra;

    sai_extra_file *extra_files;
    size_t          extra_file_count;
} sai_document;

/* Load a document (the VFS is opened and closed internally). */
SAI_API int  sai_document_load(const char *path, sai_document **out);
SAI_API int  sai_document_load_vfs(sai_vfs *vfs, sai_document **out);
SAI_API int  sai_document_load_memory(const void *data, size_t size,
                                      sai_document **out);

/* Create an empty document.  A fresh sysinfo record is added. */
SAI_API sai_document *sai_document_new(uint32_t width, uint32_t height);
SAI_API void sai_document_free(sai_document *doc);

/* Save.  If the document has no thumbnail one is generated. */
SAI_API int  sai_document_save(const sai_document *doc, const char *path);
SAI_API int  sai_document_save_memory(const sai_document *doc,
                                      uint8_t **out, size_t *size);
/* Serialise into a writer (lets callers add their own files). */
SAI_API int  sai_document_write(const sai_document *doc, sai_writer *w);

/* Append a new, transparent raster layer covering the canvas.  The
 * returned pointer is valid until the layer array is modified again. */
SAI_API sai_layer *sai_document_add_layer(sai_document *doc,
                                          const char *name);
/* Append a layer set (folder). */
SAI_API sai_layer *sai_document_add_set(sai_document *doc, const char *name);

SAI_API sai_layer *sai_document_find_layer(sai_document *doc, uint32_t id);

/* Regenerate the thumbnail from visible raster layers (normal blend over
 * white, the same size rule SAI uses). */
SAI_API int  sai_document_update_thumbnail(sai_document *doc);

/* Composite visible raster layers into a canvas-sized, straight-alpha
 * RGBA8 buffer (width*height*4).  Only Normal blending with opacity is
 * applied; other blend modes are treated as Normal.  Layer sets are
 * honoured for visibility and opacity.  `out` is caller-allocated. */
SAI_API int  sai_document_flatten_rgba8(const sai_document *doc,
                                        uint8_t *out);

/* Parse/serialise the work-time record. */
SAI_API int  sai_document_get_sysinfo(const sai_document *doc,
                                      sai_sysinfo *out);

/* ------------------------------------------------------------------------ */
/* Layer pixels                                                             */
/* ------------------------------------------------------------------------ */

/* Ensure a raster layer has a pixel buffer (allocates a transparent one). */
SAI_API int  sai_layer_alloc_pixels(sai_layer *layer);

/* Copy the canvas-aligned region of a raster layer into straight-alpha
 * RGBA8 (canvas_w*canvas_h*4).  The 8px border is skipped. */
SAI_API int  sai_layer_get_rgba8(const sai_layer *layer,
                                 uint32_t canvas_w, uint32_t canvas_h,
                                 uint8_t *out);

/* Replace the canvas-aligned region of a raster layer from straight-alpha
 * RGBA8 (canvas_w*canvas_h*4).  The border is cleared. */
SAI_API int  sai_layer_set_rgba8(sai_layer *layer,
                                 uint32_t canvas_w, uint32_t canvas_h,
                                 const uint8_t *rgba);

/* ------------------------------------------------------------------------ */
/* Low level helpers (exposed for tools and tests)                          */
/* ------------------------------------------------------------------------ */

/* The key tables extracted from sai.exe. */
SAI_API const uint32_t *sai_key_document(void);
SAI_API const uint32_t *sai_key_system_vfs(void);

/* PackBits-style RLE used for tile channels. `stride` is the distance in
 * bytes between consecutive samples of the channel in `plane`.
 * decode: returns bytes consumed from src or a negative error.
 * encode: returns bytes written to dst (dst must hold >= 2*count bytes). */
SAI_API int  sai_rle_decode(const uint8_t *src, size_t src_len,
                            uint8_t *plane, size_t count, size_t stride);
SAI_API int  sai_rle_encode(const uint8_t *plane, size_t count,
                            size_t stride, uint8_t *dst);

#ifdef __cplusplus
}
#endif

#endif /* LIBSAI_SAI_H */
