# libsai API guide

The whole public API is declared in [`include/sai.h`](../include/sai.h).
Functions that return `int` return `SAI_OK` (0) on success or a negative
`sai_error` value. `sai_strerror()` turns an error code into text.

Free memory that libsai returns with the matching function:

| Allocated by                                  | Free with              |
|-----------------------------------------------|------------------------|
| `sai_vfs_open*`                               | `sai_vfs_close`        |
| `sai_vfs_read_file`, `sai_vfs_list`, `*_save_memory` | `sai_free`      |
| `sai_writer_new`                              | `sai_writer_free`      |
| `sai_document_load*`, `sai_document_new`      | `sai_document_free`    |

Paths passed to libsai are UTF-8. On Windows they are converted to UTF-16,
so non-ASCII file names work.

## Building

```sh
# CMake (any platform)
cmake -B build && cmake --build build && ctest --test-dir build

# POSIX make
make && make test SAMPLES="a.sai b.sai"

# Windows, MSVC without CMake
build.bat
```

The library is plain C99 with no dependencies.

## Reading a document

```c
#include <sai.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    sai_document *doc;
    int err = sai_document_load(argv[1], &doc);
    if (err) {
        fprintf(stderr, "load failed: %s\n", sai_strerror(err));
        return 1;
    }
    printf("%u x %u, %zu layers\n", doc->width, doc->height, doc->layer_count);

    for (size_t i = 0; i < doc->layer_count; i++) {
        const sai_layer *l = &doc->layers[i];      /* bottom to top */
        printf("%08x %s opacity=%u blend=%s\n", l->id, l->name, l->opacity,
               sai_blend_name(l->blend));
    }

    /* Composite visible raster layers (Normal blending only). */
    uint8_t *rgba = malloc((size_t)doc->width * doc->height * 4);
    sai_document_flatten_rgba8(doc, rgba);
    /* ... write rgba to PNG with your favourite library ... */
    free(rgba);
    sai_document_free(doc);
    return 0;
}
```

### Layer pixels

For raster layers (`SAI_LAYER_RASTER`), `layer->pixels` holds
`width * height * 4` `uint16_t` values: **B, G, R, A, premultiplied**, with
14 significant bits shifted left by two (opaque alpha is `0xFFFC`). Layer
space includes an 8-pixel border around the canvas. Two helpers convert to
and from straight-alpha RGBA8 in canvas space:

```c
uint8_t *rgba = malloc(doc->width * doc->height * 4);
sai_layer_get_rgba8(layer, doc->width, doc->height, rgba);
/* edit... */
sai_layer_set_rgba8(layer, doc->width, doc->height, rgba);
```

Mask-type layers (types 4, 6 and 7) hold one `uint16_t` per pixel. Linework
layers (type 5) and other types without tiles keep their payload in
`layer->body`, which is saved back unchanged.

### Layer hierarchy

* `doc->layers` lists the top-level layers in drawing order (bottom first),
  including layer sets.
* A layer inside a set has `has_parent_set = 1` and `parent_set_id` set to
  the set's id.
* A mask lives in `doc->sublayers` with `has_parent_layer = 1` and
  `parent_layer_id` set to the layer it masks.
* `sai_document_find_layer(doc, id)` searches both arrays.

### Things the document keeps for you

Unknown tagged chunks (`layer->extra_chunks`, `doc->extra_chunks`), unknown
VFS files (`doc->extra_files`, including the work-time record), and raw
layer bodies are all preserved when you save. A load→save round trip
re-creates SAI's layer streams byte for byte.

## Creating a document

```c
sai_document *doc = sai_document_new(1920, 1080);   /* 72 dpi, sysinfo record */
sai_layer *bg = sai_document_add_layer(doc, "Background");
sai_layer_set_rgba8(bg, 1920, 1080, my_rgba_pixels);

sai_layer *set = sai_document_add_set(doc, "Lineart");
uint32_t set_id = set->id;                 /* pointers move when adding */
sai_layer *ink = sai_document_add_layer(doc, "Ink");
ink->has_parent_set = 1;
ink->parent_set_id = set_id;
ink->blend = SAI_BLEND_MULTIPLY;

int err = sai_document_save(doc, "out.sai");   /* thumbnail generated */
sai_document_free(doc);
```

`sai_document_add_*` reallocates `doc->layers`, so earlier `sai_layer *`
pointers become invalid. Keep ids, not pointers.

`layer->name` is written as raw bytes. SAI interprets them in the system
ANSI code page (CP932 on Japanese Windows). Plain ASCII works everywhere.

## The virtual file system directly

```c
sai_vfs *vfs;
sai_vfs_open("in.sai", &vfs);

sai_entry *list; size_t n;
sai_vfs_list(vfs, "/layers", &list, &n);
for (size_t i = 0; i < n; i++)
    printf("%s %u bytes\n", list[i].name, list[i].size);
sai_free(list);

uint8_t *data; size_t size;
sai_vfs_read_file(vfs, "/canvas", &data, &size);
sai_free(data);
sai_vfs_close(vfs);
```

`sai_vfs_walk()` visits every entry depth-first. `sai_vfs_read_block()`
returns a single decrypted block, which is useful for forensic tools.
`sai_open_options` lets you pass another key, for example
`sai_key_system_vfs()` for SAI's `sai.ssd`, or ignore checksum errors
when recovering a damaged file.

Write arbitrary containers with `sai_writer`:

```c
sai_writer *w = sai_writer_new();
sai_writer_add_file(w, "/canvas", canvas_bytes, canvas_len, 0);
sai_writer_mkdir(w, "/layers");
sai_writer_save(w, "out.sai");
sai_writer_free(w);
```

To add your own files next to a document, call `sai_document_write(doc, w)`
and then `sai_writer_add_file()` before saving.

## Command-line tool

`saitool` (in `tools/`) exercises the whole API:

```
saitool info   FILE.sai                 canvas, layers, sysinfo
saitool ls     FILE.sai                 list the VFS with block numbers
saitool cat    FILE.sai /path OUT       extract one inner file
saitool export FILE.sai PREFIX          flattened image, each layer, thumbnail (TGA)
saitool resave IN.sai OUT.sai           load + save
saitool new    OUT.sai W H [IMAGE.tga]  new document, optionally from a TGA
```

## Limitations

* Only `/canvas` version 0x10 is parsed (SAI 1.1 and later). Legacy
  fixed-layout canvases (versions 0–2) return `SAI_ERR_UNSUPPORTED`, but
  their VFS can still be read.
* `sai_document_flatten_rgba8` and the generated thumbnail use Normal
  blending for every mode, and ignore clipping groups, masks and textures.
  Pixels are decoded exactly; only the preview compositing is simplified.
* Linework strokes are kept as raw bytes and not parsed.
* Saved files reproduce SAI's on-disk layout. Their compatibility has been
  checked by structural comparison with files SAI wrote (identical tables,
  identical layer streams). They have not been opened in the SAI GUI as
  part of the automated tests.
