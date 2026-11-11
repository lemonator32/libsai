# libsai

C library for reading and writing PaintTool SAI (`.sai`) files.

The format was reverse engineered from PaintTool SAI 1.2.6-Beta.3. libsai
can:

* decrypt and walk the SAI virtual file system, and write new containers;
* load documents: canvas settings, layers and sets, masks, the thumbnail,
  and the work-time record;
* decode layer pixels to 16-bit premultiplied BGRA, with helpers for RGBA8;
* create and save documents, keeping anything it doesn't understand;
* flatten visible layers into an RGBA8 preview.

Re-saving a document reproduces SAI's layer streams byte for byte.

```c
#include <sai.h>

sai_document *doc;
if (sai_document_load("art.sai", &doc) == SAI_OK) {
    for (size_t i = 0; i < doc->layer_count; i++)
        printf("%s\n", doc->layers[i].name);
    sai_document_save(doc, "copy.sai");
    sai_document_free(doc);
}
```

## Build

```sh
cmake -B build && cmake --build build && ctest --test-dir build   # any platform
make                                                              # POSIX
build.bat                                                         # Windows/MSVC
```

`build/saitool` is a small command-line front end (`info`, `ls`, `cat`,
`export`, `resave`, `new`).

## Documentation

* [docs/file-format.md](docs/file-format.md): format specification
* [docs/api.md](docs/api.md): API guide
* [docs/reverse-engineering.md](docs/reverse-engineering.md): evidence and
  open questions

## License

MIT. See [LICENSE](LICENSE). PaintTool SAI is a product of SYSTEMAX; this
project is not affiliated with SYSTEMAX.
