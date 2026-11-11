# The `.sai` file format

This document describes the PaintTool SAI document format as implemented by
**PaintTool SAI Ver.1 1.2.6-Beta.3** (`sai.exe`, SHA-256
`68ecc23e03d8c1843b4b0460dae8597612c3054daa138d6e8a98a13e5a00090a`).
Every statement comes from the decompiled binary. Each section names the
function it was taken from, and the claims were checked against real
documents saved by SAI. [reverse-engineering.md](reverse-engineering.md)
gives the full evidence trail and lists what is still unknown.

All integers are **little-endian**. "FourCC" values are `u32`s whose *value*
spells the tag when read most-significant byte first. For example, `'norm'`
is `0x6E6F726D`, which appears in the file as the bytes `6D 72 6F 6E`
("mron").

A `.sai` file has two layers:

1. A **virtual file system (VFS)**: an encrypted, block-based container,
   similar to FAT, implemented in SAI's VFS module.
2. **Document files** inside that container (`/canvas`, `/laytbl`,
   `/layers/...`, `/thumbnail`), written by SAI's `fio_sai.c`.

---

## 1. The virtual file system

### 1.1 Blocks

The file is an array of **4096-byte blocks**. Block *i* starts at file
offset `i * 4096`. The file length is always a multiple of 4096. Blocks past
the end of the file read as zeros (`FUN_004bdc60` zero-fills short reads).

| Block index        | Role                                              |
|--------------------|---------------------------------------------------|
| `n * 512`          | **Table block** for blocks `n*512 … n*512+511`    |
| `1`                | **Header block**                                  |
| all others         | **Data blocks** (or free)                         |

### 1.2 Key

Both ciphers use a 256-entry `u32` key table. SAI keeps several tables in
`.rdata`:

* The global at VA `0x536160` points to a key area at VA `0x5188c0`.
* `fio_sai.c` opens documents with `key_area + 0x400`, which is the table at
  **VA `0x518cc0`**. This is the `.sai` document key. Its first value is
  `0x9913D29E`.
* The table at VA `0x5194c0` is used by the internal system VFS (`sai.ssd`).

libsai ships both tables in `src/key.c`.

The cipher's mixing function is:

```
K(x) = key[x >> 24] + key[(x >> 16) & 0xFF] + key[(x >> 8) & 0xFF] + key[x & 0xFF]   (mod 2^32)
```

### 1.3 Table blocks

A table block holds 512 entries of 8 bytes:

```c
struct TableEntry {
    u32 checksum;   /* 0 = block is free; otherwise checksum of the block */
    u32 next;       /* next block of the same file (0 = last),
                       or the free-list link for free blocks */
};
```

**Encryption** (`FUN_004bd720` encrypts, `FUN_004bd860` decrypts). The cipher
is chained, and the seed is the table's own block index (always a multiple
of 512):

```
encrypt:  c[i] = c[i-1] ^ rot16(p[i]) ^ K(c[i-1])          c[-1] = block index
decrypt:  p[i] = rot16( K(c[i-1]) ^ c[i] ^ c[i-1] )
```

`rot16` swaps the two 16-bit halves of a 32-bit word.

**Checksum** (`FUN_004be3b0`). Entry 0 describes the table block itself. Its
`checksum` field holds the checksum of words `1..1023` of the plaintext:

```
s = 0; for i in 1..1023: s = rotl32(s, 1) ^ w[i];   checksum = s | 1
```

Entry 0's `next` field is always `0x1000` (`FUN_004be760`).

A correct table-0 checksum is a reliable way to tell whether a file is a
`.sai` file and whether the right key is in use.

**Free list.** When SAI creates a table (`FUN_004be760`), entry *e* (for
1 ≤ e ≤ 510) gets `next = base + e + 1`, and entry 511 gets
`next = base + 513`, which skips the next table block. The header stores the
head of this free list. Allocation (`FUN_004be830`) pops blocks from the
head, so a freshly written file uses blocks 2, 3, 4, … in order.

**Legacy `VFS-0000` files.** In these files only table blocks whose index is
a multiple of **4096** are encrypted; the others are stored in plaintext
(`FUN_004bdb00`). SAI upgrades such files to `VFS-0001` when it opens them
for writing (`FUN_004c04a0`).

### 1.4 Data blocks

**Encryption** (`FUN_004be2a0` encrypts, `FUN_004be170` decrypts). The seed is
the block's checksum from its table entry:

