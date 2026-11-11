# libsai documentation

| Document | Contents |
|----------|----------|
| [file-format.md](file-format.md) | The complete `.sai` format specification: VFS blocks, ciphers, checksums, directory entries, canvas, layer streams, tile RLE, and thumbnail |
| [api.md](api.md) | How to build and use the C library, with examples and limitations |
| [reverse-engineering.md](reverse-engineering.md) | Where each fact came from in `sai.exe` 1.2.6-Beta.3, how it was validated, and the open questions |

The format at a glance:

```
.sai file = array of 4096-byte blocks
  block 0, 512, 1024, …   table blocks   {checksum, next} × 512   (cipher seeded by block index)
  block 1                 header         "VFS-0001", free list, root entry
  other blocks            data           (cipher seeded by the block's checksum)

VFS tree
  /#01.<id>          work-time record
  /canvas            0x10, width, height, chunks {reso, lyid, layr, …}
  /laytbl            layer order: {id, type, weight}[]
  /layers/%08x       33-byte header, blend FourCC, chunks {name, pfid, …},
                     tile map + 32×32 tiles as 8 PackBits streams
                     (16-bit premultiplied BGRA)
  /subtbl, /sublayers/%08x   masks
  /thumbnail         w, h, "BM32", BGRA
```