```
encrypt:  c[i] = p[i] + ( K(c[i-1]) ^ c[i-1] )            c[-1] = checksum
decrypt:  p[i] = c[i] - ( K(c[i-1]) ^ c[i-1] )
```

**Checksum** (`FUN_004be3e0`) is computed over all 1024 plaintext words:

```
s = 0; for i in 0..1023: s = rotl32(s, 1) ^ w[i];   checksum = s | 1
```

SAI compares the checksum after decryption and flags a mismatch as
corruption. Because of the `| 1`, a valid checksum is never 0, so a 0 always
means "free".

### 1.5 Header block (block 1)

The header is an ordinary data block, so it is encrypted with its own
checksum. It is created by `FUN_004c0f80` and refreshed by `FUN_004bea40`.

| Offset | Type      | Meaning                                                    |
|-------:|-----------|------------------------------------------------------------|
| 0x00   | u32       | random value (`rand()`-based)                              |
| 0x04   | u32       | random value                                               |
| 0x08   | char[8]   | `"VFS-0001"` (current) or `"VFS-0000"` (legacy, §1.3)      |
| 0x10   | u32       | head of the free block list                                |
| 0x14   | u32       | number of blocks covered by tables (a multiple of 512)     |
| 0x18   | u32       | number of allocated data blocks (excluding header and tables) |
| 0x1C   | —         | zero                                                       |
| 0x40   | Entry[1]  | **root directory entry** (64 bytes, see §1.6)              |

Table entry 1 (the header block's own entry) keeps its initial free-list
value `next = 2`.

### 1.6 Files, folders, and entries

A file's data is the concatenation of its blocks. Blocks are linked through
the `next` field of their table entries, starting at the entry's
`first_block`. The data is then truncated to `size`. Blocks hold no
per-block headers.

A **folder is a file** whose contents are an array of 64-byte entries:

| Offset | Type      | Meaning                                                    |
|-------:|-----------|------------------------------------------------------------|
| 0x00   | u32       | flags: `0x80000000` = used; `0` = empty slot               |
| 0x04   | char[32]  | name (NUL-padded, not NUL-terminated at 32 characters)     |
| 0x24   | u16       | zero                                                       |
| 0x26   | u8        | type: `0x10` folder, `0x80` file                           |
| 0x27   | u8        | zero                                                       |
| 0x28   | u32       | first block (0 for an empty file)                          |
| 0x2C   | u32       | size in bytes (for a folder, 64 × number of slots)         |
| 0x30   | u64       | FILETIME of the last modification (UTC)                    |
| 0x38   | u64       | zero                                                       |

Name lookup is case-insensitive and compares at most 32 characters
(`FUN_004bef50` uses `_strnicmp(…, 0x20)`). The root entry in the header
has flags 0, type 0 and an empty name. Only `first_block` and `size` are
meaningful.

### 1.7 Document tree

A document saved by SAI 1.2.6 contains:

```
/#01.<16 hex digits>     32 bytes   work-time record (§2.6)
/canvas                             canvas properties (§2.1)
/laytbl                             layer order table (§2.2)
/layers/                            one file per layer, named %08x by id
/subtbl                  optional   sub-layer table (masks)
/sublayers/              optional
/thumbnail                          preview image (§2.5)
```

The save routine (`FUN_00465140`) writes them in this order. The files
written to the root by the loop over `doc+0x4c0` are small 32-byte records
named `/%s` (in the observed documents, only the `#01.…` record).

---

## 2. Document files

### 2.1 `/canvas`

Written by `FUN_00465140` and read by `FUN_00465be0`.

```
u32 version          0x10 (values 0, 1 and 2 are legacy fixed-layout canvases)
u32 width            canvas width in pixels
u32 height           canvas height in pixels
chunks…              { u32 tag; u32 size; u8 data[size]; } until tag == 0
u32 0                terminator (no size follows)
```

| Tag    | Size | Content                                                         |
|--------|-----:|-----------------------------------------------------------------|
| `reso` | 8    | `u32 dpi` (16.16 fixed point, so `0x00480000` = 72 dpi), `u16 resolution unit`, `u16 size unit` |
| `lyid` | 4    | `u32` next layer id (SAI's id counter)                          |
| `layr` | 4    | `u32` id of the selected layer                                  |
| `wsrc` | 4    | `u32` (written only when non-zero; meaning unknown)             |
| `stid` | 4    | `u32` (read by 1.2.6 but never written; meaning unknown)        |

The reader skips unknown tags by their size. For `reso`, SAI's field table
is `{4 @332, 2 @336, 2 @338}` (byte size @ offset in SAI's canvas struct).

### 2.2 `/laytbl` and `/subtbl`

```
u32 count
struct { u32 id; u16 type; u16 weight; } entries[count]
```

`laytbl` lists the top-level layers **from bottom to top**. Layer sets
(folders) appear in the same flat list, and their children refer to them
through `pfid`. `weight` is used only as a progress-bar hint. For raster
layers (type 3) it is the number of tile rows (`height / 32`); for every
other type it is `1`. In `subtbl`, the weight is always the tile-row count.
`subtbl` and `/sublayers/` are written only when at least one layer has a
mask or another sub-layer.

### 2.3 `/layers/%08x` — layer stream

Written by `FUN_00463340` and read by `FUN_00463c50`.

**Fixed header (33 bytes)**, from the field table at VA `0x51dfa0`:

| Offset | Type | Field (SAI struct offset) |
|-------:|------|---------------------------|
| 0      | u32  | type (+32): see the table below |
| 4      | u32  | id (+40) |
| 8      | i32  | bounds x (+184) |
| 12     | i32  | bounds y (+188) |
| 16     | u32  | bounds width (+192) |
| 20     | u32  | bounds height (+196) |
| 24     | u32  | unknown (+76), 0 in observed files |
| 28     | u8   | opacity 0–100 (+81) |
| 29     | u8   | visible (+83) |
| 30     | u8   | preserve opacity (+84) |
| 31     | u8   | clipping group (+85) |
| 32     | u8   | unknown (+86) |

This is followed by a **`u32` blend mode FourCC**:

| FourCC  | Mode           |
|---------|----------------|
| `pass`  | Pass Through (layer sets) |
| `norm`  | Normal (also the fallback for unknown values) |
| `mul `  | Multiply       |
| `scrn`  | Screen         |
| `over`  | Overlay        |
| `add `  | Luminosity     |
| `sub `  | Shade          |
| `adsb`  | Lumi & Shade   |
| `cbin`  | Binary Color   |

**Layer types**

| Value | Meaning                                                     |
|------:|-------------------------------------------------------------|
| 3     | Raster (painting) layer: 4-channel tiles                    |
| 4     | 1-channel tiles (same code path as masks; UI name unknown)  |
| 5     | Linework layer: vector stroke list instead of tiles         |
| 6     | Layer mask (`plid` + `lmfl`)                                |
| 7     | 1-channel tiles (UI name unknown)                           |
| 8     | Layer set (folder)                                          |

**Chunks** follow, using the same `{u32 tag, u32 size, data}` framing and
ending with a `u32 0`. SAI writes them in this order:

| Tag    | Size | Content | When written |
|--------|-----:|---------|--------------|
| `lorg` | 8    | `i32, i32` (struct +176/+180), 0 in observed files | always |
| `name` | 256  | layer name, NUL-padded, in the system ANSI code page (CP932 on Japanese systems) | always |
| `pfid` | 4    | id of the parent layer set | when the layer is inside a set |
| `plid` | 4    | id of the owning layer | for sub-layers (masks) instead of `pfid` |
| `lmfl` | 4    | mask flags: bit 0 = struct +0x58, bit 1 = +0x59 | type 6 with `plid` |
| `fopn` | 1    | set is expanded in the layer panel | type 8 |
| `texn` | 64   | paper texture name | when a texture is assigned |
| `texp` | 3    | `u16` texture scale (+92), `u8` texture strength (+91) — default 100, 20 | always |
| `peff` | 3    | `u8` effect enabled (+94), `u8` opacity (+95), `u8` width (+96) — default 0, 100, 1 | always |
| `vmrk` | 1    | `u8` (+82) | always |

The reader skips unknown tags.

**Payload.** What comes after the terminator depends on the type:

* **Types 3, 4, 6, 7 (tiled).** `width` and `height` are multiples of 32.
  Let `tw = width/32` and `th = height/32`.
  1. A tile map of `tw * th` bytes, row-major, where `1` means "this tile is
     stored".
  2. For each present tile, in row-major order, a sequence of **RLE
     streams**. Each stream is a `u16 length` followed by `length` bytes of
     RLE data (§2.4); `length` is at most 0x800.
     * **Type 3:** 8 streams. Each stream holds one byte per pixel for 1024
       pixels. Streams 0–3 are the **high bytes** of B, G, R, A, and streams
       4–7 are the **low bytes** of B, G, R, A. In memory, SAI's tile buffer
       is two 4096-byte planes (`FUN_00462990`/`FUN_00462b70`).
     * **Types 4, 6, 7:** 2 streams. Stream 0 holds the low bytes and
       stream 1 the high bytes of one 16-bit value per pixel
       (`FUN_00462ac0`/`FUN_00462dd0`).
* **Type 5 (linework).** The stroke list. `FUN_00463340` writes, for each
  stroke: `u32` (+0x2c), `u32 0x10`, `u32` (+0x28), the stroke's fields
  (table at `0x51e090`) and a `u32 0`. Each point is a 16-byte record. A final
  `u32 0` ends the list. libsai keeps this payload as raw bytes.
* **Type 8 (set).** No payload.

**Pixel format (type 3).** Pixels are 16-bit **premultiplied** BGRA. SAI uses
14 significant bits: values are stored shifted left by 2, so full alpha is
`0xFFFC`. Every pixel in the sample files satisfies `max(B,G,R) ≤ A`, which
supports the premultiplication reading.

**Coordinates.** Raster layers are created with a border of 8 pixels. A new
layer's bounds are `x = y = 0` and
`width = roundup32(canvas_w + 16)`, `height = roundup32(canvas_h + 16)`.
Canvas pixel `(cx, cy)` is layer pixel `(cx + 8 − x, cy + 8 − y)`. This
offset was confirmed by correlating decoded layers with SAI's own thumbnail;
the correlation at offset 8 is 0.997 to 1.000.

### 2.4 Tile RLE

This is a PackBits variant: the encoder is `FUN_004aefc0` (stride 4) and
`FUN_004af110` (stride 2). The data is a sequence of control bytes `c`:

| `c`       | Meaning                                         |
|-----------|-------------------------------------------------|
| 0–127     | `c + 1` literal bytes follow                    |
| 129–255   | the next byte repeats `257 − c` times           |
| 128       | never produced                                  |

Runs and literal blocks are each at most 128 samples. SAI's encoder starts a
literal run whenever the next two samples differ, and extends it while
neighbouring samples keep differing. libsai's `sai_rle_encode` is a direct
port, so re-encoded layers are byte-identical to SAI's output.

### 2.5 `/thumbnail`

Written by `FUN_00462350`:

```
u32 width
u32 height
char[4] "BM32"
u8 pixels[height][width][4]     B, G, R, A — alpha is always 0xFF
```

The downscale factor `f` is the first of 16, 8, 4, 2 for which
`max(w, h) / f ≥ 97`; otherwise it is 1. The thumbnail is
`(w / f) × (h / f)`, which is the canvas composite over white.

### 2.6 `/#01.<id>` — work-time record

This 32-byte file is maintained by SAI's `sysinfo` code. Its name is `#01.`
followed by the record id as 16 hex digits:

| Offset | Type | Observed meaning |
|-------:|------|------------------|
| 0      | u16  | 37 (record version) |
| 2      | u16  | 0x8200 (flags) |
| 4      | u32  | 1, 2, …: the number of editing sessions |
| 8      | u64  | first edit, seconds since 1601-01-01 UTC |
| 16     | u64  | last edit, seconds since 1601-01-01 UTC |
| 24     | u64  | id, equal to the hex in the file name |

---

## 3. Writing files SAI accepts: checklist

* Use the document key (`0x518cc0`).
* Lay out blocks the way SAI does: table 0, header at 1, data from block 2
  upward, skipping every 512th block.
* Fill free table entries with the free-list pattern (§1.3), set entry 0's
  `next` to `0x1000`, and leave the header's entry `next` at `2`.
* Store data-block checksums in the table; give free blocks checksum 0.
* Write the header with `"VFS-0001"`, the free-list head, the table span,
  the used-block count, and the root entry at offset 0x40.
* Write all document files: `canvas` version 0x10, `laytbl`, `layers/…`,
  and `thumbnail`.
* For raster layers, use widths and heights that are multiples of 32, an
  8 px border, 8 RLE streams per tile, and premultiplied 14-bit-in-16-bit
  pixels.
